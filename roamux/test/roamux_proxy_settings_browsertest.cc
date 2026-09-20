// SPDX-License-Identifier: Apache-2.0
// roam-323 slices 2 and 3: the settings handler's guards, state and live
// attribution, and the routing behaviour the issue requires. Every fixture pins
// kRoamuxProxyConfig AND the initial `proxy` pref explicitly; the single
// unpinned sentinel at the bottom asserts the compiled default. Tests whose
// subject is upstream precedence are marked CHARACTERISATION: they pass before
// our handler exists and only guard the ground we build on.

#include <memory>
#include <string>
#include <string_view>
#include <tuple>

#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/password_manager/password_manager_test_base.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/login/login_handler.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/password_manager/core/browser/password_form.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "components/password_manager/core/browser/password_store/password_store_results_observer.h"
#include "components/policy/core/browser/browser_policy_connector_base.h"
#include "components/policy/core/common/mock_configuration_policy_provider.h"
#include "components/policy/core/common/policy_map.h"
#include "components/policy/core/common/policy_types.h"
#include "components/policy/policy_constants.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/pref_proxy_config_tracker_impl.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_web_ui.h"
#include "extensions/browser/extension_pref_value_map.h"
#include "extensions/browser/extension_pref_value_map_factory.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/test/embedded_test_server/register_basic_auth_handler.h"
#include "roamux/browser/ui/webui/roamux_proxy_handler.h"
#include "roamux/common/roamux_features.h"
#include "roamux/test/support/roamux_browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace roamux {
namespace {

constexpr char kProxyMarker[] = "served-by-the-test-proxy";

// Null-safe readers: a missing key is a legible failure, not a crash that
// aborts the run.
std::string Str(const base::DictValue& dict, std::string_view key) {
  const std::string* value = dict.FindString(key);
  return value ? *value : std::string();
}
bool Flag(const base::DictValue& dict, std::string_view key, bool fallback) {
  return dict.FindBool(key).value_or(fallback);
}

// Everything a proxy needs for these tests: answer every absolute-URI request
// with a marker.
std::unique_ptr<net::test_server::HttpResponse> ServeProxyMarker(
    const net::test_server::HttpRequest& request) {
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_content_type("text/plain");
  response->set_content(kProxyMarker);
  return response;
}

// set_web_ui() is protected on WebUIMessageHandler, so the listener test drives
// the handler through a subclass that exposes it (the roam-13 shortcuts-test
// shape).
class ExposedProxyHandler : public RoamuxProxyHandler {
 public:
  using RoamuxProxyHandler::RoamuxProxyHandler;
  using RoamuxProxyHandler::set_web_ui;
};

class RoamuxProxyHandlerTest : public test::RoamuxBrowserTest {
 public:
  RoamuxProxyHandlerTest() {
    features_.InitAndEnableFeature(features::kRoamuxProxyConfig);
  }

 protected:
  void SetUpOnMainThread() override {
    test::RoamuxBrowserTest::SetUpOnMainThread();
    host_resolver()->AddRule("*", "127.0.0.1");
    // The pinned starting point: no user value, so the registered default
    // (system) applies.
    prefs()->ClearPref(proxy_config::prefs::kProxy);
  }

  PrefService* prefs() { return browser()->profile()->GetPrefs(); }

  bool HasUserProxyValue(Profile* profile) {
    return profile->GetPrefs()
        ->FindPreference(proxy_config::prefs::kProxy)
        ->HasUserSetting();
  }

  std::unique_ptr<RoamuxProxyHandler> MakeHandler(Profile* profile) {
    return std::make_unique<RoamuxProxyHandler>(profile);
  }

  base::DictValue ManualFields(const std::string& host,
                               const std::string& port) {
    base::DictValue fields;
    fields.Set("mode", "manual");
    fields.Set("scheme", "http");
    fields.Set("host", host);
    fields.Set("port", port);
    return fields;
  }

