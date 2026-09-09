// The pure math behind the points board's draggable position indicator and
// the board tabs' reset rule (mmm/DESIGNSTYLE.md "Long ranked lists: tab
// reset and a draggable position indicator"). No GTK, no SDK: the page feeds
// it the controller's window (first loaded position, loaded row count, total
// ranked) and the scroller's numbers, and draws what comes back. The same
// file as linux/app/src/LeaderboardIndicator.hpp, whose tests cover it.
#pragma once

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <string>

namespace urnw {
namespace leaderboard {

// the thumb: at least 44px tall (a finger), 24px wide, on a faint track
constexpr double kThumbMinHeight = 44.0;
constexpr double kThumbWidth = 24.0;
// rows within reach of the loaded window's first row that ask for the page before
constexpr int64_t kLoadBeforeThreshold = 3;

// the SDK's tier constants (urnet::PointsLeaderboardTier*), repeated here so
// the header stays SDK-free for the test runner
constexpr int64_t kTierUnknown = 0;
constexpr int64_t kTierTop1 = 1;
constexpr int64_t kTierTop5 = 2;
constexpr int64_t kTierTop10 = 3;
constexpr int64_t kTierTop25 = 4;
constexpr int64_t kTierTop50 = 5;
constexpr int64_t kTierRest = 6;

struct Thumb {
  bool visible = false;
  double top = 0;     // from the track's top, px
  double height = 0;  // px
};

// The thumb for a track `trackHeight` px tall: its top at the first row's
// position over the total, its length the loaded window over the total,
// never shorter than the minimum, never past the track's end. Hidden while
// the list is shorter than the viewport or nothing is ranked.
inline Thumb ThumbFor(int64_t firstPosition, int64_t windowRows, int64_t total,
                      double trackHeight, double contentHeight, double viewportHeight) {
  Thumb thumb;
  if (total <= 0 || trackHeight <= 0 || contentHeight <= viewportHeight) return thumb;
  const double n = static_cast<double>(total);
  const double window = static_cast<double>(std::clamp<int64_t>(windowRows, 0, total));
  thumb.height = std::clamp(window / n * trackHeight, std::min(kThumbMinHeight, trackHeight),
                            trackHeight);
  const double first = static_cast<double>(std::clamp<int64_t>(firstPosition, 1, total));
  thumb.top = std::clamp((first - 1.0) / n * trackHeight, 0.0, trackHeight - thumb.height);
  thumb.visible = true;
  return thumb;
}

// The rank the thumb's top stands for on a track: rank 1 at the top, the
// total at the bottom of the thumb's travel. Inverse of ThumbFor's top.
inline int64_t RankForThumbTop(double thumbTop, double thumbHeight, double trackHeight,
                               int64_t total) {
  if (total <= 0) return 1;
  const double travel = trackHeight - thumbHeight;
  if (travel <= 0) return 1;
  const double fraction = std::clamp(thumbTop / travel, 0.0, 1.0);
  const int64_t rank = 1 + static_cast<int64_t>(std::llround(fraction * static_cast<double>(total - 1)));
  return std::clamp<int64_t>(rank, 1, total);
}

// The 1-based position of the first row in view, from the scroller: `offset`
// the scrolled distance, `rowsTop` where the first row starts in the scrolled
// content (the table header above it), `rowHeight` every row's height,
// `firstPosition` the loaded window's first position. A view above the rows
// reads as the first row.
inline int64_t FirstVisiblePosition(double offset, double rowsTop, double rowHeight,
                                    int64_t firstPosition, int64_t rowCount) {
  if (rowCount <= 0 || rowHeight <= 0) return firstPosition;
  const int64_t row = static_cast<int64_t>(std::floor(std::max(0.0, offset - rowsTop) / rowHeight));
  return firstPosition + std::clamp<int64_t>(row, 0, rowCount - 1);
}

// Whether the view reached the loaded window's first rows and the page before
// them should be asked for.
inline bool ShouldLoadMoreBefore(int64_t firstVisibleRow, bool loading, bool hasMoreBefore,
                                 bool hasError, int64_t threshold = kLoadBeforeThreshold) {
  if (loading || !hasMoreBefore || hasError || firstVisibleRow < 0) return false;
  return firstVisibleRow <= threshold;
}

// Activating a board tab -- the already-active one included -- scrolls that
// list to the top; the Points board also reloads its window from the top when
// the window no longer starts at position 1 (after a seek or backward paging).
struct TabReset {
  bool scrollToTop = false;
  bool reloadFromTop = false;
};

inline TabReset TabResetFor(bool pointsBoard, bool hasController, int64_t firstLoadedPosition) {
  TabReset reset;
  reset.scrollToTop = true;
  reset.reloadFromTop = pointsBoard && hasController && firstLoadedPosition > 1;
  return reset;
}

// The store key for a tier line under the rank ("Top 5%", "Everyone else");
// empty for an unknown tier. `leaderboard_tier_top` takes {percent}.
inline const char* TierLabelKey(int64_t tier) {
  switch (tier) {
    case kTierTop1:
    case kTierTop5:
    case kTierTop10:
    case kTierTop25:
    case kTierTop50:
      return "leaderboard_tier_top";
    case kTierRest:
      return "leaderboard_tier_rest";
    default:
      return "";
  }
}

// A rank with the locale's thousands grouping ("1,240"); the separator from
// the C locale's lconv, "," when the locale has none.
inline std::string GroupedRank(int64_t rank, const char* separator = nullptr) {
  std::string sep = separator != nullptr ? std::string(separator) : std::string();
  if (separator == nullptr) {
    if (const lconv* lc = std::localeconv(); lc != nullptr && lc->thousands_sep != nullptr) {
      sep = lc->thousands_sep;
    }
    if (sep.empty()) sep = ",";
  }
  std::string digits = std::to_string(rank < 0 ? -rank : rank);
  std::string out;
  out.reserve(digits.size() + digits.size() / 3 * sep.size() + 1);
  const size_t lead = digits.size() % 3;
  for (size_t i = 0; i < digits.size(); ++i) {
    if (i != 0 && (i + 3 - lead) % 3 == 0) out += sep;
    out += digits[i];
  }
  return rank < 0 ? "-" + out : out;
}

// Keyboard steps on the thumb: an arrow moves by one loaded window, a page key
// by a tenth of the list, Home and End to the ends; the result is the rank to
// seek to.
enum class Step { Up, Down, PageUp, PageDown, Home, End };

inline int64_t StepRank(int64_t rank, Step step, int64_t windowRows, int64_t total) {
  if (total <= 0) return 1;
  const int64_t window = std::max<int64_t>(1, windowRows);
  const int64_t page = std::max<int64_t>(window, total / 10);
  int64_t next = rank;
  switch (step) {
    case Step::Up: next = rank - window; break;
    case Step::Down: next = rank + window; break;
    case Step::PageUp: next = rank - page; break;
    case Step::PageDown: next = rank + page; break;
    case Step::Home: next = 1; break;
    case Step::End: next = total; break;
  }
  return std::clamp<int64_t>(next, 1, total);
}

}  // namespace leaderboard
}  // namespace urnw
