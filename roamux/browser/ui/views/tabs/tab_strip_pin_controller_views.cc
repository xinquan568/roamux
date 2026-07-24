// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/ui/views/tabs/tab_strip_pin_controller_views.h"

#include <map>

#include "base/functional/bind.h"
#include "base/metrics/user_metrics.h"
#include "base/no_destructor.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_manager.h"
#include "chrome/browser/ui/exclusive_access/fullscreen_controller.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "roamux/browser/ui/views/tabs/tab_strip_toggle_command.h"
#include "roamux/common/roamux_prefs.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/widget/widget.h"

namespace roamux {

namespace {

// Region-view attach registry: the patched region-view ctor/dtor land here;
// the features-owned glue registers per browser.
std::map<Browser*, TabStripPinControllerViews*>& GlueRegistry() {
  static base::NoDestructor<std::map<Browser*, TabStripPinControllerViews*>>
      registry;
  return *registry;
}

}  // namespace

TabStripPinControllerViews::TabStripPinControllerViews(Browser* browser)
    : browser_(browser), core_(this) {
  GlueRegistry()[browser_] = this;
  tabs_toggle::SetToggleHandler(
      browser_,
      base::BindRepeating(&TabStripPinControllerViews::OnToggleCommand,
                          base::Unretained(this)));
  pref_registrar_.Init(browser_->profile()->GetPrefs());
  pref_registrar_.Add(
      prefs::kTabStripPosition,
      base::BindRepeating(
          &TabStripPinControllerViews::OnModeRelevantPrefChanged,
          base::Unretained(this)));
  pref_registrar_.Add(
      ::prefs::kVerticalTabsExpandOnHoverEnabled,
      base::BindRepeating(
          &TabStripPinControllerViews::OnModeRelevantPrefChanged,
          base::Unretained(this)));
  browser_->tab_strip_model()->AddObserver(this);
  // Fullscreen observation starts lazily at first pin: during
  // BrowserWindowFeatures::Init the exclusive-access manager does not exist
  // yet, and the D9 fullscreen reset only matters while pinned.
  // The FullscreenController dies during Browser::OnWindowClosing — BEFORE
  // this features-owned object — so drop everything at browser-did-close or
  // the observation dangles (F4).
  browser_close_subscription_ =
      browser_->RegisterBrowserDidClose(base::BindRepeating(
          [](TabStripPinControllerViews* self, BrowserWindowInterface*) {
            self->DetachRegionView();
          },
          base::Unretained(this)));
}

TabStripPinControllerViews::~TabStripPinControllerViews() {
  // F4: the region view (and its widget) is usually gone already.
  DetachRegionView();
  tabs_toggle::ClearToggleHandler(browser_);
  GlueRegistry().erase(browser_);
}

void TabStripPinControllerViews::AttachRegionView(
    VerticalTabStripRegionView* region_view) {
  region_view_ = region_view;
  region_view_->SetRoamuxObserver(this);
  UpdateCommandEnablement();
}

void TabStripPinControllerViews::DetachRegionView() {
  if (!region_view_) {
    return;
  }
  RemovePinnedHandlers();
  core_.OnRegionDetached();  // drops locks while the region still exists
  region_view_->SetRoamuxObserver(nullptr);
  region_view_ = nullptr;
}

// --- Delegate ---

void TabStripPinControllerViews::AcquireKeepExpanded() {
  if (region_view_ && !keep_expanded_lock_) {
    keep_expanded_lock_ = region_view_->GetExpandOnHoverLock(
        ExpandOnHoverLockType::kKeepExpanded);
  }
  InstallPinnedHandlers();
}

void TabStripPinControllerViews::ReleaseKeepExpanded() {
  keep_expanded_lock_.reset();
  RemovePinnedHandlers();
}

void TabStripPinControllerViews::AcquireForceCollapse() {
  if (region_view_ && !force_collapse_lock_) {
    force_collapse_lock_ = region_view_->GetExpandOnHoverLock(
        ExpandOnHoverLockType::kForceCollapse);
  }
}

void TabStripPinControllerViews::ReleaseForceCollapse() {
  force_collapse_lock_.reset();
}

int TabStripPinControllerViews::ExternalKeepOpenLockCount() {
  return total_keep_open_locks_ - (keep_expanded_lock_ ? 1 : 0);
}

bool TabStripPinControllerViews::IsPointerInsideStrip() {
  return region_view_ && region_view_->IsMouseHovered();
}

bool TabStripPinControllerViews::IsFocusInsideStrip() {
  if (!region_view_ || !region_view_->GetFocusManager()) {
    return false;
  }
  views::View* focused = region_view_->GetFocusManager()->GetFocusedView();
  return focused && region_view_->Contains(focused);
}

void TabStripPinControllerViews::FocusStrip() {
  if (region_view_) {
    region_view_->RequestFocus();
  }
}

void TabStripPinControllerViews::ForceCollapseNow() {
  // Mode-A explicit unpin. Deliberately NOT RequestCollapse: that writes the
  // PERSISTED collapsed state (wrong for a transient pin, D8) and racing it
  // against the in-flight hover animation crashes the upstream animation
  // controller. Releasing our keep-expanded lock (already done by the core)
  // plus the suppression lock while the pointer is inside produces the
  // visual collapse through the hover model itself. Here: restore focus to
  // the page so the strip does not hold it open via focus.
  if (content::WebContents* contents =
          browser_->tab_strip_model()->GetActiveWebContents()) {
    contents->Focus();
  }
}

void TabStripPinControllerViews::ToggleCollapseViaButtonSemantics() {
  auto* state_controller =
      tabs::VerticalTabStripStateController::From(browser_);
  if (!state_controller) {
    return;
  }
  // The exact collapse-button action body (browser_actions.cc,
  // kActionToggleCollapseVertical), including its metrics.
  const bool collapse = state_controller->GetCollapseState() ==
                        tabs::VerticalTabStripCollapseState::kExpanded;
  state_controller->RequestCollapse(collapse);
  base::RecordAction(base::UserMetricsAction(
      collapse ? "VerticalTabs_TabStrip_ButtonToggleCollapsed"
               : "VerticalTabs_TabStrip_ButtonToggleUncollapsed"));
}

// --- Upstream seam ---

void TabStripPinControllerViews::OnRoamuxPointerExit() {
  core_.OnPointerExit();
}

void TabStripPinControllerViews::OnRoamuxFocusChanged() {
  core_.OnFocusChanged();
}

void TabStripPinControllerViews::OnRoamuxKeepOpenLocksChanged(
    int total_keep_open_count,
    bool released) {
  total_keep_open_locks_ = total_keep_open_count;
  if (released) {
    core_.OnExternalLockReleased();
  }
}

void TabStripPinControllerViews::OnRoamuxStripMenuCommandExecuted() {
  core_.OnCompletedInteraction();
}

// --- Pinned-lifetime handlers ---

void TabStripPinControllerViews::InstallPinnedHandlers() {
  if (pinned_handlers_installed_ || !region_view_ ||
      !region_view_->GetWidget()) {
    return;
  }
  region_view_->GetWidget()->GetRootView()->AddPreTargetHandler(this);
  if (!fullscreen_observation_.IsObserving()) {
    fullscreen_observation_.Observe(browser_->GetFeatures()
                                        .exclusive_access_manager()
                                        ->fullscreen_controller());
  }
  views::FocusManager* focus_manager = region_view_->GetFocusManager();
  if (focus_manager) {
    focus_manager->RegisterAccelerator(
        ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE),
        ui::AcceleratorManager::kHighPriority, this);
  }
  pinned_handlers_installed_ = true;
}

