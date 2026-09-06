#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Tier-2 flake report (roam-283, grill H17).

Reads the per-suite launcher summaries (`--test-launcher-summary-output`) that tier2_job.sh writes
into the artifact directory, lists what the job's green/red verdict hides — tests that passed only
on retry, final non-success statuses, skips — into the GitHub step summary, and checks retried
tests against the ledger roamux/build/ci/known_flakes.txt.

Summary states (each abnormal state is an ERROR that names the suite's tee'd log and lists
possible causes without asserting one):
  absent      no <suite>.json (the suite never started, the launcher died before writing, or the
              save failed and only logged)
  malformed   JSON parse error, or per_iteration_data not a list of dicts, or a test whose attempts
              are not a non-empty list of dicts carrying a string `status` (an interrupted write is
              one way to get here)
  incomplete  the startup placeholder (`EARLY_SUMMARY` in global_tags) or NOTRUN entries — a kill
              after startup, a broken-test early exit or a failed final save can all leave it
  complete    everything else

Classification (per iteration — the outer per_iteration_data list is iterations, not attempts):
  retried            attempt list longer than one
  final non-success  last attempt's status != SUCCESS (the suite already failed; recorded)
  skipped            any attempt with status SKIPPED, or a result_parts entry of type "skip"
                     (GTEST_SKIP() is recorded as SUCCESS + that part) — informational only here;
                     failing on skips is L17's job

