// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "UsageBar.h"

#include <algorithm>

#include "Localization.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;

namespace urnw {
namespace {

// wingdi.h declares ::Ellipse; alias the XAML shape for unqualified use
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;

// bar geometry (macOS UsageBar: 32pt bar, 12pt outer corner radius)
constexpr double kBarRadius = 12.0;
// every segment keeps at least 1.5% of the bar so it shows up (macOS
// minNonZeroValue)
constexpr double kMinFraction = 0.015;

// legend entry: a colored dot + a muted label
StackPanel MakeLegendEntry(winrt::Windows::UI::Color color, hstring const& label) {
  StackPanel entry;
  entry.Orientation(Orientation::Horizontal);
  entry.Spacing(6);
  ShapeEllipse dot;
  dot.Width(8);
  dot.Height(8);
  dot.Fill(colors::MakeBrush(color));
  dot.VerticalAlignment(VerticalAlignment::Center);
  entry.Children().Append(dot);
  TextBlock text;
  text.Text(label);
  text.FontSize(12);
  text.Foreground(colors::MutedBrush());
  text.VerticalAlignment(VerticalAlignment::Center);
  entry.Children().Append(text);
  return entry;
}

}  // namespace

UsageBar::UsageBar(Grid barHost, StackPanel legendHost) : barHost_(barHost) {
  if (legendHost) {
    legendHost.Children().Clear();
    legendHost.Children().Append(
        MakeLegendEntry(colors::kUrElectricBlue, hstring{Localized("used_data_key")}));
    legendHost.Children().Append(
        MakeLegendEntry(colors::kUrCoral, hstring{Localized("pending_data_key")}));
    legendHost.Children().Append(
        MakeLegendEntry(colors::kTextFaint, hstring{Localized("available_data_key")}));
  }
  Update(0, 0, 0);
}

void UsageBar::Update(int64_t usedByteCount, int64_t pendingByteCount,
                      int64_t availableByteCount) {
  if (!barHost_) return;
  barHost_.Children().Clear();
  barHost_.ColumnDefinitions().Clear();

  struct Segment {
    double weight;
    winrt::Windows::UI::Color color;
  };
  const double total = static_cast<double>(std::max<int64_t>(usedByteCount, 0)) +
                       static_cast<double>(std::max<int64_t>(pendingByteCount, 0)) +
                       static_cast<double>(std::max<int64_t>(availableByteCount, 0));

  Segment segments[3] = {
      {0, colors::kUrElectricBlue},  // used
      {0, colors::kUrCoral},         // pending
      {0, colors::kTextFaint},       // available
  };
  if (total <= 0) {
    // nothing to show yet: one faint full-width track
    segments[2].weight = 1.0;
  } else {
    const int64_t values[3] = {std::max<int64_t>(usedByteCount, 0),
                               std::max<int64_t>(pendingByteCount, 0),
                               std::max<int64_t>(availableByteCount, 0)};
    for (int i = 0; i < 3; ++i) {
      // floor every segment at 1.5% so a tiny slice is still visible
      segments[i].weight = std::max(values[i] / total, kMinFraction);
    }
  }

  // the visible segments, in used → pending → available order
  int visibleCount = 0;
  for (auto const& segment : segments) {
    if (segment.weight > 0) ++visibleCount;
  }
  int column = 0;
  int rendered = 0;
  for (auto const& segment : segments) {
    if (segment.weight <= 0) continue;
    ColumnDefinition col;
    col.Width(GridLength{segment.weight, GridUnitType::Star});
    barHost_.ColumnDefinitions().Append(col);

    Border piece;
    piece.Background(colors::MakeBrush(segment.color));
    const bool first = rendered == 0;
    const bool last = rendered == visibleCount - 1;
    piece.CornerRadius(CornerRadius{first ? kBarRadius : 0.0, last ? kBarRadius : 0.0,
                                    last ? kBarRadius : 0.0, first ? kBarRadius : 0.0});
    // a hairline gap between segments so the colors don't bleed together
    piece.Margin(Thickness{first ? 0.0 : 1.0, 0, 0, 0});
    Grid::SetColumn(piece, column);
    barHost_.Children().Append(piece);
    ++column;
    ++rendered;
  }
}

}  // namespace urnw