void TabStripPinControllerViews::RemovePinnedHandlers() {
  if (!pinned_handlers_installed_) {
    return;
  }
  if (region_view_ && region_view_->GetWidget()) {
    region_view_->GetWidget()->GetRootView()->RemovePreTargetHandler(this);
    views::FocusManager* focus_manager = region_view_->GetFocusManager();
    if (focus_manager) {
      focus_manager->UnregisterAccelerator(
          ui::Accelerator(ui::VKEY_ESCAPE, ui::EF_NONE), this);
    }
  }
  fullscreen_observation_.Reset();
  pinned_handlers_installed_ = false;
  pending_press_target_ = nullptr;
  pending_key_activation_ = false;
}

void TabStripPinControllerViews::OnMouseEvent(ui::MouseEvent* event) {
  if (!region_view_ || !core_.pinned()) {
    return;
  }
  if (!event->IsLeftMouseButton() && !event->IsMiddleMouseButton()) {
    return;
  }
  const gfx::Point screen_point =
      event->target() ? event->AsLocatedEvent()->root_location() : gfx::Point();
  gfx::Point local = screen_point;
  views::View::ConvertPointFromWidget(region_view_, &local);
  const bool inside = region_view_->HitTestPoint(local);
  if (event->type() == ui::EventType::kMousePressed) {
    pending_press_target_ =
        inside ? region_view_->GetEventHandlerForPoint(local) : nullptr;
    return;
  }
  if (event->type() == ui::EventType::kMouseReleased) {
    // Completed click: press began on a strip child and the release lands on
    // the SAME child (aborted clicks, whitespace, and outside-press never
    // arm — D3).
    if (pending_press_target_ && inside &&
        region_view_->GetEventHandlerForPoint(local) == pending_press_target_) {
      core_.OnCompletedInteraction();
    }
    pending_press_target_ = nullptr;
  }
}

