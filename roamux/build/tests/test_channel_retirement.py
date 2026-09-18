# SPDX-License-Identifier: Apache-2.0
"""Retirement oracle for the ``chromium_src`` override channel (roam-300, grill M24, ADR 0004).

The channel was: patches 0002 (``//roamux/chromium_src`` prepended to GN's ``default_include_dirs``)
and 0003 (a sample ``#define`` in ``chrome_constants.h``), one override header under
``roamux/chromium_src/``, the §12.5 staleness gate (``check_override_staleness.py`` +
``override_signatures.json`` + its hermetic test + a tier-2 phase), and a mechanism unittest that
consumed both markers.

This oracle proves the retirement is complete where it matters operationally: none of the retired
paths exist, and none of the retired identifiers appear in any *operational* tracked file — every
GN file and shell script anywhere in the repository, Python under ``roamux/``, ``scripts/``, the
hooks, the workflows, every ``.cc/.h/.mm`` under ``roamux/``, and the ADDED lines of every patch hunk.
Docs, ``*.md`` and patch preambles are deliberately outside the scan: historical records (the
grill report, patch preambles, ADR 0001's body) and ADR 0004 itself must be allowed to name the
channel. This file names the identifiers in its own assertions and is excluded from its own scan.

``chrome_isolated_world_ids`` is NOT a retired identifier: the upstream header survives and may be
included legitimately.
"""

import pathlib
import re
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
SELF = pathlib.Path(__file__).resolve()

RETIRED_PATHS = (
    "roamux/chromium_src",
    "roamux/build/override_signatures.json",
    "roamux/build/check_override_staleness.py",
    "roamux/build/tests/test_override_staleness.py",
    "roamux/test/roamux_overlay_mechanism_unittest.cc",
)
RETIRED_PATCH_PREFIXES = ("0002-", "0003-")

# Matched with or without a trailing slash, so the bare GN value "//roamux/chromium_src" hits too.
IDENTIFIER_RE = re.compile(
    r"ROAMUX_SAMPLE_PATCH_ACTIVE|ROAMUX_CHROMIUM_SRC_OVERRIDE_ACTIVE|check_override_staleness"
    r"|override_signatures|roamux_overlay_mechanism_unittest|roamux/chromium_src")

REPO_WIDE_SUFFIXES = (".gn", ".gni", ".sh")          # GN graph and shell: anywhere in the repo
ROAMUX_SUFFIXES = (".py", ".cc", ".h", ".mm")        # code under roamux/
OPERATIONAL_DIRS = ("scripts/", ".githooks/", ".github/workflows/")


def tracked_files(root):
    """Repo-relative POSIX paths of tracked files (``git ls-files``); falls back to a walk for a
    plain directory (the temp trees in the tests of this test)."""
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True,
                             capture_output=True).stdout
        return [p.decode() for p in out.split(b"\0") if p]
    except (subprocess.CalledProcessError, FileNotFoundError):
        return [p.relative_to(root).as_posix() for p in sorted(root.rglob("*"))
                if p.is_file() and ".git" not in p.parts]


def is_operational(rel):
    if rel.startswith("docs/") or rel.endswith(".md"):
        return False
    if rel.endswith("BUILD.gn") or rel.endswith(REPO_WIDE_SUFFIXES):
        return True
    if rel.endswith(ROAMUX_SUFFIXES) and rel.startswith("roamux/"):
        return True
    return rel.startswith(OPERATIONAL_DIRS)


def patch_added_lines(text):
    """The ``+`` lines of every hunk: everything after the first ``@@`` hunk header. This covers
    ``diff --git`` patches and the plain unified-diff patches (``---``/``+++`` headers only); a
    ``#`` preamble precedes any hunk header and is therefore never scanned."""
    lines = text.splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith("@@"))
    except StopIteration:
        return []
    return [l[1:] for l in lines[start:] if l.startswith("+") and not l.startswith("+++")]


def find_violations(root, rels=None):
    """Every (path, line-number, line) that names a retired identifier in an operational file,
    plus ('<path>', 0, 'exists') for every retired path present. ``rels`` overrides the tracked
    file list (tests of this test)."""
    root = pathlib.Path(root)
    violations = []
    for rp in RETIRED_PATHS:
        if (root / rp).exists():
            violations.append((rp, 0, "retired path exists"))
    patches_dir = root / "roamux" / "patches"
    if patches_dir.is_dir():
        for p in sorted(patches_dir.glob("*.patch")):
            if p.name.startswith(RETIRED_PATCH_PREFIXES):
                violations.append((p.relative_to(root).as_posix(), 0, "retired patch exists"))
        readme = patches_dir / "README.md"
        if readme.exists():
            for n, line in enumerate(readme.read_text(errors="replace").splitlines(), 1):
                if line.startswith("| `0002-") or line.startswith("| `0003-"):
                    violations.append(("roamux/patches/README.md", n, line.strip()))
    for rel in (rels if rels is not None else tracked_files(root)):
        path = root / rel
        if path.resolve() == SELF or not path.is_file():
            continue
        if rel.startswith("roamux/patches/") and rel.endswith(".patch"):
            candidates = enumerate(patch_added_lines(path.read_text(errors="replace")), 1)
        elif is_operational(rel):
            candidates = enumerate(path.read_text(errors="replace").splitlines(), 1)
        else:
            continue
        for n, line in candidates:
            if IDENTIFIER_RE.search(line):
                violations.append((rel, n, line.strip()))
    return violations


