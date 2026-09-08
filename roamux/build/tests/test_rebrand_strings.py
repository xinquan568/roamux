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
import posixpath
import re
import shutil
import subprocess
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

    def test_agglutinated_product_mentions_rebrand(self):
        # roam-284: Python's Unicode \b treats Hangul/Han/Ethiopic/Cyrillic letters
        # as word characters, so a product mention glued to a suffix/prefix (the
        # normal shape in agglutinative and space-free scripts) never matched.
        for src, want in (
            ("Chromium을 정보", "Roamux을 정보"),                 # ko object particle
            ("Chromium에서는 지원됩니다", "Roamux에서는 지원됩니다"),  # ko locative
            ("Chromium과 동기화", "Roamux과 동기화"),             # ko conjunction
            ("从Chromium中导入", "从Roamux中导入"),              # zh-CN, no spaces
            ("ለChromium ይመዝገቡ", "ለRoamux ይመዝገቡ"),             # am, prefixing
            ("Chromiumда кирүү", "Roamuxда кирүү"),             # ky, Cyrillic suffix
            ("从chromium中导入", "从roamux中导入"),              # lowercase mention
            ("Chromium，欢迎", "Roamux，欢迎"),                  # guard: fullwidth comma
        ):
            with self.subTest(src=src):
                self.assertEqual(self.sub(src), want)

    def test_agglutinated_exclusions_stay(self):
        # The vetoes must hold with the SAME boundaries: "Chromium OS" / "Chromium
        # Authors" glued to a CJK suffix keep "Chromium" byte-identically.
        for kept in (
            "ChromiumOS를 사용",
            "从ChromiumOS中导入",
            "Chromium OS를 사용",
            "从Chromium OS中导入",
            "Chromium Authors를 참조",
        ):
            with self.subTest(kept=kept):
                self.assertEqual(self.sub(kept), kept,
                                 f"exclusion not honoured: {kept!r} -> {self.sub(kept)!r}")

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
# Pure: the CJK-adjacent locale scan (roam-284) — the verifier behind the
# release gate's --verify-locales. It mirrors the channel's boundary model:
# a brand token glued to a CJK code point is a miss UNLESS it is one of the
# protected forms (ChromiumOS / Chromium OS / Chromium Authors / Chromium open
# source, either casing) or glued to an ASCII identifier character.
# ---------------------------------------------------------------------------
class LocaleScanTest(unittest.TestCase):
    def test_hits(self):
        for text in (
            "Chromium을 정보",
            "从Chromium中导入",
            "从chromium中导入",
            "从Chromium AuthorsExtra中",   # near-miss: the veto's ASCII suffix boundary fails on "E"
        ):
            with self.subTest(text=text):
                hits = rb.scan_cjk_adjacent(text)
                self.assertEqual(len(hits), 1, f"{text!r} -> {hits!r}")
                self.assertIn("hromium", hits[0])

    def test_non_hits(self):
        for text in (
            "ChromiumOS를 사용",
            "从ChromiumOS中导入",
            "从ChromiumOSX中导入",          # ASCII-suffixed identifier: the channel leaves it too
            "从chromiumOS中导入",
            "从Chromium OS中导入",
            "从chromium OS中导入",
            "Chromium Authors를 참조",
            "Chromium open source를 참조",
            "Chromium，欢迎",               # fullwidth comma is not a CJK letter
            "About Chromium",
            "Roamux을 정보",
        ):
            with self.subTest(text=text):
                self.assertEqual(rb.scan_cjk_adjacent(text), [], text)

    def test_verify_xtb_text_scans_compiled_ids_only(self):
        xtb = ('<?xml version="1.0" ?>\n<!DOCTYPE translationbundle>\n'
               '<translationbundle lang="ko">\n'
               '<translation id="1001">Chromium을 정보</translation>\n'
               '<translation id="1002">Chromium을 정보</translation>\n'
               '<translation id="1003">Chromium &amp; <ph name="X"/>을</translation>\n'
               '</translationbundle>\n')
        hits = rb.verify_xtb_text(xtb, {"1001", "1003"})
        # 1001 compiled + miss -> reported; 1002 not compiled -> silent; 1003:
        # text nodes are scanned separately ("Chromium &amp; " | "을"), so the
        # placeholder split is not an adjacency (the channel's own boundary).
        self.assertEqual([h[0] for h in hits], ["1001"])
        self.assertIn("Chromium을", hits[0][1])
        self.assertEqual(rb.verify_xtb_text(xtb, set()), [])


