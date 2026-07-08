// SPDX-License-Identifier: Apache-2.0
#ifndef ROAMEX_BROWSER_UI_WEBUI_ROAMEX_SHORTCUTS_HANDLER_H_
#define ROAMEX_BROWSER_UI_WEBUI_ROAMEX_SHORTCUTS_HANDLER_H_

#include <string>

#include "base/values.h"
#include "chrome/browser/ui/webui/settings/settings_page_ui_handler.h"

namespace roamex {

// The §4.3 shared shortcut surface's message handler (roam-13): lists the
// ENABLED registry entries with their current chords; validates + persists
// rebinds. All validation is C++-side; the page only records chords.
// (Registered at the settings handler registration point via patch 0011 —
// the §12.2-declared additive roamex WebUI handler.)
class RoamexShortcutsHandler : public settings::SettingsPageUIHandler {
 public:
  RoamexShortcutsHandler();
  RoamexShortcutsHandler(const RoamexShortcutsHandler&) = delete;
  RoamexShortcutsHandler& operator=(const RoamexShortcutsHandler&) = delete;
  ~RoamexShortcutsHandler() override;

  // SettingsPageUIHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override {}
  void OnJavascriptDisallowed() override {}

  // Testing seams (drive the message paths directly).
  base::ListValue GetShortcutListForTesting();
  std::string RebindForTesting(const std::string& pref_key,
                               const base::DictValue& chord_dict);

 private:
  void HandleGetShortcuts(const base::ListValue& args);
  void HandleRebindShortcut(const base::ListValue& args);

  base::ListValue BuildShortcutList();
  // Returns "" on success, else the rejection reason
  // ("invalid" | "reserved" | "roamex").
  std::string Rebind(const std::string& pref_key,
                     const base::DictValue& chord_dict);
};

}  // namespace roamex

#endif  // ROAMEX_BROWSER_UI_WEBUI_ROAMEX_SHORTCUTS_HANDLER_H_
