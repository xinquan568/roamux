# SPDX-License-Identifier: Apache-2.0
"""Tests for the git-cliff changelog generator (roam-40). The end-to-end render runs the real git-cliff
when present and skips-with-note when absent (honest degradation, roam-38's pattern). The config +
wrapper are checked structurally, hermetically.
"""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "scripts" / "gen-changelog.sh"
CLIFF = REPO_ROOT / "cliff.toml"

# roam-38's enforced Conventional-Commit types — cliff.toml must map every one to a group.
ROAM38_TYPES = ["feat", "fix", "chore", "docs", "test", "refactor", "perf", "build", "ci", "style",
                "revert"]


def _clean_git_env():
    return {k: v for k, v in os.environ.items()
            if k not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_PREFIX",
                         "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY")}


def git(cwd, *args):
    subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True, env=_clean_git_env())


class CliffConfigTest(unittest.TestCase):
    def test_cliff_covers_all_roam38_types(self):
        text = CLIFF.read_text()
        for t in ROAM38_TYPES:
            self.assertIn(f'message = "^{t}"', text, f"cliff.toml has no parser for '{t}'")

    def test_cliff_is_conventional_and_filters_unconventional(self):
        text = CLIFF.read_text()
        self.assertIn("conventional_commits = true", text)
        self.assertIn("filter_unconventional = true", text)


class GenChangelogWrapperTest(unittest.TestCase):
    def test_fails_loud_when_tool_missing(self):
        text = SCRIPT.read_text()
        self.assertIn('command -v git-cliff', text)
        self.assertIn("exit 1", text)
        self.assertIn("Install", text)

    def test_no_committed_changelog(self):
        # generate-on-demand policy: the repo must not carry a perpetually-stale CHANGELOG.md.
        self.assertFalse((REPO_ROOT / "CHANGELOG.md").exists(),
                         "CHANGELOG.md is generate-on-demand; do not commit a stale copy")


class GenChangelogEndToEndTest(unittest.TestCase):
    def setUp(self):
        if not shutil.which("git-cliff"):
            self.skipTest("git-cliff not installed — end-to-end render deferred to a runner that has it")
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamex-cliff-"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def test_renders_grouped_changelog_from_tagged_history(self):
        d = self.tmp
        git(d, "init", "-q")
        git(d, "config", "user.email", "t@roamex"); git(d, "config", "user.name", "t")
        shutil.copy(CLIFF, d / "cliff.toml")
        (d / "a").write_text("1")
        git(d, "add", "."); git(d, "commit", "-qm", "feat(x): add a thing (roam-1)")
        (d / "b").write_text("2")
        git(d, "add", "."); git(d, "commit", "-qm", "fix(y): correct a thing (roam-2)")
        git(d, "tag", "v0.1.0")
        out = subprocess.run(["git-cliff", "--config", "cliff.toml", "--tag", "v0.1.0"],
                             cwd=d, capture_output=True, text=True, env=_clean_git_env())
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertIn("Features", out.stdout)
        self.assertIn("Bug Fixes", out.stdout)
        self.assertIn("add a thing", out.stdout.lower())  # upper_first capitalizes it


if __name__ == "__main__":
    unittest.main()
