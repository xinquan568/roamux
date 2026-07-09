// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMEX_BROWSER_IMPORTER_ROAMEX_ORIGIN_STORAGE_IMPORT_STAGE_H_
#define ROAMEX_BROWSER_IMPORTER_ROAMEX_ORIGIN_STORAGE_IMPORT_STAGE_H_

#include <cstddef>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "roamex/browser/importer/edge_local_storage_reader.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"

class Profile;

namespace roamex {

// The browser-side first-party localStorage import stage (roam-17 / I-3.3):
// reads Edge's Local Storage LevelDB (copy-to-temp, read-only) and writes the
// entries into the destination profile's localStorage via LocalStorageControl
// / StorageArea::Put (bytes verbatim; the destination store is authoritative).
// Runs entirely browser-side (operator-ruled). Invoked by the import flow
// (roam-20) alongside the other import stages.
class RoamexOriginStorageImportStage {
 public:
  RoamexOriginStorageImportStage(base::FilePath source_path, Profile* profile);
  RoamexOriginStorageImportStage(const RoamexOriginStorageImportStage&) =
      delete;
  RoamexOriginStorageImportStage& operator=(
      const RoamexOriginStorageImportStage&) = delete;
  ~RoamexOriginStorageImportStage();

  // Imports all first-party localStorage entries; runs `done(accepted)` with
  // the number of Put()s the destination accepted, after all async writes
  // complete.
  void Import(base::OnceCallback<void(size_t accepted)> done);

 private:
  void OnRead(std::vector<OriginLocalStorage> origins);
  void OnPutComplete(bool success);

  const base::FilePath source_path_;
  const raw_ptr<Profile> profile_;
  // StorageArea remotes are held alive until all Puts complete.
  std::vector<mojo::Remote<blink::mojom::StorageArea>> areas_;
  size_t pending_ = 0;
  size_t accepted_ = 0;
  base::OnceCallback<void(size_t)> done_;
};

}  // namespace roamex

#endif  // ROAMEX_BROWSER_IMPORTER_ROAMEX_ORIGIN_STORAGE_IMPORT_STAGE_H_
