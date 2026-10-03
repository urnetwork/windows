// Executable spec for the update checker's release decision
// (Common/ReleaseSelection.h) and its feed (App/Config.h kUpdateRepo): which
// repo is polled, which release is offered, and which asset is downloaded -
// run against the SAME headers the app compiles, on any host with a C++20
// compiler, with the tag and asset names .github/workflows/beta-build.yml
// actually publishes to the beta fork's releases.
//
//   c++ -std=c++20 -I ../src/Common -I ../src/App update-release-tests.cpp -o /tmp/update-release-tests && /tmp/update-release-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "Config.h"
#include "ReleaseSelection.h"

using namespace urnw::update;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

void CheckEq(const std::string& expected, const std::string& actual, const std::string& what) {
  Check(expected == actual, what + ": expected \"" + expected + "\", got \"" + actual + "\"");
}

std::string Digest(char c) { return "sha256:" + std::string(64, c); }

ReleaseAsset Asset(std::string name, char digest = 'a') {
  return {name,
          "https://github.com/Ryanmello07/urnetwork-windows/releases/download/x/" + name,
          Digest(digest)};
}

// One beta build as beta-build.yml publishes it: a GitHub PRERELEASE tagged
// v<version>-beta, the portable zips and the per-arch MSIs attached, the MSI
// names carrying the version (with its -beta marker) per the checker's grammar.
Release Beta(const std::string& version) {  // version carries its -beta
  return {"v" + version,
          false,
          true,
          {Asset("URnetwork-v" + version + "-windows-x64-portable.zip"),
           Asset("URnetwork-v" + version + "-windows-arm64-portable.zip"),
           Asset("URnetwork-" + version + "-x64.msi", 'b'),
           Asset("URnetwork-" + version + "-arm64.msi", 'c')}};
}

// The F-Droid reproducible-build prerelease the nightly repo mints at
// code+2 / code+3: a prerelease that is NOT this channel's, which the
// channel-aware skip must still refuse even when it outranks the beta.
Release AndroidPrerelease(const std::string& version) {
  return {"v" + version,
          false,
          true,
          {Asset("com.bringyour.network-" + version + "-github-arm64-v8a-release.apk")}};
}

// A plain-tagged, non-prerelease release — none exist on the beta fork today,
// but the upstream handoff is exactly this shape, and only the prerelease
// skip is channel-aware: this must stay offerable.
Release Official(const std::string& version) {
  return {"v" + version,
          false,
          false,
          {Asset("URnetwork-" + version + "-x64.msi", 'b'),
           Asset("URnetwork-" + version + "-arm64.msi", 'c')}};
}

}  // namespace

