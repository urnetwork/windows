// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "LocationSheets.h"

#include <winrt/Microsoft.UI.Xaml.Input.h>

#include <string>

#include "Localization.h"
#include "Strings.h"  // Narrow: the utf-16 search box into the sdk's utf-8 filter
#include "UrColors.h"

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
  dialog.Background(colors::BackgroundBrush());
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

}  // namespace urnw
