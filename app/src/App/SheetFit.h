// Sheet fit: a sheet never exceeds the window it sits in.
//
// The window floors at 400x480 dip (WindowShell.h), and a ContentDialog centers
// on its XamlRoot and clips overflow instead of scrolling it, so a MinWidth or
// MaxHeight fixed for a large window clips the sheet -- and its command bar --
// at the floor. Clamp against the root size at sheet-open time: widths to the
// root width minus the dialog's horizontal margin (48), heights to the root
// height minus the dialog title and command bar (96, or more when the sheet
// stacks extra chrome of its own above the clamped element). Open-time
// clamping is sufficient: reopening re-clamps, so sheets carry no live
// SizeChanged re-clamp.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>

#include <winrt/Microsoft.UI.Xaml.h>

namespace urnw::sheetfit {

// min(fixed, root width - reserve). The fixed value stands while the root
// reports no size (not laid out yet), which would otherwise clamp to zero.
inline double Width(winrt::Microsoft::UI::Xaml::XamlRoot const& root, double fixed,
                    double reserve = 48.0) {
  const double w = root.Size().Width;
  return 0.0 < w ? (std::min)(fixed, w - reserve) : fixed;
}

// min(fixed, root height - reserve); same no-size guard as Width.
inline double Height(winrt::Microsoft::UI::Xaml::XamlRoot const& root, double fixed,
                     double reserve = 96.0) {
  const double h = root.Size().Height;
  return 0.0 < h ? (std::min)(fixed, h - reserve) : fixed;
}

}  // namespace urnw::sheetfit
