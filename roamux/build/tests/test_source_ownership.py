# SPDX-License-Identifier: Apache-2.0
"""One-owner-per-compiled-source check (roam-300 prerequisite; the minimal form of grill H13).

Contract
--------
A *compiled source* is every tracked ``roamux/**/*.cc`` / ``*.mm`` outside ``roamux/third_party/``.
An *owner* is either

  (i)  a string entry inside a ``sources = [ ... ]`` / ``sources += [ ... ]`` list in any
       ``roamux/**/BUILD.gn`` (nested files and ``test/support`` included), normalised to a
       repo-relative path (``//roamux/...`` or relative to that BUILD.gn's directory), or
  (ii) a patch-added list-entry line in ``roamux/patches/*.patch`` of the form
       ``+<indent>"//roamux/<path>.cc",`` (any indentation, optional trailing comma, nothing else
       on the line) — the "compiled into an upstream target via ``sources +=``" channel.

Comments, ``inputs``, ``cc_test_files``, ``deps``, ``.h`` files and patch preambles are not
owners. Every compiled source must have exactly one owner, and every owner must name a file that
exists. Known double-owners live in REGISTER with a citation and are pinned to their EXACT owners:
a vanished or third owner fails, and a register entry whose file is gone fails.

Owner paths are canonicalised (``..`` and ``.`` segments resolved) before counting, and owner
multiplicity counts: the same file listed twice by one BUILD.gn is two owners. Patch owners are
taken from hunk content only (after the first ``@@``), never from a preamble.

Documented residual: a ``+``-line of the owner shape inside a non-``sources`` list in a patch hunk
would count as an owner. None exists today.
"""

import pathlib
import posixpath
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]

# path -> (exact owners as a list — identity and multiplicity — , citation)
REGISTER = {
    "roamux/browser/updates/roamux_version_updater.cc": (
        ["roamux/BUILD.gn", "0033-settings-about-roamux.patch"],
        "grill 2026-08-26 M25: listed by roamux_browser_unittests AND compiled into //chrome/browser/ui "
        "by patch 0033 (ODR papered over with nogncheck); drained by the H13 umbrella (#297), not here"),
}

SOURCES_LIST_RE = re.compile(r"\bsources\s*\+?=\s*\[(.*?)\]", re.S)
STRING_RE = re.compile(r'"([^"]+\.(?:cc|mm))"')
PATCH_OWNER_RE = re.compile(r'^\+\s*"//(roamux/[^"]+\.(?:cc|mm))",?\s*$')


def tracked(root):
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True,
                             capture_output=True).stdout
        return [p.decode() for p in out.split(b"\0") if p]
    except (subprocess.CalledProcessError, FileNotFoundError):
        return [p.relative_to(root).as_posix() for p in sorted(root.rglob("*"))
                if p.is_file() and ".git" not in p.parts]


def _strip_gn_comments(text):
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def build_gn_owners(root, rel_build_gn):
    """(owner-name, repo-relative source path) for every sources entry of one BUILD.gn."""
    text = _strip_gn_comments((root / rel_build_gn).read_text(errors="replace"))
    base = pathlib.PurePosixPath(rel_build_gn).parent
    out = []
    for block in SOURCES_LIST_RE.findall(text):
        for s in STRING_RE.findall(block):
            path = s[2:] if s.startswith("//") else (base / s).as_posix()
            out.append((rel_build_gn, posixpath.normpath(path)))
    return out


def patch_owners(root, rel_patch):
    lines = (root / rel_patch).read_text(errors="replace").splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith("@@"))
    except StopIteration:
        return []
    return [(pathlib.PurePosixPath(rel_patch).name, posixpath.normpath(m.group(1)))
            for line in lines[start:] for m in [PATCH_OWNER_RE.match(line)] if m]


def collect(root, rels=None):
    """Returns (compiled_sources, owners) where owners maps source path -> sorted owner names."""
    root = pathlib.Path(root)
    rels = rels if rels is not None else tracked(root)
    sources = sorted(r for r in rels if r.startswith("roamux/") and r.endswith((".cc", ".mm"))
                     and not r.startswith("roamux/third_party/"))
    owners = {}
    for r in rels:
        pairs = []
        if r.startswith("roamux/") and r.endswith("BUILD.gn"):
            pairs = build_gn_owners(root, r)
        elif r.startswith("roamux/patches/") and r.endswith(".patch"):
            pairs = patch_owners(root, r)
        for owner, path in pairs:
            owners.setdefault(path, []).append(owner)
    return sources, {k: sorted(v) for k, v in owners.items()}


