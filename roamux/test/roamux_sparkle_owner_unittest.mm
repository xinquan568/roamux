// SPDX-License-Identifier: Apache-2.0
// roam-287 (grill H10 / H11 / M8): the process-wide SparkleOwner and its
// SPUUserDriver conformer, isolated from a live SPUUpdater. The owner-for-
// testing runs the PRODUCTION start-result handling on an injected outcome
// (no SPUUpdater is ever allocated — roam-140's one-updater rule holds), so
// these tests prove: a failed start is tagged, logged exactly once, retained
// and replayed to every late sink, and never enables automatic checks; the
// real driver translates a SCHEDULED (userInitiated=NO) find built from REAL
// Sparkle objects; the pending offer is reply-bounded — re-emitted on
// showUpdateInFocus only while pending, replayed to late sinks only while
// pending, invalidated BEFORE the reply block runs, and dropped by every
// clearing path.

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include <optional>
#include <string>
#include <vector>

#include "base/logging.h"
#include "base/strings/sys_string_conversions.h"
#include "base/test/bind.h"
#include "base/test/mock_log.h"
#include "base/test/task_environment.h"
#include "roamux/browser/updates/roamux_update_service.h"
#include "roamux/browser/updates/roamux_update_user_driver.h"
#include "roamux/browser/updates/update_state_machine.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

// Sparkle marks -init unavailable; the designated initializer is private API
// of the vendored 2.9.4 binary (exported selector). A REAL state object with
// userInitiated=NO is what the production callback receives for a scheduled
// check — no nil, no double.
@interface SPUUserUpdateState (RoamuxTesting)
- (instancetype)initWithStage:(SPUUserUpdateStage)stage
                userInitiated:(BOOL)userInitiated;
@end

namespace roamux::updates {
namespace {

using ::testing::_;
using ::testing::HasSubstr;
using ::testing::Return;

constexpr char kDomain[] = "SUSparkleErrorDomain";
constexpr char kDescription[] = "The feed URL is invalid";

InjectedSparkleStart Started() {
  return InjectedSparkleStart{/*started=*/true, "", 0, ""};
}
InjectedSparkleStart Failed() {
  return InjectedSparkleStart{/*started=*/false, kDomain, 4, kDescription};
}

std::string ExpectedStartError() {
  return std::string(kUpdaterUnavailableErrorPrefix) + "[" + kDomain + " 4] " +
         kDescription;
}

// A valid appcast item per Sparkle's contract (a version plus an enclosure).
// Version and display version differ on purpose so the driver's
// `displayVersionString ?: versionString` choice is observable.
SUAppcastItem* MakeItem() {
  NSDictionary* dict = @{
    @"sparkle:version" : @"990",
    @"sparkle:shortVersionString" : @"99.0.0-test",
    @"description" : @"notes",
    @"enclosure" : @{
      @"url" : @"https://example.invalid/Roamux-99.zip",
      @"length" : @"1234",
      @"type" : @"application/octet-stream",
    },
  };
  NSString* reason = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  SUAppcastItem* item = [[SUAppcastItem alloc] initWithDictionary:dict
                                                     failureReason:&reason];
#pragma clang diagnostic pop
  EXPECT_NE(nil, item) << "SUAppcastItem refused the fixture: "
                       << base::SysNSStringToUTF8(reason ?: @"(no reason)");
  return item;
}

SPUUserUpdateState* MakeScheduledState() {
  SPUUserUpdateState* state = [[SPUUserUpdateState alloc]
      initWithStage:SPUUserUpdateStageNotDownloaded
      userInitiated:NO];
  EXPECT_NE(nil, state);
  EXPECT_FALSE(state.userInitiated);
  return state;
}

class RoamuxSparkleOwnerTest : public testing::Test {
 protected:
  using Owner = std::unique_ptr<SparkleOwner, SparkleOwnerDeleter>;

  // Subscribes a recording sink; the returned vector lives in `logs_`.
  std::vector<UpdateEvent>* Subscribe(SparkleOwner* owner) {
    logs_.push_back(std::make_unique<std::vector<UpdateEvent>>());
    std::vector<UpdateEvent>* events = logs_.back().get();
    subscriptions_.push_back(AddSparkleOwnerEventSinkForTesting(
        owner, base::BindLambdaForTesting([events](const UpdateEvent& e) {
          events->push_back(e);
        })));
    return events;
  }

  // Delivers a scheduled find through the REAL conformer and captures its
  // reply block's choice.
  void DeliverScheduledFind(SparkleOwner* owner) {
    replied_ = false;
    __block bool* replied = &replied_;
    __block SPUUserUpdateChoice* choice = &choice_;
    [DriverForTesting(owner)
        showUpdateFoundWithAppcastItem:MakeItem()
                                 state:MakeScheduledState()
                                 reply:^(SPUUserUpdateChoice c) {
                                   *replied = true;
                                   *choice = c;
                                 }];
  }

