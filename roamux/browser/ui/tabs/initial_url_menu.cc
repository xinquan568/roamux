// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/ui/tabs/initial_url_menu.h"

#include "base/feature_list.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"
#include "roamux/browser/tabs/refresh_all_initial_urls_command.h"
#include "roamux/browser/tabs/reload_initial_url_command.h"
#include "roamux/browser/tabs/shortcut_registry.h"
#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "roamux/browser/ui/tabs/edit_initial_url_dialog.h"
#include "roamux/common/roamux_features.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/menus/simple_menu_model.h"

namespace roamux::tabs {

namespace {

// The command ids and their guard predicate are exported from the header
// (roam-194): the guard in patch 0005 and the tab-menu guard browsertest need
// to name them.
bool CanSetToCurrentPage(content::WebContents* contents) {
  const GURL& url = contents->GetLastCommittedURL();
  return url.is_valid() && !url.IsAboutBlank() &&
         url.spec() != "chrome://newtab/";
}

// The submenu owns its command handling (0005 pattern): upstream's
// TabMenuModel delegate is never touched.
class InitialUrlMenuModel : public ui::SimpleMenuModel,
                            public ui::SimpleMenuModel::Delegate {
 public:
  explicit InitialUrlMenuModel(content::WebContents* contents)
      : ui::SimpleMenuModel(this), contents_(contents->GetWeakPtr()) {
    AddItem(kEditInitialUrlCommandId, u"Edit initial URL…");
    AddItem(kSetInitialUrlToCurrentPageCommandId,
            u"Set initial URL to current page");
    // roam-270: the ONLY contextual surface for Ctrl+Opt+Cmd+R. The chord is
    // dispatched registry-only (no accelerators_cocoa.mm row, the roam-214
    // precedent), so nothing else in the tab menu advertises it. It is also
    // listed in Appearance settings (patch 0011), so this is contextual
    // discoverability rather than the only discoverability.
    if (base::FeatureList::IsEnabled(features::kRefreshAllInitialUrls)) {
      AddSeparator(ui::NORMAL_SEPARATOR);
      AddItem(kRefreshAllInitialUrlsCommandId,
              u"Refresh all tabs to initial URLs");
      // Without this, a Cocoa context menu renders NO accelerator even when
      // GetAcceleratorForCommandId returns one — and the accelerator being
      // visible is the entire point of this item.
      SetForceShowAcceleratorForItemAt(GetItemCount() - 1, true);
    }
  }

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdEnabled(int command_id) const override {
    if (!contents_) {
      return false;
    }
    if (command_id == kSetInitialUrlToCurrentPageCommandId) {
      return CanSetToCurrentPage(contents_.get());
    }
    if (command_id == kRefreshAllInitialUrlsCommandId) {
      // LIVE, and scoped to the WINDOW rather than this tab. Menus rebuild on
      // every open, so §4.6's static-bit reasoning — which is correct for
      // BrowserCommandController, where a stale-false bit makes the chord
      // permanently inert — does not apply here and would yield an item that is
      // always enabled. Note this deliberately does NOT ask about contents_:
      // the clicked tab may well be ineligible while others are not.
      Browser* browser = chrome::FindBrowserWithTab(contents_.get());
      if (!browser) {
        return false;
      }
      TabStripModel* model = browser->tab_strip_model();
      for (int i = 0; i < model->count(); ++i) {
        if (CanReloadInitialUrlForContents(model->GetWebContentsAt(i))) {
          return true;
        }
      }
      return false;
    }
    return true;
  }

  // roam-270: show the chord the registry currently resolves, so a rebind is
  // reflected rather than a hard-coded literal. Three things are load-bearing
  // and none is obvious: the row must be found in AllShortcuts() (the chord is
  // per-entry), GetCurrentChord needs the profile's PrefService (overrides are
  // a profile pref), and Chord::keycode is a CARBON virtual keycode which must
  // be converted before it means anything to ui::Accelerator.
  bool GetAcceleratorForCommandId(int command_id,
                                  ui::Accelerator* accelerator) const override {
    if (command_id != kRefreshAllInitialUrlsCommandId || !contents_) {
      return false;
    }
    Browser* browser = chrome::FindBrowserWithTab(contents_.get());
    if (!browser) {
      return false;
    }
    // TWO DIFFERENT ID SPACES, and conflating them silently yields no
    // accelerator at all: this menu's ids are the roamux submenu range
    // (2110-2113), while a registry row carries the CHROME command id
    // (IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS = 33013, roam-269 patch 0065).
    // Matching the row on kRefreshAllInitialUrlsCommandId finds nothing, and
    // the item renders with no shortcut — which is the one outcome this issue
    // exists to prevent.
    const RoamuxShortcut* row = nullptr;
    for (const RoamuxShortcut& entry : AllShortcuts()) {
      if (entry.command_id == IDC_ROAMUX_REFRESH_ALL_INITIAL_URLS) {
        row = &entry;
        break;
      }
    }
    if (!row) {
      return false;
    }
    const Chord chord = GetCurrentChord(browser->profile()->GetPrefs(), *row);
    int modifiers = ui::EF_NONE;
    if (chord.cmd) {
      modifiers |= ui::EF_COMMAND_DOWN;
    }
    if (chord.ctrl) {
      modifiers |= ui::EF_CONTROL_DOWN;
    }
    if (chord.opt) {
      modifiers |= ui::EF_ALT_DOWN;
    }
    if (chord.shift) {
      modifiers |= ui::EF_SHIFT_DOWN;
    }
    // Carbon keycode -> DomCode -> KeyboardCode. Deliberately NOT
    // ui::KeyboardCodeFromKeyCode(), which lives in
    // keyboard_code_conversion_mac.h and drags in Foundation: this file is a
    // .cc, and renaming it to .mm would change the injected file lists of
    // patches 0005/0012 — turning a zero-upstream-surface issue into one that
    // edits the patch stack. Both calls below are plain C++.
    const ui::DomCode dom_code =
        ui::KeycodeConverter::NativeKeycodeToDomCode(chord.keycode);
    if (dom_code == ui::DomCode::NONE) {
      return false;
    }
    *accelerator =
        ui::Accelerator(ui::DomCodeToUsLayoutKeyboardCode(dom_code), modifiers);
    return true;
  }

  void ExecuteCommand(int command_id, int event_flags) override {
    if (!contents_) {
      return;
    }
    content::WebContents* contents = contents_.get();
    TabInitialUrlHelper::MaybeCreateForWebContents(contents);
    TabInitialUrlHelper* helper =
        TabInitialUrlHelper::FromWebContents(contents);
    if (!helper) {
      return;
    }
    switch (command_id) {
      case kEditInitialUrlCommandId:
        ShowEditInitialUrlDialog(contents);
        break;
      case kSetInitialUrlToCurrentPageCommandId:
        if (CanSetToCurrentPage(contents)) {
          helper->SetUserInitialUrl(contents->GetLastCommittedURL());
        }
        break;
      case kRefreshAllInitialUrlsCommandId:
        // The same entry point the chord uses, so the two surfaces cannot
        // drift. The contents_ null-check above still applies (roam-204: this
        // submenu is retained by a window-scoped TabMenuModel and outlives
        // tabs); resolving a Browser* adds a second check, it does not retire
        // the first.
        if (Browser* browser = chrome::FindBrowserWithTab(contents)) {
          RefreshAllInitialUrls(browser);
        }
        break;
    }
  }

 private:
  // roam-204: the owning TabMenuModel is window-scoped and outlives tabs
  // (upstream holds its own tab handle weakly for the same reason), so a raw
  // pointer here dangles at window teardown. Weak + null-checks keep the
  // retained submenu inert once the tab is gone.
  base::WeakPtr<content::WebContents> contents_;
};

}  // namespace

std::unique_ptr<ui::SimpleMenuModel> MaybeAppendInitialUrlSubMenu(
    ui::SimpleMenuModel* parent,
    content::WebContents* contents) {
  if (!contents ||
      !base::FeatureList::IsEnabled(roamux::features::kInitialUrl)) {
    return nullptr;
  }
  auto submenu = std::make_unique<InitialUrlMenuModel>(contents);
  parent->AddSubMenu(kInitialUrlSubMenuCommandId, u"Initial URL",
                     submenu.get());
  return submenu;
}

}  // namespace roamux::tabs
