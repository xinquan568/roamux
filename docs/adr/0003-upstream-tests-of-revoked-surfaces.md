<!-- SPDX-License-Identifier: Apache-2.0 -->
# 0003. Upstream tests of deliberately revoked surfaces

## Status
Accepted (roam-250, 2026-08-01 — maintainer decision recorded on issue #250)

## Context

ADR 0001 fixes where *product* code lives: additive `//roamux`, upstream touched only through a
declared `chromium_src` override or a minimal `patches/` entry. It says nothing about the symmetric
case on the test side. Roamux does not only add to Chromium — it **replaces** surfaces, and upstream
ships tests asserting the surfaces it replaces. Those tests then fail, correctly describing an
upstream contract that no longer holds here. Nothing recorded what to do about them.

The instance that forced the question (roam-250): six cases in
`chrome/browser/ui/tabs/vertical_tab_strip_state_controller_interactive_uitest.cc` fail with the
stack applied. All six trace to one guard. Patch 0008 (roam-183) adds
`!base::FeatureList::IsEnabled(roamux::features::kTabStripPosition) &&` to the entry condition of the
upstream vertical-tabs menu group in `chrome/browser/ui/views/frame/system_menu_model_builder.cc`,
because with the Roamux flag on the placement is the sole tab-position control. `kTabStripPosition`
is default-on (roam-185) and the upstream fixtures pin only `tabs::kVerticalTabs` /
`tabs::kVerticalTabsExpandOnHover`, so the guard fires and the whole group is absent. The
expand-on-hover item is nested *inside* that same block, which is why one guard produces two distinct
assertion families rather than two independent causes.

Three earlier members of this family were each resolved differently, and none recorded a rule:

- **roam-244** (#244, PR #251) — upstream unit fixtures CHECK-crashed on an unregistered Roamux pref.
  Resolved primarily in **production code**: the placement accessors in `//roamux/common` were made
  total over unregistered registries. Patch `0061` then pinned the flag off for the four residual
  cases that assert the replaced contract.
- **roam-249** (#249) — `TabMetricsProviderTest` put a sample in the wrong bucket. Resolved by the
  fixture hunk in patch `0062`, landed alongside roam-254's production fix.
- **roam-254** (#254, PR #255) — the same *shape* of upstream disagreement, but a **real defect**:
  `TabMetricsProvider` still read `prefs::kVerticalTabsEnabled`, the pref roam-182's migration
  clears, while the display followed the Roamux placement. Shipped builds reported horizontal
  telemetry for users looking at a vertical strip. Fixed in production code.

Two facts decided roam-250 specifically. First, the contract the six cases would be rewritten to
assert **is already asserted, and enforced**: `roamux/test/roamux_hide_upstream_tab_controls_browsertest.cc`
checks the system menu for neither `IDC_TOGGLE_VERTICAL_TABS` nor
`IDC_TOGGLE_VERTICAL_TABS_EXPAND_ON_HOVER` (`:115-122` — the two families behind the four timeouts),
the tools/app menu (`:107-111`), the tab context menu with the Roamux submenu in its place
(`:92-103`), and carries a flag-off control asserting the stock surfaces return (`:139-160`). That
file is compiled into `roamux_browsertests`, which `roamux/build/ci/tier2_job.sh` builds (`:78`) and
runs under the `Roamux*` filter (`:97`), as the `targeted-suite-selfhosted` required check. Second,
**no job builds `interactive_ui_tests`** — the target appears in no workflow or build script.

Both repair options would preserve real behaviour that the overlay suite does not cover — the overlay
suite checks command **presence and absence**, while the six upstream cases additionally drive toggle
activation through the native system menu, round-trip expand-on-hover, and check badge type and
visibility across a vertical↔horizontal transition. What neither option can do is make any of that
*enforced*, and each carries a standing cost:

- **Pin the flag off per case** (the `0061`/`0062` idiom). Every `patches/` entry is a standing
  obligation at each Chromium re-pin: `roamux/build/apply_patches.py` fails loudly on a patch that no
  longer applies, and `tier2_job.sh:44` runs it. Patch `0061` already carries that cost for an
  upstream test file no job compiles, so this would deepen an existing liability rather than create a
  new kind — more rebase surface, still no CI-enforced signal, and duplicating the flag-off control
  that already exists at `:139-160`.
- **Rewrite them to assert suppression.** The largest and most brittle upstream diff, to produce a
  second copy of `:115-122` in a binary nothing runs. The badge cases *are* rewritable — asserting
  that `GetModelAndIndexForCommandId` returns `false` would express the suppression correctly, rather
  than the current silent fall-through to index `0` — but that assertion is exactly what
  `:115-122` already makes, under CI, in Roamux's own terms.

## Decision

We will **not repair upstream tests in place when they assert a surface Roamux deliberately
revoked.** The Roamux contract is asserted in the overlay suite, where CI enforces it; the upstream
cases are recorded here as known-divergent and left failing. No `patches/` entry, no upstream
rewrite.

This rule applies **only when both halves hold**:

1. **The divergence is intended product behaviour** — traced to a specific, deliberate revocation
   (here: patch 0008 / roam-183 suppressing the upstream toggle because the Roamux placement is the
   sole tab-position control); **and**
2. **the Roamux contract is asserted in an overlay suite that CI actually runs** — not merely
   assertable, but compiled into a target `tier2_job.sh` builds and executes.

Both halves are required. If (1) fails, the failure is a defect and is fixed in production code. If
(2) fails, the contract is unasserted anywhere and documenting the divergence would trade a red test
for no test — the overlay coverage comes first, and only then does this rule apply.

**An upstream test failure is not self-certifying as intended divergence.** roam-254 is the standing
counter-example: it looked exactly like this family — an upstream test disagreeing with a Roamux
contract change — and it was a real, user-visible defect, fixed in production code rather than
documented away. Half (1) is a claim that must be *traced*, the way roam-244 traced this family, not
a description of how the failure feels. When the tracing is inconclusive, the failure is a defect.

## Consequences

- **Register — the six known-divergent cases.** All are in
  `chrome/browser/ui/tabs/vertical_tab_strip_state_controller_interactive_uitest.cc` (line numbers
  are M149-relative and will drift; fixture and case names are the durable identifiers). The four
  `VerticalTabStripInteractiveUiTest` cases are `MAYBE_`-disabled on Windows only, so they are live
  on macOS, the platform Roamux ships.

  | Case | Line | Failure signature |
  | ---- | ---- | ----------------- |
  | `VerticalTabStripInteractiveUiTest.VerifyTabsToTheSideButton` | `:69` | **A** |
  | `VerticalTabStripInteractiveUiTest.VerifyTabsToTheTopButton` | `:93` | **A** |
  | `VerticalTabStripInteractiveUiTest.EnablingExpandOnHoverSystemContextMenu` | `:125` | **A** |
  | `VerticalTabStripInteractiveUiTest.DisablingExpandOnHoverSystemContextMenu` | `:160` | **A** |
  | `VerticalTabStripMenuInteractiveUiTest.ShowBadgeInContextMenuToggle/NewBadge` | `:210` | **B** |
  | `VerticalTabStripMenuInteractiveUiTest.ShowBadgeInContextMenuToggle/PreviewBadge` | `:210` | **B** |

  **Signature A — non-fatal expectation, then a timeout.** The case first fails
  `EXPECT_TRUE(SystemMenuContainsStringId(...))` on `IDS_SWITCH_TO_VERTICAL_TAB`,
  `IDS_SWITCH_TO_HORIZONTAL_TAB`, or `IDS_VERTICAL_TABS_{ENABLE,DISABLE}_EXPAND_ON_HOVER`, then
  blocks in `WaitForShow(...)` on `kToggleVerticalTabsElementId` or
  `kToggleVerticalTabsExpandOnHoverElementId`, which the suppressed menu can never show. **Timeout is
  the terminal outcome.**

  **Signature B — synchronous missing-badge assertion.** These cases never call
  `SystemMenuContainsStringId`. With `IDC_TOGGLE_VERTICAL_TABS` suppressed,
  `GetModelAndIndexForCommandId` returns `false`; the case ignores that return, leaves
  `command_index` at `0`, so `GetNewBadgeTypeAt(0)` reads an unrelated menu item, and it fails
  `ASSERT_TRUE(badge_type.has_value())`. (The issue body originally attributed these to
  `SystemMenuContainsStringId`; that was wrong.)

  A failure matching neither signature, or a seventh case, is **not** covered by this record and must
  be traced afresh.

- **`MaybeShowNewBadge` is bypassed, and that is correct.** Constructing the suppressed toggle would
  call `UserEducationService::MaybeShowNewBadge` for `tabs::kVerticalTabsPreviewBadge` /
  `tabs::kVerticalTabsNewBadge`, persisting `show_count` and recording badge-shown metrics; the guard
  skips that path while both features stay registered. A badge that is never shown recording nothing
  is coherent with the suppression — no user-visible surface is misreported — so this is **not** a
  second roam-254. Recorded so it reads as examined rather than overlooked.

- **Obligation:** if `interactive_ui_tests` is ever brought into CI, these six cases must be pinned
  (the `0061`/`0062` flag-pin idiom) or removed **in the same change that adds the suite** — a red
  suite cannot serve as a gate, and a gate that arrives red will be turned off rather than fixed.

- **Easier:** the upstream patch surface stays minimal — this decision adds nothing to the 58-file
  stack, and therefore nothing to the per-re-pin rebase cost. Contract coverage stays in one place,
  `roamux/test`, where CI runs it and where it is expressed in Roamux's own terms.

- **Harder:** `interactive_ui_tests` is deliberately left non-green under the current stack — until
  these cases are reconciled under the re-entry condition above — so anyone running it locally must
  consult this register to separate the known six from a real regression. That cost is why the
  register records failure signatures and not just names, and it is bounded by the same fact that
  made the decision cheap: no CI job runs the suite today. Local-only behavioural coverage of the
  suppressed surfaces (toggle activation, expand-on-hover round-trip, badge transitions) is given up
  with them; the suppression contract itself is not.
