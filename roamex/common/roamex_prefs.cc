// SPDX-License-Identifier: Apache-2.0
#include "roamex/common/roamex_prefs.h"

#include "components/prefs/pref_registry_simple.h"

namespace roamex::prefs {

void RegisterProfilePrefs(PrefRegistrySimple* registry) {
  registry->RegisterIntegerPref(kTabStripPosition, 0);            // 0 = top (Chromium default)
  registry->RegisterBooleanPref(kReopenClosed, false);           // default: skip closed tabs
  registry->RegisterBooleanPref(kSigninOptionalEntryPoint, false);  // default: sign-in surfaces suppressed
  registry->RegisterDictionaryPref(kShortcutBindings);  // §4.3: user rebinds; empty = defaults
}

}  // namespace roamex::prefs
