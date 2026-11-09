// Executable spec for the DNS settings: the "Fast DNS on connect" setting
// (App/FastDnsOnConnect.h), the opt-in host-network DNS fallback, off by
// default; and the provider picker's sdk keys (App/DnsPresetPresentation.h,
// connect ADBLOCK3RD). Runs on any host with a C++20 compiler, against a
// stand-in for urnet::DnsResolverSettings, the app sources and the generated
// English strings, so it needs no SDK build and no Windows:
//
//   c++ -std=c++20 -I ../src/App dns-settings-tests.cpp -o /tmp/dns-settings-tests && /tmp/dns-settings-tests ..
//
// The argument is the app directory (default ".."). The checks:
//  - the toggle maps to and from EnableFallback unchanged, and shows off when
//    there are no applied settings to read;
//  - the DNS editor toggle, its description and the drawer status row use the
//    fast_dns_on_connect keys, not the retired "Local DNS fallback" wording;
//  - those keys exist in the generated English strings and the retired ones
//    are gone;
//  - a key the sdk names is shown only when this build knows it, an error id
//    it does not know reads as something_went_wrong, and the sdk's pills map
//    to this app's keys;
//  - every key the picker can show is in the generated English strings.
//
// SPDX-License-Identifier: MPL-2.0

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "DnsPresetPresentation.h"
#include "FastDnsOnConnect.h"
#include "SdkErrorId.h"

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Case(const std::string& name) {
  ++gCases;
  gCurrentCase = name;
}

