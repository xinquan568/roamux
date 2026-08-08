// SPDX-License-Identifier: Apache-2.0
// roam-269: the chrome-facing half of roam-268's scheduler seam — the Delegate
// that owns the TabHandle snapshot, dequeue-time eligibility, the §2.6
// Pending/Started completion gate, and the §2.7 dialog check.
// Compiled into //chrome/browser/ui via patch 0065's sources list.
#ifndef ROAMUX_BROWSER_TABS_INITIAL_URL_REFRESH_RUN_H_
#define ROAMUX_BROWSER_TABS_INITIAL_URL_REFRESH_RUN_H_

#include <stddef.h>

#include <utility>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents_observer.h"
#include "roamux/browser/tabs/initial_url_refresh_scheduler.h"

class Browser;
class BrowserWindowInterface;

namespace content {
class NavigationHandle;
}

namespace roamux::tabs {

// One in-flight "refresh every tab to its initial URL" run, scoped to one
// Browser. Owns the scheduler and feeds it; the scheduler owns the pacing and
// knows nothing about tabs.
class InitialUrlRefreshRun : public InitialUrlRefreshScheduler::Delegate,
                             public content::WebContentsObserver {
 public:
  // Snapshots `browser`'s tabs UNFILTERED, in active-tab-first then
  // strip-index order. Unfiltered is load-bearing: filtering here would freeze
  // eligibility at trigger time, and §3.3 requires it re-evaluated at dequeue.
  explicit InitialUrlRefreshRun(Browser* browser);
  InitialUrlRefreshRun(const InitialUrlRefreshRun&) = delete;
  InitialUrlRefreshRun& operator=(const InitialUrlRefreshRun&) = delete;
  ~InitialUrlRefreshRun() override;

  // Invoked exactly once when the run finishes (naturally, on Cancel(), or on
  // browser close). The command sets this to a POSTED, identity-guarded erase
  // of its per-Browser map entry: roam-268 finishes a run at the last START,
  // so §3.5 C1b permits a successor to exist before the cleanup lands, and a
  // task keyed on Browser* alone would delete that successor.
  using FinishedCallback = base::OnceCallback<void(InitialUrlRefreshRun*)>;
  void set_finished_callback(FinishedCallback cb) {
    finished_callback_ = std::move(cb);
  }

  void Start();
  void Cancel();

  base::WeakPtr<InitialUrlRefreshRun> AsWeakPtr();

 private:
  // InitialUrlRefreshScheduler::Delegate:
  bool StartItem(size_t index) override;
  bool ShouldDefer() override;
  void OnRunFinished() override;

  // content::WebContentsObserver — the §2.6 gate. DidStopLoading settles an
  // attempt ONLY once DidStartNavigation has fired for the primary main frame
  // with our own navigation id. Deliberately no WebContentsDestroyed override:
  // M149 documents it as unusable, and the interval timer covers that case.
  void DidStartNavigation(content::NavigationHandle* handle) override;
  void DidStopLoading() override;

  // §3.5 C2: the window went away mid-run.
  void OnBrowserClosing(BrowserWindowInterface* browser);

  raw_ptr<Browser> browser_;
  std::vector<::tabs::TabHandle> snapshot_;
  InitialUrlRefreshScheduler scheduler_;
  size_t current_index_ = 0;
  int64_t pending_navigation_id_ = 0;
  bool attempt_started_ = false;
  // A DidStartNavigation seen synchronously from inside LoadURLWithParams,
  // before the returned handle told us which id to expect.
  bool in_load_call_ = false;
  int64_t buffered_start_id_ = 0;
  FinishedCallback finished_callback_;
  base::CallbackListSubscription browser_close_subscription_;
  base::WeakPtrFactory<InitialUrlRefreshRun> weak_factory_{this};
};

}  // namespace roamux::tabs

#endif  // ROAMUX_BROWSER_TABS_INITIAL_URL_REFRESH_RUN_H_
