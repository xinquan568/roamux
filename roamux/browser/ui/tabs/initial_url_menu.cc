// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/ui/tabs/initial_url_menu.h"

#include "base/feature_list.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/web_contents.h"
#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "roamux/browser/ui/tabs/edit_initial_url_dialog.h"
#include "roamux/common/roamux_features.h"
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
  }

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdEnabled(int command_id) const override {
    if (!contents_) {
      return false;
    }
    if (command_id == kSetInitialUrlToCurrentPageCommandId) {
      return CanSetToCurrentPage(contents_.get());
    }
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