  base::test::ScopedFeatureList features_;
};

// ---- slice 2: state, guards, attribution -----------------------------------

IN_PROC_BROWSER_TEST_F(RoamuxProxyHandlerTest,
                       StateStartsAtSystemAndIsEditable) {
  const base::DictValue state =
      MakeHandler(browser()->profile())->GetStateForTesting();
  EXPECT_EQ("system", Str(state, "mode"));
  EXPECT_EQ("system", Str(state, "baseOwner"));
  EXPECT_TRUE(Flag(state, "canEdit", false));
  EXPECT_TRUE(Flag(state, "canReset", false));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyHandlerTest,
                       CommitManualStoresAndReportsOurOwnership) {
  auto handler = MakeHandler(browser()->profile());
  const base::DictValue error =
      handler->SetForTesting(ManualFields("proxy.example", "8080"));
  ASSERT_TRUE(error.empty()) << Str(error, "reason");
  EXPECT_TRUE(HasUserProxyValue(browser()->profile()));
  const base::DictValue state = handler->GetStateForTesting();
  EXPECT_EQ("manual", Str(state, "mode"));
  EXPECT_EQ("proxy.example", Str(state, "host"));
  EXPECT_EQ("8080", Str(state, "port"));
  EXPECT_EQ("roamux", Str(state, "baseOwner"));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyHandlerTest,
                       InvalidFieldsLeaveThePrefUntouched) {
  auto handler = MakeHandler(browser()->profile());
  const base::DictValue error =
      handler->SetForTesting(ManualFields("proxy.example", "80junk"));
  ASSERT_FALSE(error.empty());
  EXPECT_EQ("port", Str(error, "field"));
  EXPECT_FALSE(HasUserProxyValue(browser()->profile()));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyHandlerTest, ResetClearsTheUserLayer) {
  auto handler = MakeHandler(browser()->profile());
  ASSERT_TRUE(
      handler->SetForTesting(ManualFields("proxy.example", "8080")).empty());
  ASSERT_TRUE(HasUserProxyValue(browser()->profile()));
  const base::DictValue error = handler->ResetForTesting();
  EXPECT_TRUE(error.empty()) << Str(error, "reason");
  EXPECT_FALSE(HasUserProxyValue(browser()->profile()))
      << "Reset must clear the user layer, not write mode:system";
  EXPECT_EQ("system", Str(handler->GetStateForTesting(), "baseOwner"));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyHandlerTest,
                       OffTheRecordRefusesBothMutations) {
  Profile* otr =
      browser()->profile()->GetPrimaryOTRProfile(/*create_if_needed=*/true);
  auto handler = MakeHandler(otr);
  const base::DictValue set_error =
      handler->SetForTesting(ManualFields("proxy.example", "8080"));
  ASSERT_FALSE(set_error.empty());
  EXPECT_EQ("incognito", Str(set_error, "reason"));
  const base::DictValue reset_error = handler->ResetForTesting();
  ASSERT_FALSE(reset_error.empty());
  EXPECT_EQ("incognito", Str(reset_error, "reason"));
  EXPECT_FALSE(HasUserProxyValue(browser()->profile()));
  EXPECT_FALSE(Flag(handler->GetStateForTesting(), "canEdit", true));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyHandlerTest,
                       OffTheRecordInheritsTheParentConfiguration) {
  ASSERT_TRUE(MakeHandler(browser()->profile())
                  ->SetForTesting(ManualFields("proxy.example", "8080"))
                  .empty());
  Profile* otr =
      browser()->profile()->GetPrimaryOTRProfile(/*create_if_needed=*/true);
  const base::DictValue state = MakeHandler(otr)->GetStateForTesting();
  EXPECT_EQ("manual", Str(state, "mode"))
      << "the OTR overlay reads the parent value";
  EXPECT_EQ("proxy.example", Str(state, "host"));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyHandlerTest,
                       ExternalChangePushesFreshStateToThePage) {
  content::TestWebUI test_web_ui;
  test_web_ui.set_web_contents(
      browser()->tab_strip_model()->GetActiveWebContents());
  auto handler = std::make_unique<ExposedProxyHandler>(browser()->profile());
  handler->set_web_ui(&test_web_ui);
  handler->AllowJavascriptForTesting();
  const size_t before = test_web_ui.call_data().size();
  // Someone else writes the pref (an extension, policy, or another surface).
  prefs()->Set(proxy_config::prefs::kProxy,
               base::Value(ProxyConfigDictionary::CreateFixedServers(
                   "other.example:3128", std::string())));
  ASSERT_GT(test_web_ui.call_data().size(), before)
      << "the section must not show stale attribution";
  EXPECT_EQ("cr.webUIListenerCallback",
            test_web_ui.call_data().back()->function_name());
  EXPECT_EQ("roamux-proxy-state-changed",
            test_web_ui.call_data().back()->arg1()->GetString());
}

// The engine's own gate (kEnableProxyOverrideRules) is pinned OFF here, so a
// configured list that the engine ignores must never be presented as
// controlling routing.
class RoamuxProxyOverrideRulesIneligibleTest : public RoamuxProxyHandlerTest {
 public:
  RoamuxProxyOverrideRulesIneligibleTest() {
    override_rules_.InitAndDisableFeature(kEnableProxyOverrideRules);
  }