// the fields of urnet::DnsResolverSettings the mapping touches
struct StandInSettings {
  bool EnableRemoteDoh = false;
  bool EnableFallback = false;
};

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    Fail("cannot read " + path);
    return {};
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string appDir = 1 < argc ? argv[1] : "..";
  namespace fast = urnw::fast_dns_on_connect;

  Case("no applied settings shows the toggle off");
  if (fast::FromSettings(std::optional<StandInSettings>{})) {
    Fail("absent settings read as fast dns on");
  }

  Case("the toggle reads EnableFallback unchanged");
  for (bool enabled : {false, true}) {
    StandInSettings settings;
    settings.EnableRemoteDoh = true;
    settings.EnableFallback = enabled;
    if (fast::FromSettings(std::optional<StandInSettings>{settings}) != enabled) {
      Fail(std::string("EnableFallback=") + (enabled ? "true" : "false") + " read back flipped");
    }
  }

  Case("the toggle writes EnableFallback unchanged");
  for (bool enabled : {false, true}) {
    StandInSettings settings;
    settings.EnableFallback = !enabled;
    fast::ToSettings(settings, enabled);
    if (settings.EnableFallback != enabled) {
      Fail(std::string("toggle ") + (enabled ? "on" : "off") + " not written to EnableFallback");
    }
  }

  Case("the toggle is labeled Fast DNS on connect");
  if (fast::kLabelKey != "fast_dns_on_connect" ||
      fast::kDescriptionKey != "fast_dns_on_connect_description") {
    Fail("unexpected label keys");
  }

  Case("the editor and the drawer use the fast dns keys");
  for (const char* source : {"/src/App/StatsSheets.cpp", "/src/App/ConnectPage.cpp"}) {
    const std::string text = ReadFile(appDir + source);
    if (Contains(text, "\"local_dns_fallback")) {
      Fail(std::string(source) + " still shows the retired Local DNS fallback wording");
    }
    if (!Contains(text, "fast_dns_on_connect::kLabelKey")) {
      Fail(std::string(source) + " does not label the setting Fast DNS on connect");
    }
  }
  if (!Contains(ReadFile(appDir + "/src/App/StatsSheets.cpp"), "fast_dns_on_connect::kDescriptionKey")) {
    Fail("the editor does not describe what Fast DNS on connect reveals");
  }

  Case("the English strings carry the fast dns keys");
  {
    const std::string resw = ReadFile(appDir + "/src/App/Strings/en/Resources.resw");
    if (!Contains(resw, "<data name=\"fast_dns_on_connect\"") ||
        !Contains(resw, "<value>Fast DNS on connect</value>")) {
      Fail("en Resources.resw has no Fast DNS on connect label");
    }
    if (!Contains(resw, "<data name=\"fast_dns_on_connect_description\"") ||
        !Contains(resw, "reveal your lookups to the local network")) {
      Fail("en Resources.resw has no Fast DNS on connect description");
    }
    if (!Contains(resw, "return answers that don't match your exit location")) {
      Fail("the Fast DNS on connect description does not warn that answers may not match the exit location");
    }
    if (Contains(resw, "<data name=\"local_dns_fallback\"")) {
      Fail("en Resources.resw still carries the retired local_dns_fallback key");
    }
  }

  namespace presets = urnw::dnspresets;

  Case("an sdk key this build knows is shown, any other is not");
  if (std::string_view{presets::Known("dns_group_family", presets::kGroupKeys)} !=
      "dns_group_family") {
    Fail("a known group heading is not shown");
  }
  if (std::string_view{presets::Known("block_ads_and_trackers", presets::kGroupKeys)} !=
      "block_ads_and_trackers") {
    Fail("the ad-blocking group's heading is not the existing block_ads_and_trackers");
  }
  // the plan's draft ids, which the store never took, and nothing at all
  for (const char* id :
       {"dns_group_block_ads", "dns_preset_adguard_copy", "dns_status_personal", ""}) {
    if (*presets::Known(id, presets::kGroupKeys) != '\0' ||
        *presets::Known(id, presets::kPickCopyKeys) != '\0' ||
        *presets::Known(id, presets::kStatusKeys) != '\0') {
      Fail(std::string("an id this build does not know is shown: ") + id);
    }
  }
  // a key known for one line is not taken for another's
  if (*presets::Known("dns_status_provider", presets::kSummaryKeys) != '\0') {
    Fail("a status key is shown as a summary");
  }

  Case("a personal id error reads as its own key, anything else as something_went_wrong");
  if (*presets::PersonalIdErrorKey("") != '\0') Fail("a valid id shows an error");
  for (const char* id : presets::kPersonalIdErrorKeys) {
    if (std::string_view{presets::PersonalIdErrorKey(id)} != id) {
      Fail(std::string("the sdk's error id does not read as its own key: ") + id);
    }
  }
  for (const char* id : {urnw::kSdkErrorIdInternal, "dns_preset_error_from_a_newer_sdk"}) {
    if (std::string_view{presets::PersonalIdErrorKey(id)} != "something_went_wrong") {
      Fail(std::string("an error id this build does not know is not something_went_wrong: ") + id);
    }
  }

  Case("the sdk's pills map to this app's keys");
  if (presets::PillOf("") != presets::Pill::None) Fail("no pill shows one");
  if (presets::PillOf("dns_pill_recommended") != presets::Pill::Recommended) {
    Fail("the recommendation pill is not shown");
  }
  // the sdk names android's key for the defaults pill; this app keys the same
  // text dns_pill_default
  if (presets::PillOf("dns_pill_defaults") != presets::Pill::Defaults) {
    Fail("the defaults pill is not shown");
  }
  if (presets::PillOf("dns_pill_from_a_newer_sdk") != presets::Pill::None) {
    Fail("a pill this build does not know is shown");
  }

  Case("the help links are the ur.io faq's DNS providers and personal ids sections");
  if (std::wstring_view{presets::kHelpUrl} != L"https://ur.io/docs/faq#dns-providers") {
    Fail("unexpected help link");
  }
  // the Use my account field's, as android and apple link it
  if (std::wstring_view{presets::kPersonalIdsHelpUrl} !=
      L"https://ur.io/docs/faq#dns-personal-ids") {
    Fail("unexpected personal ids help link");
  }
  if (!Contains(ReadFile(appDir + "/src/App/StatsSheets.cpp"),
                "MakeDnsLink(Loc(\"learn_more\"), hstring{dnspresets::kPersonalIdsHelpUrl})")) {
    Fail("the Use my account field does not link the personal ids help");
  }

  Case("every key the picker can show is in the generated English strings");
  {
    const std::string resw = ReadFile(appDir + "/src/App/Strings/en/Resources.resw");
    auto requireAll = [&resw](const char* what, auto const& keys) {
      for (const char* key : keys) {
        if (!Contains(resw, std::string("<data name=\"") + key + "\"")) {
          Fail(std::string(what) + " key missing from en Resources.resw: " + key);
        }
      }
    };
    requireAll("group", presets::kGroupKeys);
    requireAll("pick label", presets::kPickLabelKeys);
    requireAll("pick copy", presets::kPickCopyKeys);
    requireAll("summary", presets::kSummaryKeys);
    requireAll("status", presets::kStatusKeys);
    requireAll("status detail", presets::kStatusDetailKeys);
    requireAll("verification", presets::kVerificationKeys);
    requireAll("verification link", presets::kVerificationLinkKeys);
    requireAll("personal id where", presets::kPersonalIdWhereKeys);
    requireAll("personal id format", presets::kPersonalIdFormatKeys);
    requireAll("quota", presets::kQuotaNoteKeys);
    requireAll("error", presets::kPersonalIdErrorKeys);
    for (const char* key :
         {presets::kPillRecommendedId, "dns_pill_default", presets::kUnknownErrorKey}) {
      if (!Contains(resw, std::string("<data name=\"") + key + "\"")) {
        Fail(std::string("pill or fallback key missing from en Resources.resw: ") + key);
      }
    }
  }

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " dns settings: " << gCases << " cases, "
            << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
