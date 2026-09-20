// SPDX-License-Identifier: Apache-2.0
// roam-323: validate raw proxy fields from the Settings > System section and
// build the upstream `proxy` dictionary (ProxyConfigDictionary) — or report
// which field was wrong and why. Nothing here touches a PrefService: the
// handler commits (or clears) what Build() returns, so an invalid configuration
// can never be written even partially.

#ifndef ROAMUX_BROWSER_PROXY_PROXY_CONFIG_BUILDER_H_
#define ROAMUX_BROWSER_PROXY_PROXY_CONFIG_BUILDER_H_

#include <optional>
#include <string>

#include "base/values.h"

namespace roamux::proxy {

enum class Mode { kSystem, kDirect, kManual, kPac };

// The fields exactly as the user typed them; validation is this component's
// job.
struct Input {
  Input();
  Input(const Input&);
  Input& operator=(const Input&);
  ~Input();

  Mode mode = Mode::kSystem;
  std::string scheme;  // "http" | "https" | "socks5" (kManual only)
  std::string host;
  std::string port;
  std::string bypass_list;
  std::string pac_url;
  bool pac_mandatory = true;
};

// Either a complete dictionary to store, an instruction to clear the pref
// (kSystem), or a field-scoped rejection. `field` names the UI field; `reason`
// is a stable token the page maps to copy ("userinfo", "socks5_no_auth",
// "list_syntax", "not_an_endpoint", "host", "port", "scheme", "bypass",
// "pac_scheme", "pac_data_url", "pac_url").
struct Outcome {
  Outcome();
  Outcome(Outcome&&);
  Outcome& operator=(Outcome&&);
  ~Outcome();

  bool ok = false;
  std::string field;
  std::string reason;
  std::optional<base::DictValue> dict;
  bool clear_pref = false;
};

Outcome Build(const Input& input);

}  // namespace roamux::proxy

#endif  // ROAMUX_BROWSER_PROXY_PROXY_CONFIG_BUILDER_H_
