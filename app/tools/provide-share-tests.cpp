// Executable spec for the Connect page's sharing limits
// (App/ProvideSharePresentation.h; sdk provide_share.go, owner decision
// 2026-11-28, feature 10): the values each picker lists, No limit first and a
// limit the cli set added in its place; which of them is selected; the whole
// Mbps a speed is labelled with; and which line says how the month stands.
//
//   c++ -std=c++20 -I ../src/App provide-share-tests.cpp -o /tmp/provide-share-tests && /tmp/provide-share-tests
//
// With URNW_PROVIDE_SHARE_TESTS_SDK the status line runs on the generated
// header's own urnet::ProvideShareStatus, parsed through its json conversions.
// The header needs nlohmann/json; both are system includes because the
// generated code does not build with -Wextra -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_PROVIDE_SHARE_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> provide-share-tests.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "ProvideSharePresentation.h"

#if defined(URNW_PROVIDE_SHARE_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

namespace share = urnw::provideshare;

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

std::string Join(const std::vector<int64_t>& values) {
  std::string out;
  for (const int64_t value : values) out += (out.empty() ? "" : " ") + std::to_string(value);
  return "{" + out + "}";
}

void CheckValues(const std::vector<int64_t>& want, const std::vector<int64_t>& got,
                 const std::string& what) {
  Check(want == got, what + ": want " + Join(want) + ", got " + Join(got));
}

const char* LineName(share::StatusLine line) {
  switch (line) {
    case share::StatusLine::LimitReached: return "LimitReached";
    case share::StatusLine::UsedOfLimit: return "UsedOfLimit";
    case share::StatusLine::Used: return "Used";
  }
  return "?";
}

void CheckLine(share::StatusLine want, share::StatusLine got, const std::string& what) {
  Check(want == got, what + ": want " + LineName(want) + ", got " + LineName(got));
}

#if defined(URNW_PROVIDE_SHARE_TESTS_SDK)
using Status = urnet::ProvideShareStatus;
#else
// The fields of the sdk's struct, by the generated wrapper's names and types.
struct Status {
  int64_t max_bytes_per_second = 0;
  int64_t monthly_byte_count = 0;
  int64_t byte_count = 0;
  int64_t period_start_millis = 0;
  int64_t period_end_millis = 0;
  bool budget_reached = false;
};
#endif

// fields set one by one, so the same helper builds the mirror and the sdk's
Status MakeStatus(int64_t monthlyByteCount, int64_t byteCount, bool budgetReached) {
  Status status;
  status.monthly_byte_count = monthlyByteCount;
  status.byte_count = byteCount;
  status.period_start_millis = 1'796'083'200'000;  // 2026-12-01T00:00:00Z
  status.period_end_millis = 1'798'761'600'000;    // 2027-01-01T00:00:00Z
  status.budget_reached = budgetReached;
  return status;
}

constexpr int64_t kMbps = 125'000;
constexpr int64_t kGib = 1024LL * 1024 * 1024;

}  // namespace

