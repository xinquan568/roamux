#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Roamux tier-2 CI job (roam-36, plan §12.6 personal-machine v1): warm-base incremental build + the
# Roamux test suites, run on the self-hosted runner. The shared base checkout is touched ONLY via the
# two declared, restored channels: (1) the overlay symlink (flipped to this job's checkout, restored
# by the EXIT trap), (2) the pristine-reconcile + idempotent fail-loud patch runhook (roam-175: the
# base's tracked state is CI-owned). No sudo; no secrets on this tier.

# roam-258: hold a power assertion for the WHOLE job before anything else runs.
# The builder idle-sleeps after ONE minute on battery (pmset -b sleep 1); a long
# compile is not user activity, so the machine sleeps, the runner stops renewing
# the job lease, GitHub invalidates it, and the job reports as a failure with NO
# failing step. Re-exec (rather than wrapping just the build) so every phase is
# covered, and via exec so the tree stays flat — no background keep-alive to
# leak. caffeinate propagates the child's status, preserving fail-loud.
# Both guards matter: ROAMUX_CAFFEINATED stops infinite re-exec, and
# `command -v` lets a machine without caffeinate proceed unprotected rather
# than fail. NOTE: -i (PreventUserIdleSystemSleep) is the load-bearing flag —
# -s is documented as effective only on AC, which is not the failing case.
if [ -z "${ROAMUX_CAFFEINATED:-}" ] && command -v caffeinate >/dev/null 2>&1; then
  export ROAMUX_CAFFEINATED=1
  exec caffeinate -sim "$0" "$@"
fi

set -euo pipefail

# roam-258: refuse a battery start BEFORE any side effect — before the env-contract checks, the
# overlay symlink flip, the pristine reconcile, the patch runhook, the suites and the build. Path is
# derived from THIS script's location, not the CWD: the workflows invoke us as
# `bash roamux/build/ci/tier2_job.sh` from ${GITHUB_WORKSPACE} and we later `cd "${SRC}"`.
bash "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/require_ac_power.sh"

SRC="${ROAMUX_CHROMIUM_SRC:-${HOME}/chromium/src}"
OUT="${ROAMUX_CI_OUT:-out/CI}"
DEPOT="${ROAMUX_DEPOT_TOOLS:-${HOME}/depot_tools}"
ROAMUX_CANONICAL_OVERLAY="${ROAMUX_CANONICAL_OVERLAY:?set in the runner .env — the operator overlay the base symlink is restored to}"

