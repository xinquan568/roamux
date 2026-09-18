// SPDX-License-Identifier: Apache-2.0
#include "roamux/test/support/local_storage_snapshot_probe.h"

#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/threading/thread_restrictions.h"
#include "roamux/browser/importer/edge_local_storage_reader.h"
#include "roamux/test/support/local_storage_seed_ack.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "url/origin.h"

namespace roamux::test {

SnapshotLocalStorageProbe::SnapshotLocalStorageProbe() = default;
SnapshotLocalStorageProbe::SnapshotLocalStorageProbe(
    const SnapshotLocalStorageProbe&) = default;
SnapshotLocalStorageProbe::SnapshotLocalStorageProbe(
    SnapshotLocalStorageProbe&&) = default;
SnapshotLocalStorageProbe::~SnapshotLocalStorageProbe() = default;

std::string SnapshotLocalStorageProbe::Describe(
    const base::FilePath& snapshot_profile) const {
  return "\n  snapshot=" + snapshot_profile.AsUTF8Unsafe() +
         "\n  want storage_key=" + want_storage_key +
         "\n  want key=" + want_key_hex + " value=" + want_value_hex +
         "\n  origins_read=" + base::NumberToString(origins_read) +
         " matching_origin=" + (origin_seen ? "yes" : "no") +
         "\n  observed for that origin:" +
         (observed.empty() ? std::string(" <none>") : observed);
}

SnapshotLocalStorageProbe ProbeSnapshotLocalStorage(
    const base::FilePath& snapshot_profile,
    const GURL& origin,
    std::string_view key,
    std::string_view value) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  const blink::StorageKey want_key =
      blink::StorageKey::CreateFirstParty(url::Origin::Create(origin));
  const std::vector<uint8_t> want_entry_key = Latin1Encoded(key);
  const std::vector<uint8_t> want_entry_value = Latin1Encoded(value);
  SnapshotLocalStorageProbe probe;
  probe.want_storage_key = want_key.GetDebugString();
  probe.want_key_hex = base::HexEncode(want_entry_key);
  probe.want_value_hex = base::HexEncode(want_entry_value);
  for (const OriginLocalStorage& o : ReadEdgeLocalStorage(snapshot_profile)) {
    ++probe.origins_read;
    if (o.storage_key != want_key) {
      continue;
    }
    probe.origin_seen = true;
    for (const LocalStorageEntry& e : o.entries) {
      probe.observed += " {key=" + base::HexEncode(e.key) +
                        " value=" + base::HexEncode(e.value) + "}";
      if (e.key == want_entry_key && e.value == want_entry_value) {
        probe.found = true;
      }
    }
  }
  return probe;
}

}  // namespace roamux::test
