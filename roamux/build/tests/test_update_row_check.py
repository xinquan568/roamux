# SPDX-License-Identifier: Apache-2.0
"""roam-160: the update-row patch surface + demolition honesty gate.

RED before patch 0040 exists (shape phase names the missing fix); GREEN once the patch ships and
no retired-card reference survives. Tier-1, pure Python.
"""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import check_update_row as c

_OVERLAY = pathlib.Path(__file__).resolve().parents[1].parent
_PATCHES = _OVERLAY / "patches"


def _patch_0040(extra_file=None, flagged=True, roamux_updater=True):
    lines = ["diff --git a/chrome/browser/ui/webui/help/version_updater_mac.mm"
             " b/chrome/browser/ui/webui/help/version_updater_mac.mm",
             "@@ -1,1 +1,9 @@"]
    if flagged:
        lines.append("+#if BUILDFLAG(ROAMUX_ENABLE_SPARKLE)")
    if roamux_updater:
        lines.append("+      return std::make_unique<roamux::updates::"
                     "RoamuxVersionUpdater>(service);")
    lines += ["diff --git a/chrome/browser/ui/webui/settings/about_handler.cc"
              " b/chrome/browser/ui/webui/settings/about_handler.cc",
              "@@ -1,1 +1,2 @@", "+  // handlers"]
    if extra_file:
        lines += ["diff --git a/%s b/%s" % (extra_file, extra_file),
                  "@@ -1,1 +1,1 @@", "+x"]
    return "\n".join(lines) + "\n"


class ShippedArtifactsTest(unittest.TestCase):
    def test_shipped_shape_passes(self):
        """RED until 0040 ships; then the real patch set must satisfy shape."""
        self.assertEqual([], c.check_shape(_PATCHES))

    def test_shipped_demolition_clean(self):
        """No live reference to the retired card artifacts in the overlay."""
        self.assertEqual([], c.check_demolition(_OVERLAY))


class ShapeRejectionTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.patches = pathlib.Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def _write(self, name, text):
        (self.patches / name).write_text(text)

    def test_missing_0040_fails(self):
        self.assertTrue(any("missing" in f
                            for f in c.check_shape(self.patches)))

    def test_valid_0040_passes(self):
        self._write(c.P0040, _patch_0040())
        self.assertEqual([], c.check_shape(self.patches))

    def test_extra_file_fails(self):
        self._write(c.P0040, _patch_0040(extra_file="chrome/other.cc"))
        self.assertTrue(any("beyond the declared surface" in f
                            for f in c.check_shape(self.patches)))

    def test_unflagged_interposition_fails(self):
        self._write(c.P0040, _patch_0040(flagged=False))
        self.assertTrue(any("not flag-guarded" in f
                            for f in c.check_shape(self.patches)))

    def test_support_google_in_0033_fails(self):
        self._write(c.P0040, _patch_0040())
        self._write(c.P0033,
                    "diff --git a/x.ts b/x.ts\n@@ -1,1 +1,1 @@\n"
                    "+  href=\"https://support.google.com/chrome\"\n")
        self.assertTrue(any("support.google.com" in f
                            for f in c.check_shape(self.patches)))

    def test_stale_retired_patch_fails(self):
        self._write(c.P0040, _patch_0040())
        self._write("0032-roamux-about-mojo-binder.patch", "diff --git a/x b/x\n")
        self.assertTrue(any("retired patch still present" in f
                            for f in c.check_shape(self.patches)))


class DemolitionScanTest(unittest.TestCase):
    def test_live_reference_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            overlay = pathlib.Path(tmp)
            (overlay / "some.gn").write_text(
                'sources = [ "roamux_update_card.ts" ]\n')
            self.assertTrue(any("roamux_update_card" in f
                                for f in c.check_demolition(overlay)))

    def test_comment_mention_is_exempt(self):
        with tempfile.TemporaryDirectory() as tmp:
            overlay = pathlib.Path(tmp)
            (overlay / "some.gn").write_text(
                "# roam-160 retired the roamux_update_card element\n")
            self.assertEqual([], c.check_demolition(overlay))


if __name__ == "__main__":
    unittest.main()
