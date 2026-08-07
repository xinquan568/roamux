// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/tabs/reload_initial_url_command.h"

#include "base/feature_list.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "roamux/common/roamux_features.h"
#include "ui/base/page_transition_types.h"

namespace roamux::tabs {

namespace {

TabInitialUrlHelper* ActiveTabHelper(const Browser* browser) {
  if (!browser) {
    return nullptr;
  }
  content::WebContents* active =
      browser->tab_strip_model()->GetActiveWebContents();
  return active ? TabInitialUrlHelper::FromWebContents(active) : nullptr;
}

}  // namespace

bool CanReloadInitialUrlForContents(content::WebContents* contents) {
  if (!base::FeatureList::IsEnabled(features::kInitialUrl)) {
    return false;
  }
  if (!contents) {
    return false;
  }
  TabInitialUrlHelper* helper = TabInitialUrlHelper::FromWebContents(contents);
  return helper && helper->has_initial_url() &&
         helper->initial_url().is_valid();
}

bool CanReloadInitialUrl(const Browser* browser) {
  // roam-268: the flag gate lives in the per-contents predicate above, so both
  // entry points gate identically and this lift stays behaviour-preserving.
  if (!browser) {
    return false;
  }
  return CanReloadInitialUrlForContents(
      browser->tab_strip_model()->GetActiveWebContents());
}

void ReloadInitialUrl(Browser* browser) {
  if (!CanReloadInitialUrl(browser)) {
    return;
  }
  content::WebContents* active =
      browser->tab_strip_model()->GetActiveWebContents();
  content::NavigationController::LoadURLParams params(
      ActiveTabHelper(browser)->initial_url());
  params.transition_type = ui::PageTransitionFromInt(
      ui::PAGE_TRANSITION_TYPED | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  active->GetController().LoadURLWithParams(params);
}

}  // namespace roamux::tabs
