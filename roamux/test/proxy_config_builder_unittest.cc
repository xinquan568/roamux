// SPDX-License-Identifier: Apache-2.0
// roam-323 slice 1: the pure validator/builder. Every rejection names a field
// and a stable reason token, and carries NO dictionary — an invalid
// configuration can never be written even partially. The APIs under test are
// the pinned ones: ProxyServer canonicalisation, ProxyHostMatchingRules
// per-entry validation, and the PAC fetcher's scheme + data-URL rules.

#include "roamux/browser/proxy/proxy_config_builder.h"

#include <string>

#include "components/proxy_config/proxy_config_dictionary.h"
#include "net/base/proxy_server.h"
#include "net/base/proxy_string_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux::proxy {
namespace {

Input ManualInput(const std::string& scheme,
                  const std::string& host,
                  const std::string& port,
                  const std::string& bypass = "") {
  Input input;
  input.mode = Mode::kManual;
  input.scheme = scheme;
  input.host = host;
  input.port = port;
  input.bypass_list = bypass;
  return input;
}

Input PacInput(const std::string& url, bool mandatory = true) {
  Input input;
  input.mode = Mode::kPac;
  input.pac_url = url;
  input.pac_mandatory = mandatory;
  return input;
}

// The stored dictionary, read back through upstream's own accessor.
ProxyConfigDictionary AsDict(const Outcome& outcome) {
  return ProxyConfigDictionary(outcome.dict->Clone());
}

TEST(ProxyConfigBuilderTest, SystemClearsThePrefAndStoresNothing) {
  Input input;
  input.mode = Mode::kSystem;
  const Outcome outcome = Build(input);
  EXPECT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.clear_pref);
  EXPECT_FALSE(outcome.dict.has_value());
}

TEST(ProxyConfigBuilderTest, DirectStoresTheDirectDictionary) {
  Input input;
  input.mode = Mode::kDirect;
  const Outcome outcome = Build(input);
  ASSERT_TRUE(outcome.ok) << outcome.field << "/" << outcome.reason;
  EXPECT_FALSE(outcome.clear_pref);
  ProxyPrefs::ProxyMode mode;
  ASSERT_TRUE(AsDict(outcome).GetMode(&mode));
  EXPECT_EQ(ProxyPrefs::MODE_DIRECT, mode);
}

TEST(ProxyConfigBuilderTest, ManualStoresTheCanonicalEndpoint) {
  const Outcome outcome = Build(ManualInput("http", "proxy.example", "8080"));
  ASSERT_TRUE(outcome.ok) << outcome.field << "/" << outcome.reason;
  std::string servers;
  ASSERT_TRUE(AsDict(outcome).GetProxyServer(&servers));
  // Upstream's canonical form leaves off "http://" because http is its default
  // proxy scheme (net/base/proxy_string_util.cc:176), so this is what Chromium
  // itself writes.
  EXPECT_EQ("proxy.example:8080", servers);
  // Stronger than the string: it must read back as the same endpoint.
  EXPECT_EQ(net::ProxyServer::FromSchemeHostAndPort(
                net::ProxyServer::SCHEME_HTTP, "proxy.example", uint16_t{8080}),
            net::ProxyUriToProxyServer(servers, net::ProxyServer::SCHEME_HTTP));
}

TEST(ProxyConfigBuilderTest, ManualKeepsAnExplicitSchemeForNonHttp) {
  for (const char* scheme : {"https", "socks5"}) {
    const Outcome outcome = Build(ManualInput(scheme, "proxy.example", "1080"));
    ASSERT_TRUE(outcome.ok) << scheme;
    std::string servers;
    ASSERT_TRUE(AsDict(outcome).GetProxyServer(&servers));
    EXPECT_EQ(std::string(scheme) + "://proxy.example:1080", servers) << scheme;
  }
}

TEST(ProxyConfigBuilderTest, ManualCanonicalisesBracketedIpv6) {
  const Outcome outcome = Build(ManualInput("http", "[::1]", "3128"));
  ASSERT_TRUE(outcome.ok) << outcome.field << "/" << outcome.reason;
  std::string servers;
  ASSERT_TRUE(AsDict(outcome).GetProxyServer(&servers));
  EXPECT_EQ("[::1]:3128", servers)
      << "IPv6 must stay bracketed in the stored URI";
}

