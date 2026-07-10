// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMEX_BROWSER_UPDATES_FAKE_UPDATE_PAGE_HANDLER_H_
#define ROAMEX_BROWSER_UPDATES_FAKE_UPDATE_PAGE_HANDLER_H_

#include <string>
#include <vector>

#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "roamex/mojom/update_page.mojom.h"

// Test doubles (roam-85) so roam-37's WebUI can build + test against the real
// UpdatePageHandler/UpdatePage interfaces without a live Sparkle service.
namespace roamex::updates {

// Captures each command the WebUI issues; pushes snapshots on demand.
class FakeUpdatePageHandler : public mojom::UpdatePageHandler {
 public:
  FakeUpdatePageHandler();
  ~FakeUpdatePageHandler() override;

  void Bind(mojo::PendingRemote<mojom::UpdatePage> page,
            mojo::PendingReceiver<mojom::UpdatePageHandler> handler);

  // Pushes a snapshot to the bound page (drives the WebUI card in tests).
  void PushState(mojom::UpdateSnapshotPtr snapshot);

  // mojom::UpdatePageHandler:
  void CheckForUpdates() override;
  void Download() override;
  void InstallAndRelaunch() override;
  void Skip(const std::string& version) override;

  int check_count() const { return check_count_; }
  int download_count() const { return download_count_; }
  int install_count() const { return install_count_; }
  const std::vector<std::string>& skipped() const { return skipped_; }

 private:
  int check_count_ = 0;
  int download_count_ = 0;
  int install_count_ = 0;
  std::vector<std::string> skipped_;
  mojo::Remote<mojom::UpdatePage> page_;
  mojo::Receiver<mojom::UpdatePageHandler> receiver_{this};
};

}  // namespace roamex::updates

#endif  // ROAMEX_BROWSER_UPDATES_FAKE_UPDATE_PAGE_HANDLER_H_
