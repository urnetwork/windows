// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ProviderLocationsSheet.h"

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include <algorithm>
#include <chrono>

#include "Localization.h"
#include "Sdk.h"
#include "Strings.h"  // Widen: the utf-8 sdk ids into the wide localized text
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

// The globe box. Fit-center scales the globe to the smaller dimension and
// centers it in both, so this height is what the globe actually occupies and
// nothing spills into the rows below. Sized with the list cap so the whole
// dialog stays inside the app's established ~520 content-height budget
// (MakeSheetScroll in StatsSheets.cpp) -- a ContentDialog does not scroll its
// own content, so overshooting it clips instead of scrolling.
constexpr double kGlobeHeight = 220;
constexpr double kListMaxHeight = 260;

// Row dot geometry, mirroring the globe's: the ring is an outline
// kDotRingGap outside the solid dot's edge, and the box is sized for the ring
// so the column width never changes with selection. The dot diameter matches
// the location-chooser rows (MakeDot(..., 12)) so a country reads the same size
// everywhere.
constexpr double kDotDiameter = 12;
constexpr double kDotRingGap = 4;
constexpr double kDotRingStroke = 1.5;
constexpr double kDotBox = kDotDiameter + (kDotRingGap + kDotRingStroke) * 2;

hstring H(std::string const& s) { return winrt::to_hstring(s); }

// A UI string from the shared localization store, by key id (Localization.h).
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }

TextBlock MakeText(hstring const& text, double fontSize, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock tb;
  tb.Text(text);
  tb.FontSize(fontSize);
  if (brush) tb.Foreground(brush);
  if (wrap) tb.TextWrapping(TextWrapping::Wrap);
  return tb;
}

Button MakeSubtleButton(hstring const& text) {
  Button button;
  button.Content(winrt::box_value(text));
  button.Background(SolidColorBrush(kTransparent));
  button.BorderThickness(Thickness{0, 0, 0, 0});
  return button;
}

ContentDialog MakeDialog(XamlRoot const& root, hstring const& title) {
  ContentDialog dialog;
  dialog.XamlRoot(root);
  dialog.Title(winrt::box_value(title));
  dialog.CloseButtonText(Loc("close"));
  // brand sheet surface (macOS sheet background)
  dialog.Background(colors::BackgroundBrush());
  return dialog;
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

// The row dot color: the provider's country color (the same palette the
// location chooser uses), falling back to the web globe's neutral blue when the
// country is unknown -- so an unlocated provider still reads as a provider.
winrt::Windows::UI::Color RowColor(const ProviderLocationRow& row) {
  if (row.countryCode.empty()) return winrt::Windows::UI::Color{255, 0x00, 0x99, 0xFF};
  return ColorFromHex(urnet::getColorHex(row.countryCode));
}

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// "2h 5m" / "3m" / "45s", or empty when the SDK has no connected-since stamp.
hstring DurationText(int64_t connectedSinceMillis, int64_t nowMillis) {
  const ConnectedDuration duration = SplitConnectedDuration(connectedSinceMillis, nowMillis);
  if (!duration.valid) return hstring{L""};
  if (0 < duration.hours) {
    return hstring{Format("provider_connected_duration_hours", duration.hours, duration.minutes)};
  }
  if (0 < duration.minutes) {
    return hstring{Format("provider_connected_duration_minutes", duration.minutes)};
  }
  return hstring{Format("provider_connected_duration_seconds", duration.seconds)};
}

}  // namespace

std::shared_ptr<ProviderLocationsSheet> ProviderLocationsSheet::Create(XamlRoot const& root,
                                                                       SdkHost& sdk) {
  auto sheet = std::shared_ptr<ProviderLocationsSheet>(new ProviderLocationsSheet(sdk));
  sheet->Build(root);
  return sheet;
}

void ProviderLocationsSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, Loc("provider_locations_title"));
  std::weak_ptr<ProviderLocationsSheet> weak = weak_from_this();

  StackPanel content;
  content.Spacing(12);
  content.MinWidth(440);

  // fixed globe above the scrolling list (android parity: only the list scrolls)
  Grid globeHost;
  globeHost.Height(kGlobeHeight);
  globeHost.HorizontalAlignment(HorizontalAlignment::Stretch);
  content.Children().Append(globeHost);
  globe_ = std::make_unique<ProviderGlobe>(globeHost);
  globe_->SetOnSelect([weak](std::string clientId) {
    if (auto self = weak.lock()) self->Select(std::move(clientId));
  });
  globe_->SetOnStep([weak](int steps) {
    if (auto self = weak.lock()) self->sdk_.StepProviderSelection(steps);
  });

  status_ = MakeText(L"", 12, colors::MutedBrush(), true);
  status_.Visibility(Visibility::Collapsed);
  content.Children().Append(status_);

  list_ = StackPanel();
  list_.Spacing(4);
  ScrollViewer scroll;
  scroll.Content(list_);
  scroll.MaxHeight(kListMaxHeight);
  content.Children().Append(scroll);

  copiedNote_ = MakeText(L"", 11, colors::MutedBrush());
  copiedNote_.Visibility(Visibility::Collapsed);
  content.Children().Append(copiedNote_);

  dialog_.Content(content);
  Render();
}

void ProviderLocationsSheet::Update(std::vector<ProviderLocationRow> rows, bool remoteConnected) {
  rows_ = std::move(rows);
  remoteConnected_ = remoteConnected;

  // The optimistic trim lasts exactly until the next push (android parity): by
  // then the SDK has re-derived the window, so a removed provider is genuinely
  // gone and a removal that did not take (rpc down) puts its row back rather
  // than leaving a provider hidden that is still carrying traffic.
  removing_.clear();
  // the SDK view controller owns the selection and drops one whose provider
  // left the window, so this mirrors it rather than deciding it
  selectedClientId_ = sdk_.SelectedProviderClientId();
  Render();
}

void ProviderLocationsSheet::RefreshSelection() {
  const std::string selected = sdk_.SelectedProviderClientId();
  if (selected == selectedClientId_) return;
  selectedClientId_ = selected;
  Render();
}

void ProviderLocationsSheet::UpdateIdentities(std::vector<ProviderIdentityRow> identities) {
  // value-compare: the identity feed is signal-only and re-fires on churn
  if (SameProviderIdentityRows(identities, identities_)) return;
  identities_ = std::move(identities);
  identityByClientId_.clear();
  for (const ProviderIdentityRow& identity : identities_) {
    identityByClientId_[identity.clientId] = &identity;
  }
  Render();
}

void ProviderLocationsSheet::Render() {
  list_.Children().Clear();
  durationLabels_.clear();

  // the optimistic trim: a row being removed leaves the list immediately
  std::vector<ProviderLocationRow> visible;
  visible.reserve(rows_.size());
  for (const auto& row : rows_) {
    if (removing_.count(row.clientId) == 0) visible.push_back(row);
  }

  FrameworkElement selectedRow{nullptr};
  for (const auto& row : visible) {
    Grid rowGrid = MakeProviderRow(row);
    if (row.clientId == selectedClientId_) selectedRow = rowGrid;
    list_.Children().Append(rowGrid);
  }

  // Keep the selection on screen. It moves without the list being touched -- a
  // wheel step on the globe, the default landing on the longest connected
  // provider, a removal handing it to an older one -- and a selection the user
  // cannot see is not a selection. StartBringIntoView scrolls the minimum
  // needed and does nothing for a row that is already visible.
  //
  // Deferred: the rows were appended this tick and have no layout yet, so a
  // bring-into-view now would have nothing to measure.
  if (selectedRow && selectedClientId_ != scrolledToClientId_) {
    scrolledToClientId_ = selectedClientId_;
    if (auto queue = list_.DispatcherQueue()) {
      queue.TryEnqueue([selectedRow] { selectedRow.StartBringIntoView(); });
    }
  }

  // The window state lives in the service's device: while the rpc is down an
  // empty list is unavailable, not "none" -- say so rather than rendering a
  // stale zero as fact (drawer peers-line parity).
  if (!remoteConnected_) {
    status_.Text(Loc("provider_locations_unavailable"));
    status_.Visibility(Visibility::Visible);
  } else if (visible.empty()) {
    status_.Text(Loc("provider_locations_empty"));
    status_.Visibility(Visibility::Visible);
  } else {
    status_.Visibility(Visibility::Collapsed);
  }

  if (globe_) globe_->SetProviders(visible, selectedClientId_);
}