  base::test::TaskEnvironment task_environment_;
  std::vector<std::unique_ptr<std::vector<UpdateEvent>>> logs_;
  std::vector<base::CallbackListSubscription> subscriptions_;
  bool replied_ = false;
  SPUUserUpdateChoice choice_ = SPUUserUpdateChoiceDismiss;
};

TEST_F(RoamuxSparkleOwnerTest, StartFailureFormatsLogsAndReplaysToEveryLateSink) {
  base::test::MockLog log;
  EXPECT_CALL(log, Log(_, _, _, _, _)).WillRepeatedly(Return(false));
  EXPECT_CALL(log, Log(logging::LOGGING_ERROR, _, _, _,
                       HasSubstr(ExpectedStartError())))
      .Times(1)
      .WillOnce(Return(false));
  log.StartCapturingLogs();
  Owner owner = CreateSparkleOwnerForTesting(Failed());
  log.StopCapturingLogs();

  ASSERT_TRUE(SparkleOwnerStartErrorForTesting(owner.get()).has_value());
  EXPECT_EQ(ExpectedStartError(), *SparkleOwnerStartErrorForTesting(owner.get()));
  EXPECT_FALSE(AutomaticChecksEnabledForTesting(owner.get()));

  // Sinks subscribe AFTER construction (facades always do): each gets the
  // retained failure exactly once, synchronously.
  std::vector<UpdateEvent>* first = Subscribe(owner.get());
  ASSERT_EQ(1u, first->size());
  EXPECT_EQ(UpdateEventType::kError, (*first)[0].type);
  EXPECT_EQ(ExpectedStartError(), (*first)[0].error);
  std::vector<UpdateEvent>* second = Subscribe(owner.get());
  ASSERT_EQ(1u, second->size());
  EXPECT_EQ(ExpectedStartError(), (*second)[0].error);
  EXPECT_EQ(1u, first->size()) << "a later subscription must not re-notify earlier sinks";
}

TEST_F(RoamuxSparkleOwnerTest, StartedOwnerEnablesAutomaticChecksAndReplaysNothing) {
  base::test::MockLog log;
  // gMock matches the most recently registered expectation first: the
  // catch-all goes FIRST so the specific Times(0) below actually constrains
  // (registered the other way round, the catch-all swallowed every call).
  EXPECT_CALL(log, Log(_, _, _, _, _)).WillRepeatedly(Return(false));
  EXPECT_CALL(log, Log(logging::LOGGING_ERROR, _, _, _, HasSubstr("Sparkle")))
      .Times(0);
  log.StartCapturingLogs();
  Owner owner = CreateSparkleOwnerForTesting(Started());
  log.StopCapturingLogs();
  EXPECT_FALSE(SparkleOwnerStartErrorForTesting(owner.get()).has_value());
  EXPECT_TRUE(AutomaticChecksEnabledForTesting(owner.get()));
  EXPECT_TRUE(Subscribe(owner.get())->empty());
}

TEST_F(RoamuxSparkleOwnerTest, ScheduledFindTranslatesWithRealSparkleObjects) {
  Owner owner = CreateSparkleOwnerForTesting(Started());
  std::vector<UpdateEvent>* events = Subscribe(owner.get());
  SUAppcastItem* item = MakeItem();
  ASSERT_NE(nil, item);
  EXPECT_EQ("990", base::SysNSStringToUTF8(item.versionString));
  EXPECT_EQ("99.0.0-test", base::SysNSStringToUTF8(item.displayVersionString));
  EXPECT_EQ("notes", base::SysNSStringToUTF8(item.itemDescription));
  EXPECT_NE(nil, item.fileURL);
  EXPECT_EQ(1234u, item.contentLength);

  DeliverScheduledFind(owner.get());
  ASSERT_EQ(1u, events->size());
  EXPECT_EQ(UpdateEventType::kUpdateFound, (*events)[0].type);
  EXPECT_EQ("99.0.0-test", (*events)[0].version) << "display version wins";
  EXPECT_EQ("notes", (*events)[0].notes);
  EXPECT_FALSE(replied_) << "the reply is held until the user decides";
}

TEST_F(RoamuxSparkleOwnerTest, InFocusReemitsWhilePendingOnly) {
  Owner owner = CreateSparkleOwnerForTesting(Started());
  std::vector<UpdateEvent>* events = Subscribe(owner.get());
  DeliverScheduledFind(owner.get());
  ASSERT_EQ(1u, events->size());

  // A manual "Check for updates" during an open session arrives as
  // showUpdateInFocus — the pending offer is re-broadcast.
  [DriverForTesting(owner.get()) showUpdateInFocus];
  ASSERT_EQ(2u, events->size());
  EXPECT_EQ(UpdateEventType::kUpdateFound, (*events)[1].type);
  EXPECT_EQ("99.0.0-test", (*events)[1].version);

  // A facade created while the offer is pending receives it.
  std::vector<UpdateEvent>* late = Subscribe(owner.get());
  ASSERT_EQ(1u, late->size());
  EXPECT_EQ(UpdateEventType::kUpdateFound, (*late)[0].type);

  // Consumption: Download invokes the captured reply with Install…
  [DriverForTesting(owner.get()) commandDownload];
  EXPECT_TRUE(replied_);
  EXPECT_EQ(SPUUserUpdateChoiceInstall, choice_);
  // …after which nothing is pending: no re-emit, no replay.
  [DriverForTesting(owner.get()) showUpdateInFocus];
  EXPECT_EQ(2u, events->size());
  EXPECT_TRUE(Subscribe(owner.get())->empty());
}

TEST_F(RoamuxSparkleOwnerTest, ConsumptionInvalidatesBeforeInvoke) {
  Owner owner = CreateSparkleOwnerForTesting(Started());
  std::vector<UpdateEvent>* events = Subscribe(owner.get());
  RoamuxUpdateUserDriver* driver = DriverForTesting(owner.get());
  // Plain locals reached through pointers: a block captures the pointers by
  // value, and the C++ lambda below may not capture __block variables.
  size_t events_seen_in_reply = 0;
  bool late_sink_saw_offer = false;
  size_t* seen_ptr = &events_seen_in_reply;
  bool* late_ptr = &late_sink_saw_offer;
  SparkleOwner* raw = owner.get();
  std::vector<UpdateEvent>* events_ptr = events;
  base::CallbackListSubscription reentrant_subscription;
  base::CallbackListSubscription* reentrant_ptr = &reentrant_subscription;
  [driver showUpdateFoundWithAppcastItem:MakeItem()
                                   state:MakeScheduledState()
                                   reply:^(SPUUserUpdateChoice c) {
                                     // Sparkle may re-enter the driver while
                                     // the reply runs; neither an in-focus nor
                                     // a new subscription may see the offer.
                                     [driver showUpdateInFocus];
                                     *seen_ptr = events_ptr->size();
                                     *reentrant_ptr =
                                         AddSparkleOwnerEventSinkForTesting(
                                             raw, base::BindLambdaForTesting(
                                                      [late_ptr](const UpdateEvent&) {
                                                        *late_ptr = true;
                                                      }));
                                   }];
  ASSERT_EQ(1u, events->size());
  [DriverForTesting(owner.get()) commandDownload];
  EXPECT_EQ(1u, events_seen_in_reply) << "in-focus inside the reply re-emitted a consumed offer";
  EXPECT_FALSE(late_sink_saw_offer);
  EXPECT_EQ(1u, events->size());
}

TEST_F(RoamuxSparkleOwnerTest, SkipConsumesToo) {
  Owner owner = CreateSparkleOwnerForTesting(Started());
  std::vector<UpdateEvent>* events = Subscribe(owner.get());
  DeliverScheduledFind(owner.get());
  [DriverForTesting(owner.get()) commandSkipVersion:@"99.0.0-test"];
  EXPECT_TRUE(replied_);
  EXPECT_EQ(SPUUserUpdateChoiceSkip, choice_);
  [DriverForTesting(owner.get()) showUpdateInFocus];
  EXPECT_EQ(1u, events->size());
  EXPECT_TRUE(Subscribe(owner.get())->empty());
}

TEST_F(RoamuxSparkleOwnerTest, ClearingPathsDropThePendingOffer) {
  struct Path {
    const char* name;
    void (^apply)(RoamuxUpdateUserDriver*);
  };
  const Path paths[] = {
      {"showUpdaterError", ^(RoamuxUpdateUserDriver* d) {
         [d showUpdaterError:[NSError errorWithDomain:@"SUSparkleErrorDomain"
                                                 code:1002
                                             userInfo:nil]
             acknowledgement:^{}];
       }},
      {"showUpdateNotFoundWithError", ^(RoamuxUpdateUserDriver* d) {
         [d showUpdateNotFoundWithError:[NSError errorWithDomain:@"SUSparkleErrorDomain"
                                                            code:1001
                                                        userInfo:nil]
                        acknowledgement:^{}];
       }},
      {"showDownloadInitiatedWithCancellation", ^(RoamuxUpdateUserDriver* d) {
         [d showDownloadInitiatedWithCancellation:^{}];
       }},
      {"dismissUpdateInstallation", ^(RoamuxUpdateUserDriver* d) {
         [d dismissUpdateInstallation];
       }},
  };
  for (const Path& path : paths) {
    SCOPED_TRACE(path.name);
    Owner owner = CreateSparkleOwnerForTesting(Started());
    std::vector<UpdateEvent>* events = Subscribe(owner.get());
    DeliverScheduledFind(owner.get());
    ASSERT_EQ(1u, events->size());
    const size_t before = events->size();
    path.apply(DriverForTesting(owner.get()));
    const size_t after_path = events->size();  // the path may emit its own event
    [DriverForTesting(owner.get()) showUpdateInFocus];
    EXPECT_EQ(after_path, events->size()) << "in-focus re-emitted a dropped offer";
    std::vector<UpdateEvent>* late = Subscribe(owner.get());
    EXPECT_TRUE(late->empty()) << "a late sink received a dropped offer";
    EXPECT_GE(after_path, before);
  }
}

}  // namespace
}  // namespace roamux::updates
