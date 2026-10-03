// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "LocationSheets.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include <string>
#include <utility>

#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "SheetFit.h"  // sheetfit: sheets clamp to the window at open time
#include "Strings.h"  // Narrow: the utf-16 search box into the sdk's utf-8 filter
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Shapes;

// NOTE on captures: row handlers capture the owning sheet weakly. The window
// holds the sheet's shared_ptr while the dialog is showing, so lock() always
// succeeds during interaction; a strong capture would cycle and leak the tree.

namespace urnw {
namespace {

// wingdi.h declares ::Ellipse; alias the XAML shape so unqualified lookup under
// the using-directives stays unambiguous
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;

constexpr winrt::Windows::UI::Color kTransparent{0, 0, 0, 0};
// amber "unstable location" glyph (no brand yellow; danger red reads too strong)
constexpr winrt::Windows::UI::Color kUnstable{255, 0xF5, 0xC2, 0x42};

// trailing status glyphs (Segoe Fluent Icons, the FontIcon default font)
constexpr std::wstring_view kCheckGlyph = L"\uE73E";      // CheckMark (selected)
constexpr std::wstring_view kWarningGlyph = L"\uE7BA";    // Warning (unstable)
constexpr std::wstring_view kPrivacyGlyph = L"\uE72E";    // Lock (strong privacy)
constexpr std::wstring_view kProvidingGlyph = L"\uE774";  // Globe (providing peer)

hstring H(std::string const& s) { return winrt::to_hstring(s); }

// A UI string from the shared localization store, by key id (Localization.h).
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }

Brush MutedBrush() { return colors::MutedBrush(); }

TextBlock MakeText(hstring const& text, double fontSize, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock tb;
  tb.Text(text);
  tb.FontSize(fontSize);
  if (brush) tb.Foreground(brush);
  if (wrap) tb.TextWrapping(TextWrapping::Wrap);
  return tb;
}

// A row's primary name: single line, ellipsized when it overflows.
TextBlock MakeName(hstring const& text) {
  TextBlock tb = MakeText(text, 15, colors::TextBrush());
  tb.TextWrapping(TextWrapping::NoWrap);
  tb.TextTrimming(TextTrimming::CharacterEllipsis);
  return tb;
}

TextBlock SectionHeader(hstring const& text) {
  auto tb = MakeText(text, 12, MutedBrush());
  tb.Margin(Thickness{0, 8, 0, 0});
  return tb;
}

// Prose on the pane's rhythm: the row inset and hairline, wrapping allowed
// (the same construction as AccountPage's AddNoteRow, for the detail pane's
// standing notes).
void AddDetailNote(Panel const& host, hstring const& text) {
  Border box;
  box.Padding(ThicknessHelper::FromLengths(12, 8, 12, 8));
  box.BorderBrush(colors::BorderBrush());
  box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  box.Child(MakeText(text, 12, MutedBrush(), /*wrap=*/true));
  host.Children().Append(box);
}

// "AABBCC" / "#AABBCC" / "AARRGGBB" -> Color (fallback muted gray)
winrt::Windows::UI::Color ColorFromHex(std::string hex) {
  if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
  auto parse = [&](size_t offset) {
    return static_cast<uint8_t>(std::stoul(hex.substr(offset, 2), nullptr, 16));
  };
  try {
    if (hex.size() == 6) return {255, parse(0), parse(2), parse(4)};
    if (hex.size() == 8) return {parse(0), parse(2), parse(4), parse(6)};
  } catch (...) {
  }
  return colors::kTextMuted;
}

ShapeEllipse MakeDot(winrt::Windows::UI::Color color, double size) {
  ShapeEllipse dot;
  dot.Width(size);
  dot.Height(size);
  dot.Fill(SolidColorBrush(color));
  dot.VerticalAlignment(VerticalAlignment::Center);
  return dot;
}

FontIcon MakeGlyph(std::wstring_view glyph, winrt::Windows::UI::Color color) {
  FontIcon icon;
  icon.Glyph(hstring{glyph});
  icon.FontSize(14);
  icon.Foreground(SolidColorBrush(color));
  icon.VerticalAlignment(VerticalAlignment::Center);
  return icon;
}

ContentDialog MakeDialog(XamlRoot const& root, hstring const& title) {
  ContentDialog dialog;
  dialog.XamlRoot(root);
  dialog.Title(winrt::box_value(title));
  dialog.CloseButtonText(Loc("close"));
  // brand sheet surface (android SheetBlack: a sheet sits ABOVE the page)
  dialog.Background(colors::SheetBrush());
  return dialog;
}

// A row shell: [dot][name/caption column, stretched][trailing glyphs], full-row
// hit-testable for Tapped (mirrors SplitRulesSheet::RenderRules).
Grid MakeRowGrid() {
  Grid row;
  ColumnDefinition c0, c1, c2;
  c0.Width(GridLength{0, GridUnitType::Auto});
  c1.Width(GridLength{1, GridUnitType::Star});
  c2.Width(GridLength{0, GridUnitType::Auto});
  row.ColumnDefinitions().Append(c0);
  row.ColumnDefinitions().Append(c1);
  row.ColumnDefinitions().Append(c2);
  row.ColumnSpacing(12);
  row.Padding(Thickness{0, 6, 0, 6});
  row.Background(SolidColorBrush(kTransparent));  // hit-testable for Tapped
  return row;
}

// The dot color for a location row: countries key on the country code,
// everything else on its location/client/group id (mobile parity: solid
// colors, no flags).
winrt::Windows::UI::Color LocationColor(const urnet::ConnectLocation& loc) {
  std::string code;
  if (loc.location_type && *loc.location_type == urnet::LocationTypeCountry &&
      loc.country_code && !loc.country_code->empty()) {
    code = *loc.country_code;
  } else if (loc.connect_location_id) {
    const auto& id = *loc.connect_location_id;
    if (id.location_id && !id.location_id->empty()) {
      code = *id.location_id;
    } else if (id.client_id && !id.client_id->empty()) {
      code = *id.client_id;
    } else if (id.location_group_id && !id.location_group_id->empty()) {
      code = *id.location_group_id;
    }
  }
  if (code.empty()) return colors::kTextMuted;
  return ColorFromHex(urnet::getColorHex(code));
}

// The selected-location tests (SameId / IsBestAvailableSelected /
// IsLocationSelected) used to live here; they moved to SdkHost (declared in
// SdkHost.h) when the row-click coalescer became a third consumer of the same
// identity question. Only the peer flavour stays, because only these two
// surfaces render peer rows.
bool IsPeerSelected(std::optional<urnet::ConnectLocation> const& selected,
                    urnet::NetworkPeer const& peer) {
  if (!selected || !selected->connect_location_id) return false;
  return SameId(selected->connect_location_id->client_id, peer.ClientId);
}

bool NonEmpty(std::optional<urnet::ConnectLocationList> const& list) {
  return list && !list->empty();
}

}  // namespace

