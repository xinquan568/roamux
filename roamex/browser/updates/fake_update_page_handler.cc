// SPDX-License-Identifier: Apache-2.0
#include "roamex/browser/updates/fake_update_page_handler.h"

#include <utility>

namespace roamex::updates {

FakeUpdatePageHandler::FakeUpdatePageHandler() = default;
FakeUpdatePageHandler::~FakeUpdatePageHandler() = default;

void FakeUpdatePageHandler::Bind(
    mojo::PendingRemote<mojom::UpdatePage> page,
    mojo::PendingReceiver<mojom::UpdatePageHandler> handler) {
  page_.Bind(std::move(page));
  receiver_.Bind(std::move(handler));
}

void FakeUpdatePageHandler::PushState(mojom::UpdateSnapshotPtr snapshot) {
  page_->OnStateChanged(std::move(snapshot));
}

void FakeUpdatePageHandler::CheckForUpdates() {
  ++check_count_;
}

void FakeUpdatePageHandler::Download() {
  ++download_count_;
}

void FakeUpdatePageHandler::InstallAndRelaunch() {
  ++install_count_;
}

void FakeUpdatePageHandler::Skip(const std::string& version) {
  skipped_.push_back(version);
}

}  // namespace roamex::updates
