// Executable spec for the update's pure decisions, run against the same
// headers the tray app and the update helper compile, on any host with a C++20
// compiler:
//   - which feed is polled (Common/ReleaseSelection.h kFeeds), and which
//     release is offered on it (SelectRelease), with the tag and asset names
//     build/all/run.sh actually publishes, and the page of releases
//     urnetwork/build's API listed on 2026-10-09;
//   - how long a release must have been out, unchanged, before the official
//     feed offers it (SoakStartOf, HasSoaked), and that which releases count
//     changes only when the server's day does;
//   - the MSI ProductVersion a release code must carry (UrMsiVersion), against
//     the vectors tools/UrVersion.ps1's Go oracle uses, and against a file of
//     the oracle's own answers when one is given;
//   - the download URL a release may name, and the hosts its redirect may go
//     to;
//   - which install locations an elevated process may run from
//     (Common/InstallLocation.h);
//   - what the update helper's exit code means, and when the tray app shows
//     a report (Common/UpdateResult.h);
//   - what the helper decides before msiexec runs, which variables it drops,
//     and how the tray app's wait on it ends (Common/UpdateApply.h);
//   - when the tray app asks GitHub again after a refusal, when it says its
//     checks have not worked, which release the banner's Later holds back, and
//     what becomes of a click once the check before it has returned
//     (Common/UpdateSchedule.h).
//
//   c++ -std=c++20 -I ../src/Common update-release-tests.cpp -o /tmp/update-release-tests
//   /tmp/update-release-tests [<file of "code msi-version" lines>]
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "InstallLocation.h"
#include "ReleaseSelection.h"
#include "UpdateApply.h"
#include "UpdateResult.h"
#include "UpdateSchedule.h"

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

// The release list's Date header in every selection below, unless the test
// says otherwise: 2026-10-06T00:00:00Z, the first second of a UTC day.
constexpr std::int64_t kServerUnix = 1791244800;

// 24 hours, written out: the soak's own constant could shrink.
constexpr std::int64_t kDay = 24 * 60 * 60;

std::string Narrow(const std::wstring& text) { return std::string(text.begin(), text.end()); }

std::string Hex(std::uint32_t value) {
  std::ostringstream text;
  text << "0x" << std::hex << value;
  return text.str();
}

std::string Digest(char c) { return "sha256:" + std::string(64, c); }

// An asset as the API lists it, uploaded at `uploadedAt`.
ReleaseAsset Asset(const std::string& tag, const std::string& name, const std::string& uploadedAt,
                   char digest = 'a') {
  return {.name = name,
          .url = "https://github.com/urnetwork/build/releases/download/" + tag + "/" + name,
          .digest = Digest(digest),
          .updatedAt = uploadedAt};
}

// The code of the UTC instant `unix` seconds.
std::uint64_t CodeAt(std::int64_t unix) {
  return static_cast<std::uint64_t>(unix - kCodeEpochUnixSeconds) * 10;
}

// When the pipeline publishes a build, unless a test says otherwise: two hours
// after the instant its code names. urnetwork/build's 62 releases with the
// MSIs were published 1.2 to 2.5 hours after theirs.
std::int64_t PublishedByDefault(const std::string& version) {
  return CodeUnixSeconds(urnw::version::ParseReleaseCode(version)) + 2 * 60 * 60;
}

// One official build as run.sh publishes it and as GitHub lists it while
// nobody touches it: v<version>, both MSIs next to the SDK and the other
// platforms' assets, immutable, published at `published`, its updated_at the
// same second, and every asset uploaded half an hour before that.
Release OfficialAt(const std::string& version, std::int64_t published) {
  const std::string tag = "v" + version;
  const std::string uploaded = FormatUtcSecond(published - 30 * 60);
  return {.tag = tag,
          .draft = false,
          .prerelease = false,
          .immutable = true,
          .publishedAt = FormatUtcSecond(published),
          .updatedAt = FormatUtcSecond(published),
          .assets = {Asset(tag, "URnetworkSdk-" + version + ".aar", uploaded),
                     Asset(tag, "URnetworkSdkWindows-" + version + ".zip", uploaded),
                     Asset(tag, "URnetwork-" + version + "-x64.msi", uploaded, 'b'),
                     Asset(tag, "URnetwork-" + version + "-arm64.msi", uploaded, 'c'),
                     Asset(tag, "URnetwork-" + version + ".pkg", uploaded)}};
}

// The same, published when the pipeline would have.
Release Official(const std::string& version) {
  return OfficialAt(version, PublishedByDefault(version));
}

// The same, published at the UTC second `publishedAt` names.
Release Official(const std::string& version, const char* publishedAt) {
  return OfficialAt(version, *ParseUtcSecond(publishedAt));
}

// The F-Droid reproducible-build prerelease run.sh mints at code+2 / code+3.
Release AndroidPrereleaseAt(const std::string& version, std::int64_t published) {
  const std::string tag = "v" + version;
  return {.tag = tag,
          .draft = false,
          .prerelease = true,
          .immutable = true,
          .publishedAt = FormatUtcSecond(published),
          .updatedAt = FormatUtcSecond(published),
          .assets = {Asset(tag, "com.bringyour.network-" + version + "-github-arm64-v8a-release.apk",
                           FormatUtcSecond(published - 30 * 60))}};
}

Release AndroidPrerelease(const std::string& version) {
  return AndroidPrereleaseAt(version, PublishedByDefault(version));
}

Release AndroidPrerelease(const std::string& version, const char* publishedAt) {
  return AndroidPrereleaseAt(version, *ParseUtcSecond(publishedAt));
}

// This architecture's package of `release`, for a test that changes it.
ReleaseAsset& X64Msi(Release& release) {
  for (ReleaseAsset& asset : release.assets) {
    if (asset.name.size() > 8 && asset.name.substr(asset.name.size() - 8) == "-x64.msi") return asset;
  }
  return release.assets.front();
}

// A feed that takes -beta prereleases, and one shaped like a runner test's.
constexpr Feed kBetaLikeFeed{.id = "beta-like",
                             .numericRepoId = 1,
                             .owner = "example",
                             .repo = "example",
                             .acceptBetaPrereleases = true,
                             .requireImmutable = false,
                             .tagPrefix = "",
                             .acceptAnyPrerelease = false};
constexpr Feed kRunnerLikeFeed{.id = "runner-test",
                               .numericRepoId = 2,
                               .owner = "example",
                               .repo = "example",
                               .acceptBetaPrereleases = false,
                               .requireImmutable = false,
                               .tagPrefix = "runner-test-",
                               .acceptAnyPrerelease = true};
// The official feed without its soak, for the rules the soak would hide: a
// release whose code is two days ahead of the list cannot have been out for a
// day.
constexpr Feed kNoSoakFeed{.id = "no-soak",
                           .numericRepoId = 936244679,
                           .owner = "urnetwork",
                           .repo = "build",
                           .acceptBetaPrereleases = false,
                           .requireImmutable = true,
                           .soakSeconds = 0,
                           .tagPrefix = "",
                           .acceptAnyPrerelease = false};

void FeedTable() {
  // The official releases: urnetwork/build's, where the release pipeline
  // publishes the MSIs, addressed by the id GitHub gave that repository.
  // Never a personal fork, and no other repository of the organisation.
  const Feed* official = FeedById("official");
  Check(official != nullptr, "the official feed is in the table");
  if (!official) return;
  Check(official->owner == "urnetwork", "the feed is an official urnetwork repo");
  Check(official->owner == "urnetwork" && official->repo == "build",
        "the update checker polls urnetwork/build's published releases");
  Check(official->numericRepoId == 936244679,
        "the official feed is repository 936244679, urnetwork/build's own id");
  Check(!official->acceptBetaPrereleases && !official->acceptAnyPrerelease,
        "the official feed takes no prerelease");
  Check(official->requireImmutable, "the official feed requires immutable releases");
  Check(official->soakSeconds == kDay, "the official feed soaks a release for 24 hours");
  Check(official->tagPrefix.empty(), "the official feed's tags carry no prefix");
  Check(std::size(kFeeds) == 1, "the table holds the official feed alone");
  Check(FeedById("beta") == nullptr && FeedById("") == nullptr, "no other channel resolves");
  Check(kFeeds[0].numericRepoId == kOfficialFeed.numericRepoId &&
            kFeeds[0].owner == kOfficialFeed.owner && kFeeds[0].repo == kOfficialFeed.repo &&
            kFeeds[0].soakSeconds == kOfficialFeed.soakSeconds,
        "the table's official row is kOfficialFeed");

  // The list both the tray app and the update helper ask for.
  CheckEq("https://api.github.com/repositories/936244679/releases?per_page=30",
          ReleaseListUrl(kOfficialFeed), "the official feed's release list, by the repository's id");
  // The pipeline publishes three releases a build, and has published six
  // builds within two days (2026-09-08): a release that soaks waits up to two
  // days, and the list must still reach the one before those.
  Check(kReleaseListPageSize >= 3 * (6 + 1),
        "one page of the list holds two days of the pipeline's builds and the one before them");
  Check(kReleaseListPageSize <= 100, "GitHub serves at most 100 releases a page");
}

void AssetNames() {
  CheckEq("URnetwork-2026.8.28-1031763440-x64.msi",
          InstallerAssetName("2026.8.28-1031763440", "x64"), "x64 MSI name");
  CheckEq("URnetwork-2026.8.28-1031763440-arm64.msi",
          InstallerAssetName("2026.8.28-1031763440", "arm64"), "arm64 MSI name");
}

