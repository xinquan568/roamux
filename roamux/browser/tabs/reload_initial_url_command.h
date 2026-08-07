// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_TABS_RELOAD_INITIAL_URL_COMMAND_H_
#define ROAMUX_BROWSER_TABS_RELOAD_INITIAL_URL_COMMAND_H_

class Browser;

namespace content {
class WebContents;
}

namespace roamux::tabs {

// IDC_RELOAD_INITIAL_URL behavior (roam-12 / I-2.3), over the roam-11
// contract. Compiled into //chrome/browser/ui via patch 0010's sources list.

// The per-tab form of the predicate (roam-268, plan §3.3): flag on, and this
// tab has a captured/user-set initial URL that is valid. Lifted out of
// CanReloadInitialUrl so a caller can ask the question of ANY tab rather than
// only the active one — roam-269's refresh-all run evaluates it at dequeue.
// Null-tolerant.
bool CanReloadInitialUrlForContents(content::WebContents* contents);

// Enabled-state: flag on, the active tab has a captured/user-set initial URL,
// and it is valid. A fresh NTP/blank tab has none (the roam-11 ignorable-start
// rule), so the command is disabled there. Exactly the predicate above, applied
// to the active tab.
bool CanReloadInitialUrl(const Browser* browser);

// Loads the active tab's initial_url with user-typed semantics. No-op when
// CanReloadInitialUrl is false.
void ReloadInitialUrl(Browser* browser);

}  // namespace roamux::tabs

#endif  // ROAMUX_BROWSER_TABS_RELOAD_INITIAL_URL_COMMAND_H_
