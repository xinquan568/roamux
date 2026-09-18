<!-- SPDX-License-Identifier: Apache-2.0 -->
# Security uprev SLA

Roamux ships a pinned upstream Chromium (`roamux/build/CHROMIUM_PIN`) and tracks the latest stable milestone by
re-pinning every milestone cycle (ADR 0001; the procedure is `docs/uprev.md`). That policy says *when* in the
release cycle to move; this document adds the *time bound* and the *security clause* (roam-292, grill H1), and
says exactly what the scheduled check measures and what it does not.

## The SLA

1. **The pin is at most 14 days behind stable.** "Behind stable" is measured as `stale_days`: the number of
   calendar days (UTC) since the **earliest qualifying stable release newer than the pin** first served on the
   Mac stable channel. `stale_days` is zero while the pin is the latest stable, grows monotonically once a newer
   stable exists, and does not reset when a further refresh ships. Day 14 is inside the SLA; day 15 is the first
   breach.
2. **A fixed release within 3 days of an in-the-wild CVE affecting shipped code.** This clause is a human process
   (below); no automation decides exploitation or applicability.

## What "stable" means here

- **Source:** the [VersionHistory API](https://developer.chrome.com/docs/web-platform/versionhistory), channel
  `stable`, platform **`mac`**. `mac` is the x86_64 platform id; `mac_arm64` is a separate id. Roamux ships one
  universal2 build from one pin, so `mac` is used as the representative platform and this is a stated limitation —
  if the two ever diverge, the check reports what `mac` says. Git tags cannot serve this purpose: every version on
  every channel is tagged, and tags carry neither channel nor release date.
- **A release qualifies** when it has a serving record with `fraction` 1 **within its own `fractionGroup`** (the
  API's `fraction>=1` filter). Fractions are comparable only within a group, so the check never compares them across
  groups and makes no claim about global rollout completion. Versions serving below fraction 1 are reported as
  *rolling* and do not count — the first day of a rollout does not start the clock.
- **Release date** = the earliest qualifying `serving.startTime` for that version (a UTC calendar date). These are
  serving-interval dates, not initial-publication dates.

## What the scheduled check does

`.github/workflows/pin-staleness.yml` runs `roamux/build/ci/pin_staleness.py` daily (19:00 UTC, a preference — GitHub
may delay or drop scheduled runs; `workflow_dispatch` runs it on demand). It writes to the step summary:

| metric | meaning |
| - | - |
| `milestones_behind` | latest qualifying milestone − the pin's milestone |
| `pin_age_days` | today − the pin's own qualifying release date (unavailable if the pin has no qualifying record) |
| `release_lag_days` | latest qualifying release date − the pin's release date (unavailable likewise) |
| `stale_days` | **the SLA metric** — today − the earliest qualifying release newer than the pin (0 when none) |

Outcomes: **current**, **behind** (with **breach** past 14 days), or **undeterminable** (the source could not be read
after one retry, an unexpected response, or an unparseable pin — nothing is published and the run is red so a blind
check is visible).

On the canonical repository's `main` branch only, the check keeps **one tracking issue per staleness episode**
(title `Chromium pin staleness: <pin> is behind stable`, identified by the body line
`Tracker key: roamux-pin-staleness/<pin>`, labels `E0-foundation` + `release`, milestone `v0.0.1`): on a breach it
creates the tracker or silently updates its body; when the pin is current or back within the SLA it closes the
tracker with one comment; when the pin moves, the old episode's tracker is closed and a new one starts if the new pin
is also in breach. Closing a tracker by hand ends that episode — the check never reopens it. Two overlapping runs can
both create a tracker (an accepted residual: hosted jobs carry no concurrency group); the next run lists every
tracker it finds. A failed tracker lookup never means "no tracker": nothing is created, the run is red. Everywhere
else (forks, other branches) the check only writes the summary.

The tracker links the current remediation issue; the uprev itself is done per `docs/uprev.md`.

## The CVE clause — a human process

The scheduled check cannot decide whether a CVE is exploited in the wild or whether it affects code Roamux ships;
that is the maintainer's triage. The policy chosen here (open to revision):

- **Owner:** the maintainer.
- **Evidence:** the Chrome release notes' "exploited in the wild" wording for a CVE, or a CISA KEV entry for it, **and**
  the affected component is in code Roamux ships (Roamux carries no patches that remove upstream components, so in
  practice: any exploited Chrome CVE in the pinned milestone's code).
- **Clock origin:** the earlier of the vendor's notice and the maintainer becoming aware of it.
- **Target:** a **fixed release delivered** (tag, build, appcast) within 3 days. Moving the pin is the first step, not
  the finish line.

## What the check does not cover

- It does not know about CVEs, exploitation, or applicability (clause 2 is manual).
- It reports the `mac` channel only.
- It does not move the pin; it points at the remediation issue. The uprev procedure, the flag obligations and the
  rebrand gates are `docs/uprev.md`'s.
