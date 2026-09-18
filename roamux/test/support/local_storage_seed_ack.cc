// SPDX-License-Identifier: Apache-2.0
#include "roamux/test/support/local_storage_seed_ack.h"

#include <string>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "components/services/storage/public/mojom/local_storage_control.mojom.h"
#include "content/public/browser/storage_partition.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "url/origin.h"

namespace roamux::test {

std::vector<uint8_t> Latin1Encoded(std::string_view text) {
  std::vector<uint8_t> out;
  out.reserve(text.size() + 1);
  out.push_back(0x01);  // StorageFormat::Latin1
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

LocalStorageEntryAck::LocalStorageEntryAck(content::StoragePartition* partition,
                                           const GURL& origin,
                                           std::string_view key,
                                           std::string_view value)
    : origin_(origin), key_(Latin1Encoded(key)), value_(Latin1Encoded(value)) {
  partition->GetLocalStorageControl()->BindStorageArea(
      blink::StorageKey::CreateFirstParty(url::Origin::Create(origin)),
      area_.BindNewPipeAndPassReceiver());
  // Async overload: the observer is attached atomically with the snapshot.
  area_->GetAll(
      receiver_.BindNewPipeAndPassRemote(),
      base::BindOnce(&LocalStorageEntryAck::OnGetAll, base::Unretained(this)));
}

LocalStorageEntryAck::~LocalStorageEntryAck() = default;

bool LocalStorageEntryAck::WaitForSnapshotReply() {
  return snapshot_reply_.Wait();
}

void LocalStorageEntryAck::Wait() {
  const bool ok = matched_.Wait();
  CHECK(ok) << "localStorage acknowledgement timed out: the storage service "
               "never held the expected entry."
            << "\n  origin=" << origin_ << "\n  key=" << base::HexEncode(key_)
            << " wanted value=" << base::HexEncode(value_)
            << "\n  GetAll replied=" << (snapshot_replied_ ? "yes" : "no")
            << " snapshot value="
            << (snapshot_value_ ? base::HexEncode(*snapshot_value_)
                                : std::string("absent"))
            << "\n  KeyChanged events seen=" << key_changed_seen_;
}

void LocalStorageEntryAck::KeyChanged(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& new_value,
    const std::optional<std::vector<uint8_t>>& old_value,
    blink::mojom::StorageAreaSourcePtr source) {
  ++key_changed_seen_;
  if (key == key_ && new_value == value_) {
    Resolve();
  }
}

void LocalStorageEntryAck::KeyChangeFailed(
    const std::vector<uint8_t>& key,
    blink::mojom::StorageAreaSourcePtr source) {}

void LocalStorageEntryAck::KeyDeleted(
    const std::vector<uint8_t>& key,
    const std::optional<std::vector<uint8_t>>& old_value,
    blink::mojom::StorageAreaSourcePtr source) {}

void LocalStorageEntryAck::AllDeleted(
    bool was_nonempty,
    blink::mojom::StorageAreaSourcePtr source) {}

void LocalStorageEntryAck::ShouldSendOldValueOnMutations(bool value) {}

void LocalStorageEntryAck::OnGetAll(
    std::vector<blink::mojom::KeyValuePtr> entries) {
  snapshot_replied_ = true;
  for (const blink::mojom::KeyValuePtr& kv : entries) {
    if (kv->key == key_) {
      snapshot_value_ = kv->value;
      if (kv->value == value_) {
        Resolve();
      }
    }
  }
  snapshot_reply_.SetValue();
}

void LocalStorageEntryAck::Resolve() {
  if (resolved_) {
    return;
  }
  resolved_ = true;
  matched_.SetValue();
}

void AcknowledgeLocalStorageEntry(content::StoragePartition* partition,
                                  const GURL& origin,
                                  std::string_view key,
                                  std::string_view value) {
  LocalStorageEntryAck ack(partition, origin, key, value);
  ack.Wait();
}

}  // namespace roamux::test