int main() {
  // ---- the feed: the beta fork's releases — the fork IS the beta channel,
  // not the stable urnetwork/windows feed, not the nightly build repo ----
  const std::wstring_view repo(urnw::config::kUpdateRepo);
  Check(repo == L"Ryanmello07/urnetwork-windows",
        "the update checker polls the beta fork Ryanmello07/urnetwork-windows");
  Check(repo != L"urnetwork/windows",
        "urnetwork/windows is the STABLE feed — the fork is the beta channel");
  Check(repo != L"urnetwork/build",
        "urnetwork/build holds nightly builds, not the beta feed");

  // ---- the beta marker the channel-aware skip keys on (VersionGrammar.h) ----
  Check(urnw::version::IsBetaTag("v2026.9.22-1053244730-beta"),
        "the beta tag carries the marker");
  Check(!urnw::version::IsBetaTag("v2026.9.22-1053244730"),
        "a plain tag is not beta");
  Check(!urnw::version::IsBetaTag("v2026.9.22-1053244730-beta2"),
        "-beta2 is not the marker");

  // ---- asset names: the version keeps its -beta marker ----
  CheckEq("URnetwork-2026.8.28-1031763440-beta-x64.msi",
          InstallerAssetName("2026.8.28-1031763440-beta", "x64"), "x64 MSI name");
  CheckEq("URnetwork-2026.8.28-1031763440-beta-arm64.msi",
          InstallerAssetName("2026.8.28-1031763440-beta", "arm64"), "arm64 MSI name");

  // ---- a real release list, newest first as the API returns it ----
  const std::vector<Release> releases = {
      AndroidPrerelease("2026.9.22-1053244733"),
      AndroidPrerelease("2026.9.22-1053244732"),
      Beta("2026.9.22-1053244730-beta"),
      AndroidPrerelease("2026.8.28-1031763443"),
      Beta("2026.8.28-1031763440-beta"),
  };
  {
    const Selection s = SelectRelease(releases, "x64");
    Check(s.code == 1053244730,
          "offers the newest beta release, not another channel's prerelease");
    CheckEq("2026.9.22-1053244730-beta", s.version, "offered version is v-less");
    CheckEq("v2026.9.22-1053244730-beta", s.tag, "offered tag keeps its v");
    CheckEq("URnetwork-2026.9.22-1053244730-beta-x64.msi", s.assetName, "own-arch MSI");
    CheckEq("https://github.com/Ryanmello07/urnetwork-windows/releases/download/x/"
            "URnetwork-2026.9.22-1053244730-beta-x64.msi",
            s.assetUrl, "download URL comes from the matched asset");
    CheckEq(std::string(64, 'b'), s.digestHex, "digest comes from the matched asset");
    Check(s.newestCode == 1053244730, "newest ignores other channels' prereleases");
    CheckEq("2026.9.22-1053244730-beta", s.newestVersion, "newest version");
  }
  {
    const Selection s = SelectRelease(releases, "arm64");
    CheckEq("URnetwork-2026.9.22-1053244730-beta-arm64.msi", s.assetName,
            "arm64 picks its own MSI");
    CheckEq(std::string(64, 'c'), s.digestHex, "arm64 digest");
  }

  // ---- what is not offered ----
  {
    Release draft = Beta("2026.10.1-1060000000-beta");
    draft.draft = true;
    Release noMsi = Beta("2026.9.30-1059000000-beta");
    noMsi.assets = {Asset("URnetwork-v2026.9.30-1059000000-beta-windows-x64-portable.zip")};
    Release badDigest = Beta("2026.9.29-1058000000-beta");
    for (auto& a : badDigest.assets) a.digest = "sha512:" + std::string(64, 'b');
    // The fork's first betas attached the MSI unversioned; that shape matches
    // no grammar and must be skipped, never offered.
    Release unversionedMsi = Beta("2026.9.28-1057000000-beta");
    unversionedMsi.assets = {Asset("URnetwork.msi")};
    const Selection s = SelectRelease(
        {draft, noMsi, badDigest, unversionedMsi, Beta("2026.9.22-1053244730-beta")},
        "x64");
    Check(s.code == 1053244730,
          "drafts, MSI-less, digest-less and unversioned-MSI releases are skipped");
    Check(s.newestCode == 1059000000, "newest names the newest parsed non-draft release");
    Check(s.skipped.size() == 3, "the three unverifiable releases are reported as skipped");
  }

  // ---- the handoff path: only the PRERELEASE skip is channel-aware, so a
  // plain-tagged non-prerelease on this feed is still offerable and still
  // ranks purely by code ----
  {
    const Selection s =
        SelectRelease({Official("2026.9.25-1056000000"), Beta("2026.9.22-1053244730-beta")},
                      "x64");
    Check(s.code == 1056000000, "a non-prerelease plain release remains offerable");
    Check(s.newestCode == 1056000000, "and newest");
  }

  {
    const Selection s = SelectRelease({}, "x64");
    Check(s.code == 0 && s.newestCode == 0 && s.assetUrl.empty(), "empty list offers nothing");
    const Selection t = SelectRelease({{"latest", false, false, {}}}, "x64");
    Check(t.code == 0 && t.newestCode == 0, "a tag outside the grammar is ignored");
  }

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " update-release-tests: " << gCases
            << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
