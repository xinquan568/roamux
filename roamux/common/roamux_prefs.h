// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMUX_COMMON_ROAMUX_PREFS_H_
#define ROAMUX_COMMON_ROAMUX_PREFS_H_

class PrefRegistrySimple;
class PrefService;

// Local Roamux prefs (plan §7.2 — persisted locally, no sync).
// `RegisterProfilePrefs` is invoked from Chromium's profile-pref registration
// via a minimal patch hook (roam-3 / §12.2 inventory).
namespace roamux::prefs {

inline constexpr char kTabStripPosition[] =
    "roamux.tabs.strip_position";  // E1 (enum: top/bottom/left/right)
inline constexpr char kReopenClosed[] =
    "roamux.tabs.visit_nav.reopen_closed";  // E4 (Q(new)-C, default off)
inline constexpr char kSigninOptionalEntryPoint[] =
    "roamux.signin.optional_entry_point_enabled";  // E5 (Q(i3)-C, default off)
inline constexpr char kShortcutBindings[] =
    "roamux.shortcuts";  // E2/E4 (§4.3 rebinds; dict command-key -> chord)

void RegisterProfilePrefs(PrefRegistrySimple* registry);

// roam-182: startup normalization of the upstream vertical-tabs pref onto the
// roamux placement (sole-authority contract; maintainer-authorized to WRITE
// the upstream pref, 2026-07-20). Invoked from upstream
// MigrateObsoleteProfilePrefs via patch 0004. With the
// roamux::features::kTabStripPosition flag on and an unmanaged, explicitly-true
// "vertical_tabs.enabled": keeps a stored Left/Right placement (else sets
// Left), then clears the upstream pref. No-op in every other state, so a later
// Top/Bottom choice never re-migrates. Flag off: strict no-op.
void MigrateProfilePrefs(PrefService* pref_service);

}  // namespace roamux::prefs

#endif  // ROAMUX_COMMON_ROAMUX_PREFS_H_
