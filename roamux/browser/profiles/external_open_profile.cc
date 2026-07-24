// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/profiles/external_open_profile.h"

#include <string>

#include "base/command_line.h"
#include "base/feature_list.h"
#include "components/prefs/pref_service.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"

namespace roamux {

namespace {

// Mirrored upstream literals — //roamux/browser/profiles cannot depend on
// //chrome (precedent: kUpstreamVerticalTabsEnabled in roamux_prefs.cc).
// switches::kProfileDirectory, chrome::kGuestProfileDir,
// chrome::kSystemProfileDir respectively.
constexpr char kProfileDirectorySwitch[] = "profile-directory";
constexpr char kGuestProfileDirName[] = "Guest Profile";
constexpr char kSystemProfileDirName[] = "System Profile";

// D3: roamux.profiles.external_open_mode values. 2 (ask each time) is
// RESERVED — until built it resolves as current behavior.
enum ExternalOpenMode {
  kModeActiveProfile = 0,
  kModeDesignatedProfile = 1,
};

}  // namespace

std::optional<base::FilePath> ResolveExternalOpenProfile(
    const base::CommandLine& command_line,
    const PrefService* local_state,
    const base::FilePath& user_data_dir,
    base::FunctionRef<bool(const base::FilePath&)> profile_exists) {
  // The D4 table, in order. Every early return is "current behavior".
  if (!base::FeatureList::IsEnabled(features::kRoamuxExternalOpenProfile)) {
    return std::nullopt;
  }
  if (!local_state) {
    return std::nullopt;
  }
  if (local_state->GetInteger(prefs::kExternalOpenMode) !=
      kModeDesignatedProfile) {
    return std::nullopt;
  }
  // An explicit --profile-directory pin wins over the pref.
  if (command_line.HasSwitch(kProfileDirectorySwitch)) {
    return std::nullopt;
  }
  const std::string base_name =
      local_state->GetString(prefs::kExternalOpenProfile);
  if (base_name.empty()) {
    return std::nullopt;
  }
  // Guest/System are never designatable (D4).
  if (base_name == kGuestProfileDirName || base_name == kSystemProfileDirName) {
    return std::nullopt;
  }
  const base::FilePath path = user_data_dir.Append(base_name);
  // Deleted/unregistered profile: silent fallback to current behavior.
  if (!profile_exists(path)) {
    return std::nullopt;
  }
  return path;
}

}  // namespace roamux
