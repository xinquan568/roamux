// SPDX-License-Identifier: Apache-2.0
// roam-20 (I-3.6): the production import driver end-to-end — it runs the
// browser-side carriers (secrets = the roam-16 carry-forward, via the roam-19
// coordinator) for the user-selected items, imports origin storage into the
// destination profile, and reports. Selected secrets are HANDLED (reported),
// not silently skipped. Flag-gated. Models
// roamux_edge_import_coordinator_browsertest.cc.

#include "roamux/browser/importer/roamux_edge_import_driver.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/services/storage/privileged/mojom/indexed_db_control.mojom.h"
#include "components/services/storage/privileged/mojom/indexed_db_control_test.mojom.h"
#include "components/services/storage/public/mojom/local_storage_control.mojom.h"
#include "components/user_data_importer/common/importer_data_types.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/dns/mock_host_resolver.h"
#include "roamux/browser/importer/edge_import_report.h"
#include "roamux/browser/importer/edge_import_types.h"
#include "roamux/browser/importer/edge_local_storage_reader.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "roamux/test/support/storage_flush_barrier.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace roamux {
namespace {

constexpr char kReadIdbJs[] = R"(
  new Promise((resolve) => {
    const o = indexedDB.open('rx', 1);
    o.onsuccess = () => {
      const g = o.result.transaction('s', 'readonly').objectStore('s').get('k');
      g.onsuccess = () => { resolve(g.result || '<none>'); o.result.close(); };
    };
    o.onerror = () => resolve('<openfail>');
  })
)";

// roam-331: a one-shot StorageAreaObserver that resolves when a specific key
// lands in the storage service. localStorage.setItem from JS sends an
// asynchronous StorageArea::Put; ExecJs returning does NOT acknowledge it, so a
// Flush issued right after can flush nothing and the snapshot below captures an
// empty LevelDB. GetAll is [Sync] and its observer sees every event AFTER the
// returned snapshot (storage_area.mojom), so the value is either already in the
// snapshot or arrives here as KeyChanged -- one bounded wait, no polling.
class SeededKeyObserver : public blink::mojom::StorageAreaObserver {
 public:
  explicit SeededKeyObserver(std::vector<uint8_t> key) : key_(std::move(key)) {}

  mojo::PendingRemote<blink::mojom::StorageAreaObserver> BindRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  void Resolve() {
    if (!resolved_) {
      resolved_ = true;
      seen_.SetValue(true);
    }
  }
  [[nodiscard]] bool WaitForKey() { return seen_.Wait(); }

  // blink::mojom::StorageAreaObserver:
  void KeyChanged(const std::vector<uint8_t>& key,
                  const std::vector<uint8_t>& new_value,
                  const std::optional<std::vector<uint8_t>>& old_value,
                  blink::mojom::StorageAreaSourcePtr source) override {
    if (key == key_) {
      Resolve();
    }
  }
  void KeyChangeFailed(const std::vector<uint8_t>& key,
                       blink::mojom::StorageAreaSourcePtr source) override {}
  void KeyDeleted(const std::vector<uint8_t>& key,
                  const std::optional<std::vector<uint8_t>>& old_value,
                  blink::mojom::StorageAreaSourcePtr source) override {}
  void AllDeleted(bool was_nonempty,
                  blink::mojom::StorageAreaSourcePtr source) override {}
  void ShouldSendOldValueOnMutations(bool value) override {}

 private:
  const std::vector<uint8_t> key_;
  bool resolved_ = false;
  base::test::TestFuture<bool> seen_;
  mojo::Receiver<blink::mojom::StorageAreaObserver> receiver_{this};
};

