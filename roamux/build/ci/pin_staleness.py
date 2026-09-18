#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Chromium pin staleness check (roam-292, grill H1; the SLA in docs/security-uprev.md).

Measures how far ``roamux/build/CHROMIUM_PIN`` is behind the stable channel and, on ``main`` of the
canonical repository, files or updates one tracking issue per staleness episode.

Source: the VersionHistory API (https://developer.chrome.com/docs/web-platform/versionhistory),
platform ``mac`` (the x86_64 platform id — Roamux ships one universal2 build from one pin, so ``mac``
is the representative platform; ``mac_arm64`` is a separate id and is not queried), channel
``stable``. Git tags cannot do this job: every version on every channel is tagged and tags carry no
channel or release date.

A release QUALIFIES as "stable" when it has a serving record with ``fraction`` 1 within its own
``fractionGroup`` (that is what the API filter ``fraction>=1`` selects; fractions are comparable only
within a group, so nothing here compares them across groups and no claim of global rollout
completion is made). Per version, the release date is the earliest qualifying ``serving.startTime``
(a UTC calendar date). Versions serving at a lower fraction are reported as "rolling".

Metrics:
  milestones_behind  latest qualifying milestone - pin milestone
  pin_age_days       today - the pin's own qualifying release date       (None if the pin has no record)
  release_lag_days   latest qualifying release date - the pin's date     (None if the pin has no record)
  stale_days         today - the EARLIEST qualifying release newer than the pin (0 when none exists)

The SLA metric is ``stale_days``: zero while the pin is current, monotonic, does not reset when a newer
refresh ships. Breach when stale_days > DEFAULT_THRESHOLD_DAYS (day 14 is inside the SLA).

Outcomes (measurement): current | behind [breach] | undeterminable (fetch failure after one retry, an
unexpected response shape, or an unparseable pin — nothing is published, the run exits 1 so a blind
check is visible). Publication (--publish only): ok | lookup-failed (the tracker search errored — this
never means "no tracker exists"; nothing is created or edited) | mutation-failed (a create/edit/close
errored; named in the summary; never blindly retried). Publication failures exit 1 with the summary
already written. Annotations (::warning:: / ::error::) go to stdout as workflow commands; --summary
receives Markdown only.

Tracker: one issue per pin, identified by a visible body line ``Tracker key: roamux-pin-staleness/<pin>``
(searched with ``"roamux-pin-staleness" in:body``, then verified client-side — a title search alone
also matches roam-292 itself). Actions per run, in order: close every OPEN tracker whose pin is not the
current pin (superseded); then for the current pin: breach -> update the open tracker's body (silent),
or leave a closed one alone (the maintainer ended the episode), or create; within the SLA or current ->
close the open tracker. Overlapping runs (schedule + manual dispatch) can both create — accepted
residual under the hosted-job no-concurrency invariant; the next run lists every tracker it finds.

Usage:
  pin_staleness.py [--pin-file roamux/build/CHROMIUM_PIN] [--today YYYY-MM-DD] [--threshold-days 14]
                   [--summary FILE] [--dry-run | --publish] [--platform mac] [--repo owner/name]
                   [--remediation-issue N] [--retry-delay SECONDS]
"""

import argparse
import dataclasses
import datetime
import json
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request

DEFAULT_THRESHOLD_DAYS = 14
DEFAULT_REPO = "xinquan568/roamux"
DEFAULT_REMEDIATION_ISSUE = 293
TRACKER_LABELS = ("E0-foundation", "release")
TRACKER_MILESTONE = "v0.0.1"
KEY_PREFIX = "roamux-pin-staleness/"
SLA_DOC_URL = "https://github.com/xinquan568/roamux/blob/main/docs/security-uprev.md"
KEY_LINE_RE = re.compile(r"^Tracker key: roamux-pin-staleness/(\d+\.\d+\.\d+\.\d+)$", re.M)
DISCOVERY_LIMIT = 1000   # GitHub search caps results at 1000; hitting the cap is treated as truncation
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+\.\d+$")
VERSIONHISTORY = "https://versionhistory.googleapis.com/v1/chrome/platforms/{platform}/channels/stable/versions/all/releases"
CHROMIUMDASH_SCHEDULE = "https://chromiumdash.appspot.com/fetch_milestone_schedule?mstone={m}"


class PinError(Exception):
    pass


class Undeterminable(Exception):
    pass


@dataclasses.dataclass
class ReleaseDate:
    version: str
    date: datetime.date
    group: str


@dataclasses.dataclass
class Serving:
    version: str
    fraction: float
    group: str


@dataclasses.dataclass
class Selection:
    pin: str
    pin_date: datetime.date | None
    pin_group: str | None
    newer: list            # ReleaseDate, sorted by (date, version)
    first_newer: ReleaseDate | None
    latest: ReleaseDate | None
    rolling: list          # Serving, versions > pin serving below fraction 1


@dataclasses.dataclass
class Metrics:
    milestones_behind: int
    pin_age_days: int | None
    release_lag_days: int | None
    stale_days: int


@dataclasses.dataclass
class Measurement:
    status: str            # current | behind
    breach: bool
    metrics: Metrics | None
    selection: Selection | None
    threshold_days: int = DEFAULT_THRESHOLD_DAYS
    today: datetime.date | None = None


@dataclasses.dataclass
class Tracker:
    number: int
    state: str             # OPEN | CLOSED
    pin: str


@dataclasses.dataclass
class Publication:
    status: str            # ok | lookup-failed | mutation-failed | skipped
    detail: str = ""
    trackers: list = dataclasses.field(default_factory=list)
    actions: list = dataclasses.field(default_factory=list)
    done: list = dataclasses.field(default_factory=list)
    created: int | None = None


# --- pin and versions -------------------------------------------------------------------------

def parse_pin(text):
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if not VERSION_RE.match(line):
            raise PinError(f"CHROMIUM_PIN is not a four-part version: {line!r}")
        return line
    raise PinError("CHROMIUM_PIN holds no version line")


def version_tuple(version):
    return tuple(int(p) for p in version.split("."))


def milestone(version):
    return version_tuple(version)[0]


def _validated(record):
    """A release record with a four-part version, a numeric fraction (default 1) and a parseable
    serving.startTime; anything else is Undeterminable (a schema change must never be read as data)."""
    if not isinstance(record, dict):
        raise Undeterminable(f"release record is not an object: {record!r}")
    version = record.get("version")
    if not isinstance(version, str) or not VERSION_RE.match(version):
        raise Undeterminable(f"release record without a four-part version: {record.get('name', record)!r}")
    try:
        fraction = float(record.get("fraction", 1))
    except (TypeError, ValueError):
        raise Undeterminable(f"release record with a non-numeric fraction: {version}")
    serving = record.get("serving")
    start = serving.get("startTime") if isinstance(serving, dict) else None
    if not isinstance(start, str) or len(start) < 10:
        raise Undeterminable(f"release record without serving.startTime: {version}")
    try:
        date = datetime.date.fromisoformat(start[:10])
    except ValueError:
        raise Undeterminable(f"release record with an unparseable serving.startTime: {version} {start!r}")
    return version, fraction, date, str(record.get("fractionGroup", ""))


# --- qualification and selection ---------------------------------------------------------------

def release_dates(records):
    """version -> ReleaseDate (earliest qualifying interval). A record qualifies with fraction >= 1
    within its own group; a missing fraction counts as 1 (the API omits it for fully-served records)."""
    out = {}
    for r in records:
        version, fraction, date, group = _validated(r)
        if fraction < 1:
            continue
        if version not in out or date < out[version].date:
            out[version] = ReleaseDate(version, date, group)
    return out


def select(history, serving, pin):
    dates = release_dates(history)
    pin_rd = dates.get(pin)
    newer = sorted((rd for v, rd in dates.items() if version_tuple(v) > version_tuple(pin)),
                   key=lambda rd: (rd.date, version_tuple(rd.version)))
    latest = max(newer, key=lambda rd: version_tuple(rd.version)) if newer else None
    rolling = []
    for r in serving:
        version, fraction, _date, group = _validated(r)
        if fraction < 1 and version_tuple(version) > version_tuple(pin):
            rolling.append(Serving(version, fraction, group))
    rolling.sort(key=lambda s: version_tuple(s.version))
    return Selection(pin=pin, pin_date=pin_rd.date if pin_rd else None, pin_group=pin_rd.group if pin_rd else None,
                     newer=newer, first_newer=newer[0] if newer else None, latest=latest, rolling=rolling)


def metrics(sel, today):
    milestones_behind = (milestone(sel.latest.version) - milestone(sel.pin)) if sel.latest else 0
    pin_age = (today - sel.pin_date).days if sel.pin_date else None
    lag = ((sel.latest.date - sel.pin_date).days if sel.latest else 0) if sel.pin_date else None
    stale = (today - sel.first_newer.date).days if sel.first_newer else 0
    return Metrics(milestones_behind, pin_age, lag, stale)


def measure(pin, sel, today, threshold_days=DEFAULT_THRESHOLD_DAYS):
    m = metrics(sel, today)
    status = "behind" if sel.first_newer else "current"
    return Measurement(status=status, breach=m.stale_days > threshold_days, metrics=m, selection=sel,
                       threshold_days=threshold_days, today=today)


# --- fetching ------------------------------------------------------------------------------------

def default_fetch(url):
    with urllib.request.urlopen(url, timeout=30) as resp:
        return json.load(resp)


def _fetch_with_retry(fetch, url, retry_delay):
    last = None
    for attempt in range(2):
        try:
            return fetch(url)
        except Exception as e:  # noqa: BLE001 — any transport/parse failure is one retry, then undeterminable
            last = e
            if attempt == 0 and retry_delay:
                time.sleep(retry_delay)
    raise Undeterminable(f"fetch failed twice: {url} ({last})")


def _releases(doc, url):
    if not isinstance(doc, dict):
        raise Undeterminable(f"unexpected response shape (not an object): {url}")
    if not doc:
        return [], None
    if "releases" not in doc or not isinstance(doc["releases"], list):
        raise Undeterminable(f"unexpected response shape (no releases list): {url}")
    return doc["releases"], doc.get("nextPageToken")


def _paged(fetch, base, params, retry_delay):
    out, token = [], None
    while True:
        p = dict(params)
        if token:
            p["pageToken"] = token
        url = base + "?" + urllib.parse.urlencode(p)
        releases, token = _releases(_fetch_with_retry(fetch, url, retry_delay), url)
        out.extend(releases)
        if not token:
            return out


def fetch_versionhistory(fetch, platform, pin, retry_delay=1.0):
    base = VERSIONHISTORY.format(platform=platform)
    history = _paged(fetch, base, {"filter": f"version>={pin},fraction>=1", "order_by": "starttime asc",
                                   "pageSize": "500"}, retry_delay)
    serving = _paged(fetch, base, {"filter": "endtime=none", "pageSize": "100"}, retry_delay)
    return history, serving


def fetch_milestone_dates(fetch, milestones):
    """{milestone: 'YYYY-MM-DD'} from ChromiumDash's schedule; best effort — absence is not an error."""
    out = {}
    for m in milestones:
        try:
            doc = fetch(CHROMIUMDASH_SCHEDULE.format(m=m))
            date = (doc.get("mstones") or [{}])[0].get("stable_date", "")
            if date:
                out[m] = date[:10]
        except Exception:  # noqa: BLE001 — informational only
            continue
    return out


# --- tracker -------------------------------------------------------------------------------------

def tracker_key(pin):
    return KEY_PREFIX + pin


def tracker_title(pin):
    return f"Chromium pin staleness: {pin} is behind stable"


def default_gh(argv):
    return subprocess.run(["gh", *argv], check=True, capture_output=True, text=True, timeout=60).stdout


class LookupError_(Exception):
    """The tracker search did not yield a trustworthy answer; never interpreted as "no tracker"."""


def discover_trackers(gh, repo, limit=DISCOVERY_LIMIT):
    out = gh(["issue", "list", "--repo", repo, "--search", '"roamux-pin-staleness" in:body', "--state", "all",
              "--limit", str(limit), "--json", "number,state,body"])
    try:
        items = json.loads(out) if out and out.strip() else None
    except ValueError as e:
        raise LookupError_(f"gh issue list returned invalid JSON: {e}")
    if not isinstance(items, list):
        raise LookupError_("gh issue list returned no JSON array")
    if len(items) >= limit:
        raise LookupError_(f"tracker search truncated at {limit} results; refusing to decide on a partial view")
    trackers = []
    for item in items:
        number, state, body = (item.get("number"), item.get("state"), item.get("body")) if isinstance(item, dict) else (None, None, None)
        if (not isinstance(item, dict) or type(number) is not int or number <= 0
                or not isinstance(state, str) or state.upper() not in ("OPEN", "CLOSED")
                or not isinstance(body, str)):
            raise LookupError_(f"gh issue list returned an unexpected item: {item!r}")
        keys = sorted(set(KEY_LINE_RE.findall("\n".join(l.strip() for l in body.splitlines()))))
        if len(keys) == 1:
            trackers.append(Tracker(number=number, state=state.upper(), pin=keys[0]))
        # no standalone key line (a quotation, a placeholder) or conflicting keys: not a tracker
    return trackers


def decide_actions(measurement, trackers, pin):
    actions = [("close", t.number) for t in trackers if t.state == "OPEN" and t.pin != pin]
    mine = [t for t in trackers if t.pin == pin]
    open_mine = [t for t in mine if t.state == "OPEN"]
    closed_mine = [t for t in mine if t.state != "OPEN"]
    if measurement.breach:
        if open_mine:
            actions.append(("update", open_mine[0].number))
        elif closed_mine:
            actions.append(("leave-closed", closed_mine[0].number))
        else:
            actions.append(("create", None))
    else:
        actions.extend(("close", t.number) for t in open_mine)
    return actions


def apply_actions(actions, gh, repo, pin, title, body, today):
    """Runs the actions in order; the first failure is returned as mutation-failed with what was done.
    A create re-runs discovery immediately before creating: if the episode appeared meanwhile (an
    overlapping run) it is updated or left closed instead; if that second lookup fails, nothing is
    created (lookup-failed)."""
    done, created = [], None
    for kind, number in actions:
        try:
            if kind == "create":
                try:
                    again = [t for t in discover_trackers(gh, repo) if t.pin == pin]
                except Exception as e:  # noqa: BLE001 — a failed lookup never authorises a create
                    return Publication(status="lookup-failed", detail=f"re-check before create: {e}", actions=actions, done=done)
                open_now = [t for t in again if t.state == "OPEN"]
                if open_now:
                    gh(["issue", "edit", str(open_now[0].number), "--repo", repo, "--body", body])
                    done.append(("update", open_now[0].number))
                    continue
                if again:
                    done.append(("leave-closed", again[0].number))
                    continue
                url = gh(["issue", "create", "--repo", repo, "--title", title, "--body", body,
                          *sum((["--label", l] for l in TRACKER_LABELS), []), "--milestone", TRACKER_MILESTONE])
                m = re.search(r"/issues/(\d+)", url or "")
                created = int(m.group(1)) if m else None
                done.append(("create", created))
            elif kind == "update":
                gh(["issue", "edit", str(number), "--repo", repo, "--body", body])
                done.append((kind, number))
            elif kind == "close":
                gh(["issue", "close", str(number), "--repo", repo, "--comment",
                    f"Closed by the scheduled pin-staleness check on {today}: superseded or back within the SLA "
                    f"(current pin {pin})."])
                done.append((kind, number))
            else:
                done.append((kind, number))
        except Exception as e:  # noqa: BLE001 — a failed mutation is reported, never retried blindly
            return Publication(status="mutation-failed", detail=f"{kind} {number or ''}: {e}", actions=actions,
                               done=done, created=created)
    return Publication(status="ok", actions=actions, done=done, created=created)


# --- rendering -----------------------------------------------------------------------------------

def _fmt_days(v):
    return "unavailable (no qualifying record for the pin)" if v is None else f"{v} days"


def render_summary(measurement, publication, milestone_dates, error=None):
    lines = ["## Chromium pin staleness", ""]
    if error is not None:
        lines += [f"**Measurement: undeterminable** — {error}", "",
                  "Nothing was published. A blind check must be visible: this run is red on purpose.", ""]
        return "\n".join(lines)
    m, sel = measurement.metrics, measurement.selection
    verdict = ("**BREACH** of the SLA" if measurement.breach else
               ("behind, within the SLA" if measurement.status == "behind" else "current"))
    lines += [f"Pin `{sel.pin}` on {measurement.today}: **{measurement.status}** — {verdict} "
              f"(threshold {measurement.threshold_days} days on `stale_days`).", "",
              "| metric | value |", "| - | - |",
              f"| milestones_behind | {m.milestones_behind} |",
              f"| pin_age_days | {_fmt_days(m.pin_age_days)} |",
              f"| release_lag_days | {_fmt_days(m.release_lag_days)} |",
              f"| stale_days (SLA metric) | {m.stale_days} days |", ""]
    if sel.latest:
        lines.append(f"Latest qualifying Mac stable release: `{sel.latest.version}` ({sel.latest.date}, fraction group {sel.latest.group}).")
    if sel.first_newer:
        lines.append(f"First qualifying release newer than the pin: `{sel.first_newer.version}` ({sel.first_newer.date}, group {sel.first_newer.group}).")
    if sel.pin_date:
        lines.append(f"The pin's own qualifying release: {sel.pin_date} (group {sel.pin_group}).")
    if sel.rolling:
        lines.append("Rolling (serving below fraction 1, not counted as stable): " +
                     ", ".join(f"`{s.version}` ({s.fraction:g}, group {s.group})" for s in sel.rolling) + ".")
    if milestone_dates:
        lines.append("Announced stable dates (ChromiumDash): " + ", ".join(f"M{k} {v}" for k, v in sorted(milestone_dates.items())) + ".")
    lines += ["", "Dates are qualifying serving-interval start dates on the `mac` stable channel (UTC), not publication dates.", ""]
    if publication is not None:
        lines += [f"**Publication: {publication.status}**" + (f" — {publication.detail}" if publication.detail else ""), ""]
        if publication.trackers:
            counts = {}
            for t in publication.trackers:
                counts[t.pin] = counts.get(t.pin, 0) + 1
            lines.append("Trackers found: " + ", ".join(
                f"#{t.number} ({t.state.lower()}, pin {t.pin}" + (", DUPLICATE episode" if counts[t.pin] > 1 else "") + ")"
                for t in publication.trackers) + ".")
        if publication.done:
            lines.append("Actions: " + ", ".join(f"{k} #{n}" if n else k for k, n in publication.done) + ".")
        if publication.created:
            lines.append(f"Created tracker #{publication.created}.")
        if publication.actions and not publication.done:
            lines.append("Planned actions: " + ", ".join(f"{k} #{n}" if n else k for k, n in publication.actions) + ".")
        lines.append("")
    return "\n".join(lines)


def annotations(measurement, publication, error=None):
    out = []
    if error is not None:
        out.append(f"::error::pin staleness undeterminable: {error}")
    elif measurement.breach:
        m = measurement.metrics
        out.append(f"::warning::Chromium pin {measurement.selection.pin} is {m.stale_days} days behind stable "
                   f"({m.milestones_behind} milestone(s)); SLA is {measurement.threshold_days} days")
    if publication is not None and publication.status in ("lookup-failed", "mutation-failed"):
        out.append(f"::error::pin staleness publication {publication.status}: {publication.detail}")
    return "\n".join(out)


def render_issue_body(measurement, pin, milestone_dates, remediation_issue=DEFAULT_REMEDIATION_ISSUE):
    m, sel = measurement.metrics, measurement.selection
    lines = [f"The pinned Chromium `{pin}` is behind the stable channel (checked {measurement.today}, UTC).", "",
             "| metric | value |", "| - | - |",
             f"| milestones_behind | {m.milestones_behind} |",
             f"| pin_age_days | {_fmt_days(m.pin_age_days)} |",
             f"| release_lag_days | {_fmt_days(m.release_lag_days)} |",
             f"| stale_days (SLA metric) | {m.stale_days} days (SLA: {measurement.threshold_days}) |", ""]
    if sel.latest:
        lines.append(f"Latest qualifying Mac stable release: `{sel.latest.version}` ({sel.latest.date}, fraction group {sel.latest.group}).")
    if sel.first_newer:
        lines.append(f"First qualifying release newer than the pin: `{sel.first_newer.version}` ({sel.first_newer.date}, fraction group {sel.first_newer.group}).")
    if sel.rolling:
        lines.append("Rolling (below fraction 1, not counted): " + ", ".join(f"`{s.version}` ({s.fraction:g}, group {s.group})" for s in sel.rolling) + ".")
    if milestone_dates:
        lines.append("Announced stable dates: " + ", ".join(f"M{k} {v}" for k, v in sorted(milestone_dates.items())) + ".")
    lines += ["",
              f"SLA ([docs/security-uprev.md]({SLA_DOC_URL})): the pin is at most {DEFAULT_THRESHOLD_DAYS} days behind stable, measured as `stale_days` against qualifying "
              "Mac stable releases; a fixed release within 3 days of an in-the-wild CVE affecting shipped code is a human process "
              "the check does not cover.", "",
              f"Remediation: the uprev issue #{remediation_issue}. This tracker is updated in place by the scheduled check "
              "(body edits, no comments) and closed automatically when the pin is current or back within the SLA; closing it by "
              "hand ends this episode (no reopen). A new pin starts a new tracker.", "",
              f"Tracker key: {tracker_key(pin)}", ""]
    return "\n".join(lines)


# --- main ----------------------------------------------------------------------------------------

def _write_summary(path, text):
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text if text.endswith("\n") else text + "\n")


