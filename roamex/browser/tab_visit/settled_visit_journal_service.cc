// SPDX-License-Identifier: Apache-2.0
#include "roamex/browser/tab_visit/settled_visit_journal_service.h"

#include <utility>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "roamex/common/roamex_features.h"
#include "url/gurl.h"

namespace roamex::tab_visit {

SettledVisitJournalService::SettledVisitJournalService(bool in_memory,
                                                       base::FilePath db_path) {
  // Inert when the epic's flag is off: no store is opened, so no journal DB (on
  // disk or in memory) is ever created.
  if (!base::FeatureList::IsEnabled(features::kTabVisitNav)) {
    return;
  }
  store_ = base::SequenceBound<VisitsStore>(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE}));
  // Open on the store sequence; the bool result is not needed on the UI thread
  // (a failed open leaves the store closed, so reads return empty).
  if (in_memory) {
    store_.AsyncCall(&VisitsStore::OpenInMemory).Then(base::BindOnce([](bool) {
    }));
  } else {
    store_.AsyncCall(&VisitsStore::Open)
        .WithArgs(std::move(db_path))
        .Then(base::BindOnce([](bool) {}));
  }
}

SettledVisitJournalService::~SettledVisitJournalService() = default;

void SettledVisitJournalService::RecordVisit(const GURL& url) {
  if (store_.is_null()) {
    return;
  }
  store_.AsyncCall(&VisitsStore::Append)
      .WithArgs(url.spec(), base::Time::Now());
}

void SettledVisitJournalService::GetVisits(
    base::OnceCallback<void(std::vector<VisitRow>)> callback) {
  if (store_.is_null()) {
    std::move(callback).Run({});
    return;
  }
  store_.AsyncCall(&VisitsStore::ReadAll).Then(std::move(callback));
}

void SettledVisitJournalService::Shutdown() {
  store_.Reset();
}

}  // namespace roamex::tab_visit
