// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMEX_BROWSER_TAB_VISIT_SETTLED_VISIT_JOURNAL_SERVICE_H_
#define ROAMEX_BROWSER_TAB_VISIT_SETTLED_VISIT_JOURNAL_SERVICE_H_

#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/threading/sequence_bound.h"
#include "components/keyed_service/core/keyed_service.h"
#include "roamex/browser/tab_visit/visits_store.h"

class GURL;

namespace roamex::tab_visit {

// The per-profile settled-visit journal service (roam-21 / I-4.1) — the E4
// storage substrate the later links navigate over. It owns a VisitsStore on a
// MayBlock sequence (SQLite I/O never runs on the UI thread) and exposes an
// append + read API on the UI thread.
//
// The factory decides the store's location: a regular profile passes an on-disk
// `db_path`; an OTR/incognito profile passes `in_memory=true` so nothing ever
// touches disk. When `roamex::features::kTabVisitNav` is disabled the service
// is inert — it opens NO store, so a disabled build leaves zero on-disk
// footprint.
class SettledVisitJournalService : public KeyedService {
 public:
  // `in_memory` true (OTR) opens an in-memory DB and ignores `db_path`; false
  // (regular) opens/creates the DB at `db_path`.
  SettledVisitJournalService(bool in_memory, base::FilePath db_path);
  SettledVisitJournalService(const SettledVisitJournalService&) = delete;
  SettledVisitJournalService& operator=(const SettledVisitJournalService&) =
      delete;
  ~SettledVisitJournalService() override;

  // Appends a settled visit (async; committed on the store sequence). No-op
  // when the feature is disabled.
  void RecordVisit(const GURL& url);

  // Reads all retained visits (oldest-first) and runs `callback` on the
  // caller's sequence. Returns an empty vector when the feature is disabled.
  void GetVisits(base::OnceCallback<void(std::vector<VisitRow>)> callback);

  // KeyedService:
  void Shutdown() override;

 private:
  // Null (unbound) when the feature is disabled — the inert state.
  base::SequenceBound<VisitsStore> store_;
};

}  // namespace roamex::tab_visit

#endif  // ROAMEX_BROWSER_TAB_VISIT_SETTLED_VISIT_JOURNAL_SERVICE_H_
