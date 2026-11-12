// Everything the Earnings provider status decides before it touches a XAML
// object (support part P008, phase 2): the Demand histogram's 60 bars, its
// total and whether it is empty; which of loading, unavailable, empty and bars
// the demand area shows and whether Why? does; the line under the provide mode
// row, which merges the local idle reason (ProviderIdleReason.h) with the
// server's; and the Why? rows, each ranking number as a label, a value and a
// help line, with the lines of the reason the network checks downgraded the
// device (server FP2-PROTOCOL.md §5.5).
//
// The data is the sdk's ProviderStatusViewController's, which reads
// GET /network/provider-status about once a minute: how often the network
// offered this device to clients in each minute of the last hour, the numbers
// the provider search ranks it by, and the first reason holding it back. With
// no session, when the service's provider-only device provides, it is the same
// answer read on the api (Readings).
//
// It is all here, and all pure, for the reason ExtenderPresentation.h gives:
// the windows solution has no test project and a WinUI 3 app cannot be built
// off Windows, so tools/provider-status-tests.cpp verifies every decision on
// any host with a C++20 compiler. The functions that read the sdk's structs
// are templates over their field names (urnet::ProviderStatus,
// ProviderRankingNumber, ProviderStatusCountry), so the tests run them on
// plain mirrors, and again on the generated header's own types when one is
// available; WalletPage.cpp passes the sdk's structs straight in.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ProviderIdleReason.h"

namespace urnw::providerstatus {

// ---- the server's reasons (urnet::ProviderStatusReason*) --------------------

// passes every check: no line of its own (3.1)
inline constexpr std::string_view kReasonNone = "none";

// The store key of a reason code, provider_status_reason_<code>, or "" for a
// code this build does not know: the line then shows the server's English
// reason_text, so a new server reason is still explained.
constexpr const char* ReasonKey(std::string_view code) {
  if (code == "not_providing") return "provider_status_reason_not_providing";
  if (code == "not_connected") return "provider_status_reason_not_connected";
  if (code == "location_invalid") return "provider_status_reason_location_invalid";
  if (code == "network_only") return "provider_status_reason_network_only";
  if (code == "reliability_warming_up") return "provider_status_reason_reliability_warming_up";
  if (code == "reliability_low") return "provider_status_reason_reliability_low";
  if (code == "not_eligible") return "provider_status_reason_not_eligible";
  if (code == "egress_unprobed") return "provider_status_reason_egress_unprobed";
  if (code == "egress_failing") return "provider_status_reason_egress_failing";
  if (code == "speed_test_missing") return "provider_status_reason_speed_test_missing";
  if (code == "slow") return "provider_status_reason_slow";
  if (code == "none") return "provider_status_reason_none";
  return "";
}

// ---- the Demand histogram ----------------------------------------------------

inline constexpr std::size_t kBarCount = 60;

struct Histogram {
  // each bar's height as a share of the chart's, oldest first; the last bar is
  // the current, partial minute
  std::array<double, kBarCount> fractions{};
  int64_t total = 0;
  int64_t maxCount = 0;
  // every count is 0: the baseline and the axis stay, with "not offered"
  bool empty = true;
};

// The bars from the controller's counts per minute, oldest first. The server
// sends 60; a longer list keeps its newest 60 and a shorter one is padded on
// the old side, so the last bar is always the current minute. A bar is its
// count over the largest count (at least 1, so all zero draws no bar), and a
// negative count is 0.
inline Histogram HistogramFor(const std::vector<int64_t>& countsOldestFirst) {
  Histogram histogram;
  const std::size_t size = countsOldestFirst.size();
  const std::size_t skip = size > kBarCount ? size - kBarCount : 0;
  const std::size_t pad = size < kBarCount ? kBarCount - size : 0;
  std::array<int64_t, kBarCount> counts{};
  for (std::size_t i = skip; i < size; ++i) {
    const int64_t count = countsOldestFirst[i] < 0 ? 0 : countsOldestFirst[i];
    counts[pad + i - skip] = count;
    histogram.total += count;
    if (histogram.maxCount < count) histogram.maxCount = count;
  }
  const double scale = static_cast<double>(histogram.maxCount < 1 ? 1 : histogram.maxCount);
  for (std::size_t i = 0; i < kBarCount; ++i) {
    histogram.fractions[i] = static_cast<double>(counts[i]) / scale;
  }
  histogram.empty = histogram.maxCount == 0;
  return histogram;
}

// ---- the states --------------------------------------------------------------

enum class DemandArea {
  Hidden,       // a poll succeeded, but it has no status for this device
  Loading,      // before the first poll finishes: `loading`
  Unavailable,  // no poll succeeded and one failed, or the status has no histogram
  Empty,        // every count is 0: the baseline, the axis and "not offered"
  Bars,         // the 60 bars and the total
};

struct Sections {
  DemandArea area = DemandArea::Loading;
  // the Why? disclosure shows (it needs this device's status)
  bool why = false;
  // the line under the provide mode row may use the server's reason (LineFor)
  bool serverReason = false;

