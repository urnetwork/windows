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

bool SameId(std::optional<std::string> const& a, std::optional<std::string> const& b) {
  return a && b && !a->empty() && *a == *b;
}

// The selected-location tests (ConnectViewController.getSelectedLocation()):
// best-available when nothing is selected or the id flags it; a peer/location by
// comparing the connect_location_id parts.
bool IsBestAvailableSelected(std::optional<urnet::ConnectLocation> const& selected) {
  return !selected || (selected->connect_location_id &&
                       selected->connect_location_id->best_available.value_or(false));
}

bool IsPeerSelected(std::optional<urnet::ConnectLocation> const& selected,
                    urnet::NetworkPeer const& peer) {
  if (!selected || !selected->connect_location_id) return false;
  return SameId(selected->connect_location_id->client_id, peer.ClientId);
}

bool IsLocationSelected(std::optional<urnet::ConnectLocation> const& selected,
                        urnet::ConnectLocation const& loc) {
  if (!selected || !selected->connect_location_id || !loc.connect_location_id) return false;
  const auto& a = *selected->connect_location_id;
  const auto& b = *loc.connect_location_id;
  return SameId(a.location_id, b.location_id) || SameId(a.client_id, b.client_id) ||
         SameId(a.location_group_id, b.location_group_id);
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
  content.MinWidth(440);

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
  scroll.MaxHeight(460);
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
      self->sdk_.Connect(locationCopy);  // SDK persists the selection internally
      self->dialog_.Hide();              // dismiss on connect (iOS/android parity)
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
    self->sdk_.Connect(location);
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
      self->sdk_.ConnectBestAvailable();
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
  selected_ = selected;
  if (!selected) return;
  // Opening the view controllers is idempotent and seeds both feeds; the
  // chooser sheet calls the same thing.
  Sdk().EnsureLocations();
  if (!samplePinned_) {
    locations_ = Sdk().CurrentFilteredLocations();
    peers_ = Sdk().ConnectedProvidePeers();
  }
  Render();
}

void NetworkPage::OnLocations(std::optional<urnet::FilteredLocations> locations,
                              std::string state) {
  (void)state;
  if (samplePinned_) return;
  locations_ = std::move(locations);
  Render();
}

void NetworkPage::OnPeers(std::optional<urnet::NetworkPeerList> peers) {
  if (samplePinned_) return;
  peers_ = std::move(peers);
  Render();
}

Button NetworkPage::MakeRow(hstring const& title, hstring const& meta,
                            winrt::Windows::UI::Color dotColor, bool selected, bool unstable,
                            bool strongPrivacy, bool providing) {
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

  auto dot = MakeDot(dotColor, 8);
  Automation::AutomationProperties::SetAccessibilityView(
      dot, Automation::Peers::AccessibilityView::Raw);
  grid.Children().Append(dot);

  auto name = MakeName(title);
  name.FontSize(13);
  name.VerticalAlignment(VerticalAlignment::Center);
  Automation::AutomationProperties::SetAccessibilityView(
      name, Automation::Peers::AccessibilityView::Raw);
  Grid::SetColumn(name, 1);
  grid.Children().Append(name);

  StackPanel glyphs;
  glyphs.Orientation(Orientation::Horizontal);
  glyphs.Spacing(6);
  glyphs.VerticalAlignment(VerticalAlignment::Center);
  if (providing) glyphs.Children().Append(MakeGlyph(kProvidingGlyph, colors::kUrGreen));
  if (unstable) glyphs.Children().Append(MakeGlyph(kWarningGlyph, kUnstable));
  if (strongPrivacy) glyphs.Children().Append(MakeGlyph(kPrivacyGlyph, colors::kUrGreen));
  if (selected) glyphs.Children().Append(MakeGlyph(kCheckGlyph, colors::kToggleAccent));
  Automation::AutomationProperties::SetAccessibilityView(
      glyphs, Automation::Peers::AccessibilityView::Raw);
  Grid::SetColumn(glyphs, 2);
  grid.Children().Append(glyphs);

  auto figure = MakeText(meta, 12, MutedBrush());
  figure.VerticalAlignment(VerticalAlignment::Center);
  figure.TextWrapping(TextWrapping::NoWrap);
  Automation::AutomationProperties::SetAccessibilityView(
      figure, Automation::Peers::AccessibilityView::Raw);
  Grid::SetColumn(figure, 3);
  grid.Children().Append(figure);

  // A Button whose Content is a Panel gets NO automatic name. Everything inside
  // is Raw, so this is the row's ONLY accessible node - it has to carry the
  // whole row, including the state the trailing glyphs draw in colour.
  std::wstring announced{title};
  if (!meta.empty()) announced += L", " + std::wstring{meta};
  if (unstable) announced += L", " + std::wstring{Loc("unstable_providers_warning")};
  if (strongPrivacy) announced += L", " + std::wstring{Loc("strong_anonymization")};
  if (providing) announced += L", " + std::wstring{Loc("network_peers")};
  Automation::AutomationProperties::SetName(row, hstring{announced});
  if (selected) {
    Automation::AutomationProperties::SetFullDescription(row, Loc("selected_provider"));
  }

  row.Content(grid);
  return row;
}

