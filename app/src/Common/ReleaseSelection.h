// Which published release the update checker offers and the elevated update
// helper installs, decided pure.
//
// The feed is one GitHub repository's releases, compiled in (kFeeds) and
// addressed by the repository's numeric id: the API path
// /repositories/<id>/releases is immune to a rename, and to someone else
// registering the owner's old name, both of which redirect or take over an
// owner/name path. The owner and repo are kept only to check the download URL
// a release names (FeedAssetUrl).
//
// A release is a tag `v<YYYY.M.D>-<code>` with one MSI per architecture
// attached as `URnetwork-<YYYY.M.D>-<code>-<x64|arm64>.msi`, the names
// build/all/run.sh mints (require_windows_artifacts + the github_release_upload
// loop). SelectRelease skips, before anything else is read:
//   - drafts;
//   - prereleases, except a `-beta` one on a feed that takes them: a
//     prerelease that outranks the stable release by code (the pipeline's
//     android-only F-Droid variants at code+2 / code+3 are the model) must not
//     be offered or named as the newest release;
//   - releases that are not immutable, on a feed that requires it: an
//     immutable release's assets cannot be replaced after publication, and its
//     tag cannot be reused once it is deleted;
//   - codes whose instant is more than kFutureCodeLimit after the release
//     list's own Date header: one mistyped or hostile far-future code would
//     otherwise outrank every real release for good;
//   - releases without this product's MSI for this architecture;
//   - releases that have not soaked, on a feed that soaks (SoakStartOf,
//     HasSoaked): a build published a moment ago has run nowhere yet, and one
//     that was changed a moment ago is not the release that was out before.
// "Newest" then counts only what is left, so a release of the other platforms
// alone, or one still soaking, is never named as the newest one. The offer
// additionally needs a usable sha256 digest on that asset.
//
// The checker and the helper turn the releases JSON into these plain structs
// (ReleaseJson.h) and ask SelectRelease with the list's own Date header. The
// decision reads no clock and no state of this machine's, so both come to the
// same release from the same list; and it touches no Windows headers, so
// tools/update-release-tests.cpp runs it on any host against the names the
// release pipeline actually publishes.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "UpdateFormats.h"
#include "UpdateResult.h"  // ParseUtcSecond
#include "VersionGrammar.h"

