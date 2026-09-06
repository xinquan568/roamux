# SPDX-License-Identifier: Apache-2.0
"""Hermetic checks on the tier-2 job script (roam-36) — the declared-channel discipline.

v1 acceptance (per the frozen analysis): the job may touch the shared warm base ONLY through the two
declared, restored channels — the overlay symlink and the patch runhook. This is structural
enforcement (discipline-plus-tests), explicitly not kernel/JIT isolation; see docs/ci/self-hosted-runner.md.
"""

import os
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest

CI = pathlib.Path(__file__).resolve().parent.parent / "ci"
SCRIPT = CI / "tier2_job.sh"
POWER_GATE = CI / "require_ac_power.sh"
REPO = pathlib.Path(__file__).resolve().parents[3]

# Real `pmset -g batt` output shapes, captured from the builder.
PMSET_AC = ("Now drawing from 'AC Power'\n"
            " -InternalBattery-0 (id=24379491)\t80%; AC attached; not charging present: true\n")
PMSET_BATTERY = ("Now drawing from 'Battery Power'\n"
                 " -InternalBattery-0 (id=24379491)\t80%; discharging; 4:12 remaining present: true\n")


def _fake_bin(dirpath, name, body):
    p = pathlib.Path(dirpath) / name
    p.write_text("#!/bin/bash\n" + body)
    p.chmod(p.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return p


def _fake_pmset(dirpath, output, rc=0):
    """A pmset that VALIDATES it was called as `-g batt` — so a gate probing
    something else cannot quietly pass — and then emits the given shape."""
    return _fake_bin(dirpath, "pmset", f"""
if [ "$#" -ne 2 ] || [ "$1" != "-g" ] || [ "$2" != "batt" ]; then
  echo "fake pmset: expected exactly `-g batt`, got: $*" >&2
  exit 99
fi
cat <<'EOF'
{output}EOF
exit {rc}
""")


class Tier2JobScriptTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.exists(), f"missing {SCRIPT}")
        self.text = SCRIPT.read_text()
        self.code = "\n".join(l for l in self.text.splitlines() if not l.strip().startswith("#"))

    def test_strict_mode(self):
        self.assertIn("set -euo pipefail", self.text)

    def test_exit_trap_restores_canonical_overlay(self):
        # The symlink flip (channel 1) must be undone no matter how the job exits.
        self.assertIn("trap", self.code)
        trap_line = next(l for l in self.code.splitlines() if l.strip().startswith("trap"))
        self.assertIn("EXIT", trap_line)
        self.assertIn("restore_overlay", trap_line)
        self.assertIn('ln -sfn "${ROAMUX_CANONICAL_OVERLAY}"', self.code)

    def test_declared_channels_present(self):
        self.assertIn('ln -sfn "${GITHUB_WORKSPACE}/roamux"', self.code)  # channel 1: symlink flip
        self.assertIn("apply_patches.py", self.code)                       # channel 2: the runhook

    def test_base_reconciled_to_pristine_before_runhook(self):
        # roam-175 (roam-160 postmortem): the runhook's stack simulator matches the
        # tree only against prefixes of THIS checkout's stack, so after a
        # patch-rewriting/deleting PR the base still carries the PREVIOUS stack —
        # which matches no prefix and fails the job in seconds. The job must
        # reconcile the base's tracked state to pristine first: reset --hard (NOT
        # `checkout -- .`, which restores from a possibly-staged index), then a
        # clean that drops files a superseded stack ADDED while sparing the overlay
        # symlink (-e /roamux — untracked by design) and every ignored path (no -x:
        # out/CI and the warm caches live there). Single -f only: clean must never
        # descend into nested git repos (Chromium's DEPS-managed submodules).
        lines = self.code.splitlines()
        reset = next((i for i, l in enumerate(lines) if "reset --hard HEAD" in l),
                     None)
        clean = next((i for i, l in enumerate(lines) if "clean -fd -e /roamux" in l),
                     None)
        runhook = next(i for i, l in enumerate(lines) if "apply_patches.py" in l)
        self.assertIsNotNone(reset, "no `reset --hard HEAD` reconcile in the job")
        self.assertIsNotNone(clean, "no `clean -fd -e /roamux` reconcile in the job")
        self.assertLess(reset, runhook, "reconcile must precede the runhook")
        self.assertLess(clean, runhook, "reconcile must precede the runhook")
        self.assertNotIn("-x", lines[clean], "clean -x would nuke out/CI")
        self.assertNotIn("-ff", lines[clean], "clean -ff would enter submodules")

    def test_staleness_gate_runs(self):
        self.assertIn("check_override_staleness.py", self.code)

    def test_all_four_suites_build_and_run(self):
        # roam-6 (WB-CI): the browser-test suite joined the tier-2 gate; roam-282 (grill H15):
        # roamux_sparkle_tests joined it — it had existed in BUILD.gn for months without any
        # workflow building or running it. A regression to a shorter line must fail here, not
        # silently in CI.
        self.assertIn(
            "roamux_unittests roamux_browser_unittests roamux_sparkle_tests roamux_browsertests",
            self.code)
        for binary in ("roamux_unittests", "roamux_browser_unittests",
                       "roamux_sparkle_tests", "roamux_browsertests"):
            self.assertIn('"${OUT}/%s"' % binary, self.code)

    def test_no_suite_runs_under_a_gtest_filter(self):
        # roam-282 (grill H14): the Roamux* filter on roamux_browsertests silently dropped the one
        # fixture not named Roamux* (ThreeCarrierTest) for months. The overlay targets contain
        # only overlay suites, so no filter is ever needed; roamux/build/tests/
        # test_browsertest_fixtures.py proves every case reachable, and this pin keeps the script
        # honest at the source: no run line may carry --gtest_filter.
        run_lines = [l for l in self.code.splitlines() if '"${OUT}/roamux' in l]
        for line in run_lines:
            self.assertNotIn("--gtest_filter", line, f"filtered suite run: {line.strip()}")

    def test_every_suite_run_sets_an_explicit_retry_limit(self):
        # roam-195: the launcher ZEROES its retry limit when a --gtest_filter is passed
        # outside bot mode (base/test/launcher/test_launcher.cc: "not in bot mode and
        # filtered by flag ... Set reties to zero"), so the filtered roamux_browsertests
        # line silently ran with NO retries while the two unfiltered suites kept the
        # default of 1. Every recorded teardown-timeout flake landed in exactly that
        # unprotected suite. The explicit flag is resolved BEFORE the filter branch, so
        # passing it restores retries; assert it on ALL THREE invocations so a later
        # --gtest_filter added to another suite cannot silently disarm them again (roam-282
        # removed the last filter; the explicit limit stays on every line for exactly this reason).
        run_lines = [l for l in self.code.splitlines() if '"${OUT}/roamux' in l]
        self.assertEqual(4, len(run_lines), f"expected 4 suite runs, got {run_lines}")
        for line in run_lines:
            self.assertIn("--test-launcher-retry-limit=", line,
                          f"suite run without an explicit retry limit: {line.strip()}")

    def test_every_suite_writes_a_summary_and_a_log_into_the_artifact_dir(self):
        # roam-283 (grill H17): the launcher's summary JSON is the only record of WHICH tests
        # needed a retry; the tee'd log is the human-readable twin. Both go to ${ART}, never
        # under the base. Pinned per run line so a fifth suite cannot join without them.
        run_lines = [l for l in self.code.splitlines() if '"${OUT}/roamux' in l]
        self.assertEqual(4, len(run_lines))
        for line in run_lines:
            suite = line.split('"${OUT}/')[1].split('"')[0]
            self.assertIn(f'--test-launcher-summary-output="${{ART}}/{suite}.json"', line, line.strip())
            self.assertIn(f'2>&1 | tee "${{ART}}/{suite}.log"', line, line.strip())

    def test_artifact_dir_is_defined_with_a_local_default_and_created_before_the_first_suite(self):
        self.assertIn('ART="${ROAMUX_CI_ARTIFACTS:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/tier2-artifacts}"',
                      self.code)
        mk = self.code.index('mkdir -p "${ART}"')
        first_run = self.code.index('"${OUT}/roamux_unittests"')
        self.assertLess(mk, first_run, "the artifact dir must exist before the first tee")

    PHASES = ("reconcile", "runhook", "sparkle", "rebrand-gate", "signing-gate", "clone", "build",
              "run:roamux_unittests", "run:roamux_browser_unittests", "run:roamux_sparkle_tests",
              "run:roamux_browsertests", "staleness", "done")

    def test_phase_checkpoints_exist_in_order(self):
        # roam-283: cumulative phase-start checkpoints into the step summary, so a 12h timeout is
        # attributable to the phase that was running.
        self.assertIn('phase() { echo "phase=$1 elapsed=${SECONDS}s" | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"; }',
                      self.code)
        positions = []
        for name in self.PHASES:
            needle = f"phase {name}\n"
            self.assertIn(needle, self.code, f"missing checkpoint: phase {name}")
            positions.append(self.code.index(needle))
        self.assertEqual(positions, sorted(positions), "checkpoints must appear in phase order")

    def test_release_signing_run_opts_into_the_dmg_mount(self):
        # roam-261: the disk-image mount is opt-in per tier (see
        # dmg_mount_decision in test_release_signing.py). Tier-2 is one of its
        # only two homes now that the hermetic push path skips it, and it lives
        # here only because this line opts in — assert the opt-in explicitly so
        # it cannot be dropped by a later edit to the invocation. Without it the
        # case would silently run NOWHERE on this tier, which is the specific
        # regression roam-261's acceptance guards against.
        # Join backslash continuations so the whole invocation is ONE logical
        # line — the env prefix and the module name are on separate physical
        # lines, and asserting them apart would pass on an unrelated pairing.
        joined = self.code.replace("\\\n", " ")
        runs = [l for l in joined.splitlines()
                if "unittest roamux.build.tests.test_release_signing" in l]
        self.assertEqual(1, len(runs),
                         f"expected exactly 1 test_release_signing run, got {runs}")
        self.assertIn("REQUIRE_DMG_MOUNT=1", runs[0],
                      "the test_release_signing run must set "
                      "REQUIRE_DMG_MOUNT=1 (roam-261) or tier-2 silently "
                      "stops exercising the disk-image mount")
        self.assertIn("REQUIRE_SIGNING_PARTS=1", runs[0],
                      "roam-97's checkout-backed gate must survive alongside it")

    def test_no_sudo_no_secret_use(self):
        self.assertNotIn("sudo", self.code)
        self.assertNotIn("secrets.", self.text)

    def test_wall_time_recorded(self):
        self.assertIn("GITHUB_STEP_SUMMARY", self.code)
        self.assertIn("SECONDS", self.code)

    def test_base_writes_only_via_declared_channels(self):
        # Every line that references the base checkout var must be one of the declared channels,
        # a read-only use, or the build-dir path (out/CI lives under the base by design).
        # "reset --hard" / "clean -fd" (roam-175): the channel-2 reconcile precondition —
        # exact-command markers, so no other git mutation of the base sneaks past.
        # "readlink" (roam-280): a READ-ONLY probe — restore_overlay records the previous link
        # target before it decides whether it may re-link. It never mutates the base.
        allowed_markers = ("ln -sfn", "apply_patches.py", "check_override_staleness.py", "--chromium-src",
                           "autoninja", "gn ", "cd ", "cp ", "OUT=", "SRC=", "echo", "test ", "[ ",
                           "reset --hard HEAD", "clean -fd -e /roamux", "readlink")
        for line in self.code.splitlines():
            if "${SRC}" in line or "$SRC" in line:
                self.assertTrue(any(m in line for m in allowed_markers),
                                f"undeclared base access: {line.strip()}")

    def test_rebrand_binding_gate_runs_fail_not_skip(self):
        # roam-132 review: the GRIT-bound rebrand binding tests SKIP on tier-1 (no
        # checkout) — that is where the load-bearing "translation still binds after
        # re-key" assertions live. Tier-2 HAS the checkout, so it must run them
        # fail-not-skip (REQUIRE_GRIT=1) against the base's GRIT, from the overlay
        # root so the dotted test module resolves.
        self.assertIn("REQUIRE_GRIT=1", self.code)
        self.assertIn("test_rebrand_strings", self.code)
        self.assertIn('ROAMUX_CHROMIUM_SRC="${SRC}"', self.code)

    def test_signing_parts_gate_runs_fail_not_skip(self):
        # roam-97: the signed-release parts-path + config-seam tests exercise
        # Chromium's real signing package and SKIP on tier-1 (no checkout). Tier-2
        # HAS the checkout, so it must run them fail-not-skip
        # (REQUIRE_SIGNING_PARTS=1) against the base's signing package, from the
        # overlay root so the dotted test module resolves.
        self.assertIn("REQUIRE_SIGNING_PARTS=1", self.code)
        self.assertIn("test_release_signing", self.code)

    # --- roam-280 (grill M9 / M13): structural pins; the behaviour is proven in
    # Tier2JobBehaviourTest below. ---

    def test_flip_is_guarded_against_a_real_directory(self):
        # Recovery must never depend on the PREVIOUS job's EXIT trap having run: if the link
        # path is a real directory, `ln -sfn` would create <dir>/roamux and report success.
        guard = self.code.find('[ ! -L "${SRC}/roamux" ]')
        flip = self.code.find('ln -sfn "${GITHUB_WORKSPACE}/roamux"')
        trap = self.code.find("trap restore_overlay EXIT")
        self.assertGreater(guard, 0, "no real-directory guard before the flip (roam-280 / M9)")
        self.assertLess(guard, flip, "the guard must run BEFORE the flip")
        self.assertLess(guard, trap, "the guard must run BEFORE the trap is installed — a refused "
                                     "start must not also emit a spurious restore warning")

    def test_trap_has_an_exit_status_contract(self):
        # Under `set -e` an unguarded failing command INSIDE the trap terminates the trap and
        # replaces the job's status (reproduced on /bin/bash 3.2); a restore that "succeeds"
        # inside a real directory preserves a green status while the base is broken. The
        # function must therefore capture the status first, guard every command, refuse to
        # link into a directory, verify with readlink, and exit explicitly.
        fn = _restore_overlay_function(self.text)
        self.assertIsNotNone(fn, "restore_overlay() { ... } must keep its column-0 shape "
                                 "(the behavioural tests extract it)")
        body = [l for l in fn.splitlines()[1:-1] if l.strip() and not l.strip().startswith("#")]
        self.assertRegex(body[0], r"^\s*local rc=\$\?\s*$",
                         "the trap must capture the job's exit status FIRST")
        probe = fn.find('-L "${SRC}/roamux"')
        link = fn.find('ln -sfn "${ROAMUX_CANONICAL_OVERLAY}"')
        self.assertGreater(probe, 0, "the trap must test whether the path is a symlink")
        self.assertLess(probe, link, "the -L test must come BEFORE the re-link")
        self.assertLess(link, fn.rfind("readlink"), "the re-link must be verified with readlink")
        self.assertIn('exit "${rc}"', fn, "a real failure must stay the reported failure")
        self.assertIn("::warning::", fn)
        self.assertIn("::error::", fn, "a green job whose base could not be restored must go red")
        for line in body:
            if line.strip().startswith("echo"):
                self.assertTrue(line.rstrip().endswith("|| true"),
                                f"unguarded diagnostic inside the trap: {line.strip()}")

    def test_clone_is_staged_and_atomic(self):
        # roam-280 (M13): a clone killed mid-copy must never masquerade as a warm out/CI.
        c = self.code
        self.assertNotIn('cp -Rc out/Default "${OUT}"', c, "the clone must not target ${OUT} directly")
        stage = c.find('cp -Rc out/Default "${OUT}.partial"')
        self.assertGreater(stage, 0, "the clone must be staged in ${OUT}.partial")
        ready_ninja = c.find('"${OUT}.partial/build.ninja"')
        ready_args = c.find('"${OUT}.partial/args.gn"')
        publish = c.find('mv "${OUT}.partial" "${OUT}"')
        self.assertGreater(ready_ninja, stage, "readiness (build.ninja) must be checked after the copy")
        self.assertGreater(ready_args, stage, "readiness (args.gn) must be checked after the copy")
        self.assertGreater(publish, max(ready_ninja, ready_args), "publish (mv) only after readiness")

    def test_destructive_recovery_is_limited_to_the_default_ci_dir(self):
        # roam-280: ROAMUX_CI_OUT is an unrestricted override and only the default out/CI is
        # CI-owned (docs/ci/self-hosted-runner.md). A not-ready ${OUT} may be discarded ONLY
        # when it is literally out/CI; anything else is refused, never deleted.
        lines = self.code.splitlines()
        rms = [i for i, l in enumerate(lines) if l.strip() == 'rm -rf "${OUT}"']
        self.assertEqual(len(rms), 1, f"expected exactly one `rm -rf \"${{OUT}}\"`, got {rms}")
        gate = next((i for i, l in enumerate(lines) if '[ "${OUT}" = "out/CI" ]' in l), None)
        self.assertIsNotNone(gate, "destructive recovery must be gated on OUT being the default out/CI")
        self.assertLess(gate, rms[0], "the out/CI gate must precede the deletion")



