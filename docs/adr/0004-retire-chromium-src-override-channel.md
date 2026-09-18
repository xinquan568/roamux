<!-- SPDX-License-Identifier: Apache-2.0 -->
# 0004. Retire the `chromium_src` override channel

## Status
Accepted (2026-09-18, roam-300). Supersedes the channel-2 decision of [0001](0001-chromium-overlay-strategy.md)
(the `chromium_src` include-redirect override); 0001's other decisions stand.

## Context
ADR 0001 named three integration channels. The second — a `chromium_src` include-redirect — was carried by
patch 0002 (one line prepending `//roamux/chromium_src` to GN's `default_include_dirs`, so any `#include` of a
mirrored path resolved to the overlay copy in every target keeping that default config), one override header
(`chromium_src/chrome/common/chrome_isolated_world_ids.h`: an upstream copy plus an inert
`#define ROAMUX_CHROMIUM_SRC_OVERRIDE_ACTIVE`), and the §12.5 staleness gate (`override_signatures.json`,
`check_override_staleness.py`, its hermetic test, a tier-2 phase, an uprev runbook step). Patch 0003 was a
sibling sample: `#define ROAMUX_SAMPLE_PATCH_ACTIVE 1` in `chrome/common/chrome_constants.h`, proving the
`patches/` runhook end-to-end. Both markers were consumed only by `roamux_overlay_mechanism_unittest.cc`.

The Paranoid-mode grill review of 2026-08-26 (finding M24) found the channel maintained a mechanism nothing
real used: the only override was byte-identical to upstream apart from its marker; the channel's own README
recorded that the redirect could not replace a `.cc` and that every real need had gone to a patch; the
runhook was already proven by `test_apply_patches.py`, so 0003's marker was redundant. What the channel cost
was permanent: patch 0002 changed the compile command line of essentially every translation unit (any change
to it is a cold rebuild), patch 0003 sat in a header included by hundreds of translation units and
re-contexted at every uprev, and the staleness gate ran on every tier-2 job for one inert file. The
maintainer decided on 2026-08-27 to retire the channel rather than keep it and move the marker.

## Decision
We will retire the `chromium_src` override channel end-to-end: patches 0002 and 0003, the override and its
README, `override_signatures.json`, `check_override_staleness.py` and its test, the mechanism unittest and its
`BUILD.gn` line, the tier-2 `staleness` phase, the uprev runbook step, and the governance special-cases that
existed only for the channel (the structure checker's `chromium_src` allowance and the SPDX exemption). A
hermetic retirement oracle (`roamux/build/tests/test_channel_retirement.py`) keeps the channel out of every
operational file — GN, Python, shell, hooks, workflows, C++ and the added lines of every patch hunk — while
historical records (this ADR, patch preambles, the grill report, 0001's body) may still name it.

Upstream is now changed through exactly one channel: a minimal, fail-loud `patches/` entry recorded in the
§12.2 inventory (`roamux/patches/README.md`), plus additive `//roamux` code. A whole-file header override
**re-enters only through a fresh ADR** that names the override, its staleness obligation (how its pristine
upstream counterpart is fingerprinted and re-reviewed at every uprev) and its owner; the retired tooling can
be recovered from history (`git log -- roamux/build/check_override_staleness.py`) if that day comes.

As the prerequisite the issue named, a minimal one-owner-per-compiled-source check
(`roamux/build/tests/test_source_ownership.py`) now proves every `roamux/**/*.{cc,mm}` is listed by exactly
one owner — a `sources` entry in a `roamux/**/BUILD.gn`, or a patch-added `"//roamux/…"` source line — with
the one known double-owner (grill M25) pinned in an explicit register until the H13 umbrella (#297) drains it.

## Consequences
- **Easier:** the stack drops from 59 to 57 patches and loses its widest-blast-radius hunk (0002) before the
  M150/M151 uprev; tier-2 loses a phase; the uprev runbook loses a step; the "three channels" model in 0001
  becomes two channels with one named exception path (a fresh ADR).
- **Harder:** the one-day-to-re-add escape hatch is gone — a real header override now costs an ADR first. The
  channel's own history shows the hatch was never used.
- **One-time cost:** removing 0002 changes compile flags broadly, so the first build on the new stack is a
  cold build in whichever build directory sees the new flags first (the PR's tier-2 run pays it; `out/CI` is
  retained, so the post-merge run is warm again unless something else changes the stack or configuration).
- **Migration of existing checkouts:** ordinary `apply_patches.py` (and `--check`) compare only the paths the
  *current* stack touches, so a checkout that still carries 0002/0003 reports `[applied]` while the retired
  hunks survive. Restore them with `apply_patches.py --reconcile` (tier-2 and release already do), or
  `git checkout <pin> -- build/config/compiler/BUILD.gn chrome/common/chrome_constants.h`, and verify both
  files against `git show <pin>:<path>`.
