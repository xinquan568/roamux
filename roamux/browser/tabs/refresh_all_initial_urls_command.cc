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

// §3.5: the erase is POSTED (never re-entrant from inside OnRunFinished) and
// IDENTITY-GUARDED. Because roam-268 finishes a run at the last START rather
// than the last settle, C1b lets a fresh run for the same Browser exist before
// this task lands — so erasing on Browser* alone would delete the SUCCESSOR.
// Comparing the stored pointer against the finishing run is what prevents that.
void ErasePostedIfStillCurrent(Browser* browser,
                               InitialUrlRefreshRun* finished) {
  auto it = Runs().find(browser);
  if (it != Runs().end() && it->second.get() == finished) {
    Runs().erase(it);
  }
}

}  // namespace

bool CanRefreshAllInitialUrls(BrowserWindowInterface* browser) {
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
  raw->set_finished_callback(base::BindOnce(
      [](Browser* b, InitialUrlRefreshRun* finished) {
        base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
            FROM_HERE, base::BindOnce(&ErasePostedIfStillCurrent, b, finished));
      },
      browser));
  Runs()[browser] = std::move(run);
  raw->Start();
}

}  // namespace roamux::tabs