void TabStripPinControllerViews::OnKeyEvent(ui::KeyEvent* event) {
  if (!core_.pinned() || event->type() != ui::EventType::kKeyPressed) {
    return;
  }
  // D6: Esc with focus inside the strip pane unpins (pre-target, so it wins
  // over the pane's focus-restore Esc); Esc anywhere else is untouched.
  if (event->key_code() == ui::VKEY_ESCAPE && IsFocusInsideStrip()) {
    core_.OnEscInStrip();
    event->SetHandled();
    return;
  }
  // Pre-target intent (a consumed key is invisible post-target): a pending
  // Enter/Space whose focus target is inside the strip; confirmed by an
  // activation signal within this dispatch scope (active-tab change).
  if ((event->key_code() == ui::VKEY_RETURN ||
       event->key_code() == ui::VKEY_SPACE) &&
      IsFocusInsideStrip()) {
    pending_key_activation_ = true;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](base::WeakPtr<TabStripPinControllerViews> self) {
                         if (self) {
                           self->pending_key_activation_ = false;
                         }
                       },
                       weak_factory_.GetWeakPtr()));
  }
}

bool TabStripPinControllerViews::AcceleratorPressed(
    const ui::Accelerator& accelerator) {
  // D6: Esc unpins ONLY with focus inside the strip pane; otherwise decline
  // so the page/pane keeps its Esc.
  if (core_.pinned() && IsFocusInsideStrip()) {
    core_.OnEscInStrip();
    return true;
  }
  return false;
}

bool TabStripPinControllerViews::CanHandleAccelerators() const {
  return core_.pinned();
}

void TabStripPinControllerViews::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  if (!core_.pinned() || !selection.active_tab_changed()) {
    return;
  }
  // Keyboard-activation completion: only a change with recorded key intent
  // arms (programmatic switches never do — plan F2).
  if (pending_key_activation_) {
    pending_key_activation_ = false;
    core_.OnCompletedInteraction();
  }
}

void TabStripPinControllerViews::OnFullscreenStateChanged() {
  core_.OnLifecycleReset();  // D9: entering (or leaving) fullscreen resets
}

// --- Command plumbing ---

void TabStripPinControllerViews::OnToggleCommand() {
  if (!tabs_toggle::CanToggleTabStrip(browser_) || !region_view_) {
    return;
  }
  const bool hover_on = browser_->profile()->GetPrefs()->GetBoolean(
      ::prefs::kVerticalTabsExpandOnHoverEnabled);
  core_.OnShortcut(hover_on ? TabStripPinController::Mode::kPinPeek
                            : TabStripPinController::Mode::kPlainToggle);
}

void TabStripPinControllerViews::UpdateCommandEnablement() {
  browser_->command_controller()->UpdateCommandEnabled(
      IDC_ROAMUX_TOGGLE_TAB_STRIP, tabs_toggle::CanToggleTabStrip(browser_));
}

void TabStripPinControllerViews::OnModeRelevantPrefChanged() {
  // Move focus out of the strip BEFORE the placement-driven teardown: a
  // focused strip tab blurring during AnimateAndDestroyChildView trips an
  // upstream CHECK (vertical_tab_view.cc collection_node_). ClearFocus is
  // synchronous at the views layer (WebContents::Focus is not).
  if (IsFocusInsideStrip()) {
    region_view_->GetFocusManager()->ClearFocus();
  }
  core_.OnLifecycleReset();  // D9: placement / hover-setting change
  UpdateCommandEnablement();
}

// --- Region-view lifecycle free functions (patched ctor/dtor) ---

void OnVerticalTabStripRegionViewCreated(
    Browser* browser,
    VerticalTabStripRegionView* region_view) {
  auto it = GlueRegistry().find(browser);
  if (it != GlueRegistry().end()) {
    it->second->AttachRegionView(region_view);
  }
}

void OnVerticalTabStripRegionViewDestroyed(
    Browser* browser,
    VerticalTabStripRegionView* region_view) {
  auto it = GlueRegistry().find(browser);
  if (it != GlueRegistry().end()) {
    it->second->DetachRegionView();
  }
}

void OnVerticalTabStripMenuCommandExecuted(Browser* browser) {
  auto it = GlueRegistry().find(browser);
  if (it != GlueRegistry().end()) {
    it->second->OnRoamuxStripMenuCommandExecuted();
  }
}

}  // namespace roamux