export PATH="${DEPOT}:${PATH}"
SECONDS=0
# roam-283 (grill H17): per-suite launcher summaries (--test-launcher-summary-output) and tee'd
# logs go HERE — never under the base. CI publishes ROAMUX_CI_ARTIFACTS from RUNNER_TEMP
# (ci.yml / nightly.yml, then uploads it if: always()); locally and in the hermetic harness it
# defaults under the temp dir. Created right before the first suite runs.
ART="${ROAMUX_CI_ARTIFACTS:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/tier2-artifacts}"
# Cumulative phase-start checkpoints (roam-283): the job log shows them live; the step summary
# publishes when the step ends, so a 12h timeout is attributable to the phase that was running.
phase() { echo "phase=$1 elapsed=${SECONDS}s" | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"; }

# roam-280 (grill M9): reconcile FIRST — recovery must never depend on the previous job's EXIT
# trap having run (it does not survive a SIGKILL: a cancel past GitHub's grace window, runner
# death, the battery idle-sleep). A dangling or stale link is fine — the flip below re-points
# it. A REAL directory at the link path is not: `ln -sfn` would create <dir>/roamux inside it
# and report success, and the build would silently read the wrong tree. Refuse before any base
# mutation, and before the trap is installed, so a refused start emits no restore warning.
if [ -e "${SRC}/roamux" ] && [ ! -L "${SRC}/roamux" ]; then
  echo "::error::${SRC}/roamux is a real directory, not the overlay symlink — refusing to flip into it (linking would create ${SRC}/roamux/roamux and report success); repair the base checkout by hand"
  exit 1
fi

# roam-280 (grill M9): the EXIT trap's exit-status contract. Under `set -e` an UNGUARDED failing
# command inside a trap terminates the trap and REPLACES the job's status (reproduced on
# /bin/bash 3.2: a restore failing with 7 turned incoming 3 and 0 into 7, message never printed);
# a restore that "succeeds" inside a real directory preserves a green status while the base is
# broken. So: capture the status FIRST; every command below is guarded (an `if` condition, a
# `|| true`, or `exit`), so errexit can never cut the trap short; never link into a directory;
# verify with readlink; a real failure stays the reported failure; a green job whose base could
# not be restored goes red. The function keeps this exact column-0 shape — test_tier2_job.py
# extracts it and runs it with stdout CLOSED.
restore_overlay() {
  local rc=$?
  local prev
  prev="$(readlink "${SRC}/roamux" 2>/dev/null || true)"
  prev="${prev:-<none>}"
  if [ -e "${SRC}/roamux" ] && [ ! -L "${SRC}/roamux" ]; then
    echo "::warning::overlay restore misfire: ${SRC}/roamux is a real directory (previous link target: ${prev}; this job had linked it to ${FLIPPED_TO:-<never flipped>}) — not linking into it; repair the base checkout by hand" || true
    if [ "${rc}" -ne 0 ]; then
      exit "${rc}"
    fi
    echo "::error::the job was green but the overlay symlink could not be restored (${SRC}/roamux is a real directory)" || true
    exit 1
  fi
  if ln -sfn "${ROAMUX_CANONICAL_OVERLAY}" "${SRC}/roamux" && [ "$(readlink "${SRC}/roamux" 2>/dev/null || true)" = "${ROAMUX_CANONICAL_OVERLAY}" ]; then
    echo "overlay symlink restored to ${ROAMUX_CANONICAL_OVERLAY} (was ${prev})" || true
    exit "${rc}"
  fi
  local now
  now="$(readlink "${SRC}/roamux" 2>/dev/null || true)"
  echo "::warning::overlay symlink restore FAILED: ${SRC}/roamux -> ${now:-<none>} (expected ${ROAMUX_CANONICAL_OVERLAY}; previous ${prev})" || true
  if [ "${rc}" -ne 0 ]; then
    exit "${rc}"
  fi
  echo "::error::the job was green but the overlay symlink could not be restored" || true
  exit 1
}
trap restore_overlay EXIT

echo "== tier-2 warm-base job: base=${SRC} out=${OUT} workspace=${GITHUB_WORKSPACE} =="

# Declared channel 1: point the base's overlay at THIS job's checkout. FLIPPED_TO lets the trap
# name what this job linked to when it finds the link gone (roam-280).
FLIPPED_TO="${GITHUB_WORKSPACE}/roamux"
ln -sfn "${GITHUB_WORKSPACE}/roamux" "${SRC}/roamux"

# Channel 2 precondition (roam-175, roam-160 postmortem): reconcile the base's tracked
# state to pristine. The runhook's stack simulator matches the tree only against
# prefixes of THIS checkout's stack — after a patch-rewriting/deleting PR the base
# still carries the PREVIOUS stack, which matches no prefix and fails the job in
# seconds. reset --hard, not `checkout -- .` (that restores from a possibly-staged
# index); clean drops files a superseded stack ADDED, sparing the overlay symlink
# (-e /roamux, untracked by design) and all ignored paths (no -x: out/CI and the
# warm caches live there); single -f never descends into nested git repos (the
# DEPS-managed submodules). Consequence, documented in docs/ci/self-hosted-runner.md:
# the base's tracked state is CI-owned — uncommitted local edits do not survive a run.
phase reconcile
echo "reconciling base to pristine (drops any superseded stack state)"
git -C "${SRC}" reset --hard HEAD
git -C "${SRC}" clean -fd -e /roamux

# Declared channel 2: the runhook (idempotent; fails loudly on conflict — the rebase signal).
phase runhook
python3 "${GITHUB_WORKSPACE}/roamux/build/apply_patches.py" --chromium-src "${SRC}"

# roam-147: vendor Sparkle into this job's overlay before building. out/Default carries
# roamux_enable_sparkle=true, and since roam-140 the tier-2 targets (roamux_browsertests)
# link the Sparkle-backed updater — so the framework must be present at
# roamux/third_party/sparkle (gitignored; absent in a fresh CI checkout). Mirrors the
# release pipeline's "Vendor Sparkle" step; idempotent (a no-op once vendored, SHA-pinned).
phase sparkle
python3 "${GITHUB_WORKSPACE}/roamux/build/fetch_sparkle.py"

# roam-132: the rebrand-channel's XTB-binding tests are GRIT-dependent, so tier-1 CI (no
# checkout) SKIPS them — yet that is where the load-bearing "translation still binds after
# re-key" assertions live. This runner HAS the checkout, so run them fail-not-skip
# (REQUIRE_GRIT=1 turns a skip into a failure). Hermetic (tmp fixtures) — runs before the
# hours-long build so a regression fails fast. Uses ${SRC} only to import GRIT read-only.
phase rebrand-gate
( cd "${GITHUB_WORKSPACE}" && REQUIRE_GRIT=1 ROAMUX_CHROMIUM_SRC="${SRC}" \
    python3 -m unittest roamux.build.tests.test_rebrand_strings )

# roam-97: the signed-release parts-path + config-seam tests exercise Chromium's
# real signing package (chrome/installer/mac/signing). Tier-1 CI (no checkout)
# SKIPS them, but that is where the load-bearing "Chromium get_parts() paths
# resolve against the post-rename bundle" and "the Roamux config actually reaches
# the pipeline" assertions live. This runner HAS the checkout, so run them
# fail-not-skip (REQUIRE_SIGNING_PARTS=1 turns a skip into a failure). Hermetic
# (tmp fixtures; no real codesign/notarize) — imports ${SRC} read-only.
#
# roam-261: this run is also one of the two homes of the disk-image mount case
# (test_dmg_preserves_symlinks_and_exec_bits). It is opt-in per tier now — it
# was 35-45% of the hermetic push gate's wall clock and is the least hermetic
# thing in that suite — so REQUIRE_DMG_MOUNT=1 is what keeps tier-2 exercising
# the shipped Roamux.dmg's symlink/exec-bit preservation. Fail-not-skip: with
# the flag set, a missing hdiutil FAILS rather than skipping. Asserted by
# test_tier2_job.py; the rationale lives above dmg_mount_decision.
phase signing-gate
( cd "${GITHUB_WORKSPACE}" && REQUIRE_SIGNING_PARTS=1 REQUIRE_DMG_MOUNT=1 \
    ROAMUX_CHROMIUM_SRC="${SRC}" \
    python3 -m unittest roamux.build.tests.test_release_signing )

# Warm CI build dir: APFS-clone the operator's warm out/Default on first use (copy-on-write).
# roam-280 (grill M13): restart-safe. A clone killed mid-copy (cancel, timeout, sleep) used to
# leave a partial ${OUT} that the `-d` guard treated as complete. Now: "ready" means
# ${OUT}/build.ninja AND ${OUT}/args.gn exist; a not-ready ${OUT} is discarded and redone ONLY
# when OUT is literally the CI-owned default out/CI (docs/ci/self-hosted-runner.md) — an
# overridden ROAMUX_CI_OUT in that state is refused, never deleted; the copy goes to
# ${OUT}.partial (ours, always safe to discard), is checked for readiness, and is renamed into
# place atomically. out/Default is never a deletion target under any branch.
phase clone
cd "${SRC}"
if [ -d "${OUT}" ] && { [ ! -f "${OUT}/build.ninja" ] || [ ! -f "${OUT}/args.gn" ]; }; then
  if [ "${OUT}" = "out/CI" ]; then
    echo "::warning::${OUT} exists but is not a complete build dir (no build.ninja/args.gn) — a clone was interrupted; discarding it and re-cloning"
    rm -rf "${OUT}"
  else
    echo "::error::${OUT} exists but is not a complete build dir (no build.ninja/args.gn); only the default out/CI is CI-owned — refusing to delete an overridden ROAMUX_CI_OUT. Remove it yourself to re-clone."
    exit 1
  fi
fi
if [ ! -d "${OUT}" ]; then
  rm -rf "${OUT}.partial"
  clone_started=${SECONDS}
  echo "cloning warm build dir out/Default -> ${OUT}.partial (APFS copy-on-write), then publishing as ${OUT}"
  cp -Rc out/Default "${OUT}.partial"
  if [ ! -f "${OUT}.partial/build.ninja" ] || [ ! -f "${OUT}.partial/args.gn" ]; then
    echo "::error::clone of out/Default is incomplete (no build.ninja/args.gn in ${OUT}.partial) — not publishing it as ${OUT}"
    exit 1
  fi
  mv "${OUT}.partial" "${OUT}"
  echo "clone published as ${OUT} in $((SECONDS - clone_started))s"
fi

# roam-282 (grill H15): roamux_sparkle_tests — the only test that drives real Sparkle against the
# signed/tampered/unsigned fixtures — had existed in roamux/BUILD.gn since roam-32 without any
# workflow building or running it. It exists only under roamux_enable_sparkle=true (reference.gn
# pins that since roam-282; out/Default already carried it). The echo makes the target list visible
# in the job log, which never traces commands.
phase build
echo "tier-2 build targets: roamux_unittests roamux_browser_unittests roamux_sparkle_tests roamux_browsertests"
autoninja -C "${OUT}" roamux_unittests roamux_browser_unittests roamux_sparkle_tests roamux_browsertests

# roam-195: run every suite with an EXPLICIT retry limit. The launcher silently zeroes
# retries when a --gtest_filter is passed outside bot mode (test_launcher.cc: "not in bot
# mode and filtered by flag ... Set reties to zero"), which left the filtered
# roamux_browsertests line — and only that line — with no retries at all, while the two
# unfiltered suites kept the built-in default of 1. Every recorded occurrence of the
# TaskEnvironment "CompleteShutdown took more than 30 seconds" teardown hang (roam-195)
# crashed a test in exactly that unprotected suite, failing the whole job over a
# post-assertion shutdown stall. The explicit switch is resolved BEFORE the filter branch,
# so it restores retries; 2 absorbs a stochastic hang (bounded: a persistently hanging test
# still fails after 3 attempts, ~30s each) without masking a deterministic failure — a
# retried test is reported as such in the launcher summary. This is mitigation, not a cure:
# roam-195 stays open for the underlying teardown stall. roam-282 removed the last filter; the
# explicit limit stays on EVERY line so a future filter cannot silently disarm retries again.
RETRY_LIMIT="${ROAMUX_CI_RETRY_LIMIT:-2}"
# Each suite runs inside a named log group, so the job log shows which binary produced which
# launcher output (roam-282: a suite joining or leaving this list is provable from these lines).
# roam-283: every suite writes its launcher summary JSON and a tee'd log into ${ART} (pipefail is
# on, so the suite's status survives the tee); the Flake report step reads the JSON afterwards.
mkdir -p "${ART}"
phase run:roamux_unittests
echo "::group::run ${OUT}/roamux_unittests"
"${OUT}/roamux_unittests" --test-launcher-retry-limit="${RETRY_LIMIT}" --test-launcher-summary-output="${ART}/roamux_unittests.json" 2>&1 | tee "${ART}/roamux_unittests.log"
echo "::endgroup::"
# TEMPORARY (roam-283 acceptance probe, reverted by the next commit): fail the job right after the
# first suite so the forced-fail path — Flake report + artifact upload — is observed on the builder.
if [ -n "${ROAMUX_CI_FORCED_FAIL_PROBE:-}" ]; then echo "ROAMUX-FORCED-FAIL-PROBE ${ROAMUX_CI_FORCED_FAIL_PROBE}: exiting 1 after roamux_unittests"; exit 1; fi
phase run:roamux_browser_unittests
echo "::group::run ${OUT}/roamux_browser_unittests"
"${OUT}/roamux_browser_unittests" --test-launcher-retry-limit="${RETRY_LIMIT}" --test-launcher-summary-output="${ART}/roamux_browser_unittests.json" 2>&1 | tee "${ART}/roamux_browser_unittests.log"
echo "::endgroup::"
phase run:roamux_sparkle_tests
echo "::group::run ${OUT}/roamux_sparkle_tests"
"${OUT}/roamux_sparkle_tests" --test-launcher-retry-limit="${RETRY_LIMIT}" --test-launcher-summary-output="${ART}/roamux_sparkle_tests.json" 2>&1 | tee "${ART}/roamux_sparkle_tests.log"
echo "::endgroup::"
# roam-282 (grill H14): UNFILTERED. The roam-6 --gtest_filter="Roamux*" that once kept tier-2
# wall-clock sane when E1 was the only suite silently dropped the one fixture not named Roamux*
# (ThreeCarrierTest — the only three-carrier import-survival proof) for months. The target holds
# only overlay suites, so a filter buys nothing and fixture names are free (see roamux/BUILD.gn);
# roamux/build/tests/test_browsertest_fixtures.py proves every case reachable and
# test_tier2_job.py pins that no run line carries a filter.
phase run:roamux_browsertests
echo "::group::run ${OUT}/roamux_browsertests"
"${OUT}/roamux_browsertests" --test-launcher-retry-limit="${RETRY_LIMIT}" --test-launcher-summary-output="${ART}/roamux_browsertests.json" 2>&1 | tee "${ART}/roamux_browsertests.log"
echo "::endgroup::"

# Staleness gate against this job's overlay.
phase staleness
python3 "${GITHUB_WORKSPACE}/roamux/build/check_override_staleness.py" \
  --chromium-src "${SRC}" --overlay "${GITHUB_WORKSPACE}/roamux"

phase done
echo "tier-2 job green in ${SECONDS}s (warm incremental)" | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
