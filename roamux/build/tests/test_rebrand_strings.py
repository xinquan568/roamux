# SPDX-License-Identifier: Apache-2.0
"""Hermetic tests for the Roamux string-rebrand channel (roam-132).

Two tiers:
  * Pure tests (always run) — the guarded substitution, grd/xtb structure
    preservation, exclusions and idempotency, driven by small INLINE fixture
    strings written to a tmpdir (never the real 40 MB grd/xtb files).
  * GRIT-bound tests (skipped when grit can't be imported, mirroring
    pre_push.py's honest gtest gate) — the XTB id re-keying that keeps
    translations bound. Ids are computed through GRIT's own path
    (tclib.GenerateMessageId); the STRONGEST cases reload the post-pass grd+xtb
    with grd_reader and assert the rebranded message resolves to the TRANSLATED
    string, not the English/pseudo fallback.

Run:
  ROAMUX_CHROMIUM_SRC=~/chromium/src \
    python3 -m unittest -v roamux.build.tests.test_rebrand_strings
  # or:  python3 -m unittest discover -s roamux/build/tests
"""

import os
import pathlib
import sys
import tempfile
import unittest
from xml.dom import minidom

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import rebrand_strings as rb
import rebrand_exclusions as excl


def _resolve_chromium_src():
    src = os.environ.get("ROAMUX_CHROMIUM_SRC") or os.path.expanduser("~/chromium/src")
    return pathlib.Path(src)


CHROMIUM_SRC = _resolve_chromium_src()


def _grit_skip_reason():
    """Return None when GRIT is importable, else a human reason (honest skip)."""
    if not CHROMIUM_SRC.is_dir():
        return f"no Chromium checkout at {CHROMIUM_SRC} (set ROAMUX_CHROMIUM_SRC)"
    try:
        rb._import_grit(CHROMIUM_SRC)
    except Exception as e:  # noqa: BLE001 — any import failure is an honest skip
        return f"grit not importable from {CHROMIUM_SRC}: {e}"
    return None


GRIT_SKIP = _grit_skip_reason()

# roam-132 review: the GRIT-bound binding tests SKIP on tier-1 CI (no Chromium
# checkout). But that is where the load-bearing xtb-binding assertions live, so on
# a runner that HAS the checkout the CI step sets REQUIRE_GRIT=1 to turn the skip
# into a HARD RUN: binding tests execute (and error loudly if grit is somehow
# absent) instead of silently skipping. Tier-1 behaviour (skip) is unchanged.
REQUIRE_GRIT = os.environ.get("REQUIRE_GRIT") == "1"
_SKIP_BINDING = bool(GRIT_SKIP) and not REQUIRE_GRIT


# ---------------------------------------------------------------------------
# Pure: the guarded substitution + exclusions + idempotency (no GRIT needed).
# ---------------------------------------------------------------------------
class GuardedSubstitutionTest(unittest.TestCase):
    def sub(self, text):
        new, _ = rb.rebrand_text(text)
        return new

    def test_product_mentions_rebrand(self):
        self.assertEqual(self.sub("About Chromium"), "About Roamux")
        self.assertEqual(self.sub("Quit Chromium"), "Quit Roamux")
        self.assertEqual(self.sub("Welcome to Chromium; new window"),
                         "Welcome to Roamux; new window")
        # Possessive / punctuation-adjacent still rebrands.
        self.assertEqual(self.sub("You reached Chromium’s limit"),
                         "You reached Roamux’s limit")
        # Lowercase user-visible mention.
        self.assertEqual(self.sub("the chromium browser"), "the roamux browser")

    def test_exclusions_stay_unchanged(self):
        # Exclusion == byte-identical (the Chromium/chromium token survives at its
        # original casing: attribution, ChromeOS, domains, URLs, code/histogram ids).
        for kept in (
            "Copyright 2016 The Chromium Authors. All rights reserved.",
            "Visit https://www.chromium.org for details",
            "chromium.org",
            "org.chromium.chrome.Foo",               # code identifier
            "Recorded under Chromium.Startup.Warm",  # histogram name
            "ChromiumOS",
            "Chromium OS",
        ):
            with self.subTest(kept=kept):
                self.assertEqual(self.sub(kept), kept,
                                 f"exclusion not honoured: {kept!r} -> {self.sub(kept)!r}")

    def test_attribution_phrase_preserved_but_leading_product_rebrands(self):
        # The attribution phrase keeps "Chromium"; the leading product mention
        # (followed by " is made possible") is a real product string and rebrands.
        out = self.sub("Chromium is made possible by the Chromium open source project.")
        self.assertEqual(out, "Roamux is made possible by the Chromium open source project.")

    def test_chrome_url_untouched(self):
        # chrome://credits carries no "chromium" token; it must survive verbatim.
        s = "See chrome://credits for the open-source licenses"
        self.assertEqual(self.sub(s), s)

    def test_idempotent_no_double_rebrand(self):
        once = self.sub("About Chromium and the chromium project")
        twice = self.sub(once)
        self.assertEqual(once, twice)
        self.assertNotIn("Roamuxium", once)
        self.assertNotIn("Chromium", once.replace("Chromium open source", ""))

    def test_change_flag(self):
        self.assertTrue(rb.rebrand_text("About Chromium")[1])
        self.assertFalse(rb.rebrand_text("About Roamux")[1])
        self.assertFalse(rb.rebrand_text("The Chromium Authors")[1])


