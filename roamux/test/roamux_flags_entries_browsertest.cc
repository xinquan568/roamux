// SPDX-License-Identifier: Apache-2.0
// roam-340: every Roamux chrome://flags row is present, rendered and usable.
//
// The rows live in ONE patch (0073-roamux-flags-entries.patch) since roam-340;
// this test is the runtime half of that refactor's acceptance and the durable
// guard for every future row: a silently dropped, duplicated or mis-typed row
// is invisible to lint, governance and the unit suites (AboutFlagsTest covers
// only the metadata files and is not run by any Roamux CI job), but it shows
// here, on tier-2, every PR.
//
// Three checks, in the order the page itself works:
//   1. the model — about_flags::GetFlagFeatureEntries over the browser's
//      local-state flags storage is exactly the list chrome://flags renders
//      from: every expected row is supported, named, and no other roamux-* row
//      exists;
//   2. the page — chrome://flags renders each row as a <flags-experiment> with
//      the select the entry type calls for (experiment-select with
//      Default/Enabled/Disabled for FEATURE_VALUE_TYPE rows,
//      experiment-enable-disable for the SINGLE_VALUE_TYPE switch mirror),
//      enabled and on its default;
//   3. usable — one feature row and the switch row are changed THROUGH the
//      page's selects (their change handlers call the flags browser proxy) and
//      the model reflects it; then the page's own "Reset all" button restores
//      both, in the DOM and in the model.

#include <map>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/strings/string_util.h"
#include "base/test/run_until.h"
#include "base/values.h"
#include "chrome/browser/about_flags.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "components/webui/flags/flags_state.h"
#include "components/webui/flags/pref_service_flags_storage.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace {

// Ten FEATURE_VALUE_TYPE kill-switches + the SINGLE_VALUE_TYPE switch mirror.
// kRoamuxSchemeAlias deliberately has no row. Keep in sync with patch 0073.
constexpr const char* kExpectedRows[] = {
    "roamux-bookmark-subfolder-groups",
    "roamux-brave-style-profiles",
    "roamux-edge-import",
    "roamux-external-open-profile",
    "roamux-initial-url",
    "roamux-new-tab-position",
    "roamux-refresh-all-initial-urls",
    "roamux-signin-opt-in",
    "roamux-tab-strip-position",
    "roamux-tab-strip-toggle-shortcut",
    "roamux-tab-visit-nav",
};
constexpr char kSwitchRow[] = "roamux-signin-opt-in";
constexpr char kToggledFeatureRow[] = "roamux-new-tab-position";
constexpr char kRoamuxPrefix[] = "roamux-";

// Collects every visible roamux-* <flags-experiment> on chrome://flags after
// the page reports its feature data ready: id, select kind, disabled,
// selectedIndex, value.
constexpr char kCollectRowsJs[] = R"JS(
  (async () => {
    const app = document.querySelector('flags-app');
    await app.experimentalFeaturesReadyForTesting();
    const rows = app.shadowRoot.querySelectorAll(
        '#tab-content-available flags-experiment');
    const out = [];
    for (const row of rows) {
      const div = row.shadowRoot.querySelector('.experiment');
      if (!div || !div.id.startsWith('roamux-')) continue;
      const sel = row.getSelect();
      out.push({
        id: div.id,
        kind: sel ? sel.className : null,
        disabled: sel ? sel.disabled : true,
        selectedIndex: sel ? sel.selectedIndex : -1,
        value: sel ? sel.value : null,
      });
    }
    return JSON.stringify(out);
  })()
)JS";

// Changes the two rows through their selects (the elements' change handlers
// call the flags browser proxy — the same path a user's click takes).
constexpr char kChangeThroughUiJs[] = R"JS(
  (async () => {
    const app = document.querySelector('flags-app');
    await app.experimentalFeaturesReadyForTesting();
    const rows = [...app.shadowRoot.querySelectorAll(
        '#tab-content-available flags-experiment')];
    const find = (id) => rows.find(
        (r) => r.shadowRoot.querySelector('.experiment')?.id === id);
    const feature = find('roamux-new-tab-position').getSelect();
    feature.selectedIndex = 2;  // Default / Enabled / Disabled
    feature.dispatchEvent(new Event('change'));
    const sw = find('roamux-signin-opt-in').getSelect();
    sw.value = 'enabled';
    sw.dispatchEvent(new Event('change'));
    return 'changed';
  })()
)JS";

// Clicks the page's own "Reset all" button; its handler calls the proxy,
// reloads the feature data and re-renders, so the follow-up read waits on
// experimentalFeaturesReadyForTesting() again.
constexpr char kResetThroughButtonJs[] = R"JS(
  (async () => {
    const app = document.querySelector('flags-app');
    await app.experimentalFeaturesReadyForTesting();
    app.shadowRoot.querySelector('#experiment-reset-all').click();
    return 'clicked';
  })()
)JS";

struct ModelRow {
  bool supported = false;
  bool is_default = true;
  bool enabled = false;
  bool has_enabled = false;
  std::string name;
};

