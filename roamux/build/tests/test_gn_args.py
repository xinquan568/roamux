# SPDX-License-Identifier: Apache-2.0
"""Pinning invariants on the GN args templates (roam-238, roam-241).

The shipped release build must carry proprietary codecs (H.264/AAC): roam-238 traced a
"Microsoft Stream can't play this video" report to release.gn never setting
proprietary_codecs/ffmpeg_branding, so both took their unbranded-build defaults and the
packaged app rejected avc1/mp4a at the mime layer AND lacked the FFmpeg decoders.
reference.gn carries the same pair so dev/CI builds share the release flag reality
(the roam-241 dev-follows-shipped posture).

Shipped builds must also disable the fieldtrial testing config (roam-241): unbranded
builds compile it in by default (the gate ignores is_official_build), so users would
run a pin-varying experiment soup — the masking that hid the E1 gate-mismatch family
(roam-234/roam-239). Decision + inventory recorded on roam-241 and in ADR 0002; the
single-arg pin per file is the revert point.

These are TEXT pins, not GN semantics: they guarantee the assignments stay present,
unique, and uncommented in the templates. GN-level validation (arg names known, ffmpeg
"Chrome" branding config resolving per arch) is the release-config `gn gen` canary run
when the args change — see the roam-238 PR. A deliberate reformat of these files should
update this test alongside.

Also pinned: neither template may document a `tr`-newline-flatten invocation. A leading
full-line comment (the SPDX header) makes a flattened args string one giant comment —
the roam-114 failure class (release.gn learned this; reference.gn's usage comment was
fixed under roam-238 to the BOOTSTRAP.md copy workflow).
"""

import pathlib
import re
import unittest

ARGS_DIR = pathlib.Path(__file__).resolve().parent.parent / "args"

PINNED = (
    ("proprietary_codecs", "true"),
    ("ffmpeg_branding", '"Chrome"'),
    ("disable_fieldtrial_testing_config", "true"),
)


def _active_assignments(text, name):
    """Uncommented `name = value` lines (leading whitespace tolerated)."""
    hits = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        m = re.match(rf"{name}\s*=\s*(.+?)(?:\s+#.*)?$", stripped)
        if m:
            hits.append(m.group(1).strip())
    return hits


class GnArgsPinsTest(unittest.TestCase):
    def _pin(self, filename):
        path = ARGS_DIR / filename
        self.assertTrue(path.exists(), f"missing {path}")
        text = path.read_text()
        for name, want in PINNED:
            values = _active_assignments(text, name)
            self.assertEqual(
                len(values), 1,
                f"{filename}: expected exactly one active `{name}` assignment, found "
                f"{len(values)} ({values!r})")
            self.assertEqual(
                values[0], want,
                f"{filename}: `{name}` must be {want}, found {values[0]!r}")
        return text

    def test_release_gn_pins(self):
        self._pin("release.gn")

    def test_reference_gn_pins(self):
        self._pin("reference.gn")

    def test_no_documented_tr_flatten(self):
        # The documented invocation must never be the newline-flatten (roam-114):
        # the SPDX header line would swallow the whole flattened args string.
        for filename in ("release.gn", "reference.gn"):
            text = (ARGS_DIR / filename).read_text()
            self.assertNotIn(
                "tr '\\n'", text,
                f"{filename}: documents a tr-newline-flatten invocation (roam-114 "
                "failure class); document the copy workflow instead (BOOTSTRAP.md)")


if __name__ == "__main__":
    unittest.main()