def main(argv=None, fetch=default_fetch, gh=default_gh, stdout=None):
    stdout = stdout or sys.stdout
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--pin-file", default="roamux/build/CHROMIUM_PIN")
    ap.add_argument("--today", type=datetime.date.fromisoformat, default=None, help="YYYY-MM-DD (UTC); default: today")
    ap.add_argument("--threshold-days", type=int, default=DEFAULT_THRESHOLD_DAYS)
    ap.add_argument("--summary", default=None, help="Markdown sink (e.g. $GITHUB_STEP_SUMMARY)")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--publish", action="store_true")
    ap.add_argument("--platform", default="mac")
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--remediation-issue", type=int, default=DEFAULT_REMEDIATION_ISSUE)
    ap.add_argument("--retry-delay", type=lambda s: max(0.0, float(s)), default=1.0)
    args = ap.parse_args(argv)
    today = args.today or datetime.datetime.now(datetime.timezone.utc).date()

    def fail(error):
        _write_summary(args.summary, render_summary(None, None, {}, error=error))
        print(annotations(None, None, error=error), file=stdout)
        return 1

    try:
        with open(args.pin_file, encoding="utf-8") as f:
            pin = parse_pin(f.read())
        history, serving = fetch_versionhistory(fetch, args.platform, pin, retry_delay=args.retry_delay)
        sel = select(history, serving, pin)
    except (PinError, Undeterminable, OSError) as e:
        return fail(str(e))
    measurement = measure(pin, sel, today, args.threshold_days)
    upto = milestone(sel.latest.version) if sel.latest else milestone(pin)
    milestone_dates = fetch_milestone_dates(fetch, range(milestone(pin), upto + 1))

    publication = None
    if args.publish:
        try:
            trackers = discover_trackers(gh, args.repo)
        except Exception as e:  # noqa: BLE001 — a failed lookup never means "no tracker"
            publication = Publication(status="lookup-failed", detail=str(e))
        else:
            actions = decide_actions(measurement, trackers, pin)
            body = render_issue_body(measurement, pin, milestone_dates, args.remediation_issue)
            publication = apply_actions(actions, gh, args.repo, pin, tracker_title(pin), body, today)
            publication.trackers = trackers

    _write_summary(args.summary, render_summary(measurement, publication, milestone_dates))
    ann = annotations(measurement, publication)
    if ann:
        print(ann, file=stdout)
    return 1 if (publication is not None and publication.status in ("lookup-failed", "mutation-failed")) else 0


if __name__ == "__main__":
    sys.exit(main())
