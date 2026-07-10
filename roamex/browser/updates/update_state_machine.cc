// SPDX-License-Identifier: Apache-2.0
#include "roamex/browser/updates/update_state_machine.h"

namespace roamex::updates {

UpdateEvent::UpdateEvent() = default;
UpdateEvent::UpdateEvent(UpdateEventType t) : type(t) {}
UpdateEvent::UpdateEvent(const UpdateEvent&) = default;
UpdateEvent::~UpdateEvent() = default;

UpdateSnapshot::UpdateSnapshot() = default;
UpdateSnapshot::UpdateSnapshot(const UpdateSnapshot&) = default;
UpdateSnapshot::~UpdateSnapshot() = default;

UpdateStateMachine::UpdateStateMachine() = default;
UpdateStateMachine::~UpdateStateMachine() = default;

void UpdateStateMachine::SetSkippedVersion(const std::string& version) {
  skipped_version_ = version;
}

UpdateSnapshot UpdateStateMachine::OnEvent(const UpdateEvent& event) {
  switch (event.type) {
    case UpdateEventType::kCheckStarted:
      snapshot_ = UpdateSnapshot{};
      snapshot_.status = UpdateStatus::kChecking;
      break;

    case UpdateEventType::kUpToDate:
      snapshot_.status = UpdateStatus::kUpToDate;
      break;

    case UpdateEventType::kUpdateFound:
      if (!event.version.empty() && event.version == skipped_version_) {
        // A skipped version is treated as up-to-date (never surfaced).
        snapshot_.status = UpdateStatus::kUpToDate;
      } else {
        snapshot_.status = UpdateStatus::kAvailable;
        snapshot_.version = event.version;
        snapshot_.date = event.date;
        snapshot_.notes = event.notes;
      }
      break;

    case UpdateEventType::kDownloadStarted:
      if (snapshot_.status == UpdateStatus::kAvailable) {
        snapshot_.status = UpdateStatus::kDownloading;
        snapshot_.progress = 0.0;
      }
      break;

    case UpdateEventType::kDownloadProgress:
      if (snapshot_.status == UpdateStatus::kDownloading && event.total > 0) {
        snapshot_.progress = static_cast<double>(event.received) / event.total;
      }
      break;

    case UpdateEventType::kReadyToInstall:
      if (snapshot_.status == UpdateStatus::kDownloading) {
        snapshot_.status = UpdateStatus::kReadyToInstall;
        snapshot_.progress = 1.0;
      }
      break;

    case UpdateEventType::kError:
      snapshot_.status = UpdateStatus::kError;
      snapshot_.error = event.error;
      break;
  }
  return snapshot_;
}

}  // namespace roamex::updates