int main() {
  // ---- the units ----
  {
    CheckEq(1'000'000, share::kBytesPerSecondPerMbps * 8, "a megabit per second is 10^6 bits a second");
    CheckEq(1'073'741'824, share::kBytesPerGib, "a GiB is 1024^3 bytes");
  }

  // ---- the speed picker ----
  {
    const std::vector<int64_t> store = {0,         1 * kMbps,  2 * kMbps,  5 * kMbps,
                                        10 * kMbps, 20 * kMbps, 50 * kMbps, 100 * kMbps};
    CheckValues(store, share::SpeedChoices(0), "speed: No limit, then 1 to 100 Mbps");
    CheckValues(store, share::SpeedChoices(10 * kMbps), "speed: a listed limit adds nothing");
    CheckValues(store, share::SpeedChoices(-1), "speed: a negative limit is no limit");
    CheckValues({0, 1 * kMbps, 2 * kMbps, 3 * kMbps, 5 * kMbps, 10 * kMbps, 20 * kMbps, 50 * kMbps,
                 100 * kMbps},
                share::SpeedChoices(3 * kMbps), "speed: a limit the cli set, in its place");
    CheckValues({0, 62'500, 1 * kMbps, 2 * kMbps, 5 * kMbps, 10 * kMbps, 20 * kMbps, 50 * kMbps,
                 100 * kMbps},
                share::SpeedChoices(62'500), "speed: a limit below the slowest choice");
    CheckValues({0, 1 * kMbps, 2 * kMbps, 5 * kMbps, 10 * kMbps, 20 * kMbps, 50 * kMbps, 100 * kMbps,
                 1'000 * kMbps},
                share::SpeedChoices(1'000 * kMbps), "speed: a limit above the fastest choice");

    CheckEq(0, static_cast<int64_t>(share::IndexOf(share::SpeedChoices(0), 0)), "speed: No limit selected");
    CheckEq(4, static_cast<int64_t>(share::IndexOf(share::SpeedChoices(10 * kMbps), 10 * kMbps)),
            "speed: 10 Mbps selected");
    CheckEq(3, static_cast<int64_t>(share::IndexOf(share::SpeedChoices(3 * kMbps), 3 * kMbps)),
            "speed: the cli's limit selected");
    CheckEq(0, static_cast<int64_t>(share::IndexOf(share::SpeedChoices(-1), -1)),
            "speed: a negative limit selects No limit");

    CheckEq(0, share::SpeedMbps(0), "mbps: no limit");
    CheckEq(1, share::SpeedMbps(kMbps), "mbps: 1");
    CheckEq(100, share::SpeedMbps(100 * kMbps), "mbps: 100");
    CheckEq(3, share::SpeedMbps(3 * kMbps), "mbps: a whole limit the cli set");
    CheckEq(10, share::SpeedMbps(1'300'000), "mbps: 10.4 rounds down");
    CheckEq(11, share::SpeedMbps(1'312'500), "mbps: 10.5 rounds up");
    CheckEq(1, share::SpeedMbps(62'500), "mbps: half a megabit rounds to 1");
  }

  // ---- the monthly picker ----
  {
    const std::vector<int64_t> store = {0,         5 * kGib,   10 * kGib,  25 * kGib, 50 * kGib,
                                        100 * kGib, 250 * kGib, 500 * kGib, 1024 * kGib};
    CheckValues(store, share::MonthlyChoices(0), "monthly: No limit, then 5 GiB to 1 TiB");
    CheckValues(store, share::MonthlyChoices(1024 * kGib), "monthly: 1 TiB is listed");
    CheckValues(store, share::MonthlyChoices(-kGib), "monthly: a negative limit is no limit");
    CheckValues({0, 3 * kGib, 5 * kGib, 10 * kGib, 25 * kGib, 50 * kGib, 100 * kGib, 250 * kGib,
                 500 * kGib, 1024 * kGib},
                share::MonthlyChoices(3 * kGib), "monthly: a limit the cli set, in its place");
    CheckValues({0, 5 * kGib, 10 * kGib, 25 * kGib, 50 * kGib, 100 * kGib, 250 * kGib, 500 * kGib,
                 1024 * kGib, 2048 * kGib},
                share::MonthlyChoices(2048 * kGib), "monthly: a limit above 1 TiB");
    CheckValues({0, 5 * kGib, 10 * kGib, 10 * kGib + 1, 25 * kGib, 50 * kGib, 100 * kGib, 250 * kGib,
                 500 * kGib, 1024 * kGib},
                share::MonthlyChoices(10 * kGib + 1), "monthly: a byte past a choice is its own");
    CheckEq(4, static_cast<int64_t>(share::IndexOf(share::MonthlyChoices(50 * kGib), 50 * kGib)),
            "monthly: 50 GiB selected");
    CheckEq(8, static_cast<int64_t>(share::IndexOf(share::MonthlyChoices(1024 * kGib), 1024 * kGib)),
            "monthly: 1 TiB selected");
    CheckEq(9, static_cast<int64_t>(share::IndexOf(share::MonthlyChoices(2048 * kGib), 2048 * kGib)),
            "monthly: the cli's limit selected");
    CheckEq(0, static_cast<int64_t>(share::IndexOf(share::MonthlyChoices(0), 7)),
            "monthly: a value the list lacks selects No limit");
  }

  // ---- the line under the pickers ----
  {
    CheckLine(share::StatusLine::Used, share::StatusLineFor(MakeStatus(0, 0, false)),
              "line: no monthly limit, nothing shared yet");
    CheckLine(share::StatusLine::Used, share::StatusLineFor(MakeStatus(0, 7 * kGib, false)),
              "line: no monthly limit");
    CheckLine(share::StatusLine::UsedOfLimit, share::StatusLineFor(MakeStatus(50 * kGib, 7 * kGib, false)),
              "line: against the monthly limit");
    CheckLine(share::StatusLine::UsedOfLimit, share::StatusLineFor(MakeStatus(50 * kGib, 0, false)),
              "line: against the monthly limit, nothing shared yet");
    CheckLine(share::StatusLine::LimitReached, share::StatusLineFor(MakeStatus(50 * kGib, 50 * kGib, true)),
              "line: the limit reached");
    // the device's own verdict decides, never the app's arithmetic
    CheckLine(share::StatusLine::UsedOfLimit, share::StatusLineFor(MakeStatus(50 * kGib, 60 * kGib, false)),
              "line: past the limit, as long as the device says it is not reached");
    Status speedOnly = MakeStatus(0, 3 * kGib, false);
    speedOnly.max_bytes_per_second = 10 * kMbps;
    CheckLine(share::StatusLine::Used, share::StatusLineFor(speedOnly),
              "line: a speed limit alone has no monthly figure");
  }

#if defined(URNW_PROVIDE_SHARE_TESTS_SDK)
  // ---- against the SDK header: a status as the device hands it over ----
  {
    const urnet::ProvideShareStatus reached = nlohmann::json::parse(R"({
      "max_bytes_per_second": 1250000,
      "monthly_byte_count": 53687091200,
      "byte_count": 53687091200,
      "period_start_millis": 1796083200000,
      "period_end_millis": 1798761600000,
      "budget_reached": true
    })").get<urnet::ProvideShareStatus>();
    CheckLine(share::StatusLine::LimitReached, share::StatusLineFor(reached), "sdk: the limit reached");
    CheckEq(4, static_cast<int64_t>(share::IndexOf(share::SpeedChoices(reached.max_bytes_per_second),
                                                   reached.max_bytes_per_second)),
            "sdk: 10 Mbps selected");
    CheckEq(4, static_cast<int64_t>(share::IndexOf(share::MonthlyChoices(reached.monthly_byte_count),
                                                   reached.monthly_byte_count)),
            "sdk: 50 GiB selected");
    CheckEq(1'798'761'600'000, reached.period_end_millis, "sdk: the month's end");

    // a status with no limits: the device that shares without them
    const urnet::ProvideShareStatus open = nlohmann::json::parse(R"({
      "max_bytes_per_second": 0,
      "monthly_byte_count": 0,
      "byte_count": 9007199254740993,
      "period_start_millis": 1796083200000,
      "period_end_millis": 1798761600000
    })").get<urnet::ProvideShareStatus>();
    CheckLine(share::StatusLine::Used, share::StatusLineFor(open), "sdk: no limits");
    CheckEq(9'007'199'254'740'993, open.byte_count, "sdk: the month's data past 2^53");
    CheckEq(0, static_cast<int64_t>(share::IndexOf(share::SpeedChoices(open.max_bytes_per_second),
                                                   open.max_bytes_per_second)),
            "sdk: No limit selected");
  }
#endif

  std::cout << (gCases - gFailures) << "/" << gCases << " provide share checks passed"
#if defined(URNW_PROVIDE_SHARE_TESTS_SDK)
            << " (against urnetwork_sdk.hpp)"
#endif
            << "\n";
  return gFailures == 0 ? 0 : 1;
}
