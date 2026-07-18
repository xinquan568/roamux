// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_UPDATES_ROAMUX_VERSION_UPDATER_H_
#define ROAMUX_BROWSER_UPDATES_ROAMUX_VERSION_UPDATER_H_

#include <string>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/webui/help/version_updater.h"  // nogncheck
#include "roamux/browser/updates/version_updater_seam.h"

// roam-160: Chromium's VersionUpdater implemented over the Roamux Sparkle
// machinery, so chrome://settings/help's NATIVE update row renders our state
// (the Brave shape, with manual Download/Skip consent kept via
// NEED_PERMISSION_TO_UPDATE). Selected by patch 0040 in
// VersionUpdater::Create() under ROAMUX_ENABLE_SPARKLE; commands from the
// About WebUI arrive via patch 0040's AboutHandler messages, which call the
// UpdateCommands seam directly. This file is roamux-owned but compiled into
// the upstream target that owns version_updater_mac.mm (the §12.2
// compiled-into pattern — no GN cycle).
namespace roamux::updates {

// The plain-language error classes of the issue's copy table. Signature
// failures deliberately get NO retry affordance.
enum class UpdateErrorClass {
  kCheckFailed,      // network / appcast unreachable
  kDownloadFailed,   // download interrupted
  kSignatureFailed,  // EdDSA verification failed — security-relevant
  kInstallFailed,    // install step failed
};

struct MappedStatus {
  VersionUpdater::Status status = VersionUpdater::CHECKING;
  int progress = 0;          // 0..100, non-zero only while UPDATING
  std::u16string message;    // plain-language copy (may be empty)
  bool offer_retry = false;  // [Try again] visibility (FAILED only)
  bool emit = true;          // kIdle after a check never re-renders
};

// Pure mapping: mojom-mirrored snapshot -> native-row render state, per the
// roam-160 table. Exhaustively unit-tested; the adapter is a thin shell.
MappedStatus MapSnapshot(const UpdateSnapshot& snapshot);

// Pure classifier over the Sparkle error text (the conformer's message).
UpdateErrorClass ClassifyUpdateError(const std::string& error_text);

class RoamuxVersionUpdater : public VersionUpdater {
 public:
  // `commands` must outlive this (the per-profile service outlives the
  // per-page updater).
  explicit RoamuxVersionUpdater(UpdateCommands* commands);
  ~RoamuxVersionUpdater() override;

  RoamuxVersionUpdater(const RoamuxVersionUpdater&) = delete;
  RoamuxVersionUpdater& operator=(const RoamuxVersionUpdater&) = delete;

  // VersionUpdater:
  void CheckForUpdate(StatusCallback status_callback,
                      PromoteCallback promote_callback) override;
#if BUILDFLAG(IS_MAC)
  void PromoteUpdater() override;
#endif

 private:
  void OnSnapshot(const UpdateSnapshot& snapshot);

  const raw_ptr<UpdateCommands> commands_;
  StatusCallback status_callback_;
  base::CallbackListSubscription subscription_;
};

}  // namespace roamux::updates

#endif  // ROAMUX_BROWSER_UPDATES_ROAMUX_VERSION_UPDATER_H_
