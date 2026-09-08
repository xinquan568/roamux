// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/updates/roamux_update_service.h"

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

#include <optional>
#include <string>
#include <utility>

#include "base/callback_list.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "roamux/browser/updates/roamux_update_user_driver.h"

namespace roamux::updates {

// The ONE process-wide Sparkle owner: exactly one SPUUpdater + conformer for
// the whole app (updates are app-wide), shared by every per-profile facade AND
// by the app-launch roamux::app::InitSparkleUpdater(). Created — and started,
// with scheduled background checks enabled — on first GetOrCreate(), then
// process-lived (base::NoDestructor) so the app-wide scheduled checks keep
// running across profile teardown. Every live per-profile facade subscribes
// its own event sink via AddEventSink(): a Sparkle callback is broadcast to
// ALL current subscribers (roam-140). Each sink is a WeakPtr-bound callback
// held alive by the subscribing facade's CallbackListSubscription.
//
// roam-287 (grill H10 / M8): the start result is no longer ignored — a failed
// -startUpdater: is logged, RETAINED as a tagged kError and REPLAYED to every
// sink at subscription (facades always subscribe after the owner exists, so a
// construction-time broadcast would reach nobody); a dead updater never gets
// automatic checks enabled. The owner is a NoDestructor singleton created on
// the main thread with sequence checks on every entry point — the bare global
// with unguarded lazy init is gone. The driver's PENDING OFFER (H11) is
// replayed to late sinks as well, for as long as its reply is outstanding.
class SparkleOwner {
 public:
  static SparkleOwner* GetOrCreate();

  // Subscribe a facade's event sink; the returned subscription auto-unregisters
  // when the caller (the per-profile facade) is destroyed. Replays, to this
  // sink only and before returning: the retained start failure, then the
  // pending offer.
  base::CallbackListSubscription AddEventSink(EventCallback cb);

  void CheckForUpdates();
  void Download();
  void InstallAndRelaunch();
  void Skip(const std::string& version);

 private:
  friend class base::NoDestructor<SparkleOwner>;
  friend struct SparkleOwnerDeleter;
  friend std::unique_ptr<SparkleOwner, SparkleOwnerDeleter>
  CreateSparkleOwnerForTesting(const InjectedSparkleStart& start);
  friend const std::optional<std::string>& SparkleOwnerStartErrorForTesting(
      const SparkleOwner* owner);
  friend bool AutomaticChecksEnabledForTesting(const SparkleOwner* owner);
  friend base::CallbackListSubscription AddSparkleOwnerEventSinkForTesting(
      SparkleOwner* owner,
      EventCallback callback);
  friend RoamuxUpdateUserDriver* DriverForTesting(SparkleOwner* owner);

  // Production: the real SPUUpdater on the main bundle, started here.
  SparkleOwner();
  // Tests: the real driver, NO SPUUpdater, the injected start outcome routed
  // through the same HandleStartResult as production.
  explicit SparkleOwner(const InjectedSparkleStart& injected);
  ~SparkleOwner();

  void CreateDriver();
  // The one place a start outcome is interpreted (production and tests).
  void HandleStartResult(bool started, NSError* error);
  void NotifySinks(const UpdateEvent& e) { sinks_.Notify(e); }

  SEQUENCE_CHECKER(sequence_checker_);
  base::RepeatingCallbackList<void(const UpdateEvent&)> sinks_;
  RoamuxUpdateUserDriver* driver_ = nil;
  SPUUpdater* updater_ = nil;
  // roam-287/H10: set iff -startUpdater: failed; retained for the process.
  std::optional<std::string> start_error_;
  bool automatic_checks_enabled_ = false;
};

SparkleOwner::SparkleOwner() {
  // Sparkle requires the main thread for startUpdater:/checkForUpdates; the
  // first GetOrCreate() (applicationDidFinishLaunching or the first facade)
  // must be there. A loud crash beats a silently mis-threaded updater (M8).
  CHECK([NSThread isMainThread]) << "SparkleOwner must be created on the main thread";
  CreateDriver();
  updater_ = [[SPUUpdater alloc] initWithHostBundle:[NSBundle mainBundle]
                                  applicationBundle:[NSBundle mainBundle]
                                         userDriver:driver_
                                           delegate:nil];
  NSError* error = nil;
  const BOOL started = [updater_ startUpdater:&error];
  HandleStartResult(started == YES, error);
}

SparkleOwner::SparkleOwner(const InjectedSparkleStart& injected) {
  CreateDriver();
  NSError* error = nil;
  if (!injected.started) {
    error = [NSError
        errorWithDomain:base::SysUTF8ToNSString(injected.domain)
                   code:injected.code
               userInfo:@{
                 NSLocalizedDescriptionKey :
                     base::SysUTF8ToNSString(injected.description)
               }];
  }
  HandleStartResult(injected.started, error);
}

SparkleOwner::~SparkleOwner() = default;

void SparkleOwner::CreateDriver() {
  // base::Unretained is safe: the owner outlives its driver (the production
  // owner is never destroyed; an owner-for-testing destroys the driver with
  // itself).
  driver_ = [[RoamuxUpdateUserDriver alloc]
      initWithCallback:base::BindRepeating(&SparkleOwner::NotifySinks,
                                           base::Unretained(this))];
}

void SparkleOwner::HandleStartResult(bool started, NSError* error) {
  if (started) {
    // roam-140: this single owner runs the scheduled background checks the
    // retired SPUStandardUpdaterController used to (Info.plist
    // SUEnableAutomaticChecks / SUScheduledCheckInterval).
    automatic_checks_enabled_ = true;
    if (updater_) {
      updater_.automaticallyChecksForUpdates = YES;
    }
    return;
  }
  // roam-287 (grill H10): Sparkle documents this as THE misconfiguration
  // signal (bad SUFeedURL, missing/invalid SUPublicEDKey, no bundle id…). The
  // updater is dead for the life of the process: record it, log it once, and
  // never pretend to schedule checks on it.
  const std::string domain =
      error ? base::SysNSStringToUTF8(error.domain) : "unknown";
  const std::string description =
      error ? base::SysNSStringToUTF8(error.localizedDescription)
            : "no error object";
  start_error_ =
      base::StrCat({kUpdaterUnavailableErrorPrefix, "[", domain, " ",
                    base::NumberToString(error ? error.code : 0), "] ",
                    description});
  LOG(ERROR) << "Sparkle updater failed to start: " << *start_error_;
}

// static
SparkleOwner* SparkleOwner::GetOrCreate() {
  static base::NoDestructor<SparkleOwner> owner;
  DCHECK_CALLED_ON_VALID_SEQUENCE(owner->sequence_checker_);
  return owner.get();
}

base::CallbackListSubscription SparkleOwner::AddEventSink(EventCallback cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (start_error_) {
    UpdateEvent e;
    e.type = UpdateEventType::kError;
    e.error = *start_error_;
    cb.Run(e);
  }
  if (std::optional<UpdateEvent> pending = [driver_ pendingFoundEvent]) {
    cb.Run(*pending);
  }
  return sinks_.Add(std::move(cb));
}

void SparkleOwner::CheckForUpdates() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  [updater_ checkForUpdates];  // nil (dead or test owner): inert
}
void SparkleOwner::Download() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  [driver_ commandDownload];
}
void SparkleOwner::InstallAndRelaunch() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  [driver_ commandInstallAndRelaunch];
}
void SparkleOwner::Skip(const std::string& version) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  [driver_ commandSkipVersion:base::SysUTF8ToNSString(version)];
}

