// Everything the Protocol leaderboard (ProtocolBoard.h) and the Earnings Top
// 200 tile decide before they touch a XAML object (owner decision 2026-10-29,
// LEADERTOP: apple/LEADERTOP.md is the design and Apple the reference; the
// ranking and the API are server/LEADERTOP.md): the sdk's two answers read as
// plain values, the banners, the own-status card with its progress chart and
// claim prompt, the top-400 list with the Top 200 line, the claimed-row detail
// sheet, the tile, the states (placeholders, signed out, empty, failed and a
// failed refresh), and the board's model, which drops an answer the board no
// longer wants.
//
// The board calls the in-process Api's snTop (GET /sn/top, public, paged by a
// cursor pinned to one snapshot) and snTopMe (GET /sn/top/me, client auth;
// the card and the tile share its answer). Each answers once, on an sdk
// thread: the readers here copy the answer into plain values there, and the
// board applies them on the UI thread.
//
// Pure, for the reason ExtenderPresentation.h gives: a WinUI 3 app cannot be
// built off Windows, so tools/protocol-board-tests.cpp verifies every decision
// on any host with a C++20 compiler. The readers are templates over the
// generated header's SnTopResult and SnTopMeResult, so the tests run them on
// fakes and compile them against urnetwork_sdk.hpp; ProtocolBoard.cpp passes
// the sdk's results straight in. A nullable number reaches them either as a
// std::optional or as the sdk's value with its has_ flag (CONTRACT.md's
// Has<Field>), and either reads the same.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "CryptoPayPresentation.h"  // ParseRfc3339: the snapshot's computed_at

namespace urnw::protocolboard {

// The validator's head: the seats the board's line is drawn under, when a
// snapshot names none.
inline constexpr int64_t kHeadSeats = 200;
// Q5: the list covers the top 400, so networks just outside the line can
// watch their progress toward it.
inline constexpr int64_t kListDepth = 400;
// The list is paged 50 at a time; a seek past the loaded rows asks for the
// API's largest page instead, so a drag to 400 is two requests.
inline constexpr int64_t kPageSize = 50;
inline constexpr int64_t kSeekPageSize = 200;
// Q13: M, the margin either side of the line that reads as borderline.
inline constexpr double kNearLine = 0.20;
// Q11: the progress chart's days.
inline constexpr int64_t kChartDays = 90;
// A board shown again after this long reloads from a fresh snapshot. A
// snapshot lasts a tempo (about 72 minutes), so a shorter wait asks for the
// same rows again.
inline constexpr int64_t kRefreshAfterMillis = 10 * 60 * 1000;

// ---- labels and reasons -------------------------------------------------------

// The recommendation label (Q13, Q17), as the server sends it.
enum class Label { None, Estimate, Likely, Borderline, Unlikely, NotMeasurable };

inline Label LabelFrom(std::string_view wire) {
  if (wire == "estimate") return Label::Estimate;
  if (wire == "likely") return Label::Likely;
  if (wire == "borderline") return Label::Borderline;
  if (wire == "unlikely") return Label::Unlikely;
  if (wire == "not_measurable") return Label::NotMeasurable;
  return Label::None;  // a label this build does not know shows no chip
}

// The chip's words; "" for no chip.
inline const char* LabelKey(Label label) {
  switch (label) {
    case Label::Estimate: return "leaderboard_protocol_label_estimate";
    case Label::Likely: return "leaderboard_protocol_label_likely";
    case Label::Borderline: return "leaderboard_protocol_label_borderline";
    case Label::Unlikely: return "leaderboard_protocol_label_unlikely";
    case Label::NotMeasurable: return "leaderboard_protocol_label_not_measurable";
    case Label::None: break;
  }
  return "";
}

// The chip's colour family. Never the only signal: the chip's words say it.
enum class Tone {
  Neutral,   // estimate: an outline, no fill
  Positive,  // likely
  Warning,   // borderline: the amber family
  Muted,     // unlikely, not measurable
};

inline Tone ToneOf(Label label) {
  switch (label) {
    case Label::Likely: return Tone::Positive;
    case Label::Borderline: return Tone::Warning;
    case Label::Unlikely:
    case Label::NotMeasurable: return Tone::Muted;
    case Label::Estimate:
    case Label::None: break;
  }
  return Tone::Neutral;
}

// Why the validator cannot score the network (SnTopMe.not_measurable_reason).
enum class Reason { None, NotInCohort, SharedEgress, NotProviding };

inline Reason ReasonFrom(std::string_view wire) {
  if (wire == "not_in_cohort") return Reason::NotInCohort;
  if (wire == "shared_egress") return Reason::SharedEgress;
  if (wire == "not_providing") return Reason::NotProviding;
  return Reason::None;
}

// What the card says for the reason; "" for none.
inline const char* ReasonKey(Reason reason) {
  switch (reason) {
    case Reason::NotInCohort: return "leaderboard_protocol_not_measurable_cohort";
    case Reason::SharedEgress: return "leaderboard_protocol_not_measurable_shared";
    case Reason::NotProviding: return "leaderboard_protocol_not_measurable_providing";
    case Reason::None: break;
  }
  return "";
}

// ---- the answers, as plain values -------------------------------------------------

// SnTopSnapshot: one tempo's board.
struct Snapshot {
  std::string id;
  int64_t tempoIndex = 0;
  int64_t computedAtMillis = 0;  // 0 when the server sent none, or one that does not parse
  int64_t totalRanked = 0;
  int64_t headSeats = kHeadSeats;
  int64_t claimedFleets = 0;
  int64_t openSeats = 0;
  double projectedCutoff = 0;  // the 200th unit's score: the line
  std::optional<double> claimedCutoff;
  bool bindingOpen = false;
  std::optional<double> precisionAt200;
  bool stale = false;  // computed more than 3 tempos ago

  bool operator==(const Snapshot&) const = default;
};

// SnTopFleet: one claimed UID of a network.
struct Fleet {
  int64_t uid = 0;
  int64_t generation = 0;
  int64_t validToEpoch = 0;
  double replicaScore = 0;
  std::optional<double> validatorScore;
  std::optional<int64_t> validatorRank;
  std::optional<int64_t> validatorRankOf;
  std::optional<bool> selected;
  std::optional<double> incentive;
  bool incentiveStale = false;

  bool operator==(const Fleet&) const = default;
};

// SnTopRow: one ranked network. An anonymous row carries no name.
struct Row {
  int64_t position = 0;
  std::string name;
  std::string tag;
  double projectedScore = 0;
  double margin = 0;  // vs the line: + above, - below
  int64_t trend = 0;  // places moved since the last tempo, + up
  int64_t prefixes = 0;
  int64_t sharedPrefixes = 0;
  Label label = Label::None;
  bool measurable = false;
  bool claimPrompt = false;
  bool isMe = false;
  std::vector<Fleet> fleets;  // empty when unclaimed

  bool Claimed() const { return !fleets.empty(); }
  bool operator==(const Row&) const = default;
};

// SnTopHistoryPoint: one day of the caller's projected rank.
struct HistoryPoint {
  std::string day;  // YYYY-MM-DD
  int64_t rank = 0;
  double score = 0;

