// SPDX-License-Identifier: Apache-2.0
// roam-160: the RoamuxVersionUpdater adapter — Chromium's VersionUpdater over
// the Sparkle machinery's UpdateCommands seam. RED-first against the T1
// skeleton; the suite pins the frozen-plan mapping table, the plain-language
// error-copy classes (signature failures get no retry), consent (nothing
// downloads unprompted), kIdle non-leak, and the promote-hidden posture.

#include "roamux/browser/updates/roamux_version_updater.h"

#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "roamux/browser/updates/update_state_machine.h"
#include "roamux/browser/updates/version_updater_seam.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux::updates {
namespace {

UpdateSnapshot Snap(UpdateStatus status,
                    const std::string& version = "",
                    const std::string& error = "",
                    double progress = 0.0) {
  UpdateSnapshot s;
  s.status = status;
  s.version = version;
  s.error = error;
  s.progress = progress;
  return s;
}

// A fake seam: records commands, replays snapshots on demand.
class FakeUpdateCommands : public UpdateCommands {
 public:
  void CheckForUpdates() override { ++checks; }
  void Download() override { ++downloads; }
  void InstallAndRelaunch() override { ++relaunches; }
  void Skip(const std::string& version) override { skipped = version; }

  base::CallbackListSubscription SubscribeToSnapshots(
      SnapshotCallbackList::CallbackType callback) override {
    auto subscription = callbacks_.Add(callback);
    callback.Run(current);
    return subscription;
  }

  void Push(const UpdateSnapshot& snapshot) {
    current = snapshot;
    callbacks_.Notify(snapshot);
  }

  UpdateSnapshot current;
  int checks = 0;
  int downloads = 0;
  int relaunches = 0;
  std::string skipped;

 private:
  SnapshotCallbackList callbacks_;
};

struct Emitted {
  VersionUpdater::Status status;
  int progress;
  std::u16string message;
};

class RoamuxVersionUpdaterTest : public testing::Test {
 protected:
  void StartChecking() {
    updater_ = std::make_unique<RoamuxVersionUpdater>(&fake_);
    updater_->CheckForUpdate(
        base::BindRepeating(&RoamuxVersionUpdaterTest::OnStatus,
                            base::Unretained(this)),
        base::BindRepeating(&RoamuxVersionUpdaterTest::OnPromote,
                            base::Unretained(this)));
  }

  void OnStatus(VersionUpdater::Status status,
                int progress,
                bool rollback,
                bool powerwash,
                const std::string& version,
                int64_t size,
                const std::u16string& message) {
    emitted_.push_back({status, progress, message});
  }

  void OnPromote(VersionUpdater::PromotionState state) {
    promotions_.push_back(state);
  }

