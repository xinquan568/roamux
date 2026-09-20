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
#include "components/proxy_config/proxy_prefs.h"
#include "components/proxy_config/proxy_prefs_utils.h"
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
constexpr char kOriginMarker[] = "served-by-the-origin-directly";
constexpr char kUserProxyMarker[] = "served-by-the-user-proxy";

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

// A second proxy, for the tests where two configurations compete and the
// question is which one won — not merely whether ours was used.
std::unique_ptr<net::test_server::HttpResponse> ServeFixedMarker(
    const std::string& marker,
    const net::test_server::HttpRequest& request) {
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_content_type("text/plain");
  response->set_content(marker);
  return response;
}

// The destination itself answers /marker with its OWN body, so every routing
// assertion below names the server that replied. "No proxy marker" alone would
// also be produced by an implicit bypass, which is exactly the trap these tests
// have to avoid.
std::unique_ptr<net::test_server::HttpResponse> ServeOriginMarker(
    const net::test_server::HttpRequest& request) {
  if (request.relative_url != "/marker") {
    return nullptr;  // other paths stay 404 (the PAC-not-found test needs one)
  }
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_content_type("text/plain");
  response->set_content(kOriginMarker);
  return response;
}

// The authenticating proxy echoes the path, so a later navigation cannot be
// "proved" by the body of the document that is still on screen.
std::unique_ptr<net::test_server::HttpResponse> ServeProxyMarkerWithPath(
    const net::test_server::HttpRequest& request) {
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_content_type("text/plain");
  response->set_content(std::string(kProxyMarker) + " " + request.relative_url);
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

IN_PROC_BROWSER_TEST_F(RoamuxProxyHandlerTest,
                       AnUnknownModeIsRejectedAndChangesNothing) {
  auto handler = MakeHandler(browser()->profile());
  ASSERT_TRUE(
      handler->SetForTesting(ManualFields("proxy.example", "8080")).empty());

  for (const char* mode : {"sistem", "", "auto_detect"}) {
    base::DictValue fields;
    fields.Set("mode", mode);
    const base::DictValue error = handler->SetForTesting(fields);
    ASSERT_FALSE(error.empty()) << mode;
    EXPECT_EQ("mode", Str(error, "reason")) << mode;
  }
  // The empty dictionary is the same case: no mode at all.
  EXPECT_EQ("mode", Str(handler->SetForTesting(base::DictValue()), "reason"));

  EXPECT_TRUE(HasUserProxyValue(browser()->profile()))
      << "a malformed mode must never clear a saved proxy";
  EXPECT_EQ("manual", Str(handler->GetStateForTesting(), "mode"));
}

// An existing PAC configuration with no pac_mandatory key permits fallback in
// the network layer, so the state must not imply it blocks.
IN_PROC_BROWSER_TEST_F(RoamuxProxyHandlerTest,
                       AnExistingPacWithoutTheKeyIsReportedNonMandatory) {
  base::DictValue pac;
  pac.Set("mode", "pac_script");
  pac.Set("pac_url", "https://pac.example/proxy.pac");
  prefs()->Set(proxy_config::prefs::kProxy, base::Value(std::move(pac)));
  const base::DictValue state =
      MakeHandler(browser()->profile())->GetStateForTesting();
  ASSERT_EQ("pac", Str(state, "mode"));
  EXPECT_FALSE(Flag(state, "pacMandatory", true))
      << "an absent pac_mandatory key means fallback is allowed";
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

// A rule in the engine's own schema ("DestinationMatchers"/"ProxyList"), with
// the engine's gate on: the section reports it as actually applied. The answer
// comes from PrefProxyConfigTrackerImpl::ReadPrefConfig, so it cannot drift
// from what the network layer does with the same list.
base::ListValue OneOverrideRule() {
  base::ListValue rules;
  base::DictValue rule;
  base::ListValue matchers;
  matchers.Append("https://app.corp.example");
  base::ListValue proxies;
  proxies.Append("HTTPS rules.example:443");
  rule.Set(proxy_config::kKeyDestinationMatchers, std::move(matchers));
  rule.Set(proxy_config::kKeyProxyList, std::move(proxies));
  rules.Append(std::move(rule));
  return rules;
}

class RoamuxProxyOverrideRulesEligibleTest : public RoamuxProxyHandlerTest {
 public:
  RoamuxProxyOverrideRulesEligibleTest() {
    override_rules_.InitAndEnableFeature(kEnableProxyOverrideRules);
  }

 protected:
  base::test::ScopedFeatureList override_rules_;
};

IN_PROC_BROWSER_TEST_F(RoamuxProxyOverrideRulesEligibleTest,
                       AnAppliedRuleIsReportedActive) {
  prefs()->Set(proxy_config::prefs::kProxyOverrideRules,
               base::Value(OneOverrideRule()));
  const base::DictValue state =
      MakeHandler(browser()->profile())->GetStateForTesting();
  EXPECT_TRUE(Flag(state, "overrideRulesConfigured", false));
  EXPECT_TRUE(Flag(state, "overrideRulesActive", false))
      << "a rule the engine applies must be reported as in effect";
  EXPECT_EQ("user", Str(state, "overrideRulesOwner"));
}

// The keys are the policy's, not a guess: a dictionary whose only proxy-looking
// key is a lowercase "proxy" is not a rule the engine can apply, and must not
// be reported as one.
IN_PROC_BROWSER_TEST_F(RoamuxProxyOverrideRulesEligibleTest,
                       ADictionaryInTheWrongSchemaIsNotActive) {
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
  EXPECT_FALSE(Flag(state, "overrideRulesActive", true));
}

// A rule missing the required "ProxyList" cannot be applied either.
IN_PROC_BROWSER_TEST_F(RoamuxProxyOverrideRulesEligibleTest,
                       ARuleWithNoProxyListIsNotActive) {
  base::ListValue rules;
  base::DictValue rule;
  base::ListValue matchers;
  matchers.Append("https://app.corp.example");
  rule.Set(proxy_config::kKeyDestinationMatchers, std::move(matchers));
  rules.Append(std::move(rule));
  prefs()->Set(proxy_config::prefs::kProxyOverrideRules,
               base::Value(std::move(rules)));
  const base::DictValue state =
      MakeHandler(browser()->profile())->GetStateForTesting();
  EXPECT_TRUE(Flag(state, "overrideRulesConfigured", false));
  EXPECT_FALSE(Flag(state, "overrideRulesActive", true));
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
  prefs()->Set(proxy_config::prefs::kProxyOverrideRules,
               base::Value(OneOverrideRule()));
  const base::DictValue state =
      MakeHandler(browser()->profile())->GetStateForTesting();
  EXPECT_TRUE(Flag(state, "overrideRulesConfigured", false));
  EXPECT_FALSE(Flag(state, "overrideRulesActive", true))
      << "a list the engine ignores must not be presented as controlling "
         "routing";
}

// ---- slice 3: routing ------------------------------------------------------

class RoamuxProxyRoutingTest : public RoamuxProxyHandlerTest {
 protected:
  void SetUpOnMainThread() override {
    RoamuxProxyHandlerTest::SetUpOnMainThread();
    proxy_.RegisterRequestHandler(base::BindRepeating(&ServeProxyMarker));
    ASSERT_TRUE(proxy_.Start());
    user_proxy_.RegisterRequestHandler(
        base::BindRepeating(&ServeFixedMarker, std::string(kUserProxyMarker)));
    ASSERT_TRUE(user_proxy_.Start());
    origin_.RegisterRequestHandler(base::BindRepeating(&ServeOriginMarker));
    ASSERT_TRUE(origin_.Start());
  }

  // Deliberately NOT a loopback URL. Chromium's proxy rules implicitly bypass
  // localhost, so a test that navigated to 127.0.0.1 would observe "no proxy"
  // whatever these settings said. The fixture's host resolver maps every name
  // to 127.0.0.1, so this still reaches `origin_` — through whatever proxy
  // configuration is in force.
  GURL OriginUrl() { return origin_.GetURL("origin.example", "/marker"); }

  base::DictValue ProxyFields(
      const net::test_server::EmbeddedTestServer& server) {
    return ManualFields("127.0.0.1", base::NumberToString(server.port()));
  }

  std::string HostPort(const net::test_server::EmbeddedTestServer& server) {
    return "127.0.0.1:" + base::NumberToString(server.port());
  }

  // A proxy failure still "commits" a navigation (an error page), so the
  // assertion has to be about what the page says, exactly as upstream's own
  // proxy tests do.
  bool NavigationShowsError(const GURL& url, const std::string& error) {
    std::ignore = ui_test_utils::NavigateToURL(browser(), url);
    return BodyOf(browser()).find(error) != std::string::npos;
  }

  std::string BodyOf(Browser* target) {
    return content::EvalJs(target->tab_strip_model()->GetActiveWebContents(),
                           "document.body.textContent")
        .ExtractString();
  }

  std::string NavigateAndReadBody(const GURL& url) {
    EXPECT_TRUE(ui_test_utils::NavigateToURL(browser(), url));
    return BodyOf(browser());
  }

  std::string NavigateAndReadBody(Browser* target, const GURL& url) {
    EXPECT_TRUE(ui_test_utils::NavigateToURL(target, url));
    return BodyOf(target);
  }

  net::test_server::EmbeddedTestServer proxy_;
  net::test_server::EmbeddedTestServer user_proxy_;
  net::test_server::EmbeddedTestServer origin_;
};

IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       ManualConfigurationChangesWhichServerAnswers) {
  // The same destination, before and after: without a configuration the origin
  // answers, and with one the proxy does. Either half alone would be vacuous.
  ASSERT_EQ(kOriginMarker, NavigateAndReadBody(OriginUrl()));
  auto handler = MakeHandler(browser()->profile());
  ASSERT_TRUE(handler->SetForTesting(ProxyFields(proxy_)).empty());
  EXPECT_EQ(kProxyMarker, NavigateAndReadBody(OriginUrl()));
}

// PRECONDITION: the test environment has no system proxy configured, so mode
// `system` resolves to a direct connection here. What is asserted is that OUR
// endpoint leaves the path and the request reaches the destination — that a
// real macOS system proxy is honoured is upstream's own configuration service
// and is not established by this test.
IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       ExplicitSystemModeStopsRoutingThroughUs) {
  auto handler = MakeHandler(browser()->profile());
  ASSERT_TRUE(handler->SetForTesting(ProxyFields(proxy_)).empty());
  ASSERT_EQ(kProxyMarker, NavigateAndReadBody(OriginUrl()));
  base::DictValue system;
  system.Set("mode", "system");
  ASSERT_TRUE(handler->SetForTesting(system).empty());
  EXPECT_EQ(kOriginMarker, NavigateAndReadBody(OriginUrl()))
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
  // The destination would answer directly (the test above proves it), so the
  // failure can only be the proxy's.
  EXPECT_TRUE(NavigationShowsError(OriginUrl(), "ERR_PROXY_CONNECTION_FAILED"))
      << "a single endpoint with no DIRECT must fail closed, not go direct";
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       PacReturningDirectBypassesTheProxy) {
  auto handler = MakeHandler(browser()->profile());
  ASSERT_TRUE(handler->SetForTesting(ProxyFields(proxy_)).empty());
  ASSERT_EQ(kProxyMarker, NavigateAndReadBody(OriginUrl()));
  base::DictValue fields;
  fields.Set("mode", "pac");
  fields.Set("pacUrl",
             "data:application/x-ns-proxy-autoconfig,function "
             "FindProxyForURL(url, host) { return \"DIRECT\"; }");
  fields.Set("pacMandatory", true);
  ASSERT_TRUE(handler->SetForTesting(fields).empty());
  EXPECT_EQ(kOriginMarker, NavigateAndReadBody(OriginUrl()))
      << "mandatory PAC does not stop a script that returns DIRECT";
}

IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       MandatoryPacFailureFailsClosedNonMandatoryDoesNot) {
  auto handler = MakeHandler(browser()->profile());
  const GURL missing = origin_.GetURL("origin.example", "/no-such-file.pac");
  base::DictValue mandatory;
  mandatory.Set("mode", "pac");
  mandatory.Set("pacUrl", missing.spec());
  mandatory.Set("pacMandatory", true);
  ASSERT_TRUE(handler->SetForTesting(mandatory).empty());
  EXPECT_TRUE(NavigationShowsError(OriginUrl(),
                                   "ERR_MANDATORY_PROXY_CONFIGURATION_FAILED"))
      << "a mandatory PAC that cannot load must fail the affected request";

  base::DictValue optional_pac = mandatory.Clone();
  optional_pac.Set("pacMandatory", false);
  ASSERT_TRUE(handler->SetForTesting(optional_pac).empty());
  EXPECT_EQ(kOriginMarker, NavigateAndReadBody(OriginUrl()))
      << "a non-mandatory PAC falls back to a direct connection";
}

// The incognito overlay reads the parent value, and — the part a pref check
// cannot show — an incognito window's requests actually go through it.
IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       OffTheRecordInheritsAndRoutesThroughTheParentProxy) {
  ASSERT_TRUE(MakeHandler(browser()->profile())
                  ->SetForTesting(ProxyFields(proxy_))
                  .empty());
  Profile* otr =
      browser()->profile()->GetPrimaryOTRProfile(/*create_if_needed=*/true);
  const base::DictValue state = MakeHandler(otr)->GetStateForTesting();
  EXPECT_EQ("manual", Str(state, "mode"))
      << "the OTR overlay reads the parent value";
  EXPECT_EQ("127.0.0.1", Str(state, "host"));
  EXPECT_EQ(kProxyMarker,
            NavigateAndReadBody(CreateIncognitoBrowser(), OriginUrl()))
      << "an incognito window routes through the parent profile's proxy";
}

