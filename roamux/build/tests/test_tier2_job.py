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

    def test_all_three_suites_build_and_run(self):
        # roam-6 (WB-CI): the browser-test suite joined the tier-2 gate; a regression to the
        # two-suite line must fail here, not silently in CI.
        self.assertIn(
            "roamux_unittests roamux_browser_unittests roamux_browsertests",
            self.code)
        for binary in ("roamux_unittests", "roamux_browser_unittests",
                       "roamux_browsertests"):
            self.assertIn('"${OUT}/%s"' % binary, self.code)

    def test_every_suite_run_sets_an_explicit_retry_limit(self):
        # roam-195: the launcher ZEROES its retry limit when a --gtest_filter is passed
        # outside bot mode (base/test/launcher/test_launcher.cc: "not in bot mode and
        # filtered by flag ... Set reties to zero"), so the filtered roamux_browsertests
        # line silently ran with NO retries while the two unfiltered suites kept the
        # default of 1. Every recorded teardown-timeout flake landed in exactly that
        # unprotected suite. The explicit flag is resolved BEFORE the filter branch, so
        # passing it restores retries; assert it on ALL THREE invocations so a later
        # --gtest_filter added to another suite cannot silently disarm them again.
        run_lines = [l for l in self.code.splitlines() if '"${OUT}/roamux' in l]
        self.assertEqual(3, len(run_lines), f"expected 3 suite runs, got {run_lines}")
        for line in run_lines:
            self.assertIn("--test-launcher-retry-limit=", line,
                          f"suite run without an explicit retry limit: {line.strip()}")

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
        allowed_markers = ("ln -sfn", "apply_patches.py", "check_override_staleness.py", "--chromium-src",
                           "autoninja", "gn ", "cd ", "cp ", "OUT=", "SRC=", "echo", "test ", "[ ",
                           "reset --hard HEAD", "clean -fd -e /roamux")
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


if __name__ == "__main__":
    unittest.main()


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
        got = argv.read_text().split()
        self.assertEqual(got[0], "-sim", f"caffeinate flags must be -sim (got {got})")
        self.assertTrue(got[1].endswith("tier2_job.sh"),
                        f"caffeinate must exec the job script (got {got})")
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
