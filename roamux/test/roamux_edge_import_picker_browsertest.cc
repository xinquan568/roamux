// SPDX-License-Identifier: Apache-2.0
// roam-288 (grill H8, decision b): the import picker's Microsoft Edge row,
// proven through the REAL path — ImporterList detection (patch 0013 reads the
// Edge root from DIR_APP_DATA, overridable here) → ImportDataHandler →
// settings-import-data-dialog. The row must offer history / favorites /
// search / autofill and NOT passwords: the browser-side secret importer has no
// production entry point (roam-299 wires the importer-host seam), so an
// advertised PASSWORDS bit was a "Saved passwords" checkbox that imported
// nothing. COOKIES has no upstream checkbox at all. Two halves: the capability
// data the handler sends (browserProfiles_) and the rendered checkbox after
// the Edge row is selected.

#include <memory>
#include <string>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/stringprintf.h"
#include "base/test/scoped_path_override.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "url/gurl.h"

namespace roamux {
namespace {

// Pierces settings-ui → settings-main → settings-people-page-index →
// settings-people-page → settings-import-data-dialog (each via shadowRoot).
constexpr char kPrelude[] = R"(
    function dialog() {
      const ui = document.querySelector('settings-ui');
      const main = ui && ui.shadowRoot &&
          ui.shadowRoot.querySelector('settings-main');
      const index = main && main.shadowRoot &&
          main.shadowRoot.querySelector('settings-people-page-index');
      const people = index && index.shadowRoot &&
          index.shadowRoot.querySelector('settings-people-page');
      return people && people.shadowRoot ?
          people.shadowRoot.querySelector('settings-import-data-dialog') : null;
    }
    function inDialog(sel) {
      const d = dialog();
      return d && d.shadowRoot ? d.shadowRoot.querySelector(sel) : null;
    }
    function edgeEntry() {
      const d = dialog();
      if (!d || !Array.isArray(d.browserProfiles_)) return null;
      return d.browserProfiles_.find(p => p.name === 'Microsoft Edge') || null;
    }
)";

std::string Poll(const std::string& condition) {
  return base::StringPrintf(
      R"((function() { %s
           return new Promise(resolve => {
             const t0 = Date.now();
             (function poll() {
               let ok = false;
               try { ok = !!(%s); } catch (e) {}
               if (ok) return resolve(true);
               if (Date.now() - t0 > 8000) return resolve(false);
               setTimeout(poll, 50);
             })();
           });
         })())",
      kPrelude, condition.c_str());
}

class RoamuxEdgeImportPickerBrowserTest
    : public roamux::test::RoamuxBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    roamux::test::RoamuxBrowserTest::SetUpOnMainThread();
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(app_data_.CreateUniqueTempDir());
    // The roam-202 minimal detectable profile (the same artefact
    // EdgeDetectionTest uses): <root>/Microsoft Edge/Default/Bookmarks.
    const base::FilePath edge_default =
        app_data_.GetPath()
            .Append(FILE_PATH_LITERAL("Microsoft Edge"))
            .Append(FILE_PATH_LITERAL("Default"));
    ASSERT_TRUE(base::CreateDirectory(edge_default));
    ASSERT_TRUE(
        base::WriteFile(edge_default.Append(FILE_PATH_LITERAL("Bookmarks")), "{}"));
    // Before navigating: the handler builds a fresh ImporterList per dialog
    // open, and patch 0013's detector reads DIR_APP_DATA at that moment.
    path_override_ = std::make_unique<base::ScopedPathOverride>(
        base::DIR_APP_DATA, app_data_.GetPath());
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), GURL("chrome://settings/importData")));
    web_contents_ = browser()->tab_strip_model()->GetActiveWebContents();
    ASSERT_NE(nullptr, web_contents_);
  }

  [[nodiscard]] bool Reached(const std::string& condition) {
    return content::EvalJs(web_contents_, Poll(condition)).ExtractBool();
  }

  content::EvalJsResult Eval(const std::string& expression) {
    return content::EvalJs(web_contents_, std::string(kPrelude) + expression);
  }

  base::ScopedTempDir app_data_;
  std::unique_ptr<base::ScopedPathOverride> path_override_;
  raw_ptr<content::WebContents, DanglingUntriaged> web_contents_ = nullptr;
};

IN_PROC_BROWSER_TEST_F(RoamuxEdgeImportPickerBrowserTest,
                       EdgeRowOffersNoPasswords) {
  // 1. Detection readiness: the dialog has its browser profiles.
  ASSERT_TRUE(Reached("dialog() && Array.isArray(dialog().browserProfiles_) &&"
                      " dialog().browserProfiles_.length > 0"));

  // 2. Capability data — by exact name; other detected sources and the
  //    "Bookmarks HTML file" entry may be present in any order.
  ASSERT_NE("null", Eval("JSON.stringify(edgeEntry())").ExtractString())
      << "the Edge row was not detected under the DIR_APP_DATA override";
  ASSERT_EQ(false, Eval("edgeEntry().passwords"))
      << "PASSWORDS is advertised — the dead checkbox is back (roam-288)";
  ASSERT_EQ(true, Eval("edgeEntry().history"));
  ASSERT_EQ(true, Eval("edgeEntry().favorites"));
  ASSERT_EQ(true, Eval("edgeEntry().search"));
  ASSERT_EQ(true, Eval("edgeEntry().autofillFormData"));

  // 3. Rendered visibility — select the Edge row (the option whose value is
  //    the entry's index), then wait for Polymer to re-render selected_.
  ASSERT_TRUE(content::ExecJs(
      web_contents_, std::string(kPrelude) + R"(
        const sel = inDialog('#browserSelect');
        const idx = Array.from(sel.options)
            .findIndex(o => Number(o.value) === edgeEntry().index);
        if (idx < 0) throw new Error('no option for the Edge entry');
        sel.selectedIndex = idx;
        sel.dispatchEvent(new CustomEvent('change'));
      )"));
  // The checkbox must EXIST and be hidden (a missing element never reads as
  // hidden here); history stays offered.
  EXPECT_TRUE(Reached("inDialog('#importDialogSavedPasswords') &&"
                      " inDialog('#importDialogSavedPasswords').hidden === true"));
  EXPECT_TRUE(Reached("inDialog('#importDialogHistory') &&"
                      " !inDialog('#importDialogHistory').hidden"));
}

}  // namespace
}  // namespace roamux