  bool operator==(const Sections&) const = default;
};

// The states table: before a poll succeeds only the local line shows, with
// `loading` (no error yet) or `provider_status_unavailable` (a poll failed, for
// example the 404 before the route is deployed) in place of the chart. Once one
// succeeds the last snapshot stays, whatever a later poll does: no status for
// this device hides the area; a status without a histogram is unavailable;
// otherwise all zero is the empty chart and anything else the bars. With a
// status the line merges the server's reason and Why? shows.
constexpr Sections SectionsFor(bool loaded, bool fetchFailed, bool hasStatus,
                               bool hasAppearances, bool allZero) {
  if (!loaded) return {fetchFailed ? DemandArea::Unavailable : DemandArea::Loading, false, false};
  if (!hasStatus) return {DemandArea::Hidden, false, false};
  if (!hasAppearances) return {DemandArea::Unavailable, true, true};
  return {allZero ? DemandArea::Empty : DemandArea::Bars, true, true};
}

// The histogram and the sections from the controller's three readings
// (getIsLoaded, a failed poll or no controller at all, getProviderStatus).
struct View {
  Histogram histogram;
  Sections sections;
};

template <typename Status>
View ViewFor(bool loaded, bool fetchFailed, const std::optional<Status>& status) {
  View view;
  const bool hasAppearances = status && status->appearances.has_value();
  if (hasAppearances && status->appearances->appearances_per_minute) {
    view.histogram = HistogramFor(*status->appearances->appearances_per_minute);
  }
  view.sections =
      SectionsFor(loaded, fetchFailed, status.has_value(), hasAppearances, view.histogram.empty);
  return view;
}

// ---- a status read on the api ------------------------------------------------

// The three readings ViewFor takes, for a device the sdk cannot open a
// ProviderStatusViewController on. The sdk opens one only on a device, and
// while disconnected the provider is the service's provider-only device, so
// SdkHost reads the same GET /network/provider-status on the api and keeps the
// readings here, applying each answer exactly as the controller applies a poll
// (fetchDone): a failure keeps the last snapshot and records the error; an
// answer marks the readings loaded, clears the error and takes the first row
// whose client id is this device's, or none when the network's list has no
// row for it. A template over the sdk's result and status, like ViewFor.
template <typename Status>
struct Readings {
  bool loaded = false;
  // the last failed poll's error, "" once a poll succeeds
  std::string error;
  std::optional<Status> status;

  // A failure always reads as one, even with no message.
  void Failed(std::string_view message) {
    error = message.empty() ? std::string("no provider status") : std::string(message);
  }

