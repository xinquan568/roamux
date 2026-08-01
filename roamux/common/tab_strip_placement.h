// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_COMMON_TAB_STRIP_PLACEMENT_H_
#define ROAMUX_COMMON_TAB_STRIP_PLACEMENT_H_

#include "ui/gfx/geometry/rect.h"

class PrefService;

namespace roamux {

// Where the tab strip is docked (E1, flag roamux::features::kTabStripPosition).
// The numeric values are the persisted integers of prefs::kTabStripPosition —
// the forward contract I-1.2/I-1.3/I-1.4 consume; never renumber (stored user
// state depends on it).
//
// Placements name PHYSICAL window edges and are RTL-invariant (roam-9 D1):
// kLeft is the visual left in every UI direction, matching the upstream
// vertical strip's physical-left dock at this pin and the literal UI labels.
enum class TabStripPlacement {
  kTop = 0,  // Chromium default (the registered pref default).
  kBottom = 1,
  kLeft = 2,
  kRight = 3,
};

// The effective placement. kTop when the feature flag is off, `pref_service` is
// null, or the stored value is out of range (prefs are user-editable on disk) —
// consumers can never half-honor a placement while the epic flag is off.
TabStripPlacement GetTabStripPlacement(const PrefService* pref_service);

// Persists `placement`. No-op when `pref_service` is null.
void SetTabStripPlacement(PrefService* pref_service,
                          TabStripPlacement placement);

// The bottom-docked strip band (roam-7 / I-1.2): `strip` is the band carved
// off the bottom of `client_area`; `remaining` is everything above it.
struct BottomStripLayout {
  gfx::Rect strip;
  gfx::Rect remaining;
};

// Splits `client_area` into a bottom band of `strip_height` (clamped to
// [0, client_area.height()]) and the remaining area above it.
BottomStripLayout ComputeBottomStripLayout(const gfx::Rect& client_area,
                                           int strip_height);

// roam-8 (I-1.3): true when the roamux placement asks for a vertical strip
// (kLeft/kRight, flag on). roam-182: under the sole-authority contract this is
// the whole display decision — patch 0008 returns it directly rather than
// OR-ing it with the upstream vertical-tabs pref.
bool ShouldDisplayVerticalTabsForPlacement(const PrefService* pref_service);

// roam-8 (I-1.3): true when the vertical strip should dock at the RIGHT edge.
// roam-182: follows placement kRight ALONE (the old upstream-pref exception,
// which pinned the dock to the leading edge when upstream vertical tabs were
// on, is removed — it was the "placement does nothing" bug).
bool ShouldDockVerticalTabStripRight(const PrefService* pref_service);

// roam-9 (I-1.4): true when the vertical strip is roamux-driven onto the
// physical LEFT edge (placement kLeft). roam-182: placement alone (see
// ShouldDockVerticalTabStripRight). Placements are physical and RTL-invariant
// (D1); layout code flips to logical coordinates at the call site.
bool ShouldDockVerticalTabStripLeft(const PrefService* pref_service);

// roam-228: the strip's LOGICAL dock side — the physical placement XOR the UI
// direction. This is the translation the two predicates above deliberately do
// NOT do: placements are physical and RTL-invariant (roam-9 D1), but views
// stores child bounds LOGICALLY and mirrors them to physical at paint/hit-test
// time (View::GetMirroredX), and views::ResizeArea hands its delegate an
// already-RTL-normalised delta. Consumers working in either of those logical
// spaces — the browser-layout geometry (roam-205) and the resize handle/delta
// (roam-228) — need this; consumers expressing a physical rule (e.g. roam-206's
// collapse arrow) must keep using the physical predicates.
//
// False whenever the roamux placement does not drive the vertical display
// (kTop/kBottom, or the flag off), so a flag-off build reduces exactly to
// upstream's logical-leading dock.
//
// `is_rtl` is a parameter rather than a base::i18n::IsRTL() call so this stays
// a pure function and //roamux/common stays free of //base/i18n; call sites
// pass base::i18n::IsRTL().
bool IsVerticalTabStripOnLogicalTrailingEdge(const PrefService* pref_service,
                                             bool is_rtl);

// roam-254: whether this profile's tab strip is effectively vertical, for
// read-side consumers that must describe what the USER SEES rather than what a
// single pref says. Mirrors patch 0008's authority split exactly:
//
//   kTabStripPosition ON  -> the roamux placement is the sole authority
//                            (roam-182); the upstream pref is cleared by the
//                            migration and must not be consulted.
//   kTabStripPosition OFF -> the upstream pref remains authoritative and is
//                            read EXACTLY as upstream reads it, including its
//                            CHECK on an unregistered pref. Adding a
//                            FindPreference fallback here would turn that
//                            CHECK into a silent false — a behavioural change
//                            on a path roam-254 does not own.
//
// Introduced because Tabs.VerticalTabs.* reported horizontal for migrated
// default-on profiles: TabMetricsProvider read the pref roam-182 clears while
// the display followed the placement.
//
// BOUNDED DISCREPANCY: this is a LIVE pref read, whereas
// VerticalTabStripStateController caches its placement contribution and freezes
// it while an enable-state lock is held. During such a lock this can disagree
// with the mode a window is actually displaying. The window is bounded by the
// lock; consumers sampling session-level state (metrics) tolerate it, consumers
// needing the displayed mode must ask the controller.
bool IsVerticalTabStripEffectivelyEnabled(const PrefService* pref_service);

}  // namespace roamux

#endif  // ROAMUX_COMMON_TAB_STRIP_PLACEMENT_H_
