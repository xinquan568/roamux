# SPDX-License-Identifier: Apache-2.0
"""Hermetic tests for the roam-34 update leg — no production key, no gh, no
Sparkle keychain. Covers appcast generation + signature verification (dev
key), the sign_update output parser, the every-mode EdDSA key requirement
(K3), and the draft/publish invariants (K2)."""

import base64
import hashlib
import pathlib
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

APPCAST = (pathlib.Path(__file__).resolve().parent.parent.parent / "app" /
           "appcast")
sys.path.insert(0, str(APPCAST))
import ed25519_ref as ed  # noqa: E402  (pure Ed25519 sign+verify, test only)
import generate_appcast  # noqa: E402
import release_flow  # noqa: E402
import sign_update_wrapper  # noqa: E402
import verify_appcast  # noqa: E402

TEST_SEED = bytes.fromhex(
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")

SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"


class AppcastGenerationTest(unittest.TestCase):
    def setUp(self):
        self.pub = ed.publickey(TEST_SEED)
        self.pub_b64 = base64.b64encode(self.pub).decode()
        self.artifact = b"ROAMUX-UPDATE-99.0.0-universal2-" + b"z" * 200
        self.sig_b64 = base64.b64encode(
            ed.signature(self.artifact, TEST_SEED, self.pub)).decode()
        self.url = ("https://github.com/xinquan568/roamux/releases/download/"
                    "v99.0.0/Roamux.zip")

    def test_appcast_is_well_formed_and_signature_verifies(self):
        xml = generate_appcast.generate_appcast(
            version="99.0.0", short_version="99.0-rc.1", enclosure_url=self.url,
            artifact_bytes=self.artifact, ed_signature=self.sig_b64,
            pub_date="Thu, 01 Jan 2026 00:00:00 +0000")
        root = ET.fromstring(xml)
        # roam-93: the Sparkle channel title carries the product brand.
        self.assertEqual(root.find(".//channel/title").text, "Roamux")
        enc = root.find(".//enclosure")
        self.assertEqual(enc.get("url"), self.url)
        self.assertEqual(int(enc.get("length")), len(self.artifact))
        self.assertEqual(enc.get(f"{{{SPARKLE_NS}}}edSignature"), self.sig_b64)
        self.assertEqual(
            root.find(f".//{{{SPARKLE_NS}}}version").text, "99.0.0")
        # roam-141: the numeric version is what Sparkle compares; the human
        # short version rides shortVersionString + the item title (dialog text).
        self.assertEqual(
            root.find(f".//{{{SPARKLE_NS}}}shortVersionString").text, "99.0-rc.1")
        self.assertEqual(root.find(".//item/title").text, "99.0-rc.1")
        # The committed signature verifies against the dev public key.
        self.assertTrue(_verify(self.artifact, self.sig_b64, self.pub_b64))

    def test_enclosure_url_is_artifact_not_feed(self):
        xml = generate_appcast.generate_appcast(
            version="99.0.0", short_version="99.0-rc.1", enclosure_url=self.url,
            artifact_bytes=self.artifact, ed_signature=self.sig_b64,
            pub_date="Thu, 01 Jan 2026 00:00:00 +0000")
        url = ET.fromstring(xml).find(".//enclosure").get("url")
        self.assertTrue(url.endswith((".zip", ".dmg")), url)
        self.assertFalse(url.endswith("appcast.xml"), url)

    def test_length_is_byte_exact(self):
        xml = generate_appcast.generate_appcast(
            version="1.0", short_version="1.0", enclosure_url=self.url,
            artifact_bytes=b"abc", ed_signature=self.sig_b64,
            pub_date="x")
        self.assertEqual(
            int(ET.fromstring(xml).find(".//enclosure").get("length")), 3)


FIXTURE_DIR = pathlib.Path(__file__).resolve().parents[2] / "test" / "data" / "sparkle"
PROD_PLIST = pathlib.Path(__file__).resolve().parents[2] / "app" / "sparkle-Info.plist"
VERIFY_CLI = APPCAST / "verify_appcast.py"
SIG_ATTR = "sparkle:edSignature"


def _write_plist(path, entries):
    with open(path, "wb") as f:
        plistlib.dump(entries, f)


def _appcast_variant(tmp, name, mutate):
    """The committed fixture appcast with one textual mutation applied (never the fixture itself)."""
    text = (FIXTURE_DIR / "appcast.xml").read_text()
    out = tmp / name
    out.write_text(mutate(text))
    return out


def _replace_sig(text, new_value):
    return re.sub(rf'{SIG_ATTR}="[^"]*"', f'{SIG_ATTR}="{new_value}"', text)


class VerifyAppcastPublicKeyTest(unittest.TestCase):
    """roam-286 (grill C1 steps 3-5 / M42): staging validation verifies the DOWNLOADED appcast +
    artifact with the committed SUPublicEDKey ONLY — no keychain, no private key, no external
    tool. Driven by the committed Sparkle-signed fixture (roamux/test/data/sparkle: signatures made
    by Sparkle's own sign_update, guarded by test_sparkle_fixture.py), so a pass here is a pass
    against real Sparkle output."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = pathlib.Path(tempfile.mkdtemp(prefix="roamux-verify-appcast-"))
        cls.pub_b64 = (FIXTURE_DIR / "test_public_ed_key.b64").read_text().strip()
        cls.plist = cls.tmp / "test-Info.plist"
        _write_plist(cls.plist, {"SUPublicEDKey": cls.pub_b64})
        cls.artifact = FIXTURE_DIR / "Roamux-99.0.0.zip"
        cls.appcast = FIXTURE_DIR / "appcast.xml"

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def _refused(self, appcast=None, artifact=None, plist=None):
        with self.assertRaises(verify_appcast.StagingValidationError) as cm:
            verify_appcast.verify_enclosure(appcast or self.appcast, artifact or self.artifact,
                                            plist or self.plist)
        return str(cm.exception)

    def test_fixture_verifies_with_the_public_key_only(self):
        verify_appcast.verify_enclosure(self.appcast, self.artifact, self.plist)  # no raise

    def test_tampered_appcast_is_rejected(self):
        self.assertIn("does not verify", self._refused(appcast=FIXTURE_DIR / "appcast-tampered.xml"))

    def test_unsigned_appcast_is_rejected(self):
        msg = self._refused(appcast=FIXTURE_DIR / "appcast-unsigned.xml")
        self.assertIn("edSignature", msg)
        self.assertNotIn("does not verify", msg)

    def test_wrong_key_is_rejected(self):
        # The production SUPublicEDKey did not sign the fixture: valid signature, wrong key.
        self.assertIn("does not verify", self._refused(plist=PROD_PLIST))

    def test_truncated_artifact_fails_on_length_before_any_signature_math(self):
        trunc = self.tmp / "truncated.zip"
        trunc.write_bytes(self.artifact.read_bytes()[:-1])
        msg = self._refused(artifact=trunc)
        self.assertIn("length", msg)
        self.assertNotIn("does not verify", msg)

    def test_plist_without_key_is_rejected(self):
        plist = self.tmp / "nokey-Info.plist"
        _write_plist(plist, {"SUFeedURL": "https://example.invalid/appcast.xml"})
        self.assertIn("SUPublicEDKey", self._refused(plist=plist))

    def test_failure_branch_matrix(self):
        sig63 = base64.b64encode(b"\x01" * 63).decode()
        key31 = base64.b64encode(b"\x02" * 31).decode()
        bad_key = self.tmp / "badkey-Info.plist"
        _write_plist(bad_key, {"SUPublicEDKey": "!!!not-base64!!!"})
        short_key = self.tmp / "shortkey-Info.plist"
        _write_plist(short_key, {"SUPublicEDKey": key31})
        cases = [
            ("key_not_base64", dict(plist=bad_key), "SUPublicEDKey is not 32 bytes of base64"),
            ("key_31_bytes", dict(plist=short_key), "SUPublicEDKey is not 32 bytes of base64"),
            ("sig_not_base64", dict(appcast=_appcast_variant(self.tmp, "sig_b64.xml",
             lambda s: _replace_sig(s, "!!!not-base64!!!"))), "edSignature is not 64 bytes of base64"),
            ("sig_63_bytes", dict(appcast=_appcast_variant(self.tmp, "sig63.xml",
             lambda s: _replace_sig(s, sig63))), "edSignature is not 64 bytes of base64"),
            ("sig_empty", dict(appcast=_appcast_variant(self.tmp, "sigempty.xml",
             lambda s: _replace_sig(s, ""))), "enclosure has no sparkle:edSignature"),
            ("no_enclosure", dict(appcast=_appcast_variant(self.tmp, "noenc.xml",
             lambda s: re.sub(r"<enclosure.*?/>", "", s, flags=re.S))), "appcast has no enclosure"),
            ("length_missing", dict(appcast=_appcast_variant(self.tmp, "nolen.xml",
             lambda s: s.replace('length="900"', ""))), "enclosure length attribute missing"),
            ("length_non_integer", dict(appcast=_appcast_variant(self.tmp, "lenabc.xml",
             lambda s: s.replace('length="900"', 'length="abc"'))), "enclosure length is not an integer"),
            ("length_off_by_one", dict(appcast=_appcast_variant(self.tmp, "len899.xml",
             lambda s: s.replace('length="900"', 'length="899"'))), "enclosure length 899 != artifact size 900"),
        ]
        self.assertEqual(9, len(cases))
        for name, kwargs, reason in cases:
            with self.subTest(case=name):
                self.assertIn(reason, self._refused(**kwargs))

    # --- CLI (the shape release.yml invokes) ---
    def _cli(self, *extra, appcast=None, artifact=None, plist=None):
        return subprocess.run([sys.executable, str(VERIFY_CLI),
                               "--appcast", str(appcast or self.appcast),
                               "--artifact", str(artifact or self.artifact),
                               "--public-key-plist", str(plist or self.plist), *extra],
                              capture_output=True, text=True, timeout=120)

    def test_cli_success(self):
        r = self._cli()
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)
        self.assertIn("[ok]", r.stdout)
        self.assertIn("public key only", r.stdout)

    def test_cli_tampered(self):
        r = self._cli(appcast=FIXTURE_DIR / "appcast-tampered.xml")
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertIn("::error::", r.stderr)
        self.assertIn("does not verify", r.stderr)

    def test_cli_malformed_input(self):
        sig63 = base64.b64encode(b"\x01" * 63).decode()
        bad = _appcast_variant(self.tmp, "cli_sig63.xml", lambda s: _replace_sig(s, sig63))
        r = self._cli(appcast=bad)
        self.assertEqual(1, r.returncode, r.stdout + r.stderr)
        self.assertIn("::error::", r.stderr)
        self.assertIn("edSignature is not 64 bytes of base64", r.stderr)

    def test_cli_rejects_the_removed_keychain_flags(self):
        # The old shape (--sparkle-bin-dir/--account = keychain-backed verification) must fail
        # loudly as UNRECOGNIZED, so an un-migrated workflow cannot silently keep the import path.
        for flag in ("--account", "--sparkle-bin-dir"):
            with self.subTest(flag=flag):
                r = self._cli(flag, "x")
                self.assertEqual(2, r.returncode, r.stdout + r.stderr)
                self.assertIn(f"unrecognized arguments: {flag}", r.stderr)


class Ed25519StrictnessTest(unittest.TestCase):
    """roam-286: the reference verifier is now the release job's staging verifier, so it enforces
    RFC 8032 §5.1.7 decoding: S < l, canonical y (< q), and no x = 0 with the sign bit set. Every
    canonical Sparkle signature still verifies (test_sparkle_fixture.py guards the committed ones)."""

    @classmethod
    def setUpClass(cls):
        cls.artifact = (FIXTURE_DIR / "Roamux-99.0.0.zip").read_bytes()
        cls.pub = base64.b64decode((FIXTURE_DIR / "test_public_ed_key.b64").read_text().strip())
        enc = ET.parse(FIXTURE_DIR / "appcast.xml").getroot().find(".//enclosure")
        cls.sig = base64.b64decode(enc.get(f"{{{SPARKLE_NS}}}edSignature"))

    def test_untouched_fixture_signature_verifies(self):
        self.assertTrue(ed.verify(self.artifact, self.sig, self.pub))

    def test_signature_with_s_plus_l_is_rejected(self):
        S = int.from_bytes(self.sig[32:], "little")
        self.assertLess(S, ed.l, "Sparkle emits canonical scalars")
        malleated = self.sig[:32] + (S + ed.l).to_bytes(32, "little")
        self.assertFalse(ed.verify(self.artifact, malleated, self.pub))

    def test_decodepoint_rejects_non_canonical_y(self):
        with self.assertRaises(ValueError):
            ed.decodepoint((ed.q + 1).to_bytes(32, "little"))

    def test_decodepoint_rejects_zero_x_with_sign_bit(self):
        with self.assertRaises(ValueError):
            ed.decodepoint(((1 << 255) | 1).to_bytes(32, "little"))

    def test_decodepoint_accepts_the_canonical_identity(self):
        self.assertEqual([0, 1], list(ed.decodepoint((1).to_bytes(32, "little"))))


class SignUpdateParserTest(unittest.TestCase):
    def test_parses_the_real_2_9_4_output_shape(self):
        line = ('sparkle:edSignature="EVX9lDb2cyDxWQg5BoOMWV1WxzS8bMVfyKSFQ'
                'zBPPr0hFBJruAkq5Y6sNoWjL60ROVzc4lQhahA0dmbjNMJpAA==" '
                'length="19"')
        got = sign_update_wrapper.parse_sign_update_output(line)
        self.assertEqual(got["length"], 19)
        self.assertTrue(got["signature"].endswith("=="))

    def test_malformed_output_raises(self):
        with self.assertRaises(ValueError):
            sign_update_wrapper.parse_sign_update_output("not a signature line")


class EveryModeKeyRequirementTest(unittest.TestCase):
    def test_present_key_returns_path(self):
        self.assertEqual(
            release_flow.require_sparkle_key({"SPARKLE_ED_PRIVATE_KEY": "k"}),
            "k")

    def test_missing_key_hard_fails_in_any_mode(self):
        # K3: no "publish an unsigned-by-Sparkle update" path.
        with self.assertRaises(release_flow.MissingSparkleKeyError):
            release_flow.require_sparkle_key({})


class DraftPublishInvariantTest(unittest.TestCase):
    def test_refuses_prerelease_publish(self):
        with self.assertRaises(release_flow.NotPublishableError):
            release_flow.assert_publishable(is_prerelease=True,
                                            staging_validated=True)

    def test_refuses_publish_before_staging_validation(self):
        with self.assertRaises(release_flow.NotPublishableError):
            release_flow.assert_publishable(is_prerelease=False,
                                            staging_validated=False)

    def test_allows_publish_when_release_and_validated(self):
        release_flow.assert_publishable(is_prerelease=False,
                                        staging_validated=True)

    def test_plan_release_urls(self):
        plan = release_flow.plan_release("v99.0.0", "Roamux.zip")
        self.assertIn("/releases/download/v99.0.0/appcast.xml",
                      plan["staging_feed_url"])
        self.assertTrue(plan["production_feed_url"].endswith(
            "/releases/latest/download/appcast.xml"))
        self.assertTrue(plan["enclosure_url"].endswith(
            "/releases/download/v99.0.0/Roamux.zip"))


def _verify(data, sig_b64, pub_b64):
    return ed.verify(data, base64.b64decode(sig_b64),
                     base64.b64decode(pub_b64))


if __name__ == "__main__":
    unittest.main()