// The profile is the boundary: two profiles route through their own proxies,
// and Local State (which governs browser-service traffic and is global) is
// never touched by this section.
IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       ProfilesRouteThroughTheirOwnProxies) {
  ASSERT_TRUE(MakeHandler(browser()->profile())
                  ->SetForTesting(ProxyFields(proxy_))
                  .empty());

  Profile& other = profiles::testing::CreateProfileSync(
      g_browser_process->profile_manager(),
      g_browser_process->profile_manager()->user_data_dir().AppendASCII(
          "roamux-proxy-second"));
  ASSERT_TRUE(
      MakeHandler(&other)->SetForTesting(ProxyFields(user_proxy_)).empty());

  EXPECT_EQ(kProxyMarker, NavigateAndReadBody(OriginUrl()));
  EXPECT_EQ(kUserProxyMarker,
            NavigateAndReadBody(CreateBrowser(&other), OriginUrl()));
  EXPECT_FALSE(g_browser_process->local_state()
                   ->FindPreference(proxy_config::prefs::kProxy)
                   ->HasUserSetting())
      << "this section writes the profile pref only";
}

// ---- slice 3 (continued): who wins -----------------------------------------
// Each of these starts with OUR value already stored and pointing at a
// DIFFERENT proxy, so precedence is observed rather than assumed.

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
    ASSERT_TRUE(base::test::RunUntil([&]() {
      return prefs()->FindPreference(proxy_config::prefs::kProxy)->IsManaged();
    }));
  }

  testing::NiceMock<policy::MockConfigurationPolicyProvider> policy_provider_;
};