void RealReleaseList() {
  // newest first, as the API returns it
  const std::vector<Release> releases = {
      AndroidPrerelease("2026.9.22-1053244733"),
      AndroidPrerelease("2026.9.22-1053244732"),
      Official("2026.9.22-1053244730"),
      AndroidPrerelease("2026.8.28-1031763443"),
      Official("2026.8.28-1031763440"),
  };
  {
    const Selection s = SelectRelease(releases, "x64", kOfficialFeed, kServerUnix);
    Check(s.code == 1053244730, "offers the newest official release, not an android prerelease");
    CheckEq("2026.9.22-1053244730", s.version, "offered version is v-less");
    CheckEq("v2026.9.22-1053244730", s.tag, "offered tag keeps its v");
    CheckEq("URnetwork-2026.9.22-1053244730-x64.msi", s.assetName, "own-arch MSI");
    CheckEq("https://github.com/urnetwork/build/releases/download/v2026.9.22-1053244730/"
            "URnetwork-2026.9.22-1053244730-x64.msi",
            s.assetUrl, "download URL comes from the matched asset");
    CheckEq(std::string(64, 'b'), s.digestHex, "digest comes from the matched asset");
    Check(s.newestCode == 1053244730, "newest ignores prereleases");
    CheckEq("2026.9.22-1053244730", s.newestVersion, "newest version");
  }
  {
    const Selection s = SelectRelease(releases, "arm64", kOfficialFeed, kServerUnix);
    CheckEq("URnetwork-2026.9.22-1053244730-arm64.msi", s.assetName, "arm64 picks its own MSI");
    CheckEq(std::string(64, 'c'), s.digestHex, "arm64 digest");
  }
}

void NotOffered() {
  Release draft = Official("2026.10.1-1060000000");
  draft.draft = true;
  Release noMsi = Official("2026.9.30-1059000000");
  noMsi.assets = {Asset(noMsi.tag, "URnetwork-2026.9.30-1059000000.pkg", noMsi.publishedAt)};
  Release badDigest = Official("2026.9.29-1058000000");
  for (auto& a : badDigest.assets) a.digest = "sha512:" + std::string(64, 'b');
  Release oldZip = Official("2026.9.28-1057000000");
  oldZip.assets = {Asset(oldZip.tag, "URnetwork-v2026.9.28-1057000000-windows-x64-portable.zip",
                         oldZip.publishedAt)};
  const Selection s = SelectRelease({draft, noMsi, badDigest, oldZip, Official("2026.9.22-1053244730")},
                                    "x64", kOfficialFeed, kServerUnix);
  Check(s.code == 1053244730, "drafts, MSI-less, digest-less and zip-only releases are skipped");
  Check(s.newestCode == 1058000000,
        "newest names the newest release carrying this product's MSI, offerable or not");
  Check(s.skipped.size() == 3, "the three unverifiable releases are reported as skipped");
  Check(!s.skipped[0].soaking && !s.skipped[1].soaking && !s.skipped[2].soaking,
        "and none of them as only waiting for its day");

  const Selection empty = SelectRelease({}, "x64", kOfficialFeed, kServerUnix);
  Check(empty.code == 0 && empty.newestCode == 0 && empty.assetUrl.empty(),
        "empty list offers nothing");
  Release latest = Official("2026.9.22-1053244730");
  latest.tag = "latest";
  const Selection t = SelectRelease({latest}, "x64", kOfficialFeed, kServerUnix);
  Check(t.code == 0 && t.newestCode == 0, "a tag outside the grammar is ignored");
}

void ImmutableRequired() {
  Release mutableRelease = Official("2026.10.1-1060587890");
  mutableRelease.immutable = false;
  const Selection s = SelectRelease({mutableRelease, Official("2026.9.22-1053244730")}, "x64",
                                    kOfficialFeed, kServerUnix);
  Check(s.code == 1053244730, "a release that is not immutable is not offered on the official feed");
  Check(s.newestCode == 1053244730, "nor named as the newest");
  Check(s.skipped.size() == 1 && s.skipped[0].tag == "v2026.10.1-1060587890" &&
            s.skipped[0].reason.find("immutable") != std::string::npos,
        "and it is reported as not immutable");

  const Selection beta = SelectRelease({mutableRelease}, "x64", kBetaLikeFeed, kServerUnix);
  Check(beta.code == 1060587890, "a feed that does not require it offers a mutable release");
}

void Prereleases() {
  Release beta = Official("2026.10.3-1062717970-beta");
  beta.prerelease = true;
  Release rc = Official("2026.10.3-1062717980");
  rc.prerelease = true;
  {
    const Selection s = SelectRelease({rc, beta, Official("2026.10.1-1060587890")}, "x64",
                                      kOfficialFeed, kServerUnix);
    Check(s.code == 1060587890 && s.newestCode == 1060587890,
          "the official feed skips every prerelease, -beta ones included");
  }
  {
    const Selection s = SelectRelease({rc, beta, Official("2026.10.1-1060587890")}, "x64",
                                      kBetaLikeFeed, kServerUnix);
    Check(s.code == 1062717970, "a beta feed offers a -beta prerelease");
    Check(s.newestCode == 1062717970, "and skips any other prerelease");
  }
  {
    Release prefixed = Official("2026.10.3-1062717990");
    prefixed.tag = "runner-test-" + prefixed.tag;
    prefixed.prerelease = true;
    const std::string asset = "URnetwork-2026.10.3-1062717990-x64.msi";
    prefixed.assets = {{.name = asset,
                        .url = FeedAssetUrl(kRunnerLikeFeed, prefixed.tag, asset),
                        .digest = Digest('d'),
                        .updatedAt = {}}};
    const Selection s = SelectRelease({prefixed, rc, Official("2026.10.1-1060587890")}, "x64",
                                      kRunnerLikeFeed, kServerUnix);
    Check(s.code == 1062717990 && s.tag == prefixed.tag,
          "a runner test feed offers its prefixed prerelease");
    CheckEq("2026.10.3-1062717990", s.version, "its version drops the prefix and the v");
    Check(s.newestCode == 1062717990, "tags without the prefix are not the runner feed's");
    const Selection official = SelectRelease({prefixed}, "x64", kOfficialFeed, kServerUnix);
    Check(official.code == 0 && official.newestCode == 0,
          "the official feed never reads a prefixed tag");
  }
}

void FutureCodes() {
  // the promised two days, written out: the constant itself could shrink
  const std::uint64_t atLimit = CodeAt(kServerUnix + 48 * 60 * 60);
  const std::uint64_t pastLimit = atLimit + 10;
  auto release = [](std::uint64_t code) {
    return Official("2026.10.8-" + std::to_string(code));
  };
  // The cap itself, on a feed that does not soak: a release with a code two
  // days ahead of the list was published after the list's date.
  {
    const Selection s = SelectRelease({release(pastLimit), Official("2026.10.1-1060587890")},
                                      "x64", kNoSoakFeed, kServerUnix);
    Check(s.code == 1060587890, "a code more than 48 h past the server's date is not offered");
    Check(s.newestCode == 1060587890, "nor named as the newest");
    Check(s.skipped.size() == 1 && s.skipped[0].reason.find("future code") != std::string::npos &&
              !s.skipped[0].soaking,
          "and it is logged as a future code");
  }
  {
    const Selection s = SelectRelease({release(atLimit)}, "x64", kNoSoakFeed, kServerUnix);
    Check(s.code == atLimit, "a code exactly 48 h past the server's date is offered");
  }
  {
    // A one-digit typo of a real code lands decades out.
    const Selection s = SelectRelease({Official("2057.1.23-10627179700")}, "x64", kNoSoakFeed,
                                      kServerUnix);
    Check(s.code == 0, "a far-future code is never offered");
  }
  {
    // The cap is the server's clock, not this machine's: the same release is
    // fine a day later.
    const Selection s = SelectRelease({release(pastLimit)}, "x64", kNoSoakFeed,
                                      kServerUnix + 24 * 60 * 60);
    Check(s.code == pastLimit, "the cap moves with the server's date");
  }
  // On the official feed the cap comes first, and the soak then holds the
  // rest back.
  {
    const Selection s = SelectRelease({release(pastLimit), Official("2026.10.1-1060587890")},
                                      "x64", kOfficialFeed, kServerUnix);
    Check(s.code == 1060587890 && s.newestCode == 1060587890 && s.waitingCode == 0,
          "on the official feed a code past the cap is neither offered nor named as waiting");
    Check(s.skipped.size() == 1 && s.skipped[0].reason.find("future code") != std::string::npos,
          "and is logged as a future code there too");
    const Selection ahead = SelectRelease({release(atLimit), Official("2026.10.1-1060587890")},
                                          "x64", kOfficialFeed, kServerUnix);
    Check(ahead.code == 1060587890 && ahead.waitingCode == atLimit,
          "a code within the cap that is not out yet by the list's date waits for its day");
    Check(SelectRelease({Official("2057.1.23-10627179700")}, "x64", kOfficialFeed, kServerUnix)
                  .code == 0,
          "a far-future code is never offered on the official feed");
  }
}

void NewestCountsOwnProduct() {
  // A newer release with the other platforms' assets only.
  Release otherPlatforms = Official("2026.10.2-1061000000");
  otherPlatforms.assets = {
      Asset(otherPlatforms.tag, "URnetworkSdk-2026.10.2-1061000000.aar", otherPlatforms.publishedAt),
      Asset(otherPlatforms.tag, "URnetwork-2026.10.2-1061000000.pkg", otherPlatforms.publishedAt)};
  const Selection s = SelectRelease({otherPlatforms, Official("2026.10.1-1060587890")}, "x64",
                                    kOfficialFeed, kServerUnix);
  Check(s.newestCode == 1060587890,
        "newest counts only releases carrying this product's MSI for this architecture");
  // the arm64 MSI alone does not count for x64
  Release armOnly = Official("2026.10.2-1061000010");
  armOnly.assets = {
      Asset(armOnly.tag, "URnetwork-2026.10.2-1061000010-arm64.msi", armOnly.publishedAt)};
  const Selection x64 = SelectRelease({armOnly, Official("2026.10.1-1060587890")}, "x64",
                                      kOfficialFeed, kServerUnix);
  Check(x64.newestCode == 1060587890, "another architecture's MSI does not count");
  const Selection arm = SelectRelease({armOnly, Official("2026.10.1-1060587890")}, "arm64",
                                      kOfficialFeed, kServerUnix);
  Check(arm.newestCode == 1061000010 && arm.code == 1061000010, "for its own architecture it does");
}

