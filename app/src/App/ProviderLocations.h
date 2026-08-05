// The connected-provider rows behind the provider-locations view, plus the pure
// label/ordering logic the globe and the list share — a port of the android
// ProviderLocationRow / ProviderLocationsViewModel data layer
// (ui/connect/providerlocations/, see sdk/PROVIDERLOCATIONS.md).
//
// The SDK returns ConnectedProviderLocation sorted oldest-connected first;
// SdkHost::CurrentProviderLocations maps it into these rows, dedupes by value
// (the change listener is signal-only and the SDK re-emits on every window
// event, so an identity compare would thrash the UI), and pushes them here.
//
// Pure standard C++ — no WinRT, no localization — so the android JVM tests port
// straight across to tools/globe-tests.cpp. Anything user-facing that needs the
// string store (the "unknown" placeholder, the duration text) is formatted in
// ProviderLocationsSheet.cpp from the primitives below. App.vcxproj compiles
// this with PrecompiledHeader=NotUsing.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace urnw {

// One connected provider, as rendered by the globe and the list.
struct ProviderLocationRow {
  // the EGRESS provider client id (Destination tail) -- the id to display and
  // copy, not the window-local ephemeral id
  std::string clientId;
  std::string country;
  std::string countryCode;  // lowercase; feeds getColorHex
  std::string region;
  std::string city;
  bool hasLocation = false;
  // the coordinates to plot: the city centroid when known, else the region
  // centroid. hasCoordinates is false when the provider has neither
  bool hasCoordinates = false;
  double lat = 0;
  double lon = 0;
  int64_t connectedSinceMillis = 0;

  bool Plottable() const { return hasCoordinates; }

  bool operator==(const ProviderLocationRow& other) const {
    return clientId == other.clientId && country == other.country &&
           countryCode == other.countryCode && region == other.region && city == other.city &&
           hasLocation == other.hasLocation && hasCoordinates == other.hasCoordinates &&
           lat == other.lat && lon == other.lon &&
           connectedSinceMillis == other.connectedSinceMillis;
  }
  bool operator!=(const ProviderLocationRow& other) const { return !(*this == other); }
};

// "City, Region, Country" -- omitting whichever parts the server does not know.
// Empty when nothing is known; the view substitutes the localized
// provider_location_unknown placeholder.
std::string PlaceLabel(const ProviderLocationRow& row);

// "37.7749, -122.4194" at 4 decimal places, or an em dash when the provider has
// no coordinates.
std::string CoordinatesLabel(const ProviderLocationRow& row);

// The connected duration split for display. `valid` is false when the SDK has
// no connected-since stamp (fixed/peer destinations from an older device), in
// which case the view shows nothing rather than "0s".
struct ConnectedDuration {
  bool valid = false;
  int64_t hours = 0;
  int64_t minutes = 0;
  int64_t seconds = 0;
};
ConnectedDuration SplitConnectedDuration(int64_t connectedSinceMillis, int64_t nowMillis);

// The globe's wheel order: plottable rows sorted west to east by longitude,
// independent of the list's duration order.
std::vector<ProviderLocationRow> WheelOrder(const std::vector<ProviderLocationRow>& rows);

}  // namespace urnw