// A bare literal is accepted and canonicalised: the port is a separate field
// here, so `::1` is not ambiguous, and net::ProxyServer::FromSchemeHostAndPort
// brackets it itself. Rejecting it would refuse a valid endpoint the engine
// accepts.
TEST(ProxyConfigBuilderTest, ManualAcceptsUnbracketedIpv6AndBracketsIt) {
  const Outcome outcome = Build(ManualInput("http", "::1", "3128"));
  ASSERT_TRUE(outcome.ok) << outcome.field << "/" << outcome.reason;
  std::string servers;
  ASSERT_TRUE(AsDict(outcome).GetProxyServer(&servers));
  EXPECT_EQ("[::1]:3128", servers)
      << "the stored URI is the canonical bracketed form either way";
}

TEST(ProxyConfigBuilderTest, ManualAcceptsAFullIpv6Literal) {
  for (const char* host : {"2001:db8::1", "[2001:db8::1]"}) {
    const Outcome outcome = Build(ManualInput("http", host, "3128"));
    ASSERT_TRUE(outcome.ok)
        << host << ": " << outcome.field << "/" << outcome.reason;
    std::string servers;
    ASSERT_TRUE(AsDict(outcome).GetProxyServer(&servers));
    EXPECT_EQ("[2001:db8::1]:3128", servers) << host;
  }
}

TEST(ProxyConfigBuilderTest, ManualRejectsUserinfo) {
  const Outcome outcome =
      Build(ManualInput("http", "user:pass@proxy.example", "8080"));
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ("host", outcome.field);
  EXPECT_EQ("userinfo", outcome.reason);
  EXPECT_FALSE(outcome.dict.has_value());
}

TEST(ProxyConfigBuilderTest, Socks5CredentialsReportTheNoAuthReasonFirst) {
  const Outcome outcome =
      Build(ManualInput("socks5", "user:pass@proxy.example", "1080"));
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ("socks5_no_auth", outcome.reason)
      << "SOCKS5 has no authentication at all";
}

TEST(ProxyConfigBuilderTest, ManualRejectsListAndMappingSyntax) {
  for (const char* host :
       {"a.example,b.example", "a.example;b.example", "http=a.example"}) {
    const Outcome outcome = Build(ManualInput("http", host, "8080"));
    EXPECT_FALSE(outcome.ok) << host;
    EXPECT_EQ("list_syntax", outcome.reason) << host;
  }
}

TEST(ProxyConfigBuilderTest, ManualRejectsPercentEncodedListSyntax) {
  const Outcome outcome =
      Build(ManualInput("http", "a.example%2Cb.example", "8080"));
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ("list_syntax", outcome.reason);
}

TEST(ProxyConfigBuilderTest, ManualRejectsPathQueryFragmentAndSpace) {
  // The reason is asserted per case because the list-syntax check comes first,
  // and a query string carrying "=" is caught there — a correct rejection under
  // a different name.
  const struct {
    const char* host;
    const char* reason;
  } kCases[] = {
      {"proxy.example/pac", "not_an_endpoint"},
      {"proxy.example?x", "not_an_endpoint"},
      {"proxy.example#f", "not_an_endpoint"},
      {"proxy example", "not_an_endpoint"},
      {"proxy.example?x=1", "list_syntax"},
  };
  for (const auto& test_case : kCases) {
    const Outcome outcome = Build(ManualInput("http", test_case.host, "8080"));
    EXPECT_FALSE(outcome.ok) << test_case.host;
    EXPECT_EQ("host", outcome.field) << test_case.host;
    EXPECT_EQ(test_case.reason, outcome.reason) << test_case.host;
    EXPECT_FALSE(outcome.dict.has_value()) << test_case.host;
  }
}

// A pasted endpoint can carry trailing whitespace or a newline, which
// FromSchemeHostAndPort trims silently — storing something other than what the
// field showed. The percent-encoded forms matter for the same reason the
// list-syntax check decodes.
TEST(ProxyConfigBuilderTest, ManualRejectsWhitespaceAndControlCharacters) {
  for (const char* host :
       {"proxy.example\n", "proxy.example\r\n", " proxy.example",
        "proxy.example\t", "proxy.example%0A", "proxy.example%20",
        "proxy.exa\x01mple"}) {
    const Outcome outcome = Build(ManualInput("http", host, "8080"));
    EXPECT_FALSE(outcome.ok) << host;
    EXPECT_EQ("not_an_endpoint", outcome.reason) << host;
    EXPECT_FALSE(outcome.dict.has_value()) << host;
  }
}