IN_PROC_BROWSER_TEST_F(RoamuxProxyPolicyTest,
                       PolicyOutranksOurValueAndWeRefuseToWrite) {
  auto handler = MakeHandler(browser()->profile());
  ASSERT_TRUE(handler->SetForTesting(ProxyFields(user_proxy_)).empty());
  ASSERT_EQ(kUserProxyMarker, NavigateAndReadBody(OriginUrl()));

  SetPolicyProxy(HostPort(proxy_));
  const base::DictValue state = handler->GetStateForTesting();
  EXPECT_EQ("policy", Str(state, "baseOwner"));
  EXPECT_FALSE(Flag(state, "canEdit", true));
  EXPECT_TRUE(Flag(state, "canReset", false));
  EXPECT_TRUE(Flag(state, "hasOurValue", false))
      << "our value is still stored; it simply does not win";
  EXPECT_EQ(kProxyMarker, NavigateAndReadBody(OriginUrl()))
      << "policy's proxy must outrank the value we stored";

  const base::DictValue error =
      handler->SetForTesting(ManualFields("other.example", "9999"));
  ASSERT_FALSE(error.empty());
  EXPECT_EQ("policy", Str(error, "reason"));

  // Reset is still reachable: it removes OUR value, and the label must keep
  // naming policy rather than claiming the system configuration.
  EXPECT_TRUE(handler->ResetForTesting().empty());
  EXPECT_FALSE(HasUserProxyValue(browser()->profile()));
  EXPECT_EQ("policy", Str(handler->GetStateForTesting(), "baseOwner"));
  EXPECT_EQ(kProxyMarker, NavigateAndReadBody(OriginUrl()));
}

// An extension-set value likewise wins, and the section names the extension.
IN_PROC_BROWSER_TEST_F(RoamuxProxyRoutingTest,
                       ExtensionOutranksOurValueAndIsNamed) {
  constexpr char kExtensionId[] = "abcdefghijklmnopabcdefghijklmnop";
  auto handler = MakeHandler(browser()->profile());
  ASSERT_TRUE(handler->SetForTesting(ProxyFields(user_proxy_)).empty());
  ASSERT_EQ(kUserProxyMarker, NavigateAndReadBody(OriginUrl()));

  ExtensionPrefValueMap* map =
      ExtensionPrefValueMapFactory::GetForBrowserContext(browser()->profile());
  ASSERT_TRUE(map);
  map->RegisterExtension(kExtensionId, base::Time::Now(), /*is_enabled=*/true,
                         /*is_incognito_enabled=*/false);
  map->SetExtensionPref(kExtensionId, proxy_config::prefs::kProxy,
                        ExtensionPrefValueMap::ChromeSettingScope::kRegular,
                        base::Value(ProxyConfigDictionary::CreateFixedServers(
                            HostPort(proxy_), std::string())));

  const base::DictValue state = handler->GetStateForTesting();
  EXPECT_EQ("extension", Str(state, "baseOwner"));
  EXPECT_EQ(kExtensionId, Str(state, "controllerName"))
      << "with no registry entry the id is the honest answer";
  EXPECT_FALSE(Flag(state, "canEdit", true));
  EXPECT_EQ("extension",
            Str(handler->SetForTesting(ManualFields("other.example", "9999")),
                "reason"));
  EXPECT_EQ(kProxyMarker, NavigateAndReadBody(OriginUrl()))
      << "the extension's proxy must outrank the value we stored";
}

