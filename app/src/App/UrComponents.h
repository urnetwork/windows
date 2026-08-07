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
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

namespace urnw::kit {

// ---- the one desktop breakpoint --------------------------------------------
//
// The app has exactly two layouts and this number decides which:
//
//   below  kWideBreakpointDip   the 1:1000:1 centred single column - the
//                               flyout / narrow-window shape
//   at or above                 the horizontal composition: a main pane, a
//                               side pane BESIDE it, and a full-width row
//                               under both
//
// Every destination in MainWindow.xaml instantiates the SAME shape - a capped
// centring column, a two-column pane grid inside it, and a two-state
// VisualStateGroup named Narrow/Wide whose Wide state raises the cap, opens the
// side column and moves the side panel from the row BELOW the main panel to the
// column BESIDE it. Only the cap and the side width differ per destination.
// Read ConnectView and you have read all seven.
//
// 1000 rather than 900: the pane grid sits inside a 24px page margin and a 48px
// nav rail, so a 1000dip window leaves the two panes ~900dip between them,
// which is where a ~340dip rail beside a main column stops being cramped.
//
// The markup carries the SHAPE - named columns, named panes, and the narrow
// reading as the plain reading - and MainWindow::ApplyBreakpoint carries the
// differences. One function at window level, not seven page-level SizeChanged
// handlers: there is exactly one place where this app decides what "wide"
// means.
//
// VisualStateManager + AdaptiveTrigger is the mechanism this should have been
// and it does not work in this shell. Measured, not read in a doc:
// AdaptiveTrigger listens for size changes on Window.Current, which is null in
// a WinUI 3 desktop app, so a trigger with MinWindowWidth="1" and one Margin
// setter changed nothing at 1400px; and with a plain boolean StateTrigger
// flipped from code the trigger went active while the Setters still did
// nothing, because VisualStateGroups attached to a plain layout Grid are never
// processed. GoToState cannot reach them either (it takes a Control and reads
// the groups off that control's TEMPLATE root) and WinUI 3 has no
// GoToElementState. Wrapping every destination in a templated ContentControl
// would work and would move every x:Name in MainWindow.xaml into a template
// namescope - i.e. delete every accessor the seven page units are written
// against. See MainWindow::ApplyBreakpoint.
inline constexpr double kWideBreakpointDip = 1000.0;

// Set a line's text AND its visibility in one call: an empty string collapses
// the element instead of leaving a row of nothing behind.
//
// This exists because of a measured defect, not as a convenience. A StackPanel
// gives every child its Spacing whether or not the child drew anything, so four
// TextBlocks that are empty while disconnected - the state the app OPENS in -
// cost ~120px of blank card on the Connect screen, in the middle of the panel,
// indistinguishable from a broken layout. `.Text(...)` alone can never be
// right for a line whose content is conditional; this is the call that is.
void SetTextOrCollapse(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& line,
                       winrt::hstring const& text);

// The Windows idiom for "there is nothing here yet": a large muted Segoe Fluent
// glyph over one sentence, centred, rather than a bare "-" or an empty panel.
// A dash cannot distinguish "nothing" from "not loaded" from "failed", and an
// empty panel says nothing at all - both shipped in this app.
//
// `glyph` is a Segoe Fluent Icons codepoint (e.g. L""); `text` must come
// from the localization store.
winrt::Microsoft::UI::Xaml::FrameworkElement MakeEmptyState(winrt::hstring const& glyph,
                                                            winrt::hstring const& text);

// The same empty state on its own card surface, for a panel that has no card
// around it already - the leaderboard's rows, the payouts ledger. "Nothing
// here" still has to be a MODULE, or the destination renders as a single grey
// sentence adrift on the page, which is how the leaderboard read at 60% blank.
// Do NOT use this inside a card: a card in a card reads as two edges 16px
// apart. MakeEmptyState is the one for that.
winrt::Microsoft::UI::Xaml::FrameworkElement MakeEmptyStateCard(winrt::hstring const& glyph,
                                                                winrt::hstring const& text);

// A 1px rule in the border token, for row groups that are built in code rather
// than in markup (Settings sections, the payouts ledger, the points breakdown).
// The markup equivalent is UrDividerStyle in App.xaml; keep the two in step.
winrt::Microsoft::UI::Xaml::Controls::Border MakeDivider();

// A section header above a card: Segoe Fluent glyph + the display face at 16
// SemiBold. The glyph is marked Raw so it is not announced beside the label
// that already says the same word.
winrt::Microsoft::UI::Xaml::FrameworkElement MakeSectionHeader(winrt::hstring const& glyph,
                                                               winrt::hstring const& text);

// ---- the persistent status strip -------------------------------------------
//
// One field of the window's bottom status strip: an optional state dot, an
// 11sp caption, and the value beside it. ProtonVPN's IP / country / provider
// line is three of these.
//
// It returns the field AND hands back the two elements that change at runtime,
// because a strip that cannot be updated is a screenshot. `dot` is null unless
// `withDot`; the caller fills it per state.
//
// The shape is deliberately uniform and deliberately cheap to add to. The
// product tiering decision (spec 511c26c) makes this strip the flagship
// Advanced Mode surface: Normal shows connection state, network and provider,
// and Advanced will add egress interface, rpc port, session mode and the raw
// pre-clamp connection status to the SAME row. Four more fields must cost four
// more calls to this function and no layout change, which is why the strip is
// a horizontal panel of these rather than a hand-built Grid of fixed columns.
struct StatusField {
  winrt::Microsoft::UI::Xaml::FrameworkElement root{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse dot{nullptr};  // null unless withDot
  // The field's name. Handed back so the strip can HIDE it at narrow widths:
  // the app's minimum window is 400dip and four captioned fields need ~600, so
  // below the breakpoint the captions go and the values stand alone. They are
  // still the values' accessible names, so nothing is lost to a screen reader.
  winrt::Microsoft::UI::Xaml::Controls::TextBlock caption{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock value{nullptr};
  // What the field IS, kept so SetStatusFieldValue can rebuild the announced
  // name as "Network, sample-network" every time the value changes.
  winrt::hstring name;
};

// `label` may be empty for a field whose value speaks for itself (the state
// field: a coloured dot and the word "Connected" need no caption saying
// "Status"). Pass `accessibleName` in that case, or the field reaches a screen
// reader as a bare value with nothing saying what it is; it defaults to
// `label`.
StatusField MakeStatusField(winrt::hstring const& label, bool withDot = false,
                            winrt::hstring const& accessibleName = {});

// Write a field's value, and its announced name with it.
//
// Text alone is NOT enough and the difference was visible in the UIA tree: the
// caption is marked Raw (it would otherwise be announced as a separate item
// beside the thing it names), so the field's only accessible node is the value,
// and a value whose Name says "Network" tells a screen reader the label and
// hides the fact. This sets the name to "Network, sample-network" - the same
// shape ConnectPage::ApplyLocationRowName already uses for the provider row.
void SetStatusFieldValue(StatusField const& field, winrt::hstring const& value);

// The hairline between two fields of the strip. Vertical, unlike MakeDivider:
// the strip is a row, so its rules run the other way.
winrt::Microsoft::UI::Xaml::Controls::Border MakeStatusSeparator();

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
