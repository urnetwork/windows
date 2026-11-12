// Executable spec for the Earnings provider status
// (App/ProviderStatusPresentation.h, support part P008, phase 2): the Demand
// histogram's bars, total and empty state; the states table (loading,
// unavailable, hidden, empty, bars, and when Why? and the server's reason
// join); the line under the provide mode row, local reason first; every Why?
// row's label, value, help and tint; the locale percent; and the readings a
// status read on the api keeps for the provider-only device (Readings), which
// apply each answer as the sdk's controller applies a poll.
//
//   c++ -std=c++20 -I ../src/App provider-status-tests.cpp -o /tmp/provider-status-tests && /tmp/provider-status-tests
//
// With URNW_PROVIDER_STATUS_TESTS_SDK the same cases run on the generated
// header's own structs (urnet::ProviderStatus, ProviderRankingNumber,
// ProviderStatusCountry), and a provider status parsed through the header's
// json conversions goes through the whole page model; the sdk's reason and
// number constants are checked against the presentation's keys. The header
// needs nlohmann/json; both are system includes because the generated code
// does not build with -Wextra -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_PROVIDER_STATUS_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> provider-status-tests.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ProviderStatusPresentation.h"

#if defined(URNW_PROVIDER_STATUS_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

namespace ps = urnw::providerstatus;
using urnw::provideridle::ProviderIdleReason;

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

void CheckText(const std::string& want, const std::string& got, const std::string& what) {
  Check(want == got, what + ": want \"" + want + "\", got \"" + got + "\"");
}

void CheckEq(int64_t want, int64_t got, const std::string& what) {
  Check(want == got, what + ": want " + std::to_string(want) + ", got " + std::to_string(got));
}

#if defined(URNW_PROVIDER_STATUS_TESTS_SDK)
using ReasonLine = urnet::ProviderStatusReasonLine;
using Number = urnet::ProviderRankingNumber;
using Country = urnet::ProviderStatusCountry;
using Appearances = urnet::ProviderAppearanceHistogram;
using Status = urnet::ProviderStatus;
#else
// The fields of the sdk's structs, by the generated wrapper's names and types.
struct ReasonLine {
  std::string label;
  std::optional<int64_t> count;
  std::optional<std::string> text;
};
struct Number {
  std::string name;
  bool has_value = false;
  double value = 0;
  bool has_minimum = false;
  double minimum = 0;
  bool has_maximum = false;
  double maximum = 0;
  bool passes = false;
  int64_t count = 0;
  int64_t total = 0;
  std::string explanation;
  std::optional<std::string> reason;
  std::optional<int64_t> reason_tcp_count;
  std::optional<int64_t> reason_ipv4_count;
  std::optional<std::vector<ReasonLine>> reason_lines;
};
struct Country {
  std::string country_code;
  std::string country;
  std::string observed_country_code;
  std::string explanation;
};
struct Appearances {
  int64_t start_minute = 0;
  int64_t bucket_seconds = 0;
  std::optional<std::vector<int64_t>> appearances_per_minute;
};
struct Status {
  std::optional<std::string> client_id;
  std::string reason;
  std::string reason_text;
  std::optional<std::vector<Number>> ranking;
  std::optional<Country> country;
  std::optional<std::string> evaluate_time;
  std::optional<Appearances> appearances;
};
#endif

#if defined(URNW_PROVIDER_STATUS_TESTS_SDK)
using Result = urnet::GetProviderStatusResult;
#else
struct Result {
  std::optional<std::vector<Status>> providers;
  std::optional<bool> truncated;
};
#endif

// fields set one by one, so the same helpers build the mirrors and the sdk's
Number MakeNumber(const std::string& name, bool hasValue, double value, bool passes) {
  Number number;
  number.name = name;
  number.has_value = hasValue;
  number.value = value;
  number.passes = passes;
  return number;
}

Number WithMinimum(Number number, double minimum) {
  number.has_minimum = true;
  number.minimum = minimum;
  return number;
}

Number WithMaximum(Number number, double maximum) {
  number.has_maximum = true;
  number.maximum = maximum;
  return number;
}

Number WithCounts(Number number, int64_t count, int64_t total) {
  number.count = count;
  number.total = total;
  return number;
}

// a line as the sdk derives it from the server's reason
ReasonLine MakeReasonLine(const std::string& label, std::optional<int64_t> count,
                          std::optional<std::string> text) {
  ReasonLine line;
  line.label = label;
  line.count = count;
  line.text = text;
  return line;
}

Number WithReasonLines(Number number, std::vector<ReasonLine> lines) {
  number.reason_lines = std::move(lines);
  return number;
}

Status MakeStatus(const std::string& reason, const std::string& reasonText) {
  Status status;
  status.reason = reason;
  status.reason_text = reasonText;
  return status;
}

Status WithCounts(Status status, std::vector<int64_t> counts) {
  Appearances appearances;
  appearances.start_minute = 29'313'600;
  appearances.bucket_seconds = 60;
  appearances.appearances_per_minute = std::move(counts);
  status.appearances = appearances;
  return status;
}