 protected:
  base::test::ScopedFeatureList override_rules_;
};

IN_PROC_BROWSER_TEST_F(RoamuxProxyOverrideRulesIneligibleTest,
                       ConfiguredButNotActiveWhenTheEngineIgnoresThem) {
  base::ListValue rules;
  base::DictValue rule;
  rule.Set("host", "*.corp.example");
  rule.Set("proxy", "rulesproxy:3128");
  rules.Append(std::move(rule));
  prefs()->Set(proxy_config::prefs::kProxyOverrideRules,
               base::Value(std::move(rules)));
  const base::DictValue state =
      MakeHandler(browser()->profile())->GetStateForTesting();
  EXPECT_TRUE(Flag(state, "overrideRulesConfigured", false));
  EXPECT_FALSE(Flag(state, "overrideRulesActive", true))
      << "a list the engine ignores must not be presented as controlling "
         "routing";
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyHandlerTest,
                       OverrideRulesWithNoUsableRuleAreNotActive) {
  base::ListValue rules;
  base::DictValue rule;
  rule.Set("host", "*.corp.example");  // no "proxy": nothing to apply
  rules.Append(std::move(rule));
  prefs()->Set(proxy_config::prefs::kProxyOverrideRules,
               base::Value(std::move(rules)));
  const base::DictValue state =
      MakeHandler(browser()->profile())->GetStateForTesting();
  EXPECT_TRUE(Flag(state, "overrideRulesConfigured", false));
  EXPECT_FALSE(Flag(state, "overrideRulesActive", true));
}

// ---- slice 3: routing ------------------------------------------------------

class RoamuxProxyRoutingTest : public RoamuxProxyHandlerTest {
 protected:
  void SetUpOnMainThread() override {
    RoamuxProxyHandlerTest::SetUpOnMainThread();
    proxy_.RegisterRequestHandler(base::BindRepeating(&ServeProxyMarker));
    ASSERT_TRUE(proxy_.Start());
    ASSERT_TRUE(origin_.Start());
  }

  // A proxy failure still "commits" a navigation (an error page), so the
  // assertion has to be about what the page says, exactly as upstream's own
  // proxy tests do.
  bool NavigationShowsError(const GURL& url, const std::string& error) {
    std::ignore = ui_test_utils::NavigateToURL(browser(), url);
    const std::string text =
        content::EvalJs(browser()->tab_strip_model()->GetActiveWebContents(),
                        "document.body.textContent")
            .ExtractString();
    return text.find(error) != std::string::npos;
  }

  std::string NavigateAndReadBody(const GURL& url) {
    EXPECT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
    return content::EvalJs(browser()->tab_strip_model()->GetActiveWebContents(),
                           "document.body.textContent")
        .ExtractString();
  }