std::string PeerDisplayName(const urnet::NetworkPeer& peer) {
  if (!peer.DeviceName.empty()) return peer.DeviceName;
  if (!peer.DeviceSpec.empty()) return peer.DeviceSpec;
  return peer.ClientId.value_or(std::string());
}

std::shared_ptr<LocationChooserSheet> LocationChooserSheet::Create(XamlRoot const& root,
                                                                   SdkHost& sdk) {
  auto sheet = std::shared_ptr<LocationChooserSheet>(new LocationChooserSheet(sdk));
  sheet->Build(root);
  return sheet;
}

void LocationChooserSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, Loc("browse_locations"));
  std::weak_ptr<LocationChooserSheet> weak = weak_from_this();

  StackPanel content;
  content.Spacing(12);
  content.MinWidth(sheetfit::Width(root, 440));

  // fixed search box above the scrolling sections (mobile parity). The SDK
  // debounces stale responses and re-emits FilteredLocations -> onLocations_ ->
  // Update, so there is no app-side debounce (linux parity).
  search_ = TextBox();
  search_.PlaceholderText(Loc("search_providers_input_placeholder"));
  search_.TextChanged([weak](IInspectable const&, auto const&) {
    if (auto self = weak.lock()) self->OnSearchChanged();
  });
  content.Children().Append(search_);

  status_ = MakeText(L"", 12, MutedBrush(), true);
  status_.Visibility(Visibility::Collapsed);
  content.Children().Append(status_);

  sections_ = StackPanel();
  sections_.Spacing(12);
  ScrollViewer scroll;
  scroll.Content(sections_);
  scroll.MaxHeight(sheetfit::Height(root, 460));
  content.Children().Append(scroll);

  dialog_.Content(content);
  Render();
}

void LocationChooserSheet::Update(std::optional<urnet::FilteredLocations> locations,
                                  std::optional<urnet::NetworkPeerList> peers) {
  locations_ = std::move(locations);
  peers_ = std::move(peers);
  Render();
}

void LocationChooserSheet::OnSearchChanged() {
  query_ = Narrow(search_.Text());
  sdk_.SetLocationFilter(query_);
}

void LocationChooserSheet::AppendSection(hstring const& title,
                                         std::optional<urnet::ConnectLocationList> const& items,
                                         std::optional<urnet::ConnectLocation> const& selected) {
  if (!NonEmpty(items)) return;
  sections_.Children().Append(SectionHeader(title));
  StackPanel box;
  box.Spacing(4);
  for (const auto& loc : *items) {
    box.Children().Append(MakeLocationRow(loc, IsLocationSelected(selected, loc)));
  }
  sections_.Children().Append(box);
}

void LocationChooserSheet::Render() {
  sections_.Children().Clear();

  const auto selected = sdk_.SelectedLocation();
  const bool searching = !query_.empty();

  // 1. network peers pinned first (self-hides when there are none)
  const int peerCount = peers_ ? static_cast<int>(peers_->size()) : 0;
  if (0 < peerCount) {
    sections_.Children().Append(SectionHeader(Loc("network_peers")));
    StackPanel box;
    box.Spacing(4);
    for (const auto& peer : *peers_) {
      box.Children().Append(MakePeerRow(peer, IsPeerSelected(selected, peer)));
    }
    sections_.Children().Append(box);
  }

  // 2. searching -> best search matches; idle -> the single best-available row
  //    (both apps ignore the SDK Promoted list; the header is just a label)
  if (searching) {
    if (locations_) AppendSection(Loc("top_matches"), locations_->BestMatches, selected);
  } else {
    sections_.Children().Append(SectionHeader(Loc("promoted_locations")));
    StackPanel box;
    box.Spacing(4);
    box.Children().Append(MakeBestAvailableRow(IsBestAvailableSelected(selected)));
    sections_.Children().Append(box);
  }

  // 3. countries / regions / cities / devices (regions+cities non-empty only
  //    while searching)
  if (locations_) {
    AppendSection(Loc("countries"), locations_->Countries, selected);
    AppendSection(Loc("regions"), locations_->Regions, selected);
    AppendSection(Loc("cities"), locations_->Cities, selected);
    AppendSection(Loc("devices"), locations_->Devices, selected);
  }

  // no-results text: only while searching with nothing at all to show (peers are
  // included in the check, unlike the android original)
  const bool anyLocation =
      locations_ && (NonEmpty(locations_->BestMatches) || NonEmpty(locations_->Countries) ||
                     NonEmpty(locations_->Regions) || NonEmpty(locations_->Cities) ||
                     NonEmpty(locations_->Devices));
  if (searching && !anyLocation && peerCount == 0) {
    status_.Text(Loc("no_providers_found"));
    status_.Visibility(Visibility::Visible);
  } else {
    status_.Visibility(Visibility::Collapsed);
  }
}

Grid LocationChooserSheet::MakeLocationRow(const urnet::ConnectLocation& location, bool selected) {
  Grid row = MakeRowGrid();

  auto dot = MakeDot(LocationColor(location), 10);
  Grid::SetColumn(dot, 0);
  row.Children().Append(dot);

  StackPanel text;
  text.Spacing(2);
  text.VerticalAlignment(VerticalAlignment::Center);
  text.Children().Append(MakeName(H(location.name.value_or(std::string()))));
  const int providerCount = location.provider_count.value_or(0);
  if (0 < providerCount) {
    // CLDR plural from the store; never inflect the count here
    text.Children().Append(MakeText(
        hstring{Plural("provider_count", static_cast<int64_t>(providerCount))}, 12, MutedBrush()));
  }
  Grid::SetColumn(text, 1);
  row.Children().Append(text);

  StackPanel trailing;
  trailing.Orientation(Orientation::Horizontal);
  trailing.Spacing(6);
  trailing.VerticalAlignment(VerticalAlignment::Center);
  if (!location.stable) trailing.Children().Append(MakeGlyph(kWarningGlyph, kUnstable));
  if (location.strong_privacy) {
    trailing.Children().Append(MakeGlyph(kPrivacyGlyph, colors::kUrGreen));
  }
  if (selected) trailing.Children().Append(MakeGlyph(kCheckGlyph, colors::kToggleAccent));
  Grid::SetColumn(trailing, 2);
  row.Children().Append(trailing);

  std::weak_ptr<LocationChooserSheet> weak = weak_from_this();
  const urnet::ConnectLocation locationCopy = location;
  row.Tapped([weak, locationCopy](IInspectable const&, auto const&) {
    if (auto self = weak.lock()) {
      // Coalesced (see ConnectFromRow): the SDK still persists the selection
      // internally when the settled intent fires.
      self->sdk_.ConnectFromRow(locationCopy);
      self->dialog_.Hide();  // dismiss on connect (iOS/android parity)
    }
  });
  return row;
}

