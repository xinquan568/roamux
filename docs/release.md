<!-- SPDX-License-Identifier: Apache-2.0 -->
# Cutting a release

The release job (`.github/workflows/release.yml`, environment `release`, self-hosted builder) turns a
`v*` tag into a Sparkle-signed build. This page is the operator's protocol; the workflow enforces
every rule it can.

## Protocol

1. **Bump `roamux/build/VERSION` in a PR first** (`ROAMUX_VERSION=<marketing version>`, e.g.
   `0.0.1-alpha.10`). The value is compiled into the binary and rides the appcast (roam-156).
2. **Tag the merge commit** with `v<VERSION>` and push the tag. The job's first gates: the ref must
   be `refs/tags/v*`; the tag must equal `VERSION` (`release_version.py --check-tag`); and the
   version must not already be published (below).
3. **Approve the `release` environment deployment** (a required reviewer, Stage 0 of the 2026-08-26
   grill). The job then vendors Sparkle, builds universal2, signs the update, drafts the release,
   staging-validates the downloaded assets, and publishes.

Tags under `refs/tags/v*` are immutable at the platform level: the `release-tags` ruleset forbids
deletion and non-fast-forward updates, with no bypass actors.

## A published version is immutable (roam-285, grill H4)

`CFBundleVersion` (and the appcast's `sparkle:version`) is a pure function of the tag
(`release_version.py`: `0.0.1-alpha.9` → `0.0.1.1.9`). Re-cutting an already-published tag would
stamp the **same** version onto a **different** binary; Sparkle only offers strictly greater
versions, so every user already on the first build would be stranded on it. Therefore:

- The step **Refuse a same-version re-cut** runs right after the tag==VERSION check. It fails the
  job when a release for the tag is already **published** (GitHub's by-tag lookup returns published
  releases only, so a draft left by an interrupted cut does not block a retry), or when the tag
  push was forced. It decides "absent" only from GitHub's error body carrying status `404`; any
  other lookup failure (authentication, transport, server error, malformed body) fails the job —
  never a silent pass.
- The rejection happens **after** environment approval and runner allocation (the job still takes
  its turn in the shared-base queue) but **before** Sparkle vendoring, the build and any release
  object; the job's `always()` overlay-restore step still runs afterwards.
- **A hotfix is a new `VERSION` and a new tag.** There is no same-version path.
- **Legacy tags.** A `workflow_dispatch` runs the workflow file *of the selected ref*, so tags cut
  before this gate (`v0.0.1-alpha.9` and older) still carry the old re-cut behaviour. Never approve a
  `release` deployment dispatched on a pre-gate tag; the mechanical protection covers tags cut from
  this change onwards.

## `latest` follows version order (roam-285, grill M2)

The Sparkle feed is `https://github.com/xinquan568/roamux/releases/latest/download/appcast.xml`, and
the appcast is single-item, so whatever GitHub calls `latest` is what the whole fleet is offered.
The publish step marks a release `latest` only when its version orders at or above the current
latest release's (`release_version.py --tag <TAG> --at-least <LATEST_TAG>`, on the numeric
encoding); an older tag is published with `make_latest=false` and a warning. `latest` here is
GitHub's newest **published, non-prerelease** release — Roamux alphas qualify because the job
publishes with `prerelease=false` (the tag *name* and GitHub's prerelease *flag* are different
things); when none exists yet the new release becomes latest. A lookup failure, a malformed body or
an unorderable tag stops the job before the publish PATCH.

## Interrupted cuts and stale drafts

A cut that fails after `gh release create --draft` leaves a draft behind. The retry passes the
same-version gate (drafts are not "published") and creates a further draft; publish then picks the
first draft whose tag matches and staging downloads assets by tag, so with several drafts present
the retry's selection is not guaranteed. Delete stale drafts by hand before retrying (tracked under
roam-295). A cut that failed **after** publish is, by design, not retryable under the same version.

## Rehearsal obligation

The same-version gate can only be exercised live on a tag whose tree contains it. After
`v0.0.1-alpha.10` is published (roam-294), dispatch the release workflow on that tag once and approve
the deployment: expect rapid rejection measured from the gate step's start — no Sparkle vendoring,
no build, no draft, no asset. The hermetic tests (`test_release_version.py`,
`test_workflow_invariants.py` — the extracted step scripts run against a fake `gh`) prove the
scripts; they do not cover approval, queueing or the overlay restore, which that observation does.

## Key hygiene (roam-286, grill C1 steps 3-5 / M42 / L11)

The Sparkle EdDSA private key (`SPARKLE_ED_PRIVATE_KEY`, the only secret in the `release`
environment) is exposed to **exactly one step** — "Sign updates + generate appcast" — as a `0600` file
under `$RUNNER_TEMP`, created under `umask 077`, removed by an `EXIT` trap and again explicitly. The
workflow never places it in any keychain. Staging validation ("Create DRAFT release +
staging-validate the DOWNLOADED assets") verifies the downloaded appcast and artifact with the
**committed public key only** (`SUPublicEDKey` in `roamux/app/sparkle-Info.plist`) through the
pure-Python reference verifier in `roamux/app/appcast/ed25519_ref.py`; Sparkle's own CLI verifier has
no public-key-only mode, which is why the old step imported the private key into the runner's login
keychain (on the machine and login user that also run every same-repo PR job). The `always()`
cleanup (`keychain_cleanup.sh`) additionally deletes the legacy `roamux-release-verify` keychain
account if a pre-roam-286 run left one behind — account-scoped, never by service name, because the
operator's own Sparkle keys share it. The machine-env file (`~/roamux-runner/.env`) is parsed as
strict `KEY=value` data and never sourced (format contract in `docs/ci/self-hosted-runner.md`).
`test_workflow_invariants.py` (invariant 22) pins all of this; the first live observation of the
public-key-only staging step is the `v0.0.1-alpha.10` cut (roam-294). Still open: the draft-id lookup
interpolates the tag into a `jq` program (grill L11, tracked under roam-295).

## Locale rebrand gate (roam-284)

The rebrand channel (`roamux/build/rebrand_strings.py`) rewrites user-visible
"Chromium" to "Roamux" in the GRIT string sources and their locale `.xtb`
translations. Until roam-284 its token used Python's Unicode `\b`, which treats
Hangul, Han and kana as word characters, so a product mention glued to a particle
or a neighbouring word (`Chromium을`, `从Chromium中`, the normal shape in Korean and
Chinese) never matched and shipped as "Chromium" in those locales. The token now
uses ASCII word classes at both ends, and the release workflow runs a second gate
after the idempotency `--check`:

```
rebrand_strings.py --chromium-src "$CHROMIUM_SRC" --check --verify-locales ko,zh-CN,ja,zh-TW,zh-HK
```

It scans the compiled translations of the five CJK locales for a brand token
immediately adjacent to a CJK code point. Protected forms are exempt because the
channel must leave them alone: `ChromiumOS` / `Chromium OS`, `Chromium Authors`,
`Chromium open source`, and tokens glued to an ASCII identifier character. A hit
fails the cut naming the `.xtb` file and the message id. Tier-2 proves the same
"0 survivors" on every push: `test_rebrand_strings.py` runs the channel in memory
over a pristine `git show` snapshot of the real string units and asserts zero
CJK-adjacent survivors per locale (nothing under the checkout is written).
