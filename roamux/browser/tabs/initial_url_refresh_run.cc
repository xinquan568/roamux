// SPDX-License-Identifier: Apache-2.0
// roam-269.
#include "roamux/browser/tabs/initial_url_refresh_run.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/time/default_tick_clock.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/javascript_dialogs/app_modal_dialog_queue.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "roamux/browser/tabs/reload_initial_url_command.h"
#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "roamux/common/roamux_features.h"
#include "ui/base/page_transition_types.h"

namespace roamux::tabs {

namespace {

// §7.3: the params are read HERE, on the chrome-facing side, and handed to the
// pure scheduler as raw integers. Validation (out-of-range yields the DEFAULT,
// never a clip; then min_spacing = min(min_spacing, interval)) belongs to
// Params::FromMilliseconds — roam-268 — so the scheduler stays
// FeatureParam-unaware and testable without a FeatureList.
InitialUrlRefreshScheduler::Params ParamsFromFeature() {
  return InitialUrlRefreshScheduler::Params::FromMilliseconds(
      features::kRefreshAllInitialUrlsIntervalMs.Get(),
      features::kRefreshAllInitialUrlsMinSpacingMs.Get());
}

// §3.2: the active tab first (so the tab you are looking at is warm
// immediately), then strip order skipping it. UNFILTERED — eligibility is a
// dequeue-time question (§3.3), and filtering here would freeze it at trigger
// time.
std::vector<::tabs::TabHandle> SnapshotTabs(Browser* browser) {
  std::vector<::tabs::TabHandle> handles;
  TabStripModel* model = browser->tab_strip_model();
  const int count = model->count();
  handles.reserve(count);

  const int active = model->active_index();
  if (active >= 0 && active < count) {
    handles.push_back(model->GetTabAtIndex(active)->GetHandle());
  }
  for (int i = 0; i < count; ++i) {
    if (i == active) {
      continue;
    }
    handles.push_back(model->GetTabAtIndex(i)->GetHandle());
  }
  return handles;
}

}  // namespace

InitialUrlRefreshRun::InitialUrlRefreshRun(Browser* browser)
    : browser_(browser),
      snapshot_(SnapshotTabs(browser)),
      scheduler_(this,
                 ParamsFromFeature(),
                 base::DefaultTickClock::GetInstance()) {
  // §3.5 C2: self-scoping teardown. A per-browser subscription cannot outlive
  // its run, unlike a global observer registration.
  browser_close_subscription_ =
      browser_->RegisterBrowserDidClose(base::BindRepeating(
          &InitialUrlRefreshRun::OnBrowserClosing, weak_factory_.GetWeakPtr()));
}

InitialUrlRefreshRun::~InitialUrlRefreshRun() = default;

base::WeakPtr<InitialUrlRefreshRun> InitialUrlRefreshRun::AsWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

void InitialUrlRefreshRun::Start() {
  scheduler_.Start(snapshot_.size());
}

void InitialUrlRefreshRun::Cancel() {
  scheduler_.Cancel();
}

bool InitialUrlRefreshRun::StartItem(size_t index) {
  if (index >= snapshot_.size()) {
    return false;
  }
  current_index_ = index;
  Observe(nullptr);
  pending_navigation_id_ = 0;
  attempt_started_ = false;

  // Resolve the CONTENTS at dequeue, from the handle. Discard replaces the
  // WebContents (which is why TabInitialUrlHelper::WillDiscardContents exists),
  // so a WeakPtr<WebContents> captured at snapshot time would be dead here
  // while the handle is still perfectly valid.
  ::tabs::TabInterface* tab = snapshot_[index].Get();
  if (!tab) {
    return false;  // S5: gone.
  }
  content::WebContents* contents = tab->GetContents();
  if (!contents) {
    return false;
  }

  // S5: the tab must still belong to THIS run's browser — a tab dragged to
  // another window is not ours to navigate.
  if (browser_->tab_strip_model()->GetIndexOfWebContents(contents) ==
      TabStripModel::kNoTab) {
    return false;
  }
  // S2: never yank audio out from under the user.
  if (contents->IsCurrentlyAudible()) {
    return false;
  }
  // §3.3, re-evaluated now rather than at enqueue.
  if (!CanReloadInitialUrlForContents(contents)) {
    return false;
  }

  TabInitialUrlHelper* helper = TabInitialUrlHelper::FromWebContents(contents);
  content::NavigationController::LoadURLParams params(helper->initial_url());
  params.transition_type = ui::PageTransitionFromInt(
      ui::PAGE_TRANSITION_TYPED | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  base::WeakPtr<content::NavigationHandle> handle =
      contents->GetController().LoadURLWithParams(params);
  if (!handle) {
    return false;
  }

  // §2.6: remember OUR navigation id and observe this tab. The attempt is
  // Pending until DidStartNavigation matches that id; only then may
  // DidStopLoading settle it.
  pending_navigation_id_ = handle->GetNavigationId();
  Observe(contents);
  return true;
}

bool InitialUrlRefreshRun::ShouldDefer() {
  // §2.7. Honest but partial by construction: this sees a dialog that has been
  // CREATED, not a navigation still waiting on a renderer's beforeunload
  // response. The guarantee is "at most one dialog visible, and behind it at
  // most one further attempt Pending" — not "only one prompt can ever exist".
  auto* queue = javascript_dialogs::AppModalDialogQueue::GetInstance();
  return queue && queue->HasActiveDialog();
}

void InitialUrlRefreshRun::OnRunFinished() {
  // The command owns the per-Browser map and erases this run from a POSTED,
  // identity-guarded task — never re-entrantly from here, and never keyed on
  // Browser* alone (roam-268 finishes at the last START, so a successor run may
  // already exist by the time that task lands).
  Observe(nullptr);
  if (finished_callback_) {
    std::move(finished_callback_).Run(this);
  }
}

void InitialUrlRefreshRun::DidStartNavigation(
    content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame()) {
    return;
  }
  if (pending_navigation_id_ != 0 &&
      handle->GetNavigationId() == pending_navigation_id_) {
    attempt_started_ = true;
  }
}

void InitialUrlRefreshRun::DidStopLoading() {
  // THE GATE. A stale stop from the load that was already in flight when we
  // dequeued this tab arrives while we are still Pending and is ignored.
  // Without this, a window where every tab is mid-load settles all of them
  // instantly and the run collapses to the min_spacing floor.
  if (!attempt_started_) {
    return;
  }
  attempt_started_ = false;
  pending_navigation_id_ = 0;
  Observe(nullptr);
  scheduler_.NotifyItemSettled(current_index_);
}

void InitialUrlRefreshRun::OnBrowserClosing(BrowserWindowInterface* browser) {
  // §3.5 C2. Stop observing and drop the queue; the command's posted,
  // identity-guarded erase reclaims us.
  Observe(nullptr);
  scheduler_.Cancel();
}

}  // namespace roamux::tabs
