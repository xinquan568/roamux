// SPDX-License-Identifier: Apache-2.0
// roam-277: the Settings > Appearance "New tab position" dropdown (patch 0070)
// — gated on roamux::features::kNewTabPosition ALONE (independent of
// kTabStripPosition), live-bound in both directions to
// roamux.tabs.new_tab_position, with PRESENTATION-AWARE copy for the
// after-active-tab option: "Below" on a vertical strip, "to the right" / "to
// the left" on a horizontal strip in LTR / RTL. The orientation signal is the
// TS mirror of roamux::IsVerticalTabStripEffectivelyEnabled — the roamux
// placement when kTabStripPosition is on, upstream vertical_tabs.enabled when
// it is off — and both are pref bindings, so the label recomputes live.
// Written RED before patch 0070 (TDD/P6).

#include <string>

#include "base/i18n/base_i18n_switches.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "base/test/scoped_feature_list.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "roamux/common/roamux_features.h"
#include "roamux/common/roamux_prefs.h"
#include "roamux/test/support/roamux_browser_test.h"

namespace roamux {
namespace {

constexpr char kRowId[] = "roamuxNewTabPosition";

// Recursively searches open shadow roots for an element id, polling until it
// appears (Polymer stamps dom-if templates asynchronously). Resolves to true
// iff found within the deadline.
constexpr char kWaitForIdScript[] = R"(
    (async () => {
      const deepQuery = (root, id) => {
        const direct = root.querySelector('#' + id);
        if (direct) return direct;
        for (const el of root.querySelectorAll('*')) {
          if (el.shadowRoot) {
            const hit = deepQuery(el.shadowRoot, id);
            if (hit) return hit;
          }
        }
        return null;
      };
      for (let i = 0; i < %d; i++) {
        if (deepQuery(document, '%s')) return true;
        await new Promise(r => setTimeout(r, 100));
      }
      return false;
    })();
)";

std::string WaitForIdScript(const std::string& id, int attempts) {
  return base::StringPrintf(kWaitForIdScript, attempts, id.c_str());
}

// Runs `body` with `el` bound to the deep-queried #roamuxNewTabPosition
// element (assumes it exists) and `select` to its <select>.
std::string WithRowScript(const std::string& body) {
  return base::StringPrintf(R"(
      (async () => {
        const deepQuery = (root, id) => {
          const direct = root.querySelector('#' + id);
          if (direct) return direct;
          for (const el of root.querySelectorAll('*')) {
            if (el.shadowRoot) {
              const hit = deepQuery(el.shadowRoot, id);
              if (hit) return hit;
            }
          }
          return null;
        };
        const el = deepQuery(document, '%s');
        if (!el) return 'missing';
        const select = el.shadowRoot.querySelector('select');
        if (!select) return 'no-select';
        %s
      })();
  )",
                            kRowId, body.c_str());
}

// Polls (bounded) until option `value`'s text contains `needle`; resolves to
// the option's text either way so a failure prints what was rendered.
std::string WaitForOptionTextScript(int value, const std::string& needle) {
  return WithRowScript(base::StringPrintf(R"(
        for (let i = 0; i < 100; i++) {
          const opt = [...select.options].find(o => o.value === '%d');
          if (opt && opt.textContent.includes('%s')) return opt.textContent;
          await new Promise(r => setTimeout(r, 100));
        }
        const opt = [...select.options].find(o => o.value === '%d');
        return opt ? opt.textContent : 'no-option';
  )",
                                          value, needle.c_str(), value));
}

class NewTabPositionSettingsTestBase : public test::RoamuxBrowserTest {
 protected:
  NewTabPositionSettingsTestBase(bool new_tab_position_on,
                                 bool tab_strip_position_on) {
    std::vector<base::test::FeatureRef> enabled;
    std::vector<base::test::FeatureRef> disabled;
    (new_tab_position_on ? enabled : disabled)
        .push_back(features::kNewTabPosition);
    (tab_strip_position_on ? enabled : disabled)
        .push_back(features::kTabStripPosition);
    features_.InitWithFeatures(enabled, disabled);
  }

  PrefService* prefs() { return browser()->profile()->GetPrefs(); }

  content::WebContents* NavigateToAppearance() {
    EXPECT_TRUE(ui_test_utils::NavigateToURL(
        browser(), GURL("chrome://settings/appearance")));
    content::WebContents* web_contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    // The page is interactive once a stock Appearance anchor is stamped.
    EXPECT_EQ(true,
              content::EvalJs(web_contents, WaitForIdScript("themeRow", 100)));
    return web_contents;
  }

  void ExpectRowPresent(content::WebContents* web_contents) {
    EXPECT_EQ(true,
              content::EvalJs(web_contents, WaitForIdScript(kRowId, 100)));
  }

  void ExpectOptionText(content::WebContents* web_contents,
                        int value,
                        const std::string& needle) {
    const std::string text =
        content::EvalJs(web_contents, WaitForOptionTextScript(value, needle))
            .ExtractString();
    EXPECT_NE(std::string::npos, text.find(needle))
        << "option " << value << " rendered as: " << text;
  }

 private:
  base::test::ScopedFeatureList features_;
};

