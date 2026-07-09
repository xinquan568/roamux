// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMEX_BROWSER_TAB_VISIT_VISITS_STORE_H_
#define ROAMEX_BROWSER_TAB_VISIT_VISITS_STORE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "sql/database.h"

namespace roamex::tab_visit {

// One settled-visit row.
struct VisitRow {
  int64_t id = 0;
  std::string url;
  base::Time visited_at;
};

// The writable, append-only SQLite `visits` store backing the settled-visit
// journal (roam-21 / I-4.1). It is the FIRST writable overlay-owned SQLite DB.
//
// Contract: rows are only ever appended (never mutated), and the table is
// FIFO-capped at kMaxVisits — appending the (kMaxVisits+1)th row evicts the
// oldest. The store is opened either on disk (regular profile) or in memory
// (OTR/incognito — never touches disk). A corrupt/incompatible database is
// razed and reset rather than failing the browser.
//
// Blocking by construction (SQLite I/O): the owner runs it on a MayBlock
// sequence (see SettledVisitJournalService's base::SequenceBound); it must
// never be used on the UI thread.
class VisitsStore {
 public:
  // The FIFO cap on the number of retained visits.
  static constexpr size_t kMaxVisits = 999;

  VisitsStore();
  VisitsStore(const VisitsStore&) = delete;
  VisitsStore& operator=(const VisitsStore&) = delete;
  ~VisitsStore();

  // Opens (or creates) the on-disk database at `db_path`. Returns true on
  // success. A corrupt DB is razed and reopened empty.
  bool Open(const base::FilePath& db_path);

  // Opens a fresh in-memory database (OTR): nothing is ever written to disk.
  bool OpenInMemory();

  // Appends a visit and trims the table back to kMaxVisits (oldest-out), in one
  // transaction. No-op if the store is not open.
  void Append(const std::string& url, base::Time visited_at);

  // All retained visits, oldest-first (ascending id).
  std::vector<VisitRow> ReadAll();

  // The number of retained visits.
  size_t RowCount();

  bool is_open() const { return db_.is_open(); }

 private:
  // Initializes the schema (MetaTable v1 + the `visits` table), razing a
  // corrupt/incompatible database and retrying once.
  bool InitSchema();

  sql::Database db_;
};

}  // namespace roamex::tab_visit

#endif  // ROAMEX_BROWSER_TAB_VISIT_VISITS_STORE_H_