// Proxy authentication is upstream's 407 -> LoginHandler path; we build no
// credential store.
class RoamuxProxyAuthTest : public RoamuxProxyHandlerTest {
 protected:
  void SetUpOnMainThread() override {
    RoamuxProxyHandlerTest::SetUpOnMainThread();
    net::test_server::RegisterProxyBasicAuthHandler(auth_proxy_, "user",
                                                    "pass");
    auth_proxy_.RegisterRequestHandler(
        base::BindRepeating(&ServeProxyMarkerWithPath));
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
  // The needle includes the path, so the document still on screen from the
  // previous navigation can never satisfy it.
  bool BodyEventuallyShowsPath(const std::string& path) {
    const std::string needle = std::string(kProxyMarker) + " ";
    return base::test::RunUntil([&]() {
      const std::string body = BodyOfActiveTab();
      return body.find(needle) != std::string::npos &&
             body.find(path) != std::string::npos;
    });
  }

  void Navigate(content::WebContents* tab, const std::string& path) {
    tab->GetController().LoadURL(GURL("http://routed.example" + path),
                                 content::Referrer(), ui::PAGE_TRANSITION_TYPED,
                                 std::string());
  }

  net::test_server::EmbeddedTestServer auth_proxy_;
};

// What this establishes: the prompt is upstream's, cancelling keeps the request
// out, the supplied credential gets it through, the credential is offered to
// and stored by the password manager under the proxy's realm, and a later
// request in the same session is not challenged again.
//
// What it deliberately does NOT claim: that the password manager would FILL a
// challenge in a later browser session. The reuse below is the network layer's
// HTTP auth cache; filling from the store is upstream's HttpAuthManager path
// (and it prefills the dialog rather than submitting it), so proving it would
// take a fixture that survives a restart. Not established here.
IN_PROC_BROWSER_TEST_F(RoamuxProxyAuthTest,
                       PromptCancelRetryThenSavedAndReusedInSession) {
  // 1. The prompt appears (the LoginHandler path, not something we built).
  content::WebContents* tab =
      browser()->tab_strip_model()->GetActiveWebContents();
  Navigate(tab, "/first");
  ASSERT_TRUE(WaitForPrompt());

  // 2. Cancel: no marker, because the proxy never let the request through.
  LoginHandler::GetAllLoginHandlersForTest().front()->CancelAuth(
      /*notify_others=*/true);
  // Cancelling leaves the 407 error page committed, so the success-checking
  // wait would report failure for the expected outcome.
  content::WaitForLoadStopWithoutSuccessCheck(tab);
  EXPECT_EQ(std::string::npos, BodyOfActiveTab().find(kProxyMarker));

  // 3. Retry with the right credentials: the page loads through the proxy.
  Navigate(tab, "/first");
  ASSERT_TRUE(WaitForPrompt());
  LoginHandler::GetAllLoginHandlersForTest().front()->SetAuth(u"user", u"pass");
  ASSERT_TRUE(BodyEventuallyShowsPath("/first"))
      << "the supplied credentials must get the request through";

  // 4. Saved: Chromium asks before storing, so the test accepts that prompt
  // exactly as a user would — we store nothing ourselves.
  BubbleObserver bubble(tab);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return bubble.IsSavePromptShownAutomatically();
  })) << "the 407 credential must be offered to the password manager";
  bubble.AcceptSavePrompt();
  // The realm is the proxy's, spelled the way login_handler.cc builds it
  // (host:port/realm — the test proxy's realm is "TestServer"), and both halves
  // of the credential have to be there for a later fill to be possible at all.
  const std::string expected_realm =
      "127.0.0.1:" + base::NumberToString(auth_proxy_.port()) + "/TestServer";
  const auto credential_stored = [&]() {
    password_manager::PasswordStoreResultsObserver results;
    ProfilePasswordStoreFactory::GetForProfile(
        browser()->profile(), ServiceAccessType::EXPLICIT_ACCESS)
        ->GetAllLogins(results.GetWeakPtr());
    for (const password_manager::PasswordForm& form :
         results.WaitForResults()) {
      if (form.signon_realm == expected_realm &&
          form.username_value == u"user" && form.password_value == u"pass") {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(base::test::RunUntil(credential_stored))
      << "the 407 credential belongs to the password manager, under "
      << expected_realm;

  // 5. Reused in this session: a different path is fetched without a second
  // challenge (the network layer's auth cache — see the note above).
  Navigate(tab, "/again");
  ASSERT_TRUE(BodyEventuallyShowsPath("/again"));
  EXPECT_TRUE(LoginHandler::GetAllLoginHandlersForTest().empty())
      << "the credential must be reused, not re-prompted";
}

// ---- slice 4: the real Settings DOM ---------------------------------------
// Handler tests cannot catch a missing mount, a wrong message name, a control
// that never shows the saved value, an unrendered field error or an unreachable
// Reset button — so these drive chrome://settings/system itself through the
// same controls a user touches (the roam-277 settings-DOM precedent). Nothing
// here calls a test-only method on the element: the component has none.

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

// Runs `body` inside the section's own shadow root, with these bound:
//   $(id)      -> a control of the section
//   text(id)   -> its rendered text, trimmed
//   settle()   -> yields a few frames, for a binding or a round trip to land
//   waitUntil(fn) -> polls fn for up to 10s
constexpr char kInSectionScript[] = R"(
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
      let section = null;
      for (let i = 0; i < 100 && !section; i++) {
        section = deepQuery(document, 'roamuxProxySection');
        if (!section) await new Promise(r => setTimeout(r, 100));
      }
      if (!section) return 'missing-section';
      const $ = id => section.shadowRoot.querySelector('#' + id);
      const text = id => { const el = $(id); return el ? el.textContent.trim() : ''; };
      const settle = async () => {
        for (let i = 0; i < 5; i++) await new Promise(r => setTimeout(r, 50));
      };
      const waitUntil = async (fn) => {
        for (let i = 0; i < 100; i++) {
          if (fn()) return true;
          await new Promise(r => setTimeout(r, 100));
        }
        return false;
      };
      // The section renders once the browser has answered roamuxProxyGetState.
      if (!await waitUntil(() => !!text('roamuxProxyOwner'))) {
        return 'no-state';
      }
      %s
    })();
)";

std::string WaitForIdScript(const std::string& id, int attempts) {
  return base::StringPrintf(kWaitForIdScript, attempts, id.c_str());
}

std::string InSectionScript(const std::string& body) {
  return base::StringPrintf(kInSectionScript, body.c_str());
}