  net::test_server::EmbeddedTestServer proxy_;
  net::test_server::EmbeddedTestServer origin_;
};

IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       ManualConfigurationActuallyRoutes) {
  auto handler = MakeHandler(browser()->profile());
  ASSERT_TRUE(handler
                  ->SetForTesting(ManualFields(
                      "127.0.0.1", base::NumberToString(proxy_.port())))
                  .empty());
  EXPECT_EQ(kProxyMarker,
            NavigateAndReadBody(GURL("http://routed.example/anything")));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       ExplicitSystemModeStopsRoutingThroughUs) {
  auto handler = MakeHandler(browser()->profile());
  ASSERT_TRUE(handler
                  ->SetForTesting(ManualFields(
                      "127.0.0.1", base::NumberToString(proxy_.port())))
                  .empty());
  ASSERT_EQ(kProxyMarker,
            NavigateAndReadBody(GURL("http://routed.example/anything")));
  base::DictValue system;
  system.Set("mode", "system");
  ASSERT_TRUE(handler->SetForTesting(system).empty());
  const std::string body = NavigateAndReadBody(origin_.GetURL("/title1.html"));
  EXPECT_EQ(std::string::npos, body.find(kProxyMarker))
      << "mode system must stop routing through our endpoint";
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest, UnreachableEndpointFailsClosed) {
  const uint16_t dead_port = proxy_.port();
  ASSERT_TRUE(proxy_.ShutdownAndWaitUntilComplete());
  auto handler = MakeHandler(browser()->profile());
  ASSERT_TRUE(handler
                  ->SetForTesting(ManualFields("127.0.0.1",
                                               base::NumberToString(dead_port)))
                  .empty());
  EXPECT_TRUE(NavigationShowsError(GURL("http://routed.example/x"),
                                   "ERR_PROXY_CONNECTION_FAILED"))
      << "a single endpoint with no DIRECT must fail closed, not go direct";
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       PacReturningDirectBypassesTheProxy) {
  auto handler = MakeHandler(browser()->profile());
  base::DictValue fields;
  fields.Set("mode", "pac");
  fields.Set("pacUrl",
             "data:application/x-ns-proxy-autoconfig,function "
             "FindProxyForURL(url, host) { return \"DIRECT\"; }");
  fields.Set("pacMandatory", true);
  ASSERT_TRUE(handler->SetForTesting(fields).empty());
  const std::string body = NavigateAndReadBody(origin_.GetURL("/title1.html"));
  EXPECT_EQ(std::string::npos, body.find(kProxyMarker))
      << "mandatory PAC does not stop a script that returns DIRECT";
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       MandatoryPacFailureFailsClosedNonMandatoryDoesNot) {
  auto handler = MakeHandler(browser()->profile());
  const GURL missing = origin_.GetURL("/no-such-file.pac");
  base::DictValue mandatory;
  mandatory.Set("mode", "pac");
  mandatory.Set("pacUrl", missing.spec());
  mandatory.Set("pacMandatory", true);
  ASSERT_TRUE(handler->SetForTesting(mandatory).empty());
  EXPECT_TRUE(NavigationShowsError(GURL("http://routed.example/x"),
                                   "ERR_MANDATORY_PROXY_CONFIGURATION_FAILED"))
      << "a mandatory PAC that cannot load must fail the affected request";

  base::DictValue optional_pac = mandatory.Clone();
  optional_pac.Set("pacMandatory", false);
  ASSERT_TRUE(handler->SetForTesting(optional_pac).empty());
  EXPECT_TRUE(
      ui_test_utils::NavigateToURL(browser(), origin_.GetURL("/title1.html")))
      << "a non-mandatory PAC falls back instead of failing";
}

// ---- slice 3 (continued): who wins, profile scoping, and proxy authentication
// ----

// A policy-set value must keep routing and keep us out of the pref, and the
// section must say so.
class RoamuxProxyPolicyTest : public RoamuxProxyRoutingTest {
 public:
  void SetUpInProcessBrowserTestFixture() override {
    RoamuxProxyRoutingTest::SetUpInProcessBrowserTestFixture();
    policy_provider_.SetDefaultReturns(
        /*is_initialization_complete_return=*/true,
        /*is_first_policy_load_complete_return=*/true);
    policy::BrowserPolicyConnectorBase::SetPolicyProviderForTesting(
        &policy_provider_);
  }

 protected:
  void SetPolicyProxy(const std::string& host_port) {
    policy::PolicyMap policies;
    policies.Set(policy::key::kProxyMode, policy::POLICY_LEVEL_MANDATORY,
                 policy::POLICY_SCOPE_USER, policy::POLICY_SOURCE_CLOUD,
                 base::Value("fixed_servers"), nullptr);
    policies.Set(policy::key::kProxyServer, policy::POLICY_LEVEL_MANDATORY,
                 policy::POLICY_SCOPE_USER, policy::POLICY_SOURCE_CLOUD,
                 base::Value(host_port), nullptr);
    policy_provider_.UpdateChromePolicy(policies);
  }