// roam-331: Blink stores Latin1-only localStorage keys and values with a
// one-byte format prefix (StorageFormat::Latin1 -- see
// blink/renderer/modules/storage/cached_storage_area.cc), so a JS-seeded entry
// is NOT the bare script text on disk. Do not "simplify" these to plain
// strings: the comparison would then never match.
std::vector<uint8_t> Latin1Encoded(std::string_view text) {
  std::vector<uint8_t> out;
  out.reserve(text.size() + 1);
  out.push_back(0x01);  // StorageFormat::Latin1
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

class RoamuxEdgeImportDriverTestBase : public roamux::test::RoamuxBrowserTest {
 public:
  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    ASSERT_TRUE(embedded_test_server()->Start());
    InProcessBrowserTest::SetUpOnMainThread();
  }

 protected:
  content::WebContents* web() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
  content::StoragePartition* partition() {
    return browser()->profile()->GetDefaultStoragePartition();
  }
  base::FilePath EdgeDefaultDir(const base::FilePath& app_data_root) {
    return app_data_root.Append(FILE_PATH_LITERAL("Microsoft Edge"))
        .Append(FILE_PATH_LITERAL("Default"));
  }

  void SeedOriginStorage(const GURL& origin) {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), origin));
    ASSERT_TRUE(
        content::ExecJs(web(), "localStorage.setItem('auth','lsval');"));
    ASSERT_EQ("ok", content::EvalJs(web(), R"(
      new Promise((resolve, reject) => {
        const o = indexedDB.open('rx', 1);
        o.onupgradeneeded = () => o.result.createObjectStore('s');
        o.onsuccess = () => {
          const db = o.result, tx = db.transaction('s', 'readwrite');
          tx.objectStore('s').put('idbval', 'k');
          tx.oncomplete = () => { db.close(); resolve('ok'); };
          tx.onerror = () => reject('idb');
        };
        o.onerror = () => reject('open');
      }))"));
    // roam-331: acknowledge the local-storage write BEFORE flushing. The
    // renderer's StorageArea::Put is asynchronous, so a Flush issued here can
    // flush nothing and the snapshot below captures an empty LevelDB.
    const std::vector<uint8_t> seeded_key = Latin1Encoded("auth");
    SeededKeyObserver seed_observer(seeded_key);
    mojo::Remote<blink::mojom::StorageArea> area;
    partition()->GetLocalStorageControl()->BindStorageArea(
        blink::StorageKey::CreateFirstParty(url::Origin::Create(origin)),
        area.BindNewPipeAndPassReceiver());
    base::test::TestFuture<std::vector<blink::mojom::KeyValuePtr>> all;
    area->GetAll(seed_observer.BindRemote(), all.GetCallback());
    for (const blink::mojom::KeyValuePtr& kv : all.Get()) {
      if (kv->key == seeded_key) {
        seed_observer.Resolve();
        break;
      }
    }
    ASSERT_TRUE(seed_observer.WaitForKey())
        << "localStorage seed never reached the storage service";

    // Order that queued commit against the directory copy in SnapshotEdgeInto.
    roamux::test::FlushLocalStorageAndWait(partition());

    // IndexedDB is ordered by the awaited tx.oncomplete in the seed JS above
    // (Transaction::CommitPhaseTwo commits the backing store before issuing
    // OnComplete). The path lookup below is NOT a barrier — it returns
    // immediately — so do not treat it as one.
    mojo::Remote<storage::mojom::IndexedDBControlTest> t;
    partition()->GetIndexedDBControl().BindTestInterfaceForTesting(
        t.BindNewPipeAndPassReceiver());
    base::test::TestFuture<const base::FilePath&> p;
    t->GetBaseDataPathForTesting(p.GetCallback());
    ASSERT_FALSE(p.Get().empty());
  }

  void SnapshotEdge(const base::FilePath& app_data_root) {
    SnapshotEdgeInto(app_data_root, EdgeDefaultDir(app_data_root));
  }

  // roam-202: seeds an arbitrary profile directory, so a test can build the
  // Profile 1-only layout the selected-profile propagation must honor.
  void SnapshotEdgeInto(const base::FilePath& app_data_root,
                        const base::FilePath& dst) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    const base::FilePath src = browser()->profile()->GetPath();
    ASSERT_TRUE(base::CreateDirectory(dst));
    ASSERT_TRUE(base::CopyDirectory(
        src.Append(FILE_PATH_LITERAL("Local Storage")),
        dst.Append(FILE_PATH_LITERAL("Local Storage")), true));
    ASSERT_TRUE(base::CopyDirectory(src.Append(FILE_PATH_LITERAL("IndexedDB")),
                                    dst.Append(FILE_PATH_LITERAL("IndexedDB")),
                                    true));
    ASSERT_TRUE(base::WriteFile(
        app_data_root.Append(FILE_PATH_LITERAL("Microsoft Edge"))
            .Append(FILE_PATH_LITERAL("Last Version")),
        "150.0.0.0"));
  }

  // roam-331: verify the SNAPSHOT — the artifact the importer actually reads.
  // Checking the live profile would prove nothing: ReadEdgeLocalStorage opens
  // its own private copy, and SnapshotEdgeInto made a different one. Call this
  // BEFORE ClearOriginStorage, while a failure still points at the seed.
  void VerifySnapshotHasSeed(const base::FilePath& snapshot_profile,
                             const GURL& origin) {
    base::ScopedAllowBlockingForTesting allow_blocking;
    const blink::StorageKey want_key =
        blink::StorageKey::CreateFirstParty(url::Origin::Create(origin));
    const std::vector<uint8_t> want_entry_key = Latin1Encoded("auth");
    const std::vector<uint8_t> want_entry_value = Latin1Encoded("lsval");
    size_t origins_read = 0;
    bool origin_seen = false;
    bool found = false;
    std::string observed;
    for (const OriginLocalStorage& o : ReadEdgeLocalStorage(snapshot_profile)) {
      ++origins_read;
      if (o.storage_key != want_key) {
        continue;
      }
      origin_seen = true;
      for (const LocalStorageEntry& e : o.entries) {
        observed += " {key=" + base::HexEncode(e.key) +
                    " value=" + base::HexEncode(e.value) + "}";
        if (e.key == want_entry_key && e.value == want_entry_value) {
          found = true;
        }
      }
    }
    // An empty or partial read is missing-or-unreadable: the reader soft-fails
    // to empty on a missing, locked or corrupt DB, so do not claim which.
    // Print want-vs-observed so a failure says WHICH of those it was.
    ASSERT_TRUE(found)
        << "localStorage seed not readable from the snapshot (missing or "
           "unreadable)."
        << "\n  live=" << browser()->profile()->GetPath()
        << "\n  snapshot=" << snapshot_profile
        << "\n  want storage_key=" << want_key.GetDebugString()
        << "\n  want key=" << base::HexEncode(want_entry_key)
        << " value=" << base::HexEncode(want_entry_value)
        << "\n  origins_read=" << origins_read
        << " matching_origin=" << (origin_seen ? "yes" : "no")
        << "\n  observed for that origin:"
        << (observed.empty() ? std::string(" <none>") : observed);
  }

  void ClearOriginStorage(const GURL& origin) {
    base::test::TestFuture<void> c1, c2;
    partition()->ClearDataForOrigin(
        content::StoragePartition::REMOVE_DATA_MASK_LOCAL_STORAGE,
        origin.DeprecatedGetOriginAsURL(), c1.GetCallback());
    ASSERT_TRUE(c1.Wait());
    partition()->ClearDataForOrigin(
        content::StoragePartition::REMOVE_DATA_MASK_INDEXEDDB,
        origin.DeprecatedGetOriginAsURL(), c2.GetCallback());
    ASSERT_TRUE(c2.Wait());
  }

  void ForceInitIdb() {
    mojo::Remote<storage::mojom::IndexedDBControlTest> t;
    partition()->GetIndexedDBControl().BindTestInterfaceForTesting(
        t.BindNewPipeAndPassReceiver());
    base::test::TestFuture<void> done;
    t->ForceInitializeFromFilesForTesting(done.GetCallback());
    ASSERT_TRUE(done.Wait());
  }

  EdgeImportReport RunDriver(const base::FilePath& app_data_root,
                             const base::FilePath& profile_dir,
                             uint16_t items) {
    RoamuxEdgeImportDriver driver(browser()->profile(), app_data_root,
                                  profile_dir, items,
                                  /*keychain_for_testing=*/nullptr);
    base::test::TestFuture<EdgeImportReport> future;
    driver.Start(future.GetCallback());
    EXPECT_TRUE(future.Wait());
    return future.Take();
  }
};