Grid ProviderLocationsSheet::MakeProviderRow(const ProviderLocationRow& row) {
  std::weak_ptr<ProviderLocationsSheet> weak = weak_from_this();
  const bool selected = row.clientId == selectedClientId_;
  const winrt::Windows::UI::Color color = RowColor(row);

  Grid grid;
  ColumnDefinition dotColumn, textColumn, actionColumn;
  dotColumn.Width(GridLength{0, GridUnitType::Auto});
  textColumn.Width(GridLength{1, GridUnitType::Star});
  actionColumn.Width(GridLength{0, GridUnitType::Auto});
  grid.ColumnDefinitions().Append(dotColumn);
  grid.ColumnDefinitions().Append(textColumn);
  grid.ColumnDefinitions().Append(actionColumn);
  grid.ColumnSpacing(12);
  grid.Padding(Thickness{0, 6, 0, 6});
  grid.Background(SolidColorBrush(kTransparent));  // hit-testable for Tapped

  // the dot column: a fixed-size box so the row never shifts when the selection
  // ring appears, top aligned beside the four stacked labels
  Grid dotBox;
  dotBox.Width(kDotBox);
  dotBox.Height(kDotBox);
  dotBox.VerticalAlignment(VerticalAlignment::Top);
  ShapeEllipse dot;
  dot.Width(kDotDiameter);
  dot.Height(kDotDiameter);
  dot.Fill(SolidColorBrush(color));
  dot.HorizontalAlignment(HorizontalAlignment::Center);
  dot.VerticalAlignment(VerticalAlignment::Center);
  dotBox.Children().Append(dot);
  if (selected) {
    ShapeEllipse ring;
    // radii are stroke centerlines, so the outline sits kDotRingGap outside the
    // dot's edge
    const double ringDiameter = kDotDiameter + (kDotRingGap + kDotRingStroke / 2) * 2;
    ring.Width(ringDiameter);
    ring.Height(ringDiameter);
    ring.Stroke(SolidColorBrush(color));
    ring.StrokeThickness(kDotRingStroke);
    ring.HorizontalAlignment(HorizontalAlignment::Center);
    ring.VerticalAlignment(VerticalAlignment::Center);
    dotBox.Children().Append(ring);
  }
  Grid::SetColumn(dotBox, 0);
  grid.Children().Append(dotBox);

  // four labels, left aligned flush to the dot column
  StackPanel text;
  text.Spacing(2);
  text.HorizontalAlignment(HorizontalAlignment::Stretch);

  // the client id, click to copy (ClientContractsSheet precedent)
  TextBlock idText = MakeText(H(row.clientId), 11,
                              selected ? colors::TextBrush() : colors::FaintBrush());
  idText.FontFamily(FontFamily(L"Consolas"));
  idText.TextWrapping(TextWrapping::NoWrap);
  idText.TextTrimming(TextTrimming::CharacterEllipsis);
  idText.HorizontalAlignment(HorizontalAlignment::Left);
  {
    const std::string copyId = row.clientId;
    idText.Tapped([weak, copyId](IInspectable const&, Input::TappedRoutedEventArgs const& args) {
      // the id owns the tap; the row's select handler must not also fire
      args.Handled(true);
      if (auto self = weak.lock()) self->CopyClientId(copyId);
    });
  }
  // the id, plus the provider's identity identicon as a trailing badge when a
  // verified e2e session exists. A 2-column grid (id Star, badge Auto) so a
  // long id ellipsizes instead of pushing the badge off; the badge is created
  // only on a join hit, so absence is the "not e2e" state (no placeholder).
  if (auto it = identityByClientId_.find(row.clientId); it != identityByClientId_.end()) {
    Grid idRow;
    ColumnDefinition idCol, badgeCol;
    idCol.Width(GridLength{1, GridUnitType::Star});
    badgeCol.Width(GridLength{0, GridUnitType::Auto});
    idRow.ColumnDefinitions().Append(idCol);
    idRow.ColumnDefinitions().Append(badgeCol);
    idRow.ColumnSpacing(6);
    Grid::SetColumn(idText, 0);
    idRow.Children().Append(idText);
    FrameworkElement badge = MakeIdenticonBadge(identiconCache_, *it->second, kBadgeIdenticonSize);
    ToolTipService::SetToolTip(badge, box_value(Loc("post_quantum_encryption")));
    Grid::SetColumn(badge, 1);
    idRow.Children().Append(badge);
    text.Children().Append(idRow);
  } else {
    text.Children().Append(idText);
  }

  const std::string place = PlaceLabel(row);
  TextBlock placeText = MakeText(
      row.hasLocation && !place.empty() ? H(place) : Loc("provider_location_unknown"), 13,
      colors::TextBrush(), true);
  placeText.HorizontalAlignment(HorizontalAlignment::Left);
  text.Children().Append(placeText);

  TextBlock coordinatesText = MakeText(H(CoordinatesLabel(row)), 12, colors::MutedBrush());
  coordinatesText.FontFamily(FontFamily(L"Consolas"));
  coordinatesText.HorizontalAlignment(HorizontalAlignment::Left);
  text.Children().Append(coordinatesText);

  TextBlock durationText = MakeText(DurationText(row.connectedSinceMillis, NowMillis()), 12,
                                    colors::MutedBrush());
  durationText.HorizontalAlignment(HorizontalAlignment::Left);
  text.Children().Append(durationText);
  durationLabels_.emplace_back(row.connectedSinceMillis, durationText);

  Grid::SetColumn(text, 1);
  grid.Children().Append(text);

  // Remove: WinUI desktop has no swipe idiom, so this is the app's inline
  // destructive row action, matching the split-rule and DNS-server rows exactly
  // (subtle button, no danger tint -- the app reserves DangerBrush for the
  // page-level destructive action). Top aligned rather than centered because
  // this row is four lines tall.
  Button remove = MakeSubtleButton(L"✕");
  remove.FontSize(11);
  remove.Padding(Thickness{6, 2, 6, 2});
  remove.VerticalAlignment(VerticalAlignment::Top);
  {
    const std::string removeId = row.clientId;
    remove.Click([weak, removeId](IInspectable const& sender, RoutedEventArgs const&) {
      if (auto button = sender.try_as<Button>()) button.IsEnabled(false);
      if (auto self = weak.lock()) self->Remove(removeId);
    });
  }
  Grid::SetColumn(remove, 2);
  grid.Children().Append(remove);

  {
    const std::string selectId = row.clientId;
    grid.Tapped([weak, selectId](IInspectable const&, Input::TappedRoutedEventArgs const&) {
      if (auto self = weak.lock()) self->Select(selectId);
    });
  }
  return grid;
}

