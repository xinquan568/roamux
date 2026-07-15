// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/updates/roamux_update_service.h"

#import <Sparkle/Sparkle.h>

#include <utility>

#include "base/functional/callback.h"
#include "base/strings/sys_string_conversions.h"

namespace roamux::updates {

namespace {

// Callback the conformer uses to hand a translated event back to the service.
using EventCallback = base::RepeatingCallback<void(const UpdateEvent&)>;

}  // namespace

}  // namespace roamux::updates

// The custom SPUUserDriver: translates each Sparkle callback into an
// UpdateEvent (roam-85) and stores the found/ready reply blocks so the service
// commands can drive them (Sparkle has no Download/Install methods).
@interface RoamuxUpdateUserDriver : NSObject <SPUUserDriver>
@property(nonatomic, copy) void (^foundReply)(SPUUserUpdateChoice);
@property(nonatomic, copy) void (^readyReply)(SPUUserUpdateChoice);
- (instancetype)initWithCallback:(roamux::updates::EventCallback)cb;
- (void)commandDownload;
- (void)commandInstallAndRelaunch;
- (void)commandSkipVersion:(NSString*)version;
@end

@implementation RoamuxUpdateUserDriver {
  roamux::updates::EventCallback _cb;
  uint64_t _expectedLength;
  uint64_t _receivedLength;
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

- (void)commandDownload {
  if (self.foundReply) {
    self.foundReply(SPUUserUpdateChoiceInstall);
    self.foundReply = nil;
  }
}

- (void)commandInstallAndRelaunch {
  if (self.readyReply) {
    self.readyReply(SPUUserUpdateChoiceInstall);
    self.readyReply = nil;
  }
}

- (void)commandSkipVersion:(NSString*)version {
  if (self.foundReply) {
    self.foundReply(SPUUserUpdateChoiceSkip);
    self.foundReply = nil;
  }
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
  self.foundReply = reply;
  roamux::updates::UpdateEvent e;
  e.type = roamux::updates::UpdateEventType::kUpdateFound;
  e.version = base::SysNSStringToUTF8(appcastItem.displayVersionString
                                          ?: appcastItem.versionString);
  e.notes = base::SysNSStringToUTF8(appcastItem.itemDescription ?: @"");
  _cb.Run(e);
}
- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData*)downloadData {
}
- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError*)error {
}
- (void)showUpdateNotFoundWithError:(NSError*)error
                    acknowledgement:(void (^)(void))acknowledgement {
  [self emit:roamux::updates::UpdateEventType::kUpToDate];
  acknowledgement();
}
- (void)showUpdaterError:(NSError*)error
         acknowledgement:(void (^)(void))acknowledgement {
  roamux::updates::UpdateEvent e;
  e.type = roamux::updates::UpdateEventType::kError;
  e.error = base::SysNSStringToUTF8(error.localizedDescription);
  _cb.Run(e);
  acknowledgement();
}
- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation {
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
}
- (void)showUpdateInFocus {
}

@end

namespace roamux::updates {

// The ONE process-wide Sparkle owner: exactly one SPUUpdater + conformer for
// the whole app (updates are app-wide), shared by every per-profile facade AND
// by the app-launch roamux::app::InitSparkleUpdater(). Created — and started,
// with scheduled background checks enabled — on first
// GetOrCreateSharedSparkleOwner(), then process-lived (never torn down) so the
// app-wide scheduled checks keep running across profile teardown. The event
// callback is re-pointed at whichever facade is currently bound via a shared
// sink; the sink holds a WeakPtr-bound callback, so a facade going away
// self-cancels without leaving a dangling callback. (roam-140: retiring the
// second SPUStandardUpdaterController owner — two owners on one bundle were the
// second-click hang — leaves exactly one owner here.)
class SparkleOwner {
 public:
  static SparkleOwner* GetOrCreate();

  void SetEventSink(EventCallback cb) { *sink_ = std::move(cb); }

  void CheckForUpdates() { [updater_ checkForUpdates]; }
  void Download() { [driver_ commandDownload]; }
  void InstallAndRelaunch() { [driver_ commandInstallAndRelaunch]; }
  void Skip(const std::string& version) {
    [driver_ commandSkipVersion:base::SysUTF8ToNSString(version)];
  }