TEST(ProxyConfigBuilderTest, ManualRejectsBadPorts) {
  for (const char* port : {"0", "65536", "80junk", "", "-1"}) {
    const Outcome outcome = Build(ManualInput("http", "proxy.example", port));
    EXPECT_FALSE(outcome.ok) << port;
    EXPECT_EQ("port", outcome.field) << port;
  }
}

TEST(ProxyConfigBuilderTest, ManualRejectsEmptyHostAndUnknownScheme) {
  EXPECT_EQ("host", Build(ManualInput("http", "", "8080")).field);
  EXPECT_EQ("scheme", Build(ManualInput("ftp", "proxy.example", "8080")).field);
}

TEST(ProxyConfigBuilderTest,
     BypassListValidatesEveryEntryAndStoresTheCanonicalForm) {
  const Outcome outcome = Build(ManualInput(
      "http", "proxy.example", "8080", "*.corp.example, 10.0.0.0/8;localhost"));
  ASSERT_TRUE(outcome.ok) << outcome.field << "/" << outcome.reason;
  std::string bypass;
  ASSERT_TRUE(AsDict(outcome).GetBypassList(&bypass));
  EXPECT_NE(std::string::npos, bypass.find("*.corp.example"));
  EXPECT_NE(std::string::npos, bypass.find("10.0.0.0/8"));
  EXPECT_NE(std::string::npos, bypass.find("localhost"));
}

TEST(ProxyConfigBuilderTest,
     BypassListReportsTheOffendingEntryInsteadOfDroppingIt) {
  const Outcome outcome = Build(ManualInput("http", "proxy.example", "8080",
                                            "*.corp.example,10.0.0.0/999"));
  EXPECT_FALSE(outcome.ok)
      << "a silently dropped entry is exactly what ParseFromString would do";
  EXPECT_EQ("bypass", outcome.field);
  EXPECT_NE(std::string::npos, outcome.reason.find("10.0.0.0/999"));
  EXPECT_FALSE(outcome.dict.has_value());
}

TEST(ProxyConfigBuilderTest, PacStoresUrlAndMandatoryFlagBothWays) {
  for (bool mandatory : {true, false}) {
    const Outcome outcome =
        Build(PacInput("https://pac.example/proxy.pac", mandatory));
    ASSERT_TRUE(outcome.ok) << outcome.field << "/" << outcome.reason;
    const ProxyConfigDictionary dict = AsDict(outcome);
    std::string url;
    ASSERT_TRUE(dict.GetPacUrl(&url));
    EXPECT_EQ("https://pac.example/proxy.pac", url);
    bool stored = !mandatory;
    ASSERT_TRUE(dict.GetPacMandatory(&stored));
    EXPECT_EQ(mandatory, stored);
  }
}

TEST(ProxyConfigBuilderTest, PacAcceptsHttpAndDataButRejectsFile) {
  EXPECT_TRUE(Build(PacInput("http://pac.example/p.pac")).ok);
  EXPECT_TRUE(Build(PacInput("data:application/x-ns-proxy-autoconfig,function "
                             "FindProxyForURL(u,h){return \"DIRECT\";}"))
                  .ok);
  const Outcome file_url = Build(PacInput("file:///tmp/proxy.pac"));
  EXPECT_FALSE(file_url.ok) << "the PAC fetcher allows only http/https/data";
  EXPECT_EQ("pac_scheme", file_url.reason);
}

TEST(ProxyConfigBuilderTest, PacRejectsMalformedDataUrls) {
  for (const char* url :
       {"data:foobar", "data:application/x-ns-proxy-autoconfig;base64,%%%"}) {
    const Outcome outcome = Build(PacInput(url));
    EXPECT_FALSE(outcome.ok) << url;
    EXPECT_EQ("pac_data_url", outcome.reason) << url;
    EXPECT_FALSE(outcome.dict.has_value()) << url;
  }
}

TEST(ProxyConfigBuilderTest, PacRejectsAnInvalidUrl) {
  const Outcome outcome = Build(PacInput("not a url"));
  EXPECT_FALSE(outcome.ok);
  EXPECT_EQ("pac_url", outcome.field);
}

}  // namespace
}  // namespace roamux::proxy