Grid LocationChooserSheet::MakePeerRow(const urnet::NetworkPeer& peer, bool selected) {
  Grid row = MakeRowGrid();

  auto dot = MakeDot(ColorFromHex(urnet::getColorHex(peer.ClientId.value_or(std::string()))), 10);
  Grid::SetColumn(dot, 0);
  row.Children().Append(dot);

  StackPanel text;
  text.Spacing(2);
  text.VerticalAlignment(VerticalAlignment::Center);
  text.Children().Append(MakeName(H(PeerDisplayName(peer))));
  // secondary line = the device spec, but only when a distinct name is shown too
  if (!peer.DeviceName.empty() && !peer.DeviceSpec.empty()) {
    text.Children().Append(MakeText(H(peer.DeviceSpec), 12, MutedBrush()));
  }
  Grid::SetColumn(text, 1);
  row.Children().Append(text);

  StackPanel trailing;
  trailing.Orientation(Orientation::Horizontal);
  trailing.Spacing(6);
  trailing.VerticalAlignment(VerticalAlignment::Center);
  // the green "providing to network" glyph, always present on a peer row
  trailing.Children().Append(MakeGlyph(kProvidingGlyph, colors::kUrGreen));
  // FIX vs android (which omits the peer selection check)
  if (selected) trailing.Children().Append(MakeGlyph(kCheckGlyph, colors::kToggleAccent));
  Grid::SetColumn(trailing, 2);
  row.Children().Append(trailing);

  std::weak_ptr<LocationChooserSheet> weak = weak_from_this();
  const urnet::NetworkPeer peerCopy = peer;
  row.Tapped([weak, peerCopy](IInspectable const&, auto const&) {
    auto self = weak.lock();
    if (!self) return;
    urnet::ConnectLocation location;
    urnet::ConnectLocationId id;
    id.client_id = peerCopy.ClientId;
    location.connect_location_id = id;
    location.name = PeerDisplayName(peerCopy);
    self->sdk_.ConnectFromRow(location);  // coalesced, like every row click
    self->dialog_.Hide();
  });
  return row;
}

Grid LocationChooserSheet::MakeBestAvailableRow(bool selected) {
  Grid row = MakeRowGrid();

  auto dot = MakeDot(colors::kUrCoral, 10);  // hardcoded coral (mobile parity)
  Grid::SetColumn(dot, 0);
  row.Children().Append(dot);

  auto name = MakeName(Loc("best_available_provider"));
  name.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(name, 1);
  row.Children().Append(name);

  // FIX vs android: show the selection check when best-available is selected
  if (selected) {
    StackPanel trailing;
    trailing.Orientation(Orientation::Horizontal);
    trailing.VerticalAlignment(VerticalAlignment::Center);
    trailing.Children().Append(MakeGlyph(kCheckGlyph, colors::kToggleAccent));
    Grid::SetColumn(trailing, 2);
    row.Children().Append(trailing);
  }

  std::weak_ptr<LocationChooserSheet> weak = weak_from_this();
  row.Tapped([weak](IInspectable const&, auto const&) {
    if (auto self = weak.lock()) {
      self->sdk_.ConnectBestAvailableFromRow();  // coalesced, like every row click
      self->dialog_.Hide();
    }
  });
  return row;
}

// ============================================================================
// THE NETWORK DESTINATION (R4)
// ============================================================================

namespace {

// The window's SDK host, the same way every other page unit reaches it.
SdkHost& Sdk() { return urnw::pages::Sdk(); }

int64_t CountOf(std::optional<urnet::ConnectLocationList> const& list) {
  return list ? static_cast<int64_t>(list->size()) : 0;
}

// The reconcile keys. Rows key by the STABLE id - the same id precedence
// LocationColor uses for its dot - prefixed by kind so a location id, a client
// id, a group id and a peer can never collide with each other, with the "h:"
// group headers, or with the "best" row and the "empty" line. The SDK's
// buckets are mutually exclusive inside one push (a match-distance-0 hit is
// sorted into BestMatches INSTEAD of its type bucket), so the bare stable id
// is unique across the whole list.
std::string LocationRowKey(const urnet::ConnectLocation& location) {
  if (location.connect_location_id) {
    const auto& id = *location.connect_location_id;
    if (id.location_id && !id.location_id->empty()) return "l:" + *id.location_id;
    if (id.client_id && !id.client_id->empty()) return "c:" + *id.client_id;
    if (id.location_group_id && !id.location_group_id->empty()) {
      return "g:" + *id.location_group_id;
    }
  }
  // No id at all: the name is the only stable thing left to key on.
  return "n:" + location.name.value_or(std::string());
}

std::string PeerRowKey(const urnet::NetworkPeer& peer) {
  if (peer.ClientId && !peer.ClientId->empty()) return "p:" + *peer.ClientId;
  return "p:" + PeerDisplayName(peer);
}

// Windows::UI::Color is a plain struct with no operator==; the skip-unchanged
// test in Render's reconcile needs field equality.
bool SameColor(winrt::Windows::UI::Color a, winrt::Windows::UI::Color b) {
  return a.A == b.A && a.R == b.R && a.G == b.G && a.B == b.B;
}

// The SDK's own state strings (sdk/locations_view_controller.go). Compared by
// value rather than parsed: they are the wire, and an unrecognised one falls
// through to "loading", which is the honest answer for "the feed is open and it
// has not said otherwise".
constexpr std::string_view kLocationsLoading = "LOCATIONS_LOADING";
constexpr std::string_view kLocationsError = "LOCATIONS_ERROR";

// A synthetic location for --preview-ui. Every field is set explicitly so the
// detail pane exercises all of them; nothing here reaches the network.
urnet::ConnectLocation SampleLocation(std::string const& name, std::string const& type,
                                      std::string const& country, std::string const& code,
                                      int32_t providers, bool stable, bool strongPrivacy,
                                      bool promoted, std::string const& region = {},
                                      std::string const& city = {}) {
  urnet::ConnectLocation location;
  urnet::ConnectLocationId id;
  id.location_id = "sample-" + code + "-" + name;
  location.connect_location_id = id;
  location.name = name;
  location.location_type = type;
  location.country = country;
  location.country_code = code;
  if (!region.empty()) location.region = region;
  if (!city.empty()) location.city = city;
  location.provider_count = providers;
  location.stable = stable;
  location.strong_privacy = strongPrivacy;
  location.promoted = promoted;
  return location;
}

}  // namespace

NetworkPage::NetworkPage(winrt::URnetwork::implementation::MainWindow& window) : w_(window) {}

void NetworkPage::ApplyStrings() {
  Build();
  w_.NetworkPaneATitle().Text(Loc("available_providers"));
  w_.NetworkPaneBTitle().Text(Loc("selected_provider"));
  // Landmark names. Without them a screen reader announces two unnamed regions
  // and the user has no way to tell the list from the detail beside it.
  Automation::AutomationProperties::SetName(w_.NetworkPaneA(), Loc("available_providers"));
  Automation::AutomationProperties::SetName(w_.NetworkPaneB(), Loc("selected_provider"));
  if (search_) {
    search_.PlaceholderText(Loc("search_providers_input_placeholder"));
    Automation::AutomationProperties::SetName(search_, Loc("search_providers_input_label"));
  }
  // A (re)stringing re-letters every caption on the standing rows: that is a
  // structural case the reconcile must not patch over, so the next Render
  // rebuilds once (see listDirty_).
  listDirty_ = true;
  Render();
}