  template <typename Result>
  void Answered(const Result& result, std::string_view clientId) {
    loaded = true;
    error.clear();
    status.reset();
    if (!result.providers || clientId.empty()) return;
    for (const auto& candidate : *result.providers) {
      if (candidate.client_id && *candidate.client_id == clientId) {
        status = candidate;
        return;
      }
    }
  }

  // One callback's arguments (urnet::GetProviderStatusCallback): an error, no
  // result at all (the controller's "no provider status"), or the answer.
  template <typename Result>
  void Fetched(const std::optional<Result>& result, const std::optional<std::string>& fetchError,
               std::string_view clientId) {
    if (fetchError) {
      Failed(*fetchError);
    } else if (!result) {
      Failed({});
    } else {
      Answered(*result, clientId);
    }
  }
};

// ---- the line under the provide mode row -------------------------------------

struct Line {
  // a store key, or "" when the line is the server's text or nothing
  std::string key;
  // the server's English reason_text, for a reason code this build does not know
  std::string text;

  bool shown() const { return !key.empty() || !text.empty(); }
  bool operator==(const Line&) const = default;
};

// Local state wins because it is immediate: the server caches its decision
// for about five minutes, so right after a mode change it can still say
// network_only. Then the server's reason, unless it is `none` (a code this
// build does not know shows its reason_text, and with no text counts as
// none); then "no traffic yet". `serverReason` is "" without a status for
// this device.
inline Line LineFor(provideridle::ProviderIdleReason idle, std::string_view serverReason,
                    std::string_view serverReasonText) {
  using provideridle::ProviderIdleReason;
  switch (idle) {
    case ProviderIdleReason::AutoNotConnected:
    case ProviderIdleReason::NetworkOnly:
    case ProviderIdleReason::PausedWifiOnly:
    case ProviderIdleReason::PausedNoNetwork:
      return {provideridle::ProviderIdleReasonKey(idle), {}};
    case ProviderIdleReason::NoTrafficYet:
    case ProviderIdleReason::None:
      break;
  }
  if (!serverReason.empty() && serverReason != kReasonNone) {
    if (const char* key = ReasonKey(serverReason); *key) return {key, {}};
    if (!serverReasonText.empty()) return {{}, std::string(serverReasonText)};
  }
  if (idle == ProviderIdleReason::NoTrafficYet) {
    return {provideridle::ProviderIdleReasonKey(idle), {}};
  }
  return {};
}

// ---- the Why? rows -----------------------------------------------------------

// The labels of the sdk's reason lines (urnet::ProviderStatusReasonLine*): the
// network checks took UDP away, so the device is offered for TCP traffic only,
// or took IPv6 away, so it is offered for IPv4 traffic only (server
// FP2-PROTOCOL.md §5.5).
inline constexpr std::string_view kReasonLineTcp = "tcp";
inline constexpr std::string_view kReasonLineIpv4 = "ipv4";

template <typename Text>
struct NumberRow {
  std::string labelKey;
  Text value;
  std::string helpKey;
  // false tints the value amber: the number holds the device back
  bool passes = true;
  // under the help line, why the network checks downgraded the device, in
  // the reader's language (ReasonsFor)
  std::vector<Text> reasons;
};

// How a row's value becomes text: the store's strings and the platform's
// number formats. WalletPage passes urnw::Localized and Format, the locale's
// percent and the app's byte rate; the tests pass English.
template <typename Text>
struct ValueText {
  std::function<Text(std::string_view key)> localized;
  // a key with two text placeholders (provider_status_value_with_minimum, _with_maximum)
  std::function<Text(std::string_view key, const Text&, const Text&)> format;
  // a key with two integer placeholders (provider_status_value_count_of_total)
  std::function<Text(std::string_view key, int64_t, int64_t)> formatCounts;
  // a plural key on its one integer placeholder (provider_status_reason_line_*)
  std::function<Text(std::string_view key, int64_t count)> plural;
  // a 0-1 ratio as a whole percent in the reader's locale ("82%")
  std::function<Text(double ratio)> percent;
  // bytes per second in the app's byte rate ("3.4 MiB/s")
  std::function<Text(double bytesPerSecond)> rate;
  // text the store does not translate: digits, "35 ms", a country name
  std::function<Text(const std::string& utf8)> plain;
};

// reliability_lookback_<n>: a reliability over a window this build does not
// name, shown under the generic Reliability label
constexpr bool IsReliabilityLookback(std::string_view name) {
  constexpr std::string_view kPrefix = "reliability_lookback_";
  if (!name.starts_with(kPrefix) || name.size() == kPrefix.size()) return false;
  for (char c : name.substr(kPrefix.size())) {
    if (c < '0' || '9' < c) return false;
  }
  return true;
}

// "35 ms": the delay is not localized (the store has no key for it)
inline std::string Milliseconds(double value) {
  return std::to_string(std::llround(value)) + " ms";
}

// The lines the sdk derived from a number's reason, in its order: tcp and
// ipv4 as the store's plurals with their counts, any other label as the
// server's English, and a line with neither left out. None without lines.
template <typename Text, typename Number>
std::vector<Text> ReasonsFor(const Number& number, const ValueText<Text>& text) {
  std::vector<Text> reasons;
  if (!number.reason_lines) return reasons;
  for (const auto& line : *number.reason_lines) {
    const int64_t count = line.count ? *line.count : 0;
    if (line.label == kReasonLineTcp) {
      reasons.push_back(text.plural("provider_status_reason_line_tcp", count));
    } else if (line.label == kReasonLineIpv4) {
      reasons.push_back(text.plural("provider_status_reason_line_ipv4", count));
    } else if (line.text && !line.text->empty()) {
      reasons.push_back(text.plain(*line.text));
    }
  }
  return reasons;
}

// A row for one ranking number, or nullopt for a name this build does not know
// (skipped: the server may add numbers before the apps name them). Missing
// values read "No history yet" for a reliability and "Not yet" for a
// measurement; a value with the selection's floor or ceiling carries it.
template <typename Text, typename Number>
std::optional<NumberRow<Text>> NumberRowFor(const Number& number, const ValueText<Text>& text) {
  const std::string_view name = number.name;
  NumberRow<Text> row;
  row.passes = number.passes;
  const auto withMinimum = [&](const Text& value, const Text& minimum) {
    return number.has_minimum ? text.format("provider_status_value_with_minimum", value, minimum)
                              : value;
  };
  const auto withMaximum = [&](const Text& value, const Text& maximum) {
    return number.has_maximum ? text.format("provider_status_value_with_maximum", value, maximum)
                              : value;
  };
  const auto noHistory = [&] { return text.localized("provider_status_value_no_history"); };
  const auto notYet = [&] { return text.localized("provider_status_value_not_yet"); };

  if (name == "reliability_5m") {
    row.labelKey = "provider_status_number_reliability_5m";
    row.helpKey = "provider_status_help_reliability_5m";
    row.value = number.has_value ? text.percent(number.value) : noHistory();
  } else if (name == "reliability_1h" || name == "reliability_12h" ||
             IsReliabilityLookback(name)) {
    if (name == "reliability_1h") {
      row.labelKey = "provider_status_number_reliability_1h";
      row.helpKey = "provider_status_help_reliability_1h";
    } else if (name == "reliability_12h") {
      row.labelKey = "provider_status_number_reliability_12h";
      row.helpKey = "provider_status_help_reliability_12h";
    } else {
      row.labelKey = "reliability";
      row.helpKey = "provider_status_help_reliability_other";
    }
    row.value = number.has_value
                    ? withMinimum(text.percent(number.value), text.percent(number.minimum))
                    : noHistory();
  } else if (name == "url_checks") {
    row.labelKey = "provider_status_number_url_checks";
    row.helpKey = "provider_status_help_url_checks";
    row.value = number.has_value
                    ? withMinimum(text.formatCounts("provider_status_value_count_of_total",
                                                    static_cast<int64_t>(number.count),
                                                    static_cast<int64_t>(number.total)),
                                  text.percent(number.minimum))
                    : notYet();
  } else if (name == "speed_test") {
    // bytes per second, against the slowest the selection accepts
    row.labelKey = "provider_status_number_speed_test";
    row.helpKey = "provider_status_help_speed_test";
    row.value = number.has_value
                    ? withMinimum(text.rate(number.value), text.rate(number.minimum))
                    : notYet();
  } else if (name == "latency") {
    // milliseconds above the delay expected for the location
    row.labelKey = "provider_status_number_latency";
    row.helpKey = "provider_status_help_latency";
    row.value = number.has_value ? withMaximum(text.plain(Milliseconds(number.value)),
                                               text.plain(Milliseconds(number.maximum)))
                                 : notYet();
  } else if (name == "weight_quality" || name == "weight_speed") {
    row.labelKey = name == "weight_quality" ? "provider_status_number_weight_quality"
                                            : "provider_status_number_weight_speed";
    row.helpKey = "provider_status_help_weight";
    row.value = number.passes ? text.plain(std::format("{:.2f}", number.value))
                              : text.localized("provider_status_value_not_in_pool");
  } else if (name == "tier_quality" || name == "tier_speed") {
    // 0 is best; 3 is past the speed or delay cutoff
    row.labelKey = name == "tier_quality" ? "provider_status_number_tier_quality"
                                          : "provider_status_number_tier_speed";
    row.helpKey = "provider_status_help_tier";
    row.value = number.has_value ? text.plain(std::to_string(std::llround(number.value)))
                                 : notYet();
  } else {
    return std::nullopt;
  }
  row.reasons = ReasonsFor(number, text);
  return row;
}

// Why?'s rows: one per ranking number in the server's order (unknown names
// skipped), then where clients find the device when the status says. Whatever
// arrives is rendered and nothing is inferred: the server sends no weight or
// tier rows for a device outside the pools, and the anti-abuse checks only
// ever surface as the generic not_eligible reason.
template <typename Text, typename Status>
std::vector<NumberRow<Text>> WhyRowsFor(const Status& status, const ValueText<Text>& text) {
  std::vector<NumberRow<Text>> rows;
  if (status.ranking) {
    for (const auto& number : *status.ranking) {
      if (auto row = NumberRowFor(number, text)) rows.push_back(std::move(*row));
    }
  }
  if (status.country) {
    // the country's name, else its code upper-cased
    std::string name = status.country->country;
    if (name.empty()) {
      for (char c : status.country->country_code) {
        name.push_back('a' <= c && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
      }
    }
    if (!name.empty()) rows.push_back({"country", text.plain(name), "provider_status_help_country", true, {}});
  }
  return rows;
}

// ---- the percent -------------------------------------------------------------

// A 0-1 ratio as a whole percent in a Windows locale's pattern
// (LOCALE_IPOSITIVEPERCENT: 0 "# %", 1 "#%", 2 "%#", 3 "% #") with its percent
// sign (LOCALE_SPERCENT, UTF-8; "%" when empty). The space is a no-break space,
// so the sign never wraps away from its number. An unknown pattern is "#%".
inline std::string FormatPercent(double ratio, int pattern, std::string_view symbol) {
  const std::string number = std::to_string(std::llround(ratio * 100));
  const std::string sign = symbol.empty() ? std::string("%") : std::string(symbol);
  constexpr std::string_view kNoBreakSpace = "\xC2\xA0";
  switch (pattern) {
    case 0: return number + std::string(kNoBreakSpace) + sign;
    case 2: return sign + number;
    case 3: return sign + std::string(kNoBreakSpace) + number;
    default: return number + sign;
  }
}

}  // namespace urnw::providerstatus
