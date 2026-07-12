#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerates the fixed pre-signed Sparkle test-feed fixture (roam-32; re-signed by roam-101).

TEST-ONLY key material: the Ed25519 seed below is a committed, clearly-marked
test fixture (plan §13.6/K3 — local/CI validation uses a dev/test pair; the
production key is minted by I-6.3 in the protected release environment and
never appears in this repo). Deterministic: same seed ⇒ same fixture bytes —
Ed25519 (RFC 8032) is deterministic, and the zip is assembled with fixed
timestamps/modes.

Signatures are produced by Sparkle's own `sign_update` (roam-101): the vendored
tool at roamux/third_party/sparkle/bin/sign_update (pinned + hash-verified by
roamux/build/fetch_sparkle.py — run that first). Public-key derivation and a
post-sign parity self-check use the in-repo pure-python reference
(roamux/app/appcast/ed25519_ref) — the two implementations are byte-equivalent
for the same key + bytes, and the self-check keeps that fact enforced.
"""

import base64
import io
import pathlib
import re
import subprocess
import sys
import tempfile
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
ROAMUX_ROOT = HERE.parents[2]  # roamux/
sys.path.insert(0, str(ROAMUX_ROOT / "app" / "appcast"))
import ed25519_ref as ed  # noqa: E402  (pure Ed25519 reference, test-only)

# *** TEST KEY — NOT A SECRET. Do not use outside fixtures. ***
TEST_SEED = bytes.fromhex(
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")

ARTIFACT_NAME = "Roamux-99.0.0.zip"
APPCAST_NAME = "appcast.xml"
APPCAST_TAMPERED_NAME = "appcast-tampered.xml"
APPCAST_UNSIGNED_NAME = "appcast-unsigned.xml"
TESTHOST_ID = "com.roamux.sparkle.testhost"

SIGN_UPDATE = ROAMUX_ROOT / "third_party" / "sparkle" / "bin" / "sign_update"
SIGN_UPDATE_RE = re.compile(r'sparkle:edSignature="([A-Za-z0-9+/=]+)" length="(\d+)"')


def sign_with_sparkle(data, pub):
    """Sign `data` with the vendored Sparkle sign_update; return the base64 signature.

    Fails loudly if the vendored tool is absent (mirrors the flag-on GN convention), if its
    output shape changes, or if the signature does not pass the reference verifier (parity
    self-check: Sparkle's signer and the in-repo reference math must never drift).
    """
    if not SIGN_UPDATE.is_file():
        sys.exit(f"sign_update not found at {SIGN_UPDATE} — run "
                 f"`python3 roamux/build/fetch_sparkle.py` first (pinned + hash-verified).")
    with tempfile.TemporaryDirectory(prefix="roamux-fixture-sign-") as tmp:
        tmpdir = pathlib.Path(tmp)
        key_file = tmpdir / "test_ed_key.b64"
        key_file.touch(mode=0o600)
        key_file.write_text(base64.b64encode(TEST_SEED).decode())
        artifact_file = tmpdir / "artifact.bin"
        artifact_file.write_bytes(data)
        out = subprocess.run(
            [str(SIGN_UPDATE), "-f", str(key_file), str(artifact_file)],
            capture_output=True, text=True, check=True).stdout
    match = SIGN_UPDATE_RE.search(out)
    if not match:
        sys.exit(f"sign_update output did not match the expected shape: {out!r}")
    sig_b64, length = match.group(1), int(match.group(2))
    if length != len(data):
        sys.exit(f"sign_update length {length} != artifact length {len(data)}")
    if not ed.verify(data, base64.b64decode(sig_b64), pub):
        sys.exit("parity self-check failed: sign_update signature does not pass the "
                 "reference verifier — investigate before committing anything.")
    return sig_b64


def appcast(signature_b64, length, include_signature=True):
    sig_attr = (f' sparkle:edSignature="{signature_b64}"'
                if include_signature else "")
    return f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>Roamux test feed (FIXTURE — dev/test key, roam-32)</title>
    <item>
      <title>99.0.0</title>
      <sparkle:version>99.0.0</sparkle:version>
      <sparkle:minimumSystemVersion>12.0</sparkle:minimumSystemVersion>
      <pubDate>Thu, 01 Jan 2026 00:00:00 +0000</pubDate>
      <enclosure url="https://example.invalid/{ARTIFACT_NAME}"
                 length="{length}"
                 type="application/octet-stream"{sig_attr}/>
    </item>
  </channel>
</rss>
"""


def main():
    pk = ed.publickey(TEST_SEED)
    (HERE / "test_public_ed_key.b64").write_text(
        base64.b64encode(pk).decode() + "\n")

    # A REAL minimal .app zip: Sparkle validates the EdDSA signature after
    # extraction (before install), so extraction must succeed for the
    # signature gate to be reached. Deterministic (fixed timestamps).
    plist = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        '<plist version="1.0"><dict>\n'
        f'  <key>CFBundleIdentifier</key><string>{TESTHOST_ID}</string>\n'
        '  <key>CFBundleName</key><string>TestHost</string>\n'
        '  <key>CFBundleExecutable</key><string>TestHost</string>\n'
        '  <key>CFBundleVersion</key><string>99.0.0</string>\n'
        '  <key>CFBundleShortVersionString</key><string>99.0.0</string>\n'
        # Sparkle only supports EdDSA key rotation, never removal: the new
        # app must carry a key too (same test key).
        f'  <key>SUPublicEDKey</key><string>{base64.b64encode(pk).decode()}</string>\n'
        '</dict></plist>\n')
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        def add(name, data, mode=0o644):
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = (0o100000 | mode) << 16
            z.writestr(info, data)
        add("TestHost.app/Contents/Info.plist", plist)
        add("TestHost.app/Contents/MacOS/TestHost", "#!/bin/sh\nexit 0\n",
            mode=0o755)
    artifact = buf.getvalue()
    (HERE / ARTIFACT_NAME).write_bytes(artifact)

    sig_b64 = sign_with_sparkle(artifact, pk)
    (HERE / "artifact_signature.b64").write_text(sig_b64 + "\n")

    (HERE / APPCAST_NAME).write_text(appcast(sig_b64, len(artifact)))
    # Tampered: a REAL signature over DIFFERENT bytes than the artifact
    # (valid-key/wrong-bytes — the negative fixture's meaning).
    tampered = bytearray(artifact)
    tampered[0] ^= 0xFF
    tam_sig = sign_with_sparkle(bytes(tampered), pk)
    (HERE / APPCAST_TAMPERED_NAME).write_text(
        appcast(tam_sig, len(artifact)))
    (HERE / APPCAST_UNSIGNED_NAME).write_text(
        appcast("", len(artifact), include_signature=False))
    print("fixture regenerated (test key, sign_update-signed, deterministic)")


if __name__ == "__main__":
    main()