class RoamuxFlagsEntriesTest : public roamux::test::RoamuxBrowserTest {
 protected:
  // The list chrome://flags renders from, filtered to roamux-* rows.
  std::map<std::string, ModelRow> RoamuxModelRows() {
    flags_ui::PrefServiceFlagsStorage storage(g_browser_process->local_state());
    base::ListValue supported;
    base::ListValue unsupported;
    about_flags::GetFlagFeatureEntries(&storage,
                                       flags_ui::kGeneralAccessFlagsOnly,
                                       supported, unsupported);
    std::map<std::string, ModelRow> rows;
    auto collect = [&](const base::ListValue& list, bool is_supported) {
      for (const base::Value& value : list) {
        const base::DictValue& dict = value.GetDict();
        const std::string* internal_name = dict.FindString("internal_name");
        if (!internal_name || !base::StartsWith(*internal_name, kRoamuxPrefix)) {
          continue;
        }
        ModelRow row;
        row.supported = is_supported;
        row.is_default = dict.FindBool("is_default").value_or(true);
        if (std::optional<bool> enabled = dict.FindBool("enabled")) {
          row.has_enabled = true;
          row.enabled = *enabled;
        }
        if (const std::string* name = dict.FindString("name")) {
          row.name = *name;
        }
        rows[*internal_name] = row;
      }
    };
    collect(supported, true);
    collect(unsupported, false);
    return rows;
  }

  content::WebContents* contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  base::ListValue PageRows() {
    std::string json = content::EvalJs(contents(), kCollectRowsJs).ExtractString();
    std::optional<base::Value> parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
    EXPECT_TRUE(parsed && parsed->is_list()) << json;
    return parsed && parsed->is_list() ? std::move(*parsed).TakeList()
                                       : base::ListValue();
  }

  static const base::DictValue* FindRow(const base::ListValue& rows,
                                          const std::string& id) {
    for (const base::Value& row : rows) {
      if (const std::string* rid = row.GetDict().FindString("id");
          rid && *rid == id) {
        return &row.GetDict();
      }
    }
    return nullptr;
  }
};

IN_PROC_BROWSER_TEST_F(RoamuxFlagsEntriesTest, EveryRowIsInTheModel) {
  std::map<std::string, ModelRow> rows = RoamuxModelRows();
  std::vector<std::string> names;
  for (const auto& [name, row] : rows) {
    names.push_back(name);
  }
  std::vector<std::string> expected(std::begin(kExpectedRows),
                                    std::end(kExpectedRows));
  EXPECT_EQ(names, expected) << "the roamux-* rows chrome://flags renders from";
  for (const char* name : kExpectedRows) {
    const ModelRow& row = rows[name];
    EXPECT_TRUE(row.supported) << name << " must be supported on this platform";
    EXPECT_TRUE(row.is_default) << name << " must start on its default";
    EXPECT_FALSE(row.name.empty()) << name << " must have a visible name";
  }
  EXPECT_TRUE(rows[kSwitchRow].has_enabled) << "the switch mirror carries `enabled`";
  EXPECT_FALSE(rows[kToggledFeatureRow].has_enabled)
      << "feature rows carry no `enabled` (state is the selected option)";
}

IN_PROC_BROWSER_TEST_F(RoamuxFlagsEntriesTest, EveryRowRendersWithItsSelect) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://flags")));
  base::ListValue rows = PageRows();
  ASSERT_EQ(rows.size(), std::size(kExpectedRows)) << rows;
  for (const char* name : kExpectedRows) {
    const base::DictValue* row = FindRow(rows, name);
    ASSERT_TRUE(row) << name << " not rendered";
    const std::string* kind = row->FindString("kind");
    ASSERT_TRUE(kind) << name << " has no select";
    EXPECT_EQ(*kind, std::string(name) == kSwitchRow ? "experiment-enable-disable"
                                                     : "experiment-select")
        << name;
    EXPECT_FALSE(row->FindBool("disabled").value_or(true)) << name;
    if (std::string(name) == kSwitchRow) {
      EXPECT_EQ(*row->FindString("value"), "disabled") << name;
    } else {
      EXPECT_EQ(row->FindInt("selectedIndex").value_or(-1), 0)
          << name << " must start on Default";
    }
  }
}

IN_PROC_BROWSER_TEST_F(RoamuxFlagsEntriesTest, RowsAreUsableAndResetAllRestoresThem) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL("chrome://flags")));
  ASSERT_EQ(content::EvalJs(contents(), kChangeThroughUiJs), "changed");
  // The change handlers post to the browser; wait for the model to reflect it.
  ASSERT_TRUE(base::test::RunUntil([&] {
    std::map<std::string, ModelRow> rows = RoamuxModelRows();
    return !rows[kToggledFeatureRow].is_default && rows[kSwitchRow].enabled;
  }));
  {
    base::ListValue rows = PageRows();
    EXPECT_EQ(FindRow(rows, kToggledFeatureRow)->FindInt("selectedIndex"), 2);
    EXPECT_EQ(*FindRow(rows, kSwitchRow)->FindString("value"), "enabled");
  }

  ASSERT_EQ(content::EvalJs(contents(), kResetThroughButtonJs), "clicked");
  ASSERT_TRUE(base::test::RunUntil([&] {
    std::map<std::string, ModelRow> rows = RoamuxModelRows();
    return rows[kToggledFeatureRow].is_default && rows[kSwitchRow].is_default &&
           !rows[kSwitchRow].enabled;
  }));
  base::ListValue rows = PageRows();
  EXPECT_EQ(FindRow(rows, kToggledFeatureRow)->FindInt("selectedIndex"), 0)
      << "Reset all must put the feature row back on Default";
  EXPECT_EQ(*FindRow(rows, kSwitchRow)->FindString("value"), "disabled")
      << "Reset all must put the switch row back to disabled";
}

}  // namespace
