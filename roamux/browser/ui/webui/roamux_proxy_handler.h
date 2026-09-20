// SPDX-License-Identifier: Apache-2.0
// roam-323: the browser-side seam for the Settings > System per-profile proxy
// section. All validation is C++-side (roamux/browser/proxy) and every mutation
// is re-checked here, so the page can only submit raw fields — it can neither
// bypass a guard nor write a partial dictionary. Registered at the settings
// handler registration point by patch 0076, the §12.2-declared additive roamux
// WebUI handler (the roam-13 shortcuts-handler precedent).

#ifndef ROAMUX_BROWSER_UI_WEBUI_ROAMUX_PROXY_HANDLER_H_
#define ROAMUX_BROWSER_UI_WEBUI_ROAMUX_PROXY_HANDLER_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/settings/settings_page_ui_handler.h"
#include "components/prefs/pref_change_registrar.h"

class Profile;

namespace roamux {

class RoamuxProxyHandler : public settings::SettingsPageUIHandler {
 public:
  explicit RoamuxProxyHandler(Profile* profile);
  RoamuxProxyHandler(const RoamuxProxyHandler&) = delete;
  RoamuxProxyHandler& operator=(const RoamuxProxyHandler&) = delete;
  ~RoamuxProxyHandler() override;

  // SettingsPageUIHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

  // Testing seams — they call the same guarded implementations as the message
  // paths.
  base::DictValue GetStateForTesting();
  base::DictValue SetForTesting(const base::DictValue& fields);
  base::DictValue ResetForTesting();

 private:
  void HandleGetState(const base::ListValue& args);
  void HandleSet(const base::ListValue& args);
  void HandleReset(const base::ListValue& args);

  // The state the page renders: the stored configuration plus who actually
  // controls routing.
  base::DictValue BuildState();
  // Returns an empty dict on success, else {field, reason}.
  base::DictValue Commit(const base::DictValue& fields);
  base::DictValue ResetToSystem();
  void OnProxyPrefsChanged();

  const raw_ptr<Profile> profile_;
  PrefChangeRegistrar pref_change_registrar_;
};

}  // namespace roamux

#endif  // ROAMUX_BROWSER_UI_WEBUI_ROAMUX_PROXY_HANDLER_H_
