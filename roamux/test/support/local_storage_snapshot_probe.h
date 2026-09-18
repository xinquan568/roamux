// SPDX-License-Identifier: Apache-2.0
// roam-338 (lifted from roam-331's driver test): read a profile SNAPSHOT's
// localStorage the way the importer will, and report want-vs-observed.

#ifndef ROAMUX_TEST_SUPPORT_LOCAL_STORAGE_SNAPSHOT_PROBE_H_
#define ROAMUX_TEST_SUPPORT_LOCAL_STORAGE_SNAPSHOT_PROBE_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "url/gurl.h"

namespace roamux::test {

// Result of ProbeSnapshotLocalStorage(). `observed` lists every entry the
// reader returned for the origin as " {key=<hex> value=<hex>}". An empty read
// is missing-or-unreadable — the reader soft-fails to empty on a missing,
// locked or corrupt DB — so the result reports what was observed without
// attributing an empty read to either cause.
struct SnapshotLocalStorageProbe {
  SnapshotLocalStorageProbe();
  SnapshotLocalStorageProbe(const SnapshotLocalStorageProbe&);
  SnapshotLocalStorageProbe(SnapshotLocalStorageProbe&&);
  ~SnapshotLocalStorageProbe();

  bool origin_seen = false;
  bool found = false;
  size_t origins_read = 0;
  std::string observed;
  // What was looked for, for the message: the StorageKey's debug string and
  // the encoded key / value as hex.
  std::string want_storage_key;
  std::string want_key_hex;
  std::string want_value_hex;

  // Multi-line want-vs-observed text for an assertion message, in the order
  // roam-331's driver diagnostic used: snapshot, want storage_key, want
  // key/value, origins_read + matching_origin, observed entries. The caller
  // prepends what only it knows (its live profile path).
  std::string Describe(const base::FilePath& snapshot_profile) const;
};

// Opens `snapshot_profile`'s `Local Storage/leveldb` through
// ReadEdgeLocalStorage — the importer's own reader, which works on a private
// copy — and looks for `key` == `value` (Latin1-encoded) under `origin`'s
// first-party StorageKey. Probe the SNAPSHOT, never the live profile: the live
// store proves nothing about the copy the importer reads. Call it BEFORE the
// origin is cleared, while a failure still points at the seed. Allows blocking
// internally (the reader does file I/O).
SnapshotLocalStorageProbe ProbeSnapshotLocalStorage(
    const base::FilePath& snapshot_profile,
    const GURL& origin,
    std::string_view key,
    std::string_view value);

}  // namespace roamux::test

#endif  // ROAMUX_TEST_SUPPORT_LOCAL_STORAGE_SNAPSHOT_PROBE_H_