// How long a release must have been out before the official feed offers it:
// 24 hours, when the UTC day of the release list's date began.
void Soak() {
  const std::string version = "2026.10.5-1064000000";
  const auto offered = [&version](std::int64_t published, std::int64_t server) {
    return SelectRelease({OfficialAt(version, published)}, "x64", kOfficialFeed, server).code != 0;
  };
  // the line, at the day's first second
  Check(!offered(kServerUnix - kDay + 60, kServerUnix),
        "a release 23 h 59 m old when the day began is not offered");
  Check(!offered(kServerUnix - kDay + 1, kServerUnix), "nor one a second short of 24 h");
  Check(offered(kServerUnix - kDay, kServerUnix),
        "a release 24 h old when the day began is offered");
  Check(offered(kServerUnix - kDay - 60, kServerUnix), "and so is an older one");
  // the line is the day's start, not the list's own second
  Check(!offered(kServerUnix - kDay + 1, kServerUnix + kDay - 1),
        "a release whose 24 hours end during the day waits for the next day");
  Check(offered(kServerUnix - kDay + 1, kServerUnix + kDay),
        "and is offered from the next day's first second");
  Check(!offered(kServerUnix - 1, kServerUnix + kDay - 1),
        "a release 24 h old only at the day's last second is not offered that day");

  // the day
  Check(UtcDayStart(kServerUnix) == kServerUnix && UtcDayStart(kServerUnix + kDay - 1) == kServerUnix &&
            UtcDayStart(kServerUnix + kDay) == kServerUnix + kDay &&
            UtcDayStart(kServerUnix - 1) == kServerUnix - kDay,
        "a UTC day runs from its first second through its 86400th");
  Check(UtcDayStart(0) == 0 && UtcDayStart(-1) == -kDay, "before 1970 too");

  // a release whose published_at is missing, or is not a UTC second
  Release unpublished = OfficialAt(version, kServerUnix - 2 * kDay);
  unpublished.publishedAt.clear();
  const Selection none = SelectRelease({unpublished}, "x64", kOfficialFeed, kServerUnix);
  Check(none.code == 0 && none.newestCode == 0 && none.waitingCode == 0,
        "a release without a publication time is never offered on a feed that soaks");
  Check(none.skipped.size() == 1 && !none.skipped[0].soaking &&
            none.skipped[0].reason.find("no publication time") != std::string::npos,
        "and it is reported as having none");
  for (const char* bad : {"2026-10-01", "2026-10-01T14:51:17", "2026-10-01T14:51:17+00:00",
                          "2026-10-01T14:51:17.000Z", "2026-13-01T00:00:00Z", "yesterday",
                          "1790866277", " 2026-10-01T14:51:17Z", "2026-09-31T00:00:00Z"}) {
    Release misdated = OfficialAt(version, kServerUnix - 2 * kDay);
    misdated.publishedAt = bad;
    Check(SelectRelease({misdated}, "x64", kOfficialFeed, kServerUnix + 30 * kDay).code == 0,
          std::string("a publication time that is not a UTC second is none: ") + bad);
  }
  Check(!HasSoaked(kOfficialFeed, std::nullopt, kServerUnix),
        "no publication time has not soaked, whatever the date");

  // a published_at after the list's date
  const Selection future =
      SelectRelease({OfficialAt(version, kServerUnix + 3600)}, "x64", kOfficialFeed, kServerUnix);
  Check(future.code == 0 && future.newestCode == 0,
        "a release published after the list's date is not offered");
  Check(future.waitingCode == 1064000000 && future.waitingFromUnixSeconds == kServerUnix + 2 * kDay,
        "it counts from the first day that begins 24 h after its publication");
  Check(!offered(kServerUnix + 400 * kDay, kServerUnix),
        "nor is a release dated a year ahead of the list");

  // a list with no date
  Check(!HasSoaked(kOfficialFeed, kServerUnix - 10 * kDay, 0) &&
            !HasSoaked(kOfficialFeed, -10 * kDay, 0),
        "a list with no date counts no release on a feed that soaks, whenever it was published");
  Check(!offered(kServerUnix - kDay, 0), "and offers none");

  // feeds that do not soak
  Release fresh = OfficialAt("2026.10.3-1062717970-beta", kServerUnix - 60);
  fresh.prerelease = true;
  Check(SelectRelease({fresh}, "x64", kBetaLikeFeed, kServerUnix).code == 1062717970,
        "a feed without a soak offers a release the minute it is published");
  fresh.publishedAt.clear();
  fresh.updatedAt.clear();
  for (ReleaseAsset& asset : fresh.assets) asset.updatedAt.clear();
  Check(SelectRelease({fresh}, "x64", kBetaLikeFeed, kServerUnix).code == 1062717970,
        "and one whose times the list does not give");
  Check(HasSoaked(kBetaLikeFeed, std::nullopt, 0) && HasSoaked(kRunnerLikeFeed, std::nullopt, 0),
        "a feed without a soak counts every release");
  Check(SoakEndUnixSeconds(kBetaLikeFeed, kServerUnix - 60) == kServerUnix - 60,
        "from the second it is published");

  // what is still soaking is neither offered nor the newest, and is named
  Release otherPlatforms = OfficialAt("2026.10.5-1064000010", kServerUnix - 1800);
  otherPlatforms.assets = {Asset(otherPlatforms.tag, "URnetwork-2026.10.5-1064000010.pkg",
                                 otherPlatforms.publishedAt)};
  const Selection two = SelectRelease({otherPlatforms, OfficialAt(version, kServerUnix - 3600),
                                       OfficialAt("2026.10.4-1063000000", kServerUnix - 7200),
                                       Official("2026.10.1-1060587890")},
                                      "x64", kOfficialFeed, kServerUnix);
  Check(two.code == 1060587890,
        "a release still soaking is not offered: the newest one that has soaked is");
  Check(two.newestCode == 1060587890, "a release still soaking is not named as the newest");
  Check(two.waitingCode == 1064000000 && two.waitingVersion == version,
        "the newest release with this product's MSI that the soak holds back is named as waiting");
  CheckEq(FormatUtcSecond(kServerUnix + kDay), FormatUtcSecond(two.waitingFromUnixSeconds),
          "with the first second it counts from");
  Check(two.skipped.size() == 3 && !two.skipped[0].soaking && two.skipped[1].soaking &&
            two.skipped[2].soaking,
        "a release only its day holds back is marked as that, and one without the MSI is not");

  // SoakEndUnixSeconds against HasSoaked, across four days of publication
  // times and seven of list dates
  int disagreements = 0;
  int pairs = 0;
  for (std::int64_t published = kServerUnix - 4 * kDay; published <= kServerUnix; published += 1789) {
    const std::int64_t end = SoakEndUnixSeconds(kOfficialFeed, published);
    if (end % kDay != 0 || end < published + kDay || end >= published + 2 * kDay) ++disagreements;
    for (std::int64_t server = kServerUnix - 4 * kDay; server <= kServerUnix + 3 * kDay; server += 613) {
      ++pairs;
      if (HasSoaked(kOfficialFeed, published, server) != (server >= end)) ++disagreements;
    }
  }
  Check(SoakEndUnixSeconds(kOfficialFeed, kServerUnix - kDay) == kServerUnix &&
            SoakEndUnixSeconds(kOfficialFeed, kServerUnix - kDay + 1) == kServerUnix + kDay,
        "a release counts from the first UTC day that begins at least 24 h after its publication");
  Check(disagreements == 0 && pairs > 100000,
        "SoakEndUnixSeconds is the first second HasSoaked holds, 24 to 48 hours after publication: " +
            std::to_string(disagreements) + " of " + std::to_string(pairs) + " disagree");
}

