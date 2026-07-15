// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_APP_ROAMUX_SPARKLE_UPDATER_H_
#define ROAMUX_APP_ROAMUX_SPARKLE_UPDATER_H_

// Sparkle updater surface (roam-32, plan §13.6/K4), compiled only when
// roamux_enable_sparkle=true; the patch-0025 InitSparkleUpdater() hunk in
// app_controller_mac.mm applicationDidFinishLaunching: is guarded by
// BUILDFLAG(ROAMUX_ENABLE_SPARKLE).
namespace roamux::app {

// Ensures the single process-wide Sparkle owner exists and enables its
// scheduled background update checks. Idempotent; call from
// applicationDidFinishLaunching. (roam-140: consolidated to ONE SPUUpdater
// owner on [NSBundle mainBundle], shared with the settings/help update page —
// this is the fix for the original two-owner second-click hang. The retired
// "Check for update" menu item and the SPUStandardUpdaterController that backed
// it are gone.)
void InitSparkleUpdater();

}  // namespace roamux::app

#endif  // ROAMUX_APP_ROAMUX_SPARKLE_UPDATER_H_
