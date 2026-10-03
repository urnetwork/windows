// Which published release the update checker offers, decided pure.
//
// The feed is the beta fork's GitHub releases (Config.h kUpdateRepo — the fork
// IS the beta channel): every green build of the beta branch publishes a
// PRERELEASE tagged `v<YYYY.M.D>-<code>-beta`, one MSI per architecture
// attached as `URnetwork-<YYYY.M.D>-<code>-beta-<x64|arm64>.msi`
// (.github/workflows/beta-build.yml). The prerelease skip is therefore
// channel-aware rather than blanket: drafts are always skipped, and a
// prerelease is skipped UNLESS its tag carries the beta marker
// (version::IsBetaTag) — a prerelease that outranks the offered release by
// code without being this channel's own (the nightly repo's android-only
// F-Droid variants at code+2 / code+3 are the model) must not make the
// developer line name it as "the newest release".
//
// The checker turns the releases JSON into these plain structs and asks
// SelectRelease; the decision itself (tag grammar, asset name, digest) touches
// no Windows headers, so tools/update-release-tests.cpp runs it on any host
// against the names the beta pipeline actually publishes.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "UpdateFormats.h"
#include "VersionGrammar.h"

namespace urnw::update {

struct ReleaseAsset {
  std::string name;
  std::string url;     // browser_download_url
  std::string digest;  // the API's `sha256:<hex>`, verbatim
};

struct Release {
  std::string tag;  // tag_name, with its v
  bool draft = false;
  bool prerelease = false;
  std::vector<ReleaseAsset> assets;
};

struct Selection {
  // The newest non-draft tag that parses — this channel's beta prereleases
  // included, other prereleases excluded — offerable or not; the developer
  // screen names it either way.
  std::uint64_t newestCode = 0;
  std::string newestVersion;  // v-less

  // The newest release this build can actually install and verify: own-arch
  // MSI attached, carrying a usable sha256 digest. code == 0 means none.
  std::uint64_t code = 0;
  std::string version;  // v-less
  std::string tag;      // as minted, with the v
  std::string assetName;
  std::string assetUrl;
  std::string digestHex;  // lowercase

  // Releases that parsed but could not be offered, with why — for the log.
  struct Skip {
    std::string tag;
    std::string reason;
  };
  std::vector<Skip> skipped;
};

// The MSI asset name the release pipeline uploads for `version` (v-less) on
// `arch` ("x64" or "arm64"): URnetwork-<version>-<arch>.msi.
inline std::string InstallerAssetName(std::string_view version,
                                      std::string_view arch) {
  std::string name = "URnetwork-";
  name.append(version);
  name.push_back('-');
  name.append(arch);
  name.append(".msi");
  return name;
}

inline Selection SelectRelease(std::vector<Release> const& releases,
                               std::string_view arch) {
  Selection s;
  for (auto const& rel : releases) {
    // Drafts are never offered. Prereleases usually aren't either — but this
    // feed IS the beta channel and its releases are prereleases, so the skip
    // keeps only the ones not ours: a prerelease whose tag lacks the beta
    // marker is somebody else's channel and is skipped outright, exactly as
    // before; a beta-marked one falls through to the same grammar, asset and
    // digest gates a stable release would face.
    if (rel.draft || (rel.prerelease && !version::IsBetaTag(rel.tag))) continue;
    const std::uint64_t code = version::ParseReleaseCode(rel.tag);
    if (code == 0) continue;
    std::string ver = rel.tag;
    if (!ver.empty() && ver.front() == 'v') ver.erase(0, 1);
    if (code > s.newestCode) {
      s.newestCode = code;
      s.newestVersion = ver;
    }
    if (code <= s.code) continue;

    const std::string name = InstallerAssetName(ver, arch);
    const ReleaseAsset* match = nullptr;
    for (auto const& asset : rel.assets) {
      if (asset.name == name) match = &asset;
    }
    if (!match || match->url.empty()) {
      s.skipped.push_back({rel.tag, "lacks " + name});
      continue;
    }
    // URL and expected hash from the SAME asset object: the digest is
    // GitHub's own upload-time SHA-256 for exactly the bytes this URL serves.
    std::string digestHex = DigestHexFromAssetDigest(match->digest);
    if (digestHex.empty()) {
      s.skipped.push_back({rel.tag, "lacks a usable digest for " + name});
      continue;
    }
    s.code = code;
    s.version = ver;
    s.tag = rel.tag;
    s.assetName = name;
    s.assetUrl = match->url;
    s.digestHex = std::move(digestHex);
  }
  return s;
}

}  // namespace urnw::update
