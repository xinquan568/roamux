# SPDX-License-Identifier: Apache-2.0
"""Hermetic tests for apply_patches.py — no Chromium checkout required (CI tier-1 safe).

Fixtures build a throwaway git repo standing in for the Chromium tree, plus a patches dir.
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent.parent / "apply_patches.py"

PATCH_ADD_MARKER = """--- a/afile.txt
+++ b/afile.txt
@@ -1,3 +1,4 @@
 line1
+MARKER
 line2
 line3
"""


def _git_env():
    import os
    return {k: v for k, v in os.environ.items()
            if k not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_PREFIX",
                         "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY")}


def git(cwd, *args):
    subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True,
                   env=_git_env())


class ApplyPatchesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-runhook-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "src"
        self.src.mkdir()
        git(self.src, "init", "-q")
        git(self.src, "config", "user.email", "test@roamux")
        git(self.src, "config", "user.name", "test")
        (self.src / "afile.txt").write_text("line1\nline2\nline3\n")
        git(self.src, "add", ".")
        git(self.src, "commit", "-qm", "init")
        self.patches = self.tmp / "patches"
        self.patches.mkdir()
        (self.patches / "0001-add-marker.patch").write_text(PATCH_ADD_MARKER)

    def run_hook(self, *extra):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--chromium-src", str(self.src),
             "--patches", str(self.patches), *extra],
            capture_output=True, text=True)

    def test_apply_fresh(self):
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("MARKER", (self.src / "afile.txt").read_text())

    def test_idempotent_second_run_is_noop(self):
        self.assertEqual(self.run_hook().returncode, 0)
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.src / "afile.txt").read_text().count("MARKER"), 1)
        self.assertIn("applied", result.stdout)  # reported as already-applied, not re-applied

    def test_conflict_fails_loud_naming_the_patch(self):
        (self.src / "afile.txt").write_text("entirely\ndifferent\ncontent\n")
        git(self.src, "commit", "-aqm", "diverge")
        result = self.run_hook()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0001-add-marker.patch", result.stdout + result.stderr)

    def test_check_mode_does_not_mutate(self):
        result = self.run_hook("--check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("MARKER", (self.src / "afile.txt").read_text())

    def test_check_mode_fails_on_conflict(self):
        (self.src / "afile.txt").write_text("entirely\ndifferent\ncontent\n")
        git(self.src, "commit", "-aqm", "diverge")
        result = self.run_hook("--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("0001-add-marker.patch", result.stdout + result.stderr)


# roam-77: a later patch inserting a line ADJACENT to an earlier patch's
# insertion breaks the earlier patch's reverse-context on the fully-applied
# tree (the 0008x0020 / 0006x0024 field shape). The detector must reason about
# the stack as a whole, not per-patch.
PATCH_ADJACENT_MARKER_B = """--- a/afile.txt
+++ b/afile.txt
@@ -1,3 +1,4 @@
 line1
+MARKER-B
 MARKER
 line2
