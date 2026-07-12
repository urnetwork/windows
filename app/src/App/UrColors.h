// URnetwork brand palette (single source for code; App.xaml mirrors these for
// markup). Values match the macOS/iOS theme: dark background system, pale
// yellow accent, and the chart/series semantic colors.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace urnw {
namespace colors {

// ---- surfaces ----
// app / sheet background
inline constexpr winrt::Windows::UI::Color kBackground{255, 0x10, 0x10, 0x10};
// card background (tintedBackgroundBase)
inline constexpr winrt::Windows::UI::Color kCard{255, 0x1C, 0x1C, 0x1C};
// card hover / pressed (desktop affordance)
inline constexpr winrt::Windows::UI::Color kCardHover{255, 0x24, 0x24, 0x24};
inline constexpr winrt::Windows::UI::Color kCardPressed{255, 0x2A, 0x2A, 0x2A};
// border / chart axis: white at 12% alpha
inline constexpr winrt::Windows::UI::Color kBorder{0x1F, 0xFF, 0xFF, 0xFF};

// ---- text ----
inline constexpr winrt::Windows::UI::Color kText{255, 0xFF, 0xFF, 0xFF};
inline constexpr winrt::Windows::UI::Color kTextMuted{255, 0x98, 0x98, 0x98};
inline constexpr winrt::Windows::UI::Color kTextFaint{255, 0x5A, 0x5A, 0x5A};
inline constexpr winrt::Windows::UI::Color kDanger{255, 0xF8, 0x52, 0x3B};
// text on the pale accent / bright chips
inline constexpr winrt::Windows::UI::Color kInverseText{255, 0x10, 0x10, 0x10};

// ---- accents ----
// pale yellow: primary buttons, highlight chips
inline constexpr winrt::Windows::UI::Color kAccent{255, 0xEF, 0xF7, 0xBB};
// toggle/switch on-state blue
inline constexpr winrt::Windows::UI::Color kToggleAccent{255, 0x63, 0x8B, 0xFC};

// ---- chart / series semantics ----
// bytes / contract / local / "on"
inline constexpr winrt::Windows::UI::Color kUrGreen{255, 0x87, 0xFB, 0x67};
// packets / companion
inline constexpr winrt::Windows::UI::Color kUrPink{255, 0xED, 0x8F, 0xFF};
// blocked bytes
inline constexpr winrt::Windows::UI::Color kUrCoral{255, 0xFF, 0x6C, 0x58};
// used balance (usage bar)
inline constexpr winrt::Windows::UI::Color kUrElectricBlue{255, 0x00, 0x39, 0xDE};
// blocked packets (maroon reads as near-black against the dark background)
inline constexpr winrt::Windows::UI::Color kUrMutedCoral{255, 0xC8, 0x60, 0x4F};

inline winrt::Windows::UI::Color WithAlpha(winrt::Windows::UI::Color c, uint8_t a) {
  c.A = a;
  return c;
}

inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush MakeBrush(
    winrt::Windows::UI::Color c) {
  return winrt::Microsoft::UI::Xaml::Media::SolidColorBrush(c);
}

inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush TextBrush() {
  return MakeBrush(kText);
}
inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush MutedBrush() {
  return MakeBrush(kTextMuted);
}
inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush FaintBrush() {
  return MakeBrush(kTextFaint);
}
inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush DangerBrush() {
  return MakeBrush(kDanger);
}
inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush BorderBrush() {
  return MakeBrush(kBorder);
}
inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush CardBrush() {
  return MakeBrush(kCard);
}
inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush BackgroundBrush() {
  return MakeBrush(kBackground);
}
inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush AccentBrush() {
  return MakeBrush(kAccent);
}

}  // namespace colors
}  // namespace urnw
