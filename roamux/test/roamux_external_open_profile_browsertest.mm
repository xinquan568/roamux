// SPDX-License-Identifier: Apache-2.0
// roam-213: external opens route to the designated profile (patch 0050 seam).
// The accessible seam is -[AppController application:openURLs:] on the shared
// controller (OpenUrlsInBrowser has internal linkage), matching upstream
// app_controller_mac_browsertest.mm. Assertions are on tab contents, never
// window counts (session restore may add windows).

#import <Cocoa/Cocoa.h>

#include <string>

#include "base/files/file_path.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/app_controller_mac.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "net/base/apple/url_conversions.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "url/gurl.h"

namespace roamux {
namespace {

constexpr char kProfileBDir[] = "Profile Roam213 B";

// True iff any browser whose ORIGINAL profile is `profile` shows `url` in any
// tab (visible URL: dispatch is observable before the navigation commits —
// the test URLs never resolve).
bool ProfileHasTabWithUrl(Profile* profile, const GURL& url) {
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    if (window->GetProfile()->GetOriginalProfile() !=
        profile->GetOriginalProfile()) {
      continue;
    }
    TabStripModel* tabs = window->GetTabStripModel();
    for (int i = 0; i < tabs->count(); ++i) {
      if (tabs->GetWebContentsAt(i)->GetVisibleURL() == url) {
        return true;
      }
    }
  }
  return false;
}

void OpenExternalUrl(const GURL& url) {
  @autoreleasepool {
    [AppController.sharedController application:NSApp
                                       openURLs:@[ net::NSURLWithGURL(url) ]];
  }
}

void DesignateInLocalState(const std::string& base_name) {
  PrefService* local_state = g_browser_process->local_state();
  local_state->SetInteger(prefs::kExternalOpenMode, 1);
  local_state->SetString(prefs::kExternalOpenProfile, base_name);
}

class RoamuxExternalOpenProfileTest : public test::RoamuxBrowserTest {
 public:
  // Repo convention: flag pinned in the CONSTRUCTOR (ctor-order trap).
  RoamuxExternalOpenProfileTest() {
    features_.InitAndEnableFeature(features::kRoamuxExternalOpenProfile);
  }

 protected:
  base::FilePath ProfileBPath() {
    return g_browser_process->profile_manager()->user_data_dir().AppendASCII(
        kProfileBDir);
  }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxExternalOpenProfileTest,
                       RoutesToLoadedDesignatedProfile) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& profile_b =
      profiles::testing::CreateProfileSync(profile_manager, ProfileBPath());
  DesignateInLocalState(kProfileBDir);

  const GURL url("https://roam213.invalid/loaded");
  OpenExternalUrl(url);

  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return ProfileHasTabWithUrl(&profile_b, url); }));
  EXPECT_FALSE(ProfileHasTabWithUrl(browser()->profile(), url));
}

IN_PROC_BROWSER_TEST_F(RoamuxExternalOpenProfileTest,
                       StaleDesignationFallsBackToLastUsed) {
  DesignateInLocalState("Profile Roam213 Deleted");

  const GURL url("https://roam213.invalid/stale");
  OpenExternalUrl(url);

  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return ProfileHasTabWithUrl(browser()->profile(), url); }));
}