# ---------------------------------------------------------------------------
# Pure: grd structure preservation (streaming, structure-aware rewrite).
# ---------------------------------------------------------------------------
STRUCTURE_GRD = '''<?xml version="1.0" encoding="UTF-8"?>
<!-- This file uses the same layout as Chromium; do not edit the header. -->
<grit base_dir="." latest_public_release="0" current_release="1" source_lang_id="en">
  <release seq="1">
    <messages>
      <!-- Comment mentioning Chromium must survive verbatim. -->
      <if expr="is_macosx">
        <message name="IDS_ABOUT" desc="About Chromium menu item">About Chromium</message>
      </if>
      <message name="IDS_UMA" desc="crash consent">
        Help make Chromium better by sending <ph name="UMA_LINK">$1<ex>usage statistics</ex></ph> to Google
      </message>
      <message name="IDS_ABOUT_VERSION_COMPANY_NAME" desc="Company name">
        The Chromium Authors
      </message>
      <part file="inc.grdp" />
    </messages>
  </release>
</grit>
'''


class GrdStructureTest(unittest.TestCase):
    def rewrite(self, raw):
        new, _ = rb.rewrite_grd_text(raw)
        return new

    def test_message_text_rebranded(self):
        out = self.rewrite(STRUCTURE_GRD)
        self.assertIn(">About Roamux<", out)
        self.assertIn("Help make Roamux better by sending", out)

    def test_placeholder_names_untouched(self):
        out = self.rewrite(STRUCTURE_GRD)
        self.assertIn('<ph name="UMA_LINK">', out)
        self.assertIn("<ex>usage statistics</ex>", out)

    def test_message_names_and_desc_untouched(self):
        out = self.rewrite(STRUCTURE_GRD)
        self.assertIn('name="IDS_ABOUT"', out)
        self.assertIn('desc="About Chromium menu item"', out)  # desc is an attribute

    def test_comments_untouched(self):
        out = self.rewrite(STRUCTURE_GRD)
        self.assertIn("<!-- This file uses the same layout as Chromium;", out)
        self.assertIn("<!-- Comment mentioning Chromium must survive verbatim. -->", out)

    def test_if_and_part_structure_untouched(self):
        out = self.rewrite(STRUCTURE_GRD)
        self.assertIn('<if expr="is_macosx">', out)
        self.assertIn('<part file="inc.grdp" />', out)

    def test_excluded_message_stays_chromium(self):
        out = self.rewrite(STRUCTURE_GRD)
        self.assertIn("The Chromium Authors", out)
        self.assertNotIn("The Roamux Authors", out)

    def test_grdp_part_body_rebrands(self):
        grdp = ('<?xml version="1.0" encoding="utf-8"?>\n'
                '<grit-part>\n'
                '  <message name="IDS_RESET" desc="d">Reset Chromium settings</message>\n'
                '</grit-part>\n')
        out = self.rewrite(grdp)
        self.assertIn("Reset Roamux settings", out)

    def test_idempotent(self):
        once = self.rewrite(STRUCTURE_GRD)
        twice = self.rewrite(once)
        self.assertEqual(once, twice)
        self.assertNotIn("Roamuxium", once)


# ---------------------------------------------------------------------------
# Pure: xtb integrity (entities, empty ph, gendered branch, non-ASCII).
# ---------------------------------------------------------------------------
XTB_INTEGRITY = '''<?xml version="1.0" ?>
<!DOCTYPE translationbundle>
<translationbundle lang="de">
<translation id="1001">Chromium &amp; mehr</translation>
<translation id="1002">Bei Chromium anmelden <ph name="LEARN_MORE" /></translation>
<translation id="1003"><branch variants="variants{grammatical_gender_variant{grammatical_gender_case: MASCULINE}}">Chromium-Browser</branch></translation>
<translation id="2001">不变的 Chromium 文字</translation>
<translation id="9999">Unrelated Chromium string</translation>
</translationbundle>
'''


