// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMEX_BROWSER_UI_TABS_INITIAL_URL_MENU_H_
#define ROAMEX_BROWSER_UI_TABS_INITIAL_URL_MENU_H_

#include <memory>

namespace content {
class WebContents;
}
namespace ui {
class SimpleMenuModel;
}

namespace roamex::tabs {

// Patch-0012 entry point (roam-14, §4.5): appends the flag-gated
// "Initial URL" submenu to the tab context menu — "Edit initial URL…" (the
// dialog) and "Set initial URL to current page" (one click, locks). Returns
// the submenu model the caller must own (0005 pattern), or null when the
// flag is off or the tab has no helper.
std::unique_ptr<ui::SimpleMenuModel> MaybeAppendInitialUrlSubMenu(
    ui::SimpleMenuModel* parent,
    content::WebContents* contents);

}  // namespace roamex::tabs

#endif  // ROAMEX_BROWSER_UI_TABS_INITIAL_URL_MENU_H_
