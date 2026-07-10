#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# roam-33: materialize Apple signing material into a TEMPORARY keychain +
# temp notary key file, for the release job only (tier-3, protected env).
# Secrets arrive via env from the `release` GitHub Environment; nothing is
# echoed, and a trap wipes all temp material on exit. Prints two eval-able
# lines: ROAMEX_SIGN_IDENTITY=... and ROAMEX_NOTARY_KEY_PATH=...
set -euo pipefail

: "${ROAMEX_DEVELOPER_ID_CERT_P12:?}"
: "${ROAMEX_DEVELOPER_ID_CERT_PASSWORD:?}"
: "${ROAMEX_NOTARY_PRIVATE_KEY:?}"

WORK="$(mktemp -d)"
KEYCHAIN="$WORK/roamex-signing.keychain-db"
KEYCHAIN_PW="$(openssl rand -hex 16)"
NOTARY_KEY="$WORK/notary_key.p8"

cleanup() {
  security delete-keychain "$KEYCHAIN" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

# Import the Developer ID cert into an isolated keychain.
printf '%s' "$ROAMEX_DEVELOPER_ID_CERT_P12" | base64 --decode > "$WORK/cert.p12"
security create-keychain -p "$KEYCHAIN_PW" "$KEYCHAIN"
security set-keychain-settings -lut 21600 "$KEYCHAIN"
security unlock-keychain -p "$KEYCHAIN_PW" "$KEYCHAIN"
security import "$WORK/cert.p12" -k "$KEYCHAIN" \
  -P "$ROAMEX_DEVELOPER_ID_CERT_PASSWORD" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple:,codesign: \
  -k "$KEYCHAIN_PW" "$KEYCHAIN" >/dev/null
security list-keychains -d user -s "$KEYCHAIN" \
  "$(security list-keychains -d user | tr -d '"' | xargs)"
rm -f "$WORK/cert.p12"

# The signing identity (Developer ID Application) — the CN, not the secret.
IDENTITY="$(security find-identity -v -p codesigning "$KEYCHAIN" \
  | awk -F'"' '/Developer ID Application/{print $2; exit}')"

# Notary API key file.
printf '%s' "$ROAMEX_NOTARY_PRIVATE_KEY" | base64 --decode > "$NOTARY_KEY"
chmod 600 "$NOTARY_KEY"

echo "ROAMEX_SIGN_IDENTITY=${IDENTITY}"
echo "ROAMEX_NOTARY_KEY_PATH=${NOTARY_KEY}"
# NB: the caller must run within this process's trap window (source it) or the
# release workflow keeps WORK alive for the sign+notarize step then re-cleans.