void NetworkPage::Build() {
  if (built_) return;
  built_ = true;
  auto row = kit::MakePaneSearchRow(Loc("search_providers_input_placeholder"));
  search_ = row.box;
  search_.TextChanged([this](IInspectable const&, auto const&) {
    query_ = Narrow(search_.Text());
    // The SDK owns the search: it re-buckets and pushes FilteredLocations back
    // through the observer, exactly as it does for the sheet. No app-side
    // filtering, so the page and the sheet can never disagree about a query.
    Sdk().SetLocationFilter(query_);
    // The idle/searching branch in Render is app-side, so re-render now rather
    // than wait for a push that a no-op filter change will not produce.
    Render();
  });
  w_.NetworkSearchHost().Children().Append(row.root);
}

void NetworkPage::SetSelected(bool selected) {
  if (selected_ == selected) return;
  selected_ = selected;
  ReconcileFeeds();
}

void NetworkPage::SetPresentationActive(bool active) {
  if (presentationActive_ == active) return;
  presentationActive_ = active;
  ReconcileFeeds();
}

// Selection and presentation are the two halves, the same pair DeveloperPage's
// poll runs on - and this page only ever had the first one.
//
// SdkHost owns the locations/peers view controllers for exactly as long as the
// presentation, and the presentation stops on minimize and hide-to-tray
// (AppController::WindowPresentationShouldRun is `shown && !minimized`; when
// this defect was found the gate was `shown && activated`, so plain alt-tabbing
// hit it too). Closing the presentation closed both feeds, dropped their
// listeners and pushed std::nullopt into this page - and nothing reopened them,
// because the only opener was a navigation CHANGE and coming back to the
// destination you left on is not one. The pane then stayed empty for the life
// of the window.
//
// EnsureLocations is idempotent, so reconciling on either half is safe and the
// order the two arrive in does not matter.
//
// NEITHER HALF IS A SESSION. Both are properties of this window, and that is the
// point: with no service running at all, EnsureLocations arms the in-process Api
// source instead of the device's view controller, and this page cannot tell the
// difference - same handler, same FilteredLocations, same buckets.
void NetworkPage::ReconcileFeeds() {
  if (selected_ && presentationActive_) {
    Sdk().EnsureLocations();
    if (!samplePinned_) {
      // Deliberately not trusted to be the answer: both sources load
      // asynchronously (the view controller measured at ~1.2s against the
      // shipped dll; the api path is an http round trip), so a snapshot read
      // taken right after arming is EMPTY by construction and the real content
      // arrives on the push. This settles the state - so the pane says
      // "loading" rather than showing the last session's buckets or nothing at
      // all - it does not fetch.
      locations_ = Sdk().CurrentFilteredLocations();
      locationsState_ = Sdk().CurrentFilteredLocationState();
      peers_ = Sdk().ConnectedProvidePeers();
    }
  }
  if (selected_) Render();
}

void NetworkPage::OnLocations(std::optional<urnet::FilteredLocations> locations,
                              std::string state) {
  if (samplePinned_) return;
  locations_ = std::move(locations);
  // The state is the ONLY thing that separates "still loading", "the SDK gave
  // up on this query" and "the answer is genuinely zero providers": all three
  // arrive here as buckets with nothing in them. Dropping it (this used to be
  // `(void)state;`) is what made the three render as one silent screen.
  locationsState_ = std::move(state);
  Render();
}

void NetworkPage::OnPeers(std::optional<urnet::NetworkPeerList> peers) {
  if (samplePinned_) return;
  peers_ = std::move(peers);
  Render();
}

NetworkPage::LocationRowEntry NetworkPage::MakeRow(
    hstring const& title, hstring const& meta, winrt::Windows::UI::Color dotColor, bool selected,
    bool unstable, bool strongPrivacy, bool providing) {
  LocationRowEntry entry;
  Button row;
  if (auto app = Application::Current()) {
    auto key = winrt::box_value(hstring{L"UrPaneRowButtonStyle"});
    if (app.Resources().HasKey(key)) {
      row.Style(app.Resources().Lookup(key).try_as<Style>());
    }
  }
  // ONE height for the whole pane. Home's list rows are 36 and so are these:
  // the two panes are the same construction, so they read as one app.
  row.Height(36);
  row.MinHeight(36);

  Grid grid;
  grid.ColumnSpacing(10);
  for (auto width : {GridLengthHelper::Auto(),
                     GridLengthHelper::FromValueAndType(1, GridUnitType::Star),
                     GridLengthHelper::Auto(), GridLengthHelper::Auto()}) {
    ColumnDefinition column;
    column.Width(width);
    grid.ColumnDefinitions().Append(column);
  }

  entry.dot = MakeDot(dotColor, 8);
  Automation::AutomationProperties::SetAccessibilityView(
      entry.dot, Automation::Peers::AccessibilityView::Raw);
  grid.Children().Append(entry.dot);

  entry.title = MakeName(title);
  entry.title.FontSize(13);
  entry.title.VerticalAlignment(VerticalAlignment::Center);
  Automation::AutomationProperties::SetAccessibilityView(
      entry.title, Automation::Peers::AccessibilityView::Raw);
  Grid::SetColumn(entry.title, 1);
  grid.Children().Append(entry.title);

  entry.glyphs = StackPanel();
  entry.glyphs.Orientation(Orientation::Horizontal);
  entry.glyphs.Spacing(6);
  entry.glyphs.VerticalAlignment(VerticalAlignment::Center);
  Automation::AutomationProperties::SetAccessibilityView(
      entry.glyphs, Automation::Peers::AccessibilityView::Raw);
  Grid::SetColumn(entry.glyphs, 2);
  grid.Children().Append(entry.glyphs);

  entry.meta = MakeText(meta, 12, MutedBrush());
  entry.meta.VerticalAlignment(VerticalAlignment::Center);
  entry.meta.TextWrapping(TextWrapping::NoWrap);
  Automation::AutomationProperties::SetAccessibilityView(
      entry.meta, Automation::Peers::AccessibilityView::Raw);
  Grid::SetColumn(entry.meta, 3);
  grid.Children().Append(entry.meta);

  row.Content(grid);
  entry.root = row;
  entry.button = row;

  // The fill runs the SAME code path a reconcile pass rewrites the row with,
  // so a built row and an updated row are byte-identical.
  LocationListSpec spec;
  spec.title = title;
  spec.meta = meta;
  spec.dotColor = dotColor;
  spec.selected = selected;
  spec.unstable = unstable;
  spec.strongPrivacy = strongPrivacy;
  spec.providing = providing;
  UpdateListEntry(entry, spec);
  return entry;
}

