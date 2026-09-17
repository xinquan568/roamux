# SPDX-License-Identifier: Apache-2.0
"""Hermetic inventory-completeness check (roam-78): every declared patch under
roamux/patches/ must carry a row in the README inventory (the repo-local
mirror of the plan §12.2 upstream-hook inventory). Name-presence only — row
content stays a human review surface, not test churn."""

import pathlib
import unittest

PATCHES_DIR = pathlib.Path(__file__).resolve().parent.parent.parent / "patches"


class PatchInventoryTest(unittest.TestCase):
    def test_every_patch_has_a_readme_inventory_row(self):
        readme = (PATCHES_DIR / "README.md").read_text()
        missing = [p.name for p in sorted(PATCHES_DIR.glob("*.patch"))
                   if p.name not in readme]
        self.assertEqual(missing, [],
                         f"patches without a README inventory row: {missing}")


# roam-340: the three chrome://flags files are owned by exactly ONE patch, so an upstream change in
# that region conflicts with one patch instead of eleven, and every future Roamux flag row is added
# in that one patch (one file to edit, one alphabetical slot to respect — AboutFlagsTest, local only,
# enforces the order). Both header forms count (`--- a/` and `+++ b/`, plain unified-diff sections
# as in 0024 and `diff --git` sections alike), so a deletion or a rename of the file counts as touching it.
FLAG_FILES = (
    "chrome/browser/about_flags.cc",
    "chrome/browser/flag-metadata.json",
    "chrome/browser/flag-never-expire-list.json",
)
FLAGS_OWNER = "0073-roamux-flags-entries.patch"


def _paths_touched(patch_text):
    paths = set()
    for line in patch_text.splitlines():
        for prefix in ("--- a/", "+++ b/"):
            if line.startswith(prefix):
                paths.add(line[len(prefix):].split("\t")[0].strip())
    return paths


class FlagFilesSingleOwnerTest(unittest.TestCase):
    def test_each_flag_file_has_exactly_one_owner(self):
        for flag_file in FLAG_FILES:
            owners = sorted(p.name for p in PATCHES_DIR.glob("*.patch")
                            if flag_file in _paths_touched(p.read_text()))
            self.assertEqual(
                owners, [FLAGS_OWNER],
                f"{flag_file} must be touched by {FLAGS_OWNER} only (roam-340) — add a new Roamux "
                f"chrome://flags row to that patch, in global alphabetical order; owners now: {owners}")


if __name__ == "__main__":
    unittest.main()
