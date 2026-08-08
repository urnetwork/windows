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
#include <vector>

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
// Read WalletView and you have read the six that still work that way.
//
// HOME IS THE EXCEPTION and reads differently on purpose: its panes are
// StackPanel children that get REPARENTED, because the row form put a ~330px
// hole down the middle of the one screen the owner judges (see Reparent in
// MainWindow.xaml.cpp), and it has a second breakpoint below.
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

// ---- the second breakpoint (W1), for Home only ------------------------------
//
// Two columns capped at ~1240 is a sane reading measure and, on the display this
// app is judged on (2560px at 125%, i.e. 2062dip), it reproduced the complaint
// that started the redesign: the page stopped a third of the way across the
// window, with ~280dip of dead gutter on each side and a hole down the middle.
// Widening the cap alone does not fix that - it stretches one hero card and a
// 360 rail across 1800dip, which is not a composition, it is the same page with
// longer buttons.
//
// So above this width Home gets a THIRD column, and the activity (session,
// charts, Custom DNS) moves BESIDE the hero instead of under it. 1800 rather
// than 1600: the third column has to be worth having, and an 1800dip window
// leaves ~1530dip of content after the 220 nav pane and the 24 page margins -
// enough for a main column, an activity column and the 360 rail with a 24
// gutter each, none of them cramped.
//
// Home only, deliberately. The other destinations already spend their width on
// a table or a form; a third column there would be width for its own sake.
inline constexpr double kUltraWideDip = 1800.0;

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

// ============================================================================
// THE WAVE-2 COMPONENT KIT (R1, spec §12 inventory)
//
// Wave 1 owns the shell and Home; Wave 2 rebuilds Network / Earnings / Account /
// Settings. These builders are the shared vocabulary those four agents build
// against, so the four destinations come out of one kit rather than four
// re-inventions. Each realizes a spec §12 component in the brand tokens/faces
// (styles pulled from App.xaml by key, so the kit tracks the ramp), returns the
// live parts a page must update, and carries its own accessibility.
//
// EmptyState (MakeEmptyState / MakeEmptyStateCard), CopyField's cousin
// StatusField and the Snackbar already live above. ConnectionHero is realized by
// ConnectPage's ControlsCard (the ConnectCanvas hero + status-leads layout Wave
// 1 built); it is Home-only and not duplicated here.
// ============================================================================

// PageHeader: a page Title in the brand display face + an optional one-line Body
// description under it. Every Wave-2 destination opens with one.
//   contract: MakePageHeader(title[, description]) -> a top-aligned column.
winrt::Microsoft::UI::Xaml::FrameworkElement MakePageHeader(
    winrt::hstring const& title, winrt::hstring const& description = {});

