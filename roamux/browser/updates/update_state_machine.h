// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_UPDATES_UPDATE_STATE_MACHINE_H_
#define ROAMUX_BROWSER_UPDATES_UPDATE_STATE_MACHINE_H_

#include <string>

// The pure Termixion update state machine (roam-85, I-6.5). No Sparkle, no
// Chrome — the SPUUserDriver conformer and the Mojo handler both drive it, and
// roam-37's About card renders its snapshot. Keeping it pure makes the rich
// logic exhaustively unit-testable.
namespace roamux::updates {

// roam-287 (grill H10): the SparkleOwner tags the kError it retains when
// -[SPUUpdater startUpdater:] fails with this prefix. The About-row adapter
// classifies by the tag FIRST (Sparkle's own prose for a key-configuration
// failure can mention "signed"/"verified" and would otherwise be routed to
// the signature class) and renders a non-retryable "updates unavailable" row.
inline constexpr char kUpdaterUnavailableErrorPrefix[] = "updater-unavailable: ";

enum class UpdateStatus {
  kIdle,
  kChecking,
  kUpToDate,
  kAvailable,
  kDownloading,
  kReadyToInstall,
  kError,
};

enum class UpdateEventType {
  kCheckStarted,
  kUpToDate,
  kUpdateFound,
  kDownloadStarted,
  kDownloadProgress,
  kReadyToInstall,
  kError,
};

struct UpdateEvent {
  UpdateEvent();
  // NOLINTNEXTLINE(google-explicit-constructor) — brace-init ergonomics.
  UpdateEvent(UpdateEventType t);  // NOLINT(runtime/explicit)
  UpdateEvent(const UpdateEvent&);
  ~UpdateEvent();

  UpdateEventType type = UpdateEventType::kCheckStarted;
  std::string version;
  std::string date;
  std::string notes;
  std::string error;
  int64_t received = 0;
  int64_t total = 0;
};

struct UpdateSnapshot {
  UpdateSnapshot();
  UpdateSnapshot(const UpdateSnapshot&);
  ~UpdateSnapshot();

  UpdateStatus status = UpdateStatus::kIdle;
  std::string version;
  std::string date;
  std::string notes;
  std::string error;
  double progress = 0.0;  // 0..1 while downloading
};

class UpdateStateMachine {
 public:
  UpdateStateMachine();
  ~UpdateStateMachine();

  // Skipped versions never surface as kAvailable (persisted by the service).
  void SetSkippedVersion(const std::string& version);

  // Applies one event and returns the resulting snapshot. Illegal transitions
  // are no-ops (the snapshot is unchanged). roam-287/H11: kUpdateFound is
  // accepted from every state except a download in flight — Sparkle delivers
  // a SCHEDULED check's find with no preceding kCheckStarted.
  UpdateSnapshot OnEvent(const UpdateEvent& event);

  const UpdateSnapshot& snapshot() const { return snapshot_; }
  const std::string& skipped_version_for_testing() const {
    return skipped_version_;
  }

 private:
  UpdateSnapshot snapshot_;
  std::string skipped_version_;
};

}  // namespace roamux::updates

#endif  // ROAMUX_BROWSER_UPDATES_UPDATE_STATE_MACHINE_H_
