// Executable spec for the connection region (App/ConnectionRegion.h): the
// picker's rows, the stored value a selection writes, the shared store behind
// the login control and the DNS editor's picker, and the wiring of the
// can't-reach prompt to the sdk's rule (sdk cant_reach.go, tested there).
// Runs on any host with a C++20 compiler, against the app sources and the
// generated English strings, so it needs no SDK build and no Windows:
//
//   c++ -std=c++20 -I ../src/App connection-region-tests.cpp -o /tmp/connection-region-tests && /tmp/connection-region-tests ..
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ConnectionRegion.h"

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

// The network space's stored value, as the sdk keeps it: Set normalizes and
// answers what it stored; an unknown code stores "".
struct FakeSpace {
  std::vector<std::string> known;
  std::string stored;
  int sets = 0;
  std::string GetIngressCountryCode() const { return stored; }
  std::string SetIngressCountryCode(const std::string& code) {
    ++sets;
    stored = std::find(known.begin(), known.end(), code) != known.end() ? code : "";
    return stored;
  }
};

std::wstring NameOf(const std::string& code) {
  if (code == "cn") return L"China";
  if (code == "ir") return L"Iran";
  if (code == "ru") return L"Russia";
  if (code == "tr") return L"Türkiye";
  return {};
}

}  // namespace