  bool operator==(const HistoryPoint&) const = default;
};

// SnTopMe: the caller's network in detail.
struct Me {
  std::optional<Row> row;  // none, or position 0, while the network is not ranked
  double soloScore = 0;    // Q4: the score if it claimed alone today
  int64_t distinctPrefixes = 0;
  int64_t sharedPrefixes = 0;
  int64_t sharedWithUnits = 0;
  Reason reason = Reason::None;
  std::vector<HistoryPoint> history;  // oldest first, at most kChartDays

  bool Ranked() const { return row && 0 < row->position; }
  bool operator==(const Me&) const = default;
};

// A finished snTop.
struct TopOutcome {
  bool ok = false;
  std::optional<Snapshot> snapshot;
  std::vector<Row> rows;   // by position, inside the top 400
  std::optional<Row> me;   // the caller's row wherever it ranks; none signed out
  std::string next;        // the next page's cursor; "" at the end
  std::string error;       // what failed, for the log; the board shows its own words
};

// A finished snTopMe.
struct MeOutcome {
  bool ok = false;
  std::optional<Snapshot> snapshot;
  std::optional<Me> me;
  std::string error;
};

// ---- reading the sdk's results --------------------------------------------------
//
// By the generated header's field names (the Go JSON tags). Run on the sdk's
// thread, where the callback hands the result over.

namespace read {

template <typename T>
struct IsOptional : std::false_type {};
template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {};

// A member that may be the value or an optional of it: the value, or `fallback`.
template <typename V, typename T>
V ValueOr(const T& field, V fallback) {
  if constexpr (IsOptional<T>::value) {
    return field ? static_cast<V>(*field) : fallback;
  } else {
    return static_cast<V>(field);
  }
}

// A member that may be the value or an optional of it, as an optional.
template <typename V, typename T>
std::optional<V> Optional(const T& field) {
  if constexpr (IsOptional<T>::value) {
    return field ? std::optional<V>(static_cast<V>(*field)) : std::nullopt;
  } else {
    return static_cast<V>(field);
  }
}

// A nested struct or list member that may be held in an optional: a pointer to
// it, or null.
template <typename T>
const auto* Pointer(const T& field) {
  if constexpr (IsOptional<T>::value) {
    return field ? &*field : nullptr;
  } else {
    return &field;
  }
}

inline int64_t Millis(const std::optional<std::string>& rfc3339) {
  if (!rfc3339) return 0;
  const std::optional<int64_t> seconds = cryptopay::ParseRfc3339(*rfc3339);
  if (!seconds || *seconds <= 0) return 0;
  return *seconds * 1000;
}

// A nullable number of the contract: an optional member, or the value with its
// has_ flag. Written once per field, because the flag's name is the field's.
#define URNW_PROTOCOL_NULLABLE(V, s, field)                                            \
  [&]() -> std::optional<V> {                                                          \
    if constexpr (requires { (s).has_##field; }) {                                      \
      return (s).has_##field ? Optional<V>((s).field) : std::nullopt;                   \
    } else {                                                                           \
      return Optional<V>((s).field);                                                   \
    }                                                                                  \
  }()

template <typename S>
Snapshot SnapshotFrom(const S& s) {
  Snapshot out;
  out.id = ValueOr<std::string>(s.id, std::string());
  out.tempoIndex = ValueOr<int64_t>(s.tempo_index, int64_t{0});
  out.computedAtMillis = Millis(Optional<std::string>(s.computed_at));
  out.totalRanked = std::max<int64_t>(0, ValueOr<int64_t>(s.total_ranked, int64_t{0}));
  const int64_t seats = ValueOr<int64_t>(s.head_seats, int64_t{0});
  out.headSeats = 0 < seats ? seats : kHeadSeats;
  out.claimedFleets = ValueOr<int64_t>(s.claimed_fleets, int64_t{0});
  out.openSeats = ValueOr<int64_t>(s.open_seats, int64_t{0});
  out.projectedCutoff = ValueOr<double>(s.projected_cutoff, 0.0);
  out.claimedCutoff = URNW_PROTOCOL_NULLABLE(double, s, claimed_cutoff);
  out.bindingOpen = ValueOr<bool>(s.binding_open, false);
  out.precisionAt200 = URNW_PROTOCOL_NULLABLE(double, s, precision_at_200);
  out.stale = ValueOr<bool>(s.stale, false);
  return out;
}

template <typename F>
Fleet FleetFrom(const F& f) {
  Fleet out;
  out.uid = ValueOr<int64_t>(f.uid, int64_t{0});
  out.generation = ValueOr<int64_t>(f.generation, int64_t{0});
  out.validToEpoch = ValueOr<int64_t>(f.valid_to_epoch, int64_t{0});
  out.replicaScore = ValueOr<double>(f.replica_score, 0.0);
  out.validatorScore = URNW_PROTOCOL_NULLABLE(double, f, validator_score);
  out.validatorRank = URNW_PROTOCOL_NULLABLE(int64_t, f, validator_rank);
  out.validatorRankOf = URNW_PROTOCOL_NULLABLE(int64_t, f, validator_rank_of);
  out.selected = URNW_PROTOCOL_NULLABLE(bool, f, selected);
  out.incentive = URNW_PROTOCOL_NULLABLE(double, f, incentive);
  out.incentiveStale = ValueOr<bool>(f.incentive_stale, false);
  return out;
}

// A fleet with no UID names nothing on chain and is left out.
template <typename R>
Row RowFrom(const R& r) {
  Row out;
  out.position = ValueOr<int64_t>(r.position, int64_t{0});
  out.name = ValueOr<std::string>(r.name, std::string());
  out.tag = ValueOr<std::string>(r.tag, std::string());
  out.projectedScore = ValueOr<double>(r.projected_score, 0.0);
  out.margin = ValueOr<double>(r.margin, 0.0);
  out.trend = ValueOr<int64_t>(r.trend, int64_t{0});
  out.prefixes = ValueOr<int64_t>(r.prefixes, int64_t{0});
  out.sharedPrefixes = ValueOr<int64_t>(r.shared_prefixes, int64_t{0});
  out.label = LabelFrom(ValueOr<std::string>(r.label, std::string()));
  out.measurable = ValueOr<bool>(r.measurable, false);
  out.claimPrompt = ValueOr<bool>(r.claim_prompt, false);
  out.isMe = ValueOr<bool>(r.is_me, false);
  if (const auto* fleets = Pointer(r.fleets)) {
    for (const auto& fleet : *fleets) {
      Fleet read = FleetFrom(fleet);
      if (read.uid <= 0) continue;
      out.fleets.push_back(std::move(read));
    }
  }
  return out;
}

#undef URNW_PROTOCOL_NULLABLE

// An error's words for the log: the call's error string, or the server's
// message, or a placeholder for an answer with neither.
template <typename SdkResult>
std::optional<std::string> FailureOf(const std::optional<SdkResult>& result,
                                     const std::optional<std::string>& err) {
  if (err) return err->empty() ? std::string("error") : *err;
  if (!result) return std::string("no result");
  if (result->error) return result->error->message.empty() ? std::string("error") : result->error->message;
  return std::nullopt;
}

}  // namespace read

// A page refused because its cursor's snapshot expired: the server keeps
// snapshots 7 days and answers a cursor into an older one with HTTP 410. The
// sdk delivers a non-200 response as "<status line>: <body>" (connect's
// HttpStatusError), so the status leads the error: "410 Gone: The snapshot
// expired. Reload from the top." Sending the same cursor again fails forever.
inline bool IsSnapshotExpired(std::string_view error) {
  return 4 <= error.size() && error.substr(0, 3) == "410" && (error[3] == ' ' || error[3] == ':');
}

// The list is a strictly increasing run of positions inside the top 400: rows
// out of that range, and a second row at a position already read, are left
// out, whatever order the server sent them in.
inline void NormalizeRows(std::vector<Row>& rows) {
  std::erase_if(rows, [](const Row& row) { return row.position <= 0 || kListDepth < row.position; });
  std::stable_sort(rows.begin(), rows.end(),
                   [](const Row& a, const Row& b) { return a.position < b.position; });
  rows.erase(std::unique(rows.begin(), rows.end(),
                         [](const Row& a, const Row& b) { return a.position == b.position; }),
             rows.end());
}

template <typename SdkResult>
TopOutcome TopOutcomeFrom(const std::optional<SdkResult>& result,
                          const std::optional<std::string>& err) {
  TopOutcome out;
  if (const auto failure = read::FailureOf(result, err)) {
    out.error = *failure;
    return out;
  }
  // a page with no snapshot cannot say which board its rows are from
  const auto* snapshot = read::Pointer(result->snapshot);
  if (snapshot == nullptr) {
    out.error = "no snapshot";
    return out;
  }
  out.ok = true;
  out.snapshot = read::SnapshotFrom(*snapshot);
  if (const auto* rows = read::Pointer(result->rows)) {
    for (const auto& row : *rows) out.rows.push_back(read::RowFrom(row));
  }
  NormalizeRows(out.rows);
  if (const auto* me = read::Pointer(result->me)) {
    Row row = read::RowFrom(*me);
    if (0 < row.position) {
      row.isMe = true;
      out.me = std::move(row);
    }
  }
  out.next = read::ValueOr<std::string>(result->next, std::string());
  return out;
}

template <typename SdkResult>
MeOutcome MeOutcomeFrom(const std::optional<SdkResult>& result,
                        const std::optional<std::string>& err) {
  MeOutcome out;
  if (const auto failure = read::FailureOf(result, err)) {
    out.error = *failure;
    return out;
  }
  out.ok = true;
  if (const auto* snapshot = read::Pointer(result->snapshot)) out.snapshot = read::SnapshotFrom(*snapshot);
  if (const auto* me = read::Pointer(result->me)) {
    Me read;
    if (const auto* row = read::Pointer(me->row)) {
      read.row = read::RowFrom(*row);
      read.row->isMe = true;
    }
    read.soloScore = read::ValueOr<double>(me->solo_score, 0.0);
    read.distinctPrefixes = read::ValueOr<int64_t>(me->distinct_prefixes, int64_t{0});
    read.sharedPrefixes = read::ValueOr<int64_t>(me->shared_prefixes, int64_t{0});
    read.sharedWithUnits = read::ValueOr<int64_t>(me->shared_with_units, int64_t{0});
    read.reason = ReasonFrom(read::ValueOr<std::string>(me->not_measurable_reason, std::string()));
    if (const auto* history = read::Pointer(me->history)) {
      for (const auto& point : *history) {
        HistoryPoint day;
        day.day = read::ValueOr<std::string>(point.day, std::string());
        day.rank = read::ValueOr<int64_t>(point.rank, int64_t{0});
        day.score = read::ValueOr<double>(point.score, 0.0);
        read.history.push_back(std::move(day));
      }
    }
    out.me = std::move(read);
  }
  return out;
}

// ---- the words ------------------------------------------------------------------

// How the board's words are made: the store's strings and the user's numbers
// and dates, all UTF-8. ProtocolBoard passes urnw::Localized and Format and
// the Windows locale's numbers; the tests pass English and fakes.
struct Text {
  std::function<std::string(std::string_view key)> localized;
  // a key with one placeholder
  std::function<std::string(std::string_view key, const std::string& value)> format;
  // a key with two, in the store's order
  std::function<std::string(std::string_view key, const std::string& first, const std::string& second)> format2;
  // a count with the locale's grouping ("41,230")
  std::function<std::string(int64_t count)> count;
  // a score with one decimal ("41.2")
  std::function<std::string(double score)> score;
  // a 0-1 ratio as a percent with `decimals` decimals, in the locale's pattern
  std::function<std::string(double ratio, int decimals)> percent;
  // how long ago a time was ("4h ago")
  std::function<std::string(int64_t unixMillis)> ago;
  // a history day (YYYY-MM-DD) as the user's short date
  std::function<std::string(std::string_view day)> day;
};

inline std::string PositionText(int64_t position, const Text& text) { return "#" + text.count(position); }

// The size of a margin as a percent: whole percents, and one decimal under 1%
// so a network just over the line does not read "0%" (Apple's
// ProtocolFormat.percent).
inline std::string MarginPercent(double margin, const Text& text) {
  const double size = std::fabs(margin);
  const bool underOne = 0 < size && size * 100 < 0.95;
  // a tenth that rounds to a whole percent reads without its ".0"
  const bool tenth = underOne && static_cast<int64_t>(std::llround(size * 1000)) % 10 != 0;
  return text.percent(size, tenth ? 1 : 0);
}

// The margin to the line: "12% above the Top 200 line", or below. The 200th
// row's margin is 0, on the line, and reads as above it.
inline std::string MarginText(double margin, const Text& text) {
  const std::string percent = MarginPercent(margin, text);
  return text.format(margin < 0 ? "leaderboard_protocol_below_line" : "leaderboard_protocol_above_line",
                     percent);
}

// The margin to the line as a signed percent, for the tile's line under the
// position: "+12%" above the line (the 200th row's 0 included), and below it
// the minus sign U+2212 ahead of the percent.
inline std::string SignedMarginText(double margin, const Text& text) {
  return (margin < 0 ? std::string("\xE2\x88\x92") : std::string("+")) + MarginPercent(margin, text);
}

// The trend since the last tempo; "" when the row did not move.
inline std::string TrendText(int64_t trend, const Text& text) {
  if (trend == 0) return std::string();
  return text.format(0 < trend ? "leaderboard_protocol_trend_up" : "leaderboard_protocol_trend_down",
                     text.count(trend < 0 ? -trend : trend));
}

inline std::string UidText(int64_t uid, const Text& text) {
  return text.format("leaderboard_protocol_uid", std::to_string(uid));
}

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline std::string_view TrimSpace(std::string_view text) {
  while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
  return text;
}

// The fleets' UIDs, "UID 81, UID 77".
inline std::string UidsText(const Row& row, const Text& text) {
  std::string uids;
  for (const auto& fleet : row.fleets) {
    if (!uids.empty()) uids += ", ";
    uids += UidText(fleet.uid, text);
  }
  return uids;
}

// What a row is called (Apple's ProtocolName): its name when it opted in;
// else, claimed, its fleets' UIDs (Q12: an anonymous claimed network shows
// only "UID n"); else "Anonymous".
inline std::string RowName(const Row& row, const Text& text) {
  if (const std::string_view name = TrimSpace(row.name); !name.empty()) return std::string(name);
  if (row.Claimed()) return UidsText(row, text);
  return text.localized("anonymous");
}

// ---- the banners ------------------------------------------------------------------

struct Banner {
  std::string text;
  bool stale = false;  // the stale line rather than the claiming line

