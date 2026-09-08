// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_UPDATES_ROAMUX_UPDATE_USER_DRIVER_H_
#define ROAMUX_BROWSER_UPDATES_ROAMUX_UPDATE_USER_DRIVER_H_

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include <optional>

#include "roamux/browser/updates/roamux_update_service.h"
#include "roamux/browser/updates/update_state_machine.h"

// The custom SPUUserDriver (roam-85): translates each Sparkle callback into an
// UpdateEvent and stores the found/ready reply blocks so the service commands
// can drive them (Sparkle has no Download/Install methods). Objective-C
// header — included by the owner (.mm) and by tests only; the C++ service
// header stays Objective-C-free.
//
// roam-287 (grill H11): the driver also owns the PENDING OFFER — the last
// kUpdateFound it built, kept exactly as long as its reply block is pending.
// showUpdateInFocus (Sparkle's response to a manual check while a session is
// open) re-broadcasts it; the owner replays it to sinks that subscribe while
// it is pending; consumption (Download / Skip) invalidates it BEFORE the reply
// block runs, and every session-ending callback drops it.
@interface RoamuxUpdateUserDriver : NSObject <SPUUserDriver>
@property(nonatomic, copy) void (^foundReply)(SPUUserUpdateChoice);
@property(nonatomic, copy) void (^readyReply)(SPUUserUpdateChoice);
- (instancetype)initWithCallback:(roamux::updates::EventCallback)cb;
- (void)commandDownload;
- (void)commandInstallAndRelaunch;
- (void)commandSkipVersion:(NSString*)version;
// The pending offer, if its reply is still outstanding.
- (std::optional<roamux::updates::UpdateEvent>)pendingFoundEvent;
@end

namespace roamux::updates {

class SparkleOwner;

// Test seam: the owner's real driver (owner-for-testing or production).
RoamuxUpdateUserDriver* DriverForTesting(SparkleOwner* owner);

}  // namespace roamux::updates

#endif  // ROAMUX_BROWSER_UPDATES_ROAMUX_UPDATE_USER_DRIVER_H_