// The soak is counted from when a release last became what the list shows:
// the latest of its publication, its last change, and the upload of this
// product's package. GitHub lets a published release be edited and its
// prerelease mark set and cleared, immutable or not, and keeps published_at
// when it is; a release that sat among the prereleases for its day, where
// nothing offers it, was out for nobody.
void SoakCountsFromTheLastChange() {
  // built on 2026-09-27, and published a week before the list
  const std::string version = "2026.9.28-1057000000";
  constexpr std::uint64_t kCode = 1057000000;
  constexpr std::int64_t kPublished = kServerUnix - 7 * kDay;
  const auto select = [](const Release& release, std::int64_t server, const char* arch = "x64") {
    return SelectRelease({release}, arch, kOfficialFeed, server);
  };
  Check(select(OfficialAt(version, kPublished), kServerUnix).code == kCode,
        "a release published a week ago and not touched since is offered");

  // changed an hour before the day began
  Release edited = OfficialAt(version, kPublished);
  edited.updatedAt = FormatUtcSecond(kServerUnix - 3600);
  {
    const Selection s = select(edited, kServerUnix);
    Check(s.code == 0 && s.newestCode == 0,
          "a release published a week ago and changed an hour ago is not offered");
    Check(s.waitingCode == kCode && s.waitingFromUnixSeconds == kServerUnix + kDay,
          "it is named as waiting, from the first day that begins 24 h after the change");
    Check(s.skipped.size() == 1 && s.skipped[0].soaking &&
              s.skipped[0].reason.find("unchanged") != std::string::npos,
          "and reported as not out, unchanged, for its day");
    Check(select(edited, kServerUnix + kDay - 1).code == 0 &&
              select(edited, kServerUnix + kDay).code == kCode,
          "it is offered again once it has been unchanged for a day when a day begins");
  }
  edited.updatedAt = FormatUtcSecond(kServerUnix - kDay);
  Check(select(edited, kServerUnix).code == kCode,
        "a release unchanged for exactly 24 h when the day began is offered");
  edited.updatedAt = FormatUtcSecond(kServerUnix - kDay + 1);
  Check(select(edited, kServerUnix).code == 0, "and one second less is not enough");

  // the time it spent as a prerelease: listed as one it is skipped, and the
  // second its mark is cleared its day starts
  Release promoted = OfficialAt(version, kPublished);
  promoted.prerelease = true;
  Check(select(promoted, kServerUnix).code == 0 && select(promoted, kServerUnix).waitingCode == 0,
        "a release marked a prerelease is neither offered nor named as waiting");
  promoted.prerelease = false;
  promoted.updatedAt = FormatUtcSecond(kServerUnix + 12 * 3600);
  {
    const Selection s = select(promoted, kServerUnix + 12 * 3600 + 60);
    Check(s.code == 0 && s.waitingCode == kCode,
          "a release that became one a minute ago is not offered at once, however long ago it "
          "was published");
    CheckEq(FormatUtcSecond(kServerUnix + 2 * kDay), FormatUtcSecond(s.waitingFromUnixSeconds),
            "its day starts when it last changed");
  }

  // the package itself
  Release reuploaded = OfficialAt(version, kPublished);
  X64Msi(reuploaded).updatedAt = FormatUtcSecond(kServerUnix - 3600);
  {
    const Selection s = select(reuploaded, kServerUnix);
    Check(s.code == 0 && s.waitingCode == kCode && s.waitingFromUnixSeconds == kServerUnix + kDay,
          "a package uploaded an hour ago is not offered, whenever its release was published");
    Check(select(reuploaded, kServerUnix, "arm64").code == kCode,
          "the other architecture's package, uploaded before the publication, is");
  }

  // the latest of the three, whichever it is
  Release early = OfficialAt(version, kServerUnix - 3600);
  early.updatedAt = FormatUtcSecond(kPublished);
  Check(select(early, kServerUnix).code == 0,
        "an update time before the publication does not shorten the soak");
  {
    const std::string a = FormatUtcSecond(kPublished);
    const std::string b = FormatUtcSecond(kPublished + 3600);
    const std::string c = FormatUtcSecond(kPublished + 7200);
    Check(SoakStartOf(kCode, c, a, b).unixSeconds == std::optional<std::int64_t>{kPublished + 7200} &&
              SoakStartOf(kCode, a, c, b).unixSeconds == std::optional<std::int64_t>{kPublished + 7200} &&
              SoakStartOf(kCode, a, b, c).unixSeconds == std::optional<std::int64_t>{kPublished + 7200},
          "the soak starts at the latest of the publication, the last change and the upload");
    Check(SoakStartOf(kCode, a, a, a).problem.empty(), "and has nothing to report then");
  }

  // a missing or unreadable time never counts
  for (const char* bad : {"", "2026-10-05", "2026-10-05T23:00:00", "1791241200"}) {
    Release undated = OfficialAt(version, kPublished);
    undated.updatedAt = bad;
    const Selection s = select(undated, kServerUnix + 30 * kDay);
    Check(s.code == 0 && s.waitingCode == 0 && s.skipped.size() == 1 && !s.skipped[0].soaking &&
              s.skipped[0].reason.find("last change") != std::string::npos,
          std::string("a release without a readable time of its last change is never offered: '") +
              bad + "'");
    Release unstamped = OfficialAt(version, kPublished);
    X64Msi(unstamped).updatedAt = bad;
    const Selection t = select(unstamped, kServerUnix + 30 * kDay);
    Check(t.code == 0 && t.waitingCode == 0 && t.skipped.size() == 1 &&
              t.skipped[0].reason.find("upload time") != std::string::npos,
          std::string("nor one whose package has no readable upload time: '") + bad + "'");
  }
}

// A publication time that reads as a UTC second and cannot be the release's:
// one before the release was built. Its code is the instant it was built, to
// within the two days a code may be ahead of GitHub's clock.
void PublishedBeforeItWasBuilt() {
  const std::string version = "2026.9.28-1057000000";
  constexpr std::uint64_t kCode = 1057000000;
  const std::int64_t built = CodeUnixSeconds(kCode);
  // the list is read three weeks later: every soak that started is long over
  constexpr std::int64_t kLater = kServerUnix + 14 * kDay;
  for (const char* impossible : {"1970-01-01T00:00:00Z", "0000-01-01T00:00:00Z",
                                 "2026-09-01T00:00:00Z"}) {
    Release release = Official(version);
    release.publishedAt = impossible;
    const Selection s = SelectRelease({release}, "x64", kOfficialFeed, kLater);
    Check(s.code == 0 && s.newestCode == 0 && s.waitingCode == 0,
          std::string("a release published before it was built is never offered: ") + impossible);
    Check(s.skipped.size() == 1 && !s.skipped[0].soaking &&
              s.skipped[0].reason.find("before its own code") != std::string::npos,
          std::string("and it is reported as published before its code: ") + impossible);
  }
  // the two days, written out: the constant could shrink or grow
  Check(SelectRelease({OfficialAt(version, built - 48 * 3600)}, "x64", kOfficialFeed, kLater).code ==
            kCode,
        "a publication time 48 h before the code's instant is within a build clock's skew");
  Check(SelectRelease({OfficialAt(version, built - 48 * 3600 - 1)}, "x64", kOfficialFeed, kLater)
                .code == 0,
        "one second more is not");
  Check(SelectRelease({OfficialAt(version, built - 48 * 3600 - 1)}, "x64", kNoSoakFeed, kLater).code ==
            kCode,
        "a feed without a soak does not read the publication time at all");
}

// urnetwork/build's newest releases as its API listed them, without a token,
// on 2026-10-09: each build's release, which carries the MSIs, and the two
// android prereleases the pipeline publishes with it at code+2 and code+3.
std::vector<Release> FeedPage() {
  return {
      Official("2026.10.8-1066946420", "2026-10-08T23:34:12Z"),
      AndroidPrerelease("2026.10.8-1066946423", "2026-10-08T23:34:28Z"),
      AndroidPrerelease("2026.10.8-1066946422", "2026-10-08T23:34:21Z"),
      Official("2026.10.6-1065506180", "2026-10-07T07:29:19Z"),
      AndroidPrerelease("2026.10.6-1065506183", "2026-10-07T07:29:32Z"),
      AndroidPrerelease("2026.10.6-1065506182", "2026-10-07T07:29:26Z"),
      Official("2026.10.1-1060587890", "2026-10-01T14:51:17Z"),
      Official("2026.10.1-1060477040", "2026-10-01T11:39:28Z"),
      AndroidPrerelease("2026.10.1-1060587893", "2026-10-01T14:51:28Z"),
      AndroidPrerelease("2026.10.1-1060587892", "2026-10-01T14:51:23Z"),
      AndroidPrerelease("2026.10.1-1060477043", "2026-10-01T11:39:39Z"),
      AndroidPrerelease("2026.10.1-1060477042", "2026-10-01T11:39:33Z"),
      Official("2026.9.30-1060363020", "2026-10-01T08:32:40Z"),
      Official("2026.9.30-1060242630", "2026-10-01T05:12:28Z"),
      AndroidPrerelease("2026.9.30-1060363023", "2026-10-01T08:32:50Z"),
  };
}

void TheFeedAsListed() {
  const std::vector<Release> page = FeedPage();
  const auto at = [&page](const char* serverDate, const char* arch = "x64") {
    return SelectRelease(page, arch, kOfficialFeed, *ParseUtcSecond(serverDate));
  };
  {
    // the list's own Date header when it was read
    const Selection s = at("2026-10-09T01:24:01Z");
    CheckEq("v2026.10.6-1065506180", s.tag,
            "on 2026-10-09 the feed offers the release published on 2026-10-07");
    Check(s.code == 1065506180 && s.newestCode == 1065506180, "which is the newest that counts");
    CheckEq("https://github.com/urnetwork/build/releases/download/v2026.10.6-1065506180/"
            "URnetwork-2026.10.6-1065506180-x64.msi",
            s.assetUrl, "at urnetwork/build's download URL");
    Check(IsFeedAssetUrl(kOfficialFeed, s.tag, s.assetName, s.assetUrl),
          "which is the official feed's own");
    Check(s.waitingCode == 1066946420 && s.waitingVersion == "2026.10.8-1066946420",
          "the release published two hours before that list is held back");
    CheckEq("2026-10-10T00:00:00Z", FormatUtcSecond(s.waitingFromUnixSeconds),
            "until the first second of 2026-10-10");
    Check(s.skipped.size() == 1 && s.skipped[0].tag == "v2026.10.8-1066946420" &&
              s.skipped[0].reason.find("24 h") != std::string::npos,
          "and it is reported as not out for 24 h");
    // the update helper's question: does the list offer the tag it was given?
    Check(SelectionOffers(s, "v2026.10.6-1065506180", 1060587890),
          "an older build is offered the release that has soaked");
    Check(!SelectionOffers(s, "v2026.10.8-1066946420", 1060587890),
          "and is not offered the one still soaking, whatever tag it asks for");
  }
  CheckEq("v2026.10.6-1065506180", at("2026-10-09T23:34:12Z").tag,
          "when the newer release turns 24 h old it still waits: the line is the day's start");
  CheckEq("v2026.10.6-1065506180", at("2026-10-09T23:59:59Z").tag,
          "through that day's last second");
  {
    const Selection s = at("2026-10-10T00:00:00Z");
    CheckEq("v2026.10.8-1066946420", s.tag, "the next day begins with it offered");
    Check(s.waitingCode == 0 && s.skipped.empty(), "and nothing held back");
  }
  CheckEq("URnetwork-2026.10.6-1065506180-arm64.msi", at("2026-10-09T01:24:01Z", "arm64").assetName,
          "arm64 is offered its own MSI of the same release");
  Check(at("2026-10-20T00:00:00Z").code == 1066946420,
        "the android prereleases above a release are never offered, soaked or not");
  // the day the release of 2026-10-07 itself was still soaking
  const Selection earlier = at("2026-10-08T12:00:00Z");
  CheckEq("v2026.10.1-1060587890", earlier.tag,
          "on 2026-10-08 the feed still offered the newest release of 2026-10-01");

  // The same page, had the release it offers been edited an hour before the
  // list was read: its day starts over, and the one before it is offered.
  std::vector<Release> edited = FeedPage();
  edited[3].updatedAt = "2026-10-09T00:24:01Z";
  const Selection after =
      SelectRelease(edited, "x64", kOfficialFeed, *ParseUtcSecond("2026-10-09T01:24:01Z"));
  CheckEq("v2026.10.1-1060587890", after.tag,
          "the offered release, edited an hour before the list, is taken off the offer");
  Check(after.waitingCode == 1066946420 && after.skipped.size() == 2 && after.skipped[0].soaking &&
            after.skipped[1].soaking,
        "and it waits with the newer one");
  CheckEq("v2026.10.8-1066946420",
          SelectRelease(edited, "x64", kOfficialFeed, *ParseUtcSecond("2026-10-11T00:00:00Z")).tag,
          "two midnights later the newest release is offered as it would have been");
}

