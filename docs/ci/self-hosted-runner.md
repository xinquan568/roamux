<!-- SPDX-License-Identifier: Apache-2.0 -->
# Self-hosted builder — tier-2 warm-cache (roam-36, plan §12.6, personal-machine v1)

The tier-2 fast path: trusted jobs build Chromium **incrementally from a warm base in minutes** on a
self-hosted Apple-silicon runner. `vars.ROAMUX_CI_CHROMIUM_RUNNER` is a **kill switch**, not a soft
skip (roam-281): when it is **empty or unset**, the required `targeted-suite-selfhosted` check runs on
a hosted runner for same-repo branches and fails **red** in its first step (after any wait for the
`roamux-shared-base` group), so merges are blocked; branch protection treats a *skipped* required
check as satisfied, which is why the old `if:`-gate was a vacuous pass. Fork PRs are unchanged (tier-1
only, R15). The variable's value is never a label — any non-empty value turns the switch on; a switch
that is on with no registered runner matching the fixed label triple queues the job until GitHub's
queue timeout (blocks merge, not red). Whether re-running a completed run re-reads the variable is
unverified: after changing it, push (or re-run) and *observe* where the job was routed.

## v1 posture vs full §12.6 — read this first

This is the **personal-machine v1** (Q(i5)-B: user-provisioned, the operator's own Mac). Delivered:
a standing labeled runner (`self-hosted, macos, chromium-builder`); the pinned checkout
(`~/chromium/src` per `CHROMIUM_PIN`) as the shared warm base; a CI-owned build dir (`out/CI`,
APFS-cloned copy-on-write from the operator's `out/Default`); **declared-channel** base access (the
job touches the base only via the overlay symlink — restored by `tier2_job.sh`'s EXIT trap and, in
the release workflow, by a final `if: always()` step (roam-279); the flip refuses a real directory at
the link path and the trap reports a restore it cannot perform instead of linking into it
(roam-280) — and the reconciling (`--reconcile`, roam-341) idempotent fail-loud runhook; test-enforced
structurally and behaviourally by `test_tier2_job.py` and `test_workflow_invariants.py`).

**The base's tracked state is CI-owned (roam-175).** Every tier-2/release run first reconciles
`~/chromium/src` to HEAD plus the job's own patch stack, through the runhook's `--reconcile` mode
(roam-341; formerly `git reset --hard HEAD` + `git clean -fd -e /roamux` + apply). The runhook
lets git do it — `read-tree --reset -u` to the tree "HEAD + the simulated stack", then
`clean -fd -e /roamux` (never `-x`, so `out/*` and all ignored caches survive; `-e /roamux` spares
the overlay symlink; single `-f` never enters the DEPS submodules), then the index back to HEAD;
HEAD is never written — with one deliberate difference from the old sequence: a patched path whose
existing regular-file entry already has the stack's bytes and mode is left untouched, so its mtime survives and the retained
`out/CI` no longer re-invalidates every dependent of every patched header on a stack-identical run
(measured before the change: ~1,100 Siso timestamp invalidations and a ~24-minute build phase on a
PR that changed no C++). Rationale for reconciling at all: the
runhook's stack simulator (roam-77) matches the tree only against prefixes of the *current*
stack, so a base left at a superseded stack — any patch-rewriting or patch-deleting change, first
hit by roam-160/PR #173 — matches no prefix and fails the job; a prior release's
`rebrand_strings.py` mutations would silently contaminate the next build. Operator consequence:
**uncommitted local edits to the base do not survive a CI run** — transient local stack states
(the local build-gate flow) are fair game by design; anything you want to keep must live in the
overlay or a patch.

**Not delivered (the upgrade path, in order):** a dedicated low-privilege runner user (clean HOME —
no keychain/SSH/`gh` tokens); filesystem-enforced read-only base; JIT/ephemeral runners; per-job VM
snapshots (`tart`/Anka). Adopt these the moment this stops being a single-operator machine.

## Security model (single-operator repo)

- **Host exposure, owned:** CI jobs run as the runner's login user — anything that user can read
  (keychain, SSH keys, `gh` tokens, dotfiles) is reachable by CI-executed code. v1's accepted-risk
  argument: the **trust predicate** below means only code someone with push rights already chose to
  push can reach this runner — on this repo, that is the operator.
- **Trust predicate (enumerated exactly in the workflows, invariant-enforced):** `ci.yml` —
  protected-`main` pushes OR same-repo (non-fork) PRs; `nightly.yml` — the schedule OR a main-only
  manual dispatch. **No catch-alls.** Fork PRs are structurally unable to reach self-hosted labels
  (R15). **Tighten to protected-refs/merge-queue the day this repo gains outside collaborators.**
- **No GitHub secrets on this tier** — tier-3 secrets live only in the protected `release`
  Environment.

## Runbook

```sh
# Provision (idempotent; --dry-run to preview):
bash roamux/build/ci/provision_runner.sh
# Machine-specific env for jobs (written once):
cat > ~/roamux-runner/.env <<ENV
ROAMUX_CHROMIUM_SRC=<abs path to the pinned checkout, e.g. /Users/you/chromium/src>
ROAMUX_DEPOT_TOOLS=<abs path to depot_tools>
ROAMUX_CANONICAL_OVERLAY=<abs path to codes/roamux/roamux — the symlink restore target>
ENV
# .env format contract (roam-286): the release job PARSES this file, it never sources it. Only
# plain KEY=value lines (NAME = [A-Z][A-Z0-9_]*; value = [A-Za-z0-9_./:@+-]* — absolute paths
# without spaces), blank lines and # comments are accepted; quotes, `export`, $(...)/backticks,
# spaces, CRLF line endings or any command fail the job naming the line. The job exports only
# the three ROAMUX_* keys above; other well-formed lines (the actions-runner reads this same
# file as its own env file) are ignored.
# Start (session-lifetime; dies on reboot):
cd ~/roamux-runner && nohup ./run.sh >runner.log 2>&1 &
# Persist across reboots (deeper machine mutation — operator choice):
cd ~/roamux-runner && ./svc.sh install && ./svc.sh start
# Enable tier-2 jobs (any non-empty value turns the switch on; the value is not a label):
gh variable set ROAMUX_CI_CHROMIUM_RUNNER --body roamux-builder-1 --repo <owner>/<repo>
# Decommission (do BOTH — a set variable with a dead runner queues jobs until timeout; deleting the
# variable engages the kill switch: same-repo PRs go RED on targeted-suite-selfhosted until it is set again):
cd ~/roamux-runner && ./config.sh remove --token "$(gh api -X POST repos/<o>/<r>/actions/runners/remove-token --jq .token)"
gh variable delete ROAMUX_CI_CHROMIUM_RUNNER --repo <owner>/<repo>
```

## What tier-2 runs (roam-282)

`tier2_job.sh` builds and runs four targets, in this order and with **no `--gtest_filter`**:
`roamux_unittests`, `roamux_browser_unittests`, `roamux_sparkle_tests`, `roamux_browsertests`.
Unfiltered is deliberate: a `Roamux*` filter once silently dropped the only fixture not named
`Roamux*` (`ThreeCarrierTest`, grill H14) for months. The hermetic test
`roamux/build/tests/test_browsertest_fixtures.py` proves on every PR that every gtest source the
overlay owns, every WebUI test input and every test-named upstream file a patch touches is built
AND run by this script with every case admitted — or is listed in
`roamux/build/ci/unbuilt_tests_register.txt` with an owner and a re-entry condition. A register row
is rejected the moment its item becomes reachable, or tier-2 starts building and running the
upstream target it names, so the row must be deleted in the same change (the ADR 0003 re-entry
idiom). Today the register holds the settings-about WebUI suite (patch 0033 → `browser_tests`) and
the two patch-pinned upstream fixtures (0061/0062 → `unit_tests`), all waiting on the M41 nightly
leg (roam-295 umbrella). The job log shows each suite in its own `::group::run …` block, which is
the evidence that a suite ran.

## Flake signal and artifacts (roam-283)

Every suite runs with `--test-launcher-retry-limit=2` (roam-195), so a test that passes only on a
retry used to be invisible in the job's verdict. Since roam-283 each suite writes its launcher
summary (`--test-launcher-summary-output`) and a tee'd log into one artifact directory
(`$ROAMUX_CI_ARTIFACTS`, resolved from `RUNNER_TEMP` in the "Tier-2 artifact directory" step), and
two steps run after the script **whenever it ran, green or red** (`if: always() && steps.tier2.outcome != 'skipped'`
— on the hosted kill-switch path the script is skipped and so are they):

- **Flake report** — `roamux/build/ci/flake_report.py` writes `## Tier-2 flake report` into the
  step summary: per suite its state (`complete`, or `absent` / `malformed` / `incomplete` — each an
  error naming the suite's log and listing possible causes without asserting one), the tests that
  needed a retry (attempt count, status sequence, per-attempt elapsed, the failed attempt's
  snippet), final non-success tests, and skips (`GTEST_SKIP()` shows up as a `result_parts` skip
  entry — listed only; failing on skips is L17). Retried tests are matched against
  `roamux/build/ci/known_flakes.txt`: rows `<Fixture.Case | Fixture.*> | <owner roam-N> | <note>`
  under a `mode:` directive. The ledger is in **`mode: fail`**. roam-308 flipped it after sweeping
  every tier-2 artifact of the warn-only seeding window, which ran from 2026-09-06; every name still
  retrying on current code got a row with an open owner. In fail mode:
  - A **retried** test (more than one attempt, including one whose retries never passed) whose name
    matches no row is a `::error::`, and so is a **stale** row. A row is stale when its pattern
    matches no test in the `all_tests` of the suite summaries available that run. An unlisted retry
    that never passed draws both the retry error (its text still says "passed only on retry") and
    the final-status error.
  - Unaffected by the mode: a final non-success test (including a retry that never passed), an
    invalid ledger and an absent/malformed/incomplete summary are errors in either mode. No row
    exempts them.
  - Matching is by **name only**. A new failure signature on a listed name stays suppressed but is
    shown in the report, so owners read their rows' occurrences. Keep rows name-exact (no fixture
    wildcards), with the observed signature in the note.

  Listed flakes are always shown. Closing an owner issue deletes its row in the same change. The
  step has no `continue-on-error`, so a red report fails `targeted-suite-selfhosted`, the required
  check for same-repo PRs and `main` pushes.

  **When the Flake report goes red**, start from the error class in the step summary:
  1. *Absent / malformed / incomplete summary* for a suite: a harness or job problem. Read the named
     `<suite>.log` and fix that, never the ledger.
  2. *Final non-success test* (including a retry that never passed): a real failure. Fix it; no row
     can exempt it.
  3. *Unlisted retry* that passed on a retry (if it never passed, class 2 applies as well): fix the
     test, or file an owner issue (`roam-N`, with the run, the attempt
     statuses and the failing attempt's snippet) and add a name-exact row whose note records the
     signature. Do this in the blocked PR or in a dedicated one.
  4. *Stale row*: first check that every suite summary was complete (a missing suite makes valid
     rows look stale). Then confirm the test was really removed or renamed, and delete or rename the
     row with it.
  5. *Invalid ledger*: fix the syntax (exactly one `mode:` directive; three `|`-separated fields per
     row; the owner is `roam-N`).

  Then push the fix or ledger change and get the checks green.
- **Upload tier-2 artifacts** — `actions/upload-artifact@v4`, artifact `tier2-artifacts`, 14 days:
  `<suite>.json` + `<suite>.log` for every suite that ran. Download with
  `gh run download <run-id> -n tier2-artifacts`.

The script also prints cumulative checkpoints `phase=<name> elapsed=<s>s` (reconcile, runhook,
sparkle, rebrand-gate, signing-gate, clone, build, run:<suite> ×4, done) to the log and
the step summary, so a run killed by the 12 h bound is attributable to the phase it was in. Bounds:
the step summary is published when the step ends (runner loss leaves only the log); the runs are
keyless, but logs and snippets are not privacy-scrubbed (paths and hostnames may appear).

## Cache model

`out/CI` is an APFS clone of the operator's warm `out/Default` (near-instant, ~zero disk until
divergence) made on first job use; ninja then builds incrementally. Refresh = delete `out/CI` (the
next job re-clones). The clone is restart-safe (roam-280): it is made into `out/CI.partial`, checked
for `build.ninja`/`args.gn`, and renamed into place atomically; a directory without them is an
interrupted clone and is redone — for the default `out/CI` only; an overridden `ROAMUX_CI_OUT` in
that state is refused, never deleted. Known v1 contention: the runner shares the machine/checkout with local
development. (The `nightly` workflow is `disabled_manually` at the GitHub level as of 2026-09-18 — the reason is
not recorded in this tree; the scheduled pin-staleness check, `pin-staleness.yml`, is hosted-only and deliberately
does not depend on it, roam-292.) The three base-mutating jobs (tier-2, nightly, release) are serialized by the
`roamux-shared-base` concurrency group (roam-279: `cancel-in-progress: false` — a running build is
never killed by a newcomer; `queue: max` — waiting jobs queue in order, where GitHub's default
single pending slot would cancel the older pending job) — declared, so it still holds if a second
runner is ever added. tier-2 and nightly carry an explicit `timeout-minutes: 720` (release: 1440):
GitHub's silent 6h default service-cancelled a cold nightly (run 29827734729) mid-compile; a stated
bound fails such a run honestly instead. Each run reconciles the base to HEAD + its own stack in one step (byte-identical patched files
untouched; roam-175/roam-341 — so whatever applied set a run leaves behind, the next run
recovers; before roam-175 a patch-rewriting PR wedged the runhook until a manual reset); avoid
heavy local builds while a CI job runs — the reconcile will reset a racing local stack mid-build.

## Power: the builder must be on AC (roam-258)

**The builder idle-sleeps after ~1 minute on battery** (`pmset -g custom` → `Battery Power: sleep 1`;
AC is `0`). A long compile is not user activity, so the machine sleeps, the runner stops renewing the
job lease, GitHub invalidates it, and the job is reported as **a failure with no failing step** ~30-50
minutes later. Observed 2026-08-02 on PR #257 (run 30742307640, cancelled at 32 min); the identical
commit passed in 30m07s on AC with no code change.

Two mechanisms now defend against this, and they are **not** interchangeable:

| Mechanism | Where | What it actually guarantees |
| --- | --- | --- |
| `require_ac_power.sh` pre-flight | first thing in `tier2_job.sh` (so: ci + nightly), and an early step in `release.yml` | **Deterministic.** A job *started* on battery is refused in ~2 s with a clear message, instead of dying at 30+ minutes with no failing step. Fails **closed** — an unreadable or unrecognised `pmset` result is an error, never an assumed AC. |
| `caffeinate -sim` | `tier2_job.sh` re-execs itself under it; `release.yml` wraps its per-slice `autoninja` and the universalizer | **Unverified risk reduction**, aimed at the case the pre-flight cannot see: a machine on AC at step time that is **unplugged mid-run**. |

`ROAMUX_ALLOW_BATTERY=1` overrides the pre-flight. It **waives the guarantee**: the job proceeds and
may still be killed mid-run. Use it deliberately, not habitually.

**Known unverified:** whether `caffeinate -i` (`PreventUserIdleSystemSleep`) actually defeats this
machine's aggressive `sleep 1` battery setting has **not** been confirmed end-to-end. What *was*
measured: `caffeinate -sim` does raise `PreventUserIdleSystemSleep` and `PreventSystemSleep` in
`pmset -g assertions`, and both drop when the wrapped child exits. Confirming the rest needs the
machine physically unplugged. Do not record this fix as proven until someone does that.

### Triage: idle sleep vs a builder network drop

These look nearly identical — a failed job with no failing step — but the correct responses are
opposite. The reliable discriminator is the **renewal pattern** in `~/roamux-runner/_diag/Runner_*.log`:

| | Renewal pattern | Correct response |
| --- | --- | --- |
| **Idle sleep** | a last `Successfully renew job`, then **total silence for tens of minutes**; network errors appear only on wake | plug the builder in / hold a power assertion. Re-run. |
| **Network / DNS drop** | continuous renewal **attempts** right up to the failure, failing with `TimedOut` / `HostNotFound` | **nothing** — let the next run supply the signal |

Renewals normally fire about every 60 s, so a gap is unambiguous. The pre-flight's failure message
repeats this table, so it is present in the log where the failure happens and not only here.

### A caveat when testing power behaviour on this machine

Other processes may already hold `PreventUserIdleSystemSleep` — some editors and agent tools take a
rolling `caffeinate -i` while they run, and an orphaned `caffeinate` can outlive its parent
(`pgrep -fl caffeinate`, `pmset -g assertions`). If one is held, the builder is protected *by
accident* and any "does it sleep?" experiment is invalid. Check for and clear stray holders before
concluding anything about power behaviour. This also plausibly explains some historical
intermittency: a job launched from an active interactive session can inherit protection that a
push-triggered 3am run does not.