# ---------------------------------------------------------------------------
# GRIT-bound: id re-keying keeps translations bound (the load-bearing tests).
# ---------------------------------------------------------------------------
def _write(path, text):
    path.write_text(text, encoding="utf-8")


BINDING_GRD = '''<?xml version="1.0" encoding="UTF-8"?>
<grit base_dir="." latest_public_release="0" current_release="1" source_lang_id="en">
  <translations>
    <file path="fx_de.xtb" lang="de" />
    <file path="fx_ko.xtb" lang="ko" />
  </translations>
  <release seq="1">
    <messages>
      <message name="IDS_ABOUT" desc="about">About Chromium</message>
      <message name="IDS_UMA" desc="uma">Help make Chromium better by sending <ph name="UMA_LINK">$1<ex>stats</ex></ph></message>
      <message name="IDS_MANAGE" desc="manage" use_name_for_id="true">Manage Chromium extensions</message>
      <message name="IDS_ABOUT_VERSION_COMPANY_NAME" desc="company">The Chromium Authors</message>
      <message name="IDS_SIGNIN" desc="signin">sign in to Chromium<ph name="END_LINK">&lt;/a&gt;</ph>.</message>
      <message name="IDS_MIXED" desc="mixed">Use Chromium to sign in to Chromium<ph name="END_LINK">&lt;/a&gt;</ph>.</message>
      <part file="inc.grdp" />
    </messages>
  </release>
</grit>
'''

BINDING_GRDP = '''<?xml version="1.0" encoding="utf-8"?>
<grit-part>
  <message name="IDS_RESET" desc="reset">Reset Chromium settings</message>
</grit-part>
'''


def _binding_old_ids(tclib):
    """Pre-rebrand xtb ids of the BINDING_GRD messages (GRIT's own hash path)."""
    gid = tclib.GenerateMessageId
    return {
        "about": gid("About Chromium", ""),
        "uma": gid("Help make Chromium better by sending UMA_LINK", ""),
        "reset": gid("Reset Chromium settings", ""),
        "legal": gid("The Chromium Authors", ""),
        # roam-284: placeholder-adjacent mentions (omitted-map / wrong-hash cases).
        "signin": gid("sign in to ChromiumEND_LINK.", ""),
        "mixed": gid("Use Chromium to sign in to ChromiumEND_LINK.", ""),
    }


def _write_binding_fixture(d, tclib):
    """Materialise BINDING_GRD + inc.grdp + fx_de.xtb + fx_ko.xtb under ``d``."""
    d = pathlib.Path(d)
    old = _binding_old_ids(tclib)
    _write(d / "fx.grd", BINDING_GRD)
    _write(d / "inc.grdp", BINDING_GRDP)
    _write(d / "fx_de.xtb", '''<?xml version="1.0" ?>
<!DOCTYPE translationbundle>
<translationbundle lang="de">
<translation id="%s">Über Chromium</translation>
<translation id="%s">Hilf Chromium mit <ph name="UMA_LINK"/></translation>
<translation id="IDS_MANAGE">Chromium-Erweiterungen verwalten</translation>
<translation id="%s">Chromium-Einstellungen zurücksetzen</translation>
<translation id="%s">Die Chromium-Autoren</translation>
<translation id="%s">bei Chromium anmelden<ph name="END_LINK"/>.</translation>
<translation id="%s">Mit Chromium bei Chromium anmelden<ph name="END_LINK"/>.</translation>
</translationbundle>
''' % (old["about"], old["uma"], old["reset"], old["legal"], old["signin"], old["mixed"]))
    _write(d / "fx_ko.xtb", '''<?xml version="1.0" ?>
<!DOCTYPE translationbundle>
<translationbundle lang="ko">
<translation id="%s">Chromium을 정보</translation>
<translation id="IDS_MANAGE">Chromium 확장 프로그램 관리</translation>
<translation id="%s">Chromium에 로그인<ph name="END_LINK"/>.</translation>
<translation id="%s">Chromium으로 Chromium에 로그인<ph name="END_LINK"/>.</translation>
</translationbundle>
''' % (old["about"], old["signin"], old["mixed"]))
    return old