// At most one new offer a day. Through one server day, with a release reaching
// its 24th hour in every hour of it, what the official feed offers never
// changes; the next day begins with the newest of them offered. Every
// selection here is made afresh from the list and its date: nothing is kept
// from one check to the next, so restarting the app cannot bring a second
// offer.
void OneOfferADay() {
  std::vector<Release> list;
  std::vector<std::int64_t> soakedAt;  // when each one's 24 hours end
  std::uint64_t newest = 0;
  for (int hour = 0; hour < 24; ++hour) {
    // published through the day before, one an hour, at half past
    const std::int64_t published = kServerUnix - kDay + hour * 3600 + 1800;
    newest = CodeAt(published - 2 * 3600);
    list.push_back(OfficialAt("2026.10.5-" + std::to_string(newest), published));
    soakedAt.push_back(published + kDay);
  }
  // the release that had soaked when the day began
  const std::uint64_t standing = CodeAt(kServerUnix - 3 * kDay);
  list.push_back(
      OfficialAt("2026.10.3-" + std::to_string(standing), kServerUnix - 3 * kDay + 2 * 3600));

  int checks = 0;
  int otherOffers = 0;
  int bySecond = 0;  // changes a soak judged at the list's own second would make
  std::size_t counted = 0;
  for (std::int64_t server = kServerUnix; server < kServerUnix + kDay; server += 600) {
    ++checks;
    if (SelectRelease(list, "x64", kOfficialFeed, server).code != standing) ++otherOffers;
    std::size_t soaked = 0;
    for (const std::int64_t end : soakedAt) {
      if (end <= server) ++soaked;
    }
    if (soaked != counted) {
      ++bySecond;
      counted = soaked;
    }
  }
  Check(checks == 144 && otherOffers == 0,
        "what the feed offers does not change during one server day: " +
            std::to_string(otherOffers) + " of " + std::to_string(checks) +
            " checks offered another release");
  Check(bySecond == 24,
        "while 24 releases reached their 24th hour in that day, an hour apart: " +
            std::to_string(bySecond));
  const Selection next = SelectRelease(list, "x64", kOfficialFeed, kServerUnix + kDay);
  Check(next.code == newest && next.waitingCode == 0,
        "the next day begins with the newest of them offered: one new offer for the 24");

  // The tray app checks again before it acts on an offer that is no longer
  // fresh: one older than five minutes, since a release can be withdrawn at any
  // moment, and one from before GitHub's day changed, when a newer release may
  // count.
  Check(kOfferFreshSeconds == 5 * 60, "an offer is taken as fresh for five minutes");
  Check(!OfferMayHaveChanged(kOfficialFeed, kServerUnix + 3600, 0) &&
            !OfferMayHaveChanged(kOfficialFeed, kServerUnix + 3600, 5 * 60),
        "a fresh offer made the same day needs no second check");
  Check(OfferMayHaveChanged(kOfficialFeed, kServerUnix + 3600, 5 * 60 + 1),
        "an offer older than five minutes is checked again, the same day too");
  Check(OfferMayHaveChanged(kOfficialFeed, kServerUnix + 3600, 6 * 3600),
        "and so is one from the check before");
  Check(!OfferMayHaveChanged(kOfficialFeed, kServerUnix + kDay - 100, 99),
        "a fresh offer needs none through the day's last second");
  Check(OfferMayHaveChanged(kOfficialFeed, kServerUnix + kDay - 100, 100),
        "an offer made before GitHub's day changed is checked again, however fresh");
  Check(OfferMayHaveChanged(kOfficialFeed, kServerUnix + kDay - 1, 1),
        "one second across midnight UTC is another day");
  Check(OfferMayHaveChanged(kOfficialFeed, kServerUnix, 30 * kDay), "and so is a month");
  Check(!OfferMayHaveChanged(kBetaLikeFeed, kServerUnix + kDay - 1, 60),
        "a fresh offer of a feed that does not soak is not tied to the day");
  Check(OfferMayHaveChanged(kBetaLikeFeed, kServerUnix, 5 * 60 + 1),
        "and is checked again once it is no longer fresh: any feed's release can be withdrawn");
  Check(OfferMayHaveChanged(kOfficialFeed, kServerUnix + 3600, -1),
        "a clock that ran backwards since the check is not taken for a fresh offer");
}

// What becomes of a click on the banner once the check the tray app ran
// first has returned.
void ClickAfterTheCheck() {
  constexpr std::uint64_t kClicked = 1065506180;
  constexpr std::uint64_t kNewer = 1066946420;
  constexpr std::uint64_t kOlder = 1060587890;
  // the check found the release the click was on still offered
  constexpr ClickAfterCheck kSame{.clickedCode = kClicked,
                                  .sameFeed = true,
                                  .checked = true,
                                  .offeredCode = kClicked,
                                  .bannerHolds = true,
                                  .laterCode = 0,
                                  .viaHelper = true};
  const auto step = [&kSame](auto&& change) {
    ClickAfterCheck click = kSame;
    change(click);
    return StepAfterCheck(click);
  };
  Check(StepAfterCheck(kSame) == ClickStep::Proceed,
        "a click on a release the feed still offers goes on");
  Check(step([](ClickAfterCheck& c) { c.viaHelper = false; }) == ClickStep::Proceed,
        "and so when the user is handed the installer");

  // the feed offers something else now
  Check(step([](ClickAfterCheck& c) { c.offeredCode = kNewer; }) == ClickStep::Replaced,
        "an offer that changed waits for a click of its own");
  Check(step([](ClickAfterCheck& c) {
          c.offeredCode = kNewer;
          c.viaHelper = false;
        }) == ClickStep::Replaced,
        "on the installer's path too");
  Check(step([](ClickAfterCheck& c) { c.offeredCode = kOlder; }) == ClickStep::Replaced,
        "an older release, offered once the clicked one was withdrawn, is another release too");
  Check(step([](ClickAfterCheck& c) { c.offeredCode = 0; }) == ClickStep::Withdrawn,
        "a release the feed no longer offers is not started");
  Check(step([](ClickAfterCheck& c) {
          c.offeredCode = 0;
          c.viaHelper = false;
        }) == ClickStep::Withdrawn,
        "nor is its installer shown");
  // The check itself moves the banner on when the offer changed: to the
  // release offered now, or shut. So the banner no longer holds the click by
  // the time it is judged, and what became of its release is still the answer.
  Check(step([](ClickAfterCheck& c) {
          c.offeredCode = kNewer;
          c.bannerHolds = false;
        }) == ClickStep::Replaced,
        "a release the check has already put on the banner is said to have replaced the clicked one");
  Check(step([](ClickAfterCheck& c) {
          c.offeredCode = 0;
          c.bannerHolds = false;
        }) == ClickStep::Withdrawn,
        "a release whose banner the check has already shut is said to be withdrawn");

  // the click no longer stands
  Check(step([](ClickAfterCheck& c) { c.laterCode = kClicked; }) == ClickStep::Dropped,
        "Later chosen while the check ran is not undone by the click");
  Check(step([](ClickAfterCheck& c) { c.laterCode = kNewer; }) == ClickStep::Proceed,
        "Later on another release does not stop this one");
  Check(step([](ClickAfterCheck& c) { c.bannerHolds = false; }) == ClickStep::Dropped,
        "a banner that moved on drops the click");
  Check(step([](ClickAfterCheck& c) { c.sameFeed = false; }) == ClickStep::Dropped,
        "a change of channel drops the click");
  Check(step([](ClickAfterCheck& c) {
          c.sameFeed = false;
          c.offeredCode = kNewer;
        }) == ClickStep::Dropped,
        "whatever the other feed offers");
  Check(StepAfterCheck({}) == ClickStep::Dropped, "and no click is no click");

  // the check could not be made
  Check(step([](ClickAfterCheck& c) { c.checked = false; }) == ClickStep::Proceed,
        "the helper asks GitHub itself: a check that could not be made leaves the click to it");
  Check(step([](ClickAfterCheck& c) {
          c.checked = false;
          c.viaHelper = false;
        }) == ClickStep::Unconfirmed,
        "an installer is not shown when GitHub could not be asked whether its release is offered");
  Check(step([](ClickAfterCheck& c) {
          c.checked = false;
          c.offeredCode = 0;
        }) == ClickStep::Proceed &&
            step([](ClickAfterCheck& c) {
              c.checked = false;
              c.offeredCode = kNewer;
            }) == ClickStep::Proceed,
        "a check that could not be made says nothing about what the feed offers");
  Check(step([](ClickAfterCheck& c) {
          c.checked = false;
          c.viaHelper = false;
          c.laterCode = kClicked;
        }) == ClickStep::Dropped,
        "and Later still drops the click");
}

