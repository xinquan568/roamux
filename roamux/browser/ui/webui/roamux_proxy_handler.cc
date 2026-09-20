// SPDX-License-Identifier: Apache-2.0
#include "roamux/browser/ui/webui/roamux_proxy_handler.h"

#include <optional>
#include <string>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/pref_proxy_config_tracker_impl.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/proxy_config/proxy_prefs.h"
#include "extensions/browser/extension_pref_value_map.h"
#include "extensions/browser/extension_pref_value_map_factory.h"
#include "extensions/browser/extension_registry.h"
#include "net/base/proxy_server.h"
#include "net/base/proxy_string_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
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

// A mode we do not recognise is a malformed message, not a request for the
// system configuration: defaulting it to kSystem would let a misspelling (or a
// fields dictionary with no mode at all) silently clear a saved proxy.
std::optional<proxy::Input> ToInput(const base::DictValue& fields) {
  proxy::Input input;
  const std::string mode = StringField(fields, "mode");
  if (mode == kModeSystem) {
    input.mode = proxy::Mode::kSystem;
  } else if (mode == kModeDirect) {
    input.mode = proxy::Mode::kDirect;
  } else if (mode == kModeManual) {
    input.mode = proxy::Mode::kManual;
  } else if (mode == kModePac) {
    input.mode = proxy::Mode::kPac;
  } else {
    return std::nullopt;
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
  if (args.empty() || !args[0].is_string()) {
    return;
  }
  // A malformed payload still gets an answer: leaving the page's promise
  // pending would strand the section with no error and no state.
  if (args.size() < 2u || !args[1].is_dict()) {
    ResolveJavascriptCallback(args[0], Error("mode", "mode"));
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
  // Report the value the network layer will act on, not the presence of the
  // key: ProxyConfigDictionary::GetPacMandatory() (and the conversion to
  // net::ProxyConfig) treat an absent key as false, so an existing PAC
  // configuration without it permits fallback and must not be shown as
  // blocking. (Creating a new PAC configuration still defaults to mandatory —
  // that default lives in the page, not here.)
  if (mode == ProxyPrefs::MODE_PAC_SCRIPT) {
    bool pac_mandatory = false;
    dict.GetPacMandatory(&pac_mandatory);
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

  // Override rules: configured is not the same as in effect. Whether any rule
  // is actually applied depends on the engine's feature, the policy-affiliation
  // rules, and each rule's schema ("DestinationMatchers"/"ProxyList"/optional
  // "Conditions") — so ask the engine's own reader rather than re-implementing
  // its parser here, which is the only way this cannot drift from it.
  state.Set("overrideRulesConfigured",
            !prefs->GetList(proxy_config::prefs::kProxyOverrideRules).empty());
  net::ProxyConfigWithAnnotation engine_config;
  PrefProxyConfigTrackerImpl::ReadPrefConfig(prefs, &engine_config);
  state.Set("overrideRulesActive",
            !engine_config.value().proxy_override_rules().empty());
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

  const std::optional<proxy::Input> input = ToInput(fields);
  if (!input.has_value()) {
    return Error("mode", "mode");
  }
  proxy::Outcome outcome = proxy::Build(*input);
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