class Tier2PowerGateTest(unittest.TestCase):
    """roam-258: the builder idle-sleeps after ONE minute on battery (pmset -b
    sleep 1), which kills long jobs with no failing step. These are BEHAVIOURAL
    tests of the pre-flight — a fake pmset on PATH, never the real host."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-power-"))
        self.addCleanup(__import__("shutil").rmtree, self.tmp, ignore_errors=True)
        self.assertTrue(POWER_GATE.exists(), f"missing {POWER_GATE}")

    def _run(self, pmset_output, rc=0, env=None):
        bindir = self.tmp / "bin"
        bindir.mkdir(exist_ok=True)
        _fake_pmset(bindir, pmset_output, rc)
        e = dict(os.environ)
        e["PATH"] = f"{bindir}:{e['PATH']}"
        e.pop("ROAMUX_ALLOW_BATTERY", None)
        e.update(env or {})
        return subprocess.run(["bash", str(POWER_GATE)], capture_output=True,
                              text=True, env=e, timeout=30)

    def test_ac_power_passes(self):
        r = self._run(PMSET_AC)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_battery_fails_fast_with_a_clear_message(self):
        r = self._run(PMSET_BATTERY)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("battery", (r.stdout + r.stderr).lower())

    def test_battery_message_carries_the_triage_discriminator(self):
        # The log where the failure happens must distinguish idle-sleep from a
        # network drop, so triage does not depend on log forensics.
        r = self._run(PMSET_BATTERY)
        out = (r.stdout + r.stderr).lower()
        self.assertIn("renew", out, "must name the renewal-gap signature")

    def test_allow_battery_escape_hatch_downgrades_to_a_warning(self):
        r = self._run(PMSET_BATTERY, env={"ROAMUX_ALLOW_BATTERY": "1"})
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        out = (r.stdout + r.stderr).lower()
        self.assertIn("warning", out)
        # It waives the guarantee — that must be said, not implied by silence.
        self.assertTrue("waiv" in out or "no guarantee" in out or "may still" in out,
                        f"the override must name itself as a waiver: {out}")

    def test_emits_a_github_annotation(self):
        r = self._run(PMSET_BATTERY)
        self.assertIn("::error::", r.stdout + r.stderr)
        w = self._run(PMSET_BATTERY, env={"ROAMUX_ALLOW_BATTERY": "1"})
        self.assertIn("::warning::", w.stdout + w.stderr)

    def test_unknown_power_state_fails_closed(self):
        # Treating an unrecognised/failed probe as AC would hollow out the only
        # deterministic guarantee this fix rests on.
        for label, out, rc in (("nonzero", PMSET_AC, 3),
                               ("garbage", "wat\n", 0),
                               ("empty", "", 0)):
            with self.subTest(label):
                r = self._run(out, rc)
                self.assertNotEqual(r.returncode, 0, f"{label} must fail closed")
                self.assertIn("determine", (r.stdout + r.stderr).lower())

    def test_script_is_executable_and_spdx_headed(self):
        self.assertIn("SPDX-License-Identifier: Apache-2.0",
                      POWER_GATE.read_text().splitlines()[1])
        self.assertTrue(os.access(POWER_GATE, os.X_OK), "must be executable")


class Tier2CaffeinateTest(unittest.TestCase):
    """roam-258: the job must hold a power assertion for its whole body."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-caf-"))
        self.addCleanup(__import__("shutil").rmtree, self.tmp, ignore_errors=True)
        self.text = SCRIPT.read_text()
        self.code = "\n".join(l for l in self.text.splitlines()
                              if not l.strip().startswith("#"))

    def test_tier2_job_reexecs_under_caffeinate_once(self):
        # BEHAVIOURAL: drive the real entry point the way the workflows do
        # (`bash roamux/build/ci/tier2_job.sh`, CWD = repo root). Fake pmset
        # reports battery so the run stops at the gate and never builds.
        bindir = self.tmp / "bin"; bindir.mkdir()
        count = self.tmp / "caffeinate.count"
        argv = self.tmp / "caffeinate.argv"
        _fake_pmset(bindir, PMSET_BATTERY)
        # Fails FAST on re-entry: without this, a removed recursion guard would
        # exec forever and this test would HANG instead of failing.
        _fake_bin(bindir, "caffeinate", f"""
echo x >> "{count}"
n=$(wc -l < "{count}" | tr -d ' ')
if [ "$n" -gt 1 ]; then
  echo "fake caffeinate: re-entered ($n) — recursion guard is broken" >&2
  exit 97
fi
echo "$@" > "{argv}"
# Real caffeinate consumes its own flags, then execs the command. Without this,
# bash's `exec` builtin would try to interpret -sim as ITS options.
while [ $# -gt 0 ]; do
  case "$1" in
    -*) shift ;;
    *) break ;;
  esac
done
exec "$@"
""")
        e = dict(os.environ)
        e["PATH"] = f"{bindir}:{e['PATH']}"
        e["ROAMUX_CANONICAL_OVERLAY"] = str(self.tmp / "overlay")
        e["GITHUB_WORKSPACE"] = str(self.tmp / "ws")
        e.pop("ROAMUX_CAFFEINATED", None)
        e.pop("ROAMUX_ALLOW_BATTERY", None)
        r = subprocess.run(["bash", "roamux/build/ci/tier2_job.sh"],
                           cwd=REPO, capture_output=True, text=True, env=e,
                           timeout=120)
        n = count.read_text().count("x") if count.exists() else 0
        self.assertEqual(n, 1, f"expected exactly one caffeinate invocation, got {n}")
        # Pin the exact argv, not merely "some flag containing an i":
        # -s/-i/-m are system/idle/disk sleep, and -i is load-bearing for the
        # battery case. Also pin the command, so the re-exec target cannot drift.
        # Pinned EXACTLY — argv length included, so neither an extra argument
        # nor a drifted target can slip through.
        self.assertEqual(argv.read_text().split(),
                         ["-sim", "roamux/build/ci/tier2_job.sh"])
        out = r.stdout + r.stderr
        self.assertIn("battery", out.lower(), out)
        self.assertNotEqual(r.returncode, 97, "re-entered: recursion guard broken")
        # The GATE's status (1), not merely "some failure" — proves the exact
        # child status survives the caffeinate re-exec rather than being
        # replaced by a shell or signal code.
        self.assertEqual(r.returncode, 1,
                         "the gate's exit status must propagate through caffeinate")

    def test_tier2_caffeinate_guards_are_present(self):
        self.assertIn("ROAMUX_CAFFEINATED", self.code, "recursion guard")
        self.assertIn("command -v caffeinate", self.code,
                      "a machine without caffeinate must proceed, not fail")

    def test_tier2_power_gate_precedes_every_side_effect(self):
        gate = self.code.find("require_ac_power.sh")
        self.assertGreater(gate, 0, "the power gate must be invoked")
        for marker in ("ln -sfn", "git -C", "apply_patches.py", "autoninja",
                       "python3 -m unittest"):
            at = self.code.find(marker)
            if at > 0:
                self.assertLess(gate, at,
                                f"the power gate must precede `{marker}`")


