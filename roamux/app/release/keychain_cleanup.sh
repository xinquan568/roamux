#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# roam-33: remove the temporary signing material. Run in an always() workflow
# step so secrets never persist on the runner, even if signing failed.
set -uo pipefail
WORK="${RUNNER_TEMP:-/tmp}/roamux-signing"
if [ -n "${ROAMUX_SIGNING_KEYCHAIN:-}" ]; then
  security delete-keychain "${ROAMUX_SIGNING_KEYCHAIN}" 2>/dev/null || true
fi
rm -rf "$WORK"
# roam-286 (grill M42): release runs before roam-286 imported the Sparkle SIGNING key into the
# login keychain under this account for staging validation, with a trap as the only cleanup; the
# workflow no longer does, so this removes any leftover from an interrupted earlier run. ACCOUNT-
# scoped on purpose: the service name is shared with the operator's own Sparkle keys, which must
# never be touched from CI. Absent item (errSecItemNotFound) is the normal case.
security delete-generic-password -s "https://sparkle-project.org" -a "roamux-release-verify" >/dev/null 2>&1 || true
echo "[ok] signing material removed"
