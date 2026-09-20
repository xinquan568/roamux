// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/ui/webui/roamux_proxy_handler.h"

#include <string>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/pref_proxy_config_tracker_impl.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/proxy_config/proxy_prefs.h"
#include "components/proxy_config/proxy_prefs_utils.h"
#include "extensions/browser/extension_pref_value_map.h"
#include "extensions/browser/extension_pref_value_map_factory.h"
#include "extensions/browser/extension_registry.h"
#include "net/base/proxy_server.h"
#include "net/base/proxy_string_util.h"
#include "net/proxy_resolution/proxy_host_matching_rules.h"
#include "roamux/browser/proxy/proxy_config_builder.h"

namespace roamux {

namespace {

// The page's mode tokens; they are the builder's Mode in string form.
constexpr char kModeSystem[] = "system";
constexpr char kModeDirect[] = "direct";
constexpr char kModeManual[] = "manual";
constexpr char kModePac[] = "pac";

std::string ModeToken(ProxyPrefs::ProxyMode mode) {
  switch (mode) {
    case ProxyPrefs::MODE_DIRECT:
      return kModeDirect;
    case ProxyPrefs::MODE_FIXED_SERVERS:
      return kModeManual;
    case ProxyPrefs::MODE_PAC_SCRIPT:
      return kModePac;
    case ProxyPrefs::MODE_AUTO_DETECT:
      // Not offered by this section (WPAD is a non-goal); report it honestly so
      // the page can say the configuration came from elsewhere.
      return "auto_detect";
    case ProxyPrefs::MODE_SYSTEM:
    case ProxyPrefs::kModeCount:
      return kModeSystem;
  }
  return kModeSystem;
}

base::DictValue Error(std::string field, std::string reason) {
  base::DictValue error;
  error.Set("field", std::move(field));
  error.Set("reason", std::move(reason));
  return error;
}

std::string StringField(const base::DictValue& fields, std::string_view key) {
  const std::string* value = fields.FindString(key);
  return value ? *value : std::string();
}

proxy::Input ToInput(const base::DictValue& fields) {
  proxy::Input input;
  const std::string mode = StringField(fields, "mode");
  if (mode == kModeDirect) {
    input.mode = proxy::Mode::kDirect;
  } else if (mode == kModeManual) {
    input.mode = proxy::Mode::kManual;
  } else if (mode == kModePac) {
    input.mode = proxy::Mode::kPac;
  } else {
    input.mode = proxy::Mode::kSystem;
  }
  input.scheme = StringField(fields, "scheme");
  input.host = StringField(fields, "host");
  input.port = StringField(fields, "port");
  input.bypass_list = StringField(fields, "bypassList");
  input.pac_url = StringField(fields, "pacUrl");
  input.pac_mandatory = fields.FindBool("pacMandatory").value_or(true);
  return input;
}

}  // namespace

RoamuxProxyHandler::RoamuxProxyHandler(Profile* profile) : profile_(profile) {}
RoamuxProxyHandler::~RoamuxProxyHandler() = default;

void RoamuxProxyHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "roamuxProxyGetState",
      base::BindRepeating(&RoamuxProxyHandler::HandleGetState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "roamuxProxySet", base::BindRepeating(&RoamuxProxyHandler::HandleSet,
                                            base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "roamuxProxyReset", base::BindRepeating(&RoamuxProxyHandler::HandleReset,
                                              base::Unretained(this)));
}

void RoamuxProxyHandler::OnJavascriptAllowed() {
  // Attribution has to stay live: the base value and the override rules can
  // change under the open page, and so can the prefs that decide whether those
  // rules are eligible at all.
  pref_change_registrar_.Init(profile_->GetPrefs());
  const char* const watched[] = {
      proxy_config::prefs::kProxy,
      proxy_config::prefs::kProxyOverrideRules,
      proxy_config::prefs::kEnableProxyOverrideRulesForAllUsers,
      proxy_config::prefs::kProxyOverrideRulesScope,
      proxy_config::prefs::kProxyOverrideRulesAffiliation,
  };
  for (const char* pref : watched) {
    if (profile_->GetPrefs()->FindPreference(pref)) {
      pref_change_registrar_.Add(
          pref, base::BindRepeating(&RoamuxProxyHandler::OnProxyPrefsChanged,
                                    base::Unretained(this)));
    }
  }
}

void RoamuxProxyHandler::OnJavascriptDisallowed() {
  pref_change_registrar_.RemoveAll();
}

base::DictValue RoamuxProxyHandler::GetStateForTesting() {
  return BuildState();
}
base::DictValue RoamuxProxyHandler::SetForTesting(
    const base::DictValue& fields) {
  return Commit(fields);
}
base::DictValue RoamuxProxyHandler::ResetForTesting() {
  return ResetToSystem();
}

void RoamuxProxyHandler::HandleGetState(const base::ListValue& args) {
  AllowJavascript();
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  ResolveJavascriptCallback(args[0], BuildState());
}

void RoamuxProxyHandler::HandleSet(const base::ListValue& args) {
  AllowJavascript();
  if (args.size() < 2u || !args[0].is_string() || !args[1].is_dict()) {
    return;
  }
  ResolveJavascriptCallback(args[0], Commit(args[1].GetDict()));
}

void RoamuxProxyHandler::HandleReset(const base::ListValue& args) {
  AllowJavascript();
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  ResolveJavascriptCallback(args[0], ResetToSystem());
}

base::DictValue RoamuxProxyHandler::BuildState() {
  PrefService* prefs = profile_->GetPrefs();
  const PrefService::Preference* pref =
      prefs->FindPreference(proxy_config::prefs::kProxy);
  const ProxyConfigDictionary dict(pref->GetValue()->GetDict().Clone());

  base::DictValue state;
  ProxyPrefs::ProxyMode mode = ProxyPrefs::MODE_SYSTEM;
  dict.GetMode(&mode);
  state.Set("mode", ModeToken(mode));

  std::string servers;
  if (dict.GetProxyServer(&servers) && !servers.empty()) {
    const net::ProxyServer server =
        net::ProxyUriToProxyServer(servers, net::ProxyServer::SCHEME_HTTP);
    if (server.is_valid()) {
      state.Set(
          "scheme",
          server.scheme() == net::ProxyServer::SCHEME_HTTPS
              ? "https"
              : (server.scheme() == net::ProxyServer::SCHEME_SOCKS5 ? "socks5"
                                                                    : "http"));
      state.Set("host", server.GetHost());
      state.Set("port", base::NumberToString(server.GetPort()));
    }
  }
  std::string bypass;
  if (dict.GetBypassList(&bypass)) {
    state.Set("bypassList", bypass);
  }
  std::string pac_url;
  if (dict.GetPacUrl(&pac_url)) {
    state.Set("pacUrl", pac_url);
  }
  bool pac_mandatory = true;
  if (dict.GetPacMandatory(&pac_mandatory)) {
    state.Set("pacMandatory", pac_mandatory);
  }

  // Who actually controls the BASE configuration. Order matters: the layers
  // above us win, a recommended value that no user value shadows is upstream's
  // CONFIG_FALLBACK case, and our own claim is a user-set value in a mode other
  // than system.
  std::string base_owner = kModeSystem;
  std::string controller_name;
  if (pref->IsManaged()) {
    base_owner = "policy";
  } else if (pref->IsExtensionControlled()) {
    base_owner = "extension";
    ExtensionPrefValueMap* map =
        ExtensionPrefValueMapFactory::GetForBrowserContext(profile_);
    const std::string extension_id =
        map ? map->GetExtensionControllingPref(proxy_config::prefs::kProxy)
            : std::string();
    if (!extension_id.empty()) {
      extensions::ExtensionRegistry* registry =
          extensions::ExtensionRegistry::Get(profile_);
      const extensions::Extension* extension =
          registry ? registry->enabled_extensions().GetByID(extension_id)
                   : nullptr;
      controller_name = extension ? extension->name() : extension_id;
    }
    if (profile_->IsOffTheRecord()) {
      // The regular profile is what the map answers for, so say so rather than
      // implying we resolved the incognito layer.
      state.Set("extensionScope", "regular_profile");
    }
  } else if (!pref->HasUserSetting() && pref->GetRecommendedValue()) {
    base_owner = "recommended";
  } else if (pref->HasUserSetting() && mode != ProxyPrefs::MODE_SYSTEM) {
    base_owner = "roamux";
  }
  state.Set("baseOwner", base_owner);
  state.Set("controllerName", controller_name);

  // Override rules: configured is not the same as in effect. The engine ignores
  // the list unless its feature is on, the policy-affiliation rules allow it,
  // and a rule parses.
  const base::ListValue& rules =
      prefs->GetList(proxy_config::prefs::kProxyOverrideRules);
  const bool configured = !rules.empty();
  bool any_rule_parses = false;
  for (const base::Value& rule : rules) {
    if (rule.is_dict() && rule.GetDict().FindString("proxy")) {
      any_rule_parses = true;
      break;
    }
  }
  const bool eligible =
      base::FeatureList::IsEnabled(kEnableProxyOverrideRules) &&
      proxy_config::ProxyOverrideRulesAllowed(prefs);
  state.Set("overrideRulesConfigured", configured);
  state.Set("overrideRulesActive", configured && eligible && any_rule_parses);
  const PrefService::Preference* rules_pref =
      prefs->FindPreference(proxy_config::prefs::kProxyOverrideRules);
  state.Set(
      "overrideRulesOwner",
      rules_pref && rules_pref->IsManaged()
          ? "policy"
          : (rules_pref && rules_pref->IsExtensionControlled() ? "extension"
                                                               : "user"));

  state.Set("canEdit", pref->IsUserModifiable() && !profile_->IsOffTheRecord());
  // Design §7: Reset stays reachable in a regular profile even when a layer
  // above us controls routing (it removes OUR value); with nothing set it is
  // simply a no-op.
  state.Set("canReset", !profile_->IsOffTheRecord());
  state.Set("hasOurValue", pref->HasUserSetting());
  return state;
}

base::DictValue RoamuxProxyHandler::Commit(const base::DictValue& fields) {
  // Guards live here, not in the page: the testing seams and the message paths
  // share this implementation, so neither can skip them.
  if (profile_->IsOffTheRecord()) {
    return Error("mode", "incognito");
  }
  PrefService* prefs = profile_->GetPrefs();
  const PrefService::Preference* pref =
      prefs->FindPreference(proxy_config::prefs::kProxy);
  if (!pref->IsUserModifiable()) {
    return Error("mode", pref->IsManaged() ? "policy" : "extension");
  }

  proxy::Outcome outcome = proxy::Build(ToInput(fields));
  if (!outcome.ok) {
    return Error(outcome.field, outcome.reason);
  }
  if (outcome.clear_pref) {
    prefs->ClearPref(proxy_config::prefs::kProxy);
  } else {
    prefs->Set(proxy_config::prefs::kProxy,
               base::Value(std::move(*outcome.dict)));
  }
  return base::DictValue();
}

base::DictValue RoamuxProxyHandler::ResetToSystem() {
  if (profile_->IsOffTheRecord()) {
    return Error("mode", "incognito");
  }
  // Reset stays available even when a layer above us controls routing: it
  // removes OUR value, and the state then reports whatever still controls it.
  profile_->GetPrefs()->ClearPref(proxy_config::prefs::kProxy);
  return base::DictValue();
}

void RoamuxProxyHandler::OnProxyPrefsChanged() {
  if (IsJavascriptAllowed()) {
    FireWebUIListener("roamux-proxy-state-changed", BuildState());
  }
}

}  // namespace roamux
