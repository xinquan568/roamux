<!-- SPDX-License-Identifier: Apache-2.0 -->
# Contributing to Roamux

## One-time setup: install the git hooks

```sh
bash scripts/install-hooks.sh    # sets core.hooksPath=.githooks (per clone)
```

This activates (roam-38, §7.9):
- **pre-commit** — SPDX Apache-2.0 header + secret-scan + overlay-structure over staged files.
- **commit-msg** — Conventional Commits syntax (`<type>(<scope>): <subject>`), types
  `feat|fix|chore|docs|test|refactor|perf|build|ci|style|revert`.
- **pre-push** — the hermetic test suite (and the touched gtests where a Chromium checkout is set up).

The one test that mounts a real disk image (`test_dmg_preserves_symlinks_and_exec_bits`) is **opt-in per
tier** and does *not* run here (roam-261): it was 35–45% of this gate's wall clock and is the least
hermetic member of a suite whose contract is hermeticity. It runs where a tier sets `REQUIRE_DMG_MOUNT=1`
— tier-2 (`roamux/build/ci/tier2_job.sh`) and the nightly hosted suite. Run it locally with
`REQUIRE_DMG_MOUNT=1 python3 -m unittest roamux.build.tests.test_release_signing`; the pre-push hook
strips the variable, so a `git push` never mounts. Full rationale, and why the other two options in
roam-261 were rejected, sits above `dmg_mount_decision` in `roamux/build/tests/test_release_signing.py`.

## `--no-verify` will not help you

The CI `governance` job runs the **same checker scripts** (`scripts/checks/*.py`) over the PR's changed
files and commit messages, so bypassing the local hook is caught at the PR. The hooks are for fast local
feedback; CI is the gate.

SPDX headers are required on the code/build/CI/script set (`.cc/.h/.mm/.ts/.py/.sh/.gn/.gni/.yml`).
Exempt: `roamux/chromium_src/**` (upstream BSD copies), `.md` docs, and pure-data files
(`.json`, `CHROMIUM_PIN`, `LICENSE`, `NOTICE`). Issue linkage (`roam-<N>` branch/PR/`Closes #N`) is
enforced separately (roam-39).