// the English store values the values read, with {0} and {1} placeholders
const std::map<std::string, std::string, std::less<>> kEnglish = {
    {"provider_status_value_with_minimum", "{0} (needs {1})"},
    {"provider_status_value_with_maximum", "{0} (at most {1})"},
    {"provider_status_value_count_of_total", "{0} of {1} loaded"},
    {"provider_status_value_not_yet", "Not yet"},
    {"provider_status_value_no_history", "No history yet"},
    {"provider_status_value_not_in_pool", "Not in this pool"},
    // plurals, by the English other form: {} is the count
    {"provider_status_reason_line_tcp",
     "{} of these checks failed only over HTTP/3 (UDP), so this device is offered for TCP traffic only. Some app builds and some networks do not route UDP."},
    {"provider_status_reason_line_ipv4",
     "{} of these checks failed only over IPv6, so this device is offered for IPv4 traffic only."},
};

std::string English(std::string_view key) {
  if (auto it = kEnglish.find(key); it != kEnglish.end()) return it->second;
  return std::string(key);  // a missing key reads as itself, as urnw::Localized does
}

std::string Substitute(std::string_view key, const std::string& a, const std::string& b) {
  std::string out = English(key);
  for (const auto& [placeholder, value] : {std::pair<std::string, std::string>{"{0}", a}, {"{1}", b}}) {
    if (auto at = out.find(placeholder); at != std::string::npos) out.replace(at, 3, value);
  }
  return out;
}

ps::ValueText<std::string> EnglishText() {
  ps::ValueText<std::string> text;
  text.localized = [](std::string_view key) { return English(key); };
  text.format = [](std::string_view key, const std::string& a, const std::string& b) {
    return Substitute(key, a, b);
  };
  text.formatCounts = [](std::string_view key, int64_t a, int64_t b) {
    return Substitute(key, std::to_string(a), std::to_string(b));
  };
  text.plural = [](std::string_view key, int64_t count) {
    std::string out = English(key);
    if (auto at = out.find("{}"); at != std::string::npos) out.replace(at, 2, std::to_string(count));
    return out;
  };
  text.percent = [](double ratio) { return ps::FormatPercent(ratio, 1, "%"); };
  // the app's byte rate is outside this header: a marker shows it was used
  text.rate = [](double bytesPerSecond) {
    return "rate(" + std::to_string(std::llround(bytesPerSecond)) + ")";
  };
  text.plain = [](const std::string& utf8) { return utf8; };
  return text;
}

struct RowWant {
  std::string labelKey;
  std::string value;
  std::string helpKey;
  bool passes = true;
};

void CheckRow(const RowWant& want, const std::optional<ps::NumberRow<std::string>>& got,
              const std::string& what) {
  if (!got) {
    Check(false, what + ": no row");
    return;
  }
  CheckText(want.labelKey, got->labelKey, what + " label");
  CheckText(want.value, got->value, what + " value");
  CheckText(want.helpKey, got->helpKey, what + " help");
  Check(want.passes == got->passes, what + " tint (passes " + (want.passes ? "true" : "false") + ")");
}

std::optional<ps::NumberRow<std::string>> Row(const Number& number) {
  return ps::NumberRowFor(number, EnglishText());
}

const char* AreaName(ps::DemandArea area) {
  switch (area) {
    case ps::DemandArea::Hidden: return "Hidden";
    case ps::DemandArea::Loading: return "Loading";
    case ps::DemandArea::Unavailable: return "Unavailable";
    case ps::DemandArea::Empty: return "Empty";
    case ps::DemandArea::Bars: return "Bars";
  }
  return "?";
}

void CheckSections(ps::Sections want, ps::Sections got, const std::string& what) {
  Check(want == got, what + ": want " + AreaName(want.area) + (want.why ? " + why" : "") +
                         (want.serverReason ? " + server" : "") + ", got " + AreaName(got.area) +
                         (got.why ? " + why" : "") + (got.serverReason ? " + server" : ""));
}

void CheckLine(const ps::Line& want, const ps::Line& got, const std::string& what) {
  Check(want == got, what + ": want {" + want.key + "|" + want.text + "}, got {" + got.key + "|" +
                         got.text + "}");
}

// the twelve reason codes (urnet::ProviderStatusReason*)
const std::vector<std::string> kReasonCodes = {
    "not_providing",  "not_connected",   "location_invalid",
    "network_only",   "reliability_warming_up", "reliability_low",
    "not_eligible",   "egress_unprobed", "egress_failing",
    "speed_test_missing", "slow",        "none",
};

std::vector<int64_t> Sixty(int64_t fill) { return std::vector<int64_t>(ps::kBarCount, fill); }

}  // namespace

