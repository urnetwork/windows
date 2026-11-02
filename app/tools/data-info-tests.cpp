// Executable spec for the "About your data" sheet (App/DataInfo.h): when the
// free data refreshes (the next 00:00 UTC, whatever the device's zone), the
// countdown to it, the Used, Pending and Available split from a balance, the
// daily amount from the server's start_balance_byte_count, and when the
// banner's refresh line and the upgrade sheet's Wait for refresh show.
//
//   c++ -std=c++20 -I ../src/App data-info-tests.cpp -o /tmp/data-info-tests && /tmp/data-info-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "DataInfo.h"

using urnw::datainfo::BannerLines;
using urnw::datainfo::BannerLinesFor;
using urnw::datainfo::DataInfoFrom;
using urnw::datainfo::FormatRefreshCountdown;
using urnw::datainfo::FreeRefreshCountdown;
using urnw::datainfo::MillisUntilCountdownChanges;
using urnw::datainfo::NextFreeRefreshMillis;
using urnw::datainfo::RefreshCountdown;
using urnw::datainfo::ShowsFreeRefresh;
using urnw::datainfo::UpgradeShowsFreeRefresh;

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

void CheckEq(int64_t want, int64_t got, const std::string& what) {
  Check(want == got, what + ": want " + std::to_string(want) + ", got " + std::to_string(got));
}

void CheckText(const std::string& want, const std::string& got, const std::string& what) {
  Check(want == got, what + ": want \"" + want + "\", got \"" + got + "\"");
}

void CheckCountdown(RefreshCountdown want, RefreshCountdown got, const std::string& what) {
  Check(want == got, what + ": want " + std::to_string(want.hours) + "h " +
                         std::to_string(want.minutes) + "m, got " + std::to_string(got.hours) +
                         "h " + std::to_string(got.minutes) + "m");
}

constexpr int64_t kSecond = 1000;
constexpr int64_t kMinute = 60 * kSecond;
constexpr int64_t kHour = 60 * kMinute;
constexpr int64_t kDay = 24 * kHour;
constexpr int64_t kGib = 1024LL * 1024 * 1024;

// days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// days_from_civil), so the cases read as UTC wall-clock times
constexpr int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

constexpr int64_t Utc(int64_t y, int64_t m, int64_t d, int64_t hour = 0, int64_t minute = 0,
                      int64_t second = 0, int64_t millis = 0) {
  return DaysFromCivil(y, m, d) * kDay + hour * kHour + minute * kMinute + second * kSecond +
         millis;
}

// the provider_connected_duration_hours / _minutes English values
std::string HoursAndMinutes(int64_t hours, int64_t minutes) {
  return std::to_string(hours) + "h " + std::to_string(minutes) + "m";
}

std::string MinutesOnly(int64_t minutes) { return std::to_string(minutes) + "m"; }

std::string Label(int64_t nowMillis) {
  return FormatRefreshCountdown(FreeRefreshCountdown(nowMillis), HoursAndMinutes, MinutesOnly);
}

std::string Gib(int64_t bytes) { return std::to_string(bytes / kGib) + " GiB"; }

std::string Raw(int64_t bytes) { return std::to_string(bytes); }

}  // namespace