 private:
  SparkleOwner() {
    sink_ = std::make_shared<EventCallback>();
    auto sink = sink_;
    driver_ = [[RoamuxUpdateUserDriver alloc]
        initWithCallback:base::BindRepeating(
                             [](std::shared_ptr<EventCallback> s,
                                const UpdateEvent& e) {
                               if (*s) {
                                 s->Run(e);
                               }
                             },
                             sink)];
    updater_ = [[SPUUpdater alloc] initWithHostBundle:[NSBundle mainBundle]
                                    applicationBundle:[NSBundle mainBundle]
                                           userDriver:driver_
                                             delegate:nil];
    NSError* error = nil;
    [updater_ startUpdater:&error];
    // roam-140: this single owner now also runs the scheduled background checks
    // the retired SPUStandardUpdaterController used to (Info.plist
    // SUEnableAutomaticChecks / SUScheduledCheckInterval).
    updater_.automaticallyChecksForUpdates = YES;
  }
  ~SparkleOwner() = default;

  std::shared_ptr<EventCallback> sink_;
  RoamuxUpdateUserDriver* driver_ = nil;
  SPUUpdater* updater_ = nil;
};

// Process-lived singleton, intentionally never deleted (base::NoDestructor
// semantics): the one owner drives app-wide scheduled checks for the whole
// process lifetime.
SparkleOwner* g_process_owner = nullptr;

// static
SparkleOwner* SparkleOwner::GetOrCreate() {
  if (!g_process_owner) {
    g_process_owner = new SparkleOwner();
  }
  return g_process_owner;
}

SparkleOwner* GetOrCreateSharedSparkleOwner() {
  return SparkleOwner::GetOrCreate();
}

RoamuxUpdateService::RoamuxUpdateService() {
  // Bind this per-profile facade to the ONE process-wide owner (also created at
  // launch by InitSparkleUpdater) and route its events here. The sink holds a
  // WeakPtr-bound callback, so it self-cancels if this facade is destroyed.
  shared_owner_ = GetOrCreateSharedSparkleOwner();
  shared_owner_->SetEventSink(base::BindRepeating(
      &RoamuxUpdateService::OnUpdateEvent, weak_factory_.GetWeakPtr()));
}

RoamuxUpdateService::~RoamuxUpdateService() {
  // The owner is process-lived and persists across facade teardown; the event
  // sink is a WeakPtr-bound callback that self-cancels when this facade dies,
  // so no explicit unbind is needed (and unbinding here would clobber a
  // concurrently-bound sibling facade).
  shared_owner_ = nullptr;
}

void RoamuxUpdateService::BindFactory(
    mojo::PendingReceiver<mojom::UpdatePageHandlerFactory> receiver) {
  factory_receiver_.reset();
  factory_receiver_.Bind(std::move(receiver));
}

void RoamuxUpdateService::CreatePageHandler(
    mojo::PendingRemote<mojom::UpdatePage> page,
    mojo::PendingReceiver<mojom::UpdatePageHandler> handler) {
  page_.reset();
  page_.Bind(std::move(page));
  handler_receiver_.reset();
  handler_receiver_.Bind(std::move(handler));
  PushSnapshot(state_machine_.snapshot());
}

void RoamuxUpdateService::CheckForUpdates() {
  shared_owner_->CheckForUpdates();
}
void RoamuxUpdateService::Download() {
  shared_owner_->Download();
}
void RoamuxUpdateService::InstallAndRelaunch() {
  shared_owner_->InstallAndRelaunch();
}
void RoamuxUpdateService::Skip(const std::string& version) {
  state_machine_.SetSkippedVersion(version);
  shared_owner_->Skip(version);
}

void RoamuxUpdateService::OnUpdateEvent(const UpdateEvent& event) {
  PushSnapshot(state_machine_.OnEvent(event));
}

void RoamuxUpdateService::PushSnapshot(const UpdateSnapshot& snapshot) {
  if (!page_) {
    return;
  }
  auto out = mojom::UpdateSnapshot::New();
  out->status = static_cast<mojom::UpdateStatus>(snapshot.status);
  out->version = snapshot.version;
  out->date = snapshot.date;
  out->notes = snapshot.notes;
  out->error = snapshot.error;
  out->progress = snapshot.progress;
  page_->OnStateChanged(std::move(out));
}

}  // namespace roamux::updates
