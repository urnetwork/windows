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
#include <vector>

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

  // The OTHER half of that, and the one this page was missing. SdkHost owns the
  // two feeds only while the window is PRESENTING, and presenting means
  // `shown && activated` - so every deactivation destroyed them and pushed
  // std::nullopt here. Selection alone could not put them back: it fires on a
  // navigation change, and coming back to the destination you left on is not
  // one. Both halves now reopen, exactly as DeveloperPage's poll does.
  void SetPresentationActive(bool active);

  // --preview-ui + URNETWORK_PREVIEW_SAMPLE only: synthetic buckets, so the
  // pane can be reviewed with rows in it. A session-less process has no
  // locations at all, and an empty pane proves nothing about a layout whose
  // whole claim is density.
  void ApplyPreviewSample();

 private:
  // Why an empty provider list is empty. All three of these used to render as
  // the SAME screen - a single "Best available provider" row and nothing else -
  // which is how a torn-down feed passed for a working one for as long as it
  // did. Every one of them now says which it is.
  //
  // There is deliberately no NoSession member any more. It said "Not connected
  // to the URnetwork service, so the provider list is unavailable", which was an
  // honest description of a wrong design rather than of reality: the provider
  // list is public, and SdkHost now serves it from the in-process Api when there
  // is no device. A state that cannot occur must not be representable.
  enum class FeedState {
    Loading,  // a fetch is in flight (view controller or api; both are async)
    Failed,   // LOCATIONS_ERROR - the query failed, from either source
    Loaded,   // answered; what is (or is not) in the buckets is real
  };
  FeedState CurrentFeedState() const;

  void Build();           // one-time: the search row
  void ReconcileFeeds();  // selection AND presentation -> open the feeds, render
  void Render();          // the list pane
  void RenderDetail();

  // One element of the list pane as it stands on screen, keyed so an SDK push
  // or a search keystroke RECONCILES instead of rebuilding
  // (ConnectPage::ConnectionRowEntry parity): group headers key by group id,
  // rows by the stable location/client id, and only the rows the diff touches
  // are inserted, moved, rewritten or removed - the steady-state push pays for
  // no layout but its own changes, and the scroller's offset survives.
  enum class LocationRowKind { Group, Row, EmptyLine };
  // The list's desired state for one Render pass: a group header, a row, or
  // the inline empty line, in exactly the order the pane shows them. The
  // reconcile diffs this against rowEntries_.
  struct LocationListSpec {
    LocationRowKind kind = LocationRowKind::Row;
    std::string key;
    winrt::hstring title;  // group caption, row name, or the empty line's text
    winrt::hstring meta;   // group count or row figure
    winrt::Windows::UI::Color dotColor{0, 0, 0, 0};
    bool selected = false;
    bool unstable = false;
    bool strongPrivacy = false;
    bool providing = false;
  };
  struct LocationRowEntry {
    std::string key;
    LocationRowKind kind = LocationRowKind::Row;
    winrt::Microsoft::UI::Xaml::UIElement root{nullptr};
    // LocationRowKind::Group (kit::MakePaneGroupHeader)
    winrt::Microsoft::UI::Xaml::Controls::TextBlock headerTitle{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock headerMeta{nullptr};
    // LocationRowKind::Row (MakeRow)
    winrt::Microsoft::UI::Xaml::Controls::Button button{nullptr};
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse dot{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock title{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel glyphs{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock meta{nullptr};
    // LocationRowKind::EmptyLine (kit::MakePaneEmptyLine): the text it was
    // built with, so a feed-state change swaps the one line instead of
    // rebuilding the list.
    winrt::hstring lineText;
    // The spec this entry currently shows: a push that changes nothing about
    // an element leaves it untouched, and untouched costs no layout.
    LocationListSpec applied;
  };
  LocationRowEntry BuildListEntry(LocationListSpec const& spec);
  void UpdateListEntry(LocationRowEntry& entry, LocationListSpec const& spec);
  // The row click, by key rather than a captured copy: a row rewritten in
  // place since it was built must still connect to the location it SHOWS.
  void ConnectFromListKey(std::string const& key);

  // One row species for the whole pane: a fixed-height UrPaneRowButtonStyle
  // button, a colour dot, a trimmed title, the trailing state glyphs, and a
  // right-aligned figure. Peers, best-available and locations are all this.
  // Returns the button plus every piece a push can change, so the list
  // reconcile rewrites a standing row in place (UpdateListEntry); the detail
  // pane's callers use .button and ignore the rest.
  LocationRowEntry MakeRow(winrt::hstring const& title, winrt::hstring const& meta,
                           winrt::Windows::UI::Color dotColor, bool selected, bool unstable,
                           bool strongPrivacy, bool providing);
  // The spec emitters: what AppendGroup / AppendLocationSection were, except
  // they now describe the section instead of appending it, so Render can diff
  // before it mutates.
  void AppendGroupSpec(std::vector<LocationListSpec>& specs, std::string const& key,
                       winrt::hstring const& title, int64_t count);
  void AppendLocationSpecs(std::vector<LocationListSpec>& specs, std::string const& headerKey,
                           winrt::hstring const& title,
                           std::optional<urnet::ConnectLocationList> const& items,
                           std::optional<urnet::ConnectLocation> const& selected,
                           int64_t& runningTotal);

  winrt::URnetwork::implementation::MainWindow& w_;

  winrt::Microsoft::UI::Xaml::Controls::TextBox search_{nullptr};
  bool built_ = false;
  bool selected_ = false;
  bool presentationActive_ = false;
  // Set by ApplyPreviewSample. A real (empty) push from a session-less process
  // must not wipe the synthetic buckets back off the screen - the same pin
  // ConnectPage and the status strip carry, and for the same reason.
  bool samplePinned_ = false;

  std::optional<urnet::FilteredLocations> locations_;
  // The SDK's FilterLocationsState for the snapshot above. Kept because it is
  // the ONLY thing separating "still loading" from "the query failed" from
  // "there are genuinely no providers": all three arrive as empty buckets.
  std::string locationsState_;
  std::optional<urnet::NetworkPeerList> peers_;
  std::string query_;

  // The reconcile's memory of what is on screen (see LocationRowEntry). The
  // invariant Render maintains: rowEntries_[i].root == NetworkListHost's i-th
  // child, so an entries index IS a Children() index.
  std::vector<LocationRowEntry> rowEntries_;
  // Structural cases only - the first render, and a locale change out of
  // ApplyStrings - clear and rebuild instead of reconciling. The steady state
  // never sets this, so it never pays the full rebuild's re-measure.
  bool listDirty_ = true;
  // query_ as of the last Render. A change is a new result set, and a new
  // result set reads from the top rather than holding the old list's offset.
  std::string renderedQuery_;
};

}  // namespace urnw