// Chooses a mode in the real selector, exactly as a user would.
constexpr char kChooseMode[] = R"(
      const mode = $('roamuxProxyMode');
      mode.value = '%s';
      mode.dispatchEvent(new Event('change'));
      await settle();
)";

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
    EXPECT_EQ(true,
              content::EvalJs(web_contents, WaitForIdScript(kSectionId, 100)));
    return web_contents;
  }

  bool WaitForUserProxyValue(bool present) {
    return base::test::RunUntil([&]() {
      return prefs()
                 ->FindPreference(proxy_config::prefs::kProxy)
                 ->HasUserSetting() == present;
    });
  }

  base::DictValue StoredConfig() {
    return prefs()->GetDict(proxy_config::prefs::kProxy).Clone();
  }
};

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       SectionIsPresentWithTheFlagOn) {
  EXPECT_EQ(true, content::EvalJs(NavigateToSystemSettings(),
                                  WaitForIdScript(kSectionId, 100)));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       ManualModeStoresWhatTheControlsSay) {
  content::WebContents* web_contents = NavigateToSystemSettings();
  const std::string body = base::StringPrintf(kChooseMode, "manual") + R"(
      if ($('roamuxProxyManualFields').hidden) return 'manual-fields-hidden';
      if (!$('roamuxProxyPacFields').hidden) return 'pac-fields-shown';
      $('roamuxProxyScheme').value = 'https';
      $('roamuxProxyScheme').dispatchEvent(new Event('change'));
      $('roamuxProxyHost').value = 'ui.example';
      $('roamuxProxyPort').value = '8080';
      $('roamuxProxyBypass').value = '*.corp.example';
      $('roamuxProxyApply').click();
      if (!await waitUntil(() => $('roamuxProxyHost').value === 'ui.example' &&
                                 !$('roamuxProxyHost').invalid)) {
        return 'not-applied';
      }
      return 'ok';
  )";
  ASSERT_EQ("ok", content::EvalJs(web_contents, InSectionScript(body)));
  ASSERT_TRUE(WaitForUserProxyValue(true));
  const ProxyConfigDictionary stored(StoredConfig());
  std::string servers;
  ASSERT_TRUE(stored.GetProxyServer(&servers));
  EXPECT_EQ("https://ui.example:8080", servers)
      << "the scheme the user chose has to reach the stored endpoint";
  std::string bypass;
  EXPECT_TRUE(stored.GetBypassList(&bypass));
  // The stored list is net::ProxyHostMatchingRules::ToString(), which emits one
  // ";"-terminated entry each — that canonical form is what we keep, so what is
  // shown and what is stored can never disagree.
  EXPECT_EQ("*.corp.example;", bypass);
}

// The gap a state-only test cannot see: a saved configuration has to come BACK
// into the controls, or Apply silently discards the fields the user did not
// retype.
IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       SavedConfigurationReappearsInTheControls) {
  ASSERT_TRUE(
      prefs()->FindPreference(proxy_config::prefs::kProxy)->IsUserModifiable());
  base::DictValue fields = ManualFields("saved.example", "3128");
  fields.Set("scheme", "socks5");
  fields.Set("bypassList", "*.internal.example");
  ASSERT_TRUE(MakeHandler(browser()->profile())->SetForTesting(fields).empty());

  content::WebContents* web_contents = NavigateToSystemSettings();
  const std::string body = R"(
      if (!await waitUntil(() => $('roamuxProxyMode').value === 'manual')) {
        return 'mode: ' + $('roamuxProxyMode').value;
      }
      const shown = [$('roamuxProxyScheme').value, $('roamuxProxyHost').value,
                     $('roamuxProxyPort').value, $('roamuxProxyBypass').value];
      // The bypass control shows the canonical stored form (ToString() terminates
      // each entry with ";").
      if (shown.join('|') !== 'socks5|saved.example|3128|*.internal.example;') {
        return 'shown: ' + shown.join('|');
      }
      // Apply again, touching nothing: the stored configuration must survive.
      $('roamuxProxyApply').click();
      await settle();
      return 'ok';
  )";
  EXPECT_EQ("ok", content::EvalJs(web_contents, InSectionScript(body)));
  const ProxyConfigDictionary stored(StoredConfig());
  std::string servers;
  ASSERT_TRUE(stored.GetProxyServer(&servers));
  EXPECT_EQ("socks5://saved.example:3128", servers);
  std::string bypass;
  EXPECT_TRUE(stored.GetBypassList(&bypass));
  EXPECT_EQ("*.internal.example;", bypass)
      << "re-applying an untouched form must not drop the bypass list";
}

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       PacModeShowsItsFieldsAndStoresTheMandatoryChoice) {
  content::WebContents* web_contents = NavigateToSystemSettings();
  const std::string body = base::StringPrintf(kChooseMode, "pac") + R"(
      if ($('roamuxProxyPacFields').hidden) return 'pac-fields-hidden';
      if (!$('roamuxProxyManualFields').hidden) return 'manual-fields-shown';
      if (!$('roamuxProxyPacMandatory').checked) return 'new-pac-should-default-mandatory';
      $('roamuxProxyPacUrl').value = 'https://pac.example/proxy.pac';
      $('roamuxProxyPacMandatory').checked = false;
      $('roamuxProxyApply').click();
      if (!await waitUntil(() => $('roamuxProxyPacUrl').value ===
                                 'https://pac.example/proxy.pac' &&
                                 !$('roamuxProxyPacUrl').invalid)) {
        return 'not-applied';
      }
      // The choice comes back from the browser, not from the page's default.
      if ($('roamuxProxyPacMandatory').checked) return 'mandatory-came-back-true';
      return 'ok';
  )";
  ASSERT_EQ("ok", content::EvalJs(web_contents, InSectionScript(body)));
  ASSERT_TRUE(WaitForUserProxyValue(true));
  const ProxyConfigDictionary stored(StoredConfig());
  std::string pac_url;
  ASSERT_TRUE(stored.GetPacUrl(&pac_url));
  EXPECT_EQ("https://pac.example/proxy.pac", pac_url);
  bool mandatory = true;
  EXPECT_TRUE(stored.GetPacMandatory(&mandatory));
  EXPECT_FALSE(mandatory);
}

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       DirectModeCommitsWithoutApply) {
  content::WebContents* web_contents = NavigateToSystemSettings();
  const std::string body = base::StringPrintf(kChooseMode, "direct") + R"(
      if (!$('roamuxProxyManualFields').hidden) return 'manual-fields-shown';
      return 'ok';
  )";
  ASSERT_EQ("ok", content::EvalJs(web_contents, InSectionScript(body)));
  ASSERT_TRUE(WaitForUserProxyValue(true));
  ProxyPrefs::ProxyMode mode = ProxyPrefs::MODE_SYSTEM;
  ASSERT_TRUE(ProxyConfigDictionary(StoredConfig()).GetMode(&mode));
  EXPECT_EQ(ProxyPrefs::MODE_DIRECT, mode)
      << "a mode that needs no other field commits on selection";
}

