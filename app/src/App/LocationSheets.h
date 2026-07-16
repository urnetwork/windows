// The location/provider chooser, opened from the connect drawer's location row
// (port of the apple ProviderListSheet / android BrowseLocations and a mirror
// of the linux LocationsSheet). A ContentDialog modeled on SplitRulesSheet
// (StatsSheets.h): a search box over the SDK-bucketed location sections, with
// the connected, provide-enabled network peers (PeerViewController) pinned as
// the first section. The SDK's LocationsViewController does all grouping and
// search; this sheet only renders the lists it returns. Tapping any row
// connects to it and hides the dialog.
//
// Section order mirrors mobile: network peers, then "top matches" (while
// searching) or a single "best available" row (idle), then countries, regions,
// cities, devices. Selection is reflected with a trailing check; peers also
// carry a green "providing" glyph.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "SdkHost.h"

namespace urnw {

// A network peer's display name: DeviceName, else DeviceSpec, else the client
// id. Shared with the connect drawer's selected-location label (req4).
std::string PeerDisplayName(const urnet::NetworkPeer& peer);

class LocationChooserSheet : public std::enable_shared_from_this<LocationChooserSheet> {
 public:
  static std::shared_ptr<LocationChooserSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }
  // Rebuild the sections from the latest filtered locations + connected peers.
  // Cheap enough to run on every locations/peers change push.
  void Update(std::optional<urnet::FilteredLocations> locations,
              std::optional<urnet::NetworkPeerList> peers);

 private:
  explicit LocationChooserSheet(SdkHost& sdk) : sdk_(sdk) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void Render();
  void OnSearchChanged();
  void AppendSection(winrt::hstring const& title,
                     std::optional<urnet::ConnectLocationList> const& items,
                     std::optional<urnet::ConnectLocation> const& selected);
  winrt::Microsoft::UI::Xaml::Controls::Grid MakeLocationRow(
      const urnet::ConnectLocation& location, bool selected);
  winrt::Microsoft::UI::Xaml::Controls::Grid MakePeerRow(const urnet::NetworkPeer& peer,
                                                         bool selected);
  winrt::Microsoft::UI::Xaml::Controls::Grid MakeBestAvailableRow(bool selected);

  SdkHost& sdk_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox search_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock status_{nullptr};  // no-results
  winrt::Microsoft::UI::Xaml::Controls::StackPanel sections_{nullptr};

  std::optional<urnet::FilteredLocations> locations_;
  std::optional<urnet::NetworkPeerList> peers_;
  std::string query_;  // current search text (empty = idle)
};

}  // namespace urnw