@unittest.skipIf(_SKIP_BINDING, GRIT_SKIP or "")
class XtbBindingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-rebrand-"))
        self.addCleanup(_rmtree, self.tmp)
        self.tclib, self.grd_reader = rb._import_grit(CHROMIUM_SRC)

        self.grd = self.tmp / "fx.grd"
        old = _write_binding_fixture(self.tmp, self.tclib)
        self.about_old = old["about"]
        self.uma_old = old["uma"]
        self.reset_old = old["reset"]
        self.legal_old = old["legal"]
        self.signin_old = old["signin"]
        self.mixed_old = old["mixed"]

    def _run(self, check=False):
        return rb.run_on_grd_unit(self.grd, CHROMIUM_SRC, check=check)

    def _resolve(self, lang):
        root = self.grd_reader.Parse(str(self.grd), dir=str(self.tmp))
        root.SetOutputLanguage(lang)
        root.RunGatherers()
        out = {}
        for node in root.ActiveDescendants():
            if node.name == "message":
                out[node.attrs.get("name")] = node.Translate(lang, None)
        return out

    def _resolve_de(self):
        return self._resolve("de")

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

    def test_placeholder_adjacent_messages_map_to_fully_substituted_ids(self):
        # roam-284: the id map must decide AND hash with the same boundaries as
        # rewrite_grd_text (which skips <ph> subtrees). The flattened presentable
        # "…ChromiumEND_LINK." hid the token behind \b (omitted map) or hashed a
        # half-substituted string (wrong hash) — either way the translation unbinds.
        id_map = rb.compute_id_map(self.grd, CHROMIUM_SRC)
        gid = self.tclib.GenerateMessageId
        with self.subTest(case="signin"):
            self.assertIn(self.signin_old, id_map)
            self.assertEqual(id_map[self.signin_old], gid("sign in to RoamuxEND_LINK.", ""))
        with self.subTest(case="mixed"):
            self.assertIn(self.mixed_old, id_map)
            self.assertEqual(id_map[self.mixed_old],
                             gid("Use Roamux to sign in to RoamuxEND_LINK.", ""))

    def test_placeholder_adjacent_translations_bind(self):
        # GRIT resolves an unbound translation to pseudolocalised text in this
        # fixture, so an exact-prefix assertion cannot pass vacuously.
        self._run()
        de = self._resolve("de")
        ko = self._resolve("ko")
        cases = (
            ("de/IDS_SIGNIN", de["IDS_SIGNIN"], "bei Roamux anmelden"),
            ("de/IDS_MIXED", de["IDS_MIXED"], "Mit Roamux bei Roamux anmelden"),
            ("ko/IDS_ABOUT", ko["IDS_ABOUT"], "Roamux을 정보"),
            ("ko/IDS_SIGNIN", ko["IDS_SIGNIN"], "Roamux에 로그인"),
            ("ko/IDS_MIXED", ko["IDS_MIXED"], "Roamux으로 Roamux에 로그인"),
        )
        for case, got, prefix in cases:
            with self.subTest(case=case):
                self.assertTrue(got.startswith(prefix), f"{case}: {got!r}")
        with self.subTest(case="ko/IDS_ABOUT exact"):
            self.assertEqual(ko["IDS_ABOUT"], "Roamux을 정보")

    def test_verify_locales_clean_after_channel(self):
        self._run()
        self.assertEqual(rb.verify_locales(self.grd, CHROMIUM_SRC, ["ko", "de"]), [])

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
# roam-284: the CLI locale verifier end-to-end (GRIT-bound) — a temp "src" whose
# tools/grit is the checkout's GRIT and whose TARGET_GRDS are minimal fixtures.
# ---------------------------------------------------------------------------
_MINI_UNIT = '''<?xml version="1.0" encoding="UTF-8"?>
<grit base_dir="." latest_public_release="0" current_release="1" source_lang_id="en">
  <release seq="1">
    <messages>
      <message name="IDS_%s" desc="d">Welcome to Chromium %s</message>
    </messages>
  </release>
</grit>
'''


