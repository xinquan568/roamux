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
