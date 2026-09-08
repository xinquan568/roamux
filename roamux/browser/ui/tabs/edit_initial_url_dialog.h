// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_UI_TABS_EDIT_INITIAL_URL_DIALOG_H_
#define ROAMUX_BROWSER_UI_TABS_EDIT_INITIAL_URL_DIALOG_H_

#include <string>

namespace content {
class WebContents;
}

namespace roamux::tabs {

// Shows the tab-modal "Edit initial URL" dialog (roam-14, §4.5): textfield
// prefilled with the current value. OK writes through
// TabInitialUrlHelper::SetUserInitialUrl (locks) when the text, trimmed of
// ASCII whitespace and containing none inside, fixes up (url_formatter::FixupURL,
// bare host -> http://) to an ALLOWED initial URL — http, https or exactly
// about:blank (roam-289 / grill M18: a stored javascript: value would execute
// in the tab's current document on every replay). Anything else is a no-op.
void ShowEditInitialUrlDialog(content::WebContents* contents);

// Testing seam: the dialog's validate+accept path without native UI —
// returns false (no write) when `text` is empty, has internal ASCII
// whitespace, or does not fix up to an allowed initial URL.
bool SubmitEditInitialUrlForTesting(content::WebContents* contents,
                                    const std::string& text);

}  // namespace roamux::tabs

#endif  // ROAMUX_BROWSER_UI_TABS_EDIT_INITIAL_URL_DIALOG_H_