// tests/ur_version_test.go urFixedVectors, computed with Python's datetime.
struct MsiVector {
  std::uint64_t code;
  const char* msi;
};
constexpr MsiVector kMsiVectors[] = {
    {1060587890, "26.10.1090"},   {1062717970, "26.10.6139"},   {1060587895, "26.10.1090"},
    {1060587899, "26.10.1090"},   {1060127995, "26.9.61439"},   {1060127999, "26.9.61439"},
    {824255995, "25.12.63487"},   {1060363020, "26.10.557"},    {1060128000, "26.10.0"},
    {1060128420, "26.10.0"},      {1060128430, "26.10.1"},      {1034207990, "26.8.63487"},
    {1034208000, "26.9.0"},       {824255990, "25.12.63487"},   {824256000, "26.1.0"},
    {244080000, "24.2.58368"},    {244511990, "24.2.59391"},    {244512000, "24.3.0"},
    {1086911990, "26.10.63487"},  {1086912000, "26.11.0"},      {1, "23.5.45056"},
    {9, "23.5.45056"},            {73404575990, "255.12.63487"},
};

void MsiVersions(const char* oracleFile) {
  for (const MsiVector& v : kMsiVectors) {
    CheckEq(v.msi, UrMsiVersion(v.code), "UrMsiVersion(" + std::to_string(v.code) + ")");
  }
  CheckEq("", UrMsiVersion(0), "code 0 has no MSI version");
  CheckEq("", UrMsiVersion(73404576000), "2256 is past the last ProductVersion the layout holds");
  if (!oracleFile) return;
  // The oracle's own answers: "<code> <msi version>", or "<code> -" for a code
  // past the layout.
  std::ifstream in(oracleFile);
  Check(static_cast<bool>(in), std::string("the oracle's vectors open: ") + oracleFile);
  std::string line;
  int vectors = 0;
  int mismatches = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    std::istringstream fields(line);
    std::uint64_t code = 0;
    std::string want;
    fields >> code >> want;
    if (want == "-") want.clear();
    ++vectors;
    if (UrMsiVersion(code) != want && ++mismatches <= 10) {
      CheckEq(want, UrMsiVersion(code), "UrMsiVersion(" + std::to_string(code) + ") against the oracle");
    }
  }
  Check(mismatches == 0, std::to_string(mismatches) + " of the oracle's vectors disagree");
  Check(vectors >= 1000, "the oracle gave " + std::to_string(vectors) + " vectors");
  std::cout << "  " << vectors << " oracle vectors\n";
}

void DownloadUrls() {
  const std::string tag = "v2026.10.1-1060587890";
  const std::string asset = "URnetwork-2026.10.1-1060587890-x64.msi";
  const std::string url = "https://github.com/urnetwork/build/releases/download/" + tag + "/" + asset;
  CheckEq(url, FeedAssetUrl(kOfficialFeed, tag, asset), "the official feed's download URL");
  Check(IsFeedAssetUrl(kOfficialFeed, tag, asset, url), "the feed's own URL is accepted");
  for (const std::string& bad : {
           url + "?x=1",
           url + "/",
           "http://github.com/urnetwork/build/releases/download/" + tag + "/" + asset,
           "https://github.com/urnetwork/windows/releases/download/" + tag + "/" + asset,
           "https://github.com/someone/build/releases/download/" + tag + "/" + asset,
           "https://github.com/urnetwork/build/releases/download/v2026.10.1-1060587891/" + asset,
           "https://github.com/urnetwork/build/releases/download/" + tag + "/URnetwork-x.msi",
           "https://evil.example/urnetwork/build/releases/download/" + tag + "/" + asset,
           "https://github.com.evil.example/urnetwork/build/releases/download/" + tag + "/" + asset,
           "https://github.com/urnetwork/build/releases/download/" + tag + "/../" + asset,
           "https://github.com/urnetwork/build-fork/releases/download/" + tag + "/" + asset,
           std::string(),
       }) {
    Check(!IsFeedAssetUrl(kOfficialFeed, tag, asset, bad), "refused download URL: " + bad);
  }
}

void Redirects() {
  for (const char* good : {
           "https://release-assets.githubusercontent.com/github-production-release-asset/"
           "936244679/60615c0d?sp=r&sv=2018-11-09&sr=b&spr=https",
           "https://objects.githubusercontent.com/github-production-release-asset-2e65be/1/2",
           "https://RELEASE-ASSETS.githubusercontent.com/x",
           "https://release-assets.githubusercontent.com:443/x",
           "https://release-assets.githubusercontent.com/",
       }) {
    Check(IsAllowedAssetRedirect(good), std::string("allowed redirect: ") + good);
  }
  for (const char* bad : {
           "http://release-assets.githubusercontent.com/x",
           "HTTPS://release-assets.githubusercontent.com/x",
           "https://release-assets.githubusercontent.com.evil.example/x",
           "https://release-assets.githubusercontent.com./x",
           "https://evil.example/release-assets.githubusercontent.com",
           "https://evil.example/?release-assets.githubusercontent.com",
           "https://user@release-assets.githubusercontent.com/x",
           "https://release-assets.githubusercontent.com@evil.example/x",
           "https://release-assets.githubusercontent.com:8443/x",
           "https://release-assets.githubusercontent.com:/x",
           "https://release-assets.githubusercontent.com",
           "https://release-assets.githubusercontent.com?x",
           "https://release-assets.githubusercontent.com#x",
           "https://release-assets.githubusercontent.com\\@evil.example/",
           "https://release-assets.githubusercontent.com/x y",
           "https://release-assets.githubusercontent.com/x\ty",
           "https://xrelease-assets.githubusercontent.com/x",
           "https://githubusercontent.com/x",
           "https://github.com/urnetwork/windows/releases/download/v1/x.msi",
           "https://release-assets.githubusercontent.com\xc2\xa0/x",
           "",
       }) {
    Check(!IsAllowedAssetRedirect(bad), std::string("refused redirect: ") + bad);
  }
}

void InstallLocations() {
  using urnw::install::InstallPathInfo;
  using urnw::install::InstallRights;
  using urnw::install::IsAdminOnlyInstallDir;
  // FILE_GENERIC_READ | FILE_GENERIC_EXECUTE: what Program Files' Users ACE
  // grants (RX), and Modify and Full control over it
  constexpr std::uint32_t kUsersRx = 0x001200A9;
  constexpr std::uint32_t kUsersModify = 0x001301BF;
  constexpr std::uint32_t kFullControl = 0x001F01FF;
  constexpr InstallPathInfo kProgramFiles{
      .underProgramFiles = true, .resolvesToItself = true, .reparsePoint = false};

  Check(IsAdminOnlyInstallDir(kProgramFiles, {.folder = kUsersRx, .executable = kUsersRx}),
        "Program Files with Users RX is an admin-only install location");
  Check(IsAdminOnlyInstallDir(kProgramFiles, {.folder = 0, .executable = 0}),
        "no rights at all is admin-only too");
  Check(!IsAdminOnlyInstallDir({.underProgramFiles = false, .resolvesToItself = true},
                               {.folder = kUsersRx, .executable = kUsersRx}),
        "a user folder is refused, whatever its ACL");
  Check(!IsAdminOnlyInstallDir(kProgramFiles, {.folder = kUsersModify, .executable = kUsersRx}),
        "a folder Users can modify is refused");
  Check(!IsAdminOnlyInstallDir(kProgramFiles, {.folder = kUsersRx, .executable = kUsersModify}),
        "an executable Users can modify is refused");
  Check(!IsAdminOnlyInstallDir(kProgramFiles, {.folder = kFullControl, .executable = kFullControl}),
        "full control is refused");
  Check(!IsAdminOnlyInstallDir(
            {.underProgramFiles = true, .resolvesToItself = true, .reparsePoint = true},
            {.folder = kUsersRx, .executable = kUsersRx}),
        "a reparse point on the way is refused");
  Check(!IsAdminOnlyInstallDir({.underProgramFiles = true, .resolvesToItself = false},
                               {.folder = kUsersRx, .executable = kUsersRx}),
        "a path that resolves elsewhere is refused");
  for (const std::uint32_t bit :
       {urnw::install::kWriteData, urnw::install::kAppendData, urnw::install::kWriteEa,
        urnw::install::kDeleteChild, urnw::install::kWriteAttributes, urnw::install::kDelete,
        urnw::install::kWriteDac, urnw::install::kWriteOwner}) {
    Check(!IsAdminOnlyInstallDir(kProgramFiles, {.folder = kUsersRx | bit, .executable = kUsersRx}),
          "folder right " + Hex(bit) + " alone is refused");
    Check(!IsAdminOnlyInstallDir(kProgramFiles, {.folder = kUsersRx, .executable = kUsersRx | bit}),
          "executable right " + Hex(bit) + " alone is refused");
  }
}