int main() {
  // ---- the next refresh is the next 00:00 UTC ----
  {
    CheckEq(Utc(2026, 10, 5), NextFreeRefreshMillis(Utc(2026, 10, 4, 12, 34, 56)), "mid-day");
    CheckEq(Utc(2026, 10, 5), NextFreeRefreshMillis(Utc(2026, 10, 4, 0, 0, 0, 1)), "just after a midnight");
    // just before midnight the refresh is a millisecond away
    CheckEq(Utc(2026, 10, 5), NextFreeRefreshMillis(Utc(2026, 10, 4, 23, 59, 59, 999)), "just before midnight");
    // at midnight it has just happened: the next one is a day away
    CheckEq(Utc(2026, 10, 6), NextFreeRefreshMillis(Utc(2026, 10, 5)), "at midnight");
    CheckEq(Utc(2026, 10, 6), NextFreeRefreshMillis(Utc(2026, 10, 5, 0, 0, 0, 1)), "a millisecond after midnight");
    // month, year and leap-day rollover
    CheckEq(Utc(2026, 11, 1), NextFreeRefreshMillis(Utc(2026, 10, 31, 18)), "month rollover");
    CheckEq(Utc(2027, 1, 1), NextFreeRefreshMillis(Utc(2026, 12, 31, 23, 30)), "year rollover");
    CheckEq(Utc(2028, 2, 29), NextFreeRefreshMillis(Utc(2028, 2, 28, 10)), "into a leap day");
    CheckEq(Utc(2028, 3, 1), NextFreeRefreshMillis(Utc(2028, 2, 29, 10)), "out of a leap day");
    // a clock before 1970 still lands on a UTC midnight
    CheckEq(0, NextFreeRefreshMillis(-1), "the last millisecond of 1969");
    CheckEq(-kDay, NextFreeRefreshMillis(-kDay - 1), "two days before the epoch");
  }

  // ---- the countdown, rounded up to whole minutes ----
  {
    CheckCountdown({5, 12}, FreeRefreshCountdown(Utc(2026, 10, 4, 18, 48)), "18:48 UTC");
    CheckCountdown({23, 59}, FreeRefreshCountdown(Utc(2026, 10, 4, 0, 1)), "00:01 UTC");
    // 20:15 UTC is 3h 45m away whatever the local zone: the input is epoch time
    CheckCountdown({3, 45}, FreeRefreshCountdown(Utc(2026, 10, 4, 20, 15)), "20:15 UTC");
    CheckCountdown({5, 13}, FreeRefreshCountdown(Utc(2026, 10, 4, 18, 47, 30)), "a partial minute rounds up");
    // just before midnight it never reads zero
    CheckCountdown({0, 1}, FreeRefreshCountdown(Utc(2026, 10, 4, 23, 59, 59, 999)), "23:59:59.999");
    CheckCountdown({0, 1}, FreeRefreshCountdown(Utc(2026, 10, 4, 23, 59)), "23:59");
    CheckCountdown({0, 2}, FreeRefreshCountdown(Utc(2026, 10, 4, 23, 58, 59, 999)), "23:58:59.999");
    // at and just after midnight it rolls over to the next day's refresh
    CheckCountdown({24, 0}, FreeRefreshCountdown(Utc(2026, 10, 5)), "00:00");
    CheckCountdown({24, 0}, FreeRefreshCountdown(Utc(2026, 10, 5, 0, 0, 0, 1)), "00:00:00.001");
    CheckCountdown({23, 59}, FreeRefreshCountdown(Utc(2026, 10, 5, 0, 1)), "00:01 the next day");
  }

  // ---- a timer re-armed with MillisUntilCountdownChanges ticks once per
  // displayed minute ----
  {
    CheckEq(kMinute, MillisUntilCountdownChanges(Utc(2026, 10, 4, 18, 48)), "on a minute");
    CheckEq(30 * kSecond, MillisUntilCountdownChanges(Utc(2026, 10, 4, 18, 47, 30)), "mid-minute");
    CheckEq(1, MillisUntilCountdownChanges(Utc(2026, 10, 4, 23, 59, 59, 999)), "just before midnight");
    for (int64_t now : {Utc(2026, 10, 4, 18, 48), Utc(2026, 10, 4, 18, 47, 30, 250),
                        Utc(2026, 10, 4, 23, 59, 59, 999), Utc(2026, 10, 4, 23, 59),
                        Utc(2026, 10, 5)}) {
      const int64_t wake = MillisUntilCountdownChanges(now);
      Check(FreeRefreshCountdown(now) == FreeRefreshCountdown(now + wake - 1),
            "unchanged until the wake at " + std::to_string(now));
      Check(!(FreeRefreshCountdown(now) == FreeRefreshCountdown(now + wake)),
            "changed at the wake at " + std::to_string(now));
    }
  }

  // ---- the countdown reads as a compact duration ----
  {
    CheckText("5h 12m", Label(Utc(2026, 10, 4, 18, 48)), "hours and minutes");
    CheckText("1h 0m", Label(Utc(2026, 10, 4, 23)), "a whole hour");
    CheckText("59m", Label(Utc(2026, 10, 4, 23, 1)), "the last hour");
    CheckText("1m", Label(Utc(2026, 10, 4, 23, 59, 59, 999)), "the last minute");
    CheckText("24h 0m", Label(Utc(2026, 10, 5)), "at midnight");
  }

  // ---- Used, Pending and Available from a balance ----
  {
    auto info = DataInfoFrom(30 * kGib, 20 * kGib, 2 * kGib, Gib);
    CheckText("8 GiB", info.used, "used is what is neither available nor pending");
    CheckText("2 GiB", info.pending, "pending");
    CheckText("20 GiB", info.available, "available");
    CheckText("30 GiB", info.daily, "daily");

    // the reported case: Pending fills the bar and nothing is available
    auto pending = DataInfoFrom(30 * kGib, 0, 29 * kGib, Gib);
    CheckText("1 GiB", pending.used, "all pending: used");
    CheckText("29 GiB", pending.pending, "all pending: pending");
    CheckText("0 GiB", pending.available, "all pending: available");

    // the server samples the values independently: used never goes negative
    auto sampled = DataInfoFrom(kGib, kGib, kGib / 2, Raw);
    CheckText("0", sampled.used, "used clamps at 0");
    CheckText(std::to_string(kGib / 2), sampled.pending, "pending as reported");
    auto negative = DataInfoFrom(0, -1, -1, Raw);
    CheckText("0", negative.pending, "negative pending clamps");
    CheckText("0", negative.available, "negative available clamps");
  }

  // ---- the daily amount is the server's start balance, never a constant ----
  {
    for (int64_t start : {30 * kGib, 60 * kGib, 33 * kGib, 10 * 1024 * kGib, int64_t{12'345'678'901}}) {
      CheckText(Raw(start), DataInfoFrom(start, 0, 0, Raw).daily, "daily " + Raw(start));
    }
  }

  // ---- when each entry point shows ----
  {
    Check(ShowsFreeRefresh(false), "the sheet's refresh line shows without Pro");
    Check(!ShowsFreeRefresh(true), "Pro gets no free grant: no refresh line");

    Check(BannerLinesFor(true) == BannerLines{true}, "out of balance: the refresh leads");
    Check(BannerLinesFor(false) == BannerLines{false}, "funded or at a data limit: no refresh line");

    Check(UpgradeShowsFreeRefresh(true, false), "a blocked connect's upgrade sheet shows the refresh");
    Check(!UpgradeShowsFreeRefresh(false, false), "Get Pro and the other entries do not");
    Check(!UpgradeShowsFreeRefresh(true, true), "never for Pro");
  }

  std::cout << (gCases - gFailures) << "/" << gCases << " data info checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
