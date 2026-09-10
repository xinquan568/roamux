// SPDX-License-Identifier: Apache-2.0
// roam-320: the session-rebuild reader. Chromium periodically rebuilds the whole
// session file from live browser state and its builders emit NO extra-data
// command, so the append-only "roamux.initial_url" / "roamux.tab_uid" writes are
// erased and a relaunch re-captures the tab's CURRENT page as its initial URL.
// BuildTabExtraDataCommands() reads the live per-tab helpers at serialization
// time — the only source that is authoritative for a restored-but-unloaded tab
// and for the replacement contents of a discard — and renders their extra data
// as AddTabExtraData commands for the builder to append.
// (TDD: written RED before the implementation; this file does not compile until
// roamux/browser/tabs/session_tab_extra_data.h exists.)

#include "roamux/browser/tabs/session_tab_extra_data.h"

#include <memory>
#include <string>
#include <vector>

#include "base/test/scoped_feature_list.h"
#include "chrome/browser/sessions/session_tab_helper_factory.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/sessions/content/session_tab_helper.h"
#include "components/sessions/core/session_command.h"
#include "components/sessions/core/session_id.h"
#include "components/sessions/core/session_service_commands.h"
#include "components/tabs/public/mock_tab_interface.h"
#include "content/public/test/navigation_simulator.h"
#include "roamux/browser/tabs/tab_initial_url_helper.h"
#include "roamux/browser/tabs/tab_uid_tab_helper.h"
#include "roamux/common/roamux_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace roamux::tabs {
namespace {

std::vector<uint8_t> Bytes(const sessions::SessionCommand& command) {
  auto span = command.contents();
  return std::vector<uint8_t>(span.begin(), span.end());
}

// Equality against a command built independently through the sessions API, so
// the assertion pins the wire payload and not just the key we happened to use.
void ExpectSameCommand(const sessions::SessionCommand& got,
                       const sessions::SessionCommand& want) {
  EXPECT_EQ(want.id(), got.id());
  EXPECT_EQ(Bytes(want), Bytes(got));
}

class SessionTabExtraDataTestBase : public ChromeRenderViewHostTestHarness {
 public:
  void SetUp() override {
    ConfigureFeatures();
    ChromeRenderViewHostTestHarness::SetUp();
    // The helpers resolve their tab id through SessionTabHelper.
    CreateSessionServiceTabHelper(web_contents());
    ON_CALL(tab_, GetContents())
        .WillByDefault(::testing::Return(web_contents()));
  }

 protected:
  virtual void ConfigureFeatures() = 0;

  // Creation goes through the same entry points patch 0009 uses, so a
  // flags-off fixture exercises the real gate rather than simply not calling.
  void CreateHelpers() {
    TabInitialUrlHelper::MaybeCreateForWebContents(web_contents());
    TabUidTabHelper::MaybeCreateForTab(tab_, profile());
  }

  SessionID tab_id() {
    return sessions::SessionTabHelper::IdForTab(web_contents());
  }

  std::vector<std::unique_ptr<sessions::SessionCommand>> Build() {
    return BuildTabExtraDataCommands(web_contents(), tab_id());
  }

  TabInitialUrlHelper* initial_url_helper() {
    return TabInitialUrlHelper::FromWebContents(web_contents());
  }
  TabUidTabHelper* uid_helper() {
    return TabUidTabHelper::FromWebContents(web_contents());
  }

