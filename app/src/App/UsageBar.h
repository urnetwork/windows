// The used / pending / available stacked balance bar (macOS UsageBar parity):
// proportional segments — electric blue used, coral pending, faint available —
// with the outer corners rounded and a small color legend underneath. The
// hosts live in XAML (the account plan card and the connect drawer plan card);
// this renders into them. UI thread only.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace urnw {

class UsageBar {
 public:
  // `barHost` receives the stacked bar; `legendHost` the used/pending/available
  // color legend (built once).
  UsageBar(winrt::Microsoft::UI::Xaml::Controls::Grid barHost,
           winrt::Microsoft::UI::Xaml::Controls::StackPanel legendHost);

  void Update(int64_t usedByteCount, int64_t pendingByteCount,
              int64_t availableByteCount);

 private:
  winrt::Microsoft::UI::Xaml::Controls::Grid barHost_{nullptr};
};

}  // namespace urnw