void ProviderLocationsSheet::Select(std::string clientId) {
  if (clientId == selectedClientId_) return;
  // the SDK view controller is the source of truth; it reports back through the
  // selection handler, which lands in RefreshSelection
  sdk_.SetSelectedProviderClientId(clientId);
  selectedClientId_ = std::move(clientId);
  Render();
}

void ProviderLocationsSheet::Remove(const std::string& clientId) {
  // trim locally first so the row does not linger through the SDK round trip;
  // the SDK reports the window change a moment later and confirms it
  removing_.insert(clientId);
  // the view controller moves the selection to the next older provider when
  // the removed one is selected, so this reads the result back rather than
  // clearing it
  sdk_.RemoveConnectedProvider(clientId);
  selectedClientId_ = sdk_.SelectedProviderClientId();
  Render();
}

void ProviderLocationsSheet::CopyClientId(const std::string& clientId) {
  winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
  package.SetText(H(clientId));
  winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
  // label, then the id that was copied (data, not prose)
  copiedNote_.Text(hstring{Localized("client_id_copied") + L": " + Widen(clientId)});
  copiedNote_.Visibility(Visibility::Visible);
}

void ProviderLocationsSheet::RefreshDurations() {
  const int64_t now = NowMillis();
  for (auto& [connectedSinceMillis, label] : durationLabels_) {
    label.Text(DurationText(connectedSinceMillis, now));
  }
}

void ProviderLocationsSheet::Tick() {
  if (globe_) globe_->Tick();
  // the durations tick locally against the absolute connected-since stamps, so
  // a running clock costs one text update a second instead of an SDK event per
  // provider
  if (++tickCount_ % 10 == 0) RefreshDurations();  // ~1s cadence on the 100ms clock
}

}  // namespace urnw
