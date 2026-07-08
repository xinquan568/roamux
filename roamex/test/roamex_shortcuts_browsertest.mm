// SPDX-License-Identifier: Apache-2.0
// roam-13 (I-2.4): rebind persists; conflicting chords are rejected (both
// classes); dispatch honors the default AND the override through the real
// CommandForKeyEvent path (closing the roam-12 gap); flag-off is inert.

#import <Carbon/Carbon.h>
#import <Cocoa/Cocoa.h>

#include "base/test/scoped_feature_list.h"
#include "base/values.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/global_keyboard_shortcuts_mac.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/test_web_ui.h"
#include "roamex/browser/tabs/shortcut_registry.h"
#include "roamex/browser/ui/webui/roamex_shortcuts_handler.h"
#include "roamex/common/roamex_features.h"
#include "roamex/common/roamex_prefs.h"
#include "ui/events/test/cocoa_test_event_utils.h"

namespace roamex {
namespace {

NSEvent* KeyEventForChord(const tabs::Chord& chord) {
  NSUInteger modifiers = 0;
  if (chord.cmd) {
    modifiers |= NSEventModifierFlagCommand;
  }
  if (chord.shift) {
    modifiers |= NSEventModifierFlagShift;
  }
  if (chord.ctrl) {
    modifiers |= NSEventModifierFlagControl;
  }
  if (chord.opt) {
    modifiers |= NSEventModifierFlagOption;
  }
  return cocoa_test_event_utils::KeyEventWithKeyCode(
      chord.keycode, 'r', NSEventTypeKeyDown, modifiers);
}

class ExposedHandler : public RoamexShortcutsHandler {
 public:
  using RoamexShortcutsHandler::set_web_ui;
};

class RoamexShortcutsTest : public InProcessBrowserTest {
 public:
  RoamexShortcutsTest() {
    features_.InitAndEnableFeature(features::kInitialUrl);
  }

 protected:
  PrefService* prefs() { return browser()->profile()->GetPrefs(); }

  std::string Rebind(const tabs::Chord& chord) {
    content::TestWebUI test_web_ui;
    test_web_ui.set_web_contents(
        browser()->tab_strip_model()->GetActiveWebContents());
    ExposedHandler handler;
    handler.set_web_ui(&test_web_ui);
    return handler.RebindForTesting("reload_initial_url", chord.ToDict());
  }

  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamexShortcutsTest, DefaultChordDispatches) {
  // The roam-12 gap closure: the DEFAULT Ctrl+Cmd+R resolves through the real
  // key-event path.
  const tabs::Chord default_chord =
      tabs::GetCurrentChord(prefs(), tabs::AllShortcuts()[0]);
  CommandForKeyEventResult result =
      CommandForKeyEvent(KeyEventForChord(default_chord));
  EXPECT_TRUE(result.found());
  EXPECT_EQ(IDC_RELOAD_INITIAL_URL, result.chrome_command);
}

IN_PROC_BROWSER_TEST_F(RoamexShortcutsTest, RebindPersistsAndRedispatches) {
  const tabs::Chord default_chord =
      tabs::GetCurrentChord(prefs(), tabs::AllShortcuts()[0]);
  const tabs::Chord new_chord{.cmd = true, .ctrl = true, .keycode = kVK_ANSI_Y};
  EXPECT_EQ("", Rebind(new_chord));

  // Persisted in the pref…
  const base::DictValue& bindings = prefs()->GetDict(prefs::kShortcutBindings);
  ASSERT_NE(nullptr, bindings.FindDict("reload_initial_url"));
  // …the registry answers the new chord…
  EXPECT_EQ(new_chord, tabs::GetCurrentChord(prefs(), tabs::AllShortcuts()[0]));
  // …and dispatch follows: old chord dead, new chord live.
  EXPECT_FALSE(CommandForKeyEvent(KeyEventForChord(default_chord)).found());
  CommandForKeyEventResult result =
      CommandForKeyEvent(KeyEventForChord(new_chord));
  EXPECT_TRUE(result.found());
  EXPECT_EQ(IDC_RELOAD_INITIAL_URL, result.chrome_command);
}

IN_PROC_BROWSER_TEST_F(RoamexShortcutsTest, ConflictingChordsRejected) {
  // Reserved: Cmd+R is IDC_RELOAD's chord.
  EXPECT_EQ("reserved",
            Rebind(tabs::Chord{.cmd = true, .keycode = kVK_ANSI_R}));
  // Invalid: shift-only modifier.
  EXPECT_EQ("invalid",
            Rebind(tabs::Chord{.shift = true, .keycode = kVK_ANSI_R}));
  // No pref writes happened.
  EXPECT_EQ(nullptr, prefs()
                         ->GetDict(prefs::kShortcutBindings)
                         .FindDict("reload_initial_url"));
}

class RoamexShortcutsFlagOffTest : public InProcessBrowserTest {
 public:
  RoamexShortcutsFlagOffTest() {
    features_.InitAndDisableFeature(features::kInitialUrl);
  }

 protected:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamexShortcutsFlagOffTest, DispatchInertWhenFlagOff) {
  const tabs::Chord default_chord = tabs::AllShortcuts()[0].default_chord;
  NSUInteger modifiers =
      NSEventModifierFlagCommand | NSEventModifierFlagControl;
  NSEvent* event = cocoa_test_event_utils::KeyEventWithKeyCode(
      default_chord.keycode, 'r', NSEventTypeKeyDown, modifiers);
  EXPECT_FALSE(CommandForKeyEvent(event).found());
}

}  // namespace
}  // namespace roamex