// The in-place rewrite: everything a push can change about a row or a header
// that is already on screen - the caption, the count, the dot's colour, the
// state glyphs, the figure, the accessible name - without touching the
// element's identity, focus, or the scroller's offset. The EmptyLine kind is
// NOT rewritten here: its text lives inside the kit element, so Render's walk
// swaps that single element instead (the swap needs the host).
void NetworkPage::UpdateListEntry(LocationRowEntry& entry, LocationListSpec const& spec) {
  if (entry.kind == LocationRowKind::Group) {
    entry.headerTitle.Text(spec.title);
    entry.headerMeta.Text(spec.meta);
    return;
  }
  if (entry.kind != LocationRowKind::Row) return;
  entry.dot.Fill(SolidColorBrush(spec.dotColor));
  entry.title.Text(spec.title);
  // The state glyphs, in the row's one fixed order, rebuilt inside the
  // standing row: glyph membership is the rewrite's only shape change.
  entry.glyphs.Children().Clear();
  if (spec.providing) entry.glyphs.Children().Append(MakeGlyph(kProvidingGlyph, colors::kUrGreen));
  if (spec.unstable) entry.glyphs.Children().Append(MakeGlyph(kWarningGlyph, kUnstable));
  if (spec.strongPrivacy) {
    entry.glyphs.Children().Append(MakeGlyph(kPrivacyGlyph, colors::kUrGreen));
  }
  if (spec.selected) entry.glyphs.Children().Append(MakeGlyph(kCheckGlyph, colors::kToggleAccent));
  entry.meta.Text(spec.meta);

  // A Button whose Content is a Panel gets NO automatic name. Everything inside
  // is Raw, so this is the row's ONLY accessible node - it has to carry the
  // whole row, including the state the trailing glyphs draw in colour.
  std::wstring announced{spec.title};
  if (!spec.meta.empty()) announced += L", " + std::wstring{spec.meta};
  if (spec.unstable) announced += L", " + std::wstring{Loc("unstable_providers_warning")};
  if (spec.strongPrivacy) announced += L", " + std::wstring{Loc("strong_anonymization")};
  if (spec.providing) announced += L", " + std::wstring{Loc("network_peers")};
  Automation::AutomationProperties::SetName(entry.button, hstring{announced});
  if (spec.selected) {
    Automation::AutomationProperties::SetFullDescription(entry.button, Loc("selected_provider"));
  } else {
    // An update can UNselect a standing row; a fresh row simply never set it.
    entry.button.ClearValue(Automation::AutomationProperties::FullDescriptionProperty());
  }
}

// One element of the list, built ONCE per key (BuildConnectionRow parity). The
// row attaches its click here, by key: the key is captured by value so the
// handler never reaches back into rowEntries_, which the reconcile reseats.
NetworkPage::LocationRowEntry NetworkPage::BuildListEntry(LocationListSpec const& spec) {
  LocationRowEntry entry;
  switch (spec.kind) {
    case LocationRowKind::Group: {
      auto header = kit::MakePaneGroupHeader(spec.title, spec.meta);
      entry.root = header.root;
      entry.headerTitle = header.title;
      entry.headerMeta = header.meta;
      break;
    }
    case LocationRowKind::EmptyLine:
      entry.root = kit::MakePaneEmptyLine(spec.title);
      entry.lineText = spec.title;
      break;
    case LocationRowKind::Row: {
      entry = MakeRow(spec.title, spec.meta, spec.dotColor, spec.selected, spec.unstable,
                      spec.strongPrivacy, spec.providing);
      const std::string key = spec.key;
      entry.button.Click([this, key](IInspectable const&, auto const&) {
        ConnectFromListKey(key);
      });
      break;
    }
  }
  entry.kind = spec.kind;
  entry.key = spec.key;
  entry.applied = spec;
  return entry;
}

// The list row's connect, resolved at click time from the CACHED feeds: a row
// rewritten in place since it was built connects to what it shows NOW, not to
// a copy captured when it was built. Same action as the sheet's row - select
// AND connect; deliberately not a new "highlight" concept, one model, one
// meaning, so the detail pane genuinely shows the SELECTED provider.
// Coalesced (ConnectFromRow): a scroll-and-click hunt through this list fires
// one connect, not one per row visited.
void NetworkPage::ConnectFromListKey(std::string const& key) {
  if (key == "best") {
    Sdk().ConnectBestAvailableFromRow();  // coalesced, like every row click
    Render();
    return;
  }
  if (peers_) {
    for (auto const& peer : *peers_) {
      if (PeerRowKey(peer) != key) continue;
      urnet::ConnectLocation location;
      urnet::ConnectLocationId id;
      id.client_id = peer.ClientId;
      location.connect_location_id = id;
      location.name = PeerDisplayName(peer);
      Sdk().ConnectFromRow(location);  // coalesced, like every row click
      Render();
      return;
    }
  }
  if (locations_) {
    for (auto const* bucket : {&locations_->BestMatches, &locations_->Countries,
                               &locations_->Regions, &locations_->Cities,
                               &locations_->Devices}) {
      if (!*bucket) continue;
      for (auto const& location : **bucket) {
        if (LocationRowKey(location) != key) continue;
        Sdk().ConnectFromRow(location);  // coalesced, like every row click
        Render();
        return;
      }
    }
  }
}

void NetworkPage::AppendGroupSpec(std::vector<LocationListSpec>& specs, std::string const& key,
                                  hstring const& title, int64_t count) {
  LocationListSpec spec;
  spec.kind = LocationRowKind::Group;
  spec.key = key;
  spec.title = title;
  spec.meta = count <= 0 ? hstring{} : hstring{std::to_wstring(count)};
  specs.push_back(std::move(spec));
}

void NetworkPage::AppendLocationSpecs(std::vector<LocationListSpec>& specs,
                                      std::string const& headerKey, hstring const& title,
                                      std::optional<urnet::ConnectLocationList> const& items,
                                      std::optional<urnet::ConnectLocation> const& selected,
                                      int64_t& runningTotal) {
  if (!NonEmpty(items)) return;
  AppendGroupSpec(specs, headerKey, title, static_cast<int64_t>(items->size()));
  runningTotal += static_cast<int64_t>(items->size());
  for (auto const& location : *items) {
    const int providers = location.provider_count.value_or(0);
    LocationListSpec spec;
    spec.key = LocationRowKey(location);
    spec.title = H(location.name.value_or(std::string()));
    spec.meta = 0 < providers
                    ? hstring{Plural("provider_count", static_cast<int64_t>(providers))}
                    : hstring{};
    spec.dotColor = LocationColor(location);
    spec.selected = IsLocationSelected(selected, location);
    spec.unstable = !location.stable;
    spec.strongPrivacy = location.strong_privacy;
    specs.push_back(std::move(spec));
  }
}

