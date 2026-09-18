// SPDX-License-Identifier: Apache-2.0
// roam-338 (lifted from roam-331's driver test): value-aware acknowledgement
// that a localStorage entry has reached the storage service.

#ifndef ROAMUX_TEST_SUPPORT_LOCAL_STORAGE_SEED_ACK_H_
#define ROAMUX_TEST_SUPPORT_LOCAL_STORAGE_SEED_ACK_H_

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "base/test/test_future.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"
#include "url/gurl.h"

namespace content {
class StoragePartition;
}  // namespace content

namespace roamux::test {

// Blink's on-the-wire encoding of a Latin1-only localStorage key or value:
// a one-byte format prefix (StorageFormat::Latin1 == 0x01) followed by the
// text bytes
// (third_party/blink/renderer/modules/storage/cached_storage_area.cc). A
// plain-string comparison against on-disk or in-service bytes never matches.
std::vector<uint8_t> Latin1Encoded(std::string_view text);

// Acknowledges that the storage service holds `key` == `value` for `origin`'s
// first-party StorageKey.
//
// Why it exists: a renderer's localStorage.setItem sends an ASYNCHRONOUS
// StorageArea::Put on the StorageArea pipe; content::ExecJs replies on the
// frame pipe. Different pipes, no cross-pipe ordering — ExecJs returning proves
// nothing about the Put having arrived, so a LocalStorageControl::Flush issued
// right after it can flush nothing (roam-331).
//
// How: the constructor binds a StorageArea for the key and issues the ASYNC
// GetAll overload (the [Sync] one would hit UI-thread sync-call restrictions)
// with this object as the one-shot StorageAreaObserver. The observer sees every
// event AFTER the returned snapshot (storage_area.mojom), so the entry is
// either in the snapshot or arrives later as KeyChanged. Both paths compare the
// ENCODED key AND value — a key-only match cannot tell two seeds of the same
// key apart. The remote and the receiver live as long as this object.
//
// Two-phase (WaitForSnapshotReply, then Wait) so a control can place a write
// deterministically after the snapshot reply and exercise the observer path.
class LocalStorageEntryAck : public blink::mojom::StorageAreaObserver {
 public:
  LocalStorageEntryAck(content::StoragePartition* partition,
                       const GURL& origin,
                       std::string_view key,
                       std::string_view value);
  LocalStorageEntryAck(const LocalStorageEntryAck&) = delete;
  LocalStorageEntryAck& operator=(const LocalStorageEntryAck&) = delete;
  ~LocalStorageEntryAck() override;

  // Blocks until GetAll has replied (the snapshot was processed), whether or
  // not it matched. False on timeout.
  [[nodiscard]] bool WaitForSnapshotReply();

  // Blocks until the entry was observed (snapshot or KeyChanged). CHECK-fails —
  // deliberately not ASSERT_*, which would return from this helper only and let
  // the caller proceed past a failed wait — with a want-vs-observed diagnostic.
  void Wait();

  // blink::mojom::StorageAreaObserver:
  void KeyChanged(const std::vector<uint8_t>& key,
                  const std::vector<uint8_t>& new_value,
                  const std::optional<std::vector<uint8_t>>& old_value,
                  blink::mojom::StorageAreaSourcePtr source) override;
  void KeyChangeFailed(const std::vector<uint8_t>& key,
                       blink::mojom::StorageAreaSourcePtr source) override;
  void KeyDeleted(const std::vector<uint8_t>& key,
                  const std::optional<std::vector<uint8_t>>& old_value,
                  blink::mojom::StorageAreaSourcePtr source) override;
  void AllDeleted(bool was_nonempty,
                  blink::mojom::StorageAreaSourcePtr source) override;
  void ShouldSendOldValueOnMutations(bool value) override;

 private:
  void OnGetAll(std::vector<blink::mojom::KeyValuePtr> entries);
  void Resolve();

  const GURL origin_;
  const std::vector<uint8_t> key_;
  const std::vector<uint8_t> value_;

  mojo::Remote<blink::mojom::StorageArea> area_;
  mojo::Receiver<blink::mojom::StorageAreaObserver> receiver_{this};

  // Diagnostic state for the timeout message.
  bool snapshot_replied_ = false;
  std::optional<std::vector<uint8_t>> snapshot_value_;
  int key_changed_seen_ = 0;
  bool resolved_ = false;

  base::test::TestFuture<void> snapshot_reply_;
  base::test::TestFuture<void> matched_;
};

// Construct + Wait(): the common one-shot form.
void AcknowledgeLocalStorageEntry(content::StoragePartition* partition,
                                  const GURL& origin,
                                  std::string_view key,
                                  std::string_view value);

}  // namespace roamux::test

#endif  // ROAMUX_TEST_SUPPORT_LOCAL_STORAGE_SEED_ACK_H_