class XtbIntegrityTest(unittest.TestCase):
    def test_only_mapped_ids_change_and_file_stays_wellformed(self):
        # Re-key 1001->8001, and rebrand text for 1002/1003/2001; 9999 untouched.
        id_map = {"1001": "8001", "1002": "1002", "1003": "1003", "2001": "2001"}
        out, n = rb.rewrite_xtb_text(XTB_INTEGRITY, id_map)
        self.assertEqual(n, 4)
        # Re-keyed id + rebranded text.
        self.assertIn('<translation id="8001">Roamux &amp; mehr</translation>', out)
        # Entity preserved.
        self.assertIn("&amp;", out)
        # Empty ph preserved verbatim + surrounding text rebranded.
        self.assertIn('Bei Roamux anmelden <ph name="LEARN_MORE" />', out)
        # Gendered branch structure preserved, inner text rebranded.
        self.assertIn('<branch variants="variants{grammatical_gender_variant{'
                      'grammatical_gender_case: MASCULINE}}">Roamux-Browser</branch>', out)
        # Non-ASCII preserved, product token rebranded.
        self.assertIn("不变的 Roamux 文字", out)
        # Unmapped id untouched (id AND text).
        self.assertIn('<translation id="9999">Unrelated Chromium string</translation>', out)
        # Still XML-parseable (DOCTYPE + predefined entities).
        minidom.parseString(out)

    def test_empty_id_map_is_noop(self):
        out, n = rb.rewrite_xtb_text(XTB_INTEGRITY, {})
        self.assertEqual(n, 0)
        self.assertEqual(out, XTB_INTEGRITY)

    def test_duplicate_id_fails_loud(self):
        # Re-keying 1001 -> an id that already exists (9999) must fail loudly.
        with self.assertRaises(ValueError):
            rb.rewrite_xtb_text(XTB_INTEGRITY, {"1001": "9999"})


# ---------------------------------------------------------------------------
# GRIT-bound: id re-keying keeps translations bound (the load-bearing tests).
# ---------------------------------------------------------------------------
def _write(path, text):
    path.write_text(text, encoding="utf-8")