// MetricCard: a boxed stat tile - a muted caption over a big condensed value -
// for the KPI rows on Earnings and Account. Returns its two live TextBlocks so
// the page updates label/value without rebuilding the tile.
//   contract: MakeMetricCard(label[, value]).{root,label,value}
struct MetricCard {
  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock label{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock value{nullptr};
};
MetricCard MakeMetricCard(winrt::hstring const& label, winrt::hstring const& value = {});

// SettingsCard: one settings row on a card surface - leading 20epx glyph, a
// title with an optional one-line description, and a trailing slot the caller
// drops a control (ToggleSwitch, ComboBox) or a chevron into. The trailing
// control should point AutomationProperties.LabeledBy at `title`.
//   contract: MakeSettingsCard(glyph, title[, description]).{root,title,description,trailing}
struct SettingsCard {
  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock title{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock description{nullptr};
  // append the control/chevron here; single-cell, right-aligned
  winrt::Microsoft::UI::Xaml::Controls::Grid trailing{nullptr};
};
SettingsCard MakeSettingsCard(winrt::hstring const& glyph, winrt::hstring const& title,
                              winrt::hstring const& description = {});

// CopyField: a caption, a value (optionally masked for a secret/client id), and
// a copy button that writes the FULL value to the clipboard with a Raw glyph.
// The masked display never reaches the clipboard - the real value does.
//   contract: MakeCopyField(label, value[, masked]).{root,value,copy}
struct CopyField {
  winrt::Microsoft::UI::Xaml::FrameworkElement root{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock value{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button copy{nullptr};
};
CopyField MakeCopyField(winrt::hstring const& label, winrt::hstring const& value,
                        bool masked = false);

// PlanUsageCard: the plan name + value, a host Grid for a UsageBar, and a legend
// panel - the shape the connect drawer and Account both draw the subscription
// with. Returns the parts a page wires a urnw::UsageBar into (the bar itself is
// not a XAML control, so it stays the caller's to construct into usageBarHost).
//   contract: MakePlanUsageCard(planLabel[, planValue]).{root,planValue,usageBarHost,legend}
struct PlanUsageCard {
  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock planValue{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid usageBarHost{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel legend{nullptr};
};
PlanUsageCard MakePlanUsageCard(winrt::hstring const& planLabel,
                                winrt::hstring const& planValue = {});

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

// ---- the pane shell's dynamic rows (R3) ------------------------------------
//
// The pane layout's static chrome is styles in App.xaml (UrPaneHeaderStyle,
// UrGroupHeaderStyle, UrPaneRowStyle and friends — read the block at the foot of
// that file). These builders are the rows a page generates at RUNTIME: the
// connections table, the contracts list, the split rules, the session figures.
//
// They exist so that "one row height per list" is a property of the code rather
// than a hope. A list that builds its rows by hand acquires a 52px row the day
// someone adds a second line to one of them, and that is exactly the "random
// sized modules" the owner rejected. Every row of a list comes out of one call
// here, so a list cannot drift.

// The row itself: a FIXED height (not a minimum), a bottom hairline, the pane's
// 12px inset. Pass the height once per list.
winrt::Microsoft::UI::Xaml::Controls::Border MakePaneRow(double height);

// key on the left, value hard right, one line each, both trimmed. The session
// figures and any inspector grid are this.
struct PaneKeyValueRow {
  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock key{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock value{nullptr};
};
PaneKeyValueRow MakePaneKeyValueRow(winrt::hstring const& key,
                                    winrt::hstring const& value = {},
                                    double height = 34);

// A list row: a leading state dot, a title that trims, and a right-aligned
// figure. The connections table, the contracts list and the split rules are all
// this one shape on purpose — one row species per pane layout, not three.
struct PaneListRow {
  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse dot{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock title{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock meta{nullptr};
};
PaneListRow MakePaneListRow(double height = 36);

// ---- the pane shell's dynamic GROUPS and rows (R4) -------------------------
//
// R3 built Home, whose groups and headers are all declared in MainWindow.xaml.
// The four Wave-2 destinations generate most of theirs at runtime (the settings
// sections, the payouts table, the balance codes, the location list), so the
// pieces R3 could leave in markup have to exist as builders too. These are
// ADDITIVE: nothing above this comment changed, and every one of them is the
// markup style of the same name applied in code, so a pane built here and a
// pane built in XAML are the same pane.

// The 28px strip that opens a group inside a pane: a letterspaced caption on
// the left, an optional count/figure on the right. UrGroupHeaderStyle +
// UrGroupHeaderTextStyle + UrPaneMetaStyle, i.e. exactly what Home declares.
struct PaneGroupHeader {
  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock title{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock meta{nullptr};
  // where a group's icon-only command goes (UrPaneActionButtonStyle). Empty
  // unless the caller appends one; it costs no width when it is.
  winrt::Microsoft::UI::Xaml::Controls::Grid trailing{nullptr};
};
PaneGroupHeader MakePaneGroupHeader(winrt::hstring const& title,
                                    winrt::hstring const& meta = {});

// The two-line row: a title, and one line of explanation under it that is
// TRIMMED rather than wrapped, so the height is fixed whatever the string is.
// `trailing` is a single-cell right-aligned host for a switch, a button or a
// chevron; point that control's AutomationProperties at `title`.
//
// Height defaults to UrPaneRowTallHeight (44) because a settings list is a list
// of EXPLAINED rows: pick the tall height once for the list and the rows in it
// with no note simply centre their title in the same 44.
struct PaneTwoLineRow {
  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock title{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock note{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Grid trailing{nullptr};
};
PaneTwoLineRow MakePaneTwoLineRow(winrt::hstring const& title, winrt::hstring const& note = {},
                                  double height = 44);

// The same two-line row as a Button, for a row that opens something. Same
// metrics and the same bottom hairline, so tappable and static rows share one
// left edge and one baseline grid (UrPaneRowButtonStyle).
struct PaneTwoLineRowButton {
  winrt::Microsoft::UI::Xaml::Controls::Button root{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock title{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock note{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock value{nullptr};  // right of the chevron slot
};
PaneTwoLineRowButton MakePaneTwoLineRowButton(winrt::hstring const& title,
                                              winrt::hstring const& note = {},
                                              double height = 44);

// A table row: N cells on one grid, one fixed height, a bottom hairline. The
// widths are star weights, so the columns narrow rather than clip and the header
// strip built from the same widths stays aligned with the body.
//
// Every table on the four Wave-2 destinations (payouts, leaderboard, balance
// codes) is this call, which is what stops three tables becoming three row
// species.
struct PaneTableRow {
  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
  std::vector<winrt::Microsoft::UI::Xaml::Controls::TextBlock> cells;
};
// `textColumns` is how many LEADING columns read as text (left aligned, in the
// row-title voice); every column after them is a figure and reads right, which
// is what makes a column of numbers scannable. The payouts ledger has one
// (date); the leaderboard has two (rank and network name).
PaneTableRow MakePaneTableRow(std::vector<double> const& weights, double height = 36,
                              size_t textColumns = 1);

// Its header strip: the same weights, the same alignment rule, the column names,
// on the 28px group rhythm.
winrt::Microsoft::UI::Xaml::Controls::Border MakePaneTableHeader(
    std::vector<double> const& weights, std::vector<winrt::hstring> const& titles,
    size_t textColumns = 1);

// The empty state of a pane that FILLS: one centred muted line, sized to sit in
// the middle of a full-height pane rather than to be a short card at the top of
// one. Stretch it (it is HorizontalAlignment/VerticalAlignment Stretch inside a
// Grid cell) and it centres itself in whatever is left of the pane.
winrt::Microsoft::UI::Xaml::FrameworkElement MakePaneEmptyLine(winrt::hstring const& text);

// The search field row at the top of a list pane: a squared-off TextBox on the
// pane's 40px row metrics with the row's bottom hairline. Returns the row and
// the box.
struct PaneSearchRow {
  winrt::Microsoft::UI::Xaml::Controls::Border root{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox box{nullptr};
};
PaneSearchRow MakePaneSearchRow(winrt::hstring const& placeholder);

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
