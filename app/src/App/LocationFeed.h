// The location feed's small decisions, apart from the SDK and XAML so
// tools/location-feed-tests.cpp can run them on any host: the query as the SDK
// reads it, what the chooser sheet says about the feed, and when an open view
// controller has to run its filter again.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace urnw {

// The SDK's FilterLocationsState strings (sdk/locations_view_controller.go).
inline constexpr std::string_view kLocationFeedLoading = "LOCATIONS_LOADING";
inline constexpr std::string_view kLocationFeedError = "LOCATIONS_ERROR";

// Trimmed as LocationsViewController::FilterLocations trims, so a box holding
// only spaces is the idle list and not a search for " ".
inline std::string TrimLocationQuery(std::string query) {
  const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
  query.erase(query.begin(), std::find_if(query.begin(), query.end(), notSpace));
  query.erase(std::find_if(query.rbegin(), query.rend(), notSpace).base(), query.end());
  return query;
}

enum class LocationFeedNotice { None, Loading, Failed, NoResults };

// What the chooser says over its sections. `rows` is whether a section the
// sheet renders for this query has anything in it. The state comes first: a
// loading push and a loaded push with no providers carry the same empty buckets.
// A failed search keeps the previous query's rows (the SDK keeps them through an
// outage), so they do not answer it; a failed idle refresh keeps a usable list.
inline LocationFeedNotice ChooserFeedNotice(std::string_view state, bool hasSnapshot, bool rows,
                                            bool searching) {
  if (state == kLocationFeedError) {
    return rows && !searching ? LocationFeedNotice::None : LocationFeedNotice::Failed;
  }
  if (rows) return LocationFeedNotice::None;
  if (state == kLocationFeedLoading || !hasSnapshot) return LocationFeedNotice::Loading;
  return LocationFeedNotice::NoResults;
}

// Whether an open view controller must run its filter: the wanted query moved,
// or its last answer failed. The SDK schedules no retry, so each arming is one.
inline bool LocationFilterRunNeeded(bool queryChanged, std::string_view state) {
  return queryChanged || state == kLocationFeedError;
}

}  // namespace urnw