  testing::NiceMock<policy::MockConfigurationPolicyProvider> policy_provider_;
};

IN_PROC_BROWSER_TEST_F(RoamuxProxyPolicyTest,
                       PolicyKeepsRoutingAndWeRefuseToWrite) {
  SetPolicyProxy("127.0.0.1:" + base::NumberToString(proxy_.port()));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return prefs()->FindPreference(proxy_config::prefs::kProxy)->IsManaged();
  }));
  auto handler = MakeHandler(browser()->profile());
  const base::DictValue state = handler->GetStateForTesting();
  EXPECT_EQ("policy", Str(state, "baseOwner"));
  EXPECT_FALSE(Flag(state, "canEdit", true));

  const base::DictValue error =
      handler->SetForTesting(ManualFields("other.example", "9999"));
  ASSERT_FALSE(error.empty());
  EXPECT_EQ("policy", Str(error, "reason"));
  EXPECT_EQ(kProxyMarker, NavigateAndReadBody(GURL("http://routed.example/x")))
      << "policy's proxy must keep routing";

  // Reset is still reachable: it removes OUR value (there is none) and the
  // label must keep naming policy rather than claiming the system config.
  EXPECT_TRUE(handler->ResetForTesting().empty());
  EXPECT_EQ("policy", Str(handler->GetStateForTesting(), "baseOwner"));
}

// An extension-set value likewise wins, and the section names the extension.
IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       ExtensionKeepsRoutingAndIsNamed) {
  constexpr char kExtensionId[] = "abcdefghijklmnopabcdefghijklmnop";
  ExtensionPrefValueMap* map =
      ExtensionPrefValueMapFactory::GetForBrowserContext(browser()->profile());
  ASSERT_TRUE(map);
  map->RegisterExtension(kExtensionId, base::Time::Now(), /*is_enabled=*/true,
                         /*is_incognito_enabled=*/false);
  map->SetExtensionPref(
      kExtensionId, proxy_config::prefs::kProxy,
      ExtensionPrefValueMap::ChromeSettingScope::kRegular,
      base::Value(ProxyConfigDictionary::CreateFixedServers(
          "127.0.0.1:" + base::NumberToString(proxy_.port()), std::string())));

  auto handler = MakeHandler(browser()->profile());
  const base::DictValue state = handler->GetStateForTesting();
  EXPECT_EQ("extension", Str(state, "baseOwner"));
  EXPECT_EQ(kExtensionId, Str(state, "controllerName"))
      << "with no registry entry the id is the honest answer";
  EXPECT_FALSE(Flag(state, "canEdit", true));
  EXPECT_EQ("extension",
            Str(handler->SetForTesting(ManualFields("other.example", "9999")),
                "reason"));
  EXPECT_EQ(kProxyMarker, NavigateAndReadBody(GURL("http://routed.example/x")));
}

// The profile is the boundary: two profiles route through their own proxies,
// and Local State (which governs browser-service traffic and is global) is
// never touched by this section.
IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       ProfilesRouteThroughTheirOwnProxies) {
  net::test_server::EmbeddedTestServer second_proxy;
  second_proxy.RegisterRequestHandler(
      base::BindRepeating([](const net::test_server::HttpRequest& request) {
        auto response = std::make_unique<net::test_server::BasicHttpResponse>();
        response->set_content_type("text/plain");
        response->set_content("served-by-the-second-proxy");
        return std::unique_ptr<net::test_server::HttpResponse>(
            std::move(response));
      }));
  ASSERT_TRUE(second_proxy.Start());

  ASSERT_TRUE(MakeHandler(browser()->profile())
                  ->SetForTesting(ManualFields(
                      "127.0.0.1", base::NumberToString(proxy_.port())))
                  .empty());

  Profile& other = profiles::testing::CreateProfileSync(
      g_browser_process->profile_manager(),
      g_browser_process->profile_manager()->user_data_dir().AppendASCII(
          "roamux-proxy-second"));
  ASSERT_TRUE(MakeHandler(&other)
                  ->SetForTesting(ManualFields(
                      "127.0.0.1", base::NumberToString(second_proxy.port())))
                  .empty());
  Browser* other_browser = CreateBrowser(&other);

  EXPECT_EQ(kProxyMarker, NavigateAndReadBody(GURL("http://routed.example/x")));
  ASSERT_TRUE(ui_test_utils::NavigateToURL(other_browser,
                                           GURL("http://routed.example/x")));
  EXPECT_EQ(
      "served-by-the-second-proxy",
      content::EvalJs(other_browser->tab_strip_model()->GetActiveWebContents(),
                      "document.body.textContent")
          .ExtractString());
  EXPECT_FALSE(g_browser_process->local_state()
                   ->FindPreference(proxy_config::prefs::kProxy)
                   ->HasUserSetting())
      << "this section writes the profile pref only";
}

