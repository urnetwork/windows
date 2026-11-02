// The "About your data" sheet: what Used, Pending and Available mean, the
// daily balance and when the free data refreshes.
//
// The sheet explains the usage bar: Used, Pending (balance held by open
// connections, returned when they close) and Available, the daily balance the
// server reports (start_balance_byte_count, never a hard-coded amount), and
// when the free data refreshes. The server grants the free daily balance at
// 00:00 UTC to every network without Pro (RefreshFreeTransferBalances), so the
// refresh is the next UTC midnight after the wall clock the caller passes in.
// It opens from the info button by the daily balance on Account and from the
// Why? link in the out-of-balance banner.
//
// Pure, with no Windows headers or clocks (the caller passes the time in epoch
// milliseconds, and the strings and byte format as functions), so
// tools/data-info-tests.cpp runs it on any host with a C++20 compiler.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>

#include "BalanceGate.h"

namespace urnw::datainfo {

inline constexpr int64_t kDayMillis = 24LL * 60 * 60 * 1000;
inline constexpr int64_t kMinuteMillis = 60'000;

// The next free data refresh: the first 00:00 UTC strictly after nowMillis.
// Epoch time has no leap seconds, so UTC days are whole multiples of a day.
constexpr int64_t NextFreeRefreshMillis(int64_t nowMillis) {
  int64_t day = nowMillis / kDayMillis;
  if (nowMillis % kDayMillis < 0) {
    // division truncates toward zero; a time before 1970 belongs to the day before
    --day;
  }
  return (day + 1) * kDayMillis;
}

struct RefreshCountdown {
  int64_t hours = 0;
  int64_t minutes = 0;
  constexpr bool operator==(const RefreshCountdown&) const = default;
};

// The time left until the next free data refresh, rounded up to whole minutes
// so it never reads zero while the refresh is still ahead.
constexpr RefreshCountdown FreeRefreshCountdown(int64_t nowMillis) {
  const int64_t remaining = NextFreeRefreshMillis(nowMillis) - nowMillis;
  const int64_t totalMinutes = (remaining + kMinuteMillis - 1) / kMinuteMillis;
  return {totalMinutes / 60, totalMinutes % 60};
}

// How long until FreeRefreshCountdown shows a different value, so a timer
// re-armed with it ticks once per displayed minute.
constexpr int64_t MillisUntilCountdownChanges(int64_t nowMillis) {
  const int64_t remaining = NextFreeRefreshMillis(nowMillis) - nowMillis;
  return (remaining - 1) % kMinuteMillis + 1;
}

// The countdown as a compact duration ("5h 12m", or "12m" in the last hour),
// with the provider_connected_duration_hours / _minutes strings.
template <typename HoursAndMinutes, typename MinutesOnly>
auto FormatRefreshCountdown(RefreshCountdown countdown, HoursAndMinutes hoursAndMinutes,
                            MinutesOnly minutesOnly) {
  return 0 < countdown.hours ? hoursAndMinutes(countdown.hours, countdown.minutes)
                             : minutesOnly(countdown.minutes);
}

template <typename Text>
struct DataInfo {
  Text used;
  Text pending;
  Text available;
  Text daily;
};

// The sheet's amounts from the balance, split the way the usage bar splits
// it: used is start - available - pending, clamped at 0 (the server samples
// the values independently, so the raw difference can go negative).
template <typename FormatBytes>
auto DataInfoFrom(int64_t startBalanceByteCount, int64_t availableByteCount,
                  int64_t pendingByteCount, FormatBytes formatBytes) {
  const int64_t available = availableByteCount < 0 ? 0 : availableByteCount;
  const int64_t pending = pendingByteCount < 0 ? 0 : pendingByteCount;
  const int64_t start = startBalanceByteCount < 0 ? 0 : startBalanceByteCount;
  const int64_t rawUsed = start - available - pending;
  const int64_t used = rawUsed < 0 ? 0 : rawUsed;
  using Text = decltype(formatBytes(start));
  return DataInfo<Text>{formatBytes(used), formatBytes(pending), formatBytes(available),
                        formatBytes(start)};
}

// Whether the sheet says when the free data refreshes. Pro networks get the
// Pro grant instead of the free daily one, so the line would not apply.
constexpr bool ShowsFreeRefresh(bool pro) { return !pro; }

struct BannerLines {
  // "Free data refreshes in {time}." first, with the Why? link to the sheet
  bool refresh = false;
  constexpr bool operator==(const BannerLines&) const = default;
};

// The out-of-balance banner leads with when the free data refreshes whenever
// it is open (balance::OutOfBalance), so Get Pro does not read as the only way
// back. What the traffic is doing comes after it (balance::TrafficLineFor).
constexpr BannerLines BannerLinesFor(bool outOfBalance) { return {outOfBalance}; }

// The string for what the traffic is doing, in the banner and the tray notice.
// No session: an out-of-balance account is asked for balance, and a data limit
// has nothing to ask for (nullptr, no line).
constexpr const char* TrafficLineKey(bool dataLimit, balance::TrafficLine line) {
  switch (line) {
    case balance::TrafficLine::Held:
      return dataLimit ? "data_limit_held_notice" : "insufficient_balance_held_notice";
    case balance::TrafficLine::Blocked:
      return "conn_traffic_blocked";
    case balance::TrafficLine::NotProtected:
      return "conn_not_protected";
    case balance::TrafficLine::None:
      break;
  }
  return dataLimit ? nullptr : "insufficient_balance_message";
}

// The alert's title, in the banner and the tray notice. A refused start waiting
// on the balance (balance::BannerOpen) reads as out of balance.
constexpr const char* AlertTitleKey(bool dataLimit) {
  return dataLimit ? "data_limit_reached" : "insufficient_balance";
}

// Whether the upgrade sheet leads with when the free data refreshes and offers
// Wait for refresh: only when a start connect blocked by the balance opened it
// (AppController::ShowUpgradeForBlockedConnect). Pro is never blocked, and
// gets no free grant.
constexpr bool UpgradeShowsFreeRefresh(bool openedByBlockedConnect, bool pro) {
  return openedByBlockedConnect && !pro;
}

}  // namespace urnw::datainfo
