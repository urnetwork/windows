// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose — see ProviderLocations.h. App.vcxproj compiles this with
// PrecompiledHeader=NotUsing.
#include "ProviderLocations.h"

#include <algorithm>
#include <cstdio>

namespace urnw {

std::string PlaceLabel(const ProviderLocationRow& row) {
  std::string label;
  for (const std::string* part : {&row.city, &row.region, &row.country}) {
    if (part->empty()) continue;
    if (!label.empty()) label += ", ";
    label += *part;
  }
  return label;
}

std::string CoordinatesLabel(const ProviderLocationRow& row) {
  if (!row.hasCoordinates) return "\xE2\x80\x94";  // em dash, utf-8
  char buffer[64];
  // C locale: the SDK coordinates are data, so the decimal separator stays '.'
  // in every language (android formats with Locale.US for the same reason)
  std::snprintf(buffer, sizeof(buffer), "%.4f, %.4f", row.lat, row.lon);
  return std::string(buffer);
}

ConnectedDuration SplitConnectedDuration(int64_t connectedSinceMillis, int64_t nowMillis) {
  if (connectedSinceMillis <= 0) return ConnectedDuration{};
  const int64_t elapsedSeconds = (std::max)(int64_t{0}, (nowMillis - connectedSinceMillis) / 1000);
  ConnectedDuration duration;
  duration.valid = true;
  duration.hours = elapsedSeconds / 3600;
  duration.minutes = (elapsedSeconds % 3600) / 60;
  duration.seconds = elapsedSeconds % 60;
  return duration;
}

}  // namespace urnw