SparkleOwner* GetOrCreateSharedSparkleOwner() {
  return SparkleOwner::GetOrCreate();
}

// --- test seams -------------------------------------------------------------

void SparkleOwnerDeleter::operator()(SparkleOwner* owner) const {
  delete owner;
}

std::unique_ptr<SparkleOwner, SparkleOwnerDeleter> CreateSparkleOwnerForTesting(
    const InjectedSparkleStart& start) {
  return std::unique_ptr<SparkleOwner, SparkleOwnerDeleter>(
      new SparkleOwner(start));
}

const std::optional<std::string>& SparkleOwnerStartErrorForTesting(
    const SparkleOwner* owner) {
  return owner->start_error_;
}

bool AutomaticChecksEnabledForTesting(const SparkleOwner* owner) {
  return owner->automatic_checks_enabled_;
}

base::CallbackListSubscription AddSparkleOwnerEventSinkForTesting(
    SparkleOwner* owner,
    EventCallback callback) {
  return owner->AddEventSink(std::move(callback));
}

RoamuxUpdateUserDriver* DriverForTesting(SparkleOwner* owner) {
  return owner->driver_;
}

// --- the per-profile facade --------------------------------------------------

RoamuxUpdateService::RoamuxUpdateService()
    : RoamuxUpdateService(GetOrCreateSharedSparkleOwner()) {}

RoamuxUpdateService::RoamuxUpdateService(SparkleOwner* owner) {
  // Bind this per-profile facade to the owner and SUBSCRIBE its own event
  // sink. Multiple facades can be subscribed at once (app-wide broadcast); the
  // subscription is WeakPtr-bound and held by this facade, so it
  // auto-unregisters on teardown without clobbering a sibling facade. The
  // owner replays its retained events into OnUpdateEvent right here (see the
  // member-order note in the header).
  shared_owner_ = owner;
  sink_subscription_ = shared_owner_->AddEventSink(base::BindRepeating(
      &RoamuxUpdateService::OnUpdateEvent, weak_factory_.GetWeakPtr()));
}

RoamuxUpdateService::~RoamuxUpdateService() {
  // The owner is process-lived (or, in tests, outlives the profile). Dropping
  // sink_subscription_ (member destruction) auto-unregisters this facade's
  // sink from the owner's broadcast list — no sibling facade is affected.
  shared_owner_ = nullptr;
}

void RoamuxUpdateService::CheckForUpdates() {
  ++checks_for_testing_;
  shared_owner_->CheckForUpdates();
}
void RoamuxUpdateService::Download() {
  ++downloads_for_testing_;
  shared_owner_->Download();
}
void RoamuxUpdateService::InstallAndRelaunch() {
  ++relaunches_for_testing_;
  shared_owner_->InstallAndRelaunch();
}
void RoamuxUpdateService::Skip(const std::string& version) {
  state_machine_.SetSkippedVersion(version);
  shared_owner_->Skip(version);
}

void RoamuxUpdateService::OnUpdateEvent(const UpdateEvent& event) {
  PushSnapshot(state_machine_.OnEvent(event));
}

base::CallbackListSubscription RoamuxUpdateService::SubscribeToSnapshots(
    SnapshotCallbackList::CallbackType callback) {
  // roam-160: fire immediately (this subscriber only) so the native row
  // renders the current state, then on every change.
  auto subscription = snapshot_callbacks_.Add(callback);
  callback.Run(state_machine_.snapshot());
  return subscription;
}

void RoamuxUpdateService::PushSnapshot(const UpdateSnapshot& snapshot) {
  snapshot_callbacks_.Notify(snapshot);
}

}  // namespace roamux::updates
