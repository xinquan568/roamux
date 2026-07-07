#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Roamex tier-2 CI job (roam-36, plan §12.6 personal-machine v1): warm-base incremental build + the
# Roamex test suites, run on the self-hosted runner. The shared base checkout is touched ONLY via the
# two declared, restored channels: (1) the overlay symlink (flipped to this job's checkout, restored
# by the EXIT trap), (2) the idempotent fail-loud patch runhook. No sudo; no secrets on this tier.
set -euo pipefail

SRC="${ROAMEX_CHROMIUM_SRC:-${HOME}/chromium/src}"
OUT="${ROAMEX_CI_OUT:-out/CI}"
DEPOT="${ROAMEX_DEPOT_TOOLS:-${HOME}/depot_tools}"
ROAMEX_CANONICAL_OVERLAY="${ROAMEX_CANONICAL_OVERLAY:?set in the runner .env — the operator overlay the base symlink is restored to}"

export PATH="${DEPOT}:${PATH}"
SECONDS=0

restore_overlay() {
  ln -sfn "${ROAMEX_CANONICAL_OVERLAY}" "${SRC}/roamex"
  echo "overlay symlink restored to ${ROAMEX_CANONICAL_OVERLAY}"
}
trap restore_overlay EXIT

echo "== tier-2 warm-base job: base=${SRC} out=${OUT} workspace=${GITHUB_WORKSPACE} =="

# Declared channel 1: point the base's overlay at THIS job's checkout.
ln -sfn "${GITHUB_WORKSPACE}/roamex" "${SRC}/roamex"

# Declared channel 2: the runhook (idempotent; fails loudly on conflict — the rebase signal).
python3 "${GITHUB_WORKSPACE}/roamex/build/apply_patches.py" --chromium-src "${SRC}"

# Warm CI build dir: APFS-clone the operator's warm out/Default on first use (copy-on-write).
cd "${SRC}"
if [ ! -d "${OUT}" ]; then
  echo "cloning warm build dir out/Default -> ${OUT} (APFS copy-on-write)"
  cp -Rc out/Default "${OUT}"
fi

autoninja -C "${OUT}" roamex_unittests roamex_browser_unittests
"${OUT}/roamex_unittests"
"${OUT}/roamex_browser_unittests"

# Staleness gate against this job's overlay.
python3 "${GITHUB_WORKSPACE}/roamex/build/check_override_staleness.py" \
  --chromium-src "${SRC}" --overlay "${GITHUB_WORKSPACE}/roamex"

echo "tier-2 job green in ${SECONDS}s (warm incremental)" | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"