  base::test::TaskEnvironment task_environment_;
  FakeUpdateCommands fake_;
  std::unique_ptr<RoamuxVersionUpdater> updater_;
  std::vector<Emitted> emitted_;
  std::vector<VersionUpdater::PromotionState> promotions_;
};

// --- pure mapping table (the frozen six-state table) ---

TEST(MapSnapshotTest, CheckingMapsToChecking) {
  EXPECT_EQ(VersionUpdater::CHECKING,
            MapSnapshot(Snap(UpdateStatus::kChecking)).status);
}

TEST(MapSnapshotTest, UpToDateMapsToUpdated) {
  EXPECT_EQ(VersionUpdater::UPDATED,
            MapSnapshot(Snap(UpdateStatus::kUpToDate)).status);
}

TEST(MapSnapshotTest, AvailableMapsToNeedPermissionWithVersionMessage) {
  const MappedStatus m =
      MapSnapshot(Snap(UpdateStatus::kAvailable, "0.0.2-alpha.1"));
  EXPECT_EQ(VersionUpdater::NEED_PERMISSION_TO_UPDATE, m.status);
  EXPECT_NE(std::u16string::npos, m.message.find(u"0.0.2-alpha.1"))
      << "the available message must carry the offered version";
}

TEST(MapSnapshotTest, DownloadingMapsToUpdatingWithPercent) {
  const MappedStatus m =
      MapSnapshot(Snap(UpdateStatus::kDownloading, "", "", 0.42));
  EXPECT_EQ(VersionUpdater::UPDATING, m.status);
  EXPECT_EQ(42, m.progress);
}

TEST(MapSnapshotTest, ProgressIsZeroOutsideUpdating) {
  EXPECT_EQ(0, MapSnapshot(Snap(UpdateStatus::kChecking)).progress);
  EXPECT_EQ(0, MapSnapshot(Snap(UpdateStatus::kUpToDate)).progress);
  EXPECT_EQ(0, MapSnapshot(Snap(UpdateStatus::kReadyToInstall)).progress);
}

TEST(MapSnapshotTest, ReadyToInstallMapsToNearlyUpdated) {
  EXPECT_EQ(VersionUpdater::NEARLY_UPDATED,
            MapSnapshot(Snap(UpdateStatus::kReadyToInstall)).status);
}

TEST(MapSnapshotTest, EveryErrorCollapsesToFailed) {
  // The about-page icon map renders cr:error ONLY for FAILED — variants must
  // ride the message, never the enum.
  const MappedStatus m =
      MapSnapshot(Snap(UpdateStatus::kError, "", "SUAppcastError 2001"));
  EXPECT_EQ(VersionUpdater::FAILED, m.status);
}

TEST(MapSnapshotTest, IdleAfterConstructionDoesNotEmit) {
  EXPECT_FALSE(MapSnapshot(Snap(UpdateStatus::kIdle)).emit);
}

// --- the error-copy table ---

TEST(ClassifyUpdateErrorTest, AppcastErrorsAreCheckFailures) {
  EXPECT_EQ(UpdateErrorClass::kCheckFailed,
            ClassifyUpdateError("the appcast could not be loaded"
                                " (SUAppcastError 2001)"));
}

TEST(ClassifyUpdateErrorTest, DownloadErrorsClassify) {
  EXPECT_EQ(UpdateErrorClass::kDownloadFailed,
            ClassifyUpdateError("an error occurred while downloading the"
                                " update (SUDownloadError 2000)"));
}

TEST(ClassifyUpdateErrorTest, SignatureErrorsClassify) {
  EXPECT_EQ(UpdateErrorClass::kSignatureFailed,
            ClassifyUpdateError("the update is improperly signed and could"
                                " not be validated (SUSignatureError 4001)"));
}

TEST(ClassifyUpdateErrorTest, InstallErrorsClassify) {
  EXPECT_EQ(UpdateErrorClass::kInstallFailed,
            ClassifyUpdateError("an error occurred while installing the"
                                " update (SUInstallationError 4005)"));
}

TEST(MapSnapshotTest, SignatureFailureOffersNoRetry) {
  const MappedStatus m = MapSnapshot(
      Snap(UpdateStatus::kError, "",
           "the update is improperly signed and could not be validated"));
  EXPECT_EQ(VersionUpdater::FAILED, m.status);
  EXPECT_FALSE(m.offer_retry)
      << "signature verification failure must not offer a breezy retry";
}

TEST(MapSnapshotTest, CheckFailureOffersRetry) {
  const MappedStatus m =
      MapSnapshot(Snap(UpdateStatus::kError, "", "SUAppcastError 2001"));
  EXPECT_TRUE(m.offer_retry);
}

// --- the adapter shell ---

TEST_F(RoamuxVersionUpdaterTest, CheckForUpdateDispatchesCheck) {
  StartChecking();
  EXPECT_EQ(1, fake_.checks);
}

TEST_F(RoamuxVersionUpdaterTest, NothingDownloadsUnprompted) {
  fake_.current = Snap(UpdateStatus::kAvailable, "0.0.2");
  StartChecking();
  fake_.Push(Snap(UpdateStatus::kAvailable, "0.0.2"));
  EXPECT_EQ(0, fake_.downloads)
      << "rendering an available update must never trigger a download";
  EXPECT_EQ(0, fake_.relaunches);
}

TEST_F(RoamuxVersionUpdaterTest, SnapshotsForwardAsStatusEvents) {
  StartChecking();
  fake_.Push(Snap(UpdateStatus::kChecking));
  fake_.Push(Snap(UpdateStatus::kUpToDate));
  ASSERT_GE(emitted_.size(), 2u);
  EXPECT_EQ(VersionUpdater::CHECKING, emitted_[emitted_.size() - 2].status);
  EXPECT_EQ(VersionUpdater::UPDATED, emitted_.back().status);
}

TEST_F(RoamuxVersionUpdaterTest, IdleNeverReachesTheRow) {
  StartChecking();
  const size_t before = emitted_.size();
  fake_.Push(Snap(UpdateStatus::kIdle));
  EXPECT_EQ(before, emitted_.size()) << "kIdle must not re-render the row";
}

TEST_F(RoamuxVersionUpdaterTest, PromotePostureIsHidden) {
  StartChecking();
  ASSERT_FALSE(promotions_.empty());
  EXPECT_EQ(VersionUpdater::PROMOTE_HIDDEN, promotions_.front());
}

}  // namespace
}  // namespace roamux::updates
