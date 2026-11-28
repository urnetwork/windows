// Everything the Connect page's sharing limits decide before they touch a XAML
// object (sdk provide_share.go; owner decision 2026-11-28, feature 10): the
// values the two pickers list, which of them is selected, the whole Mbps a
// speed is labelled with, and which line says how the month stands.
//
// The data is the device's provide share status (urnet::ProvideShareStatus):
// the limits in force, the data this device shared with others in the current
// calendar month, when the month ends and whether its data reached the monthly
// limit. A limit of 0 is no limit. The device keeps the limits in its network
// space, which the service's provider-only device reads as well.
//
// It is all here, and all pure, for the reason ExtenderPresentation.h gives:
// the windows solution has no test project and a WinUI 3 app cannot be built
// off Windows, so tools/provide-share-tests.cpp verifies every decision on any
// host with a C++20 compiler. StatusLineFor is a template over the status's
// field names, so the tests run it on a plain mirror, and again on the
// generated header's urnet::ProvideShareStatus when one is available;
// ConnectPage.cpp passes the sdk's struct straight in.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace urnw::provideshare {

// A megabit per second, in the bytes per second a speed limit is kept in.
inline constexpr int64_t kBytesPerSecondPerMbps = 125'000;
// A GiB, in the bytes a monthly limit is kept in.
inline constexpr int64_t kBytesPerGib = 1024LL * 1024 * 1024;

// The store's choices after No limit: speeds in Mbps, monthly limits in GiB.
inline constexpr std::array<int64_t, 7> kSpeedMbps{1, 2, 5, 10, 20, 50, 100};
inline constexpr std::array<int64_t, 8> kMonthlyGib{5, 10, 25, 50, 100, 250, 500, 1024};

// The values a picker lists, ascending: 0, No limit, then each choice in
// `unit`, with the limit in force added in its place when it is none of them
// (a limit the cli set). A negative limit is no limit, as the sdk keeps it.
template <std::size_t N>
std::vector<int64_t> ChoicesFor(const std::array<int64_t, N>& choices, int64_t unit,
                                int64_t current) {
  std::vector<int64_t> values;
  values.reserve(N + 2);
  values.push_back(0);
  for (const int64_t choice : choices) values.push_back(choice * unit);
  if (0 < current && std::find(values.begin(), values.end(), current) == values.end()) {
    values.insert(std::upper_bound(values.begin(), values.end(), current), current);
  }
  return values;
}

// The speed picker's values, in bytes per second.
inline std::vector<int64_t> SpeedChoices(int64_t maxBytesPerSecond) {
  return ChoicesFor(kSpeedMbps, kBytesPerSecondPerMbps, maxBytesPerSecond);
}

// The monthly picker's values, in bytes.
inline std::vector<int64_t> MonthlyChoices(int64_t monthlyByteCount) {
  return ChoicesFor(kMonthlyGib, kBytesPerGib, monthlyByteCount);
}

// Where `value` is in `choices`, or No limit's place, 0, when it is not there.
inline std::size_t IndexOf(const std::vector<int64_t>& choices, int64_t value) {
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (choices[i] == value) return i;
  }
  return 0;
}

// A speed in the whole Mbps its choice is labelled with: a speed the cli set
// between two choices reads as the nearer whole Mbps.
inline int64_t SpeedMbps(int64_t bytesPerSecond) {
  return std::llround(static_cast<double>(bytesPerSecond) /
                      static_cast<double>(kBytesPerSecondPerMbps));
}

// The line under the pickers.
enum class StatusLine {
  // provide_share_limit_reached: the month's data reached the monthly limit,
  // so sharing with others waits for the month's end
  LimitReached,
  // provide_share_used_of_limit: the month's data against the monthly limit
  UsedOfLimit,
  // provide_share_used: the month's data, with no monthly limit
  Used,
};

template <typename Status>
constexpr StatusLine StatusLineFor(const Status& status) {
  if (status.budget_reached) return StatusLine::LimitReached;
  if (0 < status.monthly_byte_count) return StatusLine::UsedOfLimit;
  return StatusLine::Used;
}

}  // namespace urnw::provideshare
