// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/updates/roamux_update_user_driver.h"

#include <utility>

#include "base/strings/sys_string_conversions.h"

@implementation RoamuxUpdateUserDriver {
  roamux::updates::EventCallback _cb;
  uint64_t _expectedLength;
  uint64_t _receivedLength;
  // roam-287: the pending offer — set with foundReply, cleared with it.
  std::optional<roamux::updates::UpdateEvent> _pendingFound;
}
@synthesize foundReply = _foundReply;
@synthesize readyReply = _readyReply;

- (instancetype)initWithCallback:(roamux::updates::EventCallback)cb {
  if ((self = [super init])) {
    _cb = std::move(cb);
  }
  return self;
}

- (void)emit:(roamux::updates::UpdateEventType)type {
  roamux::updates::UpdateEvent e;
  e.type = type;
  _cb.Run(e);
}

- (std::optional<roamux::updates::UpdateEvent>)pendingFoundEvent {
  return _pendingFound;
}

// Consumption invalidates BEFORE it invokes: Sparkle may re-enter this driver
// synchronously from the reply (progress callbacks, showUpdateInFocus), and a
// facade constructed inside that window subscribes — neither may see an offer
// whose reply is gone.
- (void)consumeFoundReplyWith:(SPUUserUpdateChoice)choice {
  void (^reply)(SPUUserUpdateChoice) = self.foundReply;
  if (!reply) {
    return;
  }
  self.foundReply = nil;
  _pendingFound.reset();
  reply(choice);
}

- (void)commandDownload {
  [self consumeFoundReplyWith:SPUUserUpdateChoiceInstall];
}

- (void)commandInstallAndRelaunch {
  if (self.readyReply) {
    void (^reply)(SPUUserUpdateChoice) = self.readyReply;
    self.readyReply = nil;
    reply(SPUUserUpdateChoiceInstall);
  }
}

- (void)commandSkipVersion:(NSString*)version {
  [self consumeFoundReplyWith:SPUUserUpdateChoiceSkip];
}

// A session-ending callback: whatever offer was pending is gone.
- (void)dropPendingOffer {
  _pendingFound.reset();
  self.foundReply = nil;
}

// SPUUserDriver — the state-driving callbacks:
- (void)showUpdatePermissionRequest:(SPUUpdatePermissionRequest*)request
                              reply:
                                  (void (^)(SUUpdatePermissionResponse*))reply {
  reply([[SUUpdatePermissionResponse alloc] initWithAutomaticUpdateChecks:YES
                                                        sendSystemProfile:NO]);
}
- (void)showUserInitiatedUpdateCheckWithCancellation:
    (void (^)(void))cancellation {
  [self emit:roamux::updates::UpdateEventType::kCheckStarted];
}
- (void)showUpdateFoundWithAppcastItem:(SUAppcastItem*)appcastItem
                                 state:(SPUUserUpdateState*)state
                                 reply:(void (^)(SPUUserUpdateChoice))reply {
  // Scheduled (state.userInitiated == NO) and user-initiated finds arrive
  // here alike; the state machine surfaces both (roam-287/H11).
  self.foundReply = reply;
  roamux::updates::UpdateEvent e;
  e.type = roamux::updates::UpdateEventType::kUpdateFound;
  e.version = base::SysNSStringToUTF8(appcastItem.displayVersionString
                                          ?: appcastItem.versionString);
  e.notes = base::SysNSStringToUTF8(appcastItem.itemDescription ?: @"");
  _pendingFound = e;
  _cb.Run(e);
}
- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData*)downloadData {
}
- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError*)error {
}
- (void)showUpdateNotFoundWithError:(NSError*)error
                    acknowledgement:(void (^)(void))acknowledgement {
  [self dropPendingOffer];
  [self emit:roamux::updates::UpdateEventType::kUpToDate];
  acknowledgement();
}
- (void)showUpdaterError:(NSError*)error
         acknowledgement:(void (^)(void))acknowledgement {
  [self dropPendingOffer];
  roamux::updates::UpdateEvent e;
  e.type = roamux::updates::UpdateEventType::kError;
  e.error = base::SysNSStringToUTF8(error.localizedDescription);
  _cb.Run(e);
  acknowledgement();
}
- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation {
  // The offer was accepted (its reply consumed); nothing is pending any more.
  _pendingFound.reset();
  [self emit:roamux::updates::UpdateEventType::kDownloadStarted];
}
- (void)showDownloadDidReceiveExpectedContentLength:(uint64_t)length {
  _expectedLength = length;
}
- (void)showDownloadDidReceiveDataOfLength:(uint64_t)length {
  _receivedLength += length;
  roamux::updates::UpdateEvent e;
  e.type = roamux::updates::UpdateEventType::kDownloadProgress;
  e.received = static_cast<int64_t>(_receivedLength);
  e.total = static_cast<int64_t>(_expectedLength);
  _cb.Run(e);
}
- (void)showDownloadDidStartExtractingUpdate {
}
- (void)showExtractionReceivedProgress:(double)progress {
}
- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply {
  self.readyReply = reply;
  [self emit:roamux::updates::UpdateEventType::kReadyToInstall];
}
- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)terminated
                          retryTerminatingApplication:(void (^)(void))retry {
}
- (void)showUpdateInstalledAndRelaunched:(BOOL)relaunched
                         acknowledgement:(void (^)(void))acknowledgement {
  acknowledgement();
}
- (void)dismissUpdateInstallation {
  [self dropPendingOffer];
}
- (void)showUpdateInFocus {
  // A manual "Check for updates" while a session is open (roam-287/H11): the
  // pending offer is re-broadcast so the row shows it; nothing pending, nothing
  // to show.
  if (_pendingFound) {
    _cb.Run(*_pendingFound);
  }
}

@end