@unittest.skipIf(_SKIP_BINDING, GRIT_SKIP or "")
class XtbBindingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-rebrand-"))
        self.addCleanup(_rmtree, self.tmp)
        self.tclib, self.grd_reader = rb._import_grit(CHROMIUM_SRC)

        self.grd = self.tmp / "fx.grd"
        _write(self.grd, '''<?xml version="1.0" encoding="UTF-8"?>
<grit base_dir="." latest_public_release="0" current_release="1" source_lang_id="en">
  <translations>
    <file path="fx_de.xtb" lang="de" />
  </translations>
  <release seq="1">
    <messages>
      <message name="IDS_ABOUT" desc="about">About Chromium</message>
      <message name="IDS_UMA" desc="uma">Help make Chromium better by sending <ph name="UMA_LINK">$1<ex>stats</ex></ph></message>
      <message name="IDS_MANAGE" desc="manage" use_name_for_id="true">Manage Chromium extensions</message>
      <message name="IDS_ABOUT_VERSION_COMPANY_NAME" desc="company">The Chromium Authors</message>
      <part file="inc.grdp" />
    </messages>
  </release>
</grit>
''')
        _write(self.tmp / "inc.grdp", '''<?xml version="1.0" encoding="utf-8"?>
<grit-part>
  <message name="IDS_RESET" desc="reset">Reset Chromium settings</message>
</grit-part>
''')
        gid = self.tclib.GenerateMessageId
        self.about_old = gid("About Chromium", "")
        self.uma_old = gid("Help make Chromium better by sending UMA_LINK", "")
        self.reset_old = gid("Reset Chromium settings", "")
        self.legal_old = gid("The Chromium Authors", "")
        _write(self.tmp / "fx_de.xtb", '''<?xml version="1.0" ?>
<!DOCTYPE translationbundle>
<translationbundle lang="de">
<translation id="%s">Über Chromium</translation>
<translation id="%s">Hilf Chromium mit <ph name="UMA_LINK"/></translation>
<translation id="IDS_MANAGE">Chromium-Erweiterungen verwalten</translation>
<translation id="%s">Chromium-Einstellungen zurücksetzen</translation>
<translation id="%s">Die Chromium-Autoren</translation>
</translationbundle>
''' % (self.about_old, self.uma_old, self.reset_old, self.legal_old))

    def _run(self, check=False):
        return rb.run_on_grd_unit(self.grd, CHROMIUM_SRC, check=check)

    def _resolve_de(self):
        root = self.grd_reader.Parse(str(self.grd), dir=str(self.tmp))
        root.SetOutputLanguage("de")
        root.RunGatherers()
        out = {}
        for node in root.ActiveDescendants():
            if node.name == "message":
                out[node.attrs.get("name")] = node.Translate("de", None)
        return out

    def test_id_map_computed_through_grit(self):
        id_map = rb.compute_id_map(self.grd, CHROMIUM_SRC)
        gid = self.tclib.GenerateMessageId
        # Plain + <ph> + grdp-included get re-keyed to the NEW presentable id.
        self.assertEqual(id_map[self.about_old], gid("About Roamux", ""))
        self.assertEqual(id_map[self.uma_old],
                         gid("Help make Roamux better by sending UMA_LINK", ""))
        self.assertEqual(id_map[self.reset_old], gid("Reset Roamux settings", ""))
        # use_name_for_id: id is the NAME, unchanged.
        self.assertEqual(id_map["IDS_MANAGE"], "IDS_MANAGE")
        # Excluded legal message: never in the map.
        self.assertNotIn(self.legal_old, id_map)

    def test_xtb_rekeyed_and_text_rebranded(self):
        self._run()
        xtb = (self.tmp / "fx_de.xtb").read_text(encoding="utf-8")
        gid = self.tclib.GenerateMessageId
        self.assertIn('id="%s">Über Roamux<' % gid("About Roamux", ""), xtb)
        self.assertIn('id="%s">Hilf Roamux mit ' % gid("Help make Roamux better by sending UMA_LINK", ""), xtb)
        self.assertIn('id="%s">Roamux-Einstellungen' % gid("Reset Roamux settings", ""), xtb)
        # use_name_for_id keeps its id; only text rebrands.
        self.assertIn('id="IDS_MANAGE">Roamux-Erweiterungen verwalten<', xtb)
        # Excluded legal entry: id AND text unchanged.
        self.assertIn('id="%s">Die Chromium-Autoren<' % self.legal_old, xtb)

    def test_grd_and_grdp_rebranded(self):
        self._run()
        grd = self.grd.read_text(encoding="utf-8")
        grdp = (self.tmp / "inc.grdp").read_text(encoding="utf-8")
        self.assertIn(">About Roamux<", grd)
        self.assertIn("Help make Roamux better by sending", grd)
        self.assertIn("The Chromium Authors", grd)          # excluded, stays
        self.assertIn("Reset Roamux settings", grdp)

    def test_strongest_translation_still_binds(self):
        # Load the POST-pass grd + xtb with GRIT and assert each rebranded
        # message resolves to the TRANSLATED (de) string, not English/pseudo.
        self._run()
        de = self._resolve_de()
        self.assertEqual(de["IDS_ABOUT"], "Über Roamux")
        self.assertIn("Roamux", de["IDS_UMA"])
        self.assertNotIn("Chromium", de["IDS_UMA"])
        self.assertEqual(de["IDS_MANAGE"], "Roamux-Erweiterungen verwalten")
        self.assertEqual(de["IDS_RESET"], "Roamux-Einstellungen zurücksetzen")
        # Excluded legal message stays Chromium AND stays bound to its de text.
        self.assertEqual(de["IDS_ABOUT_VERSION_COMPANY_NAME"], "Die Chromium-Autoren")

    def test_idempotent_second_run_noop(self):
        self._run()
        after_first = _snapshot(self.tmp)
        result = self._run()
        self.assertEqual(result.changed_files, [])
        self.assertEqual(_snapshot(self.tmp), after_first)
        # No corruption tokens anywhere.
        for blob in after_first.values():
            self.assertNotIn("Roamuxium", blob)

    def test_check_mode_reports_and_mutates_nothing(self):
        before = _snapshot(self.tmp)
        result = self._run(check=True)
        self.assertTrue(result.would_change)
        self.assertEqual(_snapshot(self.tmp), before)  # byte-identical
        # After a real pass, --check is a clean no-op.
        self._run()
        clean = self._run(check=True)
        self.assertFalse(clean.would_change)


