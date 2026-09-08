# SPDX-License-Identifier: Apache-2.0
"""Staging validation (roam-34, K2; roam-286): before publishing, prove the DRAFT
release's DOWNLOADED appcast + artifact verify against the committed SUPublicEDKey
— with the PUBLIC key only.

Ed25519 verification needs only the 32-byte public key. Sparkle's CLI verifier has
no public-key-only mode (it takes a private key from the keychain or a file), so
until roam-286 this step imported the production PRIVATE key into the runner's
login keychain to perform a public-key operation, with a bash EXIT trap as the only
cleanup (grill C1/M42). Now: the pure-Python reference verifier (ed25519_ref —
byte-equivalent to Sparkle's signer, enforced by the committed fixture guard in
test_sparkle_fixture.py and by regenerate_fixture.py's parity self-check), no
external tool, no keychain, no private key. In-app verification stays Sparkle's own.

Checks, in this order — any failure stops the release before publish:
  1. SUPublicEDKey present in the plist and 32 bytes of base64;
  2. the appcast has an enclosure carrying a non-empty sparkle:edSignature of
     64 bytes of base64;
  3. the enclosure's `length` is present, an integer, and byte-exact against the
     downloaded artifact;
  4. the signature verifies over the downloaded artifact bytes.
"""

import argparse
import base64
import binascii
import pathlib
import plistlib
import sys
import xml.etree.ElementTree as ET

import ed25519_ref as ed  # same directory: on sys.path both as a script and under the tests

SPARKLE_NS = "http://www.andymatuschak.org/xml-namespaces/sparkle"


class StagingValidationError(Exception):
    """A staging check failed; the message is the reason printed as ::error::."""


def public_key_from_plist(plist_path):
    with open(plist_path, "rb") as f:
        entries = plistlib.load(f)
    key = entries.get("SUPublicEDKey") if isinstance(entries, dict) else None
    if not isinstance(key, str) or not key:
        raise StagingValidationError(f"SUPublicEDKey missing from {plist_path}")
    return key


def _decode(value, nbytes, what):
    try:
        raw = base64.b64decode(value, validate=True)
    except (binascii.Error, ValueError):
        raw = None
    if raw is None or len(raw) != nbytes:
        raise StagingValidationError(f"{what} is not {nbytes} bytes of base64")
    return raw


def enclosure_from_appcast(appcast_path):
    root = ET.fromstring(pathlib.Path(appcast_path).read_bytes())
    enclosure = root.find(".//enclosure")
    if enclosure is None:
        raise StagingValidationError("appcast has no enclosure")
    return enclosure


def verify_enclosure(appcast_path, artifact_path, plist_path):
    """Raise StagingValidationError unless the appcast's edSignature verifies over
    the artifact bytes with the plist's SUPublicEDKey. Returns the artifact size."""
    public_key = _decode(public_key_from_plist(plist_path), 32, "SUPublicEDKey")
    enclosure = enclosure_from_appcast(appcast_path)
    signature_b64 = enclosure.get(f"{{{SPARKLE_NS}}}edSignature")
    if not signature_b64:
        raise StagingValidationError("enclosure has no sparkle:edSignature")
    signature = _decode(signature_b64, 64, "edSignature")
    length = enclosure.get("length")
    if length is None:
        raise StagingValidationError("enclosure length attribute missing")
    try:
        length = int(length)
    except ValueError:
        raise StagingValidationError(f"enclosure length is not an integer: {length!r}") from None
    data = pathlib.Path(artifact_path).read_bytes()
    if length != len(data):
        raise StagingValidationError(f"enclosure length {length} != artifact size {len(data)}")
    if not ed.verify(data, signature, public_key):
        raise StagingValidationError("edSignature does not verify against SUPublicEDKey")
    return len(data)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Verify a Sparkle appcast enclosure against the committed public key.")
    parser.add_argument("--appcast", required=True, type=pathlib.Path)
    parser.add_argument("--artifact", required=True, type=pathlib.Path)
    parser.add_argument("--public-key-plist", required=True, type=pathlib.Path,
                        help="Info.plist fragment carrying SUPublicEDKey (base64)")
    args = parser.parse_args(argv)
    try:
        size = verify_enclosure(args.appcast, args.artifact, args.public_key_plist)
    except StagingValidationError as e:
        print(f"::error::staging validation FAILED — {e}", file=sys.stderr)
        return 1
    print(f"[ok] staging validation passed — appcast edSignature verifies against "
          f"SUPublicEDKey over {size} bytes (public key only; ed25519 reference "
          f"verifier, no keychain)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