int main() {
  // ---- the histogram: 60 bars from the counts ----
  {
    std::vector<int64_t> counts = Sixty(0);
    counts[0] = 2;   // oldest
    counts[30] = 8;
    counts[59] = 4;  // the current minute
    const ps::Histogram h = ps::HistogramFor(counts);
    CheckEq(14, h.total, "total is the sum of the 60 counts");
    CheckEq(8, h.maxCount, "max count");
    Check(!h.empty, "a count makes it not empty");
    Check(h.fractions[0] == 0.25, "oldest bar is on the left at count/max");
    Check(h.fractions[30] == 1.0, "the largest bar is full height");
    Check(h.fractions[59] == 0.5, "the current minute is the right-most bar");
    Check(h.fractions[1] == 0.0, "a zero draws no bar");
    Check(h.fractions.size() == 60, "exactly 60 bars");
  }
  {
    // the zero-max case: all zero draws no bar, over max(0, 1)
    const ps::Histogram h = ps::HistogramFor(Sixty(0));
    Check(h.empty, "all zero is empty");
    CheckEq(0, h.total, "all zero total");
    CheckEq(0, h.maxCount, "all zero max");
    bool flat = true;
    for (double f : h.fractions) flat = flat && f == 0.0;
    Check(flat, "all zero draws no bar");
    Check(ps::HistogramFor({}).empty, "no counts at all is empty");
  }
  {
    // one appearance in the hour fills its bar; a count of 1 is not a zero
    std::vector<int64_t> counts = Sixty(0);
    counts[17] = 1;
    const ps::Histogram h = ps::HistogramFor(counts);
    Check(!h.empty && h.fractions[17] == 1.0 && h.total == 1, "a single appearance");
  }
  {
    // a short list is padded on the old side, so its last count stays "now"
    const ps::Histogram h = ps::HistogramFor({3, 6});
    Check(h.fractions[58] == 0.5 && h.fractions[59] == 1.0 && h.fractions[0] == 0.0,
          "a short list keeps its newest count at the right");
    CheckEq(9, h.total, "a short list's total");
    // a long list keeps its newest 60
    std::vector<int64_t> counts(65, 1);
    counts[0] = 1000;  // older than the hour
    counts[64] = 2;
    const ps::Histogram longer = ps::HistogramFor(counts);
    CheckEq(61, longer.total, "a long list's total counts the newest 60");
    CheckEq(2, longer.maxCount, "a long list's max ignores the dropped minutes");
    Check(longer.fractions[59] == 1.0 && longer.fractions[0] == 0.5, "a long list keeps its newest 60");
    // a negative count is 0
    std::vector<int64_t> negative = Sixty(0);
    negative[3] = -5;
    negative[4] = 5;
    const ps::Histogram clamped = ps::HistogramFor(negative);
    Check(clamped.fractions[3] == 0.0 && clamped.total == 5, "a negative count is 0");
  }

  // ---- the states table ----
  {
    using ps::DemandArea;
    CheckSections({DemandArea::Loading, false, false}, ps::SectionsFor(false, false, false, false, true),
                  "not loaded, no error");
    CheckSections({DemandArea::Unavailable, false, false}, ps::SectionsFor(false, true, false, false, true),
                  "not loaded, error (the 404 before deploy)");
    CheckSections({DemandArea::Hidden, false, false}, ps::SectionsFor(true, false, false, false, true),
                  "loaded, no status for this device");
    CheckSections({DemandArea::Unavailable, true, true}, ps::SectionsFor(true, false, true, false, true),
                  "loaded, status without appearances");
    CheckSections({DemandArea::Empty, true, true}, ps::SectionsFor(true, false, true, true, true),
                  "loaded, all zero");
    CheckSections({DemandArea::Bars, true, true}, ps::SectionsFor(true, false, true, true, false),
                  "loaded");
    // a later failed poll keeps the last snapshot
    CheckSections({DemandArea::Bars, true, true}, ps::SectionsFor(true, true, true, true, false),
                  "loaded, then a failed poll");
    CheckSections({DemandArea::Hidden, false, false}, ps::SectionsFor(true, true, false, false, true),
                  "loaded without a status, then a failed poll");
  }
  {
    // the same table from the controller's readings
    using ps::DemandArea;
    const std::optional<Status> none;
    CheckSections({DemandArea::Loading, false, false}, ps::ViewFor(false, false, none).sections,
                  "view: before the first poll");
    CheckSections({DemandArea::Unavailable, false, false}, ps::ViewFor(false, true, none).sections,
                  "view: no controller or a failed poll");
    CheckSections({DemandArea::Hidden, false, false}, ps::ViewFor(true, false, none).sections,
                  "view: no status for this device");
    const std::optional<Status> bare = MakeStatus("none", "");
    CheckSections({DemandArea::Unavailable, true, true}, ps::ViewFor(true, false, bare).sections,
                  "view: a status without appearances");
    const std::optional<Status> zero = WithCounts(MakeStatus("none", ""), Sixty(0));
    CheckSections({DemandArea::Empty, true, true}, ps::ViewFor(true, false, zero).sections,
                  "view: all zero");
    std::vector<int64_t> counts = Sixty(0);
    counts[59] = 3;
    const std::optional<Status> some = WithCounts(MakeStatus("none", ""), counts);
    const ps::View view = ps::ViewFor(true, false, some);
    CheckSections({DemandArea::Bars, true, true}, view.sections, "view: counts");
    CheckEq(3, view.histogram.total, "view: the total");
    // an appearances object without its list draws the empty chart
    Status listless = MakeStatus("none", "");
    listless.appearances = Appearances{};
    CheckSections({DemandArea::Empty, true, true}, ps::ViewFor(true, false, std::optional<Status>(listless)).sections,
                  "view: appearances without counts");
  }

  // ---- the reason codes ----
  {
    for (const std::string& code : kReasonCodes) {
      CheckText("provider_status_reason_" + code, ps::ReasonKey(code), "reason key " + code);
    }
    CheckText("", ps::ReasonKey("brand_new_reason"), "an unknown code has no key");
    CheckText("", ps::ReasonKey(""), "no code, no key");
  }

  // ---- the line under the provide mode row ----
  {
    // the local reasons win over anything the server says
    for (ProviderIdleReason idle : {ProviderIdleReason::AutoNotConnected, ProviderIdleReason::NetworkOnly,
                                    ProviderIdleReason::PausedWifiOnly, ProviderIdleReason::PausedNoNetwork}) {
      const std::string key = urnw::provideridle::ProviderIdleReasonKey(idle);
      for (const std::string& code : {std::string("reliability_low"), std::string("none"), std::string(""),
                                      std::string("brand_new_reason")}) {
        CheckLine({key, ""}, ps::LineFor(idle, code, "server text"), key + " over server " + code);
      }
    }
    // right after Always is picked the cached server answer can still say network_only
    CheckLine({"provider_status_reason_network_only", ""},
              ps::LineFor(ProviderIdleReason::None, "network_only", "text"),
              "a stale network_only with nothing local");
    // then the server's reason, over "no traffic yet"
    for (const std::string& code : kReasonCodes) {
      if (code == "none") continue;
      CheckLine({"provider_status_reason_" + code, ""}, ps::LineFor(ProviderIdleReason::None, code, "x"),
                "server " + code);
      CheckLine({"provider_status_reason_" + code, ""}, ps::LineFor(ProviderIdleReason::NoTrafficYet, code, "x"),
                "server " + code + " over no traffic yet");
    }
    // the server's none: "no traffic yet" when the window is empty, else nothing
    CheckLine({"provider_idle_no_traffic_yet", ""}, ps::LineFor(ProviderIdleReason::NoTrafficYet, "none", "Everything checks out."),
              "server none with no traffic yet");
    CheckLine({}, ps::LineFor(ProviderIdleReason::None, "none", "Everything checks out."),
              "server none with traffic");
    // no status yet: the local line alone
    CheckLine({"provider_idle_no_traffic_yet", ""}, ps::LineFor(ProviderIdleReason::NoTrafficYet, "", ""),
              "no status, no traffic yet");
    CheckLine({}, ps::LineFor(ProviderIdleReason::None, "", ""), "no status, nothing to say");
    // a code this build does not know shows the server's English
    CheckLine({"", "A new reason, in English."},
              ps::LineFor(ProviderIdleReason::None, "brand_new_reason", "A new reason, in English."),
              "an unknown code falls back to reason_text");
    CheckLine({"", "A new reason, in English."},
              ps::LineFor(ProviderIdleReason::NoTrafficYet, "brand_new_reason", "A new reason, in English."),
              "an unknown code beats no traffic yet");
    CheckLine({"provider_idle_no_traffic_yet", ""}, ps::LineFor(ProviderIdleReason::NoTrafficYet, "brand_new_reason", ""),
              "an unknown code without text counts as none");
    Check(!ps::LineFor(ProviderIdleReason::None, "brand_new_reason", "").shown(), "nothing to show");
    Check(ps::LineFor(ProviderIdleReason::None, "brand_new_reason", "x").shown(), "text shows");
  }

  // ---- the Why? rows, by name ----
  {
    CheckRow({"provider_status_number_reliability_5m", "82%", "provider_status_help_reliability_5m", true},
             Row(MakeNumber("reliability_5m", true, 0.82, true)), "reliability_5m");
    CheckRow({"provider_status_number_reliability_5m", "No history yet", "provider_status_help_reliability_5m", true},
             Row(MakeNumber("reliability_5m", false, 0, true)), "reliability_5m without history");
    CheckRow({"provider_status_number_reliability_1h", "98% (needs 95%)", "provider_status_help_reliability_1h", true},
             Row(WithMinimum(MakeNumber("reliability_1h", true, 0.98, true), 0.95)), "reliability_1h");
    CheckRow({"provider_status_number_reliability_1h", "No history yet", "provider_status_help_reliability_1h", true},
             Row(WithMinimum(MakeNumber("reliability_1h", false, 0, true), 0.95)), "reliability_1h without history");
    CheckRow({"provider_status_number_reliability_12h", "41% (needs 70%)", "provider_status_help_reliability_12h", false},
             Row(WithMinimum(MakeNumber("reliability_12h", true, 0.41, false), 0.7)), "reliability_12h below its minimum");
    CheckRow({"reliability", "90% (needs 50%)", "provider_status_help_reliability_other", true},
             Row(WithMinimum(MakeNumber("reliability_lookback_3", true, 0.9, true), 0.5)), "reliability_lookback_3");
    CheckRow({"provider_status_number_url_checks", "19 of 20 loaded (needs 80%)", "provider_status_help_url_checks", true},
             Row(WithCounts(WithMinimum(MakeNumber("url_checks", true, 0.95, true), 0.8), 19, 20)), "url_checks");
    CheckRow({"provider_status_number_url_checks", "Not yet", "provider_status_help_url_checks", false},
             Row(WithMinimum(MakeNumber("url_checks", false, 0, false), 0.8)), "url_checks before a check");
    CheckRow({"provider_status_number_speed_test", "rate(3565158) (needs rate(1048576))", "provider_status_help_speed_test", true},
             Row(WithMinimum(MakeNumber("speed_test", true, 3'565'158, true), 1'048'576)), "speed_test");
    CheckRow({"provider_status_number_speed_test", "Not yet", "provider_status_help_speed_test", false},
             Row(WithMinimum(MakeNumber("speed_test", false, 0, false), 1'048'576)), "speed_test before a test");
    CheckRow({"provider_status_number_latency", "35 ms (at most 200 ms)", "provider_status_help_latency", true},
             Row(WithMaximum(MakeNumber("latency", true, 35, true), 200)), "latency");
    CheckRow({"provider_status_number_latency", "Not yet", "provider_status_help_latency", false},
             Row(WithMaximum(MakeNumber("latency", false, 0, false), 200)), "latency before a test");
    CheckRow({"provider_status_number_weight_quality", "0.42", "provider_status_help_weight", true},
             Row(MakeNumber("weight_quality", true, 0.4166, true)), "weight_quality with 2 decimals");
    CheckRow({"provider_status_number_weight_quality", "Not in this pool", "provider_status_help_weight", false},
             Row(MakeNumber("weight_quality", true, 0.4166, false)), "weight_quality out of the pool");
    CheckRow({"provider_status_number_weight_speed", "1.50", "provider_status_help_weight", true},
             Row(MakeNumber("weight_speed", true, 1.5, true)), "weight_speed");
    CheckRow({"provider_status_number_weight_speed", "Not in this pool", "provider_status_help_weight", false},
             Row(MakeNumber("weight_speed", true, 0, false)), "weight_speed out of the pool");
    CheckRow({"provider_status_number_tier_quality", "0", "provider_status_help_tier", true},
             Row(WithMaximum(MakeNumber("tier_quality", true, 0, true), 2)), "tier_quality, the best");
    CheckRow({"provider_status_number_tier_speed", "3", "provider_status_help_tier", false},
             Row(WithMaximum(MakeNumber("tier_speed", true, 3, false), 2)), "tier_speed past the cutoff");
    // a value without its floor or ceiling reads bare
    CheckRow({"provider_status_number_reliability_1h", "98%", "provider_status_help_reliability_1h", true},
             Row(MakeNumber("reliability_1h", true, 0.98, true)), "reliability_1h without a minimum");
    CheckRow({"provider_status_number_latency", "35 ms", "provider_status_help_latency", true},
             Row(MakeNumber("latency", true, 35, true)), "latency without a maximum");
    // unknown names are skipped
    for (const std::string& name : {std::string("some_future_number"), std::string("reliability_lookback_"),
                                    std::string("reliability_lookback_x"), std::string("arin_risk"), std::string("")}) {
      Check(!Row(MakeNumber(name, true, 1, true)), "an unknown name is skipped: " + name);
    }
  }

  // ---- the reason lines under the network checks (server FP2-PROTOCOL.md §5.5) ----
  {
    const std::string tcp =
        "3 of these checks failed only over HTTP/3 (UDP), so this device is offered for TCP traffic only. Some app builds and some networks do not route UDP.";
    const std::string ipv4 = "2 of these checks failed only over IPv6, so this device is offered for IPv4 traffic only.";
    const Number downgraded = WithReasonLines(
        WithCounts(WithMinimum(MakeNumber("url_checks", true, 0.65, false), 0.8), 13, 20),
        {MakeReasonLine("tcp", 3, std::nullopt), MakeReasonLine("ipv4", 2, std::nullopt)});
    const auto row = Row(downgraded);
    CheckRow({"provider_status_number_url_checks", "13 of 20 loaded (needs 80%)", "provider_status_help_url_checks", false},
             row, "a downgraded url_checks");
    Check(row && row->reasons == std::vector<std::string>{tcp, ipv4},
          "a downgraded url_checks: the tcp line, then the ipv4 line, with their counts");
    // a label this build has no string for shows the server's English; one
    // without text, and one with an empty text, show nothing
    const auto unknown = Row(WithReasonLines(MakeNumber("url_checks", true, 0.65, false),
                                             {MakeReasonLine("text", std::nullopt, "A newer server's sentence."),
                                              MakeReasonLine("pq", std::nullopt, std::nullopt),
                                              MakeReasonLine("pq", std::nullopt, "")}));
    Check(unknown && unknown->reasons == std::vector<std::string>{"A newer server's sentence."},
          "an unknown reason line shows the server's English");
    // no lines, as from a server or sdk before them: no reasons
    const auto plain = Row(WithCounts(WithMinimum(MakeNumber("url_checks", true, 0.95, true), 0.8), 19, 20));
    Check(plain && plain->reasons.empty(), "a url_checks without lines has no reasons");
    // the other rows never have lines from the sdk
    const auto reliability = Row(MakeNumber("reliability_5m", true, 0.82, true));
    Check(reliability && reliability->reasons.empty(), "reliability_5m has no reasons");
  }

  // ---- the Why? rows, as the server sends them ----
  {
    Status status = MakeStatus("reliability_warming_up", "Building reliability.");
    status.ranking = std::vector<Number>{
        MakeNumber("reliability_5m", true, 1, true),
        WithMinimum(MakeNumber("reliability_12h", true, 0.41, false), 0.7),
        MakeNumber("some_future_number", true, 7, true),
        WithCounts(WithMinimum(MakeNumber("url_checks", true, 0.95, true), 0.8), 19, 20),
        WithMaximum(MakeNumber("tier_speed", true, 3, false), 2),
    };
    Country country;
    country.country_code = "de";
    country.country = "Germany";
    status.country = country;
    const auto rows = ps::WhyRowsFor(status, EnglishText());
    std::vector<std::string> labels;
    for (const auto& row : rows) labels.push_back(row.labelKey);
    Check(labels == std::vector<std::string>{"provider_status_number_reliability_5m",
                                             "provider_status_number_reliability_12h",
                                             "provider_status_number_url_checks",
                                             "provider_status_number_tier_speed", "country"},
          "the server's order, unknown names skipped, the country last");
    if (rows.size() == 5) {
      CheckText("Germany", rows[4].value, "the country's name");
      CheckText("provider_status_help_country", rows[4].helpKey, "the country's help");
      Check(rows[4].passes, "the country is never amber");
      Check(!rows[1].passes && !rows[3].passes, "the failing numbers are amber");
    }

    // the country's code, upper-cased, when its name is missing
    country.country = "";
    status.country = country;
    status.ranking.reset();
    const auto coded = ps::WhyRowsFor(status, EnglishText());
    Check(coded.size() == 1 && coded[0].value == "DE", "the country code upper-cased without a name");
    // not_eligible: no weight or tier rows arrive, and none are made up
    Status ineligible = MakeStatus("not_eligible", "This connection isn't eligible to provide right now.");
    ineligible.ranking = std::vector<Number>{MakeNumber("reliability_5m", true, 1, true)};
    const auto few = ps::WhyRowsFor(ineligible, EnglishText());
    Check(few.size() == 1, "only what arrives is rendered");
    Check(ps::WhyRowsFor(MakeStatus("none", ""), EnglishText()).empty(), "no numbers, no rows");
  }

  // ---- a status read on the api (no session: the provider-only device) ----
  {
    const std::string self = "018f2b1e-0000-7000-8000-00000000c0de";
    const auto rowFor = [](const std::string& id, const std::string& reason) {
      Status status = MakeStatus(reason, "");
      status.client_id = id;
      return status;
    };
    const auto viewOf = [](const ps::Readings<Status>& readings) {
      return ps::ViewFor(readings.loaded, !readings.error.empty(), readings.status);
    };
    std::vector<int64_t> counts = Sixty(0);
    counts[59] = 3;
    Result result;
    result.providers = std::vector<Status>{
        rowFor("018f2b1e-0000-7000-8000-000000000001", "slow"),
        WithCounts(rowFor(self, "none"), counts),
        rowFor(self, "egress_failing"),
    };

    ps::Readings<Status> readings;
    CheckSections({ps::DemandArea::Loading, false, false}, viewOf(readings).sections,
                  "api: before any answer");
    readings.Answered(result, self);
    Check(readings.loaded && readings.error.empty(), "api: an answer loads and clears the error");
    Check(readings.status && readings.status->reason == "none",
          "api: the first row whose client id is this device's");
    CheckSections({ps::DemandArea::Bars, true, true}, viewOf(readings).sections,
                  "api: this device's histogram");

    readings.Failed("404 Not Found");
    Check(readings.loaded && readings.status && readings.status->reason == "none" &&
              readings.error == "404 Not Found",
          "api: a failed poll keeps the last snapshot and records the error");
    CheckSections({ps::DemandArea::Bars, true, true}, viewOf(readings).sections,
                  "api: the area keeps the snapshot after a failed poll");

    readings.Answered(result, "018f2b1e-0000-7000-8000-00000000ffff");
    Check(readings.loaded && !readings.status, "api: no row for this device is no status");
    CheckSections({ps::DemandArea::Hidden, false, false}, viewOf(readings).sections,
                  "api: no row for this device");
    readings.Answered(Result{}, self);
    Check(readings.loaded && !readings.status, "api: an answer with no list is no status");
    readings.Answered(result, "");
    Check(!readings.status, "api: an unknown client id matches no row");

    ps::Readings<Status> failed;
    failed.Failed("");
    Check(!failed.loaded && failed.error == "no provider status",
          "api: a failure with no message still reads as one");
    CheckSections({ps::DemandArea::Unavailable, false, false}, viewOf(failed).sections,
                  "api: a failure before any answer is unavailable, never loading for good");

    ps::Readings<Status> fetched;
    fetched.Fetched(std::optional<Result>{}, std::optional<std::string>{"timeout"}, self);
    Check(!fetched.loaded && fetched.error == "timeout", "api: a callback error is a failed poll");
    fetched.Fetched(std::optional<Result>{}, std::optional<std::string>{}, self);
    Check(!fetched.loaded && fetched.error == "no provider status",
          "api: a callback with no result is a failed poll");
    fetched.Fetched(std::optional<Result>{result}, std::optional<std::string>{}, self);
    Check(fetched.loaded && fetched.error.empty() && fetched.status &&
              fetched.status->reason == "none",
          "api: a callback with a result applies it");
  }

  // ---- the locale percent ----
  {
    CheckText("82%", ps::FormatPercent(0.82, 1, "%"), "pattern 1 (en)");
    CheckText("82\xC2\xA0%", ps::FormatPercent(0.82, 0, "%"), "pattern 0 (fr), a no-break space");
    CheckText("%82", ps::FormatPercent(0.82, 2, "%"), "pattern 2 (tr)");
    CheckText("%\xC2\xA0" "82", ps::FormatPercent(0.82, 3, "%"), "pattern 3");
    CheckText("82\xD9\xAA", ps::FormatPercent(0.82, 1, "\xD9\xAA"), "the locale's sign");
    CheckText("82%", ps::FormatPercent(0.82, 9, "%"), "an unknown pattern is #%");
    CheckText("82%", ps::FormatPercent(0.82, 1, ""), "no sign falls back to %");
    CheckText("100%", ps::FormatPercent(1.0, 1, "%"), "a whole");
    CheckText("0%", ps::FormatPercent(0.0, 1, "%"), "none");
    CheckText("96%", ps::FormatPercent(0.955, 1, "%"), "no decimals, rounded");
  }

#if defined(URNW_PROVIDER_STATUS_TESTS_SDK)
  // ---- against the SDK header: a provider status as the controller hands it over ----
  {
    // urnet::ProviderStatus from json, through the header's own from_json
    nlohmann::json document = nlohmann::json::parse(R"({
      "client_id": "018f2b1e-0000-7000-8000-00000000c0de",
      "reason": "reliability_warming_up",
      "reason_text": "Building reliability.",
      "admission": {"connected": true, "location_valid": true, "provide_public": true,
                    "reliability_ok": false, "speed_test_done": true, "egress": "pass"},
      "ranking": [
        {"name": "reliability_5m", "has_value": true, "value": 1, "passes": true},
        {"name": "reliability_1h", "has_value": true, "value": 0.98, "has_minimum": true, "minimum": 0.95, "passes": true},
        {"name": "reliability_12h", "has_value": true, "value": 0.41, "has_minimum": true, "minimum": 0.7, "passes": false},
        {"name": "url_checks", "has_value": true, "value": 0.95, "has_minimum": true, "minimum": 0.8, "passes": true, "count": 19, "total": 20,
         "reason": "3 of these checks failed only over HTTP/3 (UDP), so this device is offered for TCP traffic only. Some app builds and some networks do not route UDP.",
         "reason_tcp_count": 3, "reason_lines": [{"label": "tcp", "count": 3}]},
        {"name": "speed_test", "has_value": true, "value": 3565158, "has_minimum": true, "minimum": 1048576, "passes": true},
        {"name": "latency", "has_value": true, "value": 35, "has_maximum": true, "maximum": 200, "passes": true},
        {"name": "weight_quality", "has_value": true, "value": 0.4166, "passes": true},
        {"name": "tier_quality", "has_value": true, "value": 0, "has_maximum": true, "maximum": 2, "passes": true},
        {"name": "some_future_number", "has_value": true, "value": 7, "passes": true}
      ],
      "country": {"country_code": "de", "country": "Germany", "observed_country_code": "", "explanation": ""},
      "evaluate_time": "2026-10-05T01:00:00Z",
      "appearances": {"start_minute": 29313600, "bucket_seconds": 60}
    })");
    std::vector<int64_t> counts = Sixty(0);
    counts[0] = 1;
    counts[59] = 4;
    document["appearances"]["appearances_per_minute"] = counts;
    const std::optional<urnet::ProviderStatus> status = document.get<urnet::ProviderStatus>();

    const ps::View view = ps::ViewFor(true, false, status);
    CheckSections({ps::DemandArea::Bars, true, true}, view.sections, "sdk: the area");
    CheckEq(5, view.histogram.total, "sdk: the total");
    Check(view.histogram.fractions[0] == 0.25 && view.histogram.fractions[59] == 1.0, "sdk: the bars");
    CheckLine({"provider_status_reason_reliability_warming_up", ""},
              ps::LineFor(ProviderIdleReason::NoTrafficYet, status->reason, status->reason_text),
              "sdk: the line");
    const auto rows = ps::WhyRowsFor(*status, EnglishText());
    Check(rows.size() == 9, "sdk: eight known numbers and the country, got " + std::to_string(rows.size()));
    if (rows.size() == 9) {
      CheckText("19 of 20 loaded (needs 80%)", rows[3].value, "sdk: url_checks");
      Check(rows[3].reasons ==
                std::vector<std::string>{
                    "3 of these checks failed only over HTTP/3 (UDP), so this device is offered for TCP traffic only. Some app builds and some networks do not route UDP."},
            "sdk: url_checks has the sdk's tcp line");
      Check(rows[0].reasons.empty(), "sdk: reliability_5m has no reasons");
      CheckText("35 ms (at most 200 ms)", rows[5].value, "sdk: latency");
      CheckText("0.42", rows[6].value, "sdk: weight_quality");
      CheckText("Germany", rows[8].value, "sdk: the country");
      Check(!rows[2].passes, "sdk: reliability_12h is amber");
    }

    // a status the server could not read the histogram for
    document.erase("appearances");
    const std::optional<urnet::ProviderStatus> without = document.get<urnet::ProviderStatus>();
    CheckSections({ps::DemandArea::Unavailable, true, true}, ps::ViewFor(true, false, without).sections,
                  "sdk: no appearances");
  }
  {
    // GET /network/provider-status as the api hands it over, read for the
    // provider-only device through the header's own GetProviderStatusResult
    const urnet::GetProviderStatusResult result = nlohmann::json::parse(R"({
      "providers": [
        {"client_id": "018f2b1e-0000-7000-8000-000000000001", "reason": "slow", "reason_text": ""},
        {"client_id": "018f2b1e-0000-7000-8000-00000000c0de", "reason": "egress_unprobed",
         "reason_text": "Not checked yet.",
         "appearances": {"start_minute": 29313600, "bucket_seconds": 60}}
      ],
      "truncated": false
    })").get<urnet::GetProviderStatusResult>();
    ps::Readings<urnet::ProviderStatus> readings;
    readings.Fetched(std::optional<urnet::GetProviderStatusResult>{result}, std::nullopt,
                     "018f2b1e-0000-7000-8000-00000000c0de");
    Check(readings.loaded && readings.status && readings.status->reason == "egress_unprobed",
          "sdk: the provider-only device's row from the api's answer");
    CheckSections({ps::DemandArea::Empty, true, true},
                  ps::ViewFor(readings.loaded, !readings.error.empty(), readings.status).sections,
                  "sdk: a row with an empty histogram");
  }
  {
    // the sdk's constants are the codes and names the presentation knows
    const std::vector<std::string> sdkReasons = {
        urnet::ProviderStatusReasonNotProviding,   urnet::ProviderStatusReasonNotConnected,
        urnet::ProviderStatusReasonLocationInvalid, urnet::ProviderStatusReasonNetworkOnly,
        urnet::ProviderStatusReasonReliabilityWarmingUp, urnet::ProviderStatusReasonReliabilityLow,
        urnet::ProviderStatusReasonNotEligible,    urnet::ProviderStatusReasonEgressUnprobed,
        urnet::ProviderStatusReasonEgressFailing,  urnet::ProviderStatusReasonSpeedTestMissing,
        urnet::ProviderStatusReasonSlow,           urnet::ProviderStatusReasonNone,
    };
    Check(sdkReasons == kReasonCodes, "the reason codes are the sdk's urnet::ProviderStatusReason* constants");
    Check(ps::kReasonNone == urnet::ProviderStatusReasonNone, "none is the sdk's");
    Check(ps::kReasonLineTcp == urnet::ProviderStatusReasonLineTcp &&
              ps::kReasonLineIpv4 == urnet::ProviderStatusReasonLineIpv4,
          "the reason line labels are the sdk's urnet::ProviderStatusReasonLine* constants");
    for (const char* name : {urnet::ProviderStatusNumberReliability5m, urnet::ProviderStatusNumberReliability1h,
                             urnet::ProviderStatusNumberReliability12h, urnet::ProviderStatusNumberUrlChecks,
                             urnet::ProviderStatusNumberSpeedTest, urnet::ProviderStatusNumberLatency,
                             urnet::ProviderStatusNumberWeightQuality, urnet::ProviderStatusNumberTierQuality,
                             urnet::ProviderStatusNumberWeightSpeed, urnet::ProviderStatusNumberTierSpeed}) {
      Check(Row(MakeNumber(name, true, 1, true)).has_value(), std::string("the sdk's number ") + name + " has a row");
    }
    Check(urnw::provideridle::kProvideModePublic == urnet::ProvideModePublic,
          "the idle reason's public tier is urnet::ProvideModePublic");
  }
#endif

  std::cout << (gCases - gFailures) << "/" << gCases << " provider status checks passed"
#if defined(URNW_PROVIDER_STATUS_TESTS_SDK)
            << " (against urnetwork_sdk.hpp)"
#endif
            << "\n";
  return gFailures == 0 ? 0 : 1;
}