def violations(root, rels=None, register=None):
    register = REGISTER if register is None else register
    root = pathlib.Path(root)
    sources, owners = collect(root, rels)
    out = []
    for s in sources:
        o = owners.get(s, [])
        if s in register:
            want, cite = register[s]
            # identity AND multiplicity: a second entry in an already-registered owner file fails too
            if sorted(o) != sorted(want):
                out.append(f"{s}: registered owners {sorted(want)} but found {o} ({cite})")
        elif len(o) != 1:
            out.append(f"{s}: {len(o)} owner(s) {o} — expected exactly one")
    for path, o in owners.items():
        if not (root / path).is_file():
            out.append(f"{path}: named by {o} but does not exist")
    for s in register:
        if not (root / s).is_file():
            out.append(f"{s}: in the exception register but does not exist — drop the entry")
    return out


class SourceOwnershipTest(unittest.TestCase):
    def test_every_compiled_source_has_exactly_one_owner(self):
        v = violations(REPO)
        self.assertEqual(v, [], "one-owner-per-compiled-source (roam-300 / H13 minimal):\n  " + "\n  ".join(v))


class SourceOwnershipCheckTest(unittest.TestCase):
    """Tests of the check itself, on temp trees (no register unless given)."""

    def _tree(self, files):
        d = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(lambda: shutil.rmtree(d, ignore_errors=True))
        for rel, text in files.items():
            p = d / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
        return d, sorted(files)

    def test_single_owner_passes_both_channels(self):
        d, rels = self._tree({
            "roamux/BUILD.gn": 'test("t") {\n  sources = [\n    "test/a.cc",\n    "test/support/s.cc",\n    "test/support/s.h",\n  ]\n}\n',
            "roamux/test/a.cc": "", "roamux/test/support/s.cc": "", "roamux/test/support/s.h": "",
            "roamux/browser/b.mm": "",
            "roamux/patches/0010-x.patch": "# preamble\n--- a/x/BUILD.gn\n+++ b/x/BUILD.gn\n@@ -1 +1,3 @@\n sources += [\n+      \"//roamux/browser/b.mm\",\n ]\n",
        })
        self.assertEqual(violations(d, rels, register={}), [])

    def test_nested_build_gn_relative_paths(self):
        d, rels = self._tree({"roamux/app/x/BUILD.gn": 'source_set("x") {\n  sources = [ "impl.cc" ]\n}\n',
                              "roamux/app/x/impl.cc": ""})
        self.assertEqual(violations(d, rels, register={}), [])

    def test_parent_relative_owner_is_canonicalised(self):
        d, rels = self._tree({"roamux/app/x/BUILD.gn": 'sources = [ "../y/impl.cc" ]\n', "roamux/app/y/impl.cc": ""})
        self.assertEqual(violations(d, rels, register={}), [])

    def test_differently_spelled_double_owner_fails(self):
        d, rels = self._tree({"roamux/app/x/BUILD.gn": 'sources = [ "../y/impl.cc" ]\n', "roamux/app/y/impl.cc": "",
                              "roamux/BUILD.gn": 'sources = [ "//roamux/app/y/impl.cc" ]\n'})
        self.assertTrue(any("impl.cc" in v and "2 owner" in v for v in violations(d, rels, register={})))

    def test_preamble_source_reference_is_not_an_owner(self):
        d, rels = self._tree({"roamux/test/a.cc": "",
                              "roamux/patches/0010-x.patch": "+      \"//roamux/test/a.cc\",\n--- a/f\n+++ b/f\n@@ -1 +1 @@\n+int x;\n"})
        self.assertTrue(any("a.cc" in v and "0 owner" in v for v in violations(d, rels, register={})))

    def test_register_counts_multiplicity(self):
        reg = {"roamux/test/a.cc": (["roamux/BUILD.gn", "0010-x.patch"], "cite")}
        d, rels = self._tree({"roamux/BUILD.gn": 'sources = [ "test/a.cc" ]\nsources += [ "test/a.cc" ]\n', "roamux/test/a.cc": "",
                              "roamux/patches/0010-x.patch": "--- a/f\n+++ b/f\n@@ -1 +1 @@\n+    \"//roamux/test/a.cc\",\n"})
        self.assertTrue(any("registered owners" in v for v in violations(d, rels, register=reg)))

    def test_orphan_fails(self):
        d, rels = self._tree({"roamux/test/orphan.cc": ""})
        self.assertTrue(any("orphan.cc" in v and "0 owner" in v for v in violations(d, rels, register={})))

    def test_double_owner_fails(self):
        d, rels = self._tree({"roamux/BUILD.gn": 'sources = [ "test/a.cc" ]\n', "roamux/test/a.cc": "",
                              "roamux/patches/0010-x.patch": "--- a/f\n+++ b/f\n@@ -1 +1 @@\n+    \"//roamux/test/a.cc\",\n"})
        self.assertTrue(any("a.cc" in v and "2 owner" in v for v in violations(d, rels, register={})))

    def test_dangling_owner_fails(self):
        d, rels = self._tree({"roamux/BUILD.gn": 'sources = [ "test/gone.cc" ]\n'})
        self.assertTrue(any("gone.cc" in v and "does not exist" in v for v in violations(d, rels, register={})))

    def test_comment_inputs_and_cc_test_files_are_not_owners(self):
        d, rels = self._tree({
            "roamux/BUILD.gn": '# sources = [ "test/a.cc" ]\ninputs = [ "test/a.cc" ]\ncc_test_files = [ "test/a.cc" ]\n'
                               'sources = [\n  # "test/b.cc" is commented out\n  "test/a.cc",\n]\n',
            "roamux/test/a.cc": "",
            "roamux/patches/0010-x.patch": "--- a/f\n+++ b/f\n@@ -1 +1,2 @@\n+  # \"//roamux/test/a.cc\" mentioned in a comment\n+  foo(\"//roamux/test/a.cc\")\n",
        })
        self.assertEqual(violations(d, rels, register={}), [])

    def test_patch_owner_accepts_any_indentation(self):
        for indent in ("    ", "      ", "        "):
            d, rels = self._tree({"roamux/browser/b.cc": "",
                                  "roamux/patches/0010-x.patch": f"--- a/f\n+++ b/f\n@@ -1 +1 @@\n+{indent}\"//roamux/browser/b.cc\",\n"})
            self.assertEqual(violations(d, rels, register={}), [], indent)

    def test_register_pins_exact_owners(self):
        reg = {"roamux/test/a.cc": (["roamux/BUILD.gn", "0010-x.patch"], "cite")}
        files = {"roamux/BUILD.gn": 'sources = [ "test/a.cc" ]\n', "roamux/test/a.cc": "",
                 "roamux/patches/0010-x.patch": "--- a/f\n+++ b/f\n@@ -1 +1 @@\n+    \"//roamux/test/a.cc\",\n"}
        d, rels = self._tree(files)
        self.assertEqual(violations(d, rels, register=reg), [])
        # a third owner is not covered by the register
        files["roamux/patches/0011-y.patch"] = "--- a/f\n+++ b/f\n@@ -1 +1 @@\n+    \"//roamux/test/a.cc\",\n"
        d, rels = self._tree(files)
        self.assertTrue(any("registered owners" in v for v in violations(d, rels, register=reg)))
        # a vanished owner is not covered either
        del files["roamux/patches/0011-y.patch"]; del files["roamux/patches/0010-x.patch"]
        d, rels = self._tree(files)
        self.assertTrue(any("registered owners" in v for v in violations(d, rels, register=reg)))

    def test_stale_register_entry_fails(self):
        d, rels = self._tree({"roamux/BUILD.gn": "sources = []\n"})
        reg = {"roamux/test/gone.cc": (["roamux/BUILD.gn"], "cite")}
        self.assertTrue(any("exception register" in v for v in violations(d, rels, register=reg)))


if __name__ == "__main__":
    unittest.main()
