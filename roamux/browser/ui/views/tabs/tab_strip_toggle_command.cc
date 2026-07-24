// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/ui/views/tabs/tab_strip_toggle_command.h"

#include <map>

#include "base/feature_list.h"
#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/features.h"
#include "components/prefs/pref_service.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/tab_strip_placement.h"

namespace roamux::tabs_toggle {

namespace {

std::map<BrowserWindowInterface*, base::RepeatingClosure>& Handlers() {
  static base::NoDestructor<
      std::map<BrowserWindowInterface*, base::RepeatingClosure>>
      handlers;
  return *handlers;
}

}  // namespace

bool CanToggleTabStrip(BrowserWindowInterface* browser) {
  if (!browser ||
      !base::FeatureList::IsEnabled(features::kTabStripToggleShortcut)) {
    return false;
  }
  if (!tabs::IsVerticalTabsFeatureEnabled()) {
    return false;
  }
  const TabStripPlacement placement =
      GetTabStripPlacement(browser->GetProfile()->GetPrefs());
  return placement == TabStripPlacement::kLeft ||
         placement == TabStripPlacement::kRight;
}

void ToggleTabStrip(BrowserWindowInterface* browser) {
  auto it = Handlers().find(browser);
  if (it != Handlers().end()) {
    it->second.Run();
  }
  // No handler (0053-only build, or flag-off window): deliberate no-op.
}

void SetToggleHandler(BrowserWindowInterface* browser,
                      base::RepeatingClosure handler) {
  Handlers()[browser] = std::move(handler);
}

void ClearToggleHandler(BrowserWindowInterface* browser) {
  Handlers().erase(browser);
}

}  // namespace roamux::tabs_toggle
