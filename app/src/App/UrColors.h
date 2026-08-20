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
// app background
inline constexpr winrt::Windows::UI::Color kBackground{255, 0x10, 0x10, 0x10};
// sheet background (android SheetBlack = Black.lighten(0.03)). Sheets must sit
// ABOVE the page, not flush with it: dialogs drawn in kBackground lose their
// edge against the window behind them. Compose's lighten() lerps in Oklab, so
// the 0.03 lift off #101010 lands on #151515, not the naive #171717.
inline constexpr winrt::Windows::UI::Color kSheet{255, 0x15, 0x15, 0x15};
// card background (tintedBackgroundBase)
inline constexpr winrt::Windows::UI::Color kCard{255, 0x1C, 0x1C, 0x1C};
// card hover / pressed (desktop affordance). Spent by the UrCardButton template
// in App.xaml (UrCardHoverBrush / UrCardPressedBrush) rather than by code:
// hover and press are control states, so the platform draws them.
inline constexpr winrt::Windows::UI::Color kCardHover{255, 0x24, 0x24, 0x24};
inline constexpr winrt::Windows::UI::Color kCardPressed{255, 0x2A, 0x2A, 0x2A};
// border / chart axis: white at 12% alpha
inline constexpr winrt::Windows::UI::Color kBorder{0x1F, 0xFF, 0xFF, 0xFF};

// ---- text ----
// Android's onBackground / onPrimary is OFF-white, not pure white (OffWhite in
// theme/Color.kt): body copy and the code-built rows all land here. Pure white
// is reserved for the ABC Gravity headlines, which set it explicitly.
inline constexpr winrt::Windows::UI::Color kOffWhite{255, 0xF8, 0xF8, 0xF8};
inline constexpr winrt::Windows::UI::Color kText = kOffWhite;
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

// ---- entitlement ----
// Pro gold. Android reserves this for the Pro entitlement across the whole
// product -- the profile ring, the network-name button on ur.io, the referral
// panel -- and for NOTHING else, so that gold reads as "this account is Pro"
// rather than as decoration. Do not reuse it for warnings, highlights or chrome;
// kAccent / kUrAmber cover those.
inline constexpr winrt::Windows::UI::Color kProGold{255, 0xFF, 0xC4, 0x00};
inline constexpr winrt::Windows::UI::Color kProGoldLight{255, 0xFF, 0xE0, 0x82};

// ---- connect status ----
// The dot beside the connect status line, from android's
// circle_indicator_{green,yellow,blue} drawables. The connecting yellow is
// Yellow400 (NOT the pale kAccent) and the idle blue is Blue500 (neither
// kToggleAccent/Blue400 nor kUrElectricBlue/Blue600) -- both are ramp steps
// nothing else on windows uses yet.
inline constexpr winrt::Windows::UI::Color kStatusConnecting{255, 0xE6, 0xEA, 0x23};
inline constexpr winrt::Windows::UI::Color kStatusIdle{255, 0x2A, 0x60, 0xFF};

// ---- chart / series semantics ----
// bytes / contract / local / "on"
inline constexpr winrt::Windows::UI::Color kUrGreen{255, 0x87, 0xFB, 0x67};
// packets / companion
inline constexpr winrt::Windows::UI::Color kUrPink{255, 0xED, 0x8F, 0xFF};
// blocked bytes
inline constexpr winrt::Windows::UI::Color kUrCoral{255, 0xFF, 0x6C, 0x58};
// used balance (usage bar); the P2P transport segment
inline constexpr winrt::Windows::UI::Color kUrElectricBlue{255, 0x00, 0x39, 0xDE};
// ---- transport bar (TRANSPORTSTATS) ----
// The two brand tokens the transport distribution bar needs that nothing else on
// windows used yet: urLightBlue (H1) and urYellow (whodis pump), byte-matched to
// the apple/android assets. The other transports reuse tokens above: H3 kUrGreen,
// whodis kUrPink, P2P kUrElectricBlue, queued kTextFaint (dark, so it cannot be
// confused with the pale H1 blue). Coral is deliberately NOT a transport color
// -- the Blocked chart next to the bar is coral.
inline constexpr winrt::Windows::UI::Color kUrLightBlue{255, 0xD6, 0xE6, 0xF4};
inline constexpr winrt::Windows::UI::Color kUrYellow{255, 0xE6, 0xEA, 0x23};
// blocked packets (maroon reads as near-black against the dark background)
inline constexpr winrt::Windows::UI::Color kUrMutedCoral{255, 0xC8, 0x60, 0x4F};
// idle / none status (0 network peers): amber
inline constexpr winrt::Windows::UI::Color kUrAmber{255, 0xF5, 0xC2, 0x42};

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
// every ContentDialog in the app: sheets sit above the page, not flush with it
inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush SheetBrush() {
  return MakeBrush(kSheet);
}
inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush ProGoldBrush() {
  return MakeBrush(kProGold);
}
inline winrt::Microsoft::UI::Xaml::Media::SolidColorBrush AccentBrush() {
  return MakeBrush(kAccent);
}

}  // namespace colors
}  // namespace urnw
