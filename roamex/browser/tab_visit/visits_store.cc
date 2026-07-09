// SPDX-License-Identifier: Apache-2.0
#include "roamex/browser/tab_visit/visits_store.h"

#include <utility>

#include "base/files/file_util.h"
#include "sql/meta_table.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace roamex::tab_visit {

namespace {

// SQLite UMA tag — registered as a variant in
// tools/metrics/histograms/metadata/sql/histograms.xml (patch 0015).
constexpr char kDatabaseTag[] = "RoamexSettledVisitJournal";

constexpr int kCurrentVersion = 1;
constexpr int kCompatibleVersion = 1;

int64_t ToStorage(base::Time t) {
  return t.ToDeltaSinceWindowsEpoch().InMicroseconds();
}

base::Time FromStorage(int64_t v) {
  return base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(v));
}

}  // namespace

VisitsStore::VisitsStore() : db_(sql::Database::Tag(kDatabaseTag)) {}

VisitsStore::~VisitsStore() = default;

bool VisitsStore::Open(const base::FilePath& db_path) {
  if (db_.Open(db_path) && InitSchema()) {
    return true;
  }
  // The on-disk journal is corrupt/unusable/incompatible. It is a best-effort,
  // rebuildable record — delete it and recreate a fresh one rather than failing
  // the browser.
  db_.Close();
  base::DeleteFile(db_path);
  return db_.Open(db_path) && InitSchema();
}

bool VisitsStore::OpenInMemory() {
  return db_.OpenInMemory() && InitSchema();
}

bool VisitsStore::InitSchema() {
  static constexpr char kCreateTable[] =
      "CREATE TABLE IF NOT EXISTS visits("
      "id INTEGER PRIMARY KEY AUTOINCREMENT,"
      "url TEXT NOT NULL,"
      "visited_at INTEGER NOT NULL)";

  sql::MetaTable meta;
  // A future/incompatible schema is treated as unusable so the caller resets
  // it.
  return meta.Init(&db_, kCurrentVersion, kCompatibleVersion) &&
         meta.GetCompatibleVersionNumber() <= kCurrentVersion &&
         db_.Execute(kCreateTable);
}

void VisitsStore::Append(const std::string& url, base::Time visited_at) {
  if (!db_.is_open()) {
    return;
  }
  sql::Transaction transaction(&db_);
  if (!transaction.Begin()) {
    return;
  }

  sql::Statement insert(
      db_.GetUniqueStatement("INSERT INTO visits(url,visited_at) VALUES(?,?)"));
  insert.BindString(0, url);
  insert.BindInt64(1, ToStorage(visited_at));
  if (!insert.Run()) {
    return;
  }

  // FIFO trim: keep only the newest kMaxVisits rows (oldest-out). At the
  // (kMaxVisits+1)th append this evicts exactly one row.
  sql::Statement trim(db_.GetUniqueStatement(
      "DELETE FROM visits WHERE id NOT IN "
      "(SELECT id FROM visits ORDER BY id DESC LIMIT ?)"));
  trim.BindInt64(0, static_cast<int64_t>(kMaxVisits));
  if (!trim.Run()) {
    return;
  }

  transaction.Commit();
}

std::vector<VisitRow> VisitsStore::ReadAll() {
  std::vector<VisitRow> rows;
  if (!db_.is_open()) {
    return rows;
  }
  sql::Statement s(db_.GetUniqueStatement(
      "SELECT id,url,visited_at FROM visits ORDER BY id ASC"));
  while (s.Step()) {
    VisitRow row;
    row.id = s.ColumnInt64(0);
    row.url = s.ColumnString(1);
    row.visited_at = FromStorage(s.ColumnInt64(2));
    rows.push_back(std::move(row));
  }
  return rows;
}

size_t VisitsStore::RowCount() {
  if (!db_.is_open()) {
    return 0;
  }
  sql::Statement s(db_.GetUniqueStatement("SELECT COUNT(*) FROM visits"));
  if (s.Step()) {
    return static_cast<size_t>(s.ColumnInt64(0));
  }
  return 0;
}

}  // namespace roamex::tab_visit