@unittest.skipIf(_SKIP_BINDING, GRIT_SKIP or "")
class VerifyLocalesCliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-rebrand-cli-"))
        self.addCleanup(_rmtree, self.tmp)
        tclib, _ = rb._import_grit(CHROMIUM_SRC)
        (self.tmp / "tools").mkdir()
        os.symlink(CHROMIUM_SRC / "tools" / "grit", self.tmp / "tools" / "grit")
        # Unit 1 carries the binding fixture (+ fx_ko.xtb); the others are one-message units.
        first = self.tmp / rb.TARGET_GRDS[0]
        first.parent.mkdir(parents=True)
        _write_binding_fixture(first.parent, tclib)
        (first.parent / "fx.grd").rename(first)
        for i, rel in enumerate(rb.TARGET_GRDS[1:], 1):
            p = self.tmp / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            _write(p, _MINI_UNIT % (i, i))
        self.ko = first.parent / "fx_ko.xtb"

    def _main(self, *args):
        script = pathlib.Path(rb.__file__).resolve()
        return subprocess.run([sys.executable, str(script), "--chromium-src", str(self.tmp), *args],
                              capture_output=True, text=True)

    def _seed(self, text):
        raw = self.ko.read_text(encoding="utf-8")
        new = re.sub(r'(<translation id="IDS_MANAGE">)[^<]*', r"\g<1>" + text, raw)
        self.assertNotEqual(new, raw)
        _write(self.ko, new)

    def test_verify_locales_seeded_miss_is_red(self):
        r = self._main()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = self._main("--check", "--verify-locales", "ko")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("[ok] locale scan", r.stdout)
        # A CJK-adjacent survivor in a COMPILED ko translation fails the gate, naming file + id.
        self._seed("Chromium을 관리")
        r = self._main("--check", "--verify-locales", "ko")
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        out = r.stdout + r.stderr
        self.assertIn("FAIL:", out)
        self.assertIn("fx_ko.xtb", out)
        self.assertIn("id=IDS_MANAGE", out)
        # A protected form glued to a suffix is not a miss.
        self._seed("ChromiumOS를 관리")
        r = self._main("--check", "--verify-locales", "ko")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


# ---------------------------------------------------------------------------
# roam-284: the live scan — the real TARGET_GRDS units, five CJK locales, on a
# PRISTINE snapshot (git show HEAD:<path>) materialised read-only into a temp
# tree; the checkout's working tree is never read for content, never written.
# ---------------------------------------------------------------------------
CJK_LOCALES = ("ko", "zh-CN", "ja", "zh-TW", "zh-HK")


def _git_show(src, rel):
    r = subprocess.run(["git", "-C", str(src), "show", f"HEAD:{rel}"],
                       capture_output=True)
    return r.stdout.decode("utf-8") if r.returncode == 0 else None


