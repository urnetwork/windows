#pragma once

#include <cstdint>

#include <winrt/Windows.UI.h>

#include "UrColors.h"

namespace urnw {

// The provide indicator (apple/android parity), from the LIVE effective provide
// mode: solid dot = Network tier (also Auto while idle), dot + outer ring =
// Public tier (amber while paused — pause stops public only), coral = not
// providing. ProvideMode is a bit set (0 none, 1 network, 2 friends-and-family,
// 3 public) — per-case only. One rule shared by the Connect page's provide
// group and the Earnings page's provide-mode row, so they never disagree.
struct ProvideModeVisual {
  winrt::Windows::UI::Color color;
  bool ring;
};

inline ProvideModeVisual ProvideModeVisualFor(int64_t provideMode, bool paused) {
  switch (provideMode) {
    case 3:  // public
      return {paused ? urnw::colors::kUrAmber : urnw::colors::kUrGreen, true};
    case 1:  // network (also Auto while idle)
    case 2:  // friends-and-family
      return {urnw::colors::kUrGreen, false};
    default:
      return {urnw::colors::kUrCoral, false};
  }
}

}  // namespace urnw