void Outcomes() {
  Check(OutcomeOf(0) == Outcome::Installed, "exit 0 installed");
  Check(OutcomeOf(3010) == Outcome::RestartRequired, "3010 is restart to finish, not a failure");
  Check(OutcomeOf(1641) == Outcome::RestartRequired, "1641 is a restart too");
  for (const std::int64_t code : {1602, 1603, 1618, 1625, 1638, 1}) {
    Check(OutcomeOf(code) == Outcome::Failed, "msiexec " + std::to_string(code) + " failed");
  }
  for (const Refusal refusal :
       {Refusal::NotElevated, Refusal::NotInstalled, Refusal::DevBuild, Refusal::BadArguments,
        Refusal::Busy, Refusal::ReleaseList, Refusal::NotOffered, Refusal::Download,
        Refusal::Digest, Refusal::Package, Refusal::Staging, Refusal::InstallerNotStarted,
        Refusal::RateLimited, Refusal::Signature}) {
    const auto code = static_cast<std::int64_t>(refusal);
    Check(IsRefusal(code) && OutcomeOf(code) == Outcome::Refused,
          "helper refusal " + std::to_string(code) + " is a refusal");
    Check(!KeepsPackage(code), "a refusal keeps no package");
  }
  Check(KeepsPackage(0) && KeepsPackage(3010) && KeepsPackage(1641),
        "an install that took keeps the package as the repair source");
  Check(!KeepsPackage(1603) && !KeepsPackage(1618), "a failed install deletes it");

  const UpdateResult good{.tag = "v2026.10.1-1060587890",
                          .code = 1060587890,
                          .exitCode = 0,
                          .finishedUtc = "2026-10-01T14:51:17Z"};
  Check(IsWellFormed(good), "a report the helper writes is well formed");
  UpdateResult bad = good;
  bad.code = 1060587891;
  Check(!IsWellFormed(bad), "a code that is not the tag's is refused");
  bad = good;
  bad.tag = "latest";
  Check(!IsWellFormed(bad), "a tag outside the grammar is refused");
  bad = good;
  bad.finishedUtc = "2026-10-01 14:51:17";
  Check(!IsWellFormed(bad), "a time that is not a UTC second is refused");
  bad = good;
  bad.exitCode = -1;
  Check(!IsWellFormed(bad), "a negative exit code is refused");
  bad = good;
  bad.exitCode = 0x100000000;
  Check(!IsWellFormed(bad), "an exit code no process returns is refused");
  UpdateResult prefixed = good;
  prefixed.tag = "runner-test-" + good.tag;
  Check(IsWellFormed(prefixed, "runner-test-") && !IsWellFormed(prefixed),
        "a runner test feed's tag reads with its prefix only");
}

void HelperDecisions() {
  // the tag argument
  Check(IsTagArgument(kOfficialFeed, "v2026.10.1-1060587890"), "an official tag is a tag argument");
  Check(IsTagArgument(kOfficialFeed, "2026.10.1-1060587890"), "so is one without its v");
  Check(IsTagArgument(kRunnerLikeFeed, "runner-test-v2026.10.1-1060587890"),
        "a runner test feed's prefixed tag is one of its");
  for (const char* bad : {
           "", "latest", "v2026.10.1-1060587890 ", " v2026.10.1-1060587890",
           "v2026.10.1-1060587890\"", "v2026.10.1-1060587890 /qn", "..", "../v2026.10.1-1060587890",
           "v2026.10.1-1060587890/x", "v2026.10.1-1060587890\\x", "-v2026.10.1-1060587890",
           "v2026.10.1-1060587890-rc", "runner-test-v2026.10.1-1060587890",
           "v2026.10.1-1060587890\xc2\xa0",
       }) {
    Check(!IsTagArgument(kOfficialFeed, bad), std::string("not an official tag argument: ") + bad);
  }
  Check(!IsTagArgument(kRunnerLikeFeed, "v2026.10.1-1060587890"),
        "a runner test feed takes only its prefixed tags");
  Check(!IsTagArgument(kOfficialFeed, "v2026.10.1-" + std::string(90, '1')),
        "a tag past the length cap is refused");

  // the selection must offer the tag, above this build
  Selection offered;
  offered.code = 1060587890;
  offered.tag = "v2026.10.1-1060587890";
  offered.assetName = "URnetwork-2026.10.1-1060587890-x64.msi";
  offered.assetUrl = FeedAssetUrl(kOfficialFeed, offered.tag, offered.assetName);
  offered.digestHex = std::string(64, 'a');
  Check(SelectionOffers(offered, "v2026.10.1-1060587890", 1053244730),
        "the list offering the tag above this build is an offer");
  Check(!SelectionOffers(offered, "v2026.10.2-1061000000", 1053244730),
        "a list that offers another tag is no offer of this one");
  Check(!SelectionOffers(offered, "v2026.10.1-1060587890", 1060587890),
        "a release no newer than this build is no offer");
  Check(!SelectionOffers(offered, "v2026.10.1-1060587890", 1070000000),
        "nor is one older than this build");
  Selection noDigest = offered;
  noDigest.digestHex.clear();
  Check(!SelectionOffers(noDigest, "v2026.10.1-1060587890", 1053244730),
        "an offer without a digest is no offer");
  Selection none;
  Check(!SelectionOffers(none, "", 0), "an empty selection offers nothing");

  // the package's identity
  CheckEq("{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", std::string(kUpgradeCode), "the UpgradeCode");
  Check(PackageMatches("{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", "26.10.1090", 1060587890),
        "this product at the release's ProductVersion matches");
  Check(PackageMatches("{a7c1e2d3-4b5f-6081-9c2d-3e4f50617283}", "26.10.1090", 1060587890),
        "the UpgradeCode compares without case");
  Check(!PackageMatches("{B7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", "26.10.1090", 1060587890),
        "another product's UpgradeCode is refused");
  Check(!PackageMatches("{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", "0.0.1", 1060587890),
        "an unstamped package under a release tag is refused");
  Check(!PackageMatches("{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", "26.10.1089", 1060587890),
        "an older package re-uploaded under a newer tag is refused");
  Check(!PackageMatches("{A7C1E2D3-4B5F-6081-9C2D-3E4F50617283}", "", 0),
        "code 0 has no package");

  // msiexec's command line
  CheckEq("\"C:\\Windows\\system32\\msiexec.exe\" /i \"C:\\Program Files\\URnetwork\\updates\\"
          "v2026.10.1-1060587890\\URnetwork-2026.10.1-1060587890-x64.msi\" /passive /norestart "
          "/l*v \"C:\\Program Files\\URnetwork\\updates\\v2026.10.1-1060587890\\install.log\" "
          "UPDATE_RELAUNCH=1",
          Narrow(MsiexecCommandLine(
              L"C:\\Windows\\system32\\msiexec.exe",
              L"C:\\Program Files\\URnetwork\\updates\\v2026.10.1-1060587890\\"
              L"URnetwork-2026.10.1-1060587890-x64.msi",
              L"C:\\Program Files\\URnetwork\\updates\\v2026.10.1-1060587890\\install.log")),
          "msiexec installs passively, never restarts, logs verbosely and asks for the relaunch");

  // finishedUtc
  CheckEq("2026-10-01T12:46:29Z", FormatUtcSecond(CodeUnixSeconds(1060587890)),
          "a release code's instant as a UTC second");
  CheckEq("1970-01-01T00:00:00Z", FormatUtcSecond(0), "the epoch");
  CheckEq("2000-02-29T23:59:59Z", FormatUtcSecond(951868799), "a leap day's last second");
  Check(IsUtcSecond(FormatUtcSecond(kServerUnix)), "what FormatUtcSecond writes, IsUtcSecond reads");
}

