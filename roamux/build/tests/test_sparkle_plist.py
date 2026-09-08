# SPDX-License-Identifier: Apache-2.0
"""roam-289 (grill M17, plist half): the Sparkle keys merged into the app's
Info.plist. `SUVerifyUpdateBeforeExtraction` must be the plist BOOLEAN true —
Sparkle 2 defaults it to NO and then runs the zip/dmg parsers on
unauthenticated bytes; the key needs EdDSA-signed enclosures (K3) and Sparkle
≥ 2.7.3 (vendored 2.9.4). A `<string>YES</string>` would be silently ignored,
hence the type check. check_sparkle_bundle.py (the manual flag-on smoke gate;
no CI caller) enforces the same on the merged bundle — this pins the source
plist for CI and pins that the bundle script still names the key."""

import base64
import pathlib
import plistlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
PLIST = ROOT / "roamux" / "app" / "sparkle-Info.plist"
BUNDLE_CHECK = ROOT / "roamux" / "build" / "check_sparkle_bundle.py"


class SparklePlistTest(unittest.TestCase):
    def setUp(self):
        with open(PLIST, "rb") as f:
            self.plist = plistlib.load(f)

    def test_verify_update_before_extraction_is_boolean_true(self):
        value = self.plist.get("SUVerifyUpdateBeforeExtraction")
        self.assertIs(value, True,
                      "SUVerifyUpdateBeforeExtraction must be <true/> (a plist boolean) — "
                      f"got {value!r}")

    def test_existing_keys_keep_their_contract(self):
        self.assertIs(self.plist.get("SUEnableAutomaticChecks"), True)
        self.assertEqual(86400, self.plist.get("SUScheduledCheckInterval"))
        self.assertTrue(self.plist["SUFeedURL"].endswith("/releases/latest/download/appcast.xml"))
        self.assertEqual(32, len(base64.b64decode(self.plist["SUPublicEDKey"], validate=True)))

    def test_bundle_check_names_the_key(self):
        # Structural coupling only: the manual gate must keep asserting the key
        # (its boolean enforcement is proven by running it, not here).
        self.assertRegex(BUNDLE_CHECK.read_text(),
                         re.compile(r'SUVerifyUpdateBeforeExtraction'))


if __name__ == "__main__":
    unittest.main()
