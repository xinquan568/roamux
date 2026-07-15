// SPDX-License-Identifier: Apache-2.0
#include "roamux/app/roamux_sparkle_updater.h"

#include "roamux/browser/updates/roamux_update_service.h"

namespace roamux::app {

void InitSparkleUpdater() {
  // roam-140: consolidated to ONE process-wide Sparkle owner. This retires the
  // old SPUStandardUpdaterController that used to live here — it was the SECOND
  // SPUUpdater on [NSBundle mainBundle] and, together with the WebUI-driven
  // owner, was the root cause of the second-click "Check for updates" hang.
  // Creating the shared owner starts its SPUUpdater and enables the scheduled
  // background checks (Info.plist SUEnableAutomaticChecks /
  // SUScheduledCheckInterval); the settings/help update page drives the very
  // same owner on demand.
  roamux::updates::GetOrCreateSharedSparkleOwner();
}

}  // namespace roamux::app
