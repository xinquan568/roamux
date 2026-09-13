// SPDX-License-Identifier: Apache-2.0
#include "roamux/test/support/storage_flush_barrier.h"

#include <vector>

#include "base/check.h"
#include "base/test/test_future.h"
#include "components/services/storage/public/mojom/local_storage_control.mojom.h"
#include "components/services/storage/public/mojom/storage_usage_info.mojom.h"
#include "content/public/browser/storage_partition.h"

namespace roamux::test {

void FlushLocalStorageAndWait(content::StoragePartition* partition) {
  partition->GetLocalStorageControl()->Flush();
  base::test::TestFuture<std::vector<storage::mojom::StorageUsageInfoPtr>>
      usage;
  partition->GetLocalStorageControl()->GetUsage(usage.GetCallback());
  // CHECK, deliberately not ASSERT_*: a fatal gtest assertion here would
  // return from this helper only, leaving the caller to proceed as if the
  // barrier had held.
  CHECK(usage.Wait()) << "GetUsage() never replied; local-storage commits are "
                         "not ordered against the caller";
}

}  // namespace roamux::test