"""


class AdjacentInsertStackTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-runhook-adj-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.src = self.tmp / "src"
        self.src.mkdir()
        git(self.src, "init", "-q")
        git(self.src, "config", "user.email", "test@roamux")
        git(self.src, "config", "user.name", "test")
        (self.src / "afile.txt").write_text("line1\nline2\nline3\n")
        git(self.src, "add", ".")
        git(self.src, "commit", "-qm", "init")
        self.patches = self.tmp / "patches"
        self.patches.mkdir()
        (self.patches / "0001-add-marker.patch").write_text(PATCH_ADD_MARKER)
        (self.patches / "0002-add-marker-b.patch").write_text(
            PATCH_ADJACENT_MARKER_B)

    def run_hook(self, *extra):
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--chromium-src", str(self.src),
             "--patches", str(self.patches), *extra],
            capture_output=True, text=True)

    def afile(self):
        return (self.src / "afile.txt").read_text()

    def test_fresh_check_and_apply(self):
        result = self.run_hook("--check")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("MARKER", self.afile())
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.afile(),
                         "line1\nMARKER-B\nMARKER\nline2\nline3\n")

    def test_check_passes_on_fully_applied_stack(self):
        # The field failure: per-patch reverse-check reads 0001 as neither
        # applied nor appliable once 0002's line sits inside its context.
        self.assertEqual(self.run_hook().returncode, 0)
        result = self.run_hook("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("[applied]"), 2, result.stdout)

    def test_apply_is_idempotent_on_fully_applied_stack(self):
        self.assertEqual(self.run_hook().returncode, 0)
        before = self.afile()
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.afile(), before)
        self.assertEqual(self.afile().count("MARKER-B"), 1)

    def test_prefix_apply_completes_the_stack(self):
        git(self.src, "apply", str(self.patches / "0001-add-marker.patch"))
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.afile(),
                         "line1\nMARKER-B\nMARKER\nline2\nline3\n")

    def test_prefix_check_reports_and_does_not_mutate(self):
        git(self.src, "apply", str(self.patches / "0001-add-marker.patch"))
        before = self.afile()
        result = self.run_hook("--check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("[applied]", result.stdout)
        self.assertIn("[appliable]", result.stdout)
        self.assertEqual(self.afile(), before)

    def test_relative_patches_dir_works(self):
        # The scratch simulation changes cwd for git apply; a relative
        # --patches path must still resolve (roam-77 Step-8 regression).
        import os
        rel = os.path.relpath(self.patches, os.getcwd())
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--chromium-src", str(self.src),
             "--patches", rel, "--check"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("[appliable]"), 2, result.stdout)

    def test_diverged_tree_fails_loud_both_modes(self):
        self.assertEqual(self.run_hook().returncode, 0)
        with (self.src / "afile.txt").open("a") as f:
            f.write("local-edit\n")
        for extra in (["--check"], []):
            result = self.run_hook(*extra)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("afile.txt", result.stdout + result.stderr)


# ---------------------------------------------------------------------------------------------
# roam-341: `--reconcile` — CI's base reconcile, performed by git as `read-tree --reset -u` to a
# tree of "HEAD + the simulated stack", so a patched file whose bytes already equal the stack's
# target keeps its mtime (Siso then does not re-invalidate its dependents), while every other
# guarantee of the old `reset --hard HEAD` + `clean -fd -e /roamux` + apply sequence is kept.
# The OLD sequence is run on a twin repo as the oracle wherever the non-union outcome matters.
# ---------------------------------------------------------------------------------------------
import os
import stat as _stat

PATCH_OLD_MARKER = """--- a/afile.txt
+++ b/afile.txt
@@ -1,3 +1,4 @@
 line1
+OLD-MARKER
 line2
 line3
"""

PATCH_DELETE_DEL = """--- a/del.txt
+++ /dev/null
@@ -1 +0,0 @@
-gone
"""

PATCH_ADD_NEW = """--- /dev/null
+++ b/added.txt
@@ -0,0 +1 @@
+added
"""

PATCH_INNER = """--- a/dir/inner.txt
+++ b/dir/inner.txt
@@ -1 +1,2 @@
 inner
+INNER-MARKER
"""

PATCH_A1 = """--- a/a1.h
+++ b/a1.h
@@ -1 +1,2 @@
 a1
+A1-MARKER
"""

PATCH_CONFLICT = """--- a/afile.txt
+++ b/afile.txt
@@ -1,3 +1,4 @@
 nope
+MARKER
 line2
 line3