Ledger: `mode: warn|fail` (exactly one directive) then rows `<pattern> | <owner roam-N> | <note>`;
'*' wildcards within the test name. warn: unlisted retried → ::warning::, exit 0. fail: unlisted
retried → ::error::, exit 1. Stale rows (matching nothing in any suite's all_tests — the known
universe, distinct from the executed set) are notes in warn mode and errors in fail mode.
Exit 1 whenever a suite is not complete, a test ends non-success, the ledger is invalid, or the
artifact directory is missing.
"""

import argparse
import json
import pathlib
import re
import sys

DEFAULT_SUITES = ("roamux_unittests", "roamux_browser_unittests", "roamux_sparkle_tests",
                  "roamux_browsertests")
OWNER_RE = re.compile(r"\broam-\d+\b")
SNIPPET_CHARS = 400
ABNORMAL_CAUSES = {
    "absent": "the suite never started, the launcher died before writing, or the save failed "
              "(the launcher only logs a failed save)",
    "malformed": "an interrupted write (the launcher replaces the file with CREATE_ALWAYS) or a "
                 "schema change in the launcher",
    "incomplete": "a kill after startup, a broken-test early exit, or a failed final save that "
                  "left the startup placeholder in place",
}


class LedgerError(ValueError):
    pass


def parse_ledger(text):
    """-> (mode, [(pattern, owner, note, lineno)]); raises LedgerError."""
    mode, rows = None, []
    for n, line in enumerate(text.splitlines(), start=1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith("mode:"):
            if mode is not None:
                raise LedgerError(f"line {n}: duplicate `mode:` directive")
            value = s.split(":", 1)[1].strip()
            if value not in ("warn", "fail"):
                raise LedgerError(f"line {n}: invalid mode {value!r} (expected `mode: warn` or `mode: fail`)")
            mode = value
            continue
        fields = [f.strip() for f in s.split("|")]
        if len(fields) != 3:
            raise LedgerError(f"line {n}: expected 3 '|'-separated fields, got {len(fields)}")
        pattern, owner, note = fields
        if not pattern:
            raise LedgerError(f"line {n}: empty test pattern")
        if not OWNER_RE.search(owner):
            raise LedgerError(f"line {n}: owner {owner!r} names no roam-N")
        if not note:
            raise LedgerError(f"line {n}: empty note")
        rows.append((pattern, owner, note, n))
    if mode is None:
        raise LedgerError("missing `mode: warn|fail` directive")
    return mode, rows


def pattern_matches(name, pattern):
    """'*' matches any run of characters; everything else is literal (gtest-style)."""
    rx = "".join(".*" if ch == "*" else re.escape(ch) for ch in pattern)
    return re.fullmatch(rx, name) is not None


def load_summary(path):
    """-> (state, doc_or_None, detail)."""
    if not path.exists():
        return "absent", None, "no summary file"
    try:
        doc = json.loads(path.read_text())
    except (OSError, ValueError) as e:
        return "malformed", None, f"JSON parse error: {e}"
    if not isinstance(doc, dict):
        return "malformed", None, "top level is not an object"
    iterations = doc.get("per_iteration_data")
    if not isinstance(iterations, list) or not all(isinstance(i, dict) for i in iterations):
        return "malformed", doc, "per_iteration_data is not a list of objects"
    for it in iterations:
        for name, attempts in it.items():
            if (not isinstance(attempts, list) or not attempts
                    or not all(isinstance(a, dict) and isinstance(a.get("status"), str) for a in attempts)):
                return "malformed", doc, f"{name}: attempts must be a non-empty list of objects with a string status"
    tags = doc.get("global_tags") or []
    notrun = any(a["status"] == "NOTRUN" for it in iterations for atts in it.values() for a in atts)
    if "EARLY_SUMMARY" in tags or notrun:
        return "incomplete", doc, ("startup placeholder (EARLY_SUMMARY)" if "EARLY_SUMMARY" in tags
                                   else "NOTRUN entries present")
    return "complete", doc, ""


def classify(doc):
    """-> (retried, final_non_success, skipped): lists of (iteration_index, name, attempts).
    Iterations are kept apart — a test may pass on retry in one iteration and crash in the next."""
    retried, failed, skipped = [], [], []
    for k, it in enumerate(doc["per_iteration_data"]):
        for name, attempts in it.items():
            if len(attempts) > 1:
                retried.append((k, name, attempts))
            if attempts[-1]["status"] not in ("SUCCESS", "SKIPPED"):   # a skip is not a failure
                failed.append((k, name, attempts))
            if any(a["status"] == "SKIPPED"
                   or any(isinstance(p, dict) and p.get("type") == "skip" for p in (a.get("result_parts") or []))
                   for a in attempts):
                skipped.append((k, name, attempts))
    return retried, failed, skipped


def _statuses(attempts):
    return " → ".join(a["status"] for a in attempts)


def _elapsed(attempts):
    return ", ".join(str(a.get("elapsed_time_ms", "?")) for a in attempts)


def _plain(text):
    """One line, no backticks — safe inside a Markdown list item."""
    return " ⏎ ".join(l.strip() for l in text.splitlines() if l.strip()).replace("`", "'")


def _snippets(attempts):
    out = []
    for i, a in enumerate(attempts, start=1):
        if a["status"] != "SUCCESS":
            snip = (a.get("output_snippet") or "").strip()
            if snip:
                out.append(f"attempt {i} ({a['status']}): {_plain(snip[:SNIPPET_CHARS])}")
    return out


def render_and_check(artifacts, suites, ledger_path):
    """-> (markdown_lines, annotations, exit_code)."""
    md, ann, rc = ["## Tier-2 flake report"], [], 0
    try:
        mode, rows = parse_ledger(ledger_path.read_text())
    except (OSError, LedgerError) as e:
        ann.append(f"::error::{ledger_path.name}: {e}")
        md.append(f"**Ledger error:** {e}")
        return md, ann, 1
    md.append(f"_mode: **{mode}** — ledger `{ledger_path}` ({len(rows)} row{'s' if len(rows) != 1 else ''})_")
    if not artifacts.is_dir():
        ann.append(f"::error::tier-2 produced no artifacts directory at {artifacts}")
        md.append(f"**no artifacts:** `{artifacts}` does not exist — tier-2 produced no artifacts.")
        return md, ann, 1

    known_universe, universe_available, executed = set(), False, set()
    retried_names = set()
    for suite in suites:
        path = artifacts / f"{suite}.json"
        log = f"{suite}.log"
        state, doc, detail = load_summary(path)
        if doc and isinstance(doc.get("all_tests"), list):
            universe_available = True                     # even an empty list is an answer
            known_universe.update(str(x) for x in doc["all_tests"])
        if state != "complete":
            rc = 1
            ann.append(f"::error::{suite}: summary {state} ({detail}) — see {log}")
            md.append(f"### {suite} — **{state}**")
            md.append(f"{detail}. See `{log}`. Possible causes: {ABNORMAL_CAUSES[state]}; this report asserts none.")
            if doc and doc.get("global_tags"):
                md.append(f"global_tags: `{', '.join(map(str, doc['global_tags']))}`")
            continue
        retried, failed, skipped = classify(doc)
        n_iter = len(doc["per_iteration_data"])
        executed.update(n for it in doc["per_iteration_data"] for n in it)
        n_tests = sum(len(it) for it in doc["per_iteration_data"])
        md.append(f"### {suite} — complete ({n_tests} tests run, {len(retried)} retried, "
                  f"{len(failed)} final non-success, {len(skipped)} skipped)")
        if doc.get("global_tags"):
            md.append(f"global_tags: `{', '.join(map(str, doc['global_tags']))}`")
        entries = []                                      # (k, name, attempts) — one row per iteration
        for e in retried + failed:
            if e not in entries:
                entries.append(e)
        entries.sort(key=lambda e: (e[0], e[1]))
        table, snippets = [], []
        for k, name, attempts in entries:
            label = f"`{name}`" + (f" (iteration {k + 1})" if n_iter > 1 else "")
            row = next(((pat, o, note) for pat, o, note, _ in rows if pattern_matches(name, pat)), None)
            is_retry = len(attempts) > 1
            if row:
                ledger_cell = f"{row[1]} ({row[2]})"
                if is_retry:
                    retried_names.add(name)
            elif is_retry:
                retried_names.add(name)
                ledger_cell = "unlisted → " + ("warning" if mode == "warn" else "ERROR")
                ann.append(f"::{'warning' if mode == 'warn' else 'error'}::{suite}: {name} passed only on retry "
                           f"({_statuses(attempts)}) — not in {ledger_path.name}")
                if mode == "fail":
                    rc = 1
            else:
                ledger_cell = "—"
            table.append(f"| {label} | {len(attempts)} attempts | {_statuses(attempts)} | {_elapsed(attempts)} | {ledger_cell} |")
            for s in _snippets(attempts):
                snippets.append(f"- `{name}`" + (f" (iteration {k + 1})" if n_iter > 1 else "") + f" — {s}")
            if attempts[-1]["status"] not in ("SUCCESS", "SKIPPED"):
                rc = 1
                ann.append(f"::error::{suite}: {name} final status {attempts[-1]['status']} ({_statuses(attempts)})")
        if table:
            md.append("| test | attempts | statuses | elapsed (ms) | ledger |")
            md.append("|---|---|---|---|---|")
            md.extend(table)
        if snippets:
            md.append("snippets (non-success attempts):")
            md.extend(snippets)
        if skipped:
            md.append("skipped (informational; L17 will enforce): " + ", ".join(f"`{n}`" for _, n, _ in skipped))

    # Ledger section: every row's status this run. Stale = matches nothing in the known universe
    # (the union of all_tests), which is distinct from "not executed this run".
    md.append("### Ledger")
    if not universe_available:
        md.append("stale rows not checked — no summary carried an all_tests list this run.")
    for pat, owner, note, n in rows:
        if any(pattern_matches(x, pat) for x in retried_names):
            status = "retried in this run (listed)"
        elif any(pattern_matches(x, pat) for x in executed):
            status = "listed, not retried"
        elif universe_available and not any(pattern_matches(x, pat) for x in known_universe):
            status = "**stale** — matches no test in any suite's all_tests"
            if mode == "fail":
                rc = 1
                ann.append(f"::error::ledger line {n}: `{pat}` ({owner}) is stale — matches no test in any suite's all_tests")
        elif universe_available:
            status = "listed, not executed this run"
        else:
            status = "listed (universe unavailable)"
        md.append(f"- line {n}: `{pat}` | {owner} | {note} — {status}")
    md.append(f"**Verdict:** exit {rc} ({'all suites complete' if rc == 0 else 'see errors above'}; mode {mode}).")
    return md, ann, rc


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--artifacts", required=True, type=pathlib.Path)
    ap.add_argument("--ledger", required=True, type=pathlib.Path)
    ap.add_argument("--summary", type=pathlib.Path, help="GitHub step summary file to append to")
    ap.add_argument("--suites", default=",".join(DEFAULT_SUITES), help="comma-separated suite names")
    args = ap.parse_args(argv)
    suites = [s for s in args.suites.split(",") if s]
    md, ann, rc = render_and_check(args.artifacts, suites, args.ledger)
    text = "\n".join(md) + "\n"
    print(text)
    for a in ann:
        print(a)
    if args.summary:
        with open(args.summary, "a") as f:
            f.write(text)
    return rc


if __name__ == "__main__":
    sys.exit(main())