// The pane's list of providers. INCREMENTAL, for the same reason
// ConnectPage::ApplyConnectionsList is: the SDK pushes on every location and
// peer change and the search re-buckets on every keystroke, and a
// Clear()+rebuild re-measures the whole (unvirtualized, MinHeight-rowed) tree
// AND resets the scroller to the top - the worst single measure cost in the
// app. Instead each render computes the desired list (LocationListSpec), keys
// it, and diffs it against what is standing (rowEntries_): rows that left are
// removed, rows that moved are reseated, rows that changed are rewritten in
// place, new rows insert at their order position - and the scroll offset is
// read before the mutations and restored after.
void NetworkPage::Render() {
  if (!w_.NetworkListHost()) return;
  auto host = w_.NetworkListHost();
  auto scroll = w_.NetworkListScroll();

  const auto selected = Sdk().SelectedLocation();
  const bool searching = !query_.empty();

  // ---- the desired list, in the pane's own order ----------------------------
  std::vector<LocationListSpec> specs;
  int64_t total = 0;

  // 1. network peers, pinned first (mobile parity, and the chooser's order)
  const int64_t peerCount = peers_ ? static_cast<int64_t>(peers_->size()) : 0;
  if (0 < peerCount) {
    AppendGroupSpec(specs, "h:peers", Loc("network_peers"), peerCount);
    total += peerCount;
    for (auto const& peer : *peers_) {
      LocationListSpec spec;
      spec.key = PeerRowKey(peer);
      spec.title = H(PeerDisplayName(peer));
      spec.meta = H(peer.DeviceSpec);
      spec.dotColor = ColorFromHex(urnet::getColorHex(peer.ClientId.value_or(std::string())));
      spec.selected = IsPeerSelected(selected, peer);
      spec.providing = true;
      specs.push_back(std::move(spec));
    }
  }

  // 2. searching -> the SDK's best matches; idle -> the single best-available row
  if (searching) {
    if (locations_) {
      AppendLocationSpecs(specs, "h:top", Loc("top_matches"), locations_->BestMatches, selected,
                          total);
    }
  } else {
    AppendGroupSpec(specs, "h:promoted", Loc("promoted_locations"), 0);
    LocationListSpec spec;
    spec.key = "best";
    spec.title = Loc("best_available_provider");
    spec.dotColor = colors::kUrCoral;
    spec.selected = IsBestAvailableSelected(selected);
    specs.push_back(std::move(spec));
    total += 1;
  }

  // 3. the SDK's own buckets, in the SDK's own order
  int64_t bucketRows = 0;
  if (locations_) {
    AppendLocationSpecs(specs, "h:countries", Loc("countries"), locations_->Countries, selected,
                        total);
    AppendLocationSpecs(specs, "h:regions", Loc("regions"), locations_->Regions, selected, total);
    AppendLocationSpecs(specs, "h:cities", Loc("cities"), locations_->Cities, selected, total);
    AppendLocationSpecs(specs, "h:devices", Loc("devices"), locations_->Devices, selected, total);
    bucketRows = CountOf(locations_->Countries) + CountOf(locations_->Regions) +
                 CountOf(locations_->Cities) + CountOf(locations_->Devices);
    if (searching) bucketRows += CountOf(locations_->BestMatches);
  }

  // 4. WHY the buckets are empty, when they are.
  //
  // The list is never structurally empty when idle - the synthetic "Best
  // available provider" row is always in it - so the overlay below, which keys
  // off `host.Children().Size() == 0`, could not fire for the case that
  // actually matters. A feed still loading, a failed query and a network with
  // genuinely no providers ALL rendered as that one row and nothing else, with
  // no way for anyone (user or bug report) to tell them apart. This is that
  // missing sentence, inline where the buckets would be.
  const FeedState feed = CurrentFeedState();
  if (bucketRows <= 0 && !samplePinned_) {
    hstring line;
    switch (feed) {
      case FeedState::Loading:
        line = Loc("loading");
        break;
      case FeedState::Failed:
        // No promise of a retry: the SDK does not schedule one. The list
        // reloads when this destination is re-entered, when the window comes
        // back, or on the next search - so say what is true and stop.
        line = pages::Adv("adv_providers_load_failed", L"Could not load the provider list.");
        break;
      case FeedState::Loaded:
        line = searching ? Loc("no_locations_found") : Loc("no_providers_found");
        break;
    }
    LocationListSpec spec;
    spec.kind = LocationRowKind::EmptyLine;
    spec.key = "empty";
    spec.title = line;
    specs.push_back(std::move(spec));
  }

  // ---- reconcile ------------------------------------------------------------
  // A full rebuild is for the structural cases only (listDirty_: the first
  // render, a locale change out of ApplyStrings); the steady-state path never
  // pays for it. A query change is a new result set and reads from the top
  // instead of holding the old list's offset (ConnectPage's resetScroll rule).
  const bool resetScroll = listDirty_ || renderedQuery_ != query_;
  if (listDirty_) {
    host.Children().Clear();
    rowEntries_.clear();
    listDirty_ = false;
  }

  // Read the offset BEFORE the mutations; it is restored after them.
  const double offset = resetScroll ? 0.0 : scroll.VerticalOffset();

  // Rows that left the set - bucketed away, filtered out, or gone from the
  // feed - walk back to front so the Children() indices stay valid as they
  // come out.
  for (size_t i = rowEntries_.size(); 0 < i--;) {
    bool stays = false;
    for (auto const& spec : specs) {
      if (spec.key == rowEntries_[i].key) {
        stays = true;
        break;
      }
    }
    if (stays) continue;
    host.Children().RemoveAt(static_cast<uint32_t>(i));
    rowEntries_.erase(rowEntries_.begin() + static_cast<ptrdiff_t>(i));
  }

  // The desired order: update in place where the element already stands,
  // reseat it if the push moved it, build and insert it if it is new. The
  // entries index IS the Children() index - every mutation mirrors into both.
  for (size_t i = 0; i < specs.size(); ++i) {
    auto const& spec = specs[i];
    size_t at = rowEntries_.size();
    for (size_t k = i; k < rowEntries_.size(); ++k) {
      if (rowEntries_[k].key == spec.key) {
        at = k;
        break;
      }
    }
    if (at < rowEntries_.size()) {
      auto& entry = rowEntries_[at];
      if (entry.kind == LocationRowKind::EmptyLine) {
        if (entry.lineText != spec.title) {
          // The feed state changed under the one keyed empty line: swap the
          // single element in place.
          auto fresh = kit::MakePaneEmptyLine(spec.title);
          host.Children().SetAt(static_cast<uint32_t>(at), fresh);
          entry.root = fresh;
          entry.lineText = spec.title;
          entry.applied = spec;
        }
      } else if (entry.applied.title == spec.title && entry.applied.meta == spec.meta &&
                 SameColor(entry.applied.dotColor, spec.dotColor) &&
                 entry.applied.selected == spec.selected &&
                 entry.applied.unstable == spec.unstable &&
                 entry.applied.strongPrivacy == spec.strongPrivacy &&
                 entry.applied.providing == spec.providing) {
        // Already showing exactly this spec: leave the element untouched, so a
        // push that changes nothing about it costs no layout at all.
      } else {
        UpdateListEntry(entry, spec);
        entry.applied = spec;
      }
      if (at != i) {
        LocationRowEntry moved = std::move(rowEntries_[at]);
        rowEntries_.erase(rowEntries_.begin() + static_cast<ptrdiff_t>(at));
        host.Children().RemoveAt(static_cast<uint32_t>(at));
        host.Children().InsertAt(static_cast<uint32_t>(i), moved.root);
        rowEntries_.insert(rowEntries_.begin() + static_cast<ptrdiff_t>(i), std::move(moved));
      }
    } else {
      LocationRowEntry entry = BuildListEntry(spec);
      host.Children().InsertAt(static_cast<uint32_t>(i), entry.root);
      rowEntries_.insert(rowEntries_.begin() + static_cast<ptrdiff_t>(i), std::move(entry));
    }
  }
  renderedQuery_ = query_;

  // Restore what a rebuild would have lost. ChangeView applies against the new
  // extent once layout settles; the animation is disabled because this is a
  // correction, not a transition.
  if (resetScroll) {
    scroll.ChangeView(nullptr, winrt::Windows::Foundation::IReference<double>{0.0}, nullptr,
                      true);
  } else if (0 < offset) {
    scroll.ChangeView(nullptr, winrt::Windows::Foundation::IReference<double>{offset}, nullptr,
                      true);
  }

  // The empty state is a centred line inside the FULL-HEIGHT list area (the
  // markup overlays this host on the scroller), never a short card at the top.
  auto empty = w_.NetworkListEmptyHost();
  empty.Children().Clear();
  const bool nothing = host.Children().Size() == 0;
  if (nothing) {
    empty.Children().Append(kit::MakePaneEmptyLine(
        searching ? Loc("no_locations_found") : Loc("connecting_status_indicator")));
  }

  w_.NetworkPaneAMeta().Text(total <= 0 ? hstring{} : hstring{std::to_wstring(total)});
  RenderDetail();
}

