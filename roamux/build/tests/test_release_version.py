# SPDX-License-Identifier: Apache-2.0
"""roam-141: tag → Sparkle-orderable CFBundleVersion mapping.

Pins the encoding AND the ordering property that actually matters — the encoded
versions must sort the way releases progress, because Sparkle's comparator only
understands numeric dotted versions.
"""

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import release_version as rv


class BundleVersionTest(unittest.TestCase):
    def test_alpha_encoding(self):
        self.assertEqual(rv.bundle_version("v0.0.1-alpha.3"), "0.0.1.1.3")

    def test_leading_v_optional(self):
        self.assertEqual(rv.bundle_version("0.0.1-alpha.3"), "0.0.1.1.3")

    def test_stages_map_to_distinct_numbers(self):
        self.assertEqual(rv.bundle_version("v1.2.3-beta.4"), "1.2.3.2.4")
        self.assertEqual(rv.bundle_version("v1.2.3-rc.4"), "1.2.3.3.4")

    def test_final_release_sorts_above_prereleases(self):
        self.assertEqual(rv.bundle_version("v0.0.1"), "0.0.1.9.0")

    def test_bad_tag_rejected(self):
        for bad in ["1.0", "v0.0.1-gamma.1", "v0.0.1-alpha", "nope"]:
            with self.assertRaises(ValueError, msg=bad):
                rv.bundle_version(bad)

    def test_short_version_is_human(self):
        self.assertEqual(rv.short_version("v0.0.1-alpha.3"), "0.0.1-alpha.3")
        self.assertEqual(rv.short_version("v0.0.1"), "0.0.1")


def _key(bundle):
    return [int(p) for p in bundle.split(".")]


class OrderingPropertyTest(unittest.TestCase):
    # The releases in ascending order; encoded keys must be strictly increasing
    # (this is exactly what Sparkle's numeric comparator will see).
    ASCENDING = [
        "v0.0.1-alpha.2", "v0.0.1-alpha.3", "v0.0.1-alpha.9", "v0.0.1-alpha.10",
        "v0.0.1-beta.1", "v0.0.1-rc.1", "v0.0.1", "v0.0.2-alpha.1", "v0.1.0-alpha.1",
    ]

    def test_encoded_keys_are_strictly_increasing(self):
        keys = [_key(rv.bundle_version(t)) for t in self.ASCENDING]
        for a, b, ta, tb in zip(keys, keys[1:], self.ASCENDING, self.ASCENDING[1:]):
            self.assertLess(a, b, f"{ta} ({a}) must sort below {tb} ({b})")


if __name__ == "__main__":
    unittest.main()
