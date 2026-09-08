# SPDX-License-Identifier: Apache-2.0
"""roam-287: the About page hides "Try again" by comparing the status message's
FIRST LINE against strings registered in settings_ui.cc (patch 0033) — the
signature-failure copy (roam-160) and the dead-updater copy (roam-287). The
adapter (roamux_version_updater.cc) emits those first lines. If either side
drifts by a character the button silently reappears, so this pins the two
literals equal, and pins that the page's predicate references both keys."""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
PATCH = ROOT / "roamux" / "patches" / "0033-settings-about-roamux.patch"
ADAPTER = ROOT / "roamux" / "browser" / "updates" / "roamux_version_updater.cc"

KEYS = {
    "roamuxUpdateErrSignature": "kSignatureFailedCopy",
    "roamuxUpdateErrUnavailable": "kUpdaterUnavailableCopy",
}


def _patch_string(text, key):
    """The C++ string literal(s) passed to AddString("<key>", ...) in the patch,
    concatenated (the source splits long literals across lines)."""
    m = re.search(r'AddString\("' + re.escape(key) + r'",\s*((?:\+?\s*"(?:[^"\\]|\\.)*"\s*)+)\)', text)
    if not m:
        return None
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))


def _adapter_string(text, const):
    m = re.search(r"char16_t " + re.escape(const) + r"\[\]\s*=\s*((?:u\"(?:[^\"\\]|\\.)*\"\s*)+);", text)
    if not m:
        return None
    return "".join(re.findall(r'u"((?:[^"\\]|\\.)*)"', m.group(1)))


class AboutCopySyncTest(unittest.TestCase):
    def setUp(self):
        self.patch = PATCH.read_text()
        self.adapter = ADAPTER.read_text()

    def test_no_retry_copies_match_between_page_strings_and_adapter(self):
        for key, const in KEYS.items():
            with self.subTest(key=key):
                page = _patch_string(self.patch, key)
                emitted = _adapter_string(self.adapter, const)
                self.assertIsNotNone(page, f"{key} is not registered in patch 0033")
                self.assertIsNotNone(emitted, f"{const} is not defined in the adapter")
                self.assertEqual(page, emitted, f"{key} must equal the adapter's {const} verbatim")

    def test_page_predicate_references_both_no_retry_keys(self):
        # The TS hunk: showRoamuxTryAgain_ is false when plainCopy equals ANY
        # registered no-retry copy.
        for key in KEYS:
            with self.subTest(key=key):
                self.assertTrue(f"loadTimeData.getString('{key}')" in self.patch,
                                f"the about_page.ts predicate does not reference {key}")


if __name__ == "__main__":
    unittest.main()
