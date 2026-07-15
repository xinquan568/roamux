# SPDX-License-Identifier: Apache-2.0
"""roam-140: chrome://roamux-about is retired; its update surface moved onto
chrome://settings/help.

Two invariants guard the migration, both checked hermetically against the
overlay source/patch text (this test never builds):

  1. chrome://roamux-about is NOT registered anywhere — the standalone WebUI
     host source is deleted and no patch re-registers its WebUIConfig
     (RoamuxAboutUIConfig, formerly added by the now-removed patch 0027).

  2. The settings WebUI host binds roamux::mojom::UpdatePageHandlerFactory —
     the Mojo binder that used to dispatch to RoamuxAboutUI now dispatches to
     SettingsUI (repointed patch 0032 + the SettingsUI::BindInterface overload).
     See the TODO on test_settings_host_binds_update_page_handler_factory.

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
    """Invariant 2: the settings host binds the update-page factory (roam-140)."""

    # roam-140: patch 0032 now repoints the UpdatePageHandlerFactory binder to
    # settings::SettingsUI (and 0033 lands the gated SettingsUI::BindInterface
    # overload), so this invariant is live.
    def test_settings_host_binds_update_page_handler_factory(self):
        binder = "\n".join(
            text for text in _patch_texts().values()
            if "RegisterWebUIControllerInterfaceBinder" in text
            and "UpdatePageHandlerFactory" in text
        )
        self.assertIn(
            "SettingsUI", binder,
            "the UpdatePageHandlerFactory Mojo binder must dispatch to "
            "SettingsUI now that chrome://settings/help hosts the update card",
        )
        self.assertNotIn(
            "RoamuxAboutUI", binder,
            "the UpdatePageHandlerFactory binder must no longer name the "
            "retired RoamuxAboutUI (roam-140)",
        )


if __name__ == "__main__":
    unittest.main()