// The error has to be readable copy, on the field it belongs to — not a token.
IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       InvalidInputExplainsItselfOnTheFieldAndWritesNothing) {
  content::WebContents* web_contents = NavigateToSystemSettings();
  const std::string body = base::StringPrintf(kChooseMode, "manual") + R"(
      $('roamuxProxyHost').value = 'user:pass@ui.example';
      $('roamuxProxyPort').value = '8080';
      $('roamuxProxyApply').click();
      if (!await waitUntil(() => $('roamuxProxyHost').invalid)) {
        return 'host-not-marked-invalid';
      }
      if ($('roamuxProxyPort').invalid) return 'wrong-field-marked';
      return $('roamuxProxyHost').errorMessage;
  )";
  const std::string message =
      content::EvalJs(web_contents, InSectionScript(body)).ExtractString();
  EXPECT_NE(std::string::npos, message.find("username and password"))
      << "the field must explain the problem, not print a token; got: "
      << message;
  EXPECT_EQ(std::string::npos, message.find("userinfo")) << message;
  EXPECT_FALSE(
      prefs()->FindPreference(proxy_config::prefs::kProxy)->HasUserSetting());
}

// A rejected bypass entry names the entry, and keeps it as context rather than
// as the whole explanation.
IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       ARejectedBypassEntryIsNamedInTheFieldError) {
  content::WebContents* web_contents = NavigateToSystemSettings();
  const std::string body = base::StringPrintf(kChooseMode, "manual") + R"(
      $('roamuxProxyHost').value = 'ui.example';
      $('roamuxProxyPort').value = '8080';
      $('roamuxProxyBypass').value = 'ok.example,10.0.0.0/999';
      $('roamuxProxyApply').click();
      if (!await waitUntil(() => $('roamuxProxyBypass').invalid)) {
        return 'bypass-not-marked-invalid';
      }
      return $('roamuxProxyBypass').errorMessage;
  )";
  const std::string message =
      content::EvalJs(web_contents, InSectionScript(body)).ExtractString();
  EXPECT_NE(std::string::npos, message.find("10.0.0.0/999")) << message;
  EXPECT_NE(std::string::npos, message.find("*.example.com")) << message;
  EXPECT_FALSE(
      prefs()->FindPreference(proxy_config::prefs::kProxy)->HasUserSetting());
}

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       ResetButtonIsReachableAndClears) {
  ASSERT_TRUE(MakeHandler(browser()->profile())
                  ->SetForTesting(ManualFields("ui.example", "8080"))
                  .empty());
  content::WebContents* web_contents = NavigateToSystemSettings();
  const std::string body = R"(
      if ($('roamuxProxyReset').disabled) return 'reset-disabled';
      $('roamuxProxyReset').click();
      if (!await waitUntil(() => $('roamuxProxyMode').value === 'system')) {
        return 'mode: ' + $('roamuxProxyMode').value;
      }
      return 'ok';
  )";
  ASSERT_EQ("ok", content::EvalJs(web_contents, InSectionScript(body)));
  EXPECT_TRUE(WaitForUserProxyValue(false));
}

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       TheLabelFollowsAnExternalChange) {
  content::WebContents* web_contents = NavigateToSystemSettings();
  prefs()->Set(proxy_config::prefs::kProxy,
               base::Value(ProxyConfigDictionary::CreateFixedServers(
                   "external.example:3128", std::string())));
  const std::string body = R"(
      if (!await waitUntil(() => $('roamuxProxyHost').value === 'external.example')) {
        return 'host: ' + $('roamuxProxyHost').value;
      }
      return text('roamuxProxyOwner');
  )";
  const std::string owner =
      content::EvalJs(web_contents, InSectionScript(body)).ExtractString();
  EXPECT_NE(std::string::npos, owner.find("set here"))
      << "the section must not show stale attribution; got: " << owner;
}