namespace urnw::update {

// One release feed.
struct Feed {
  // The channel's name: "official". The opt-in developer channel joins the
  // table with its own feed.
  std::string_view id;
  // GitHub's repository id, the path of the release list.
  std::uint64_t numericRepoId = 0;
  // How GitHub spells the repository in its download URLs.
  std::string_view owner;
  std::string_view repo;
  // A prerelease whose tag ends in -beta counts; every other prerelease is
  // skipped.
  bool acceptBetaPrereleases = false;
  // Only immutable releases count.
  bool requireImmutable = false;
  // How long a release must have been out, unchanged, when the UTC day of the
  // release list's date began, before it counts (SoakStartOf, HasSoaked). 0: a
  // release counts from the moment it is published.
  std::int64_t soakSeconds = 0;
  // A runner test's feed only, never one of kFeeds: every tag starts with
  // this, and it is stripped before the grammar is read.
  std::string_view tagPrefix;
  // A runner test's feed only: any prerelease counts.
  bool acceptAnyPrerelease = false;
};

// The official releases: urnetwork/build's, never a personal fork. That
// repository is where the release pipeline (build/all/run.sh) publishes its
// builds as immutable releases, the Windows MSIs among their assets; a build
// it published without them is skipped (SelectRelease). The app's own
// repository, urnetwork/windows, publishes none (urnetwork/windows#3 asks for
// them): were it to, this constant's id, owner and repo are all that name the
// feed. The numeric id is the one GitHub assigned the repository. Immutable
// releases are required, as on every official feed.
//
// The pipeline publishes whenever it runs, several builds on some days, and a
// build published a moment ago has run nowhere yet. So a release counts only
// once it has been out, unchanged, for a day, judged at the start of GitHub's
// day. Which releases count therefore changes only when that day does, and
// the newest of them is the same release for every install and for the update
// helper all day. A release that is deleted, marked a prerelease or edited
// stops counting at once, whatever the hour: that is how one is withdrawn.
inline constexpr Feed kOfficialFeed{
    .id = "official",
    .numericRepoId = 936244679,
    .owner = "urnetwork",
    .repo = "build",
    .acceptBetaPrereleases = false,
    .requireImmutable = true,
    .soakSeconds = 24 * 60 * 60,
    .tagPrefix = "",
    .acceptAnyPrerelease = false,
};

// Every feed this build knows. The update channel names one of them.
inline constexpr Feed kFeeds[] = {kOfficialFeed};

// The feed named `id`, or null.
inline const Feed* FeedById(std::string_view id) {
  for (const Feed& feed : kFeeds) {
    if (feed.id == id) return &feed;
  }
  return nullptr;
}

// How many releases one request of a feed's list asks for. The pipeline
// publishes three releases a build (the one with the MSIs and two android
// prereleases), a release that soaks waits up to two days to count, and the
// list must still reach the newest release that does. The most builds the
// pipeline has published within two days is six: 18 releases, and the one
// before them. 30 holds ten builds.
inline constexpr int kReleaseListPageSize = 30;

// The release list of `feed`: its repository by id, which no rename and no
// owner name registered again can move.
inline std::string ReleaseListUrl(const Feed& feed) {
  return "https://api.github.com/repositories/" + std::to_string(feed.numericRepoId) +
         "/releases?per_page=" + std::to_string(kReleaseListPageSize);
}

// How far after the release list's Date header a release code may point: a
// code is minted from the clock of the machine that built the release, and
// two days covers any honest skew between that clock and GitHub's.
inline constexpr std::int64_t kFutureCodeLimitSeconds = 48 * 60 * 60;

// The instant every release code counts from, 2023-05-23T00:00:00Z, in Unix
// seconds. A code is tenths of a second after it.
inline constexpr std::int64_t kCodeEpochUnixSeconds = 1684800000;

// The Unix second a release code names. Codes are at most 18 digits
// (VersionGrammar.h), so this cannot overflow.
inline constexpr std::int64_t CodeUnixSeconds(std::uint64_t code) {
  return kCodeEpochUnixSeconds + static_cast<std::int64_t>(code / 10);
}

inline constexpr std::int64_t kSecondsPerDay = 24 * 60 * 60;

// The first second of the UTC day `unixSeconds` falls in.
inline constexpr std::int64_t UtcDayStart(std::int64_t unixSeconds) {
  std::int64_t intoDay = unixSeconds % kSecondsPerDay;
  if (intoDay < 0) intoDay += kSecondsPerDay;
  return unixSeconds - intoDay;
}

// The second a release's soak is counted from, or why it has none.
struct SoakStart {
  std::optional<std::int64_t> unixSeconds;
  // Why there is none, for the log; empty when there is one.
  std::string_view problem;
};

// When the release of `code` last became what the list shows, by the list's
// own three times, each an ISO 8601 UTC second: the latest of
//   - `publishedAt`, the release's published_at;
//   - `updatedAt`, the release's updated_at, the time GitHub gives for its
//     last change. A published release can still be edited, and marked a
//     prerelease or a release again, immutable or not, and its published_at
//     stays as it was. A release marked a prerelease is skipped here, so the
//     time it spent as one is time it was offered to nobody;
//   - `assetUpdatedAt`, the updated_at of this product's package in it: when
//     those bytes were uploaded.
// None, with why, when any of the three is missing or is not a UTC second,
// and when published_at is more than kFutureCodeLimit before the instant the
// code names. A release cannot be published before it is built, and a time
// that far back (a zeroed one, 1970-01-01T00:00:00Z) would read as a soak
// that ended long ago.
inline SoakStart SoakStartOf(std::uint64_t code, std::string_view publishedAt,
                             std::string_view updatedAt, std::string_view assetUpdatedAt) {
  const std::optional<std::int64_t> published = ParseUtcSecond(publishedAt);
  if (!published) return {std::nullopt, "has no publication time to judge its soak by"};
  if (*published < CodeUnixSeconds(code) - kFutureCodeLimitSeconds) {
    return {std::nullopt, "has a publication time before its own code's"};
  }
  const std::optional<std::int64_t> changed = ParseUtcSecond(updatedAt);
  if (!changed) return {std::nullopt, "has no time of its last change to judge its soak by"};
  const std::optional<std::int64_t> uploaded = ParseUtcSecond(assetUpdatedAt);
  if (!uploaded) return {std::nullopt, "has no upload time of its package to judge its soak by"};
  return {std::max({*published, *changed, *uploaded}), {}};
}

// Whether a release of `feed` counts yet in a release list dated
// `serverUnixSeconds`, its Date header. `sinceUnixSeconds` is the second the
// release's soak is counted from (SoakStartOf), or nullopt when it has none:
// on a feed that soaks, such a release never counts, and no release does
// while the list has no date. Both times are GitHub's; this machine's clock
// takes no part.
//
// The line is drawn at the start of the list's UTC day, not at its second. A
// release whose soak had ended when today began counts all day; one whose
// soak ends today counts from tomorrow. So every list of one day, whoever
// fetches it and when, counts the same releases, unless one of them is
// withdrawn or changed during it. The tray app's check and the update
// helper's own, minutes apart, disagree only then and across midnight UTC.
inline constexpr bool HasSoaked(const Feed& feed, std::optional<std::int64_t> sinceUnixSeconds,
                                std::int64_t serverUnixSeconds) {
  if (feed.soakSeconds <= 0) return true;
  if (!sinceUnixSeconds || serverUnixSeconds <= 0) return false;
  return *sinceUnixSeconds + feed.soakSeconds <= UtcDayStart(serverUnixSeconds);
}

// The first second a release of `feed` counts whose soak is counted from
// `sinceUnixSeconds`: for every later list date HasSoaked holds, and for none
// before.
inline constexpr std::int64_t SoakEndUnixSeconds(const Feed& feed, std::int64_t sinceUnixSeconds) {
  if (feed.soakSeconds <= 0) return sinceUnixSeconds;
  const std::int64_t soaked = sinceUnixSeconds + feed.soakSeconds;
  const std::int64_t day = UtcDayStart(soaked);
  return day == soaked ? soaked : day + kSecondsPerDay;
}

// How long after a check the tray app still takes its offer for what the feed
// offers. A release can be withdrawn at any moment (deleted, marked a
// prerelease, edited), and the banner that offers it may have been up for
// hours.
inline constexpr std::int64_t kOfferFreshSeconds = 5 * 60;

// Whether the tray app should check again before it acts on an offer made
// from a list dated `serverUnixSecondsAtCheck`, `secondsSince` seconds ago by
// this machine's steady clock: before it starts the update helper, and
// before it hands the user an installer to run. It should once the offer is
// older than kOfferFreshSeconds, and, on a feed that soaks, once GitHub's day
// has changed, when a newer release may count. The helper asks GitHub itself
// and would refuse a release that is no longer offered, after the download
// and the administrator prompt; nothing else asks before an installer is
// shown.
inline constexpr bool OfferMayHaveChanged(const Feed& feed, std::int64_t serverUnixSecondsAtCheck,
                                          std::int64_t secondsSince) {
  if (secondsSince < 0 || secondsSince > kOfferFreshSeconds) return true;
  if (feed.soakSeconds <= 0 || serverUnixSecondsAtCheck <= 0) return false;
  return UtcDayStart(serverUnixSecondsAtCheck + secondsSince) !=
         UtcDayStart(serverUnixSecondsAtCheck);
}

struct ReleaseAsset {
  std::string name;
  std::string url;        // browser_download_url
  std::string digest;     // the API's `sha256:<hex>`, verbatim
  std::string updatedAt;  // updated_at, verbatim: 2026-10-01T14:07:24Z
};

struct Release {
  std::string tag;  // tag_name, with its v (and a test feed's prefix)
  bool draft = false;
  bool prerelease = false;
  bool immutable = false;
  std::string publishedAt;  // published_at, verbatim: 2026-10-01T14:51:17Z
  std::string updatedAt;    // updated_at, verbatim
  std::vector<ReleaseAsset> assets;
};

struct Selection {
  // The newest release that counts and carries this product's MSI for this
  // architecture, offerable or not: the developer screen names it either way.
  std::uint64_t newestCode = 0;
  std::string newestVersion;  // v-less

