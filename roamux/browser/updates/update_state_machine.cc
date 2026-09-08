// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/updates/update_state_machine.h"

namespace roamux::updates {

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
      // Only meaningful as the result of a check.
      if (snapshot_.status == UpdateStatus::kChecking) {
        snapshot_.status = UpdateStatus::kUpToDate;
      }
      break;

    case UpdateEventType::kUpdateFound:
      // roam-287 (grill H11): an update found is an update available, whoever
      // asked — a SCHEDULED check delivers this with no kCheckStarted, and the
      // old "only the outcome of a check" rule dropped every background find.
      // The one thing a find must not disturb is a download in flight.
      if (snapshot_.status == UpdateStatus::kDownloading ||
          snapshot_.status == UpdateStatus::kReadyToInstall) {
        break;
      }
      if (!event.version.empty() && event.version == skipped_version_) {
        // A skipped version never surfaces; it reads as up-to-date only as
        // the outcome of a (user-initiated) check, otherwise nothing changes.
        if (snapshot_.status == UpdateStatus::kChecking) {
          snapshot_.status = UpdateStatus::kUpToDate;
        }
        break;
      }
      snapshot_ = UpdateSnapshot{};  // a fresh offer clears stale error/progress
      snapshot_.status = UpdateStatus::kAvailable;
      snapshot_.version = event.version;
      snapshot_.date = event.date;
      snapshot_.notes = event.notes;
      break;

    case UpdateEventType::kDownloadStarted:
      if (snapshot_.status == UpdateStatus::kAvailable) {
        snapshot_.status = UpdateStatus::kDownloading;
        snapshot_.progress = 0.0;
      }
      break;

    case UpdateEventType::kDownloadProgress:
      if (snapshot_.status == UpdateStatus::kDownloading && event.total > 0) {
        double p = static_cast<double>(event.received) / event.total;
        snapshot_.progress = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
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

}  // namespace roamux::updates
