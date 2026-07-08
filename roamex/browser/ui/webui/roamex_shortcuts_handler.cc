// SPDX-License-Identifier: Apache-2.0
#include "roamex/browser/ui/webui/roamex_shortcuts_handler.h"

#include <string>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_ui.h"
#include "roamex/browser/tabs/shortcut_registry.h"
#include "roamex/browser/tabs/shortcut_registry_mac.h"

namespace roamex {

namespace {

std::string ChordDisplayString(const tabs::Chord& chord) {
  std::string text;
  if (chord.ctrl) {
    text += "\xE2\x8C\x83";  // ⌃
  }
  if (chord.opt) {
    text += "\xE2\x8C\xA5";  // ⌥
  }
  if (chord.shift) {
    text += "\xE2\x87\xA7";  // ⇧
  }
  if (chord.cmd) {
    text += "\xE2\x8C\x98";  // ⌘
  }
  text += "[" + base::NumberToString(chord.keycode) + "]";
  return text;
}

}  // namespace

RoamexShortcutsHandler::RoamexShortcutsHandler() = default;
RoamexShortcutsHandler::~RoamexShortcutsHandler() = default;

void RoamexShortcutsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "roamexGetShortcuts",
      base::BindRepeating(&RoamexShortcutsHandler::HandleGetShortcuts,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "roamexRebindShortcut",
      base::BindRepeating(&RoamexShortcutsHandler::HandleRebindShortcut,
                          base::Unretained(this)));
}

base::ListValue RoamexShortcutsHandler::GetShortcutListForTesting() {
  return BuildShortcutList();
}

std::string RoamexShortcutsHandler::RebindForTesting(
    const std::string& pref_key,
    const base::DictValue& chord_dict) {
  return Rebind(pref_key, chord_dict);
}

void RoamexShortcutsHandler::HandleGetShortcuts(const base::ListValue& args) {
  AllowJavascript();
  const base::Value& callback_id = args[0];
  ResolveJavascriptCallback(callback_id, BuildShortcutList());
}

void RoamexShortcutsHandler::HandleRebindShortcut(const base::ListValue& args) {
  AllowJavascript();
  const base::Value& callback_id = args[0];
  const std::string* pref_key = args[1].GetIfString();
  const base::DictValue* chord_dict = args[2].GetIfDict();
  std::string error = "invalid";
  if (pref_key && chord_dict) {
    error = Rebind(*pref_key, *chord_dict);
  }
  ResolveJavascriptCallback(callback_id, base::Value(error));
}

base::ListValue RoamexShortcutsHandler::BuildShortcutList() {
  Profile* profile = Profile::FromWebUI(web_ui());
  base::ListValue list;
  for (const tabs::RoamexShortcut* entry :
       tabs::EnabledShortcuts(tabs::AllShortcuts())) {
    const tabs::Chord chord =
        tabs::GetCurrentChord(profile->GetPrefs(), *entry);
    base::DictValue item;
    item.Set("key", entry->pref_key);
    item.Set("label", entry->label);
    item.Set("chordText", ChordDisplayString(chord));
    list.Append(std::move(item));
  }
  return list;
}

std::string RoamexShortcutsHandler::Rebind(const std::string& pref_key,
                                           const base::DictValue& chord_dict) {
  Profile* profile = Profile::FromWebUI(web_ui());
  std::optional<tabs::Chord> chord = tabs::Chord::FromDict(chord_dict);
  if (!chord) {
    return "invalid";
  }
  for (const tabs::RoamexShortcut* entry :
       tabs::EnabledShortcuts(tabs::AllShortcuts())) {
    if (pref_key != entry->pref_key) {
      continue;
    }
    const std::vector<tabs::Chord> reserved = tabs::EnumerateReservedChords();
    switch (tabs::ValidateRebind(profile->GetPrefs(), tabs::AllShortcuts(),
                                 *entry, *chord, reserved)) {
      case tabs::RebindResult::kOk:
        tabs::StoreRebind(profile->GetPrefs(), *entry, *chord);
        return "";
      case tabs::RebindResult::kInvalid:
        return "invalid";
      case tabs::RebindResult::kConflictsReserved:
        return "reserved";
      case tabs::RebindResult::kConflictsRoamex:
        return "roamex";
    }
  }
  return "invalid";
}

}  // namespace roamex