// Proxy authentication is upstream's 407 -> LoginHandler path; we build no
// credential store.
class RoamuxProxyAuthTest : public RoamuxProxyHandlerTest {
 protected:
  void SetUpOnMainThread() override {
    RoamuxProxyHandlerTest::SetUpOnMainThread();
    net::test_server::RegisterProxyBasicAuthHandler(auth_proxy_, "user",
                                                    "pass");
    auth_proxy_.RegisterRequestHandler(base::BindRepeating(&ServeProxyMarker));
    ASSERT_TRUE(auth_proxy_.Start());
    ASSERT_TRUE(MakeHandler(browser()->profile())
                    ->SetForTesting(ManualFields(
                        "127.0.0.1", base::NumberToString(auth_proxy_.port())))
                    .empty());
  }

  bool WaitForPrompt() {
    return base::test::RunUntil(
        []() { return !LoginHandler::GetAllLoginHandlersForTest().empty(); });
  }

  std::string BodyOfActiveTab() {
    // A page mid-load (or an error page) makes EvalJs fail; that is a "no
    // marker yet", not a test failure, so the value is read defensively.
    const content::EvalJsResult result =
        content::EvalJs(browser()->tab_strip_model()->GetActiveWebContents(),
                        "document.body.textContent");
    return result.is_string() ? result.ExtractString() : std::string();
  }

  // Polls the page instead of relying on load-success semantics: an
  // authenticated proxy response and a 407 error page both "finish" loading.
  bool BodyEventuallyContains(const std::string& needle) {
    return base::test::RunUntil(
        [&]() { return BodyOfActiveTab().find(needle) != std::string::npos; });
  }

  net::test_server::EmbeddedTestServer auth_proxy_;
};