  // The newest release only the feed's soak holds back, and the Unix second
  // it counts from if nothing about it changes before then
  // (SoakEndUnixSeconds): the developer screen names it. Code 0 when the soak
  // holds none back.
  std::uint64_t waitingCode = 0;
  std::string waitingVersion;  // v-less
  std::int64_t waitingFromUnixSeconds = 0;

  // The newest release this build can actually install and verify: own-arch
  // MSI attached, carrying a usable sha256 digest. code == 0 means none.
  std::uint64_t code = 0;
  std::string version;  // v-less
  std::string tag;      // as minted, with the v (and a test feed's prefix)
  std::string assetName;
  std::string assetUrl;
  std::string digestHex;  // lowercase
  // The same release's SHA256SUMS and SHA256SUMS.sig (UpdateSignature.h),
  // empty when it has none.
  std::string sumsUrl;
  std::string sumsSignatureUrl;

  // Releases that parsed but could not be offered, with why, for the log.
  struct Skip {
    std::string tag;
    std::string reason;
    // Only the feed's soak holds it back. That is how a feed that soaks
    // looks on most days, and nothing is wrong with the release.
    bool soaking = false;
  };
  std::vector<Skip> skipped;
};

// The MSI asset name the release pipeline uploads for `version` (v-less) on
// `arch` ("x64" or "arm64"): URnetwork-<version>-<arch>.msi.
inline std::string InstallerAssetName(std::string_view version, std::string_view arch) {
  std::string name = "URnetwork-";
  name.append(version);
  name.push_back('-');
  name.append(arch);
  name.append(".msi");
  return name;
}

// The release a tag names on `feed`: the tag without the feed's prefix and its
// v, and its code; nullopt when the tag is not one of the feed's.
struct FeedTag {
  std::string version;  // v-less
  std::uint64_t code = 0;
};
inline std::optional<FeedTag> ParseFeedTag(const Feed& feed, std::string_view tag) {
  if (tag.substr(0, feed.tagPrefix.size()) != feed.tagPrefix) return std::nullopt;
  tag.remove_prefix(feed.tagPrefix.size());
  const std::uint64_t code = version::ParseReleaseCode(tag);
  if (code == 0) return std::nullopt;
  if (!tag.empty() && tag.front() == 'v') tag.remove_prefix(1);
  return FeedTag{.version = std::string(tag), .code = code};
}

// The download URL a release of `feed` carries for `asset` under `tag`:
// https://github.com/<owner>/<repo>/releases/download/<tag>/<asset>. The tag
// and asset names come from the grammar, which has nothing to percent-encode.
inline std::string FeedAssetUrl(const Feed& feed, std::string_view tag, std::string_view asset) {
  std::string url = "https://github.com/";
  url.append(feed.owner);
  url.push_back('/');
  url.append(feed.repo);
  url.append("/releases/download/");
  url.append(tag);
  url.push_back('/');
  url.append(asset);
  return url;
}

// Whether a release's download URL is the one its feed, tag and asset name.
// Matched whole: a URL that only starts with it names something else.
inline bool IsFeedAssetUrl(const Feed& feed, std::string_view tag, std::string_view asset,
                           std::string_view url) {
  return url == FeedAssetUrl(feed, tag, asset);
}

namespace selection_detail {

inline bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

}  // namespace selection_detail

// The release to offer from `releases` (the API's order) for `arch`, on
// `feed`, judged against `serverUnixSeconds`, the release list's Date header.
// A list without one (0) offers nothing: every code is in its future.
inline Selection SelectRelease(std::vector<Release> const& releases, std::string_view arch,
                               const Feed& feed, std::int64_t serverUnixSeconds) {
  Selection s;
  for (auto const& rel : releases) {
    if (rel.draft) continue;
    if (rel.prerelease && !feed.acceptAnyPrerelease &&
        !(feed.acceptBetaPrereleases && selection_detail::EndsWith(rel.tag, "-beta"))) {
      continue;
    }
    const std::optional<FeedTag> tag = ParseFeedTag(feed, rel.tag);
    if (!tag) continue;
    if (feed.requireImmutable && !rel.immutable) {
      s.skipped.push_back({rel.tag, "is not an immutable release", false});
      continue;
    }
    if (CodeUnixSeconds(tag->code) > serverUnixSeconds + kFutureCodeLimitSeconds) {
      s.skipped.push_back(
          {rel.tag, "has a future code (more than 48 h after the server's date)", false});
      continue;
    }

    const std::string name = InstallerAssetName(tag->version, arch);
    const ReleaseAsset* match = nullptr;
    for (auto const& asset : rel.assets) {
      if (asset.name == name) match = &asset;
    }
    if (!match || match->url.empty()) {
      s.skipped.push_back({rel.tag, "lacks " + name, false});
      continue;
    }
    const SoakStart since =
        SoakStartOf(tag->code, rel.publishedAt, rel.updatedAt, match->updatedAt);
    if (!HasSoaked(feed, since.unixSeconds, serverUnixSeconds)) {
      if (!since.unixSeconds) {
        s.skipped.push_back({rel.tag, std::string(since.problem), false});
        continue;
      }
      s.skipped.push_back(
          {rel.tag,
           "had not been out, unchanged, for " + std::to_string(feed.soakSeconds / 3600) +
               " h when the server's day began",
           true});
      if (tag->code > s.waitingCode) {
        s.waitingCode = tag->code;
        s.waitingVersion = tag->version;
        s.waitingFromUnixSeconds = SoakEndUnixSeconds(feed, *since.unixSeconds);
      }
      continue;
    }
    if (tag->code > s.newestCode) {
      s.newestCode = tag->code;
      s.newestVersion = tag->version;
    }
    if (tag->code <= s.code) continue;
    // URL and expected hash from the same asset object: the digest is GitHub's
    // own upload-time SHA-256 for exactly the bytes this URL serves.
    std::string digestHex = DigestHexFromAssetDigest(match->digest);
    if (digestHex.empty()) {
      s.skipped.push_back({rel.tag, "lacks a usable digest for " + name, false});
      continue;
    }
    s.code = tag->code;
    s.version = tag->version;
    s.tag = rel.tag;
    s.assetName = name;
    s.assetUrl = match->url;
    s.digestHex = std::move(digestHex);
    s.sumsUrl.clear();
    s.sumsSignatureUrl.clear();
    for (auto const& asset : rel.assets) {
      if (asset.name == "SHA256SUMS") s.sumsUrl = asset.url;
      if (asset.name == "SHA256SUMS.sig") s.sumsSignatureUrl = asset.url;
    }
  }
  return s;
}

// The hosts a release download may redirect to: GitHub's release-asset
// storage.
inline constexpr std::string_view kAssetRedirectHosts[] = {
    "release-assets.githubusercontent.com",
    "objects.githubusercontent.com",
};

// Whether a download's redirect Location may be followed: https to one of
// kAssetRedirectHosts on the default port, with a path, no user info and
// nothing a lenient parser could read as another host. Anything else,
// including a URL this cannot parse, is refused.
inline bool IsAllowedAssetRedirect(std::string_view url) {
  constexpr std::string_view kScheme = "https://";
  if (url.substr(0, kScheme.size()) != kScheme) return false;
  for (const char c : url) {
    // controls, spaces and the characters a URL parser may treat as separators
    if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f ||
        c == '\\' || c == '"' || c == '<' || c == '>' || c == '`') {
      return false;
    }
  }
  std::string_view rest = url.substr(kScheme.size());
  const std::size_t authorityEnd = rest.find_first_of("/?#");
  if (authorityEnd == std::string_view::npos || rest[authorityEnd] != '/') return false;
  std::string_view authority = rest.substr(0, authorityEnd);
  if (authority.find('@') != std::string_view::npos) return false;
  if (const std::size_t colon = authority.find(':'); colon != std::string_view::npos) {
    if (authority.substr(colon + 1) != "443") return false;
    authority = authority.substr(0, colon);
  }
  for (const std::string_view host : kAssetRedirectHosts) {
    if (EqualsAsciiCaseless(authority, host)) return true;
  }
  return false;
}

