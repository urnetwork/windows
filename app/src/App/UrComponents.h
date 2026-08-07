// The parts of the component kit a XAML style cannot express.
//
// Most of the kit IS markup — UrButton, UrCard, UrLabel, UrTextField, the
// switch and the snackbar surface are styles in App.xaml, over native WinUI
// controls, wearing the brand tokens. Two pieces carry behaviour instead of
// appearance and live here:
//
//   ValidationState   the iOS UrTextField/ValidationState.swift enum, plus the
//                     one function that renders it onto a field's supporting
//                     line. It replaces per-call Foreground(...) assignments
//                     scattered through the screens.
//   Snackbar          an InfoBar that dismisses itself. InfoBar is the Windows
//                     idiom for the transient bar iOS calls UrSnackBar, but it
//                     has no timer, so a "Wallet connected" bar opened today
//                     stays on screen until the user closes it.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace urnw::kit {

// iOS Components/UrTextField/ValidationState.swift.
enum class ValidationState {
  NotChecked,  // nothing has been asked of the server yet
  Validating,  // a check is in flight
  Valid,
  Invalid,
};

// Renders `text` on a field's supporting line in the colour its validation
// state calls for: danger for Invalid, brand green for Valid, muted for
// NotChecked and Validating (iOS foregroundSupportColor, extended with the
// green the create-network screen already used for "this name is available").
//
// An empty `text` still applies the colour, so a caller can clear the line
// without it flashing the previous verdict's colour on the next write.
void ApplySupportingText(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& line,
                         winrt::hstring const& text, ValidationState state);

// An InfoBar that closes itself after a few seconds — but only when what it is
// saying is safe to miss.
//
// The timer is the whole point of the pattern AND its whole danger. An
// acknowledgement ("Thanks for the feedback!") should go away on its own; an
// ERROR must not, because it is usually the only diagnostic the user will ever
// get — the wallet-connect failure's message is a raw server string that exists
// nowhere else in the product. So the timer is gated on severity: Informational
// and Success time out, Warning and Error stay until dismissed. A call site
// that needs something else passes an explicit duration.
//
// This is not hypothetical. Before the gate, --preview-ui rendered "Failed to
// connect the wallet." on screen at 2.2s and nothing at all at 12s.
//
// Non-copyable and non-movable: it hands its own identity to a timer callback.
// Hold it by value in the owning page, or by unique_ptr.
class Snackbar {
 public:
  // ~4s, the Material/WinUI convention for a message with no action
  static constexpr int kDefaultDurationMs = 4000;
  // "do not dismiss yourself"
  static constexpr int kPersistent = 0;

  Snackbar(winrt::Microsoft::UI::Xaml::Controls::InfoBar bar,
           winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
           int durationMs = kDefaultDurationMs);
  ~Snackbar();

  Snackbar(Snackbar const&) = delete;
  Snackbar& operator=(Snackbar const&) = delete;

  // `durationMs`: omit to let the severity decide (see above), kPersistent to
  // pin the bar open, or any positive value to override the default.
  void Show(winrt::hstring const& message,
            winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity =
                winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Informational,
            std::optional<int> durationMs = std::nullopt);
  void Hide();

 private:
  winrt::Microsoft::UI::Xaml::Controls::InfoBar bar_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
  int defaultDurationMs_ = kDefaultDurationMs;
  // The Tick would otherwise capture a raw `this`, and "the page owns both, so
  // the timer cannot outlive the Snackbar" is an invariant nothing enforces —
  // it was also the one raw-this-across-a-callback this branch introduced. The
  // destructor clears this token, so a tick that somehow outlives its Snackbar
  // finds a null instead of a dangling pointer.
  std::shared_ptr<Snackbar*> self_;
};

}  // namespace urnw::kit
