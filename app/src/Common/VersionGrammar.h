// The release tag grammar, parsed: v<YYYY.M.D>-<code>[-beta].
//
// This is the wire format three parties must agree on: the CI derive step that
// mints tags (android parity), the VERSIONINFO stamp each binary carries
// (Version.h holds the same string, minus the leading v), and the update
// checker that ranks GitHub release tags by code. Header-only and pure — no
// Windows headers, no allocation — so the service selftest exercises exactly
// the bytes the update checker will run.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace urnw::version {

namespace grammar_detail {

// std::isdigit takes an int and is UB on negative chars; this is total.
inline constexpr bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

struct DigitRun {
  std::uint64_t value = 0;
  std::size_t length = 0;  // 0 == no acceptable run at this position
};

// The leading decimal run of s, refused (length 0) when it is empty or longer
// than maxDigits. A too-long run is a REFUSAL, not a truncation: parsing a
// prefix of a number would accept tags the grammar does not describe. The cap
// is also the overflow guard — callers never pass more than 18, and 18 nines
// (~1e18) sits far below uint64_t's ~1.8e19, so the accumulation below cannot
// wrap.
inline constexpr DigitRun TakeDigits(std::string_view s,
                                     std::size_t maxDigits) noexcept {
  std::size_t run = 0;
  while (run < s.size() && IsDigit(s[run])) ++run;
  if (run == 0 || run > maxDigits) return {};
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < run; ++i)
    value = value * 10 + static_cast<std::uint64_t>(s[i] - '0');
  return {value, run};
}

}  // namespace grammar_detail

// The numeric release code of a tag matching v<YYYY.M.D>-<code>[-beta], the
// leading v optional — Version.h stamps the v-less form into the binary while
// the git tags carry the v. Returns 0 on no match. 0 is also what a dev build
// stamps as its own code, so "not a release tag" and "never newer than
// anything" are deliberately the same answer.
inline constexpr std::uint64_t ParseReleaseCode(std::string_view tag) noexcept {
  using grammar_detail::TakeDigits;

  if (!tag.empty() && tag.front() == 'v') tag.remove_prefix(1);

  // YYYY: exactly four digits. The founding is 2023; nothing shorter is real.
  const auto year = TakeDigits(tag, 4);
  if (year.length != 4) return 0;
  tag.remove_prefix(year.length);
  if (tag.empty() || tag.front() != '.') return 0;
  tag.remove_prefix(1);

  // M and D are calendar values without leading-zero padding (2026.8.9). The
  // range checks are cheap honesty: a "month" of 0 or 13 is not a typo worth
  // ranking, it is a different tag scheme.
  const auto month = TakeDigits(tag, 2);
  if (month.length == 0 || month.value < 1 || month.value > 12) return 0;
  tag.remove_prefix(month.length);
  if (tag.empty() || tag.front() != '.') return 0;
  tag.remove_prefix(1);

  const auto day = TakeDigits(tag, 2);
  if (day.length == 0 || day.value < 1 || day.value > 31) return 0;
  tag.remove_prefix(day.length);

  if (tag.empty() || tag.front() != '-') return 0;
  tag.remove_prefix(1);

  // The code: ~1e9 today (ten digits), growing with the calendar. 18 digits
  // is the acceptance cap so the value always fits uint64_t — a longer run is
  // refused outright rather than wrapped into a plausible number that would
  // outrank every real release forever.
  const auto code = TakeDigits(tag, 18);
  if (code.length == 0) return 0;
  tag.remove_prefix(code.length);

  // Nothing may follow but the literal beta marker.
  if (!tag.empty() && tag != "-beta") return 0;
  return code.value;
}

}  // namespace urnw::version
