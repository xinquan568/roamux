// SPDX-License-Identifier: Apache-2.0
// roam-323: what happens to routing when our value goes away. A browsertest
// cannot install a distinguishable base (OS) configuration — the only
// in-process switch, --proxy-server, lands ABOVE the user layer instead
// (RoamuxProxyCommandLineProxyTest establishes that) — but the resolution
// itself is a pure function, so the case is testable here: with a base
// configuration present, our pref wins while it is set, and clearing it (mode
// "system", or Reset) hands routing back to that base rather than to a direct
// connection.

#include <string>

#include "components/prefs/testing_pref_service.h"
#include "components/proxy_config/pref_proxy_config_tracker_impl.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "net/base/proxy_server.h"
#include "net/base/proxy_string_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_service.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "roamux/browser/proxy/proxy_config_builder.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux::proxy {
namespace {

constexpr char kOurEndpoint[] = "ourproxy.example:8080";
constexpr char kBaseEndpoint[] = "baseproxy.example:3128";

// A base configuration that is distinguishable from ours, standing in for the
// macOS system configuration.
net::ProxyConfigWithAnnotation BaseConfig() {
  net::ProxyConfig config;
  config.proxy_rules().ParseFromString(kBaseEndpoint);
  return net::ProxyConfigWithAnnotation(config, TRAFFIC_ANNOTATION_FOR_TESTS);
}

std::string SingleProxyOf(const net::ProxyConfigWithAnnotation& config) {
  const net::ProxyList& list = config.value().proxy_rules().single_proxies;
  if (list.IsEmpty() || !list.First().is_single_proxy()) {
    return "<not a single endpoint>";
  }
  return net::ProxyServerToProxyUri(list.First().First());
}

class ProxyPrefVsBaseTest : public testing::Test {
 protected:
  ProxyPrefVsBaseTest() {
    PrefProxyConfigTrackerImpl::RegisterProfilePrefs(prefs_.registry());
  }

  // The effective configuration a network context would see, given a base
  // configuration that IS available — the assertion a browsertest cannot make.
  net::ProxyConfigWithAnnotation Effective(ProxyPrefs::ConfigState* state_out) {
    net::ProxyConfigWithAnnotation pref_config;
    const ProxyPrefs::ConfigState pref_state =
        PrefProxyConfigTrackerImpl::ReadPrefConfig(&prefs_, &pref_config);
    net::ProxyConfigWithAnnotation effective;
    PrefProxyConfigTrackerImpl::GetEffectiveProxyConfig(
        pref_state, pref_config, net::ProxyConfigService::CONFIG_VALID,
        BaseConfig(), /*ignore_fallback_config=*/false, state_out, &effective);
    return effective;
  }

  // Commits through the real builder, so what is stored is what the section
  // would store.
  void CommitManual(const std::string& host, const std::string& port) {
    Input input;
    input.mode = Mode::kManual;
    input.scheme = "http";
    input.host = host;
    input.port = port;
    Outcome outcome = Build(input);
    ASSERT_TRUE(outcome.ok) << outcome.field << "/" << outcome.reason;
    ASSERT_TRUE(outcome.dict.has_value());
    prefs_.SetUserPref(proxy_config::prefs::kProxy,
                       base::Value(std::move(*outcome.dict)));
  }

  void CommitSystem() {
    Input input;
    input.mode = Mode::kSystem;
    const Outcome outcome = Build(input);
    ASSERT_TRUE(outcome.ok);
    ASSERT_TRUE(outcome.clear_pref)
        << "mode system must clear, never store a value";
    prefs_.RemoveUserPref(proxy_config::prefs::kProxy);
  }

  TestingPrefServiceSimple prefs_;
};

TEST_F(ProxyPrefVsBaseTest, WithNoValueOfOursTheBaseConfigurationRoutes) {
  ProxyPrefs::ConfigState state = ProxyPrefs::CONFIG_UNSET;
  EXPECT_EQ(kBaseEndpoint, SingleProxyOf(Effective(&state)));
  EXPECT_EQ(ProxyPrefs::CONFIG_SYSTEM, state);
}

TEST_F(ProxyPrefVsBaseTest, OurValueOutranksTheBaseConfiguration) {
  ASSERT_NO_FATAL_FAILURE(CommitManual("ourproxy.example", "8080"));
  ProxyPrefs::ConfigState state = ProxyPrefs::CONFIG_UNSET;
  EXPECT_EQ(kOurEndpoint, SingleProxyOf(Effective(&state)));
  EXPECT_NE(ProxyPrefs::CONFIG_SYSTEM, state);
}

// The claim the issue actually makes about mode "system": routing goes back to
// whatever the machine says — NOT to a direct connection, which is all a
// browsertest with no base configuration can observe.
TEST_F(ProxyPrefVsBaseTest, SystemModeRestoresTheBaseConfiguration) {
  ASSERT_NO_FATAL_FAILURE(CommitManual("ourproxy.example", "8080"));
  ProxyPrefs::ConfigState state = ProxyPrefs::CONFIG_UNSET;
  ASSERT_EQ(kOurEndpoint, SingleProxyOf(Effective(&state)));

  ASSERT_NO_FATAL_FAILURE(CommitSystem());
  EXPECT_EQ(kBaseEndpoint, SingleProxyOf(Effective(&state)))
      << "clearing our value must hand routing back to the base configuration";
  EXPECT_EQ(ProxyPrefs::CONFIG_SYSTEM, state);
  EXPECT_FALSE(prefs_.GetUserPref(proxy_config::prefs::kProxy))
      << "and it must do so by removing the value, not by storing one";
}

// Direct is a choice of ours, and it must NOT be what "system" degrades into.
TEST_F(ProxyPrefVsBaseTest, DirectIsOursAndOutranksTheBaseConfigurationToo) {
  Input input;
  input.mode = Mode::kDirect;
  Outcome outcome = Build(input);
  ASSERT_TRUE(outcome.ok);
  prefs_.SetUserPref(proxy_config::prefs::kProxy,
                     base::Value(std::move(*outcome.dict)));
  ProxyPrefs::ConfigState state = ProxyPrefs::CONFIG_UNSET;
  const net::ProxyConfigWithAnnotation effective = Effective(&state);
  EXPECT_TRUE(effective.value().proxy_rules().empty())
      << "no proxy is what the user asked for";
  EXPECT_NE(ProxyPrefs::CONFIG_SYSTEM, state);
}

}  // namespace
}  // namespace roamux::proxy