class RoamuxEdgeImportDriverTest : public RoamuxEdgeImportDriverTestBase {
 public:
  RoamuxEdgeImportDriverTest() {
    features_.InitAndEnableFeature(features::kEdgeImport);
  }

 private:
  base::test::ScopedFeatureList features_;
};

// The driver imports the browser-side origin-storage carriers into the
// destination profile and reports them.
IN_PROC_BROWSER_TEST_F(RoamuxEdgeImportDriverTest,
                       ImportsBrowserSideCarriersAndReports) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  const GURL origin = embedded_test_server()->GetURL("/title1.html");
  ASSERT_NO_FATAL_FAILURE(SeedOriginStorage(origin));

  base::ScopedTempDir edge;
  ASSERT_TRUE(edge.CreateUniqueTempDir());
  SnapshotEdge(edge.GetPath());
  ASSERT_NO_FATAL_FAILURE(
      VerifySnapshotHasSeed(EdgeDefaultDir(edge.GetPath()), origin));
  ClearOriginStorage(origin);

  // Only non-secret items selected; the driver always imports origin storage.
  EdgeImportReport report = RunDriver(edge.GetPath(),
                                      EdgeDefaultDir(edge.GetPath()),
                                      user_data_importer::HISTORY);

  ASSERT_TRUE(report.Find(EdgeCarrier::kLocalStorage));
  EXPECT_EQ(CarrierStatus::kImported,
            report.Find(EdgeCarrier::kLocalStorage)->status);
  ASSERT_TRUE(report.Find(EdgeCarrier::kIndexedDb));
  EXPECT_EQ(CarrierStatus::kImported,
            report.Find(EdgeCarrier::kIndexedDb)->status);

  ForceInitIdb();
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), origin));
  EXPECT_EQ("lsval", content::EvalJs(web(), "localStorage.getItem('auth')"));
  EXPECT_EQ("idbval", content::EvalJs(web(), kReadIdbJs));
}

