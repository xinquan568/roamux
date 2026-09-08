// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_UPDATES_ROAMUX_UPDATE_SERVICE_H_
#define ROAMUX_BROWSER_UPDATES_ROAMUX_UPDATE_SERVICE_H_

#include <memory>
#include <optional>
#include <string>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/keyed_service/core/keyed_service.h"
#include "roamux/browser/updates/update_state_machine.h"
#include "roamux/browser/updates/version_updater_seam.h"

// The Roamux update service (roam-85, I-6.5; roam-160): a per-profile facade
// over ONE process-wide Sparkle owner (updates are app-wide). It implements
// the UpdateCommands seam — the native About row's RoamuxVersionUpdater
// subscribes to UpdateSnapshot state and AboutHandler dispatches the consent
// commands — with the state coming from the pure UpdateStateMachine, driven
// by a custom SPUUserDriver conformer that stores Sparkle's found/ready reply
// blocks so Download/Skip/InstallAndRelaunch can drive them. (The roam-140
// Mojo update-page surface retired with the update card.) Flag-on
// (roamux_enable_sparkle) only.
namespace roamux::updates {

// Opaque process-wide Sparkle owner (defined in the .mm; holds the single
// SPUUpdater + conformer). Exposed as a forward decl so the header stays
// Objective-C-free for C++ includers.
class SparkleOwner;

// Returns the ONE process-wide Sparkle owner (exactly one SPUUpdater on
// [NSBundle mainBundle]), creating and starting it — including its scheduled
// background checks — on first call. Shared by every per-profile
// RoamuxUpdateService AND by the app-launch roamux::app::InitSparkleUpdater();
// this single-owner rule is the roam-140 fix for the two-owner second-click
// hang. The returned owner is process-lived (never torn down). Flag-on only
// (roamux_enable_sparkle).
SparkleOwner* GetOrCreateSharedSparkleOwner();

// The event sink signature: the conformer hands translated events to the
// owner, which broadcasts them to every subscribed facade.
using EventCallback = base::RepeatingCallback<void(const UpdateEvent&)>;

// --- roam-287 test seams (the owner stays opaque to C++ includers) ---------
// Deleter for an owner-for-testing (the destructor is private; the .mm
// defines operator()).
struct SparkleOwnerDeleter {
  void operator()(SparkleOwner* owner) const;
};
// The injected start outcome for an owner-for-testing: it runs the PRODUCTION
// start-result handling on this outcome and allocates no SPUUpdater (so the
// one-updater rule of roam-140 holds in tests and every command is inert).
struct InjectedSparkleStart {
  bool started = true;
  std::string domain;
  int code = 0;
  std::string description;
};
std::unique_ptr<SparkleOwner, SparkleOwnerDeleter> CreateSparkleOwnerForTesting(
    const InjectedSparkleStart& start);
const std::optional<std::string>& SparkleOwnerStartErrorForTesting(
    const SparkleOwner* owner);
bool AutomaticChecksEnabledForTesting(const SparkleOwner* owner);
base::CallbackListSubscription AddSparkleOwnerEventSinkForTesting(
    SparkleOwner* owner,
    EventCallback callback);

class RoamuxUpdateService : public KeyedService, public UpdateCommands {
 public:
  RoamuxUpdateService();
  // roam-287: bind to a specific owner (tests bind an owner-for-testing
  // through the keyed-service testing factory). The default constructor
  // delegates with the shared process-wide owner.
  explicit RoamuxUpdateService(SparkleOwner* owner);
  ~RoamuxUpdateService() override;

  RoamuxUpdateService(const RoamuxUpdateService&) = delete;
  RoamuxUpdateService& operator=(const RoamuxUpdateService&) = delete;

  // UpdateCommands:
  void CheckForUpdates() override;
  void Download() override;
  void InstallAndRelaunch() override;
  void Skip(const std::string& version) override;

  // UpdateCommands (roam-160): the non-Mojo egress for the native About row's
  // RoamuxVersionUpdater. Fires immediately with the current snapshot.
  base::CallbackListSubscription SubscribeToSnapshots(
      SnapshotCallbackList::CallbackType callback) override;

  // Called by the conformer (on the main thread) with a translated event.
  // Also the browsertests' state-injection seam (roam-160): synthetic events
  // exercise the full production chain minus live Sparkle.
  void OnUpdateEvent(const UpdateEvent& event);

  const UpdateSnapshot& snapshot_for_testing() const {
    return state_machine_.snapshot();
  }
  const std::string& skipped_version_for_testing() const {
    return state_machine_.skipped_version_for_testing();
  }
  // Dispatch counters (roam-160 step-8 F2): the browsertest proves each About
  // button's click actually reaches the service verb.
  int checks_for_testing() const { return checks_for_testing_; }
  int downloads_for_testing() const { return downloads_for_testing_; }
  int relaunches_for_testing() const { return relaunches_for_testing_; }

 private:
  void PushSnapshot(const UpdateSnapshot& snapshot);

  // Member order is load-bearing (roam-287): the owner REPLAYS retained
  // events synchronously inside AddEventSink, i.e. OnUpdateEvent runs during
  // the constructor body — everything it touches (the machine, the snapshot
  // list) is declared here, before shared_owner_/sink_subscription_, and the
  // WeakPtrFactory is last (fully constructed before the body runs).
  UpdateStateMachine state_machine_;
  int checks_for_testing_ = 0;
  int downloads_for_testing_ = 0;
  int relaunches_for_testing_ = 0;
  // roam-160: non-Mojo snapshot listeners (the native-row adapter).
  SnapshotCallbackList snapshot_callbacks_;
  // The single process-wide Sparkle owner (not owned uniquely; process-lived).
  raw_ptr<SparkleOwner> shared_owner_ = nullptr;
  // This facade's subscription to the owner's app-wide event broadcast;
  // dropping it (on destruction) auto-unregisters the sink (roam-140).
  base::CallbackListSubscription sink_subscription_;
  base::WeakPtrFactory<RoamuxUpdateService> weak_factory_{this};
};

}  // namespace roamux::updates

#endif  // ROAMUX_BROWSER_UPDATES_ROAMUX_UPDATE_SERVICE_H_
