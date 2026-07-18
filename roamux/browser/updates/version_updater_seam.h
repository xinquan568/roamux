// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_BROWSER_UPDATES_VERSION_UPDATER_SEAM_H_
#define ROAMUX_BROWSER_UPDATES_VERSION_UPDATER_SEAM_H_

#include <string>

#include "base/callback_list.h"
#include "roamux/browser/updates/update_state_machine.h"

// roam-160: the seam between RoamuxUpdateService and consumers that are not
// the Mojo page (the RoamuxVersionUpdater adapter behind the native About
// row). Pure C++ — no Sparkle, no Mojo — so adapter tests run against a fake.
// RoamuxUpdateService implements it; the snapshot subscription fires
// immediately with the current snapshot, then on every change.
namespace roamux::updates {

using SnapshotCallbackList =
    base::RepeatingCallbackList<void(const UpdateSnapshot&)>;

class UpdateCommands {
 public:
  virtual ~UpdateCommands() = default;

  virtual void CheckForUpdates() = 0;
  virtual void Download() = 0;
  virtual void InstallAndRelaunch() = 0;
  virtual void Skip(const std::string& version) = 0;

  // Subscribes to snapshot changes; the callback is invoked synchronously
  // with the current snapshot before this returns.
  virtual base::CallbackListSubscription SubscribeToSnapshots(
      SnapshotCallbackList::CallbackType callback) = 0;
};

}  // namespace roamux::updates

#endif  // ROAMUX_BROWSER_UPDATES_VERSION_UPDATER_SEAM_H_