IN_PROC_BROWSER_TEST_F(RoamuxProxyAuthTest,
                       PromptCancelRetryThenSavedAndFilled) {
  // 1. The prompt appears (the LoginHandler path, not something we built).
  content::WebContents* tab =
      browser()->tab_strip_model()->GetActiveWebContents();
  tab->GetController().LoadURL(GURL("http://routed.example/x"),
                               content::Referrer(), ui::PAGE_TRANSITION_TYPED,
                               std::string());
  ASSERT_TRUE(WaitForPrompt());

  // 2. Cancel: no marker, because the proxy never let the request through.
  LoginHandler::GetAllLoginHandlersForTest().front()->CancelAuth(
      /*notify_others=*/true);
  // Cancelling leaves the 407 error page committed, so the success-checking
  // wait would report failure for the expected outcome.
  content::WaitForLoadStopWithoutSuccessCheck(tab);
  EXPECT_EQ(std::string::npos, BodyOfActiveTab().find(kProxyMarker));

  // 3. Retry with the right credentials: the page loads through the proxy.
  tab->GetController().LoadURL(GURL("http://routed.example/x"),
                               content::Referrer(), ui::PAGE_TRANSITION_TYPED,
                               std::string());
  ASSERT_TRUE(WaitForPrompt());
  LoginHandler::GetAllLoginHandlersForTest().front()->SetAuth(u"user", u"pass");
  ASSERT_TRUE(BodyEventuallyContains(kProxyMarker))
      << "the supplied credentials must get the request through";

  // 4. Saved: the credential goes to the password manager under the proxy's
  // signon realm (login_handler.cc builds it as host:port/realm). Chromium asks
  // before storing, so the test accepts that prompt exactly as a user would —
  // we store nothing ourselves.
  BubbleObserver bubble(tab);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return bubble.IsSavePromptShownAutomatically();
  })) << "the 407 credential must be offered to the password manager";
  bubble.AcceptSavePrompt();
  // (original comment) the credential reached the password manager under the
  // proxy's signon realm (login_handler.cc builds it as host:port/realm) — we
  // store nothing ourselves.
  const std::string expected_realm =
      "127.0.0.1:" + base::NumberToString(auth_proxy_.port());
  const auto credential_stored = [&]() {
    password_manager::PasswordStoreResultsObserver results;
    ProfilePasswordStoreFactory::GetForProfile(
        browser()->profile(), ServiceAccessType::EXPLICIT_ACCESS)
        ->GetAllLogins(results.GetWeakPtr());
    for (const password_manager::PasswordForm& form :
         results.WaitForResults()) {
      if (form.signon_realm.find(expected_realm) != std::string::npos &&
          form.username_value == u"user") {
        return true;
      }
    }
    return false;
  };
  const bool saved = base::test::RunUntil(credential_stored);
  EXPECT_TRUE(saved) << "the 407 credential belongs to the password manager";

  // 5. Filled: a fresh navigation goes through without prompting again.
  tab->GetController().LoadURL(GURL("http://routed.example/again"),
                               content::Referrer(), ui::PAGE_TRANSITION_TYPED,
                               std::string());
  ASSERT_TRUE(BodyEventuallyContains(kProxyMarker));
  EXPECT_TRUE(LoginHandler::GetAllLoginHandlersForTest().empty())
      << "the saved credential must be reused, not re-prompted";
}

// ---- slice 4: the real Settings DOM ---------------------------------------
// Handler tests cannot catch a missing mount, a wrong message name, an
// unrendered field error or an unreachable Reset button, so these drive
// chrome://settings/system itself (the roam-277 settings-DOM precedent).

constexpr char kSectionId[] = "roamuxProxySection";

// Polls open shadow roots for an id, because Polymer stamps dom-if templates
// asynchronously.
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

// Runs `body` with `el` bound to the deep-queried element of `id`.
constexpr char kWithElementScript[] = R"(
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
      let el = null;
      for (let i = 0; i < 100 && !el; i++) {
        el = deepQuery(document, '%s');
        if (!el) await new Promise(r => setTimeout(r, 100));
      }
      if (!el) return 'missing';
      %s
    })();
)";

std::string WaitForIdScript(const std::string& id, int attempts) {
  return base::StringPrintf(kWaitForIdScript, attempts, id.c_str());
}

std::string WithElementScript(const std::string& id, const std::string& body) {
  return base::StringPrintf(kWithElementScript, id.c_str(), body.c_str());
}

