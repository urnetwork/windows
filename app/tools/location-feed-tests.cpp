// Executable spec for the location feed's decisions (App/LocationFeed.h): the
// query as the SDK trims it, what the chooser sheet says about the feed, and when
// an open view controller runs its filter again. Run on any host with a C++20
// compiler:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App location-feed-tests.cpp -o /tmp/lf && /tmp/lf
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <string_view>

#include "LocationFeed.h"

namespace {

int failures = 0;

void Check(bool passed, std::string_view name) {
  std::cout << (passed ? "ok   " : "FAIL ") << name << "\n";
  if (!passed) ++failures;
}

constexpr std::string_view kLoaded = "LOCATIONS_LOADED";

}  // namespace

int main() {
  using urnw::ChooserFeedNotice;
  using urnw::LocationFeedNotice;
  using urnw::LocationFilterRunNeeded;
  using urnw::TrimLocationQuery;
  using urnw::kLocationFeedError;
  using urnw::kLocationFeedLoading;

  Check(TrimLocationQuery("  germ \t") == "germ", "surrounding space is trimmed");
  Check(TrimLocationQuery("   ").empty(), "a box of spaces is the idle list");
  Check(TrimLocationQuery("new york") == "new york", "inner space stays");

  // Empty buckets mean three different things, by state.
  Check(ChooserFeedNotice(kLocationFeedLoading, true, false, true) == LocationFeedNotice::Loading,
        "a search in flight with no rows is loading, not no results");
  Check(ChooserFeedNotice(kLocationFeedLoading, true, false, false) == LocationFeedNotice::Loading,
        "an idle load with no rows is loading, not a blank list");
  Check(ChooserFeedNotice("", false, false, false) == LocationFeedNotice::Loading,
        "a feed that has not answered yet is loading");
  Check(ChooserFeedNotice(kLocationFeedError, false, false, false) == LocationFeedNotice::Failed,
        "a failed first load says so");
  Check(ChooserFeedNotice(kLocationFeedError, true, false, true) == LocationFeedNotice::Failed,
        "a failed search with no rows says so");
  Check(ChooserFeedNotice(kLoaded, true, false, true) == LocationFeedNotice::NoResults,
        "a search answered with nothing is no results");
  Check(ChooserFeedNotice(kLoaded, true, false, false) == LocationFeedNotice::NoResults,
        "an idle list answered with nothing is no results");

  // Rows on screen.
  Check(ChooserFeedNotice(kLocationFeedLoading, true, true, true) == LocationFeedNotice::None,
        "rows stay quiet while a newer search loads");
  Check(ChooserFeedNotice(kLoaded, true, true, true) == LocationFeedNotice::None,
        "an answered search with rows says nothing");
  Check(ChooserFeedNotice(kLocationFeedError, true, true, false) == LocationFeedNotice::None,
        "a failed idle refresh keeps a usable list");
  Check(ChooserFeedNotice(kLocationFeedError, true, true, true) == LocationFeedNotice::Failed,
        "a failed search does not pass the last query's rows off as its answer");

  Check(LocationFilterRunNeeded(true, kLoaded), "a moved query runs");
  Check(LocationFilterRunNeeded(false, kLocationFeedError), "a failed load retries on arming");
  Check(!LocationFilterRunNeeded(false, kLoaded), "an answered query does not refetch");
  Check(!LocationFilterRunNeeded(false, kLocationFeedLoading), "a load in flight is left alone");

  if (failures != 0) {
    std::cout << failures << " failed\n";
    return 1;
  }
  std::cout << "location feed: all passed\n";
  return 0;
}