class RoamuxNewTabPositionSettingsTest : public NewTabPositionSettingsTestBase {
 public:
  RoamuxNewTabPositionSettingsTest()
      : NewTabPositionSettingsTestBase(/*new_tab_position_on=*/true,
                                       /*tab_strip_position_on=*/true) {}
};

class RoamuxNewTabPositionSettingsFlagOffTest
    : public NewTabPositionSettingsTestBase {
 public:
  RoamuxNewTabPositionSettingsFlagOffTest()
      : NewTabPositionSettingsTestBase(/*new_tab_position_on=*/false,
                                       /*tab_strip_position_on=*/true) {}
};

class RoamuxNewTabPositionSettingsTabStripPositionOffTest
    : public NewTabPositionSettingsTestBase {
 public:
  RoamuxNewTabPositionSettingsTabStripPositionOffTest()
      : NewTabPositionSettingsTestBase(/*new_tab_position_on=*/true,
                                       /*tab_strip_position_on=*/false) {}
};

// roam-9 precedent: force the UI direction before the WebUI data source
// exists, so loadTimeData's textdirection is "rtl".
class RoamuxNewTabPositionSettingsRtlTest
    : public NewTabPositionSettingsTestBase {
 public:
  RoamuxNewTabPositionSettingsRtlTest()
      : NewTabPositionSettingsTestBase(/*new_tab_position_on=*/true,
                                       /*tab_strip_position_on=*/true) {}

 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    NewTabPositionSettingsTestBase::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(::switches::kForceUIDirection,
                                    ::switches::kForceDirectionRTL);
  }
};

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionSettingsTest,
                       RowPresentAndLiveBoundWhenFlagOn) {
  content::WebContents* web_contents = NavigateToAppearance();
  ExpectRowPresent(web_contents);

  // JS -> pref: pick after_active_tab (2).
  EXPECT_EQ("ok", content::EvalJs(web_contents, WithRowScript(R"(
        select.value = '2';
        select.dispatchEvent(new Event('change'));
        return 'ok';
  )")));
  // settings_private writes are async over the extension API; poll (bounded).
  for (int i = 0; i < 1000 && prefs()->GetInteger(prefs::kNewTabPosition) != 2;
       ++i) {
    base::RunLoop().RunUntilIdle();
  }
  EXPECT_EQ(2, prefs()->GetInteger(prefs::kNewTabPosition));

  // Pref -> JS: a C++ write must reflect back into the dropdown live.
  prefs()->SetInteger(prefs::kNewTabPosition, 0);
  EXPECT_EQ("0", content::EvalJs(web_contents, WithRowScript(R"(
        for (let i = 0; i < 100; i++) {
          if (select.value === '0') return select.value;
          await new Promise(r => setTimeout(r, 100));
        }
        return select.value;
  )")));
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionSettingsFlagOffTest,
                       RowAbsentWhenFlagOff) {
  content::WebContents* web_contents = NavigateToAppearance();
  // The stock anchor is up; the roamux row was never stamped (single settle).
  EXPECT_EQ(false, content::EvalJs(web_contents, WaitForIdScript(kRowId, 5)));
}

// The gate is kNewTabPosition alone: kTabStripPosition OFF must not hide it.
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionSettingsTabStripPositionOffTest,
                       RowPresentWithTabStripPositionOff) {
  content::WebContents* web_contents = NavigateToAppearance();
  ExpectRowPresent(web_contents);
}

// Roamux placement is the orientation authority (kTabStripPosition ON):
// Top -> horizontal -> "right"; a live C++ write to Left -> "Below".
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionSettingsTest,
                       LabelsFollowRoamuxPlacementLive) {
  prefs()->SetInteger(prefs::kTabStripPosition, 0);  // top
  content::WebContents* web_contents = NavigateToAppearance();
  ExpectRowPresent(web_contents);
  ExpectOptionText(web_contents, 2, "right");
  prefs()->SetInteger(prefs::kTabStripPosition, 2);  // left (vertical)
  ExpectOptionText(web_contents, 2, "Below");
  ExpectOptionText(web_contents, 0, "end of the tab strip");
  ExpectOptionText(web_contents, 1, "group");
}

// Upstream vertical_tabs.enabled is the authority when kTabStripPosition is
// OFF: false -> "right"; a live C++ write to true -> "Below".
IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionSettingsTabStripPositionOffTest,
                       LabelsFollowUpstreamVerticalTabsPref) {
  prefs()->SetBoolean(prefs::kUpstreamVerticalTabsEnabled, false);
  content::WebContents* web_contents = NavigateToAppearance();
  ExpectRowPresent(web_contents);
  ExpectOptionText(web_contents, 2, "right");
  prefs()->SetBoolean(prefs::kUpstreamVerticalTabsEnabled, true);
  ExpectOptionText(web_contents, 2, "Below");
}

IN_PROC_BROWSER_TEST_F(RoamuxNewTabPositionSettingsRtlTest, LabelsAreRtlAware) {
  prefs()->SetInteger(prefs::kTabStripPosition, 0);  // top (horizontal)
  content::WebContents* web_contents = NavigateToAppearance();
  ExpectRowPresent(web_contents);
  ExpectOptionText(web_contents, 2, "left");
}

}  // namespace
}  // namespace roamux
