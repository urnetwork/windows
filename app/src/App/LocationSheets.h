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

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

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

// ---- the Network destination (R4) -----------------------------------------
//
// The chooser above, as a PAGE, in the pane idiom the owner approved on Home.
// It lives in this unit rather than in one of its own because it is the same
// model rendered twice: LocationChooserSheet and NetworkPage read the identical
// two SDK feeds, apply the identical bucket order (peers, best available / top
// matches, countries, regions, cities, devices), and share PeerDisplayName and
// the id-comparison predicates in this file's anonymous namespace. Splitting
// them would have duplicated all of that and let the two drift.
//
// The sheet is NOT retired. Home's location row still opens it - a modal picker
// is the right thing when you are mid-connect on another screen - and the two
// subscribe independently (SdkHost::SetLocationsObserver exists for exactly
// this; see the note there).
//
// WHAT IT DOES NOT DO. The spec asks the detail pane for latency, load and
// capability. urnet::ConnectLocation carries name, provider_count,
// location_type, city/region/country(+code), stable, strong_privacy, promoted
// and match_distance - and no latency and no load, anywhere in the SDK surface.
// So the detail pane renders the eight fields that exist and no columns for the
// two that do not.
class NetworkPage {
 public:
  explicit NetworkPage(winrt::URnetwork::implementation::MainWindow& window);

  void ApplyStrings();

  // The two SDK feeds, already marshalled onto the UI thread by the window.
  void OnLocations(std::optional<urnet::FilteredLocations> locations, std::string state);
  void OnPeers(std::optional<urnet::NetworkPeerList> peers);

  // Selecting the destination opens the SDK's locations/peer view controllers
  // (EnsureLocations) and re-renders from whatever snapshot exists now; the
  // pushes take over from there.
  void SetSelected(bool selected);

  // --preview-ui + URNETWORK_PREVIEW_SAMPLE only: synthetic buckets, so the
  // pane can be reviewed with rows in it. A session-less process has no
  // locations at all, and an empty pane proves nothing about a layout whose
  // whole claim is density.
  void ApplyPreviewSample();

 private:
  void Build();     // one-time: the search row
  void Render();    // the list pane
  void RenderDetail();

  // One row species for the whole pane: a fixed-height UrPaneRowButtonStyle
  // button, a colour dot, a trimmed title, the trailing state glyphs, and a
  // right-aligned figure. Peers, best-available and locations are all this.
  winrt::Microsoft::UI::Xaml::Controls::Button MakeRow(
      winrt::hstring const& title, winrt::hstring const& meta,
      winrt::Windows::UI::Color dotColor, bool selected, bool unstable, bool strongPrivacy,
      bool providing);
  void AppendGroup(winrt::hstring const& title, int64_t count);
  void AppendLocationSection(winrt::hstring const& title,
                             std::optional<urnet::ConnectLocationList> const& items,
                             std::optional<urnet::ConnectLocation> const& selected,
                             int64_t& runningTotal);

  winrt::URnetwork::implementation::MainWindow& w_;

  winrt::Microsoft::UI::Xaml::Controls::TextBox search_{nullptr};
  bool built_ = false;
  bool selected_ = false;
  // Set by ApplyPreviewSample. A real (empty) push from a session-less process
  // must not wipe the synthetic buckets back off the screen - the same pin
  // ConnectPage and the status strip carry, and for the same reason.
  bool samplePinned_ = false;

  std::optional<urnet::FilteredLocations> locations_;
  std::optional<urnet::NetworkPeerList> peers_;
  std::string query_;
};

}  // namespace urnw
