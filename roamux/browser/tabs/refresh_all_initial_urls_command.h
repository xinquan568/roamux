// SPDX-License-Identifier: Apache-2.0
// roam-269: IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS — refresh every eligible tab in
// a window to its initial URL, paced by roam-268's InitialUrlRefreshScheduler.
// Compiled into //chrome/browser/ui via patch 0065's sources list.
#ifndef ROAMUX_BROWSER_TABS_REFRESH_ALL_INITIAL_URLS_COMMAND_H_
#define ROAMUX_BROWSER_TABS_REFRESH_ALL_INITIAL_URLS_COMMAND_H_

class Browser;
class BrowserWindowInterface;

namespace roamux::tabs {

// Enabled-state for the command (plan §4.6). Deliberately a COARSE, STATIC
// condition: the flag is on and this is a normal tabbed window. It is
// emphatically NOT an any-tab-eligible predicate — BrowserCommandController::
// ExecuteCommandWithDisposition returns early when !IsCommandEnabled(id),
// BEFORE dispatch, and a direct initial-URL edit notifies no observer, so a
// stale-false bit would make the chord permanently inert with no
// execution-time recompute able to rescue it. Eligibility is evaluated inside
// the run, at dequeue (§3.3).
//
// Takes BrowserWindowInterface* rather than the Browser* the issue sketched:
// Browser derives from it, so one predicate serves both the command-controller
// site and the shortcut_registry_mac event-window seam, which already resolves
// BrowserWindowInterface* (the CanToggleTabStrip precedent, roam-214).
bool CanRefreshAllInitialUrls(BrowserWindowInterface* browser);

// Starts a run over `browser`'s tabs, or CANCELS the run already in flight for
// that browser (§3.5 C1 — a second press cancels; the loading tab is left
// alone because it is already navigating). No-op when
// CanRefreshAllInitialUrls is false.
void RefreshAllInitialUrls(Browser* browser);

}  // namespace roamux::tabs

#endif  // ROAMUX_BROWSER_TABS_REFRESH_ALL_INITIAL_URLS_COMMAND_H_