class RoamuxProxySectionDomTest : public RoamuxProxyHandlerTest {
 protected:
  content::WebContents* NavigateToSystemSettings() {
    EXPECT_TRUE(ui_test_utils::NavigateToURL(browser(),
                                             GURL("chrome://settings/system")));
    content::WebContents* web_contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    // The page is interactive once a stock System row is stamped.
    EXPECT_EQ(true,
              content::EvalJs(web_contents,
                              WaitForIdScript("hardwareAcceleration", 100)));
    return web_contents;
  }
};

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       SectionIsPresentWithTheFlagOn) {
  EXPECT_EQ(true, content::EvalJs(NavigateToSystemSettings(),
                                  WaitForIdScript(kSectionId, 100)));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       ManualModeEditsThePrefThroughTheUi) {
  content::WebContents* web_contents = NavigateToSystemSettings();
  ASSERT_EQ(true,
            content::EvalJs(web_contents, WaitForIdScript(kSectionId, 100)));
  const std::string body = base::StringPrintf(
      R"(
        await el.setModeForTesting('manual', {scheme: 'http', host: 'ui.example', port: '8080'});
        return 'ok';
      )");
  ASSERT_EQ("ok",
            content::EvalJs(web_contents, WithElementScript(kSectionId, body)));
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return prefs()
        ->FindPreference(proxy_config::prefs::kProxy)
        ->HasUserSetting();
  }));
  const base::DictValue state =
      MakeHandler(browser()->profile())->GetStateForTesting();
  EXPECT_EQ("manual", Str(state, "mode"));
  EXPECT_EQ("ui.example", Str(state, "host"));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       InvalidInputShowsAFieldErrorAndWritesNothing) {
  content::WebContents* web_contents = NavigateToSystemSettings();
  ASSERT_EQ(true,
            content::EvalJs(web_contents, WaitForIdScript(kSectionId, 100)));
  const std::string body = R"(
        await el.setModeForTesting('manual', {scheme: 'http', host: 'user:pass@ui.example', port: '8080'});
        return el.errorFieldForTesting() + '/' + el.errorReasonForTesting();
      )";
  EXPECT_EQ("host/userinfo",
            content::EvalJs(web_contents, WithElementScript(kSectionId, body)));
  EXPECT_FALSE(
      prefs()->FindPreference(proxy_config::prefs::kProxy)->HasUserSetting());
}

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest, ResetIsReachableAndClears) {
  ASSERT_TRUE(MakeHandler(browser()->profile())
                  ->SetForTesting(ManualFields("ui.example", "8080"))
                  .empty());
  content::WebContents* web_contents = NavigateToSystemSettings();
  ASSERT_EQ(true,
            content::EvalJs(web_contents, WaitForIdScript(kSectionId, 100)));
  const std::string body = R"(
        await el.resetForTesting();
        return 'ok';
      )";
  ASSERT_EQ("ok",
            content::EvalJs(web_contents, WithElementScript(kSectionId, body)));
  EXPECT_TRUE(base::test::RunUntil([&]() {
    return !prefs()
                ->FindPreference(proxy_config::prefs::kProxy)
                ->HasUserSetting();
  }));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       TheLabelFollowsAnExternalChange) {
  content::WebContents* web_contents = NavigateToSystemSettings();
  ASSERT_EQ(true,
            content::EvalJs(web_contents, WaitForIdScript(kSectionId, 100)));
  prefs()->Set(proxy_config::prefs::kProxy,
               base::Value(ProxyConfigDictionary::CreateFixedServers(
                   "external.example:3128", std::string())));
  const std::string body = R"(
        for (let i = 0; i < 100; i++) {
          if (el.baseOwnerForTesting() === 'roamux') return el.baseOwnerForTesting();
          await new Promise(r => setTimeout(r, 100));
        }
        return el.baseOwnerForTesting();
      )";
  EXPECT_EQ("roamux",
            content::EvalJs(web_contents, WithElementScript(kSectionId, body)))
      << "the section must not show stale attribution";
}

// Flag off: upstream's own row is what the user sees, and our section is
// absent.
class RoamuxProxySectionFlagOffTest : public test::RoamuxBrowserTest {
 public:
  RoamuxProxySectionFlagOffTest() {
    features_.InitAndDisableFeature(features::kRoamuxProxyConfig);
  }

 protected:
  base::test::ScopedFeatureList features_;
};

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionFlagOffTest,
                       SectionIsAbsentAndStockRowRemains) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(),
                                           GURL("chrome://settings/system")));
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  EXPECT_EQ(true, content::EvalJs(web_contents, WaitForIdScript("proxy", 100)))
      << "upstream's stock proxy row must be untouched when the flag is off";
  EXPECT_EQ(false,
            content::EvalJs(web_contents, WaitForIdScript(kSectionId, 5)));
}

// ---- the one unpinned sentinel --------------------------------------------

class RoamuxProxyCompiledDefaultTest : public test::RoamuxBrowserTest {};

IN_PROC_BROWSER_TEST_F(RoamuxProxyCompiledDefaultTest, FeatureShipsDisabled) {
  EXPECT_FALSE(base::FeatureList::IsEnabled(features::kRoamuxProxyConfig))
      << "roam-323 lands default-OFF; graduation is roam-326";
}

}  // namespace
}  // namespace roamux