# ---------------------------------------------------------------------------
# CLI fail-loud (pure — main() aborts before importing GRIT).
# ---------------------------------------------------------------------------
class CliTest(unittest.TestCase):
    def _main(self, *args):
        import subprocess
        script = pathlib.Path(rb.__file__).resolve()
        return subprocess.run([sys.executable, str(script), *args],
                              capture_output=True, text=True)

    def test_missing_chromium_src_fails_loud(self):
        r = self._main("--chromium-src", "/no/such/dir/xyz")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("/no/such/dir/xyz", r.stdout + r.stderr)

    def test_missing_target_grd_fails_loud(self):
        with tempfile.TemporaryDirectory() as d:
            r = self._main("--chromium-src", d)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("target grd missing", r.stdout + r.stderr)


# ---------------------------------------------------------------------------
# Branded-grd exclusions (roam-132 review): the components_chromium_strings.grd
# About/version license labels attribute the upstream project and must NOT
# rebrand, while sibling user-visible product strings must.
# ---------------------------------------------------------------------------
BRANDED_GRD = '''<?xml version="1.0" encoding="UTF-8"?>
<grit base_dir="." latest_public_release="0" current_release="1" source_lang_id="en">
  <release seq="1">
    <messages>
      <message name="IDS_VERSION_UI_LICENSE" desc="license label">
        Chromium is made possible by the <ph name="BEGIN_LINK_CHROMIUM">&lt;a&gt;</ph>Chromium<ph name="END_LINK_CHROMIUM">&lt;/a&gt;</ph> open source project.
      </message>
      <message name="IDS_VERSION_UI_LICENSE_OTHER" desc="license label other">
        Chromium is also made possible by other open source software.
      </message>
      <message name="IDS_SESSION_CRASHED_VIEW_MESSAGE" desc="user-visible">
        Chromium didn't shut down correctly.
      </message>
    </messages>
  </release>
</grit>
'''


class BrandedGrdExclusionTest(unittest.TestCase):
    def test_license_names_are_excluded(self):
        for name in ("IDS_VERSION_UI_LICENSE", "IDS_VERSION_UI_LICENSE_CHROMIUM",
                     "IDS_VERSION_UI_LICENSE_OTHER"):
            self.assertTrue(excl.is_message_excluded(name),
                            f"{name} must be excluded (attribution stays Chromium)")

    def test_attribution_stays_but_user_visible_rebrands(self):
        out, _ = rb.rewrite_grd_text(BRANDED_GRD)
        # Attribution labels keep every "Chromium".
        self.assertIn("Chromium is made possible by the", out)
        self.assertIn("Chromium is also made possible by other open source software.", out)
        self.assertNotIn("Roamux is made possible", out)
        self.assertNotIn("Roamux is also made possible", out)
        # The <ph> attribution link name is never touched regardless.
        self.assertIn('<ph name="BEGIN_LINK_CHROMIUM">', out)
        # The sibling user-visible product string DOES rebrand.
        self.assertIn("Roamux didn't shut down correctly.", out)

    def test_exclusion_version_bumped(self):
        # The set changed for the two new branded grds — VERSION must have advanced.
        self.assertGreaterEqual(excl.VERSION, 2)


# ---------------------------------------------------------------------------
# CI gate (roam-132 review): on a runner WITH the checkout the CI step sets
# REQUIRE_GRIT=1 so the binding tests run fail-not-skip. This guard makes that
# contract self-enforcing — if REQUIRE_GRIT is set but grit is unavailable, the
# binding assertions would not execute, so fail loudly here.
# ---------------------------------------------------------------------------
class GritRequirementGateTest(unittest.TestCase):
    def test_grit_present_when_required(self):
        if REQUIRE_GRIT:
            self.assertIsNone(
                GRIT_SKIP,
                f"REQUIRE_GRIT=1 but GRIT is unavailable ({GRIT_SKIP}) — the "
                "load-bearing xtb-binding tests would not execute. The tier-2 CI "
                "gate must run with a synced Chromium checkout (ROAMUX_CHROMIUM_SRC).")
        else:
            self.skipTest("REQUIRE_GRIT not set — tier-1 skip behaviour is unchanged")


def _rmtree(path):
    import shutil
    shutil.rmtree(path, ignore_errors=True)


def _snapshot(root):
    return {p.name: p.read_text(encoding="utf-8")
            for p in sorted(pathlib.Path(root).iterdir()) if p.is_file()}


if __name__ == "__main__":
    unittest.main()
