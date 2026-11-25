// The elevated update helper's report: what it did with one release, written
// as <install folder>\updates\last-result.json and read by the tray app.
//
// The helper writes the file when it ends, whether msiexec ran or the helper
// refused first, replacing the previous one atomically. It sits in the
// admin-only install folder (InstallLocation.h), with an ACL of its own that
// only an administrator can change, and every user can read it. The tray app
// reads it when the helper it waited on ends, and on its next launch, since
// the installer usually closes the app before the helper finishes. It shows a
// report only while the report still says something about the build that
// reads it (ViewOfReport).
//
//   {"tag":"v2026.10.1-1060587890","code":1060587890,"exitCode":0,
//    "finishedUtc":"2026-10-01T14:51:17Z"}
//
// exitCode is msiexec's own exit code when msiexec ran, and otherwise one of
// the helper's refusals (Refusal), which set the Win32 customer bit no msiexec
// code carries. It is also the helper's process exit code.
//
// Pure, header-only and free of JSON: tools/update-release-tests.cpp runs it
// on any host. UpdateResultJson.h reads and writes the file.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "VersionGrammar.h"

namespace urnw::update {

// The helper's own refusals, before or instead of msiexec.
enum class Refusal : std::uint32_t {
  // Started without administrator rights.
  NotElevated = 0x20000001,
  // Its own executable is not in an admin-only install location.
  NotInstalled = 0x20000002,
  // A dev build (code 0), which never updates itself.
  DevBuild = 0x20000003,
  // The arguments are not --apply-update and a tag of the channel's feed.
  BadArguments = 0x20000004,
  // Another update helper is running.
  Busy = 0x20000005,
  // The release list could not be fetched or read.
  ReleaseList = 0x20000006,
  // The release list does not offer the tag it was asked for, above this
  // build's own code.
  NotOffered = 0x20000007,
  // The release's download URL, its redirect, or the download itself failed.
  Download = 0x20000008,
  // The download does not match GitHub's SHA-256 for it.
  Digest = 0x20000009,
  // The package's UpgradeCode or ProductVersion is not the release's.
  Package = 0x2000000A,
  // The download folder could not be prepared, or something there was a
  // reparse point.
  Staging = 0x2000000B,
  // msiexec could not be started.
  InstallerNotStarted = 0x2000000C,
  // GitHub refused the release list (403 or 429): it limits the requests
  // each network may make, and this one has spent them for now.
  RateLimited = 0x2000000D,
  // The release's SHA256SUMS.sig does not verify under the release key, the
  // installer is not its line in the signed SHA256SUMS, or the release has
  // no signature while one is required (UpdateSignature.h).
  Signature = 0x2000000E,
};

// The Win32 customer bit: set on every Refusal, never on a msiexec code.
inline constexpr std::int64_t kRefusalBit = 0x20000000;

inline constexpr bool IsRefusal(std::int64_t exitCode) {
  return exitCode > 0 && exitCode <= 0xFFFFFFFF && (exitCode & kRefusalBit) != 0;
}

// ERROR_SUCCESS_REBOOT_INITIATED and ERROR_SUCCESS_REBOOT_REQUIRED.
inline constexpr std::int64_t kRebootInitiated = 1641;
inline constexpr std::int64_t kRebootRequired = 3010;

// What an update's exit code means for the user.
enum class Outcome {
  // Installed; the app that reads this is the new version.
  Installed,
  // Installed, but files that were in use are replaced at the next restart:
  // the new product is registered and its service runs, while the old files
  // stay on disk until then. Not a failure.
  RestartRequired,
  // msiexec failed; the previous version is still installed.
  Failed,
  // The helper refused before or instead of msiexec; nothing was installed.
  Refused,
};

inline constexpr Outcome OutcomeOf(std::int64_t exitCode) {
  if (exitCode == 0) return Outcome::Installed;
  if (exitCode == kRebootRequired || exitCode == kRebootInitiated) return Outcome::RestartRequired;
  if (IsRefusal(exitCode)) return Outcome::Refused;
  return Outcome::Failed;
}

// Whether the helper keeps the package as the product's repair source: after
// an install that took, and only then.
inline constexpr bool KeepsPackage(std::int64_t exitCode) {
  const Outcome outcome = OutcomeOf(exitCode);
  return outcome == Outcome::Installed || outcome == Outcome::RestartRequired;
}

struct UpdateResult {
  std::string tag;
  std::uint64_t code = 0;
  std::int64_t exitCode = 0;
  // ISO 8601 UTC, to the second: 2026-10-01T14:51:17Z
  std::string finishedUtc;
};

// Whether `text` is an ISO 8601 UTC second, YYYY-MM-DDThh:mm:ssZ.
inline constexpr bool IsUtcSecond(std::string_view text) {
  constexpr std::string_view kShape = "0000-00-00T00:00:00Z";
  if (text.size() != kShape.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const bool digit = text[i] >= '0' && text[i] <= '9';
    if (kShape[i] == '0' ? !digit : text[i] != kShape[i]) return false;
  }
  return true;
}

// Whether a report read from the file is one the helper could have written: a
// tag the release grammar reads (after a runner test feed's prefix,
// `tagPrefix`) with that code, an exit code a process can return, and a UTC
// second.
inline bool IsWellFormed(const UpdateResult& result, std::string_view tagPrefix = {}) {
  std::string_view tag = result.tag;
  if (tag.substr(0, tagPrefix.size()) != tagPrefix) return false;
  tag.remove_prefix(tagPrefix.size());
  return result.code != 0 && version::ParseReleaseCode(tag) == result.code &&
         result.exitCode >= 0 && result.exitCode <= 0xFFFFFFFF && IsUtcSecond(result.finishedUtc);
}

// How many days `month` (1 to 12) has in `year` of the Gregorian calendar.
inline constexpr std::int64_t DaysInMonth(std::int64_t year, std::int64_t month) {
  constexpr std::int64_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return month == 2 && leap ? 29 : kDays[month - 1];
}

// The Unix second an ISO 8601 UTC second names, the inverse of
// UpdateApply.h FormatUtcSecond; nullopt when `text` is not one, a day its
// month does not have included (2026-02-31 would otherwise read as a day in
// March).
inline std::optional<std::int64_t> ParseUtcSecond(std::string_view text) {
  if (!IsUtcSecond(text)) return std::nullopt;
  const auto number = [text](std::size_t at, std::size_t digits) {
    std::int64_t value = 0;
    for (std::size_t i = 0; i < digits; ++i) value = value * 10 + (text[at + i] - '0');
    return value;
  };
  const std::int64_t year = number(0, 4);
  const std::int64_t month = number(5, 2);
  const std::int64_t day = number(8, 2);
  const std::int64_t hour = number(11, 2);
  const std::int64_t minute = number(14, 2);
  const std::int64_t second = number(17, 2);
  if (month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 || second > 59) {
    return std::nullopt;
  }
  if (day > DaysInMonth(year, month)) return std::nullopt;
  // days since 1970-01-01 (Howard Hinnant's days_from_civil)
  const std::int64_t y = year - (month <= 2 ? 1 : 0);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int64_t yearOfEra = y - era * 400;
  const std::int64_t dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const std::int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  const std::int64_t days = era * 146097 + dayOfEra - 719468;
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

// How far before the helper's start its report may say it finished and still
// be its own: the clock a moment set back between the two.
inline constexpr std::int64_t kReportClockSlackSeconds = 120;

// Whether the report read once the helper ended is that helper's own: its
// tag, the helper's process exit code, and written no earlier than the helper
// started. The earliest refusals (NotElevated, NotInstalled, DevBuild,
// BadArguments, Busy, a Staging before the folder exists) write none, and
// leave an earlier attempt's report in place, with its own reason and log.
inline bool IsReportOfRun(const UpdateResult& report, std::string_view tag, std::int64_t exitCode,
                          std::int64_t startedUnixSeconds) {
  const std::optional<std::int64_t> finished = ParseUtcSecond(report.finishedUtc);
  return report.tag == tag && report.exitCode == exitCode && finished &&
         *finished >= startedUnixSeconds - kReportClockSlackSeconds;
}

// What the tray app says about a report.
enum class ReportView {
  // Nothing: the report is about a release this build has passed, or what it
  // asked for has happened.
  Hidden,
  // "Updated to v...": this build is the release the report installed.
  Installed,
  // The update installed while this older app still ran: it is the new
  // version once URnetwork starts again.
  RestartApp,
  // Installed up to a restart of Windows that has not happened yet (3010).
  RestartWindows,
  // Failed or refused, and this build is still older than the release.
  NotInstalled,
};

// The view of `report` for a build of `ownCode`. `live`: this app started the
// helper that wrote it and waited on it, so it predates the update.
// `restartedSince`: Windows has started since the report was written. A report
// is shown only while it is still true for this build: a 3010 stops asking for
// a restart once there has been one, an install shows as installed only in the
// release it installed, and a failure only while this build is still older
// than the release that failed.
inline ReportView ViewOfReport(const UpdateResult& report, std::uint64_t ownCode, bool live,
                               bool restartedSince) {
  switch (OutcomeOf(report.exitCode)) {
    case Outcome::Installed:
      if (ownCode == report.code) return ReportView::Installed;
      return live && ownCode < report.code ? ReportView::RestartApp : ReportView::Hidden;
    case Outcome::RestartRequired:
      if (ownCode > report.code) return ReportView::Hidden;
      if (!restartedSince) return ReportView::RestartWindows;
      return ownCode == report.code ? ReportView::Installed : ReportView::Hidden;
    case Outcome::Failed:
    case Outcome::Refused:
      return ownCode < report.code ? ReportView::NotInstalled : ReportView::Hidden;
  }
  return ReportView::Hidden;
}

}  // namespace urnw::update