int main(int argc, char** argv) {
  const std::string appDir = 1 < argc ? argv[1] : "..";
  namespace region = urnw::connection_region;
  const std::vector<std::string> sdkCodes = {"ru", "cn", "tr", "ir", "kz"};

  Case("the picker lists exactly the sdk regions plus Other / Undisclosed");
  const auto rows = region::PickerRows(sdkCodes, L"Other / Undisclosed", NameOf);
  if (rows.size() != sdkCodes.size() + 1) {
    Fail("row count " + std::to_string(rows.size()));
  } else {
    if (rows[0].code != "" || rows[0].label != L"Other / Undisclosed") {
      Fail("first row is not Other / Undisclosed");
    }
    for (const auto& code : sdkCodes) {
      int seen = 0;
      for (const auto& row : rows) seen += row.code == code ? 1 : 0;
      if (seen != 1) Fail("region " + code + " listed " + std::to_string(seen) + " times");
    }
    // by display name; a code the platform cannot name shows upper case
    const std::vector<std::wstring> labels = {L"China", L"Iran", L"KZ", L"Russia", L"Türkiye"};
    for (size_t i = 0; i < labels.size(); ++i) {
      if (rows[i + 1].label != labels[i]) Fail("row " + std::to_string(i + 1) + " out of order");
    }
  }

  Case("a repeated, upper-case or empty sdk code is listed once or not at all");
  if (region::PickerRows({"CN", "cn", ""}, L"U", NameOf).size() != 2) {
    Fail("duplicates or empties listed");
  }

  Case("a selection stores the row's code; Undisclosed stores \"\"");
  {
    FakeSpace space{sdkCodes, "", 0};
    region::RegionSelection<FakeSpace> picker(space);
    if (picker.Select(rows[region::SelectedRow(rows, "cn")]) != "cn" || space.stored != "cn") {
      Fail("selecting China did not store cn");
    }
    if (picker.Select(rows[0]) != "" || space.stored != "") {
      Fail("selecting Undisclosed did not store \"\"");
    }
    if (region::StoredValueForSelection("IR") != "ir") Fail("stored value not lower case");
  }

  Case("the stored value selects its row; an unlisted one shows Undisclosed");
  if (region::SelectedRow(rows, "") != 0) Fail("\"\" not row 0");
  if (rows[region::SelectedRow(rows, "RU")].code != "ru") Fail("ru not selected");
  if (region::SelectedRow(rows, "zz") != 0) Fail("unlisted code not row 0");

  Case("the login control and the settings picker share the same stored value");
  {
    FakeSpace space{sdkCodes, "", 0};
    region::RegionSelection<FakeSpace> login(space);
    region::RegionSelection<FakeSpace> settings(space);
    login.Select(rows[region::SelectedRow(rows, "tr")]);
    if (settings.Current() != "tr") Fail("settings does not see the login choice");
    settings.Select(rows[0]);
    if (login.Current() != "") Fail("login does not see the settings choice");
  }

  Case("resolver summary: none is the default resolvers");
  {
    region::ResolverSummary summary;
    if (!summary.Defaults()) Fail("empty summary not defaults");
    summary.encrypted = {"https://223.5.5.5/dns-query"};
    if (summary.Defaults()) Fail("summary with servers reads as defaults");
  }

  // Which failures and stalls raise the can't-reach prompt is the sdk's one
  // rule for every app (sdk cant_reach.go IsCantReachError and
  // CantReachOnStall; support inbox 7956, 7973): its tests hold the cases this
  // file kept, with the other apps', whose lists had drifted from these. The
  // wiring below pins that the window and the connect page ask the sdk.

  // ---- wiring: the app sources and the generated strings ----------------
  const std::string src = appDir + "/src/App/";
  const std::string sdkHost = ReadFile(src + "SdkHost.cpp");
  const std::string login = ReadFile(src + "LoginPage.cpp");
  const std::string dns = ReadFile(src + "StatsSheets.cpp");
  const std::string connect = ReadFile(src + "ConnectPage.cpp");
  const std::string window = ReadFile(src + "MainWindow.xaml.cpp");
  const std::string xaml = ReadFile(src + "MainWindow.xaml");
  const std::string block = ReadFile(src + "ConnectionRegionSheet.cpp");
  const std::string project = ReadFile(src + "App.vcxproj");
  const std::string english = ReadFile(src + "Strings/en/Resources.resw");

  Case("SdkHost reads and writes the network space's own value");
  if (!Contains(sdkHost, "networkSpace_->getIngressCountryCode()") ||
      !Contains(sdkHost, "networkSpace_->setIngressCountryCode(")) {
    Fail("SdkHost does not go through NetworkSpace get/setIngressCountryCode");
  }
  for (const char* call : {"urnet::ingressCountryCodes()", "urnet::ingressDohUrls(",
                           "urnet::ingressPlainDnsServers("}) {
    if (!Contains(sdkHost, call)) Fail(std::string("SdkHost does not call ") + call);
  }

  Case("both doors and the prompt use the one picker over SdkHost");
  if (!Contains(block, "RegionSelection<SdkHost>") && !Contains(block, "selection_.Select(")) {
    Fail("the picker does not save through RegionSelection");
  }
  if (!Contains(ReadFile(src + "ConnectionRegionSheet.h"), "RegionSelection<SdkHost>")) {
    Fail("the picker is not RegionSelection<SdkHost>");
  }
  if (!Contains(xaml, "x:Name=\"ConnectingFromLink\"") || !Contains(xaml, "Click=\"OnConnectingFrom\"")) {
    Fail("the login screen has no Connecting from control");
  }
  if (!Contains(window, "ConnectionRegionSheet::Create")) Fail("the window does not open the picker");
  if (!Contains(window, "urnet::isCantReachError(error)")) {
    Fail("the window does not ask the sdk's rule for can't-reach failures");
  }
  if (Contains(window, "IsCantReachFailure")) Fail("the window keeps its own failure list");
  if (!Contains(xaml, "x:Name=\"RegionPromptBar\"")) Fail("no can't-reach prompt bar");
  for (const char* site : {"w_.NoteReachFailure(routing.error)", "w_.NoteReachFailure(error)",
                           "self->NoteReachFailure(r.error)"}) {
    if (!Contains(login, site)) Fail(std::string("login does not report ") + site);
  }
  if (!Contains(dns, "ConnectionRegionBlock::Create")) Fail("DNS editor has no region picker");
  if (!Contains(connect, "urnet::cantReachOnStall(previousStallReason, windowStallReason_)")) {
    Fail("the signed-in stall does not raise the prompt by the sdk's rule");
  }
  if (!Contains(project, "ConnectionRegionSheet.cpp") || !Contains(project, "ConnectionRegion.h")) {
    Fail("App.vcxproj does not build the region sources");
  }

  Case("the DNS editor heads the egress controls Traffic DNS, below the region");
  {
    // the provider picker leads the traffic controls (connect ADBLOCK3RD F2),
    // and the resolver switches sit under its Custom pick
    const size_t regionAt = dns.find("ConnectionRegionBlock::Create");
    const size_t trafficAt = dns.find("Loc(connection_region::kTrafficDnsKey)");
    const size_t pickerAt = dns.find("BuildPicker(body)");
    if (trafficAt == std::string::npos || regionAt == std::string::npos ||
        pickerAt == std::string::npos || !(regionAt < trafficAt && trafficAt < pickerAt)) {
      Fail("region, Traffic DNS heading, DNS picker not in that order");
    }
  }

  Case("the keys exist in the generated English strings");
  for (std::string_view key :
       {region::kTitleKey, region::kDescriptionKey, region::kConnectingFromKey,
        region::kUndisclosedKey, region::kCantReachKey, region::kPickerTitleKey,
        region::kEncryptedResolversKey, region::kDefaultResolversKey,
        region::kFallbackResolversKey, region::kTrafficDnsKey}) {
    if (!Contains(english, "<data name=\"" + std::string(key) + "\"")) {
      Fail("missing string " + std::string(key));
    }
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
