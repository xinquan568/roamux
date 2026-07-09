// SPDX-License-Identifier: Apache-2.0
// roam-21 (I-4.1): the settled-visit journal service + factory — regular and
// OTR get separate instances, OTR never touches disk, the regular journal
// persists across reopen, and a disabled feature creates no journal DB. All
// RecordVisit- dependent assertions drain the store's SequenceBound (via
// GetVisits) first.

#include "roamex/browser/tab_visit/settled_visit_journal_service.h"

#include <memory>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/browser_task_environment.h"
#include "roamex/browser/tab_visit/settled_visit_journal_factory.h"
#include "roamex/browser/tab_visit/visits_store.h"
#include "roamex/common/roamex_features.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace roamex::tab_visit {
namespace {

std::vector<VisitRow> GetVisitsSync(SettledVisitJournalService* service) {
  base::test::TestFuture<std::vector<VisitRow>> future;
  service->GetVisits(future.GetCallback());
  return future.Take();
}

class SettledVisitJournalServiceTestBase : public testing::Test {
 public:
  SettledVisitJournalServiceTestBase() {
    ChromeBrowserMainExtraPartsProfiles::
        EnsureBrowserContextKeyedServiceFactoriesBuilt();
  }

 protected:
  base::FilePath JournalPath(Profile* profile) {
    return profile->GetPath().Append(FILE_PATH_LITERAL("RoamexTabVisits"));
  }

  content::BrowserTaskEnvironment task_environment_;
  TestingProfile profile_;
};

class SettledVisitJournalServiceTest
    : public SettledVisitJournalServiceTestBase {
 public:
  SettledVisitJournalServiceTest() {
    features_.InitAndEnableFeature(features::kTabVisitNav);
  }

 private:
  base::test::ScopedFeatureList features_;
};

TEST_F(SettledVisitJournalServiceTest, RegularAndOtrGetSeparateInstances) {
  Profile* otr = profile_.GetPrimaryOTRProfile(/*create_if_needed=*/true);
  SettledVisitJournalService* regular =
      SettledVisitJournalFactory::GetForProfile(&profile_);
  SettledVisitJournalService* otr_service =
      SettledVisitJournalFactory::GetForProfile(otr);
  ASSERT_TRUE(regular);
  ASSERT_TRUE(otr_service);
  EXPECT_NE(regular, otr_service);
}

TEST_F(SettledVisitJournalServiceTest, OtrRecordsInMemoryNeverOnDisk) {
  Profile* otr = profile_.GetPrimaryOTRProfile(/*create_if_needed=*/true);
  SettledVisitJournalService* service =
      SettledVisitJournalFactory::GetForProfile(otr);
  service->RecordVisit(GURL("https://incognito.test/x"));

  // Drain the store sequence: the visit is retained in memory ...
  std::vector<VisitRow> rows = GetVisitsSync(service);
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ("https://incognito.test/x", rows[0].url);

  // ... but nothing is ever written to disk.
  EXPECT_FALSE(base::PathExists(JournalPath(otr)));
  EXPECT_FALSE(base::PathExists(JournalPath(&profile_)));
}

TEST_F(SettledVisitJournalServiceTest, RegularJournalPersistsAcrossReopen) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath path =
      dir.GetPath().Append(FILE_PATH_LITERAL("RoamexTabVisits"));
  {
    SettledVisitJournalService service(/*in_memory=*/false, path);
    service.RecordVisit(GURL("https://persist.test/y"));
    ASSERT_EQ(1u, GetVisitsSync(&service).size());  // barrier: commit lands
    service.Shutdown();
  }
  // A fresh service on the SAME path reads the persisted row back.
  SettledVisitJournalService reopened(/*in_memory=*/false, path);
  std::vector<VisitRow> rows = GetVisitsSync(&reopened);
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ("https://persist.test/y", rows[0].url);
  reopened.Shutdown();
}

class SettledVisitJournalServiceDisabledTest
    : public SettledVisitJournalServiceTestBase {
 public:
  SettledVisitJournalServiceDisabledTest() {
    features_.InitAndDisableFeature(features::kTabVisitNav);
  }

 private:
  base::test::ScopedFeatureList features_;
};

TEST_F(SettledVisitJournalServiceDisabledTest, FeatureDisabledCreatesNoDb) {
  SettledVisitJournalService* service =
      SettledVisitJournalFactory::GetForProfile(&profile_);
  ASSERT_TRUE(service);
  service->RecordVisit(GURL("https://disabled.test/z"));
  // Inert: no visits, no journal file.
  EXPECT_TRUE(GetVisitsSync(service).empty());
  EXPECT_FALSE(base::PathExists(JournalPath(&profile_)));
}

}  // namespace
}  // namespace roamex::tab_visit
