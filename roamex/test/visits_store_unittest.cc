// SPDX-License-Identifier: Apache-2.0
// roam-21 (I-4.1): the writable visits store — append-only, the 11-entry
// scenario, cap-999 FIFO (oldest-out), on-disk-vs-in-memory, close/reopen
// persistence, and raze-on-corrupt.

#include "roamex/browser/tab_visit/visits_store.h"

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/stringprintf.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamex::tab_visit {
namespace {

class VisitsStoreTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.CreateUniqueTempDir()); }
  base::FilePath DbPath() {
    return dir_.GetPath().Append(FILE_PATH_LITERAL("RoamexTabVisits"));
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
};

TEST_F(VisitsStoreTest, AppendOnlyElevenEntryScenarioInOrder) {
  VisitsStore store;
  ASSERT_TRUE(store.OpenInMemory());
  const base::Time t0 = base::Time::Now();
  for (int i = 0; i < 11; ++i) {
    store.Append(base::StringPrintf("https://example.test/%d", i),
                 t0 + base::Seconds(i));
  }
  std::vector<VisitRow> rows = store.ReadAll();
  ASSERT_EQ(11u, rows.size());
  for (int i = 0; i < 11; ++i) {
    EXPECT_EQ(base::StringPrintf("https://example.test/%d", i), rows[i].url);
    EXPECT_EQ(t0 + base::Seconds(i), rows[i].visited_at);
  }
}

TEST_F(VisitsStoreTest, Cap999FifoEvictsOldest) {
  VisitsStore store;
  ASSERT_TRUE(store.OpenInMemory());
  const base::Time t0 = base::Time::Now();
  for (int i = 0; i < 1000; ++i) {
    store.Append(base::StringPrintf("https://example.test/%d", i),
                 t0 + base::Milliseconds(i));
  }
  EXPECT_EQ(VisitsStore::kMaxVisits, store.RowCount());
  std::vector<VisitRow> rows = store.ReadAll();
  ASSERT_EQ(VisitsStore::kMaxVisits, rows.size());
  // Oldest (index 0) evicted; newest (index 999) retained.
  EXPECT_EQ("https://example.test/1", rows.front().url);
  EXPECT_EQ("https://example.test/999", rows.back().url);
}

TEST_F(VisitsStoreTest, OnDiskCreatesFileInMemoryDoesNot) {
  {
    VisitsStore file_store;
    ASSERT_TRUE(file_store.Open(DbPath()));
    file_store.Append("https://a.test/", base::Time::Now());
  }
  EXPECT_TRUE(base::PathExists(DbPath()));

  const base::FilePath other =
      dir_.GetPath().Append(FILE_PATH_LITERAL("mem-marker"));
  {
    VisitsStore mem_store;
    ASSERT_TRUE(mem_store.OpenInMemory());
    mem_store.Append("https://b.test/", base::Time::Now());
    EXPECT_EQ(1u, mem_store.ReadAll().size());
  }
  // An in-memory store never creates any file (no path was ever supplied).
  EXPECT_FALSE(base::PathExists(other));
}

TEST_F(VisitsStoreTest, PersistsAcrossCloseAndReopen) {
  const base::Time t = base::Time::Now();
  {
    VisitsStore store;
    ASSERT_TRUE(store.Open(DbPath()));
    store.Append("https://persist.test/x", t);
  }
  VisitsStore reopened;
  ASSERT_TRUE(reopened.Open(DbPath()));
  std::vector<VisitRow> rows = reopened.ReadAll();
  ASSERT_EQ(1u, rows.size());
  EXPECT_EQ("https://persist.test/x", rows[0].url);
  EXPECT_EQ(t, rows[0].visited_at);
}

TEST_F(VisitsStoreTest, CorruptDatabaseIsRazedAndReset) {
  ASSERT_TRUE(base::WriteFile(DbPath(), "this is not a sqlite database"));
  VisitsStore store;
  ASSERT_TRUE(store.Open(DbPath()));
  EXPECT_EQ(0u, store.RowCount());
  store.Append("https://after-raze.test/", base::Time::Now());
  EXPECT_EQ(1u, store.RowCount());
}

}  // namespace
}  // namespace roamex::tab_visit
