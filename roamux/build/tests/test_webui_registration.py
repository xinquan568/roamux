# SPDX-License-Identifier: Apache-2.0
"""roam-128: user-facing WebUI pages must never be registered as internal.

content::DefaultInternalWebUIConfig hides a page behind chrome://chrome-urls'
"internal debug pages" toggle — shipping the About/update surface that way put
an interstitial in front of the roam-90 live update E2E. Tripwire: the About
WebUI header must use the regular DefaultWebUIConfig.
"""

import pathlib
import unittest

ABOUT_HEADER = (pathlib.Path(__file__).resolve().parents[2]
                / "browser" / "ui" / "webui" / "about" / "roamux_about_ui.h")


class WebUIRegistrationTest(unittest.TestCase):
    def test_about_page_is_not_internal(self):
        text = ABOUT_HEADER.read_text()
        self.assertNotIn("DefaultInternalWebUIConfig", text,
                         "the About/update page is user-facing — the internal "
                         "config gates it behind a debug toggle (roam-128)")
        self.assertIn("DefaultWebUIConfig", text,
                      "the About page must register as a regular WebUI")


if __name__ == "__main__":
    unittest.main()