// Which of the three indistinguishable empty screens this one is.
//
// THE STATE STRING IS CHECKED BEFORE THE SNAPSHOT, and that order is the whole
// trick. A LOCATIONS_LOADING push carries the document `null`, which parses into
// an ENGAGED optional whose six buckets are all nullopt - byte for byte the same
// value as "loaded, and this network has no providers". Nothing below the state
// string can tell those apart, so nothing below it is allowed to try.
//
// Only when the state says nothing useful (empty: no source has answered yet)
// does an absent snapshot mean "loading". It always does at first - both the
// view controller's initial load and the api fetch are async, so any snapshot
// read taken right after arming is empty by construction.
//
// There is no longer a session check here. SdkHost serves this pane from the
// in-process Api whenever there is no device, so "no service session" is not a
// state of the provider list at all: it loads, or it is loading, or it failed.
NetworkPage::FeedState NetworkPage::CurrentFeedState() const {
  if (locationsState_ == kLocationsError) return FeedState::Failed;
  if (locationsState_ == kLocationsLoading) return FeedState::Loading;
  if (!locations_) return FeedState::Loading;
  return FeedState::Loaded;
}

void NetworkPage::RenderDetail() {
  auto host = w_.NetworkDetailHost();
  if (!host) return;
  host.Children().Clear();

  const auto selected = Sdk().SelectedLocation();
  const bool best = IsBestAvailableSelected(selected);

  // ---- what the selected provider IS ---------------------------------------
  host.Children().Append(kit::MakePaneGroupHeader(Loc("selected_location")).root);
  auto value = [&](std::string_view key, hstring const& text) {
    if (text.empty()) return;
    host.Children().Append(kit::MakePaneKeyValueRow(Loc(key), text).root);
  };

  if (best || !selected) {
    host.Children().Append(
        kit::MakePaneKeyValueRow(Loc("name_label"), Loc("best_available_provider")).root);
    w_.NetworkPaneBMeta().Text(Loc("best_available_provider"));

    // What best-available MEANS, and how to override it. The two ids are
    // genuinely missing from the store - Adv() fallback, reported for
    // urnetwork/localizations. Deliberately NO "currently connected: X" line:
    // the SDK does not expose the resolved location under best-available, so
    // printing one would be fabrication.
    AddDetailNote(host, pages::Adv("adv_best_available_note",
                            L"URnetwork picks the fastest healthy providers for you, with no "
                            L"location constraint, and re-picks as the network changes."));
    AddDetailNote(host, pages::Adv("adv_pick_location_note",
                            L"Pick a country, region, city or device in the list to connect "
                            L"there instead."));

    // Quick-pick: the top countries as the SAME rows the list uses - one
    // builder (MakeRow), the LocationColor dot, the provider_count plural meta,
    // and the coalesced connect on click - so picking here behaves
    // byte-identically to clicking the row in the list (AppendLocationSpecs).
    if (locations_ && NonEmpty(locations_->Countries)) {
      const int64_t total = static_cast<int64_t>(locations_->Countries->size());
      const int64_t shown = total < 5 ? total : 5;
      host.Children().Append(
          kit::MakePaneGroupHeader(Loc("countries"), hstring{std::to_wstring(shown)}).root);
      for (int64_t i = 0; i < shown; ++i) {
        auto const& location = (*locations_->Countries)[static_cast<size_t>(i)];
        const int providers = location.provider_count.value_or(0);
        auto row = MakeRow(H(location.name.value_or(std::string())),
                           0 < providers
                               ? hstring{Plural("provider_count",
                                                static_cast<int64_t>(providers))}
                               : hstring{},
                           LocationColor(location), /*selected=*/false, !location.stable,
                           location.strong_privacy, /*providing=*/false);
        const urnet::ConnectLocation copy = location;
        row.button.Click([this, copy](IInspectable const&, auto const&) {
          Sdk().ConnectFromRow(copy);
          Render();
        });
        host.Children().Append(row.root);
      }
    }
  } else {
    const auto& location = *selected;
    const hstring name = H(location.name.value_or(std::string()));
    value("name_label", name);
    w_.NetworkPaneBMeta().Text(name);
    const int providers = location.provider_count.value_or(0);
    if (0 < providers) {
      host.Children().Append(
          kit::MakePaneKeyValueRow(
              Loc("available_providers"),
              hstring{Plural("provider_count", static_cast<int64_t>(providers))})
              .root);
    }
    value("country", H(location.country.value_or(std::string())));
    // "Regions"/"Cities" are the store's bucket headers, not field labels, and
    // there is no singular key for either. Used here rather than inventing
    // "Region"/"City" - reported for the store.
    value("regions", H(location.region.value_or(std::string())));
    value("cities", H(location.city.value_or(std::string())));
    host.Children().Append(
        kit::MakePaneKeyValueRow(Loc("strong_anonymization"),
                                 location.strong_privacy ? Loc("yes") : Loc("no"))
            .root);
    host.Children().Append(
        kit::MakePaneKeyValueRow(Loc("promoted"),
                                 location.promoted.value_or(false) ? Loc("yes") : Loc("no"))
            .root);
    if (!location.stable) {
      // Amber, and a sentence, rather than a "Stable: No" row: the store has no
      // "Stable" label and this is the shipped string for the condition.
      // 36 like the key-value rows around it - the pane row-height rule
      // (36/40/44) has no 34.
      auto warning = kit::MakePaneRow(36);
      auto text = MakeText(Loc("unstable_providers_warning"), 12,
                           SolidColorBrush(kUnstable));
      text.VerticalAlignment(VerticalAlignment::Center);
      warning.Child(text);
      host.Children().Append(warning);
    }
  }

  // Reset to automatic. Only shown when it would change something.
  if (!best) {
    auto reset = MakeRow(Loc("best_available_provider"), hstring{}, colors::kUrCoral,
                         /*selected=*/false, false, false, false);
    reset.button.Click([this](IInspectable const&, auto const&) {
      Sdk().ConnectBestAvailableFromRow();  // coalesced, like every row click
      Render();
    });
    host.Children().Append(reset.root);
  }

  // ---- the buckets behind the list ----------------------------------------
  // Honest counts, not a second copy of the list: this is what the SDK returned
  // for the current query, which is the one thing a detail pane can say about a
  // whole list.
  host.Children().Append(kit::MakePaneGroupHeader(Loc("available_providers")).root);
  auto count = [&](std::string_view key, int64_t n) {
    host.Children().Append(kit::MakePaneKeyValueRow(Loc(key), hstring{std::to_wstring(n)}).root);
  };
  count("network_peers", peers_ ? static_cast<int64_t>(peers_->size()) : 0);
  count("countries", locations_ ? CountOf(locations_->Countries) : 0);
  count("regions", locations_ ? CountOf(locations_->Regions) : 0);
  count("cities", locations_ ? CountOf(locations_->Cities) : 0);
  count("devices", locations_ ? CountOf(locations_->Devices) : 0);

  // ---- blocked locations ---------------------------------------------------
  // The list of blocked countries is a network API read, which this page does
  // not own; the row opens the sheet that does. It stays on Settings too - this
  // is the second door to it, beside the locations it constrains.
  host.Children().Append(kit::MakePaneGroupHeader(Loc("blocked_locations_2")).root);
  auto blocked = kit::MakePaneTwoLineRowButton(Loc("blocked_locations_2"),
                                               Loc("select_country_to_block"));
  blocked.root.Click([this](IInspectable const&, auto const&) {
    w_.ShowBlockedLocationsFromNetwork();
  });
  host.Children().Append(blocked.root);
}