@unittest.skipIf(_SKIP_BINDING, GRIT_SKIP or "")
class LiveLocaleScanTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-rebrand-live-"))
        cls.units = []                         # (rel_grd, [rel_xtb...])
        for rel in rb.TARGET_GRDS:
            grd = _git_show(CHROMIUM_SRC, rel)
            if grd is None:
                _rmtree(cls.tmp)
                raise unittest.SkipTest(f"git show HEAD:{rel} failed in {CHROMIUM_SRC}")
            cls._put(rel, grd)
            stack = [(rel, grd)]
            seen = set()
            while stack:
                frel, fraw = stack.pop()
                for part in rb._PART_RE.findall(fraw):
                    prel = posixpath.normpath(posixpath.join(posixpath.dirname(frel), part))
                    if not prel.endswith(".grdp") or prel in seen:
                        continue
                    seen.add(prel)
                    praw = _git_show(CHROMIUM_SRC, prel)
                    if praw is None:
                        continue               # mirrors discover_grdp_files (skips non-files)
                    cls._put(prel, praw)
                    stack.append((prel, praw))
            xtbs = []
            for xrel in rb._XTB_FILE_RE.findall(grd):
                if not any(xrel.endswith(f"_{loc}.xtb") for loc in CJK_LOCALES):
                    continue
                arel = posixpath.normpath(posixpath.join(posixpath.dirname(rel), xrel))
                xraw = _git_show(CHROMIUM_SRC, arel)
                if xraw is not None:
                    cls._put(arel, xraw)
                    xtbs.append(arel)
            cls.units.append((rel, xtbs))

    @classmethod
    def tearDownClass(cls):
        _rmtree(cls.tmp)

    @classmethod
    def _put(cls, rel, text):
        p = cls.tmp / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        _write(p, text)

    @staticmethod
    def _locale_of(rel):
        return next(loc for loc in CJK_LOCALES if rel.endswith(f"_{loc}.xtb"))

    def test_channel_leaves_zero_cjk_adjacent_survivors(self):
        before = {loc: 0 for loc in CJK_LOCALES}
        after = {loc: 0 for loc in CJK_LOCALES}
        n_xtb = 0
        probe_done = False
        for rel, xtbs in self.units:
            grd = self.tmp / rel
            compiled = rb.compute_compiled_ids(grd, self.tmp)
            id_map = rb.compute_id_map(grd, self.tmp)
            post_ids = {id_map.get(i, i) for i in compiled}
            for xrel in xtbs:
                loc = self._locale_of(xrel)
                raw = (self.tmp / xrel).read_text(encoding="utf-8")
                before[loc] += len(rb.verify_xtb_text(raw, compiled))
                new = rb.rewrite_xtb_text(raw, id_map)[0]
                survivors = rb.verify_xtb_text(new, post_ids)
                after[loc] += len(survivors)
                n_xtb += 1
                with self.subTest(unit=rel, xtb=xrel):
                    self.assertEqual(survivors, [], f"{xrel}: {survivors[:5]}")
                if not probe_done and loc == "ko":
                    # Non-vacuity: seed one transformed ko translation (under its
                    # RESULTING id) and the scan must name exactly that id.
                    ids = [i for i in rb._TRANS_ID_RE.findall(new) if i in post_ids]
                    self.assertTrue(ids, f"{xrel}: no compiled translation to probe")
                    target = ids[0]
                    seeded = re.sub(r'(<translation id="%s"[^>]*>)(.*?)(</translation>)' % re.escape(target),
                                    r"\g<1>Chromium을 테스트\g<3>", new, count=1, flags=re.DOTALL)
                    self.assertNotEqual(seeded, new)
                    self.assertEqual([h[0] for h in rb.verify_xtb_text(seeded, post_ids)], [target])
                    probe_done = True
        self.assertGreater(n_xtb, 0, "no CJK xtb materialised")
        self.assertTrue(probe_done, "non-vacuity probe did not run")
        print("\n[roam-284 live scan] CJK-adjacent brand tokens before -> after the channel:")
        for loc in CJK_LOCALES:
            print(f"  {loc}: {before[loc]} -> {after[loc]}")
        for loc in CJK_LOCALES:
            with self.subTest(locale=loc):
                self.assertEqual(after[loc], 0)


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
        # v2: the two new branded grds; v3 (roam-284): ASCII token boundaries at
        # both ends + widened " OS"/" Authors" vetoes — VERSION must have advanced.
        self.assertGreaterEqual(excl.VERSION, 3)


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
    shutil.rmtree(path, ignore_errors=True)


def _snapshot(root):
    return {p.name: p.read_text(encoding="utf-8")
            for p in sorted(pathlib.Path(root).iterdir()) if p.is_file()}


if __name__ == "__main__":
    unittest.main()