  bool operator==(const Banner&) const = default;
};

// Q16: "claiming isn't open yet" while binding is closed, neutral, gone once
// it opens; and "last updated" over a snapshot more than 3 tempos old, which
// needs the time it was computed to say anything.
inline std::vector<Banner> BannersFor(const std::optional<Snapshot>& snapshot, const Text& text) {
  std::vector<Banner> banners;
  if (!snapshot) return banners;
  if (!snapshot->bindingOpen) banners.push_back({text.localized("leaderboard_protocol_banner_closed"), false});
  if (snapshot->stale && 0 < snapshot->computedAtMillis) {
    banners.push_back(
        {text.format("leaderboard_protocol_banner_stale", text.ago(snapshot->computedAtMillis)), true});
  }
  return banners;
}

// ---- the progress chart (Q11) ----------------------------------------------------

// Days since 1970-01-01 for a YYYY-MM-DD day (Howard Hinnant's days_from_civil);
// nullopt for anything else.
inline std::optional<int64_t> DayNumber(std::string_view day) {
  if (day.size() != 10 || day[4] != '-' || day[7] != '-') return std::nullopt;
  auto number = [&](size_t at, size_t count) -> std::optional<int64_t> {
    int64_t value = 0;
    for (size_t i = at; i < at + count; ++i) {
      if (day[i] < '0' || '9' < day[i]) return std::nullopt;
      value = value * 10 + (day[i] - '0');
    }
    return value;
  };
  const auto y = number(0, 4), m = number(5, 2), d = number(8, 2);
  if (!y || !m || !d || *m < 1 || 12 < *m || *d < 1 || 31 < *d) return std::nullopt;
  const int64_t year = *y - (*m <= 2 ? 1 : 0);
  const int64_t era = (0 <= year ? year : year - 399) / 400;
  const int64_t yoe = year - era * 400;
  const int64_t doy = (153 * (*m + (2 < *m ? -3 : 9)) + 2) / 5 + *d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

struct ChartPoint {
  double x = 0;  // 0 the window's first day, 1 its last
  double y = 0;  // 0 the best rank on the chart (the top), 1 the worst
  int64_t rank = 0;
  std::string day;  // the user's date, for the point's label

  bool operator==(const ChartPoint&) const = default;
};

// The network's daily projected rank over the last 90 days, the line drawn
// across, the y axis inverted so up is better. The window ends on the last
// day the server sent, so a network ranked for a week fills the right edge;
// the rank axis always takes in the line, so it is always drawn.
struct Chart {
  bool shown = false;
  std::vector<ChartPoint> points;  // oldest first
  double lineY = 0;
  std::string title;
  std::string lineLabel;
  std::string spoken;  // the chart's accessible summary

  bool operator==(const Chart&) const = default;
};

inline Chart ChartFor(const std::vector<HistoryPoint>& history, int64_t headSeats, const Text& text) {
  Chart chart;
  struct Day {
    int64_t number;
    const HistoryPoint* point;
  };
  std::vector<Day> days;
  for (const auto& point : history) {
    const auto number = DayNumber(point.day);
    if (!number || point.rank <= 0) continue;
    days.push_back({*number, &point});
  }
  if (days.empty()) return chart;
  std::stable_sort(days.begin(), days.end(), [](const Day& a, const Day& b) { return a.number < b.number; });
  // one point a day: the last the server sent for it
  std::vector<Day> unique;
  for (const auto& day : days) {
    if (!unique.empty() && unique.back().number == day.number) {
      unique.back() = day;
    } else {
      unique.push_back(day);
    }
  }
  const int64_t last = unique.back().number;
  const int64_t first = last - (kChartDays - 1);
  std::erase_if(unique, [first](const Day& day) { return day.number < first; });
  int64_t best = headSeats, worst = headSeats;
  for (const auto& day : unique) {
    best = std::min(best, day.point->rank);
    worst = std::max(worst, day.point->rank);
  }
  const double span = static_cast<double>(std::max<int64_t>(1, worst - best));
  auto yOf = [&](int64_t rank) { return static_cast<double>(rank - best) / span; };
  for (const auto& day : unique) {
    chart.points.push_back({.x = static_cast<double>(day.number - first) / static_cast<double>(kChartDays - 1),
                            .y = yOf(day.point->rank),
                            .rank = day.point->rank,
                            .day = text.day(day.point->day)});
  }
  chart.lineY = yOf(headSeats);
  chart.shown = true;
  chart.title = text.localized("leaderboard_protocol_chart_title");
  chart.lineLabel = text.localized("leaderboard_protocol_chart_line");
  chart.spoken = text.format("leaderboard_protocol_chart_a11y", text.count(chart.points.back().rank));
  return chart;
}

// The point a pointer at `x` (0-1 across the chart) shows: the nearest day.
inline std::optional<size_t> NearestPoint(const Chart& chart, double x) {
  if (chart.points.empty()) return std::nullopt;
  size_t nearest = 0;
  for (size_t i = 1; i < chart.points.size(); ++i) {
    if (std::fabs(chart.points[i].x - x) < std::fabs(chart.points[nearest].x - x)) nearest = i;
  }
  return nearest;
}

// ---- the claim prompt (Q5, Q13, Q16) ----------------------------------------------

struct ClaimPrompt {
  bool shown = false;
  bool button = false;  // binding open: "Claim on Earnings", the screen's one primary button
  std::string inside;   // "You're projected in the top 200."
  std::string action;   // the button's words, or, binding closed, "Claiming opens soon..."
  std::string borderline;  // close to the line; "" otherwise
  std::string warning;     // every prompt carries it

  bool operator==(const ClaimPrompt&) const = default;
};

// Close to the line (M, Q13): the label says borderline, or, while every row
// reads "estimate" (Q17), the margin is within M of the line.
inline bool CloseToLine(const Row& row) {
  switch (row.label) {
    case Label::Borderline: return true;
    case Label::Estimate:
    case Label::None: return std::fabs(row.margin) < kNearLine;
    case Label::Likely:
    case Label::Unlikely:
    case Label::NotMeasurable: break;
  }
  return false;
}

// Only when the server flags the row (claim_prompt: measurable, projected
// inside the top 200, and not claimed already). CONTRACT.md, amended
// 2026-10-29: apps use the flag and never compute it themselves. While binding
// is closed a note takes the button's place and nothing invites registering
// early. A network close to the line hears that rankings move every tempo.
inline ClaimPrompt ClaimPromptFor(const Row& row, const Snapshot& snapshot, const Text& text) {
  ClaimPrompt prompt;
  if (!row.claimPrompt) return prompt;
  prompt.shown = true;
  prompt.button = snapshot.bindingOpen;
  prompt.inside = text.localized("leaderboard_protocol_claim_inside");
  prompt.action = text.localized(snapshot.bindingOpen ? "leaderboard_protocol_claim_button"
                                                      : "leaderboard_protocol_claim_soon");
  if (CloseToLine(row)) prompt.borderline = text.localized("leaderboard_protocol_claim_borderline");
  prompt.warning = text.localized("leaderboard_protocol_claim_warning");
  return prompt;
}

// ---- the own-status card ---------------------------------------------------------

enum class CardBody {
  SignIn,    // signed out: the public list only, a sign-in prompt in the card's place
  Loading,   // placeholders in the card's own sizes
  Failed,    // the retry row
  Unranked,  // the header, and why the validator cannot score it, if it says
  Ranked,
};

struct Card {
  CardBody body = CardBody::Loading;
  std::string tag;
  std::string name;  // the network's name, or "Anonymous" while it is not shown
  bool anonymous = true;
  std::string position;  // "#173"
  std::string projected;  // "projected"
  std::string ofRanked;   // "of 41,230 ranked networks"; "" with no count
  std::string margin;     // "12% above the Top 200 line"
  bool above = true;
  std::string trend;      // "up 9 since the last update"; "" when it did not move
  int trendDirection = 0;
  Label label = Label::None;
  std::string labelText;
  Tone tone = Tone::Neutral;
  Chart chart;
  std::string why;   // counts only, never IPs
  std::string solo;  // Q4
  std::string notMeasurable;  // why the validator cannot score it; no prompt then
  std::vector<std::string> uids;  // the network's claimed UIDs
  ClaimPrompt claim;

  bool operator==(const Card&) const = default;
};

// ---- the list ----------------------------------------------------------------------

struct ListRow {
  int64_t position = 0;
  std::string positionText;  // "173"
  std::string name;          // the name, "Anonymous", or "UID n"
  bool anonymous = false;    // the name is the anonymous word: it steps back
  std::string tag;
  std::string score;
  int trendDirection = 0;
  std::string trend;         // the arrow's count ("3"); "" when it did not move
  std::string trendSpoken;   // "up 3 since the last update"
  std::vector<std::string> uids;  // the claimed badges ("UID 81")
  bool claimed = false;      // opens the detail sheet
  bool dimmed = false;       // not measurable
  std::string dimmedSpoken;  // what the dim row's icon reads as
  bool insideLine = false;   // at or above the line: the faint accent tint
  bool isMe = false;
  std::string you;           // "You" on the caller's row
  Label label = Label::None; // the caller's row only, so the list stays readable
  std::string labelText;
  Tone tone = Tone::Neutral;
  std::string spoken;        // the whole row, for a screen reader

  bool operator==(const ListRow&) const = default;
};

struct ListItem {
  bool line = false;  // the Top 200 line, not a row
  ListRow row;
  std::string lineText;    // "Top 200 line · score 41.2"
  std::string lineSpoken;  // "Top 200 line, score 41.2"

  bool operator==(const ListItem&) const = default;
};

enum class ListBody {
  Placeholders,  // never loaded, and the load has not failed (or is being retried)
  Failed,        // the first load failed: the retry row
  Empty,         // "Rankings will appear after the next update."
  Rows,
};

struct ListView {
  ListBody body = ListBody::Placeholders;
  std::vector<ListItem> items;
  bool paging = false;         // a next page is on its way: the footer's spinner
  bool pageFailed = false;     // a next page failed: the footer's retry
  bool refreshFailed = false;  // a reload over the rows failed: they stay, with a notice
  int64_t total = 0;           // the position indicator's range: 1 to this (at most 400)

  bool operator==(const ListView&) const = default;
};

// The caller's place, pinned under the list while its own row is out of view,
// or ranked past 400.
struct YouBar {
  bool shown = false;
  int64_t position = 0;
  bool inList = false;  // a position the list reaches (1-400): a press seeks there
  std::string you;
  std::string positionText;
  std::string margin;

  bool operator==(const YouBar&) const = default;
};

// ---- the claimed-row detail sheet ------------------------------------------------

struct Fact {
  std::string label;
  std::string value;

  bool operator==(const Fact&) const = default;
};

struct FleetDetail {
  std::string title;     // "UID 81"
  std::string selected;  // "Selected", "Not selected", or "" before the validator says
  bool isSelected = false;
  std::vector<Fact> facts;

  bool operator==(const FleetDetail&) const = default;
};

struct Detail {
  std::string title;     // the row's name, or its first UID
  std::string subtitle;  // "kayak-hollow · claimed"
  std::vector<Fact> facts;  // the board's own: the projected rank
  std::vector<FleetDetail> fleets;
  std::string note;  // the incentive's lag, when any fleet shows one

  bool operator==(const Detail&) const = default;
};

inline Detail DetailFor(const Row& row, const Text& text) {
  Detail detail;
  detail.title = RowName(row, text);
  const std::string name = row.name.empty() ? text.localized("anonymous") : row.name;
  detail.subtitle = name + " \xC2\xB7 " + text.localized("leaderboard_protocol_detail_claimed");
  detail.facts.push_back(
      {text.localized("leaderboard_protocol_detail_projected_rank"), PositionText(row.position, text)});
  bool incentive = false;
  for (const auto& fleet : row.fleets) {
    FleetDetail out;
    out.title = UidText(fleet.uid, text);
    if (fleet.selected) {
      out.isSelected = *fleet.selected;
      out.selected = text.localized(*fleet.selected ? "leaderboard_protocol_detail_selected"
                                                    : "leaderboard_protocol_detail_not_selected");
    }
    if (fleet.validatorRank && 0 < *fleet.validatorRank) {
      const std::string rank = text.count(*fleet.validatorRank);
      out.facts.push_back({text.localized("leaderboard_protocol_detail_validator_rank"),
                           fleet.validatorRankOf && 0 < *fleet.validatorRankOf
                               ? text.format2("leaderboard_protocol_detail_validator_rank_value", rank,
                                              text.count(*fleet.validatorRankOf))
                               : "#" + rank});
    }
    if (fleet.validatorScore) {
      out.facts.push_back(
          {text.localized("leaderboard_protocol_detail_validator_score"), text.score(*fleet.validatorScore)});
    }
    out.facts.push_back(
        {text.localized("leaderboard_protocol_detail_replica_score"), text.score(fleet.replicaScore)});
    if (fleet.incentive) {
      incentive = true;
      std::string value = text.percent(*fleet.incentive, 1);
      if (fleet.incentiveStale) value += " \xC2\xB7 " + text.localized("leaderboard_protocol_detail_stale");
      out.facts.push_back({text.localized("leaderboard_protocol_detail_incentive"), value});
    }
    out.facts.push_back(
        {text.localized("leaderboard_protocol_detail_generation"), std::to_string(fleet.generation)});
    if (0 < fleet.validToEpoch) {
      out.facts.push_back({text.localized("leaderboard_protocol_detail_valid_until"),
                           text.format("leaderboard_protocol_detail_epoch", std::to_string(fleet.validToEpoch))});
    }
    detail.fleets.push_back(std::move(out));
  }
  if (incentive) detail.note = text.localized("leaderboard_protocol_detail_incentive_note");
  return detail;
}

// ---- the Earnings Top 200 tile (apple/LEADERTOP.md §8) ------------------------------

enum class TileBody {
  Hidden,     // signed out, not loaded, failed, or neither inside the top 200 nor claimed
  Claimable,  // a measurable network projected inside the top 200
  Claimed,    // a network with a claimed fleet
};

struct TileFleet {
  std::string uid;            // "UID 81"
  std::string validatorRank;  // "#1 of 142 claimed"; "" before the validator says
  std::string selected;       // "Selected" / "Not selected"; "" before it says
  bool isSelected = false;

  bool operator==(const TileFleet&) const = default;
};

struct Tile {
  TileBody body = TileBody::Hidden;
  Label label = Label::None;
  std::string labelText;
  Tone tone = Tone::Neutral;
  std::string position;   // "#173"
  std::string projected;  // "projected · +12%"
  std::string about;      // what the rank is
  // binding open: the existing claim flow, unchanged
  bool claimButton = false;  // "Claim your spot" (unclaimed)
  bool signButton = false;   // "Sign fleet binding" (bindings renew per epoch)
  std::string warning;       // the claim prompt's warning, under the buttons
  // binding closed: a note instead of the button, and nothing to sign
  std::string soonTitle;
  std::string soonBody;
  // claimed: the fleet's UID, validator rank and selection, as the detail sheet
  std::vector<TileFleet> fleets;
  std::string boundDetail;
  bool nearLine = false;  // a claimed fleet within M of the line, or below it

  bool operator==(const Tile&) const = default;
};

// The tile reads the card's answer (GET /sn/top/me), so the two agree.
inline Tile TileFor(const std::optional<Me>& me, const std::optional<Snapshot>& snapshot, const Text& text) {
  Tile tile;
  if (!me || !me->Ranked() || !snapshot) return tile;
  const Row& row = *me->row;
  const ClaimPrompt prompt = ClaimPromptFor(row, *snapshot, text);
  if (!row.Claimed() && !prompt.shown) return tile;
  tile.body = row.Claimed() ? TileBody::Claimed : TileBody::Claimable;
  tile.label = row.label;
  if (const char* key = LabelKey(row.label); *key) tile.labelText = text.localized(key);
  tile.tone = ToneOf(row.label);
  tile.position = PositionText(row.position, text);
  tile.projected = text.format("earnings_top200_projected", SignedMarginText(row.margin, text));
  tile.about = text.localized("earnings_top200_body");
  if (snapshot->bindingOpen) {
    tile.claimButton = tile.body == TileBody::Claimable;
    tile.signButton = true;
    if (tile.body == TileBody::Claimable) tile.warning = text.localized("leaderboard_protocol_claim_warning");
  } else if (tile.body == TileBody::Claimable) {
    tile.soonTitle = text.localized("earnings_top200_claiming_soon");
    tile.soonBody = text.localized("earnings_top200_claiming_soon_body");
  }
  if (tile.body == TileBody::Claimed) {
    for (const auto& fleet : row.fleets) {
      TileFleet out;
      out.uid = UidText(fleet.uid, text);
      if (fleet.validatorRank && 0 < *fleet.validatorRank) {
        const std::string rank = text.count(*fleet.validatorRank);
        out.validatorRank = fleet.validatorRankOf && 0 < *fleet.validatorRankOf
                                ? text.format2("leaderboard_protocol_detail_validator_rank_value", rank,
                                               text.count(*fleet.validatorRankOf))
                                : "#" + rank;
      }
      if (fleet.selected) {
        out.isSelected = *fleet.selected;
        out.selected = text.localized(*fleet.selected ? "leaderboard_protocol_detail_selected"
                                                      : "leaderboard_protocol_detail_not_selected");
      }
      tile.fleets.push_back(std::move(out));
    }
    tile.boundDetail = text.localized("top200_bound_detail");
    tile.nearLine = row.margin < kNearLine;
  }
  return tile;
}

// ---- what the board knows, and its model ---------------------------------------

// UI thread only.
struct State {
  // the list (GET /sn/top)
  bool listLoaded = false;   // a first page came back since the board opened
  bool listLoading = false;  // a reload (the first page) is out
  bool listFailed = false;   // the last reload failed
  bool pageLoading = false;  // a next page is out
  bool pageFailed = false;   // the last next page failed
  std::optional<Snapshot> snapshot;
  std::vector<Row> rows;
  std::optional<Row> meRow;  // the caller's row from the list's answer, wherever it ranks
  std::string next;
  int64_t loadedAtMillis = 0;
  // a position the list is loading through, to scroll to once it lands; 0 none
  int64_t seekTarget = 0;
  // the card and the tile (GET /sn/top/me)
  bool meLoaded = false;
  bool meLoading = false;
  bool meFailed = false;
  std::optional<Me> me;
  std::optional<Snapshot> meSnapshot;
};

// The snapshot the board speaks from: the list's, else the card's.
inline const std::optional<Snapshot>& BoardSnapshot(const State& state) {
  return state.snapshot ? state.snapshot : state.meSnapshot;
}

// The caller's row: the card's answer, else the list's.
inline std::optional<Row> OwnRow(const State& state) {
  if (state.me && state.me->Ranked()) return state.me->row;
  if (state.meRow && 0 < state.meRow->position) return state.meRow;
  return std::nullopt;
}

// Each answer comes back on the sdk's thread and is applied on the UI thread
// later, and by then the board may have been reset (a sign-out) and opened
// again, for another account. Each open takes a new number, each request
// carries the number it went out under, and only the open board's answers
// apply (Admits). Within an open, only the latest list request's answer and
// the latest card request's answer apply, so a reload overtaken by a newer one
// cannot paint an older snapshot. UI thread only.
class Model {
 public:
  // What one list request asks for.
  struct PageRequest {
    uint64_t load = 0;
    std::string cursor;  // "" for the first page
    int64_t limit = kPageSize;
    bool first = true;
  };

  uint64_t Open() {
    state_ = {};
    ++lastLoad_;
    ++lastMe_;
    open_ = ++lastOpen_;
    return open_;
  }
  void Close() {
    state_ = {};
    ++lastLoad_;
    ++lastMe_;
    open_ = 0;
  }
  uint64_t Current() const { return open_; }
  bool Admits(uint64_t generation) const { return generation != 0 && generation == open_; }

  // The first page, from a fresh snapshot: the first load, a refresh, a retry.
  // A next page still out is dropped.
  PageRequest BeginReload() {
    state_.listLoading = true;
    state_.pageLoading = false;
    state_.pageFailed = false;
    return PageRequest{.load = ++lastLoad_, .cursor = {}, .limit = kPageSize, .first = true};
  }

  // The page after the loaded rows, pinned to their snapshot by the cursor;
  // nullopt when one is out, the list ended, or it reached 400.
  std::optional<PageRequest> BeginNextPage(int64_t limit = kPageSize) {
    if (!state_.listLoaded || state_.listLoading || state_.pageLoading || state_.next.empty()) return std::nullopt;
    if (!state_.rows.empty() && kListDepth <= state_.rows.back().position) return std::nullopt;
    state_.pageLoading = true;
    state_.pageFailed = false;
    return PageRequest{.load = ++lastLoad_, .cursor = state_.next,
                       .limit = std::clamp<int64_t>(limit, 1, kSeekPageSize), .first = false};
  }

  enum class Applied {
    Dropped,  // not the latest request's answer
    Applied,
    // a page from another snapshot, or one whose snapshot expired (410): the
    // list reloads from the first page of the newest snapshot
    Restart,
  };

  Applied ApplyPage(const PageRequest& request, TopOutcome outcome, int64_t nowMillis) {
    if (request.load != lastLoad_) return Applied::Dropped;
    if (request.first) {
      state_.listLoading = false;
      if (!outcome.ok) {
        // a list on screen stays, with its notice
        state_.listFailed = true;
        state_.seekTarget = 0;
        return Applied::Applied;
      }
      state_.listFailed = false;
      state_.listLoaded = true;
      state_.snapshot = std::move(outcome.snapshot);
      state_.rows = std::move(outcome.rows);
      state_.meRow = std::move(outcome.me);
      state_.next = std::move(outcome.next);
      state_.loadedAtMillis = nowMillis;
      return Applied::Applied;
    }
    state_.pageLoading = false;
    if (!outcome.ok) {
      // the cursor's snapshot expired: the first page of the newest snapshot,
      // never the dead cursor again (a seek carries on through the new pages)
      if (IsSnapshotExpired(outcome.error)) return Applied::Restart;
      state_.pageFailed = true;
      state_.seekTarget = 0;
      return Applied::Applied;
    }
    if (state_.snapshot && outcome.snapshot && !state_.snapshot->id.empty() && !outcome.snapshot->id.empty() &&
        state_.snapshot->id != outcome.snapshot->id) {
      return Applied::Restart;
    }
    // only what extends the list: a page that overlaps the loaded rows adds
    // its rows past them
    const int64_t last = state_.rows.empty() ? 0 : state_.rows.back().position;
    for (auto& row : outcome.rows) {
      if (last < row.position) state_.rows.push_back(std::move(row));
    }
    NormalizeRows(state_.rows);
    if (outcome.me) state_.meRow = std::move(outcome.me);
    state_.next = std::move(outcome.next);
    return Applied::Applied;
  }

  uint64_t BeginMe() {
    state_.meLoading = true;
    return ++lastMe_;
  }

  // A failed card read keeps a card on screen, and drops nothing else.
  bool ApplyMe(uint64_t load, MeOutcome outcome) {
    if (load != lastMe_) return false;
    state_.meLoading = false;
    if (!outcome.ok) {
      state_.meFailed = true;
      return true;
    }
    state_.meFailed = false;
    state_.meLoaded = true;
    state_.me = std::move(outcome.me);
    state_.meSnapshot = std::move(outcome.snapshot);
    return true;
  }

  // The indicator released at `position`: a position the list has not reached
  // is loaded through and then scrolled to; true when pages are needed.
  bool Seek(int64_t position) {
    position = std::clamp<int64_t>(position, 1, kListDepth);
    if (!state_.rows.empty() && position <= state_.rows.back().position) {
      state_.seekTarget = 0;
      return false;
    }
    if (state_.next.empty()) {
      state_.seekTarget = 0;
      return false;
    }
    state_.seekTarget = position;
    return true;
  }
  // A seek is still short of its position, and another page can bring it.
  bool WantsSeekPage() const {
    return 0 < state_.seekTarget && !state_.next.empty() && !state_.pageFailed && !state_.pageLoading &&
           (state_.rows.empty() || state_.rows.back().position < state_.seekTarget);
  }
  // The seek's position is loaded: it is taken, once.
  std::optional<int64_t> TakeLandedSeek() {
    if (state_.seekTarget <= 0) return std::nullopt;
    const bool reached = !state_.rows.empty() && state_.seekTarget <= state_.rows.back().position;
    if (!reached && !state_.next.empty() && !state_.pageFailed) return std::nullopt;
    const int64_t target = state_.seekTarget;
    state_.seekTarget = 0;
    return reached ? std::optional<int64_t>(target) : std::nullopt;
  }

  // A board shown again after a while reloads from a fresh snapshot.
  bool ShouldRefresh(int64_t nowMillis) const {
    return state_.listLoaded && !state_.listLoading && 0 < state_.loadedAtMillis &&
           kRefreshAfterMillis <= nowMillis - state_.loadedAtMillis;
  }

  const State& state() const { return state_; }

 private:
  State state_;
  uint64_t lastOpen_ = 0;
  uint64_t open_ = 0;
  uint64_t lastLoad_ = 0;
  uint64_t lastMe_ = 0;
};

// ---- the views ------------------------------------------------------------------

// The card from what the board knows. `signedIn` is the session's; `namePublic`
// the "Show my network name" switch (points_leaderboard_public, shared with
// the Points board), and `ownName` and `ownTag` the network's own, which the
// header shows only while the switch is on.
inline Card CardFor(const State& state, bool signedIn, bool namePublic, const std::string& ownName,
                    const std::string& ownTag, const Text& text) {
  Card card;
  if (!signedIn) {
    card.body = CardBody::SignIn;
    return card;
  }
  if (!state.meLoaded) {
    card.body = state.meFailed && !state.meLoading ? CardBody::Failed : CardBody::Loading;
    return card;
  }
  const Me empty;
  const Me& me = state.me ? *state.me : empty;
  const std::string rowName = me.row ? me.row->name : std::string();
  card.anonymous = !namePublic;
  if (namePublic) card.name = !rowName.empty() ? rowName : ownName;
  if (card.name.empty()) {
    card.anonymous = true;
    card.name = text.localized("anonymous");
  }
  card.tag = me.row && !me.row->tag.empty() ? me.row->tag : ownTag;
  if (const char* key = ReasonKey(me.reason); *key) card.notMeasurable = text.localized(key);
  if (!me.Ranked()) {
    card.body = CardBody::Unranked;
    return card;
  }
  const Row& row = *me.row;
  const std::optional<Snapshot>& snapshot = BoardSnapshot(state);
  card.body = CardBody::Ranked;
  card.position = PositionText(row.position, text);
  card.projected = text.localized("leaderboard_protocol_projected");
  if (snapshot && 0 < snapshot->totalRanked) {
    card.ofRanked = text.format("leaderboard_protocol_of_ranked", text.count(snapshot->totalRanked));
  }
  card.margin = MarginText(row.margin, text);
  card.above = 0 <= row.margin;
  card.trend = TrendText(row.trend, text);
  card.trendDirection = row.trend == 0 ? 0 : (0 < row.trend ? 1 : -1);
  card.label = row.label;
  if (const char* key = LabelKey(row.label); *key) card.labelText = text.localized(key);
  card.tone = ToneOf(row.label);
  card.chart = ChartFor(me.history, snapshot ? snapshot->headSeats : kHeadSeats, text);
  card.why = text.format2("leaderboard_protocol_why", text.count(me.distinctPrefixes),
                          text.count(me.sharedPrefixes));
  card.solo = text.format("leaderboard_protocol_solo", text.score(me.soloScore));
  for (const auto& fleet : row.fleets) card.uids.push_back(UidText(fleet.uid, text));
  // the prompt is the server's flag, never the app's reckoning (the server
  // leaves it off for a network it cannot score)
  if (snapshot) card.claim = ClaimPromptFor(row, *snapshot, text);
  // not measurable: why, and the chip says so when the server's label does not
  if (!row.measurable || row.label == Label::NotMeasurable) {
    if (card.labelText.empty()) {
      card.labelText = text.localized(LabelKey(Label::NotMeasurable));
      card.label = Label::NotMeasurable;
      card.tone = Tone::Muted;
    }
    return card;
  }
  card.notMeasurable.clear();
  return card;
}

inline ListRow ListRowFor(const Row& row, bool isMe, int64_t seats, const Text& text) {
  ListRow out;
  out.position = row.position;
  out.positionText = text.count(row.position);
  out.name = RowName(row, text);
  const bool named = !TrimSpace(row.name).empty();
  out.anonymous = !named && !row.Claimed();
  out.tag = row.tag;
  out.score = text.score(row.projectedScore);
  out.trendDirection = row.trend == 0 ? 0 : (0 < row.trend ? 1 : -1);
  if (row.trend != 0) out.trend = text.count(row.trend < 0 ? -row.trend : row.trend);
  out.trendSpoken = TrendText(row.trend, text);
  // an anonymous claimed row is named by its UIDs already: no badges then
  if (named) {
    for (const auto& fleet : row.fleets) out.uids.push_back(UidText(fleet.uid, text));
  }
  out.claimed = row.Claimed();
  out.dimmed = !row.measurable;
  if (out.dimmed) out.dimmedSpoken = text.localized(LabelKey(Label::NotMeasurable));
  out.insideLine = row.position <= seats;
  out.isMe = isMe;
  if (isMe) {
    out.you = text.localized("leaderboard_protocol_you");
    out.label = row.label;
    if (const char* key = LabelKey(row.label); *key) out.labelText = text.localized(key);
    out.tone = ToneOf(row.label);
  }
  // the row as one sentence (Apple's ProtocolRowAccessibility): the caller
  // first, then the rank, the name, the score and its trend, the claimed
  // UIDs, and the label on the caller's own row or "Not measurable" on a
  // dimmed one
  std::string spoken = isMe ? out.you + ", " : std::string();
  spoken += PositionText(row.position, text) + ", " + out.name + ", " + out.score;
  if (!out.trendSpoken.empty()) spoken += ", " + out.trendSpoken;
  if (!out.uids.empty()) spoken += ", " + UidsText(row, text);
  if (isMe && !out.labelText.empty()) {
    spoken += ", " + out.labelText;
  } else if (out.dimmed) {
    spoken += ", " + out.dimmedSpoken;
  }
  out.spoken = std::move(spoken);
  return out;
}

// The list from what the board knows: the rows by position, the line between
// the head's last seat and the next, the caller's row marked wherever it falls.
inline ListView ListFor(const State& state, const Text& text) {
  ListView view;
  if (!state.listLoaded) {
    view.body = state.listFailed && !state.listLoading ? ListBody::Failed : ListBody::Placeholders;
    return view;
  }
  view.refreshFailed = state.listFailed;
  view.paging = state.pageLoading;
  view.pageFailed = state.pageFailed && !state.pageLoading;
  if (state.rows.empty()) {
    view.body = ListBody::Empty;
    return view;
  }
  view.body = ListBody::Rows;
  const std::optional<Snapshot>& snapshot = state.snapshot;
  const int64_t seats = snapshot && 0 < snapshot->headSeats ? snapshot->headSeats : kHeadSeats;
  const std::optional<Row> own = OwnRow(state);
  for (const auto& row : state.rows) {
    const bool isMe = row.isMe || (own && own->position == row.position);
    ListItem item;
    item.row = ListRowFor(row, isMe, seats, text);
    view.items.push_back(std::move(item));
    if (row.position == seats) {
      const std::string score = text.score(snapshot ? snapshot->projectedCutoff : row.projectedScore);
      ListItem line;
      line.line = true;
      line.lineText = text.format("leaderboard_protocol_line", score);
      line.lineSpoken = text.format("leaderboard_protocol_line_a11y", score);
      view.items.push_back(std::move(line));
    }
  }
  // the positions the list can reach: the snapshot's count, up to 400, until
  // the list has ended, and then its last row
  const int64_t last = state.rows.back().position;
  const int64_t ranked = snapshot ? snapshot->totalRanked : 0;
  view.total = std::clamp<int64_t>(state.next.empty() ? last : std::max<int64_t>(ranked, last), 1, kListDepth);
  return view;
}

inline YouBar YouBarFor(const State& state, bool signedIn, const Text& text) {
  YouBar bar;
  if (!signedIn) return bar;
  const std::optional<Row> own = OwnRow(state);
  if (!own) return bar;
  bar.shown = true;
  bar.position = own->position;
  bar.inList = own->position <= kListDepth;
  bar.you = text.localized("leaderboard_protocol_you");
  bar.positionText = PositionText(own->position, text);
  bar.margin = MarginText(own->margin, text);
  return bar;
}

// ---- the list's geometry -------------------------------------------------------------
//
// Every row is `rowHeight` tall from `rowsTop` in the scrolled content, and
// the line's `lineHeight` follows the row at index `lineAfter` (-1 for no
// line). The position indicator and a seek read the list through these.

// Where the row at `index` starts.
inline double RowTop(int64_t index, double rowsTop, double rowHeight, double lineHeight, int64_t lineAfter) {
  const double lineAbove = 0 <= lineAfter && lineAfter < index ? lineHeight : 0;
  return rowsTop + static_cast<double>(index) * rowHeight + lineAbove;
}

// The index of the first row in view at `offset`; a view above the rows reads
// as the first, and the line reads as the row after it.
inline int64_t FirstVisibleIndex(double offset, double rowsTop, double rowHeight, double lineHeight,
                                 int64_t lineAfter, int64_t rowCount) {
  if (rowCount <= 0 || rowHeight <= 0) return 0;
  double y = std::max(0.0, offset - rowsTop);
  if (0 <= lineAfter) {
    const double lineTop = static_cast<double>(lineAfter + 1) * rowHeight;
    if (lineTop <= y) y = std::max(lineTop, y - lineHeight);
  }
  const auto index = static_cast<int64_t>(std::floor(y / rowHeight));
  return std::clamp<int64_t>(index, 0, rowCount - 1);
}

// The row a claimed position names, for its detail sheet.
inline std::optional<Row> RowAt(const State& state, int64_t position) {
  for (const auto& row : state.rows) {
    if (row.position == position) return row;
  }
  return std::nullopt;
}

}  // namespace urnw::protocolboard