void NetworkPage::ApplyPreviewSample() {
  urnet::FilteredLocations sample;
  urnet::ConnectLocationList countries;
  countries.push_back(SampleLocation("Germany", urnet::LocationTypeCountry, "Germany", "DE",
                                     412, true, true, true));
  countries.push_back(SampleLocation("United States", urnet::LocationTypeCountry,
                                     "United States", "US", 1876, true, false, true));
  countries.push_back(SampleLocation("Japan", urnet::LocationTypeCountry, "Japan", "JP", 233,
                                     true, false, false));
  countries.push_back(SampleLocation("Netherlands", urnet::LocationTypeCountry, "Netherlands",
                                     "NL", 198, true, true, false));
  countries.push_back(SampleLocation("Brazil", urnet::LocationTypeCountry, "Brazil", "BR", 76,
                                     false, false, false));
  countries.push_back(SampleLocation("Singapore", urnet::LocationTypeCountry, "Singapore",
                                     "SG", 141, true, false, false));
  countries.push_back(SampleLocation("United Kingdom", urnet::LocationTypeCountry,
                                     "United Kingdom", "GB", 604, true, false, false));
  countries.push_back(SampleLocation("Canada", urnet::LocationTypeCountry, "Canada", "CA", 287,
                                     true, false, false));
  countries.push_back(SampleLocation("France", urnet::LocationTypeCountry, "France", "FR", 351,
                                     true, false, false));
  countries.push_back(SampleLocation("Sweden", urnet::LocationTypeCountry, "Sweden", "SE", 119,
                                     true, true, false));
  countries.push_back(SampleLocation("Australia", urnet::LocationTypeCountry, "Australia",
                                     "AU", 92, false, false, false));
  countries.push_back(SampleLocation("India", urnet::LocationTypeCountry, "India", "IN", 508,
                                     true, false, false));
  sample.Countries = countries;

  urnet::ConnectLocationList regions;
  for (auto const& entry : {std::pair{"Bavaria", "Germany"}, std::pair{"California", "United States"},
                            std::pair{"Kanto", "Japan"}, std::pair{"Ontario", "Canada"},
                            std::pair{"North Holland", "Netherlands"}}) {
    regions.push_back(SampleLocation(entry.first, urnet::LocationTypeRegion, entry.second, "DE",
                                     48, true, false, false, entry.first));
  }
  sample.Regions = regions;

  urnet::ConnectLocationList cities;
  for (auto const& entry : {std::pair{"Frankfurt", "Germany"}, std::pair{"Berlin", "Germany"},
                            std::pair{"Amsterdam", "Netherlands"}, std::pair{"Tokyo", "Japan"},
                            std::pair{"New York", "United States"},
                            std::pair{"London", "United Kingdom"}, std::pair{"Paris", "France"},
                            std::pair{"Toronto", "Canada"}, std::pair{"Sao Paulo", "Brazil"},
                            std::pair{"Stockholm", "Sweden"}}) {
    cities.push_back(SampleLocation(entry.first, urnet::LocationTypeCity, entry.second, "DE", 27,
                                    true, false, false, {}, entry.first));
  }
  sample.Cities = cities;

  urnet::NetworkPeerList samplePeers;
  for (auto const& entry : {std::pair{"workshop-desktop", "windows"},
                            std::pair{"kitchen-pi", "linux/arm64"},
                            std::pair{"studio-mbp", "darwin/arm64"}}) {
    urnet::NetworkPeer peer;
    peer.ClientId = std::string("peer-") + entry.first;
    peer.DeviceName = entry.first;
    peer.DeviceSpec = entry.second;
    samplePeers.push_back(peer);
  }

  LogWarn(
      "preview-sample: rendering SYNTHETIC network locations - no session, no api, none of "
      "these providers exist");
  locations_ = sample;
  peers_ = samplePeers;
  // Pinned LAST, so the render above lands and every real (empty) push after
  // this point is ignored rather than blanking the pane.
  samplePinned_ = true;
  Render();
}

}  // namespace urnw
