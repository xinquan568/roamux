# SPDX-License-Identifier: Apache-2.0
"""roam-140: chrome://roamux-about is retired; its update surface moved onto
chrome://settings/help.

Two invariants guard the migration, both checked hermetically against the
overlay source/patch text (this test never builds):

  1. chrome://roamux-about is NOT registered anywhere — the standalone WebUI
     host source is deleted and no patch re-registers its WebUIConfig
     (RoamuxAboutUIConfig, formerly added by the now-removed patch 0027).

  2. roam-160 retired the Mojo update surface entirely (the native About row
     rides VersionUpdater now): NO patch may bind or register
     UpdatePageHandlerFactory on ANY path — a partial resurrection of the
     binder is exactly the half-registered shape that killed the renderer in
     roam-152.

roam-128 history (retired with the page): the standalone host had to use the
regular DefaultWebUIConfig (never DefaultInternalWebUIConfig) so it was not
hidden behind chrome://chrome-urls' "internal debug pages" toggle. That
invariant no longer applies once the page is gone.
"""

import pathlib
import unittest

ROAMUX_ROOT = pathlib.Path(__file__).resolve().parents[2]
PATCHES_DIR = ROAMUX_ROOT / "patches"
ABOUT_WEBUI_DIR = ROAMUX_ROOT / "browser" / "ui" / "webui" / "about"


def _patch_texts():
    return {p.name: p.read_text() for p in sorted(PATCHES_DIR.glob("*.patch"))}


class RoamuxAboutRetiredTest(unittest.TestCase):
    """Invariant 1: chrome://roamux-about is fully unregistered (roam-140)."""

    def test_about_webui_host_source_is_deleted(self):
        self.assertFalse(
            ABOUT_WEBUI_DIR.exists(),
            "the standalone chrome://roamux-about WebUI host was retired "
            "(roam-140); its update surface now lives on chrome://settings/help",
        )

    def test_no_patch_registers_the_about_webui_config(self):
        # Patch 0027 used to AddWebUIConfig(RoamuxAboutUIConfig) for
        # chrome://roamux-about. It is deleted; no patch may re-introduce that
        # registration. (This deliberately matches the *Config* registration,
        # not the RoamuxAboutUI binder class in patch 0032, which the settings
        # subagent repoints in invariant 2 below.)
        offenders = [
            name for name, text in _patch_texts().items()
            if "RoamuxAboutUIConfig" in text
        ]
        self.assertEqual(
            [], offenders,
            "chrome://roamux-about is retired (roam-140) but these patches "
            "still register its WebUIConfig: {}".format(offenders),
        )


class SettingsUpdateBinderTest(unittest.TestCase):
    """Invariant 2 (roam-160): the Mojo update surface is fully retired."""

    # roam-140/152 bound UpdatePageHandlerFactory on SettingsUI's per-WebUI
    # broker for the <roamux-update-card>. roam-160 deleted the card AND the
    # mojom; updates ride VersionUpdater/AboutHandler messages instead. A
    # LINGERING binder registration (either the broker path or the legacy
    # frame map) would reference a deleted interface — refuse both.
    def test_no_patch_binds_the_retired_update_page_factory(self):
        offenders = [
            name for name, text in _patch_texts().items()
            if "UpdatePageHandlerFactory" in text
        ]
        self.assertEqual(
            [], offenders,
            "roam-160 retired the update-page Mojo surface, but these patches "
            "still reference UpdatePageHandlerFactory: {}".format(offenders),
        )


if __name__ == "__main__":
    unittest.main()
