#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include "ExtenderPresentation.h"
#include "Localization.h"
#include "Strings.h"  // Widen: the SDK's utf-8 reason into the row's text
#include "UrColors.h"

namespace urnw {

// The provide indicator (apple/android parity), from the LIVE effective provide
// mode: solid dot = Network tier (also Auto while idle), dot + outer ring =
// Public tier (amber while paused — pause stops public only), neutral muted =
// not providing. "Never" is a setting the user chose, not an error state, so
// it no longer spends coral (red is reserved for danger). ProvideMode is a bit
// set (0 none, 1 network, 2 friends-and-family, 3 public) — per-case only. One
// rule shared by the Connect page's provide group and the Earnings page's
// provide-mode row, so they never disagree.
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
      return {urnw::colors::kTextMuted, false};
  }
}

// ---- the provider extender rows (connect/EXTENDER.md N7) ---------------------
//
// The Connect page's settings row and the Earnings page's read-only row draw the
// same dot and the same line, so they share one rule here, as the two provide
// indicators share the one above. ExtenderPresentation.h decides; this paints.

// The dot: the provide indicator's drawing without its public ring, and one more
// colour. Grey is the muted text colour, green and coral the provide glyph's
// own, and the yellow is its paused amber, so the extender row and the provide
// indicator never show two different yellows.
inline winrt::Windows::UI::Color ExtenderProvideToneColor(ExtenderProvideTone tone) {
  switch (tone) {
    case ExtenderProvideTone::Green: return urnw::colors::kUrGreen;
    case ExtenderProvideTone::Yellow: return urnw::colors::kUrAmber;
    case ExtenderProvideTone::Red: return urnw::colors::kUrCoral;
    case ExtenderProvideTone::Grey: break;
  }
  return urnw::colors::kTextMuted;
}

// The state line's colour: UrErrorTextBrush (#FF6C58) in the error state, the
// muted brush of UrRowNoteStyle otherwise. The error brush comes from the app
// dictionary so the line follows App.xaml, with its own value standing in if the
// key is ever missing.
inline winrt::Microsoft::UI::Xaml::Media::Brush ExtenderProvideNoteBrush(
    ExtenderProvideTone tone) {
  if (tone != ExtenderProvideTone::Red) return urnw::colors::MutedBrush();
  if (auto app = winrt::Microsoft::UI::Xaml::Application::Current()) {
    const auto key = winrt::box_value(winrt::hstring{L"UrErrorTextBrush"});
    if (app.Resources().HasKey(key)) {
      if (auto brush =
              app.Resources().Lookup(key).try_as<winrt::Microsoft::UI::Xaml::Media::Brush>()) {
        return brush;
      }
    }
  }
  return urnw::colors::MakeBrush(urnw::colors::kUrCoral);
}

// The one line of state text, from the store: the model's keys through
// Localized and Format, the SDK's raw reason widened from UTF-8, and " · "
// between the active line and the other family's failure.
inline std::wstring ExtenderProvideText(ExtenderProvideRowModel const& model) {
  return ComposeExtenderProvideText<std::wstring>(
      model, std::wstring{L" \u00B7 "},
      [](std::string_view key) { return urnw::Localized(key); },
      [](std::string_view key, std::wstring const& argument) {
        return urnw::Format(key, argument);
      },
      [](std::string const& utf8) { return urnw::Widen(utf8); });
}

}  // namespace urnw