// Under policy the controls are unusable but Reset stays reachable, and the
// label names the organization rather than the system.
class RoamuxProxySectionPolicyDomTest : public RoamuxProxySectionDomTest {
 public:
  void SetUpInProcessBrowserTestFixture() override {
    RoamuxProxySectionDomTest::SetUpInProcessBrowserTestFixture();
    policy_provider_.SetDefaultReturns(
        /*is_initialization_complete_return=*/true,
        /*is_first_policy_load_complete_return=*/true);
    policy::BrowserPolicyConnectorBase::SetPolicyProviderForTesting(
        &policy_provider_);
  }

 protected:
  testing::NiceMock<policy::MockConfigurationPolicyProvider> policy_provider_;
};

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionPolicyDomTest,
                       PolicyDisablesTheControlsButNotReset) {
  policy::PolicyMap policies;
  policies.Set(policy::key::kProxyMode, policy::POLICY_LEVEL_MANDATORY,
               policy::POLICY_SCOPE_USER, policy::POLICY_SOURCE_CLOUD,
               base::Value("fixed_servers"), nullptr);
  policies.Set(policy::key::kProxyServer, policy::POLICY_LEVEL_MANDATORY,
               policy::POLICY_SCOPE_USER, policy::POLICY_SOURCE_CLOUD,
               base::Value("policy.example:3128"), nullptr);
  policy_provider_.UpdateChromePolicy(policies);
  ASSERT_TRUE(base::test::RunUntil([&]() {
    return prefs()->FindPreference(proxy_config::prefs::kProxy)->IsManaged();
  }));

  content::WebContents* web_contents = NavigateToSystemSettings();
  const std::string body = R"(
      if (!await waitUntil(() => $('roamuxProxyMode').disabled)) {
        return 'mode-selector-still-enabled';
      }
      if (!$('roamuxProxyApply').disabled) return 'apply-still-enabled';
      if ($('roamuxProxyReset').disabled) return 'reset-disabled';
      if ($('roamuxProxyHost').value !== 'policy.example') {
        return 'host: ' + $('roamuxProxyHost').value;
      }
      return text('roamuxProxyOwner');
  )";
  const std::string owner =
      content::EvalJs(web_contents, InSectionScript(body)).ExtractString();
  EXPECT_NE(std::string::npos, owner.find("organization"))
      << "policy control must be named, not hidden; got: " << owner;
}

// An extension controlling the value is named in the label, from the DOM.
IN_PROC_BROWSER_TEST_F(RoamuxProxySectionDomTest,
                       ExtensionControlIsNamedInTheLabel) {
  constexpr char kExtensionId[] = "abcdefghijklmnopabcdefghijklmnop";
  ExtensionPrefValueMap* map =
      ExtensionPrefValueMapFactory::GetForBrowserContext(browser()->profile());
  ASSERT_TRUE(map);
  map->RegisterExtension(kExtensionId, base::Time::Now(), /*is_enabled=*/true,
                         /*is_incognito_enabled=*/false);
  map->SetExtensionPref(kExtensionId, proxy_config::prefs::kProxy,
                        ExtensionPrefValueMap::ChromeSettingScope::kRegular,
                        base::Value(ProxyConfigDictionary::CreateFixedServers(
                            "ext.example:3128", std::string())));

  content::WebContents* web_contents = NavigateToSystemSettings();
  const std::string body = R"(
      if (!await waitUntil(() => $('roamuxProxyApply').disabled)) {
        return 'apply-still-enabled';
      }
      return text('roamuxProxyOwner');
  )";
  const std::string owner =
      content::EvalJs(web_contents, InSectionScript(body)).ExtractString();
  EXPECT_NE(std::string::npos, owner.find(kExtensionId))
      << "the extension in control must be named; got: " << owner;
}

// Configured-but-ignored override rules must not be presented as if they were
// shaping routing (the engine's own gate is pinned off here).
class RoamuxProxySectionRulesIgnoredDomTest : public RoamuxProxySectionDomTest {
 public:
  RoamuxProxySectionRulesIgnoredDomTest() {
    override_rules_.InitAndDisableFeature(kEnableProxyOverrideRules);
  }

 protected:
  base::test::ScopedFeatureList override_rules_;
};

IN_PROC_BROWSER_TEST_F(RoamuxProxySectionRulesIgnoredDomTest,
                       IgnoredOverrideRulesSayTheyAreNotApplied) {
  base::ListValue rules;
  base::DictValue rule;
  base::ListValue matchers;
  matchers.Append("https://app.corp.example");
  base::ListValue proxies;
  proxies.Append("HTTPS rules.example:443");
  rule.Set(proxy_config::kKeyDestinationMatchers, std::move(matchers));
  rule.Set(proxy_config::kKeyProxyList, std::move(proxies));
  rules.Append(std::move(rule));
  prefs()->Set(proxy_config::prefs::kProxyOverrideRules,
               base::Value(std::move(rules)));

  content::WebContents* web_contents = NavigateToSystemSettings();
  const std::string body = R"(
      if (!await waitUntil(() => !$('roamuxProxyOverrideRules').hidden)) {
        return 'rules-row-hidden';
      }
      return text('roamuxProxyOverrideRules');
  )";
  const std::string rules_text =
      content::EvalJs(web_contents, InSectionScript(body)).ExtractString();
  EXPECT_NE(std::string::npos, rules_text.find("not being applied"))
      << "a list the engine ignores must not read as in effect; got: "
      << rules_text;
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
