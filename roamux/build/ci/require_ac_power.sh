#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# roam-258: refuse to start a long self-hosted job while the builder is on battery.
#
# The builder idle-sleeps after ONE minute on battery (`pmset -b sleep 1`) and a long compile does
# not count as user activity. The machine sleeps, the runner stops renewing the job lease, GitHub
# invalidates it, and the job is reported as a failure with NO failing step ~30-50 minutes later.
# Failing here costs ~2 seconds and says why.
#
# This gate is the DETERMINISTIC half of the fix. tier2_job.sh also re-execs under `caffeinate`,
# but that is unverified risk reduction aimed at a machine unplugged MID-RUN, which no start-time
# check can see. Do not read the two as interchangeable.
#
# Fails CLOSED: an unreadable or unrecognised power state is an error, never an assumed AC.
set -euo pipefail

ALLOW="${ROAMUX_ALLOW_BATTERY:-}"

if ! batt="$(pmset -g batt 2>&1)"; then
  echo "::error::roam-258 power gate: could not determine power source (pmset failed: ${batt})" >&2
  exit 1
fi

case "${batt}" in
  *"AC Power"*)
    echo "power gate: on AC power — proceeding."
    exit 0
    ;;
  *"Battery Power"*)
    ;;
  *)
    echo "::error::roam-258 power gate: could not determine power source from pmset output: ${batt}" >&2
    exit 1
    ;;
esac

# --- on battery ---------------------------------------------------------------------------------
# The message IS the triage artifact. A job that dies later with no failing step is ambiguous
# between idle sleep and a builder network/DNS drop, and the two have opposite correct responses.
read -r -d '' DISCRIMINATOR <<'EOF' || true
  How to tell this apart from a builder network drop, if a job dies later with no failing step:
    * idle sleep  -> _diag/Runner_*.log shows a last "Successfully renew job" then TOTAL SILENCE for
                     tens of minutes, with network errors appearing only on wake. Fix: plug it in.
    * network drop -> continuous renewal ATTEMPTS right up to the failure, failing with
                     TimedOut/HostNotFound. Fix: nothing — let the next run supply the signal.
EOF

if [ "${ALLOW}" = "1" ]; then
  echo "::warning::roam-258 power gate: builder is on BATTERY; proceeding because ROAMUX_ALLOW_BATTERY=1."
  echo "This override WAIVES the guarantee: the job may still be killed mid-run by idle sleep and" >&2
  echo "report as a failure with no failing step. caffeinate reduces that risk but is unverified" >&2
  echo "against an aggressive battery sleep setting." >&2
  echo "${DISCRIMINATOR}" >&2
  exit 0
fi

echo "::error::roam-258 power gate: builder is on BATTERY — refusing to start a long job." >&2
cat >&2 <<EOF
Plug the builder into AC and re-run. On battery this machine idle-sleeps after ~1 minute, which
kills the job ~30-50 minutes later with no failing step rather than failing here in 2 seconds.
Set ROAMUX_ALLOW_BATTERY=1 to override (this waives the guarantee — see docs/ci/self-hosted-runner.md).

${DISCRIMINATOR}
EOF
exit 1