class ChannelRetirementTest(unittest.TestCase):
    def test_channel_is_gone(self):
        violations = find_violations(REPO)
        self.assertEqual(violations, [],
                         "the chromium_src override channel is retired (roam-300, ADR 0004); "
                         "these operational files still reference it:\n  " +
                         "\n  ".join(f"{p}:{n}: {l}" for p, n, l in violations))

    def test_tier2_has_no_staleness_phase(self):
        code = (REPO / "roamux/build/ci/tier2_job.sh").read_text()
        self.assertNotIn("phase staleness\n", code)
        self.assertNotIn("check_override_staleness.py", code)


class ChannelRetirementOracleTest(unittest.TestCase):
    """Tests of the oracle itself, on temp trees."""

    def _tree(self, files):
        d = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(d, ignore_errors=True))
        for rel, text in files.items():
            p = d / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
        return d, sorted(files)

    def test_clean_tree_passes(self):
        d, rels = self._tree({"roamux/BUILD.gn": 'sources = [ "test/x.cc" ]\n',
                              "roamux/patches/0004-x.patch": "# preamble mentions roamux/chromium_src as history\n"
                                                             "--- a/f\n+++ b/f\n@@ -1 +1 @@\n+int x;\n",
                              "docs/adr/0004-x.md": "roamux/chromium_src is retired\n"})
        self.assertEqual(find_violations(d, rels), [])

    def test_dangling_build_gn_entry_fails(self):
        d, rels = self._tree({"roamux/BUILD.gn": 'sources = [ "test/roamux_overlay_mechanism_unittest.cc" ]\n'})
        self.assertTrue(any(p == "roamux/BUILD.gn" for p, _, _ in find_violations(d, rels)))

    def test_bare_include_dir_reference_fails(self):
        d, rels = self._tree({"roamux/build/args/x.gn": 'include_dirs = [ "//roamux/chromium_src" ]\n'})
        self.assertTrue(any(p == "roamux/build/args/x.gn" for p, _, _ in find_violations(d, rels)))

    def test_root_build_gn_and_shell_are_scanned(self):
        d, rels = self._tree({"BUILD.gn": 'include_dirs = [ "//roamux/chromium_src" ]\n',
                              "tools/run.sh": "python3 roamux/build/check_override_staleness.py\n",
                              "roamux/third_party/x/x.gn": 'x = "//roamux/chromium_src"\n'})
        hit = {p for p, _, _ in find_violations(d, rels)}
        self.assertEqual(hit, {"BUILD.gn", "tools/run.sh", "roamux/third_party/x/x.gn"})

    def test_marker_reintroduced_by_patch_fails(self):
        d, rels = self._tree({"roamux/patches/0099-x.patch":
                              "diff --git a/f b/f\n--- a/f\n+++ b/f\n@@ -1 +1,2 @@\n int y;\n+#define ROAMUX_SAMPLE_PATCH_ACTIVE 1\n"})
        self.assertTrue(any(p.endswith("0099-x.patch") for p, _, _ in find_violations(d, rels)))

    def test_marker_reintroduced_by_plain_unified_diff_fails(self):
        d, rels = self._tree({"roamux/patches/0098-x.patch":
                              "--- a/f\n+++ b/f\n@@ -1 +1,2 @@\n int y;\n+#define ROAMUX_CHROMIUM_SRC_OVERRIDE_ACTIVE 1\n"})
        self.assertTrue(any(p.endswith("0098-x.patch") for p, _, _ in find_violations(d, rels)))

    def test_retired_path_and_readme_row_fail(self):
        d, rels = self._tree({"roamux/chromium_src/README.md": "x\n",
                              "roamux/patches/README.md": "| `0002-chromium-src-include-redirect.patch` | persistent | x |\n"})
        v = find_violations(d, rels)
        self.assertTrue(any(p == "roamux/chromium_src" for p, _, _ in v))
        self.assertTrue(any(p == "roamux/patches/README.md" for p, _, _ in v))

    def test_docs_and_preambles_are_not_scanned(self):
        d, rels = self._tree({"docs/uprev.md": "check_override_staleness.py used to run here\n",
                              "roamux/patches/0005-x.patch": "# check_override_staleness was the roam-2 gate\n"
                                                             "--- a/f\n+++ b/f\n@@ -1 +1 @@\n+int x;\n"})
        self.assertEqual(find_violations(d, rels), [])

    def test_surviving_upstream_header_is_allowed(self):
        d, rels = self._tree({"roamux/test/x.cc": '#include "chrome/common/chrome_isolated_world_ids.h"\n'})
        self.assertEqual(find_violations(d, rels), [])


if __name__ == "__main__":
    unittest.main()
