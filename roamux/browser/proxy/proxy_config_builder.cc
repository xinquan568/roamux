// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/proxy/proxy_config_builder.h"

#include <string>
#include <string_view>

#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "net/base/data_url.h"
#include "net/base/proxy_server.h"
#include "net/base/proxy_string_util.h"
#include "net/base/scheme_host_port_matcher.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_host_matching_rules.h"
#include "url/gurl.h"

namespace roamux::proxy {

namespace {

Outcome Reject(std::string field, std::string reason) {
  Outcome outcome;
  outcome.field = std::move(field);
  outcome.reason = std::move(reason);
  return outcome;
}

// `,` `;` and `=` are proxy-LIST and per-scheme-MAPPING syntax to the parser
// that will read what we store (ProxyConfig::ProxyRules::ParseFromString), so a
// single-endpoint field must refuse them — before and after percent-decoding,
// since the decoded form is what the host canonicaliser yields.
bool HasListSyntax(std::string_view value) {
  return value.find_first_of(",;=") != std::string_view::npos;
}

bool HasCredentials(std::string_view host) {
  return host.find('@') != std::string_view::npos;
}

std::optional<net::ProxyServer::Scheme> ParseScheme(std::string_view scheme) {
  if (scheme == "http") {
    return net::ProxyServer::SCHEME_HTTP;
  }
  if (scheme == "https") {
    return net::ProxyServer::SCHEME_HTTPS;
  }
  if (scheme == "socks5") {
    return net::ProxyServer::SCHEME_SOCKS5;
  }
  return std::nullopt;
}

// The canonical URI must survive the downstream parser as exactly one endpoint;
// anything else means we would be storing a list or a per-scheme map behind a
// single-endpoint field.
bool SurvivesAsSingleEndpoint(const std::string& uri,
                              const net::ProxyServer& server) {
  net::ProxyConfig::ProxyRules rules;
  rules.ParseFromString(uri);
  return rules.type == net::ProxyConfig::ProxyRules::Type::PROXY_LIST &&
         rules.single_proxies.size() == 1u &&
         rules.single_proxies.First().is_single_proxy() &&
         rules.single_proxies.First().First() == server;
}

Outcome BuildManual(const Input& input) {
  const std::optional<net::ProxyServer::Scheme> scheme =
      ParseScheme(input.scheme);
  if (!scheme.has_value()) {
    return Reject("scheme", "scheme");
  }
  // SOCKS5 has no authentication in Chromium at all, so credentials in a SOCKS5
  // endpoint get that reason rather than the generic one.
  if (HasCredentials(input.host)) {
    return Reject("host", *scheme == net::ProxyServer::SCHEME_SOCKS5
                              ? "socks5_no_auth"
                              : "userinfo");
  }
  const std::string decoded_host = base::UnescapeBinaryURLComponent(input.host);
  if (HasListSyntax(input.host) || HasListSyntax(decoded_host)) {
    return Reject("host", "list_syntax");
  }
  if (input.host.empty()) {
    return Reject("host", "host");
  }
  if (input.host.find_first_of("/?# \t") != std::string::npos ||
      decoded_host.find_first_of("/?# \t") != std::string::npos) {
    return Reject("host", "not_an_endpoint");
  }
  // A bare `::1` is ambiguous (ParseHostAndPort would read it as host + port),
  // so an IPv6 literal must arrive bracketed.
  if (input.host.find(':') != std::string::npos &&
      !(input.host.front() == '[' && input.host.back() == ']')) {
    return Reject("host", "host");
  }
  unsigned port = 0;
  if (input.port.empty() ||
      !base::ContainsOnlyChars(input.port, "0123456789") ||
      !base::StringToUint(input.port, &port) || port == 0u || port > 65535u) {
    return Reject("port", "port");
  }
  const net::ProxyServer server = net::ProxyServer::FromSchemeHostAndPort(
      *scheme, input.host, static_cast<uint16_t>(port));
  if (!server.is_valid()) {
    return Reject("host", "host");
  }
  const std::string uri = net::ProxyServerToProxyUri(server);
  if (!SurvivesAsSingleEndpoint(uri, server)) {
    return Reject("host", "list_syntax");
  }

  // Every bypass entry must be accepted individually:
  // ProxyHostMatchingRules::ParseFromString drops the ones it cannot parse and
  // reports nothing, so the user would never learn that an entry was ignored.
  net::ProxyHostMatchingRules bypass_rules;
  for (const std::string_view entry : base::SplitStringPiece(
           input.bypass_list,
           net::SchemeHostPortMatcher::kParseRuleListDelimiterList,
           base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (!bypass_rules.AddRuleFromString(entry)) {
      return Reject("bypass", std::string(entry));
    }
  }

  Outcome outcome;
  outcome.ok = true;
  outcome.dict =
      ProxyConfigDictionary::CreateFixedServers(uri, bypass_rules.ToString());
  return outcome;
}

Outcome BuildPac(const Input& input) {
  const GURL url(input.pac_url);
  if (!url.is_valid()) {
    return Reject("pac_url", "pac_url");
  }
  // The PAC fetcher accepts only these schemes, so anything else would store a
  // configuration that can never load (pac_file_fetcher_impl.cc).
  const bool is_data = url.SchemeIs(url::kDataScheme);
  if (!url.SchemeIs(url::kHttpScheme) && !url.SchemeIs(url::kHttpsScheme) &&
      !is_data) {
    return Reject("pac_url", "pac_scheme");
  }
  if (is_data) {
    std::string mime_type;
    std::string charset;
    std::string data;
    if (!net::DataURL::Parse(url, &mime_type, &charset, &data)) {
      return Reject("pac_url", "pac_data_url");
    }
  }
  Outcome outcome;
  outcome.ok = true;
  outcome.dict = ProxyConfigDictionary::CreatePacScript(input.pac_url,
                                                        input.pac_mandatory);
  return outcome;
}

}  // namespace

Input::Input() = default;
Input::Input(const Input&) = default;
Input& Input::operator=(const Input&) = default;
Input::~Input() = default;

Outcome::Outcome() = default;
Outcome::Outcome(Outcome&&) = default;
Outcome& Outcome::operator=(Outcome&&) = default;
Outcome::~Outcome() = default;

Outcome Build(const Input& input) {
  switch (input.mode) {
    case Mode::kSystem: {
      // Clearing the user layer is what relinquishes our claim; writing
      // CreateSystem() would leave a user-set value behind (and
      // HasUserSetting() true) forever.
      Outcome outcome;
      outcome.ok = true;
      outcome.clear_pref = true;
      return outcome;
    }
    case Mode::kDirect: {
      Outcome outcome;
      outcome.ok = true;
      outcome.dict = ProxyConfigDictionary::CreateDirect();
      return outcome;
    }
    case Mode::kManual:
      return BuildManual(input);
    case Mode::kPac:
      return BuildPac(input);
  }
}

}  // namespace roamux::proxy