"""

OLD_MTIME = 946684800  # 2000-01-01T00:00:00Z


def _gitout(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True,
                          text=True, env=_git_env()).stdout


def _snapshot(root):
    """Everything under root except .git: path -> (kind, mode-bits, bytes-or-link-target)."""
    out = {}
    root = pathlib.Path(root)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d != ".git"]
        for name in filenames + [d for d in dirnames if (pathlib.Path(dirpath) / d).is_symlink()]:
            f = pathlib.Path(dirpath) / name
            rel = str(f.relative_to(root))
            st = os.lstat(f)
            if _stat.S_ISLNK(st.st_mode):
                out[rel] = ("link", None, os.readlink(f))
            else:
                out[rel] = ("file", st.st_mode & 0o777, f.read_bytes())
    return out


class ReconcileModeTest(unittest.TestCase):
    """Behavioural contract of `apply_patches.py --reconcile` on real git repos (no checkout)."""

    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-reconcile-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        self.patches = self.tmp / "patches"
        self.patches.mkdir()
        (self.patches / "0001-add-marker.patch").write_text(PATCH_ADD_MARKER)
        self.src = self.make_repo(self.tmp / "src")

    # -- fixture helpers -------------------------------------------------------------------
    def make_repo(self, path, detached=False):
        path.mkdir()
        git(path, "init", "-q", "-b", "main")
        git(path, "config", "user.email", "test@roamux")
        git(path, "config", "user.name", "test")
        git(path, "config", "core.filemode", "true")
        git(path, "config", "core.logAllRefUpdates", "true")
        (path / "afile.txt").write_text("line1\nline2\nline3\n")
        (path / "del.txt").write_text("gone\n")
        (path / "keep.txt").write_text("keep\n")
        (path / "dir").mkdir()
        (path / "dir" / "inner.txt").write_text("inner\n")
        (path / "a1.h").write_text("a1\n")
        (path / ".gitignore").write_text("out/\n")
        git(path, "add", ".")
        git(path, "commit", "-qm", "init")
        git(path, "tag", "1.0.0.0")
        if detached:
            git(path, "checkout", "-q", "1.0.0.0")
        return path

    def run_hook(self, *extra, src=None, patches=None, env=None):
        e = dict(_git_env())
        if env:
            e.update(env)
        return subprocess.run(
            [sys.executable, str(SCRIPT), "--chromium-src", str(src or self.src),
             "--patches", str(patches or self.patches), *extra],
            capture_output=True, text=True, env=e)

    def reconcile(self, **kw):
        r = self.run_hook("--reconcile", **kw)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    def old_sequence(self, src, patches=None):
        """The pre-roam-341 CI reconcile, as the oracle for non-union outcomes."""
        git(src, "reset", "-q", "--hard", "HEAD")
        git(src, "clean", "-fdq", "-e", "/roamux")
        r = self.run_hook(src=src, patches=patches)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def apply_plain(self, src=None):
        r = self.run_hook(src=src)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def age(self, rel, src=None):
        f = (src or self.src) / rel
        os.utime(f, (OLD_MTIME, OLD_MTIME))
        return os.lstat(f).st_mtime

    def mtime(self, rel, src=None):
        return os.lstat((src or self.src) / rel).st_mtime

    def afile(self, src=None):
        return ((src or self.src) / "afile.txt").read_text()

    def assert_same_outcome(self, setup, patches=None):
        """Apply `setup` to two twin repos; new sequence on one, old sequence on the other."""
        new = self.make_repo(self.tmp / "new")
        old = self.make_repo(self.tmp / "old")
        setup(new)
        setup(old)
        head_new = _gitout(new, "rev-parse", "HEAD")
        self.reconcile(src=new, patches=patches)
        self.old_sequence(old, patches)
        self.assertEqual(_snapshot(new), _snapshot(old))
        self.assertEqual(_gitout(new, "status", "--porcelain"), _gitout(old, "status", "--porcelain"))
        self.assertEqual(_gitout(new, "ls-files", "-s"), _gitout(old, "ls-files", "-s"))
        self.assertEqual(_gitout(new, "rev-parse", "HEAD"), head_new, "HEAD must not move")
        return new, old

    # 1 -- the point of the change --------------------------------------------------------
    def test_kept_when_bytes_equal_preserves_mtime(self):
        self.apply_plain()
        old = self.age("afile.txt")
        r = self.reconcile()
        self.assertEqual(self.mtime("afile.txt"), old, "byte-identical patched file must keep its mtime")
        self.assertIn("MARKER", self.afile())
        self.assertIn("1 kept", r.stdout)
        self.assertIn("0 rewritten", r.stdout)

    # 2 -- rewritten when bytes differ; shrunk stack; empty stack ---------------------------
    def test_superseded_stack_is_rewritten(self):
        old_patches = self.tmp / "old-patches"
        old_patches.mkdir()
        (old_patches / "0001-old.patch").write_text(PATCH_OLD_MARKER)
        self.apply_plain(src=None) if False else None
        r = self.run_hook(patches=old_patches)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("OLD-MARKER", self.afile())
        old = self.age("afile.txt")
        r = self.reconcile()
        self.assertIn("MARKER\n", self.afile())
        self.assertNotIn("OLD-MARKER", self.afile())
        self.assertNotEqual(self.mtime("afile.txt"), old)
        self.assertIn("[rewritten] afile.txt", r.stdout)
        self.assertIn("reconcile: 0 kept, 1 rewritten, 0 removed, 0 restored", r.stdout)

    def test_shrunk_stack_restores_dropped_patch_paths(self):
        (self.patches / "0002-delete-del.patch").write_text(PATCH_DELETE_DEL)
        self.apply_plain()
        self.assertFalse((self.src / "del.txt").exists())
        (self.patches / "0002-delete-del.patch").unlink()
        self.reconcile()
        self.assertEqual((self.src / "del.txt").read_text(), "gone\n")
        self.assertIn("MARKER", self.afile())

    def test_empty_stack_behaves_like_reset_hard(self):
        empty = self.tmp / "empty"
        empty.mkdir()

        def setup(src):
            (src / "keep.txt").write_text("dirty\n")
            (src / "stray.txt").write_text("stray\n")
        new, _ = self.assert_same_outcome(setup, patches=empty)
        self.assertEqual((new / "keep.txt").read_text(), "keep\n")
        self.assertFalse((new / "stray.txt").exists())

    # 3 -- pristine tree ----------------------------------------------------------------------
    def test_pristine_tree_is_patched_and_plain_runhook_sees_applied(self):
        self.reconcile()
        self.assertIn("MARKER", self.afile())
        r = self.run_hook("--check")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("[applied]", r.stdout)

    # 4 -- a stack that does not apply mutates nothing ------------------------------------------
    def test_non_applying_stack_fails_loud_without_mutation(self):
        (self.patches / "0001-add-marker.patch").write_text(PATCH_CONFLICT)
        (self.src / "keep.txt").write_text("dirty\n")
        old = self.age("afile.txt")
        before = _snapshot(self.src)
        head = _gitout(self.src, "rev-parse", "HEAD")
        index = _gitout(self.src, "ls-files", "-s")
        r = self.run_hook("--reconcile")
        self.assertEqual(r.returncode, 1)
        self.assertIn("0001-add-marker.patch", r.stdout + r.stderr)
        self.assertEqual(_snapshot(self.src), before)
        self.assertEqual(self.mtime("afile.txt"), old)
        self.assertEqual(_gitout(self.src, "rev-parse", "HEAD"), head)
        self.assertEqual(_gitout(self.src, "ls-files", "-s"), index)

    # 5 -- non-union equivalence with the old sequence -------------------------------------------
    def test_non_union_states_match_reset_hard(self):
        def setup(src):
            git(src, "config", "core.filemode", "true")
            (src / "ren-src.txt").write_text("r\n")                       # a tracked file to rename —
            git(src, "add", "ren-src.txt")                                # committed FIRST, so the
            git(src, "commit", "-qm", "ren")                              # staged states below stay
            git(src, "tag", "-f", "1.0.0.0")                              # out of HEAD
            (src / "keep.txt").write_text("modified\n")                  # modified tracked
            (src / "dir" / "inner.txt").unlink()                          # deleted in worktree
            git(src, "rm", "-q", "--cached", "del.txt")                   # staged deletion, file kept
            (src / "new.txt").write_text("new\n")
            git(src, "add", "new.txt")                                    # staged plain addition
            (src / "out").mkdir()
            (src / "out" / "ign.txt").write_text("ign\n")
            git(src, "add", "-f", "out/ign.txt")                          # staged IGNORED addition
            (src / "a[1].h").write_text("wild\n")
            git(src, "add", "a[1].h")                                     # wildcard-shaped staged addition
            git(src, "mv", "ren-src.txt", "ren-dst.txt")                  # staged rename
            git(src, "rm", "-q", "--cached", "dir/inner.txt")             # staged rename INTO an
            (src / "out" / "inner.txt").write_text("inner\n")            # ignored destination
            git(src, "add", "-f", "out/inner.txt")                        # (dir/inner.txt already
                                                                          #  deleted in worktree)
            (src / "keep.txt").write_text("staged+unstaged\n")
            git(src, "add", "keep.txt")
            (src / "keep.txt").write_text("unstaged-on-top\n")            # staged + unstaged edit
            os.chmod(src / ".gitignore", 0o755)                           # mode-only change
            (src / "stray.txt").write_text("stray\n")                     # untracked leftover
        new, old = self.assert_same_outcome(setup)
        self.assertIn("MARKER", self.afile(new))
        for gone in ("new.txt", "out/ign.txt", "a[1].h", "ren-dst.txt", "out/inner.txt", "stray.txt"):
            self.assertFalse((new / gone).exists(), gone)
        self.assertEqual((new / "keep.txt").read_text(), "keep\n")
        self.assertEqual((new / "ren-src.txt").read_text(), "r\n")
        self.assertEqual((new / "dir" / "inner.txt").read_text(), "inner\n")

    def test_summary_reports_exact_paths_including_rename_sources(self):
        git(self.src, "mv", "keep.txt", "moved.txt")                      # staged rename
        (self.src / "dir" / "inner.txt").write_text("dirty\n")
        r = self.reconcile()
        reported = {l.split(None, 1)[1] for l in r.stdout.splitlines() if l.startswith("[restored]")}
        self.assertEqual(reported, {"keep.txt", "moved.txt", "dir/inner.txt"}, r.stdout)
        self.assertIn("reconcile: 0 kept, 1 rewritten, 0 removed, 3 restored", r.stdout)

    def test_wildcard_named_neighbour_does_not_touch_union_mtime(self):
        # `a[1].h` as a pathspec would match `a1.h`; with --literal-pathspecs it must not.
        (self.patches / "0002-a1.patch").write_text(PATCH_A1)
        self.apply_plain()
        old = self.age("a1.h")
        (self.src / "a[1].h").write_text("wild\n")
        git(self.src, "add", "a[1].h")
        r = self.reconcile()
        self.assertEqual(self.mtime("a1.h"), old, "the literal neighbour must keep its mtime")
        self.assertEqual((self.src / "a1.h").read_text(), "a1\nA1-MARKER\n")
        self.assertFalse((self.src / "a[1].h").exists())
        self.assertIn("2 kept", r.stdout)
        self.assertIn("[restored]  a[1].h", r.stdout)

    def test_symlink_replacing_non_union_file_and_gitlink_untouched(self):
        def setup(src):
            (src / "keep.txt").unlink()
            os.symlink("/etc/hosts", src / "keep.txt")                    # regular file -> symlink
            sub = src / "sub"
            sub.mkdir()
            git(sub, "init", "-q")
            git(sub, "config", "user.email", "t@t")
            git(sub, "config", "user.name", "t")
            (sub / "s.txt").write_text("s\n")
            git(sub, "add", ".")
            dates = {**_git_env(), "GIT_AUTHOR_DATE": "2000-01-01T00:00:00Z",
                     "GIT_COMMITTER_DATE": "2000-01-01T00:00:00Z"}   # identical OID in both twins
            subprocess.run(["git", "-C", str(sub), "commit", "-qm", "sub"], check=True,
                           capture_output=True, env=dates)
            sha = _gitout(sub, "rev-parse", "HEAD").strip()
            git(src, "update-index", "--add", "--cacheinfo", f"160000,{sha},sub")   # gitlink
            git(src, "commit", "-qm", "gitlink")
            git(src, "tag", "-f", "1.0.0.0")
            (sub / "s.txt").write_text("dirty-in-sub\n")                 # nested repo left alone
        new, _ = self.assert_same_outcome(setup)
        self.assertFalse((new / "keep.txt").is_symlink())
        self.assertEqual((new / "keep.txt").read_text(), "keep\n")
        self.assertEqual((new / "sub" / "s.txt").read_text(), "dirty-in-sub\n")

    # 6 -- union entry types --------------------------------------------------------------------
    def test_union_path_replaced_by_symlink_dangling_or_directory(self):
        target = self.tmp / "elsewhere.txt"
        target.write_text("line1\nMARKER\nline2\nline3\n")             # equal-content symlink target
        (self.src / "afile.txt").unlink()
        os.symlink(target, self.src / "afile.txt")
        self.reconcile()
        self.assertFalse((self.src / "afile.txt").is_symlink())
        self.assertIn("MARKER", self.afile())
        self.assertEqual(target.read_text(), "line1\nMARKER\nline2\nline3\n", "link target untouched")
        (self.src / "afile.txt").unlink()
        other = self.tmp / "other.txt"
        other.write_text("something else\n")                            # DIFFERING-content target
        os.symlink(other, self.src / "afile.txt")
        r = self.reconcile()
        self.assertFalse((self.src / "afile.txt").is_symlink())
        self.assertIn("MARKER", self.afile())
        self.assertEqual(other.read_text(), "something else\n", "link target untouched")
        self.assertIn("[rewritten] afile.txt", r.stdout)
        (self.src / "afile.txt").unlink()
        os.symlink(self.tmp / "missing", self.src / "afile.txt")        # dangling
        self.reconcile()
        self.assertFalse((self.src / "afile.txt").is_symlink())
        self.assertIn("MARKER", self.afile())
        (self.src / "afile.txt").unlink()
        (self.src / "afile.txt" / "out").mkdir(parents=True)             # obstructing directory,
        (self.src / "afile.txt" / "out" / "x").write_text("ign\n")       # with ignored contents
        self.reconcile()
        self.assertTrue((self.src / "afile.txt").is_file())
        self.assertIn("MARKER", self.afile())

    def test_symlinked_parent_and_hardlink_alias(self):
        (self.patches / "0002-inner.patch").write_text(PATCH_INNER)
        real = self.tmp / "realdir"
        real.mkdir()
        (real / "inner.txt").write_text("inner\n")
        shutil.rmtree(self.src / "dir")
        os.symlink(real, self.src / "dir")                               # symlinked parent of a union path
        self.reconcile()
        self.assertFalse((self.src / "dir").is_symlink())
        self.assertEqual((self.src / "dir" / "inner.txt").read_text(), "inner\nINNER-MARKER\n")
        self.assertEqual((real / "inner.txt").read_text(), "inner\n", "the link's target untouched")
        # hardlink alias (under the ignored out/, so clean spares it) of a union file whose
        # content differs from the target
        (self.src / "afile.txt").write_text("line1\nOLD\nline2\nline3\n")
        (self.src / "out").mkdir()
        os.link(self.src / "afile.txt", self.src / "out" / "alias.txt")
        self.reconcile()
        self.assertIn("MARKER", self.afile())
        self.assertEqual((self.src / "out" / "alias.txt").read_text(), "line1\nOLD\nline2\nline3\n",
                         "git must unlink before writing; a hardlink alias is not written through")

    def test_directory_obstruction_through_symlinked_ancestor_never_escapes(self):
        # BLOCKER (Step-8 review): src/dir -> outside, and outside/inner.txt is a DIRECTORY holding a
        # sentinel. The union path dir/inner.txt is then a real directory reached THROUGH a symlink;
        # removing it would delete the external directory. The reconcile must leave the link's
        # target alone and let git replace the symlinked ancestor — for a surviving union path and
        # for a stack-DELETED one alike, matching the old sequence.
        (self.patches / "0002-inner.patch").write_text(PATCH_INNER)
        for case in ("surviving", "deleted", "deleted-ignored-link"):
            with self.subTest(case=case):
                new = self.make_repo(self.tmp / f"sym-{case}-new")
                old = self.make_repo(self.tmp / f"sym-{case}-old")
                patches = self.patches
                if case.startswith("deleted"):
                    patches = self.tmp / f"patches-{case}"
                    patches.mkdir()
                    (patches / "0001-add-marker.patch").write_text(PATCH_ADD_MARKER)
                    (patches / "0002-delete-inner.patch").write_text(
                        "--- a/dir/inner.txt\n+++ /dev/null\n@@ -1 +0,0 @@\n-inner\n")
                outside = self.tmp / f"outside-{case}"
                (outside / "inner.txt").mkdir(parents=True)
                (outside / "inner.txt" / "sentinel").write_text("keep me\n")
                for src in (new, old):
                    shutil.rmtree(src / "dir")
                    os.symlink(outside, src / "dir")
                    if case == "deleted-ignored-link":
                        # the link itself is ignored: clean would keep it, and git skips an
                        # entry it only DELETES behind an obstructing ancestor — the stack-
                        # deleted path would stay reachable through the link (round-2 finding)
                        (src / ".git" / "info" / "exclude").write_text("/dir\n")
                self.reconcile(src=new, patches=patches)
                self.old_sequence(old, patches)
                self.assertEqual((outside / "inner.txt" / "sentinel").read_text(), "keep me\n",
                                 "the reconcile must never remove anything outside the checkout")
                self.assertEqual(_snapshot(new), _snapshot(old))
                self.assertFalse((new / "dir").is_symlink(), "the symlinked ancestor is gone")
                if case == "surviving":
                    self.assertEqual((new / "dir" / "inner.txt").read_text(), "inner\nINNER-MARKER\n")
                else:
                    self.assertFalse(os.path.lexists(new / "dir" / "inner.txt"))
                    self.assertFalse(os.path.lexists(new / "dir"), "nothing may keep the deleted path reachable")

    def test_staged_file_directory_conflicts_both_directions(self):
        (self.patches / "0002-inner.patch").write_text(PATCH_INNER)
        # union file afile.txt vs staged afile.txt/child
        (self.src / "afile.txt").unlink()
        (self.src / "afile.txt").mkdir()
        (self.src / "afile.txt" / "child").write_text("c\n")
        git(self.src, "add", "afile.txt/child")
        # union dir/inner.txt vs staged FILE dir
        shutil.rmtree(self.src / "dir")
        (self.src / "dir").write_text("i am a file\n")
        git(self.src, "add", "dir")
        self.reconcile()
        self.assertTrue((self.src / "afile.txt").is_file())
        self.assertIn("MARKER", self.afile())
        self.assertEqual((self.src / "dir" / "inner.txt").read_text(), "inner\nINNER-MARKER\n")
        self.assertEqual(_gitout(self.src, "diff", "--cached", "--name-only"), "")

    def test_staged_deletion_of_union_file_with_equal_bytes_keeps_mtime(self):
        self.apply_plain()
        old = self.age("afile.txt")
        git(self.src, "rm", "-q", "--cached", "afile.txt")
        self.reconcile()
        self.assertEqual(self.mtime("afile.txt"), old)
        self.assertIn("MARKER", self.afile())
        self.assertEqual(_gitout(self.src, "diff", "--cached", "--name-only"), "")

    # 7 -- stack deletions ------------------------------------------------------------------------
    def test_stack_deletion_removes_tracked_file_in_all_states(self):
        (self.patches / "0002-delete-del.patch").write_text(PATCH_DELETE_DEL)

        def plain(src):
            pass

        def staged(src):
            git(src, "rm", "-q", "--cached", "del.txt")

        def ignored(src):
            (src / ".gitignore").write_text("out/\ndel.txt\n")
            git(src, "add", ".gitignore")
            git(src, "commit", "-qm", "ignore del")
            git(src, "tag", "-f", "1.0.0.0")

        def obstructed(src):
            (src / "del.txt").unlink()
            (src / "del.txt" / "out").mkdir(parents=True)
            (src / "del.txt" / "out" / "x").write_text("ign\n")
        for i, setup in enumerate((plain, staged, ignored, obstructed)):
            with self.subTest(case=setup.__name__):
                new = self.make_repo(self.tmp / f"n{i}")
                old = self.make_repo(self.tmp / f"o{i}")
                setup(new)
                setup(old)
                r = self.reconcile(src=new)
                self.old_sequence(old)
                self.assertFalse((new / "del.txt").exists(), setup.__name__)
                self.assertEqual(_snapshot(new), _snapshot(old), setup.__name__)
                self.assertIn("[removed]   del.txt", r.stdout, setup.__name__)

    # 8 -- index, HEAD, ORIG_HEAD, reflogs ---------------------------------------------------------
    def test_head_orig_head_and_reflogs_untouched(self):
        for detached in (False, True):
            with self.subTest(detached=detached):
                src = self.make_repo(self.tmp / ("det" if detached else "att"), detached=detached)
                head = _gitout(src, "rev-parse", "HEAD")
                (src / ".git" / "ORIG_HEAD").write_bytes(b"0" * 40 + b"\n")   # a pre-existing ORIG_HEAD
                logs = {p: p.read_bytes() for p in (src / ".git" / "logs").rglob("*") if p.is_file()}
                self.reconcile(src=src)
                self.assertEqual(_gitout(src, "rev-parse", "HEAD"), head)
                self.assertEqual((src / ".git" / "ORIG_HEAD").read_bytes(), b"0" * 40 + b"\n",
                                 "ORIG_HEAD must not be rewritten")
                self.assertEqual({p: p.read_bytes() for p in (src / ".git" / "logs").rglob("*") if p.is_file()},
                                 logs, "no reflog may change")
                self.assertEqual(_gitout(src, "diff", "--cached", "--name-only"), "")
                self.assertEqual(_gitout(src, "status", "--porcelain").strip(), "M afile.txt")

    # 9 -- clean semantics ---------------------------------------------------------------------------
    def test_clean_semantics_and_patch_added_file(self):
        (self.patches / "0002-add-new.patch").write_text(PATCH_ADD_NEW)
        elsewhere = self.tmp / "overlay"
        elsewhere.mkdir()
        os.symlink(elsewhere, self.src / "roamux")                         # the overlay symlink
        (self.src / "out").mkdir()
        (self.src / "out" / "cache").write_text("warm\n")                   # ignored: must survive
        (self.src / "stray.txt").write_text("stray\n")
        self.reconcile()
        self.assertTrue((self.src / "roamux").is_symlink())
        self.assertEqual((self.src / "out" / "cache").read_text(), "warm\n")
        self.assertFalse((self.src / "stray.txt").exists())
        self.assertEqual((self.src / "added.txt").read_text(), "added\n")
        old = self.age("added.txt")
        self.reconcile()
        self.assertEqual(self.mtime("added.txt"), old, "a patch-added file survives clean and keeps its mtime")

    # 10 -- interruption ---------------------------------------------------------------------------
    def test_interrupted_reconcile_converges_without_moving_head(self):
        head = _gitout(self.src, "rev-parse", "HEAD")
        r = self.run_hook("--reconcile", env={"ROAMUX_RECONCILE_STOP_AFTER": "read-tree"})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(_gitout(self.src, "rev-parse", "HEAD"), head)
        self.assertNotEqual(_gitout(self.src, "diff", "--cached", "--name-only"), "", "stopped with a seeded index")
        self.reconcile()
        self.assertEqual(_gitout(self.src, "rev-parse", "HEAD"), head)
        self.assertEqual(_gitout(self.src, "diff", "--cached", "--name-only"), "")
        self.assertIn("MARKER", self.afile())

    # 11 -- flags ----------------------------------------------------------------------------------
    def test_reconcile_and_check_are_mutually_exclusive(self):
        before = _snapshot(self.src)
        r = self.run_hook("--reconcile", "--check")
        self.assertEqual(r.returncode, 2)
        self.assertIn("--reconcile cannot be combined with --check", r.stderr)
        self.assertEqual(_snapshot(self.src), before)

    # 12 -- the runhook's own git usage is pinned ---------------------------------------------------
    def test_runhook_source_pins_its_git_channels(self):
        code = SCRIPT.read_text()
        code = code.split('"""', 2)[2]  # the module docstring describes the OLD sequence by name
        self.assertIn('"clean", "-fd", "-e", "/roamux"', code)
        self.assertNotIn("-fdx", code)
        self.assertNotIn("-ffd", code)
        for forbidden in ("reset --hard", '"--hard"', "commit-tree", "update-ref", "symbolic-ref"):
            self.assertNotIn(forbidden, code, f"the reconcile must not use {forbidden}")
        self.assertIn('"read-tree", "--reset", "-u"', code)


if __name__ == "__main__":
    unittest.main()