// roam-202: a Profile 1-only install (no Default) must import from the
// selected profile — the driver follows SourceProfile.source_path instead of
// re-deriving Default.
IN_PROC_BROWSER_TEST_F(RoamuxEdgeImportDriverTest,
                       ImportsFromSelectedProfileOneLayout) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  const GURL origin = embedded_test_server()->GetURL("/title1.html");
  ASSERT_NO_FATAL_FAILURE(SeedOriginStorage(origin));

  base::ScopedTempDir edge;
  ASSERT_TRUE(edge.CreateUniqueTempDir());
  const base::FilePath profile1 =
      edge.GetPath()
          .Append(FILE_PATH_LITERAL("Microsoft Edge"))
          .Append(FILE_PATH_LITERAL("Profile 1"));
  SnapshotEdgeInto(edge.GetPath(), profile1);
  ASSERT_NO_FATAL_FAILURE(VerifySnapshotHasSeed(profile1, origin));
  ClearOriginStorage(origin);

  EdgeImportReport report =
      RunDriver(edge.GetPath(), profile1, user_data_importer::HISTORY);

  ASSERT_TRUE(report.Find(EdgeCarrier::kLocalStorage));
  EXPECT_EQ(CarrierStatus::kImported,
            report.Find(EdgeCarrier::kLocalStorage)->status);
  ASSERT_TRUE(report.Find(EdgeCarrier::kIndexedDb));
  EXPECT_EQ(CarrierStatus::kImported,
            report.Find(EdgeCarrier::kIndexedDb)->status);
}

// When PASSWORDS|COOKIES are selected they are HANDLED (reported), not silently
// skipped — even with no source secrets (reported unsupported, no Keychain).
IN_PROC_BROWSER_TEST_F(RoamuxEdgeImportDriverTest,
                       SecretsSelectedAreHandledNotSkipped) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  const GURL origin = embedded_test_server()->GetURL("/title1.html");
  ASSERT_NO_FATAL_FAILURE(SeedOriginStorage(origin));

  base::ScopedTempDir edge;
  ASSERT_TRUE(edge.CreateUniqueTempDir());
  SnapshotEdge(edge.GetPath());  // origin storage only; no Login Data / Cookies
  ClearOriginStorage(origin);

  const uint16_t items = user_data_importer::HISTORY |
                         user_data_importer::PASSWORDS |
                         user_data_importer::COOKIES;
  EdgeImportReport report =
      RunDriver(edge.GetPath(), EdgeDefaultDir(edge.GetPath()), items);

  // The secret carriers appear in the report (handled, not skipped); with no
  // source secrets they are unsupported — never dropped.
  ASSERT_TRUE(report.Find(EdgeCarrier::kPasswords));
  EXPECT_EQ(CarrierStatus::kUnsupported,
            report.Find(EdgeCarrier::kPasswords)->status);
  ASSERT_TRUE(report.Find(EdgeCarrier::kCookies));
  EXPECT_EQ(CarrierStatus::kUnsupported,
            report.Find(EdgeCarrier::kCookies)->status);
  // Origin storage still imports alongside.
  ASSERT_TRUE(report.Find(EdgeCarrier::kIndexedDb));
  EXPECT_EQ(CarrierStatus::kImported,
            report.Find(EdgeCarrier::kIndexedDb)->status);
}

class RoamuxEdgeImportDriverDisabledTest
    : public RoamuxEdgeImportDriverTestBase {
 public:
  RoamuxEdgeImportDriverDisabledTest() {
    features_.InitAndDisableFeature(features::kEdgeImport);
  }

 private:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxEdgeImportDriverDisabledTest,
                       FeatureDisabledImportsNothing) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir edge;
  ASSERT_TRUE(edge.CreateUniqueTempDir());

  EdgeImportReport report =
      RunDriver(edge.GetPath(), EdgeDefaultDir(edge.GetPath()),
                user_data_importer::PASSWORDS);
  ASSERT_TRUE(report.Find(EdgeCarrier::kIndexedDb));
  EXPECT_EQ(CarrierStatus::kFeatureDisabled,
            report.Find(EdgeCarrier::kIndexedDb)->status);
}

}  // namespace
}  // namespace roamux