# ---------------------------------------------------------------------------------------------
# roam-280 (grill M33): BEHAVIOURAL coverage of the whole job. The real script runs against a fake
# builder — PATH fakes for pmset / caffeinate / git / python3 / autoninja / cp / readlink, a temp base
# checkout with a stale overlay link, a fake warm out/Default with suite stubs — invoked exactly as
# the workflows do (`bash roamux/build/ci/tier2_job.sh`, CWD = repo root). Every fake appends one
# line to a single events.log so cross-command ORDER is provable, not inferred.
#
# The fake EXECUTABLES are created once per test class and driven per test through environment
# variables and sourced body files: endpoint-security agents stall the first exec of every new
# binary (measured here: 4.2 s for a fresh set of fakes, 0.27 s for the same set again), so per-test
# executables would cost ~45 s per run of this class on a managed Mac. The suite stubs in the fake
# out/Default are SYMLINKS to one static stub — `cp -R` copies symlinks as symlinks, so the clone
# in out/CI executes the same file too.


def _write_exec(path, body):
    path = pathlib.Path(path)
    path.write_text("#!/bin/bash\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return path


def _restore_overlay_function(text):
    """The restore_overlay function text — from `restore_overlay() {` to the first column-0 `}` —
    or None. Used to pin its shape and to run it in isolation (closed stdout)."""
    out, capturing = [], False
    for line in text.splitlines():
        if line.startswith("restore_overlay() {"):
            capturing = True
        if capturing:
            out.append(line)
            if line == "}":
                return "\n".join(out) + "\n"
    return None


# In the script's run order — B2 asserts the suites ran in exactly this sequence (roam-282 added
# roamux_sparkle_tests between the unit suites and the browser tests).
SUITES = ("roamux_unittests", "roamux_browser_unittests", "roamux_sparkle_tests",
          "roamux_browsertests")


def _make_fake_bin(bindir):
    """The static fakes. Behaviour per test comes from ROAMUX_FAKE_* variables:
    ROAMUX_FAKE_EVENTS (log), ROAMUX_FAKE_AUTONINJA (body file sourced by autoninja),
    ROAMUX_FAKE_SUITE_DIR (per-suite body files, sourced by the suite stub if present),
    ROAMUX_FAKE_CP / ROAMUX_FAKE_READLINK (body files; unset -> the real tool)."""
    bindir = pathlib.Path(bindir)
    bindir.mkdir(exist_ok=True)
    _fake_pmset(bindir, PMSET_AC)
    _write_exec(bindir / "caffeinate",
                'while [ $# -gt 0 ]; do case "$1" in -*) shift ;; *) break ;; esac; done\n'
                'exec "$@"\n')
    for tool in ("git", "python3"):
        _write_exec(bindir / tool, f'echo "{tool} $*" >> "${{ROAMUX_FAKE_EVENTS:?}}"\nexit 0\n')
    _write_exec(bindir / "autoninja",
                'echo "autoninja $*" >> "${ROAMUX_FAKE_EVENTS:?}"\n'
                'if [ -n "${ROAMUX_FAKE_AUTONINJA:-}" ]; then . "${ROAMUX_FAKE_AUTONINJA}"; fi\nexit 0\n')
    # roam-283: when (and only when) the launcher flag is present, the stub writes a minimal valid
    # summary BEFORE sourcing the per-suite body, so a failing body cannot bypass the write (a body
    # may overwrite the file). Event line and exit status are unchanged from before roam-283.
    _write_exec(bindir / "suite_stub",
                'name="$(basename "$0")"\necho "$name $*" >> "${ROAMUX_FAKE_EVENTS:?}"\n'
                'for a in "$@"; do case "$a" in --test-launcher-summary-output=*)\n'
                '  printf \'%s\' \'{"all_tests":[],"disabled_tests":[],"global_tags":[],"per_iteration_data":[{}],"test_locations":{}}\' > "${a#--test-launcher-summary-output=}" ;; esac; done\n'
                'if [ -n "${ROAMUX_FAKE_SUITE_DIR:-}" ] && [ -f "${ROAMUX_FAKE_SUITE_DIR}/$name" ]; then\n'
                '  . "${ROAMUX_FAKE_SUITE_DIR}/$name"\nfi\nexit 0\n')
    _write_exec(bindir / "cp",
                'if [ -n "${ROAMUX_FAKE_CP:-}" ]; then . "${ROAMUX_FAKE_CP}"; fi\nexec /bin/cp "$@"\n')
    _write_exec(bindir / "readlink",
                'if [ -n "${ROAMUX_FAKE_READLINK:-}" ]; then . "${ROAMUX_FAKE_READLINK}"; fi\n'
                'exec /usr/bin/readlink "$@"\n')
    return bindir


class _Tier2Harness:
    """A fake builder for tier2_job.sh: per-test state (SRC, workspace, canonical, events.log,
    body files) on top of the class-level static fakes in `fakebin`."""

    def __init__(self, tmp, fakebin, *, autoninja="exit 0", suites=None, cp=None):
        self.tmp = pathlib.Path(tmp)
        self.bin = pathlib.Path(fakebin)
        self.bodies = self.tmp / "bodies"
        self.bodies.mkdir()
        self.src = self.tmp / "src"
        self.default = self.src / "out" / "Default"
        self.default.mkdir(parents=True)
        self.ws = self.tmp / "ws"
        (self.ws / "roamux").mkdir(parents=True)
        self.prev_ws = self.tmp / "ws-prev" / "roamux"
        self.prev_ws.mkdir(parents=True)
        self.canonical = self.tmp / "codes" / "roamux" / "roamux"
        self.canonical.mkdir(parents=True)
        self.link = self.src / "roamux"
        self.link.symlink_to(self.prev_ws)          # what a previous job left behind
        self.events = self.tmp / "events.log"
        self.summary = self.tmp / "summary.txt"
        self.artifacts = self.tmp / "artifacts"        # roam-283: per-test, never shared
        (self.default / "build.ninja").write_text("# fake ninja file\n")
        (self.default / "args.gn").write_text("is_debug = false\n")
        for name in SUITES:
            (self.default / name).symlink_to(self.bin / "suite_stub")
        (self.bodies / "autoninja").write_text(autoninja + "\n")
        suite_dir = self.bodies / "suites"
        suite_dir.mkdir()
        for name, body in (suites or {}).items():
            (suite_dir / name).write_text(body + "\n")
        self.cp_body = None
        if cp is not None:
            self.cp_body = self.bodies / "cp"
            self.cp_body.write_text(cp + "\n")

    def env(self, **overrides):
        e = {"PATH": f"{self.bin}:/usr/bin:/bin", "HOME": str(self.tmp),
             "ROAMUX_CHROMIUM_SRC": str(self.src),
             "ROAMUX_DEPOT_TOOLS": str(self.bin),     # the script prepends DEPOT to PATH
             "ROAMUX_CANONICAL_OVERLAY": str(self.canonical),
             "GITHUB_WORKSPACE": str(self.ws),
             "GITHUB_STEP_SUMMARY": str(self.summary),
             "ROAMUX_CI_ARTIFACTS": str(self.artifacts),
             "ROAMUX_FAKE_EVENTS": str(self.events),
             "ROAMUX_FAKE_AUTONINJA": str(self.bodies / "autoninja"),
             "ROAMUX_FAKE_SUITE_DIR": str(self.bodies / "suites")}
        if self.cp_body is not None:
            e["ROAMUX_FAKE_CP"] = str(self.cp_body)
        e.update(overrides)
        return e

    def run(self, **overrides):
        return subprocess.run(["bash", "roamux/build/ci/tier2_job.sh"], cwd=REPO,
                              capture_output=True, text=True, env=self.env(**overrides),
                              timeout=120)

    def events_list(self):
        return self.events.read_text().splitlines() if self.events.exists() else []

    def out(self, name="out/CI"):
        return self.src / name


class Tier2JobBehaviourTest(unittest.TestCase):
    """roam-280: the job's declared-channel discipline, executed. B1 is the characterization
    case M33 asked for (it passes against the pre-roam-280 script); B3-B12 are the RED cases."""

    @classmethod
    def setUpClass(cls):
        cls.fakes_root = pathlib.Path(tempfile.mkdtemp(prefix="roamux-tier2-fakes-"))
        cls.fakebin = _make_fake_bin(cls.fakes_root / "bin")

    @classmethod
    def tearDownClass(cls):
        __import__("shutil").rmtree(cls.fakes_root, ignore_errors=True)

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-tier2-"))
        self.addCleanup(__import__("shutil").rmtree, self.tmp, ignore_errors=True)

    def _harness(self, **kw):
        return _Tier2Harness(self.tmp, self.fakebin, **kw)

    def _dir_swap(self, status):
        # A child that replaces the overlay link with a REAL directory, then exits `status`.
        return (f'rm "{self.tmp}/src/roamux"\nmkdir "{self.tmp}/src/roamux"\n'
                f'exit {status}')

    # B1 — characterization (passes before and after roam-280)
    def test_forced_build_failure_restores_link_and_keeps_status(self):
        h = self._harness(autoninja="exit 23")
        r = h.run()
        self.assertEqual(r.returncode, 23, r.stdout + r.stderr)
        self.assertEqual(os.readlink(h.link), str(h.canonical), "the trap must restore the link")
        ev = h.events_list()
        reset = next(i for i, l in enumerate(ev) if l.startswith("git ") and "reset --hard HEAD" in l)
        clean = next(i for i, l in enumerate(ev) if l.startswith("git ") and "clean -fd -e /roamux" in l)
        apply_ = next(i for i, l in enumerate(ev)
                      if l.startswith("python3 ") and "apply_patches.py" in l
                      and f"--chromium-src {h.src}" in l)
        self.assertLess(reset, clean, ev)
        self.assertLess(clean, apply_, ev)
        self.assertTrue((h.out() / "build.ninja").exists(), "out/CI must have been cloned")
        self.assertFalse((h.src / "out" / "CI.partial").exists(), "no staging dir may remain")

    # B2 — mostly characterization; the `published` line is roam-280's
    def test_green_run_restores_link_and_publishes_clone(self):
        h = self._harness()
        r = h.run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("tier-2 job green", h.summary.read_text())
        self.assertEqual(os.readlink(h.link), str(h.canonical))
        self.assertIn("clone published as out/CI in", r.stdout)
        # the cloned suite stubs really ran (all three) from out/CI
        ran = [l.split()[0] for l in h.events_list() if l.split()[0] in SUITES]
        self.assertEqual(ran, list(SUITES), h.events_list())

    # B3
    def test_real_directory_at_link_path_refuses_to_flip(self):
        h = self._harness()
        h.link.unlink()
        h.link.mkdir()
        (h.link / "marker").write_text("keep me\n")
        r = h.run()
        self.assertNotEqual(r.returncode, 0, "a real directory at the link path must refuse to start")
        self.assertIn("::error::", r.stdout + r.stderr)
        self.assertTrue(h.link.is_dir() and not h.link.is_symlink(), "the directory must be left alone")
        self.assertFalse((h.link / "roamux").exists(), "flipping into it would create roamux/roamux")
        self.assertTrue((h.link / "marker").exists())
        self.assertEqual(h.events_list(), [], "nothing may be reconciled after a refused start")

    # B4
    def test_trap_reports_a_misfire_and_keeps_the_real_failure(self):
        h = self._harness(autoninja=self._dir_swap(41))
        r = h.run()
        out = r.stdout + r.stderr
        self.assertEqual(r.returncode, 41, "the build's status must survive the misfire: " + out)
        self.assertIn("::warning::", out)
        self.assertIn("previous link target: <none>", out,
                      "observed: the link is gone by the time the trap runs")
        self.assertIn(f"this job had linked it to {h.ws}/roamux", out,
                      "expected: the target this job flipped to, recorded at flip time")
        self.assertTrue(h.link.is_dir() and not h.link.is_symlink())
        self.assertFalse((h.link / "roamux").exists(), "the trap must not link INTO the directory")

    # B5
    def test_trap_turns_a_green_job_red_when_restore_is_impossible(self):
        h = self._harness(suites={"roamux_browsertests": self._dir_swap(0)})
        r = h.run()
        out = r.stdout + r.stderr
        self.assertNotEqual(r.returncode, 0, "a green job that left the base broken must go red: " + out)
        self.assertIn("::error::", out)
        self.assertIn("green", out)
        self.assertFalse((h.link / "roamux").exists())

    # B6
    def test_trap_failure_does_not_replace_the_jobs_status(self):
        # The link cannot be rewritten (its parent is read-only), so the trap's ln fails with
        # status 1 — distinct from the build's 37. The chmod cleanup is registered AFTER setUp's
        # rmtree cleanup, so unittest's LIFO order restores permissions BEFORE the tree is removed.
        src = self.tmp / "src"
        h = self._harness(autoninja=f'chmod 555 "{src}" || exit 99\nexit 37')
        self.addCleanup(os.chmod, src, 0o755)
        r = h.run()
        out = r.stdout + r.stderr
        self.assertNotEqual(r.returncode, 99, "the injection itself failed (chmod)")
        self.assertEqual(r.returncode, 37, "the build's status must not be replaced by ln's: " + out)
        self.assertIn("::warning::", out)
        self.assertIn("restore FAILED", out)
        self.assertNotIn("overlay symlink restored to", out)

    # B7
    def test_interrupted_clone_is_discarded_and_redone(self):
        h = self._harness()
        h.out().mkdir()
        (h.out() / "args.gn").write_text("half\n")          # no build.ninja: an interrupted clone
        (h.src / "out" / "CI.partial").mkdir()
        (h.src / "out" / "CI.partial" / "junk").write_text("stale\n")
        r = h.run()
        out = r.stdout + r.stderr
        self.assertEqual(r.returncode, 0, out)
        self.assertIn("::warning::", out)
        self.assertIn("interrupted", out)
        self.assertTrue((h.out() / "build.ninja").exists(), "must be re-cloned")
        self.assertFalse((h.src / "out" / "CI.partial").exists())

    # B8
    def test_overridden_out_dir_is_never_deleted(self):
        h = self._harness()
        custom = h.src / "out" / "custom"
        custom.mkdir()
        (custom / "marker").write_text("operator data\n")     # not ready, not CI-owned
        r = h.run(ROAMUX_CI_OUT="out/custom")
        out = r.stdout + r.stderr
        self.assertNotEqual(r.returncode, 0, out)
        self.assertIn("::error::", out)
        self.assertIn("ROAMUX_CI_OUT", out)
        self.assertTrue((custom / "marker").exists(), "an overridden OUT must never be deleted")

    # B9
    def test_trap_reaches_its_exit_status_when_logging_fails(self):
        # The function text itself (no copy), run under /bin/bash with stdout CLOSED so every echo
        # fails: (a) a failing job keeps its status; (b) a green job with an unrestorable base goes
        # red; (c) the previous-target CAPTURE fails (readlink broken) and the status still survives.
        # Under errexit an unguarded diagnostic or capture would replace the status.
        fn = _restore_overlay_function(SCRIPT.read_text())
        self.assertIsNotNone(fn, "restore_overlay must be extractable")
        h = self._harness()
        prelude = (f'set -euo pipefail\nSRC="{h.src}"\nROAMUX_CANONICAL_OVERLAY="{h.canonical}"\n'
                   f'FLIPPED_TO="{h.ws}/roamux"\n{fn}trap restore_overlay EXIT\nexec 1>&-\n')
        r = subprocess.run(["/bin/bash", "-c", prelude + "exit 37\n"], capture_output=True,
                           text=True, timeout=30)
        self.assertEqual(r.returncode, 37, r.stderr)
        self.assertEqual(os.readlink(h.link), str(h.canonical))
        h.link.unlink()
        h.link.mkdir()
        r = subprocess.run(["/bin/bash", "-c", prelude + "exit 0\n"], capture_output=True,
                           text=True, timeout=30)
        self.assertEqual(r.returncode, 1, "green + unrestorable must be red even when logging fails")
        self.assertFalse((h.link / "roamux").exists())
        h.link.rmdir()
        h.link.symlink_to(h.prev_ws)
        broken = h.bodies / "readlink"
        broken.write_text("exit 3\n")
        e = dict(os.environ)
        e["PATH"] = f"{h.bin}:/usr/bin:/bin"
        e["ROAMUX_FAKE_READLINK"] = str(broken)
        r = subprocess.run(["/bin/bash", "-c", prelude + "exit 37\n"], capture_output=True,
                           text=True, env=e, timeout=30)
        self.assertEqual(r.returncode, 37, "a failed previous-target capture must not replace the status")

    # B10
    def test_copy_failure_leaves_out_absent_and_staging_present(self):
        events = self.tmp / "events.log"
        cp = (f'eval dest=\\${{$#}}\n'
              f'echo "cp $* out_ci_before=$( [ -e out/CI ] && echo yes || echo no )" >> "{events}"\n'
              'mkdir -p "$dest"\ntouch "$dest/args.gn"\n'
              f'echo "cp populated out_ci_while_staged=$( [ -e out/CI ] && echo yes || echo no )" >> "{events}"\n'
              'exit 1')
        h = self._harness(cp=cp)
        r = h.run()
        self.assertNotEqual(r.returncode, 0, "a failed copy must fail the job")
        self.assertFalse(h.out().exists(), "out/CI must not exist after a failed copy")
        self.assertTrue((h.src / "out" / "CI.partial").exists(), "the staging dir holds the debris")
        ev = h.events_list()
        self.assertIn("out_ci_before=no", next(l for l in ev if "out_ci_before=" in l),
                      "out/CI must be absent when copying begins")
        self.assertIn("out_ci_while_staged=no", next(l for l in ev if "out_ci_while_staged=" in l),
                      "out/CI must still be absent while the staging dir is populated")
        self.assertEqual(os.readlink(h.link), str(h.canonical), "the trap still restores the link")

    # B11
    def test_retry_discards_stale_staging_and_publishes(self):
        h = self._harness()
        stage = h.src / "out" / "CI.partial"
        stage.mkdir()
        (stage / "args.gn").write_text("stale\n")
        r = h.run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue((h.out() / "build.ninja").exists())
        self.assertTrue((h.out() / "args.gn").exists())
        self.assertFalse(stage.exists())
        self.assertEqual((h.out() / "args.gn").read_text(), "is_debug = false\n",
                         "the published dir must come from out/Default, not the stale staging")
        self.assertIn("clone published as out/CI in", r.stdout)

    # B12
    def test_incomplete_staging_copy_is_not_published(self):
        # Either readiness file missing must prevent publication.
        for missing, present in (("build.ninja", "args.gn"), ("args.gn", "build.ninja")):
            with self.subTest(missing=missing):
                tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-tier2-b12-"))
                self.addCleanup(__import__("shutil").rmtree, tmp, ignore_errors=True)
                cp = f'eval dest=\\${{$#}}\nmkdir -p "$dest"\ntouch "$dest/{present}"\nexit 0'
                h = _Tier2Harness(tmp, self.fakebin, cp=cp)
                r = h.run()
                out = r.stdout + r.stderr
                self.assertNotEqual(r.returncode, 0,
                                    f"a staging copy without {missing} must not be published: " + out)
                self.assertIn("::error::", out)
                self.assertIn("incomplete", out)
                self.assertFalse(h.out().exists(), "out/CI must not have been published")

    # B13 (roam-283) — the acceptance's hermetic half: a failing suite leaves its JSON + log, the
    # earlier suite's too, nothing for the suites that never ran; the trap still restores the link
    # and the injected status survives.
    def test_forced_suite_failure_leaves_json_and_log_in_artifacts(self):
        h = self._harness(suites={"roamux_browser_unittests": "exit 7"})
        r = h.run()
        self.assertEqual(r.returncode, 7, r.stdout + r.stderr)
        self.assertEqual(os.readlink(h.link), str(h.canonical))
        for name in ("roamux_unittests", "roamux_browser_unittests"):
            self.assertTrue((h.artifacts / f"{name}.json").exists(), name)
            self.assertTrue((h.artifacts / f"{name}.log").exists(), name)
        for name in ("roamux_sparkle_tests", "roamux_browsertests"):
            self.assertFalse((h.artifacts / f"{name}.json").exists(), name)
            self.assertFalse((h.artifacts / f"{name}.log").exists(), name)

    # B14 (roam-283)
    def test_phase_checkpoints_in_order(self):
        h = self._harness()
        r = h.run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        lines = [l for l in h.summary.read_text().splitlines() if l.startswith("phase=")]
        self.assertEqual([f"phase={p}" for p in Tier2JobScriptTest.PHASES],
                         [l.split(" ")[0] for l in lines], lines)
        for l in lines:
            self.assertRegex(l, r"^phase=[a-z:_-]+ elapsed=\d+s$")

    # B15 (roam-283)
    def test_artifacts_dir_is_created_before_the_first_suite(self):
        h = self._harness()
        self.assertFalse(h.artifacts.exists())
        r = h.run()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        names = sorted(p.name for p in h.artifacts.iterdir())
        self.assertEqual(sorted(f"{s}.{ext}" for s in SUITES for ext in ("json", "log")), names)


if __name__ == "__main__":
    unittest.main()