// PRE_ pattern: profile B exists, carries restore state ("continue where you
// left off" + a seeded tab), is designated in Local State — and is NOT in the
// persisted last-active list, so the main body starts with B registered but
// genuinely unloaded and the seam exercises RunInProfileSafely's async
// LoadProfileByPath branch.
IN_PROC_BROWSER_TEST_F(RoamuxExternalOpenProfileTest,
                       PRE_RoutesToUnloadedDesignatedProfile) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& profile_b =
      profiles::testing::CreateProfileSync(profile_manager, ProfileBPath());

  // Seed restore state: a committed tab in B's session plus "continue where
  // you left off" (SessionStartupPref::kPrefValueLast).
  Browser* browser_b = CreateBrowser(&profile_b);
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser_b, GURL("data:text/html,roam213-seeded-session")));
  profile_b.GetPrefs()->SetInteger(::prefs::kRestoreOnStartup, 1);
  DesignateInLocalState(kProfileBDir);

  // Unloaded guarantee (startup reloads profile.last_active_profiles, not
  // just profile.last_used): close B's browser NOW so shutdown bookkeeping
  // records only the default profile as active, then pin both Local State
  // entries explicitly (belt and braces, per plan).
  CloseBrowserSynchronously(browser_b);
  PrefService* local_state = g_browser_process->local_state();
  local_state->SetString(::prefs::kProfileLastUsed, chrome::kInitialProfile);
  {
    ScopedListPrefUpdate update(local_state, ::prefs::kProfilesLastActive);
    update->clear();
    update->Append(chrome::kInitialProfile);
  }
  local_state->CommitPendingWrite();
}

IN_PROC_BROWSER_TEST_F(RoamuxExternalOpenProfileTest,
                       RoutesToUnloadedDesignatedProfile) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();

  // Precondition (fail fast, per plan): B is registered but NOT loaded.
  ASSERT_NE(profile_manager->GetProfileAttributesStorage()
                .GetProfileAttributesWithPath(ProfileBPath()),
            nullptr);
  ASSERT_EQ(profile_manager->GetProfileByPath(ProfileBPath()), nullptr);

  const GURL url("https://roam213.invalid/unloaded");
  OpenExternalUrl(url);

  ASSERT_TRUE(base::test::RunUntil([&]() {
    Profile* profile_b = profile_manager->GetProfileByPath(ProfileBPath());
    return profile_b && ProfileHasTabWithUrl(profile_b, url);
  }));
  // Session-restore pin (the frozen analysis's unknown, decided by
  // observation, 2026-07-24): opening an external URL into a freshly-loaded
  // "continue where you left off" profile DOES restore that profile's
  // previous session — the seeded tab comes back alongside the external URL
  // (2 tabs total). Deterministic across runs; documented in the flag/setting
  // copy. If an uprev changes this, these assertions fail loudly and the
  // stance gets re-decided.
  Profile* profile_b = profile_manager->GetProfileByPath(ProfileBPath());
  EXPECT_TRUE(ProfileHasTabWithUrl(
      profile_b, GURL("data:text/html,roam213-seeded-session")));
  int tab_count = 0;
  for (BrowserWindowInterface* window : GetAllBrowserWindowInterfaces()) {
    if (window->GetProfile()->GetOriginalProfile() == profile_b) {
      tab_count += window->GetTabStripModel()->count();
    }
  }
  EXPECT_EQ(tab_count, 2);  // the external URL + the restored session tab
}

// Flag OFF: identity with current behavior — the designation is ignored even
// when fully configured and the designated profile is loaded.
class RoamuxExternalOpenProfileFlagOffTest : public test::RoamuxBrowserTest {
 public:
  RoamuxExternalOpenProfileFlagOffTest() {
    features_.InitAndDisableFeature(features::kRoamuxExternalOpenProfile);
  }

 protected:
  base::FilePath ProfileBPath() {
    return g_browser_process->profile_manager()->user_data_dir().AppendASCII(
        kProfileBDir);
  }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxExternalOpenProfileFlagOffTest,
                       DesignationIgnoredWhenFlagOff) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& profile_b =
      profiles::testing::CreateProfileSync(profile_manager, ProfileBPath());
  DesignateInLocalState(kProfileBDir);

  const GURL url("https://roam213.invalid/flag-off");
  OpenExternalUrl(url);

  ASSERT_TRUE(base::test::RunUntil(
      [&]() { return ProfileHasTabWithUrl(browser()->profile(), url); }));
  EXPECT_FALSE(ProfileHasTabWithUrl(&profile_b, url));
}

}  // namespace
}  // namespace roamux
