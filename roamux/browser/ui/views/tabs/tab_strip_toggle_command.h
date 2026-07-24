// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_UI_VIEWS_TABS_TAB_STRIP_TOGGLE_COMMAND_H_
#define ROAMUX_BROWSER_UI_VIEWS_TABS_TAB_STRIP_TOGGLE_COMMAND_H_

#include "base/functional/callback.h"

class BrowserWindowInterface;

namespace roamux::tabs_toggle {

// roam-214: the always-built command shim (plan F5 resolution). Patch 0053
// wires IDC_ROAMUX_TOGGLE_TAB_STRIP into BrowserCommandController against
// these functions; the per-window pin controller glue (patch 0054) installs
// the real handler at construction. With no handler installed (0053-only
// builds, or flag-off windows where the controller is never created) the
// command executes as a deliberate no-op.

// D10 availability: feature flag on AND placement in {left,right} AND the
// upstream vertical-tabs feature active. Also consulted by the macOS
// registry resolver's availability filter (plan R1), so key-event
// resolution and command enablement can never disagree.
bool CanToggleTabStrip(BrowserWindowInterface* browser);

// Executes the toggle for `browser` via the installed handler; no-op
// without one.
void ToggleTabStrip(BrowserWindowInterface* browser);

// Handler registration (patch-0054 glue; per-window lifetime).
void SetToggleHandler(BrowserWindowInterface* browser,
                      base::RepeatingClosure handler);
void ClearToggleHandler(BrowserWindowInterface* browser);

}  // namespace roamux::tabs_toggle

#endif  // ROAMUX_BROWSER_UI_VIEWS_TABS_TAB_STRIP_TOGGLE_COMMAND_H_
