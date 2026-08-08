// SPDX-License-Identifier: Apache-2.0
// roam-269.
#include "roamux/browser/tabs/refresh_all_initial_urls_command.h"

#include <map>
#include <memory>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "roamux/browser/tabs/initial_url_refresh_run.h"
#include "roamux/common/roamux_features.h"

namespace roamux::tabs {

namespace {

// The roam-214 house pattern: per-Browser state in a function-local
// NoDestructor map rather than a global observer registry.
std::map<Browser*, std::unique_ptr<InitialUrlRefreshRun>>& Runs() {
  static base::NoDestructor<
      std::map<Browser*, std::unique_ptr<InitialUrlRefreshRun>>>
      runs;
  return *runs;
}

// §3.5. Two requirements pull in opposite directions and this resolves both:
//
//  (a) The map must stop holding a FINISHED run IMMEDIATELY. roam-268 finishes
//      a run at the last START, so C1b permits a fresh run right away; if a
//      finished entry lingered, the next trigger would find it and "cancel" a
//      no-op instead of starting a successor — the chord would appear to do
//      nothing every other press.
//  (b) The run must NOT be destroyed re-entrantly: we are called from inside
//      its own OnRunFinished.
//
// So: unlink synchronously (satisfying (a)), then hand ownership to DeleteSoon
// (satisfying (b)). Removing the entry before returning also means no successor
// can be created while this entry is still present, which dissolves the
// delete-the-successor hazard rather than merely guarding against it.
void RetireFinishedRun(Browser* browser, InitialUrlRefreshRun* finished) {
  auto it = Runs().find(browser);
  if (it == Runs().end() || it->second.get() != finished) {
    return;  // already replaced or retired.
  }
  std::unique_ptr<InitialUrlRefreshRun> owned = std::move(it->second);
  Runs().erase(it);
  base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(
      FROM_HERE, std::move(owned));
}

}  // namespace

bool CanRefreshAllInitialUrls(const BrowserWindowInterface* browser) {
  // Plan §4.6 — deliberately COARSE and STATIC. See the header for why an
  // any-tab-eligible predicate here would be a defect rather than a refinement.
  if (!browser) {
    return false;
  }
  if (!base::FeatureList::IsEnabled(features::kRefreshAllInitialUrls)) {
    return false;
  }
  return browser->GetType() == BrowserWindowInterface::TYPE_NORMAL;
}

void RefreshAllInitialUrls(Browser* browser) {
  if (!CanRefreshAllInitialUrls(browser)) {
    return;
  }

  // §3.5 C1: a second press while a run is live CANCELS it. The tab currently
  // loading is left alone — it is already navigating, and aborting would leave
  // a half-loaded page.
  auto it = Runs().find(browser);
  if (it != Runs().end()) {
    it->second->Cancel();
    return;
  }

  auto run = std::make_unique<InitialUrlRefreshRun>(browser);
  InitialUrlRefreshRun* raw = run.get();
  raw->set_finished_callback(base::BindOnce(&RetireFinishedRun, browser));
  Runs()[browser] = std::move(run);
  raw->Start();
}

}  // namespace roamux::tabs
