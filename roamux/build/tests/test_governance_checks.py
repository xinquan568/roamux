# SPDX-License-Identifier: Apache-2.0
"""Hermetic tests for the governance checkers (roam-38) — the single source of truth the hooks AND CI
both invoke. No Chromium checkout, no external deps.
"""

import ast
import contextlib
import inspect
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

CHECKS = pathlib.Path(__file__).resolve().parents[3] / "scripts" / "checks"


def run_check(script, *args, stdin=None, cwd=None):
    return subprocess.run([sys.executable, str(CHECKS / script), *args],
                          capture_output=True, text=True, input=stdin, cwd=cwd)


class TmpTree(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-gov-"))
        self.addCleanup(__import__("shutil").rmtree, self.tmp, ignore_errors=True)

    def write(self, rel, text):
        f = self.tmp / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(text)
        return str(f)


class SpdxHeaderTest(TmpTree):
    def test_missing_header_rejected(self):
        f = self.write("roamux/common/x.cc", "int main() { return 0; }\n")
        r = run_check("spdx_header.py", f)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("x.cc", r.stdout + r.stderr)

    def test_present_header_accepted(self):
        f = self.write("roamux/common/x.cc", "// SPDX-License-Identifier: Apache-2.0\nint main(){}\n")
        self.assertEqual(run_check("spdx_header.py", f).returncode, 0)

    def test_chromium_src_copy_exempt(self):
        # A full upstream copy keeps the BSD header and must NOT be required to carry ours.
        f = self.write("roamux/chromium_src/chrome/x.h", "// Copyright The Chromium Authors\n")
        self.assertEqual(run_check("spdx_header.py", f).returncode, 0)

    def test_markdown_doc_exempt(self):
        f = self.write("README.md", "# Roamux\nno header here\n")
        self.assertEqual(run_check("spdx_header.py", f).returncode, 0)

    def test_json_data_exempt(self):
        f = self.write("roamux/build/override_signatures.json", '{"pin":"x"}\n')
        self.assertEqual(run_check("spdx_header.py", f).returncode, 0)


class SecretScanTest(TmpTree):
    def test_aws_key_rejected(self):
        # A realistic key (not the documented ...EXAMPLE fake, which is legitimately allowlisted).
        f = self.write("x.py", "KEY = 'AKIAZ7XQ2WPL4NR8YT3D'\n")  # roamux:allow-secret (fixture)
        r = run_check("secret_scan.py", f)
        self.assertNotEqual(r.returncode, 0)

    def test_documented_example_key_allowlisted(self):
        # The AWS-docs fake key must NOT trip the scanner.
        f = self.write("x.py", "KEY = 'AKIAIOSFODNN7EXAMPLE'\n")  # roamux:allow-secret (fixture)
        self.assertEqual(run_check("secret_scan.py", f).returncode, 0)

    def test_pem_private_key_rejected(self):
        f = self.write("x.pem", "-----BEGIN OPENSSH PRIVATE KEY-----\nabc\n")  # roamux:allow-secret (fixture)
        self.assertNotEqual(run_check("secret_scan.py", f).returncode, 0)

    def test_keys_template_placeholder_allowlisted(self):
        f = self.write("roamux/build/google_keys.gni.template",
                       '# google_api_key = "<your key>"\n')
        self.assertEqual(run_check("secret_scan.py", f).returncode, 0)


    def test_allow_marker_honored_in_test_path(self):
        f = self.write("roamux/build/tests/test_x.py",
                       "KEY = 'AKIAZ7XQ2WPL4NR8YT3D'  # roamux:allow-secret\n")
        self.assertEqual(run_check("secret_scan.py", f).returncode, 0)

    def test_allow_marker_ignored_in_normal_source(self):
        # A normal source/config file cannot silence detection with the marker (abuse surface closed).
        f = self.write("roamux/common/x.cc",
                       "const char* K = \"AKIAZ7XQ2WPL4NR8YT3D\";  // roamux:allow-secret\n")
        self.assertNotEqual(run_check("secret_scan.py", f).returncode, 0)

    def test_allow_marker_ignored_when_path_merely_contains_test_(self):
        # 'test_' as a bare substring of a non-test path must NOT enable suppression.
        f = self.write("roamux/browser/contest_manager.cc",
                       "K = 'AKIAZ7XQ2WPL4NR8YT3D'  # roamux:allow-secret\n")
        r = run_check("secret_scan.py", f, cwd=str(self.tmp))
        # invoke with the repo-relative path so the dir/basename gate sees the real shape
        r2 = subprocess.run([sys.executable, str(CHECKS / "secret_scan.py"),
                             "roamux/browser/contest_manager.cc"],
                            capture_output=True, text=True, cwd=str(self.tmp))
        self.assertNotEqual(r2.returncode, 0)


class CommitMsgTest(TmpTree):
    def test_conventional_accepted(self):
        f = self.write("m", "feat(prefs): add the thing\n")
        self.assertEqual(run_check("commit_msg.py", f).returncode, 0)

    def test_valid_subject_without_roam_n_accepted(self):
        # roam-38 checks SYNTAX ONLY — no roam-N linkage (that is roam-39's gate).
        f = self.write("m", "chore: tidy up\n")
        self.assertEqual(run_check("commit_msg.py", f).returncode, 0)

    def test_non_conventional_rejected(self):
        for bad in ("wip\n", "updated stuff\n", "Fixed the bug\n"):
            f = self.write("m", bad)
            self.assertNotEqual(run_check("commit_msg.py", f).returncode, 0, bad)


class PrePushDecisionTest(unittest.TestCase):
    def _decision(self, environ):
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "pre_push", str(CHECKS / "pre_push.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod.gtest_decision(environ)

    def test_skips_when_unset(self):
        self.assertEqual(self._decision({})[0], "skip")

    def test_skips_when_no_out_dir(self):
        self.assertEqual(self._decision({"ROAMUX_CHROMIUM_SRC": "/nonexistent"})[0], "skip")

    def test_runs_when_out_default_present(self):
        d = pathlib.Path(tempfile.mkdtemp(prefix="roamux-fakesrc-"))
        self.addCleanup(__import__("shutil").rmtree, d, ignore_errors=True)
        (d / "out" / "Default").mkdir(parents=True)
        action, src = self._decision({"ROAMUX_CHROMIUM_SRC": str(d)})
        self.assertEqual(action, "run")
        self.assertEqual(src, str(d))


class PrePushReportingTest(unittest.TestCase):
    """roam-259: a blocked push must leave the failing test's output somewhere
    durable. Progress went to block-buffered stdout while failures went to
    unbuffered stderr, so a piped push printed the banner before the context
    and the real failure text was easy to lose."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-prepush-"))
        self.addCleanup(__import__("shutil").rmtree, self.tmp, ignore_errors=True)
        self.mod = self._load()

    def _load(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "pre_push_reporting", str(CHECKS / "pre_push.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod

    def _child(self, script):
        return [sys.executable, "-c", script]

    def test_tee_writes_child_stdout_and_stderr_to_the_log_and_stdout(self):
        log = self.tmp / "pre-push.log"
        script = ("import sys; print('to-stdout'); "
                  "print('to-stderr', file=sys.stderr)")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = self.mod._run_teed(self._child(script), log)
        self.assertEqual(rc, 0)
        text = log.read_text()
        # stderr is merged into the single pipe, so BOTH reach both sinks —
        # that merge is what makes the failure text survive a piped push.
        self.assertIn("to-stdout", text)
        self.assertIn("to-stderr", text)
        self.assertIn("to-stdout", buf.getvalue())
        self.assertIn("to-stderr", buf.getvalue())

    def test_tee_propagates_the_child_return_code(self):
        log = self.tmp / "pre-push.log"
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = self.mod._run_teed(self._child("import sys; sys.exit(3)"), log)
        self.assertEqual(rc, 3)

    def test_tee_survives_an_unwritable_log_path(self):
        # A gate that failed because its LOGGING failed would be a worse bug
        # than the one being fixed.
        unwritable = self.tmp / "nonexistent-dir" / "pre-push.log"
        buf, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(err):
            rc = self.mod._run_teed(self._child("print('still ran')"), unwritable)
        self.assertEqual(rc, 0)
        self.assertIn("still ran", buf.getvalue())

    def test_log_path_resolves_when_dot_git_is_a_file(self):
        # Linked worktrees (which this repo's own /issue2pr pipeline uses for
        # every run) have `.git` as a FILE, so `REPO/.git/x` is not a path.
        # Build a REAL worktree — a hand-faked gitdir pointer would prove
        # nothing about what `git rev-parse --git-dir` actually returns.
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("GIT_")}
        real = self.tmp / "realrepo"
        real.mkdir()

        def git(*args, cwd):
            return subprocess.run(
                ["git", "-c", "user.email=t@example.com", "-c", "user.name=t",
                 *args], cwd=cwd, env=env, capture_output=True, text=True)

        if git("init", "-q", ".", cwd=real).returncode != 0:
            self.skipTest("git unavailable")
        git("commit", "-q", "--allow-empty", "-m", "init", cwd=real)
        wt = self.tmp / "worktree"
        added = git("worktree", "add", "-q", str(wt), cwd=real)
        self.assertEqual(added.returncode, 0, added.stderr)
        self.assertTrue((wt / ".git").is_file(),
                        "fixture must reproduce the .git-is-a-file shape")

        with mock.patch.object(self.mod, "REPO", wt):
            p = self.mod._log_path()
        self.assertIsNotNone(p, "no durable log location resolved in a worktree")
        self.assertTrue(pathlib.Path(p).parent.is_dir(),
                        "resolved log dir must exist: %s" % p)
        # And it must actually be writable there, which is the whole point.
        pathlib.Path(p).write_text("probe")
        self.assertEqual(pathlib.Path(p).read_text(), "probe")

    def test_main_routes_every_child_through_the_tee(self):
        # The helper is worthless if main still calls subprocess.run directly.
        src = self.tmp / "fakesrc"
        (src / "out" / "Default").mkdir(parents=True)
        (src / "out" / "Default" / "roamux_unittests").write_text("")
        calls = []

        def fake_teed(cmd, log, **kwargs):
            calls.append(pathlib.Path(str(cmd[0])).name)
            return 0

        buf = io.StringIO()
        with mock.patch.object(self.mod, "_run_teed", fake_teed), \
                mock.patch.dict(os.environ,
                                {"ROAMUX_CHROMIUM_SRC": str(src)}), \
                contextlib.redirect_stdout(buf):
            rc = self.mod.main()
        self.assertEqual(rc, 0)
        self.assertEqual(len(calls), 3, "hermetic suite, autoninja, gtest: %r" % calls)
        self.assertIn("autoninja", calls)
        self.assertIn("roamux_unittests", calls)

    def test_progress_prints_are_flushed(self):
        # Unflushed prints block-buffer when piped, which is how the banner
        # overtook the context that explained it. Parse rather than grep lines:
        # a print call can wrap, putting flush=True on a continuation line.
        tree = ast.parse(textwrap.dedent(inspect.getsource(self.mod.main)))
        calls = [n for n in ast.walk(tree)
                 if isinstance(n, ast.Call)
                 and isinstance(n.func, ast.Name) and n.func.id == "print"]
        self.assertTrue(calls, "no print calls found in main()")
        for call in calls:
            kwargs = {kw.arg for kw in call.keywords}
            self.assertIn("flush", kwargs,
                          "unflushed print at line %d of main()" % call.lineno)


class OverlayStructureTest(TmpTree):
    def check_rel(self, rel):
        self.write(rel, "// SPDX-License-Identifier: Apache-2.0\nx\n" if rel.endswith((".cc", ".h"))
                   else "diff\n")
        return run_check("overlay_structure.py", rel, cwd=str(self.tmp))

    def test_bad_patch_name_rejected(self):
        self.assertNotEqual(self.check_rel("roamux/patches/bad name.patch").returncode, 0)

    def test_good_patch_name_accepted(self):
        self.assertEqual(self.check_rel("roamux/patches/0005-ok.patch").returncode, 0)

    def test_our_file_at_upstream_path_rejected(self):
        r = self.check_rel("chrome/browser/x.cc")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("upstream mirror path", r.stdout + r.stderr)

    def test_our_file_under_chromium_src_accepted(self):
        self.assertEqual(self.check_rel("roamux/chromium_src/chrome/browser/x.h").returncode, 0)

    def test_additive_roamux_file_accepted(self):
        self.assertEqual(self.check_rel("roamux/browser/x.cc").returncode, 0)


if __name__ == "__main__":
    unittest.main()