void NetworkPage::AppendGroup(hstring const& title, int64_t count) {
  auto header = kit::MakePaneGroupHeader(
      title, count <= 0 ? hstring{} : hstring{std::to_wstring(count)});
  w_.NetworkListHost().Children().Append(header.root);
}

void NetworkPage::AppendLocationSection(hstring const& title,
                                        std::optional<urnet::ConnectLocationList> const& items,
                                        std::optional<urnet::ConnectLocation> const& selected,
                                        int64_t& runningTotal) {
  if (!NonEmpty(items)) return;
  AppendGroup(title, static_cast<int64_t>(items->size()));
  runningTotal += static_cast<int64_t>(items->size());
  for (auto const& location : *items) {
    const int providers = location.provider_count.value_or(0);
    auto row = MakeRow(H(location.name.value_or(std::string())),
                       0 < providers
                           ? hstring{Plural("provider_count", static_cast<int64_t>(providers))}
                           : hstring{},
                       LocationColor(location), IsLocationSelected(selected, location),
                       !location.stable, location.strong_privacy, /*providing=*/false);
    const urnet::ConnectLocation copy = location;
    // Same action as the sheet's row: select AND connect. Deliberately not a
    // new "highlight" concept - one model, one meaning, and the detail pane
    // then genuinely shows the SELECTED provider rather than a hover state.
    row.Click([this, copy](IInspectable const&, auto const&) {
      Sdk().Connect(copy);
      Render();
    });
    w_.NetworkListHost().Children().Append(row);
  }
}

void NetworkPage::Render() {
  if (!w_.NetworkListHost()) return;
  auto host = w_.NetworkListHost();
  host.Children().Clear();

  const auto selected = Sdk().SelectedLocation();
  const bool searching = !query_.empty();
  int64_t total = 0;

  // 1. network peers, pinned first (mobile parity, and the chooser's order)
  const int64_t peerCount = peers_ ? static_cast<int64_t>(peers_->size()) : 0;
  if (0 < peerCount) {
    AppendGroup(Loc("network_peers"), peerCount);
    total += peerCount;
    for (auto const& peer : *peers_) {
      auto row = MakeRow(H(PeerDisplayName(peer)), H(peer.DeviceSpec),
                         ColorFromHex(urnet::getColorHex(peer.ClientId.value_or(std::string()))),
                         IsPeerSelected(selected, peer), /*unstable=*/false,
                         /*strongPrivacy=*/false, /*providing=*/true);
      const urnet::NetworkPeer copy = peer;
      row.Click([this, copy](IInspectable const&, auto const&) {
        urnet::ConnectLocation location;
        urnet::ConnectLocationId id;
        id.client_id = copy.ClientId;
        location.connect_location_id = id;
        location.name = PeerDisplayName(copy);
        Sdk().Connect(location);
        Render();
      });
      host.Children().Append(row);
    }
  }

  // 2. searching -> the SDK's best matches; idle -> the single best-available row
  if (searching) {
    if (locations_) AppendLocationSection(Loc("top_matches"), locations_->BestMatches, selected, total);
  } else {
    AppendGroup(Loc("promoted_locations"), 0);
    auto row = MakeRow(Loc("best_available_provider"), hstring{}, colors::kUrCoral,
                       IsBestAvailableSelected(selected), /*unstable=*/false,
                       /*strongPrivacy=*/false, /*providing=*/false);
    row.Click([this](IInspectable const&, auto const&) {
      Sdk().ConnectBestAvailable();
      Render();
    });
    host.Children().Append(row);
    total += 1;
  }

  // 3. the SDK's own buckets, in the SDK's own order
  if (locations_) {
    AppendLocationSection(Loc("countries"), locations_->Countries, selected, total);
    AppendLocationSection(Loc("regions"), locations_->Regions, selected, total);
    AppendLocationSection(Loc("cities"), locations_->Cities, selected, total);
    AppendLocationSection(Loc("devices"), locations_->Devices, selected, total);
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
      auto warning = kit::MakePaneRow(34);
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
    reset.Click([this](IInspectable const&, auto const&) {
      Sdk().ConnectBestAvailable();
      Render();
    });
    host.Children().Append(reset);
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