namespace msi_detail {

struct CivilDate {
  std::int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
};

// The proleptic Gregorian date `days` after 1970-01-01 (Howard Hinnant's
// civil_from_days).
inline constexpr CivilDate CivilFromDays(std::int64_t days) {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
  const unsigned yearOfEra =
      (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const unsigned monthIndex = (5 * dayOfYear + 2) / 153;
  const unsigned day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
  const unsigned month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
  const std::int64_t year = static_cast<std::int64_t>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0);
  return CivilDate{.year = year, .month = month, .day = day};
}

}  // namespace msi_detail

// The MSI ProductVersion a release build of `code` carries, as
// tools/UrVersion.ps1 derives it: for the UTC instant code/10 seconds after
// the founding, (Y-2000).M.((D-1)*2048 + floor(secondOfDay*2048/86400)).
// Empty for code 0, which no release carries, and past 2255, where the first
// field leaves its byte.
inline std::string UrMsiVersion(std::uint64_t code) {
  if (code == 0) return {};
  const std::int64_t seconds = CodeUnixSeconds(code);
  const std::int64_t days = seconds / 86400;
  const std::int64_t secondOfDay = seconds % 86400;
  const msi_detail::CivilDate date = msi_detail::CivilFromDays(days);
  const std::int64_t major = date.year - 2000;
  if (major < 0 || major > 255) return {};
  const std::int64_t patch =
      static_cast<std::int64_t>(date.day - 1) * 2048 + secondOfDay * 2048 / 86400;
  return std::to_string(major) + "." + std::to_string(date.month) + "." + std::to_string(patch);
}

}  // namespace urnw::update
