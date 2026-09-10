// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_TABS_SESSION_TAB_EXTRA_DATA_H_
#define ROAMUX_BROWSER_TABS_SESSION_TAB_EXTRA_DATA_H_

#include <memory>
#include <vector>

#include "components/sessions/core/session_id.h"

namespace content {
class WebContents;
}

namespace sessions {
class SessionCommand;
}

namespace roamux::tabs {

// Renders the Roamux per-tab session "extra data" of `contents` as
// AddTabExtraData commands keyed by `tab_id` (roam-320).
//
// WHY THIS EXISTS. The helpers announce their values once, through
// SessionService::AddTabExtraData, which appends a command to the session log.
// But SessionService periodically REBUILDS that log from live browser state
// (every 250 commands, when a navigation index leaves the archived range, when
// history entries are deleted, at service construction), and its builders emit
// no extra-data command of any kind — so the rebuild erases the initial URL,
// its lock bit and the durable tab uid. On the next launch nothing is restored,
// the initial-URL helper stays uncaptured, and the restored tab's first commit
// is captured as its initial URL.
//
// WHY IT READS RATHER THAN REMEMBERS. Mirroring what the producers wrote is not
// enough: a restored tab is armed with its value immediately but defers its
// session write to the first committed navigation (so a restored, never-loaded
// tab has state nothing has announced), and a discard gives the tab NEW contents
// with a NEW SessionID while copying the helper state across unpersisted. The
// tab's live helpers are the only source that is correct in both cases, so this
// is a read at serialization time.
//
// Contract: it looks helpers up and never creates them, never mints an
// identity and never starts a persist; it needs no tab-strip index, so it is
// equally correct on the reset path and on TabRestored (which passes -1); it
// emits only the Roamux keys; and it returns an empty vector when the producers'
// features are off, when no helper is attached, or when nothing is captured.
std::vector<std::unique_ptr<sessions::SessionCommand>>
BuildTabExtraDataCommands(content::WebContents* contents, SessionID tab_id);

}  // namespace roamux::tabs

#endif  // ROAMUX_BROWSER_TABS_SESSION_TAB_EXTRA_DATA_H_
