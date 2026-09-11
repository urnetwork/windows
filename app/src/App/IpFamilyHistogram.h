// The IP-family histogram (connect/IPV6.md D2): the connected providers as
// dots, one per provider, stacked horizontally under Both / v4 / v6 according
// to the address families the platform proved for each. It sits directly under
// the transport distribution bar in the activity pane, so "which carrier moved
// the bytes" is followed by "which exits can carry which family". The dots are
// the same size as the connect widget's live dots and the same green as its
// Added dots, so the two surfaces read as one set of providers; a row wraps
// when it has more dots than fit.
//
// All the grouping is IpFamilyGroups.h (pure, tested off-Windows); this class
// converts the SDK's grid points, keeps a change key so an unchanged push
// costs nothing, and draws. Built into a host Grid like TransportBar and
// TransferChart; no per-frame path — the rows change only when the window
// does. UI thread only.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include "IpFamilyGroups.h"
#include "Sdk.h"

namespace urnw {

// The localized row label for a family token ("both" / "v4" / "v6"): "Both" is
// a word and is routed through the store; v4 / v6 are protocol names and are
// not translated. Shared with the provider-locations rows.
winrt::hstring IpFamilyLabelText(std::string const& token);

class IpFamilyHistogram {
 public:
  // `host` receives the whole component.
  explicit IpFamilyHistogram(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);

  // Replace the grid (from LiveStats::gridPoints, every stats push) and the
  // dot diameter to draw at (ConnectCanvas::PointDiameterFor, so the dots are
  // the hero's live size). Rebuilds only when the Added providers per row or
  // the diameter actually changed.
  void SetGrid(std::vector<urnet::ProviderGridPoint> const& points, double dotDiameter);

  // The current rows, for the page's own bookkeeping and the preview build.
  const IpFamilyGroups& Groups() const { return groups_; }

 private:
  struct Row {
    IpFamilyGroup group = IpFamilyGroup::Both;
    winrt::Microsoft::UI::Xaml::Controls::TextBlock label{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::RichTextBlock flow{nullptr};
  };

  void BuildVisuals(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
  void Rebuild();
  void RebuildRow(Row& row);

  IpFamilyGroups groups_;
  double dotDiameter_ = 0;
  bool built_ = false;

  winrt::Microsoft::UI::Xaml::Controls::StackPanel root_{nullptr};
  std::vector<Row> rows_;
  winrt::Microsoft::UI::Xaml::Media::Brush mutedBrush_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Brush faintBrush_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::SolidColorBrush dotBrush_{nullptr};
};

}  // namespace urnw