// When the tray app shows a report, and whose report it is.
void Reports() {
  constexpr std::uint64_t kRelease = 1060587890;
  const auto report = [](std::int64_t exitCode) {
    return UpdateResult{.tag = "v2026.10.1-1060587890",
                        .code = kRelease,
                        .exitCode = exitCode,
                        .finishedUtc = "2026-10-01T14:51:17Z"};
  };
  const auto view = [&](std::int64_t exitCode, std::uint64_t own, bool live, bool restarted) {
    return ViewOfReport(report(exitCode), own, live, restarted);
  };
  constexpr std::uint64_t kOlder = 1053244730;
  constexpr std::uint64_t kNewer = 1070000000;
  // installed
  Check(view(0, kRelease, false, false) == ReportView::Installed,
        "an install reads as installed in the release it installed");
  Check(view(0, kRelease, true, false) == ReportView::Installed, "and so while live");
  Check(view(0, kOlder, true, false) == ReportView::RestartApp,
        "an install the old app outlived asks for URnetwork's restart");
  Check(view(0, kOlder, false, false) == ReportView::Hidden,
        "an older build launched after an install is told nothing");
  Check(view(0, kNewer, false, false) == ReportView::Hidden,
        "a newer build is told nothing about an older install");
  // installed up to a restart
  Check(view(3010, kRelease, false, false) == ReportView::RestartWindows,
        "3010 asks for Windows' restart until there has been one");
  Check(view(3010, kOlder, false, false) == ReportView::RestartWindows,
        "and so in the old exe that is still on disk");
  Check(view(1641, kRelease, false, false) == ReportView::RestartWindows, "1641 is a restart too");
  Check(view(3010, kRelease, false, true) == ReportView::Installed,
        "3010 after a restart reads as installed");
  Check(view(3010, kOlder, false, true) == ReportView::Hidden,
        "3010 after a restart, in an older build, is told nothing");
  Check(view(3010, kNewer, false, false) == ReportView::Hidden,
        "a newer build is told nothing about an older 3010");
  // not installed
  for (const std::int64_t code : {std::int64_t{1603}, static_cast<std::int64_t>(Refusal::Digest),
                                  static_cast<std::int64_t>(Refusal::NotOffered)}) {
    Check(view(code, kOlder, false, false) == ReportView::NotInstalled,
          "a failure " + std::to_string(code) + " shows while this build is older");
    Check(view(code, kOlder, true, false) == ReportView::NotInstalled, "and so while live");
    Check(view(code, kRelease, false, false) == ReportView::Hidden,
          "a failure " + std::to_string(code) + " is moot once the release runs");
    Check(view(code, kNewer, false, false) == ReportView::Hidden,
          "a failure " + std::to_string(code) + " is moot in a newer build");
  }

  // the UTC second, both ways
  for (const std::int64_t second : {std::int64_t{0}, std::int64_t{951868799}, std::int64_t{951868800},
                                    CodeUnixSeconds(1060587890), kServerUnix, std::int64_t{4102444799}}) {
    const std::optional<std::int64_t> parsed = ParseUtcSecond(FormatUtcSecond(second));
    Check(parsed && *parsed == second, "the UTC second of " + std::to_string(second) + " reads back");
  }
  Check(ParseUtcSecond("2026-10-01T14:51:17Z") == std::optional<std::int64_t>{1790866277},
        "2026-10-01T14:51:17Z is 1790866277");
  for (const char* bad : {"", "2026-10-01 14:51:17Z", "2026-13-01T00:00:00Z", "2026-00-01T00:00:00Z",
                          "2026-10-00T00:00:00Z", "2026-10-01T24:00:00Z", "2026-10-01T00:60:00Z",
                          "2026-10-01T00:00:60Z", "2026-10-01T00:00:00"}) {
    Check(!ParseUtcSecond(bad), std::string("not a UTC second: ") + bad);
  }
  // a day its month does not have is no day of the next month
  for (const char* bad : {"2026-02-29T00:00:00Z", "2026-02-31T00:00:00Z", "2026-04-31T00:00:00Z",
                          "2026-09-31T12:00:00Z", "2100-02-29T00:00:00Z", "2026-10-32T00:00:00Z"}) {
    Check(!ParseUtcSecond(bad), std::string("not a day of its month: ") + bad);
  }
  Check(ParseUtcSecond("2024-02-29T23:59:59Z") == std::optional<std::int64_t>{1709251199} &&
            ParseUtcSecond("2000-02-29T00:00:00Z") == std::optional<std::int64_t>{951782400} &&
            ParseUtcSecond("2026-12-31T23:59:59Z") == std::optional<std::int64_t>{1798761599} &&
            ParseUtcSecond("2026-04-30T00:00:00Z") == std::optional<std::int64_t>{1777507200},
        "the last day of a month, and a leap day in a leap year, read as themselves");

  // whose report
  const UpdateResult written = report(static_cast<std::int64_t>(Refusal::Digest));
  constexpr std::int64_t kStarted = 1790866200;  // 77 s before it finished
  Check(IsReportOfRun(written, "v2026.10.1-1060587890", 0x20000009, kStarted),
        "the report the helper wrote is this run's");
  Check(!IsReportOfRun(written, "v2026.10.1-1060587890",
                       static_cast<std::int64_t>(Refusal::NotInstalled), kStarted),
        "an earlier report is not the run that refused before writing one");
  Check(!IsReportOfRun(written, "v2026.10.2-1061000000", 0x20000009, kStarted),
        "another tag's report is not this run's");
  Check(!IsReportOfRun(written, "v2026.10.1-1060587890", 0x20000009, kStarted + 3600),
        "a report written before the helper started is not this run's");
  Check(IsReportOfRun(written, "v2026.10.1-1060587890", 0x20000009, 1790866277 + 120),
        "two minutes of clock set back are allowed");
  Check(!IsReportOfRun(written, "v2026.10.1-1060587890", 0x20000009, 1790866277 + 121),
        "and no more");
}

// Which variables the elevated helper drops, and how the tray's wait ends.
void HelperProcess() {
  for (const wchar_t* name : {L"URNETWORK_APP_ROOT", L"urnetwork_app_root", L"Urnetwork_Network_Host",
                              L"URNETWORK_NETWORK_ENV", L"URNETWORK_"}) {
    Check(IsAppOverrideName(name), "an app override: " + Narrow(name));
  }
  for (const wchar_t* name : {L"", L"PATH", L"URNETWORK", L"URNETWORKX_ROOT", L"_URNETWORK_APP_ROOT",
                              L"XURNETWORK_APP_ROOT", L"URNETWORK-APP-ROOT"}) {
    Check(!IsAppOverrideName(name), "not an app override: " + Narrow(name));
  }

  // The helper ends after three slices while the app runs on.
  int slices = 0;
  int asked = 0;
  HelperWait waited = AwaitHelper([&] { return ++slices == 3; }, [&] { ++asked; return false; });
  Check(waited == HelperWait::Ended && slices == 3 && asked == 2,
        "the wait ends with the helper, after the slices it took");
  // The app begins to exit at the fourth slice. (The helper here ends at the
  // hundredth, so a wait that never looks at the exit ends too, and fails
  // the check instead of hanging the spec.)
  slices = 0;
  asked = 0;
  waited = AwaitHelper([&] { return ++slices > 100; }, [&] { return ++asked >= 4; });
  Check(waited == HelperWait::AppExiting && slices == 4,
        "the wait ends as the app begins to exit, without the helper");
  // A helper that ends in the slice the app begins to exit is a helper that
  // ended: its report is the app's to read.
  waited = AwaitHelper([] { return true; }, [] { return true; });
  Check(waited == HelperWait::Ended, "an ended helper wins over an exit asked in the same slice");
}

RateLimit Limit(std::int64_t retryAfter, std::int64_t reset, bool exhausted, std::int64_t server) {
  RateLimit limit;
  limit.retryAfterSeconds = retryAfter;
  limit.resetUnixSeconds = reset;
  limit.exhausted = exhausted;
  limit.serverUnixSeconds = server;
  return limit;
}

void Schedule() {
  constexpr std::int64_t kCadence = 6 * 60 * 60;
  constexpr std::int64_t kDay = 24 * 60 * 60;

  // asking again after a refusal
  Check(NextCheckDelaySeconds(kCadence, RateLimit{}) == kCadence,
        "a refusal without headers keeps the cadence");
  Check(NextCheckDelaySeconds(0, RateLimit{}) == 0, "no header holds nothing");
  Check(NextCheckDelaySeconds(0, Limit(60, 0, false, kServerUnix)) == 60,
        "Retry-After holds the next request");
  Check(NextCheckDelaySeconds(kCadence, Limit(60, 0, false, kServerUnix)) == kCadence,
        "a short Retry-After does not bring the cadence forward");
  Check(NextCheckDelaySeconds(0, Limit(0, kServerUnix + 1800, true, kServerUnix)) == 1800,
        "a spent hour holds until GitHub's reset, measured on GitHub's clock");
  Check(NextCheckDelaySeconds(0, Limit(0, kServerUnix + 1800, false, kServerUnix)) == 0,
        "a reset time with requests left holds nothing");
  Check(NextCheckDelaySeconds(0, Limit(0, kServerUnix + 1800, true, 0)) == 0,
        "a reset without the server's date is not measured against this machine's clock");
  Check(NextCheckDelaySeconds(0, Limit(60, kServerUnix + 1800, true, kServerUnix)) == 1800,
        "the later of Retry-After and the reset holds");
  Check(NextCheckDelaySeconds(0, Limit(7200, kServerUnix + 1800, true, kServerUnix)) == 7200,
        "the later of the reset and Retry-After holds");
  Check(NextCheckDelaySeconds(0, Limit(10 * kDay, 0, false, kServerUnix)) == kDay,
        "no Retry-After holds checks for more than a day");
  Check(NextCheckDelaySeconds(0, Limit(0, kServerUnix + 10 * kDay, true, kServerUnix)) == kDay,
        "no reset time holds checks for more than a day");
  Check(NextCheckDelaySeconds(0, Limit(0, kServerUnix - 600, true, kServerUnix)) == 0,
        "a reset already past holds nothing");
  Check(NextCheckDelaySeconds(0, Limit(-5, 0, false, kServerUnix)) == 0,
        "a negative Retry-After holds nothing");
  Check(NextCheckDelaySeconds(2 * kDay, RateLimit{}) == 2 * kDay,
        "a cadence longer than a day is kept");

  // saying the checks have not worked
  constexpr std::int64_t kStale = 72 * 60 * 60;
  Check(!CheckIsStale(kServerUnix, kServerUnix - kStale, true), "72 hours exactly is not stale yet");
  Check(CheckIsStale(kServerUnix, kServerUnix - kStale - 1, true), "a second past 72 hours is stale");
  Check(!CheckIsStale(kServerUnix, kServerUnix - 3600, true), "an hour is not stale");
  Check(!CheckIsStale(kServerUnix, kServerUnix - kStale - 1, false),
        "with automatic checks off nothing is said");
  Check(!CheckIsStale(kServerUnix, 0, true), "without a baseline nothing is claimed");
  Check(!CheckIsStale(kServerUnix, kServerUnix + 3600, true),
        "a success after this clock's now (a clock set back) is not stale");

  // the banner's Later
  constexpr std::uint64_t kOffered = 1065506180;
  constexpr std::uint64_t kNewer = 1066946420;
  Check(!HiddenByLater(0, kOffered) && !HiddenByLater(0, kNewer),
        "a launch starts with no release hidden by Later");
  Check(HiddenByLater(kOffered, kOffered), "Later hides the release it was chosen on");
  Check(!HiddenByLater(kOffered, kNewer), "Later does not hide a newer release");
  Check(!HiddenByLater(kNewer, kOffered), "nor an older one");
  Check(!HiddenByLater(0, 0), "and no Later hides no offer");
}

}  // namespace

int main(int argc, char** argv) {
  FeedTable();
  AssetNames();
  RealReleaseList();
  NotOffered();
  ImmutableRequired();
  Prereleases();
  FutureCodes();
  NewestCountsOwnProduct();
  Soak();
  SoakCountsFromTheLastChange();
  PublishedBeforeItWasBuilt();
  TheFeedAsListed();
  OneOfferADay();
  ClickAfterTheCheck();
  MsiVersions(argc > 1 ? argv[1] : nullptr);
  DownloadUrls();
  Redirects();
  InstallLocations();
  Outcomes();
  HelperDecisions();
  Reports();
  HelperProcess();
  Schedule();

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " update-release-tests: " << gCases
            << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