  base::test::ScopedFeatureList features_;
  // MockTabInterface is already a NiceMock<TabInterface>; wrapping it again
  // trips gmock's strictness-modifier static_assert.
  ::tabs::MockTabInterface tab_;
};

// Both producers enabled — the shipping configuration.
class SessionTabExtraDataTest : public SessionTabExtraDataTestBase {
 protected:
  void ConfigureFeatures() override {
    features_.InitWithFeatures({features::kInitialUrl, features::kTabVisitNav},
                               {});
  }
};

// Both producers disabled: nothing exists to serialize.
class SessionTabExtraDataFlagsOffTest : public SessionTabExtraDataTestBase {
 protected:
  void ConfigureFeatures() override {
    features_.InitWithFeatures({},
                               {features::kInitialUrl, features::kTabVisitNav});
  }
};

// The uid helper is created under kTabVisitNav alone, so kInitialUrl being off
// does NOT mean there is nothing to serialize.
class SessionTabExtraDataUidOnlyTest : public SessionTabExtraDataTestBase {
 protected:
  void ConfigureFeatures() override {
    features_.InitWithFeatures({features::kTabVisitNav},
                               {features::kInitialUrl});
  }
};

TEST_F(SessionTabExtraDataTest, NoHelpersEmitsNothing) {
  // Features ON but the helpers were never created: the statement is about the
  // reader, not about the flags.
  ASSERT_EQ(nullptr, initial_url_helper());
  ASSERT_EQ(nullptr, uid_helper());
  EXPECT_TRUE(Build().empty());
}

TEST_F(SessionTabExtraDataTest, ReadDoesNotCreateHelpers) {
  ASSERT_EQ(nullptr, initial_url_helper());
  ASSERT_EQ(nullptr, uid_helper());
  Build();
  // A serializer must never allocate helpers or mint identities as a side
  // effect of being asked what to write.
  EXPECT_EQ(nullptr, initial_url_helper());
  EXPECT_EQ(nullptr, uid_helper());
}

TEST_F(SessionTabExtraDataTest, UncapturedHelperEmitsNothing) {
  CreateHelpers();
  ASSERT_NE(nullptr, initial_url_helper());
  ASSERT_FALSE(initial_url_helper()->has_initial_url());
  // The uid helper always carries a value, so only the uid key is expected.
  auto commands = Build();
  ASSERT_EQ(1u, commands.size());
  ExpectSameCommand(*commands[0],
                    *sessions::CreateAddTabExtraDataCommand(
                        tab_id(), TabUidTabHelper::kExtraDataKey,
                        uid_helper()->uid()));
}

TEST_F(SessionTabExtraDataTest, CapturedInitialUrlEmitsItsKey) {
  CreateHelpers();
  content::NavigationSimulator::NavigateAndCommitFromBrowser(
      web_contents(), GURL("https://captured.test/"));
  ASSERT_TRUE(initial_url_helper()->has_initial_url());

  auto commands = Build();
  ASSERT_EQ(2u, commands.size());
  ExpectSameCommand(
      *commands[0],
      *sessions::CreateAddTabExtraDataCommand(
          tab_id(), TabInitialUrlHelper::kExtraDataKey,
          TabInitialUrlHelper::EncodeExtraData(GURL("https://captured.test/"),
                                               /*locked=*/false)));
}

TEST_F(SessionTabExtraDataTest, LockedValueRoundTrips) {
  CreateHelpers();
  ASSERT_TRUE(
      initial_url_helper()->SetUserInitialUrl(GURL("https://edited.test/")));
  ASSERT_TRUE(initial_url_helper()->is_user_locked());

  auto commands = Build();
  ASSERT_EQ(2u, commands.size());
  // The lock bit rides the same encoded value, so losing the command loses the
  // user's explicit edit as well as the captured URL.
  ExpectSameCommand(
      *commands[0],
      *sessions::CreateAddTabExtraDataCommand(
          tab_id(), TabInitialUrlHelper::kExtraDataKey,
          TabInitialUrlHelper::EncodeExtraData(GURL("https://edited.test/"),
                                               /*locked=*/true)));
}

TEST_F(SessionTabExtraDataTest, BothKeysEmittedInDeterministicOrder) {
  CreateHelpers();
  content::NavigationSimulator::NavigateAndCommitFromBrowser(
      web_contents(), GURL("https://captured.test/"));

  auto commands = Build();
  ASSERT_EQ(2u, commands.size());
  ExpectSameCommand(
      *commands[0],
      *sessions::CreateAddTabExtraDataCommand(
          tab_id(), TabInitialUrlHelper::kExtraDataKey,
          TabInitialUrlHelper::EncodeExtraData(GURL("https://captured.test/"),
                                               /*locked=*/false)));
  ExpectSameCommand(*commands[1],
                    *sessions::CreateAddTabExtraDataCommand(
                        tab_id(), TabUidTabHelper::kExtraDataKey,
                        uid_helper()->uid()));
}

TEST_F(SessionTabExtraDataFlagsOffTest, BothFlagsOffEmitsNothing) {
  CreateHelpers();  // real gate: both entry points no-op with the flags off
  ASSERT_EQ(nullptr, initial_url_helper());
  ASSERT_EQ(nullptr, uid_helper());
  EXPECT_TRUE(Build().empty());
}

TEST_F(SessionTabExtraDataUidOnlyTest, InitialUrlOffUidOnEmitsOnlyUid) {
  CreateHelpers();
  ASSERT_EQ(nullptr, initial_url_helper());
  ASSERT_NE(nullptr, uid_helper());

  auto commands = Build();
  ASSERT_EQ(1u, commands.size());
  ExpectSameCommand(*commands[0],
                    *sessions::CreateAddTabExtraDataCommand(
                        tab_id(), TabUidTabHelper::kExtraDataKey,
                        uid_helper()->uid()));
}

}  // namespace
}  // namespace roamux::tabs
