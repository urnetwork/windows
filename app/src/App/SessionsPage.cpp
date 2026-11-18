// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "SessionsPage.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include "AppController.h"  // NoteOwnSessionSignOut: this session's sign-out signs the app out
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "BittensorKeyStore.h"  // this session's sign-out warns of a key that isn't backed up
#include "SessionGlyphs.h"
#include "SettingsSheets.h"  // rows::MakeSheet, rows::CopyToClipboard, rows::Lookup
#include "StatsFormat.h"     // RelativeTime, the user's dates and digit grouping
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using urnw::pages::H;
using urnw::pages::Loc;
using urnw::pages::Sdk;

namespace urnw {

using winrt::Windows::Foundation::IInspectable;

namespace {

namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;

// The country circle, and the device's logo in white at about half its size
// (§3): 40, the size android, iOS, ur.io and linux draw it at.
constexpr double kCircleSize = 40;
constexpr double kDeviceGlyphSize = 20;
// three lines beside the circle; a minimum, so larger text grows the row
// rather than clipping it
constexpr double kRowMinHeight = 72;

constexpr wchar_t kCopyGlyph[] = L"\uE8C8";  // Copy
// a group's heading: ChevronDown while it is closed, ChevronUp while it is
// open, as the Earnings Why? disclosure row
constexpr wchar_t kChevronDownGlyph[] = L"\uE70D";
constexpr wchar_t kChevronUpGlyph[] = L"\uE70E";

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// The page's words: the store's strings, the relative times against `nowMillis`,
// and the user's dates and digit grouping (StatsFormat.h).
sessions::Text SessionsText(int64_t nowMillis) {
  sessions::Text text;
  text.localized = [](std::string_view key) { return Narrow(Localized(key)); };
  text.format = [](std::string_view key, std::string const& value) {
    return Narrow(Format(key, Widen(value)));
  };
  text.format2 = [](std::string_view key, std::string const& first, std::string const& second) {
    return Narrow(Format(key, Widen(first), Widen(second)));
  };
  text.relative = [nowMillis](int64_t unixMillis) { return RelativeTime(unixMillis, nowMillis); };
  text.date = [](int64_t unixMillis) { return FormatLocalDate(unixMillis); };
  text.dateTime = [](int64_t unixMillis) { return FormatLocalDateTime(unixMillis); };
  text.count = [](int64_t count) { return FormatLocalCount(count); };
  return text;
}

// One line of a row: one line high whatever it says, trimmed with its whole
// text on hover, and read out as `spoken` when that differs (a relative time is
// read as the full date and time).
TextBlock MakeLine(std::string const& text, wchar_t const* style, std::string const& spoken = {}) {
  TextBlock line;
  if (auto found = rows::Lookup(style)) line.Style(found);
  line.Text(H(text));
  line.TextTrimming(TextTrimming::CharacterEllipsis);
  line.TextWrapping(TextWrapping::NoWrap);
  ToolTipService::SetToolTip(line, winrt::box_value(H(text)));
  if (!spoken.empty() && spoken != text) {
    Automation::AutomationProperties::SetName(line, H(spoken));
  }
  return line;
}

// Prose on the pane's rhythm: the row inset and hairline, wrapping allowed.
TextBlock AppendProse(Panel const& host, hstring const& text, Media::Brush const& brush) {
  Border box;
  box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
  box.BorderBrush(colors::BorderBrush());
  box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  TextBlock prose;
  prose.Text(text);
  prose.FontSize(12);
  prose.TextWrapping(TextWrapping::Wrap);
  prose.Foreground(brush);
  box.Child(prose);
  host.Children().Append(box);
  return prose;
}

// The circle in the session's country colour with the device's logo in white
// (§3, §8): what the row's first line says in words, so it is Raw for automation.
Grid MakeCountryCircle(sessions::Row const& row) {
  const sessions::Rgb rgb = sessions::CircleColorFor(
      row.countryCode, [](std::string const& code) { return urnet::getColorHex(code); });
  Grid circle;
  circle.Width(kCircleSize);
  circle.Height(kCircleSize);
  circle.VerticalAlignment(VerticalAlignment::Center);
  shapes::Ellipse dot;
  dot.Width(kCircleSize);
  dot.Height(kCircleSize);
  dot.Fill(colors::MakeBrush(winrt::Windows::UI::Color{255, rgb.r, rgb.g, rgb.b}));
  circle.Children().Append(dot);
  const auto white = colors::MakeBrush(winrt::Windows::UI::Color{255, 255, 255, 255});
  auto logo = kit::MakeRowPathIcon(sessions::GlyphPath(row.glyph), glyph::kSessionGlyphViewBox,
                                   kDeviceGlyphSize, white);
  logo.HorizontalAlignment(HorizontalAlignment::Center);
  circle.Children().Append(logo);
  Automation::AutomationProperties::SetAccessibilityView(
      circle, Automation::Peers::AccessibilityView::Raw);
  return circle;
}

// "This session": the current session's tag, in words, ahead of its first line.
Border MakeCurrentTag() {
  Border tag;
  tag.CornerRadius(CornerRadiusHelper::FromUniformRadius(9));
  tag.BorderThickness(ThicknessHelper::FromUniformLength(1));
  tag.BorderBrush(colors::AccentBrush());
  tag.Padding(ThicknessHelper::FromLengths(6, 0, 6, 1));
  tag.VerticalAlignment(VerticalAlignment::Center);
  TextBlock label;
  label.Text(Loc("sessions_this_session"));
  label.FontSize(11);
  label.Foreground(colors::AccentBrush());
  tag.Child(label);
  return tag;
}

}  // namespace

SessionsPage::SessionsPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window), snackbar_(window.SessionsInfo(), window.DispatcherQueue()) {}

SessionsPage::~SessionsPage() {
  *alive_ = false;  // the listener's queued snapshots stop here
  try {
    CloseController();
  } catch (...) {
    // the sdk may already be shutting down at teardown; nothing left to close on
  }
}

void SessionsPage::Build() {
  if (built_) return;
  built_ = true;
  w_.SessionsBackButton().Click([this](auto const&, auto const&) { w_.CloseSessions(); });
  w_.SessionsRefreshButton().Click([this](auto const&, auto const&) { Refresh(); });
}

void SessionsPage::ApplyStrings() {
  Build();
  w_.SessionsPaneTitle().Text(Loc("sessions_title"));
  Automation::AutomationProperties::SetName(w_.SessionsPane(), Loc("sessions_title"));
  // "‹ Account": the page has no rail item; this is the way back
  w_.SessionsBackButton().Content(winrt::box_value(hstring{L"‹ " + std::wstring{Loc("account")}}));
  Automation::AutomationProperties::SetName(w_.SessionsBackButton(), Loc("account"));
  w_.SessionsRefreshText().Text(Loc("refresh"));
  // the button's content is a panel, which gives it no name of its own
  Automation::AutomationProperties::SetName(w_.SessionsRefreshButton(), Loc("refresh"));
}

// ---- lifecycle ---------------------------------------------------------------

void SessionsPage::Open() {
  Build();
  CloseController();  // a page opened again starts over
  open_ = true;
  snapshot_ = {};
  rendered_.reset();
  // IsLoggedIn(), not apiReady(): apiReady is api_.has_value(), set at SDK init
  // rather than at sign-in. --preview-ui has no session either.
  if (!w_.previewUi() && Sdk().apiReady() && Sdk().IsLoggedIn()) OpenController();
  Render(/*force=*/true);
}

void SessionsPage::Close() {
  open_ = false;
  CloseController();
  snapshot_ = {};
  rendered_.reset();
  rowControls_.clear();
  sectionControls_.clear();
  bulkButton_ = nullptr;
  tryAgainButton_ = nullptr;
  w_.SessionsHost().Children().Clear();
  w_.SessionsRefreshRing().IsActive(false);
  w_.SessionsRefreshRing().Visibility(Visibility::Collapsed);
}

void SessionsPage::ResetForSignOut() { Close(); }

void SessionsPage::SetPresentationActive(bool active) {
  presentationActive_ = active;
  ApplyVisibility();
  // back in the foreground: the controller refreshes
  if (controller_) controller_->setForeground(active);
}

void SessionsPage::OpenController() {
  try {
    controller_.emplace(Sdk().api().openClientSessionViewController());
  } catch (std::exception const& e) {
    LogError("sessions: could not open the controller: {}", e.what());
    controller_.reset();
    return;
  }
  if (!*controller_) {
    // a handle of 0: the sdk refused the Api
    LogError("sessions: the sdk opened no controller");
    controller_.reset();
    return;
  }
  const uint64_t generation = fence_.Open();
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  auto alive = alive_;
  // The sdk calls from its own thread after every change. The snapshot is read
  // there, where its getters belong; only the plain value crosses to the UI
  // thread.
  sub_.emplace(controller_->addClientSessionListener(
      [queue, weak, alive, generation](urnet::ClientSessionSnapshot snapshot) {
        sessions::Snapshot read;
        try {
          read = sessions::SnapshotFrom(snapshot);
        } catch (std::exception const& e) {
          LogError("sessions: reading a snapshot failed: {}", e.what());
          return;
        }
        queue.TryEnqueue([weak, alive, generation, read = std::move(read)]() mutable {
          if (!*alive) return;
          if (auto self = weak.get()) self->sessions().ApplySnapshot(generation, std::move(read));
        });
      }));
  // Visible first, so the start's refresh is the one load: SetVisible refreshes
  // only on a change, and a start that finds that refresh queued adds none.
  visibleSent_.reset();
  ApplyVisibility();
  controller_->start();
}

void SessionsPage::CloseController() {
  fence_.Close();  // a snapshot already queued is dropped
  sub_.reset();    // unsubscribes, before the controller closes
  if (controller_) {
    controller_->close();
    controller_.reset();
  }
  visibleSent_.reset();
}

void SessionsPage::ApplyVisibility() {
  if (!controller_) return;
  // polls every 30 seconds while visible; a hidden window polls nothing
  const bool visible = open_ && presentationActive_;
  if (visibleSent_ == visible) return;
  visibleSent_ = visible;
  controller_->setVisible(visible);
}

void SessionsPage::ApplySnapshot(uint64_t generation, sessions::Snapshot snapshot) {
  // from a controller this page has closed: a sign-out, or another account's
  if (!fence_.Admits(generation)) return;
  if (snapshot.error && snapshot.error != snapshot_.error) {
    // once per new error; the page shows the flags' words, never this message
    LogWarn("sessions: {} (retryable={} sign_in_required={} session_revoked={} unsupported={})",
            snapshot.error->message, snapshot.error->retryable, snapshot.error->signInRequired,
            snapshot.error->sessionRevoked, snapshot.error->unsupported);
  }
  snapshot_ = std::move(snapshot);
  Render(/*force=*/false);
}

// ---- rendering ----------------------------------------------------------------

void SessionsPage::Render(bool force) {
  if (!built_ || !open_) return;
  auto host = w_.SessionsHost();
  if (!controller_) {
    // signed out or --preview-ui: nothing to list, and the page says why in
    // the sign-in-required body's generic words (§5)
    rendered_.reset();
    rowControls_.clear();
    sectionControls_.clear();
    bulkButton_ = nullptr;
    tryAgainButton_ = nullptr;
    host.Children().Clear();
    AppendProse(host, Loc(sessions::SignInRequiredKey(/*signedOutRemotely=*/false)),
                colors::FaintBrush());
    w_.SessionsRefreshRing().IsActive(false);
    w_.SessionsRefreshRing().Visibility(Visibility::Collapsed);
    return;
  }
  sessions::View view = sessions::ViewFor(snapshot_, SessionsText(NowMillis()));
  // the snapshot's Refreshing: the rows stay and the header says it is running
  w_.SessionsRefreshRing().IsActive(view.refreshing);
  w_.SessionsRefreshRing().Visibility(view.refreshing ? Visibility::Visible
                                                      : Visibility::Collapsed);
  view.refreshing = false;  // the header's alone
  if (!force && rendered_ && *rendered_ == view) return;
  RenderBody(view);
  rendered_ = std::move(view);
}

void SessionsPage::RenderBody(sessions::View const& view) {
  const FocusTarget focused = FocusedTarget();
  auto host = w_.SessionsHost();
  host.Children().Clear();
  rowControls_.clear();
  sectionControls_.clear();
  bulkButton_ = nullptr;
  tryAgainButton_ = nullptr;

  switch (view.body) {
    case sessions::Body::Progress: {
      // never loaded: progress, in the middle of where the list will be
      StackPanel progress;
      progress.Spacing(8);
      progress.HorizontalAlignment(HorizontalAlignment::Center);
      progress.Margin(ThicknessHelper::FromLengths(0, 32, 0, 32));
      ProgressRing ring;
      ring.IsActive(true);
      ring.Width(24);
      ring.Height(24);
      Automation::AutomationProperties::SetName(ring, Loc("loading"));
      progress.Children().Append(ring);
      host.Children().Append(progress);
      break;
    }
    case sessions::Body::LoadFailed: {
      AppendProse(host, Loc("sessions_load_failed"), colors::DangerBrush());
      tryAgainButton_ = Button();
      tryAgainButton_.Content(winrt::box_value(Loc("try_again")));
      tryAgainButton_.Style(rows::Lookup(L"UrPaneActionSecondaryStyle"));
      tryAgainButton_.Click([this](auto const&, auto const&) { Refresh(); });
      host.Children().Append(tryAgainButton_);
      break;
    }
    case sessions::Body::Unsupported:
      AppendProse(host, Loc("sessions_unsupported"), colors::MutedBrush());
      break;
    case sessions::Body::SignInRequired:
      // The generic wording, or the remote sign-out's when the controller
      // reports that trusted cause. The credential's rejection reaches
      // SdkHost's logout listeners, which run the app's sign-out (§5), and the
      // sign-in page says the same (AuthLogoutNotice.h).
      AppendProse(host, Loc(sessions::SignInRequiredKey(view.signedOutRemotely)),
                  colors::MutedBrush());
      break;
    case sessions::Body::Empty:
      AppendProse(host, Loc("sessions_empty"), colors::MutedBrush());
      break;
    case sessions::Body::Rows:
      // the last list stays through a failed refresh, under this notice
      if (view.refreshFailed) {
        AppendProse(host, Loc("sessions_refresh_failed"), colors::DangerBrush());
      }
      // the device sign-ins and their Show more, then the client groups below
      // them, then the bulk sign-out
      for (auto const& row : view.rows) AppendRow(host, row);
      if (view.showMore) AppendShowMore(host, sessions::kGroupSignIns, hstring{});
      for (auto const& group : view.groups) AppendGroup(host, group);
      AppendBulk(host, view);
      break;
  }
  if (view.lastUsedHelp) AppendProse(host, Loc("sessions_last_used_help"), colors::MutedBrush());
  if (view.legacyNote) AppendProse(host, Loc("sessions_legacy_note"), colors::MutedBrush());
  RestoreFocus(focused);
}

// One session (§3): the country circle, three lines, and Sign out. A client
// group's row is the same row, named for its kind of client
// (sessions::RowFor), and its sign-out runs the same way.
void SessionsPage::AppendRow(Panel const& host, sessions::Row const& row) {
  Border box;
  box.MinHeight(kRowMinHeight);
  box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
  box.BorderBrush(colors::BorderBrush());
  box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));

  Grid grid;
  grid.ColumnSpacing(12);
  ColumnDefinition circleColumn, textColumn, actionColumn;
  circleColumn.Width(GridLengthHelper::Auto());
  textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  actionColumn.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(circleColumn);
  grid.ColumnDefinitions().Append(textColumn);
  grid.ColumnDefinitions().Append(actionColumn);
  grid.Children().Append(MakeCountryCircle(row));

  StackPanel text;
  text.Spacing(2);
  text.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(text, 1);

  // line 1: the device and its version, after the current session's tag; the
  // tag's column only when there is one, so every row's line 1 starts where
  // its other lines do
  Grid first;
  auto line1 = MakeLine(row.line1, L"UrRowTitleStyle", row.line1Spoken);
  if (row.current) {
    first.ColumnSpacing(8);
    ColumnDefinition tagColumn, firstColumn;
    tagColumn.Width(GridLengthHelper::Auto());
    firstColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    first.ColumnDefinitions().Append(tagColumn);
    first.ColumnDefinitions().Append(firstColumn);
    first.Children().Append(MakeCurrentTag());
    Grid::SetColumn(line1, 1);
  }
  first.Children().Append(line1);
  text.Children().Append(first);

  // line 2: where and when it was last used
  text.Children().Append(MakeLine(row.line2, L"UrRowNoteStyle", row.line2Spoken));

  // line 3: when and how it signed in and its short id, with the copy of the
  // whole id (§1.3)
  Grid third;
  third.ColumnSpacing(4);
  ColumnDefinition thirdColumn, copyColumn;
  thirdColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  copyColumn.Width(GridLengthHelper::Auto());
  third.ColumnDefinitions().Append(thirdColumn);
  third.ColumnDefinitions().Append(copyColumn);
  auto line3 = MakeLine(row.line3, L"UrRowNoteStyle", row.line3Spoken);
  line3.VerticalAlignment(VerticalAlignment::Center);
  third.Children().Append(line3);
  Button copy;
  copy.Style(rows::Lookup(L"UrPaneActionButtonStyle"));
  copy.HorizontalAlignment(HorizontalAlignment::Left);
  FontIcon copyGlyph;
  copyGlyph.FontFamily(Media::FontFamily(kit::kIconFontFamily));
  copyGlyph.Glyph(kCopyGlyph);
  copyGlyph.FontSize(12);
  copyGlyph.Foreground(colors::MutedBrush());
  copy.Content(copyGlyph);
  Automation::AutomationProperties::SetName(copy, Loc("sessions_copy_id"));
  // which id: every row's copy button has the one name
  Automation::AutomationProperties::SetFullDescription(copy, H(row.idText));
  ToolTipService::SetToolTip(copy, winrt::box_value(Loc("sessions_copy_id")));
  const std::string sessionId = row.sessionId;
  copy.Click([this, sessionId](auto const&, auto const&) { CopyId(sessionId); });
  Grid::SetColumn(copy, 1);
  third.Children().Append(copy);
  text.Children().Append(third);

  // the row's own action error (§5); Sign out is offered again
  if (row.action == sessions::ActionState::Failed) {
    TextBlock failed;
    failed.Text(Loc(sessions::ActionFailedKey(/*bulk=*/false)));
    failed.FontSize(11);
    failed.TextWrapping(TextWrapping::Wrap);
    failed.Foreground(colors::DangerBrush());
    text.Children().Append(failed);
  }
  grid.Children().Append(text);

  // Sign out: progress and "Signing out…" while the controller's request is out
  // or pending (§4); disabled then, which with the single-sheet gate and the
  // controller's own refusal keeps a revoke from running twice
  StackPanel action;
  action.Orientation(Orientation::Horizontal);
  action.Spacing(8);
  action.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(action, 2);
  const bool pending = row.action == sessions::ActionState::Pending;
  if (pending) {
    ProgressRing ring;
    ring.IsActive(true);
    ring.Width(16);
    ring.Height(16);
    Automation::AutomationProperties::SetAccessibilityView(
        ring, Automation::Peers::AccessibilityView::Raw);
    action.Children().Append(ring);
  }
  Button signOut;
  signOut.Content(winrt::box_value(pending ? Loc("sessions_signing_out") : Loc("sign_out")));
  signOut.IsEnabled(!pending);
  // "Sign out Android": the row's action for a screen reader, whatever the
  // button shows
  Automation::AutomationProperties::SetName(signOut, H(row.signOutName));
  if (pending) {
    Automation::AutomationProperties::SetItemStatus(signOut, Loc("sessions_signing_out"));
  }
  signOut.Click([this, sessionId](auto const&, auto const&) { ConfirmSignOut(sessionId); });
  action.Children().Append(signOut);
  grid.Children().Append(action);

  box.Child(grid);
  host.Children().Append(box);
  rowControls_.push_back(RowControls{
      sessionId, signOut, copy, row.group.empty() ? std::string(sessions::kGroupSignIns) : row.group});
}

// "Show more" at the end of a section: the device sign-ins' or an open group's
// next rows, through the controller. A pane row, like the disclosure rows.
void SessionsPage::AppendShowMore(Panel const& host, std::string const& kind,
                                  hstring const& section) {
  Button more;
  more.Style(rows::Lookup(L"UrPaneRowButtonStyle"));
  TextBlock label;
  if (auto found = rows::Lookup(L"UrRowTitleStyle")) label.Style(found);
  label.Text(Loc("sessions_show_more"));
  // the button's name says it
  Automation::AutomationProperties::SetAccessibilityView(
      label, Automation::Peers::AccessibilityView::Raw);
  more.Content(label);
  Automation::AutomationProperties::SetName(more, Loc("sessions_show_more"));
  // which section: every section's Show more has the one name
  if (!section.empty()) Automation::AutomationProperties::SetFullDescription(more, section);
  more.Click([this, kind](auto const&, auto const&) { ShowMore(kind); });
  host.Children().Append(more);
  SectionFor(kind).showMore = more;
}

// A client group below the device sign-ins: its heading, which opens and
// closes it, then while it is open its rows and Show more. The heading is the
// app's disclosure row (the Earnings Why?, Account's Advanced): a pane row
// button with its words and a chevron, named for a screen reader by its words,
// and a heading of the page as the pane's group headers are. Opening a group
// asks the controller, which publishes the open group's first rows at once.
void SessionsPage::AppendGroup(Panel const& host, sessions::GroupView const& group) {
  Button header;
  header.Style(rows::Lookup(L"UrPaneRowButtonStyle"));
  Grid content;
  content.ColumnSpacing(8);
  ColumnDefinition labelColumn, chevronColumn;
  labelColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  chevronColumn.Width(GridLengthHelper::Auto());
  content.ColumnDefinitions().Append(labelColumn);
  content.ColumnDefinitions().Append(chevronColumn);
  TextBlock label;
  if (auto found = rows::Lookup(L"UrRowTitleStyle")) label.Style(found);
  label.Text(H(group.label));
  // "Hosted proxies (1,204+)" is the button's name; read once
  Automation::AutomationProperties::SetAccessibilityView(
      label, Automation::Peers::AccessibilityView::Raw);
  content.Children().Append(label);
  FontIcon chevron;
  chevron.FontFamily(Media::FontFamily(kit::kIconFontFamily));
  chevron.Glyph(group.expanded ? kChevronUpGlyph : kChevronDownGlyph);
  chevron.FontSize(12);
  chevron.Foreground(colors::MutedBrush());
  chevron.VerticalAlignment(VerticalAlignment::Center);
  Automation::AutomationProperties::SetAccessibilityView(
      chevron, Automation::Peers::AccessibilityView::Raw);
  Grid::SetColumn(chevron, 1);
  content.Children().Append(chevron);
  header.Content(content);
  Automation::AutomationProperties::SetName(header, H(group.label));
  Automation::AutomationProperties::SetHeadingLevel(
      header, Automation::Peers::AutomationHeadingLevel::Level3);
  const std::string kind = group.kind;
  const bool expanded = group.expanded;
  header.Click([this, kind, expanded](auto const&, auto const&) { SetGroupExpanded(kind, !expanded); });
  host.Children().Append(header);
  SectionFor(kind).header = header;

  if (!group.expanded) return;
  for (auto const& row : group.rows) AppendRow(host, row);
  if (group.showMore) AppendShowMore(host, kind, H(group.label));
}

SessionsPage::SectionControls& SessionsPage::SectionFor(std::string const& kind) {
  for (auto& section : sectionControls_) {
    if (section.kind == kind) return section;
  }
  sectionControls_.push_back(SectionControls{kind});
  return sectionControls_.back();
}

// "Sign out all other sessions", at the bottom of the list, with a current
// session and other sessions it signs out (§4; the controller counts them,
// and the hosted proxies, which it keeps, are not among them).
void SessionsPage::AppendBulk(Panel const& host, sessions::View const& view) {
  if (!view.bulkShown) return;
  const bool pending = view.bulk == sessions::ActionState::Pending;
  bulkButton_ = Button();
  bulkButton_.Style(rows::Lookup(L"UrPaneActionSecondaryStyle"));
  if (pending) {
    StackPanel content;
    content.Orientation(Orientation::Horizontal);
    content.Spacing(8);
    ProgressRing ring;
    ring.IsActive(true);
    ring.Width(14);
    ring.Height(14);
    content.Children().Append(ring);
    TextBlock label;
    label.Text(Loc("sessions_signing_out"));
    label.VerticalAlignment(VerticalAlignment::Center);
    content.Children().Append(label);
    bulkButton_.Content(content);
    Automation::AutomationProperties::SetItemStatus(bulkButton_, Loc("sessions_signing_out"));
  } else {
    bulkButton_.Content(winrt::box_value(Loc("sessions_sign_out_all_others")));
  }
  // enabled once it failed too: a retry confirms again and goes through the
  // controller, which reuses its operation
  bulkButton_.IsEnabled(!pending);
  Automation::AutomationProperties::SetName(bulkButton_, Loc("sessions_sign_out_all_others"));
  bulkButton_.Click([this](auto const&, auto const&) { ConfirmSignOutOthers(); });
  host.Children().Append(bulkButton_);
  // under the button: "Couldn't sign out the other sessions. Try again."
  if (view.bulk == sessions::ActionState::Failed) {
    AppendProse(host, Loc(sessions::ActionFailedKey(/*bulk=*/true)), colors::DangerBrush());
  }
}

// ---- focus ------------------------------------------------------------------

SessionsPage::FocusTarget SessionsPage::FocusedTarget() const {
  FocusTarget target;
  auto root = w_.SessionsHost().XamlRoot();
  if (!root) return target;
  const IInspectable focused = Input::FocusManager::GetFocusedElement(root);
  if (!focused) return target;
  for (auto const& row : rowControls_) {
    if (row.signOut && focused == row.signOut) {
      return {FocusKind::SignOut, row.sessionId, row.section};
    }
    if (row.copy && focused == row.copy) return {FocusKind::Copy, row.sessionId, row.section};
  }
  for (auto const& section : sectionControls_) {
    if (section.header && focused == section.header) {
      return {FocusKind::GroupHeader, {}, section.kind};
    }
    if (section.showMore && focused == section.showMore) {
      return {FocusKind::ShowMore, {}, section.kind};
    }
  }
  if (bulkButton_ && focused == bulkButton_) return {FocusKind::Bulk, {}};
  if (tryAgainButton_ && focused == tryAgainButton_) return {FocusKind::TryAgain, {}};
  return target;
}

// The same control in the rebuilt body; when it is gone (its row was signed
// out, its group emptied, its section shows every row) or disabled (a sign-out
// is running), the nearest thing before it: the next thing on its row, the
// last row of a section whose Show more is gone, its group's heading. Then
// Refresh, which is always there.
void SessionsPage::RestoreFocus(FocusTarget const& target) {
  if (target.kind == FocusKind::None) return;
  const auto focus = [](Button const& button) {
    return button && button.IsEnabled() && button.Focus(FocusState::Programmatic);
  };
  const auto focusRow = [&focus](RowControls const& row) {
    return focus(row.signOut) || focus(row.copy);
  };
  const SectionControls* section = nullptr;
  for (auto const& controls : sectionControls_) {
    if (controls.kind == target.section) section = &controls;
  }
  switch (target.kind) {
    case FocusKind::SignOut:
    case FocusKind::Copy:
      for (auto const& row : rowControls_) {
        if (row.sessionId != target.sessionId) continue;
        if (target.kind == FocusKind::SignOut && focus(row.signOut)) return;
        if (focus(row.copy)) return;
      }
      break;
    case FocusKind::ShowMore: {
      if (section && focus(section->showMore)) return;
      // every row shown: the section's last one
      const RowControls* last = nullptr;
      for (auto const& row : rowControls_) {
        if (row.section == target.section) last = &row;
      }
      if (last && focusRow(*last)) return;
      break;
    }
    case FocusKind::Bulk:
      if (focus(bulkButton_)) return;
      break;
    case FocusKind::TryAgain:
      if (focus(tryAgainButton_)) return;
      break;
    case FocusKind::GroupHeader:
    case FocusKind::None:
      break;
  }
  // a group's heading: the target itself, or what is left above a gone row
  // or Show more of its group
  if (section && focus(section->header)) return;
  w_.SessionsRefreshButton().Focus(FocusState::Programmatic);
}

// ---- actions ----------------------------------------------------------------

void SessionsPage::Refresh() {
  if (controller_) controller_->refresh();
}

// Presentation only: the controller requests nothing, keeps the group open
// through reloads, and publishes the snapshot with its rows before it returns
// (SessionsPage.h). Its focus comes back to the rebuilt heading.
void SessionsPage::SetGroupExpanded(std::string const& kind, bool expanded) {
  if (controller_) controller_->setGroupExpanded(kind, expanded);
}

void SessionsPage::ShowMore(std::string const& kind) {
  if (controller_) controller_->showMoreSessions(kind);
}

void SessionsPage::CopyId(std::string const& sessionId) {
  // the whole id: the row shows its first 8 characters
  rows::CopyToClipboard(sessionId);
  snackbar_.Show(Loc("copied"), InfoBarSeverity::Success);
}

// Names the session, defaults to Cancel and signs out only on the explicit
// button (ConfirmRemoveAuth's shape); the current session's warns that this
// app signs out with it, and a hosted proxy's that the proxy stops working.
winrt::fire_and_forget SessionsPage::ConfirmSignOut(std::string sessionId) {
  if (w_.sheetOpen() || !rendered_) co_return;
  // a device sign-in's row or an open group's; read here, before the dialog,
  // since a snapshot may rebuild rendered_ while it is up
  sessions::Confirmation confirmation;
  bool current = false;
  std::string section;
  if (const sessions::Row* row = sessions::FindRow(*rendered_, sessionId)) {
    confirmation = sessions::ConfirmationFor(*row, SessionsText(NowMillis()));
    current = row->current;
    // where focus goes back to should the row be gone when the dialog closes
    section = row->group.empty() ? std::string(sessions::kGroupSignIns) : row->group;
  } else {
    co_return;
  }
  // This session's sign-out signs this app out: a one-tap Bittensor key here
  // whose phrase isn't known to be saved stays on this device, and the
  // confirmation says so.
  if (current) {
    std::string networkId;
    if (auto jwt = Sdk().ParsedJwt(); jwt && jwt->NetworkId) networkId = *jwt->NetworkId;
    if (BittensorKeyNeedsBackup(networkId)) {
      confirmation.body += "\n\n" + Narrow(std::wstring_view{Loc("bittensor_key_sign_out_message")});
    }
  }
  // the controller this confirmation is for
  const uint64_t generation = fence_.Current();
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  bool confirmed = false;
  try {
    auto dialog = rows::MakeSheet(self->Content().XamlRoot(), H(confirmation.title));
    dialog.PrimaryButtonText(Loc("sign_out"));
    dialog.CloseButtonText(Loc("cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);  // Enter must not sign out
    TextBlock body;
    body.Text(H(confirmation.body));
    body.FontSize(14);
    body.TextWrapping(TextWrapping::Wrap);
    body.MinWidth(320);
    dialog.Content(body);
    confirmed = co_await dialog.ShowAsync() == ContentDialogResult::Primary;
  } catch (...) {
  }
  w_.SetSheetOpen(false);
  if (confirmed) {
    // this app's own session: its sign-out follows however the sdk reports
    // the rejection (AppController::NoteOwnSessionSignOut), when this page's
    // controller is the one that signs it out
    if (current && controller_ && fence_.Admits(generation)) urnw::App().NoteOwnSessionSignOut();
    SignOut(sessionId, generation);
  }
  // back on the row the dialog was for, or its nearest neighbour
  RestoreFocus({FocusKind::SignOut, sessionId, section});
}

// "Sign out all other sessions?", which says the hosted proxies stay when the
// list has any (the controller's bulk sign-out keeps them).
winrt::fire_and_forget SessionsPage::ConfirmSignOutOthers() {
  if (w_.sheetOpen() || !rendered_) co_return;
  const sessions::Confirmation confirmation =
      sessions::BulkConfirmationFor(*rendered_, SessionsText(NowMillis()));
  const uint64_t generation = fence_.Current();
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  bool confirmed = false;
  try {
    auto dialog = rows::MakeSheet(self->Content().XamlRoot(), H(confirmation.title));
    dialog.PrimaryButtonText(Loc("sign_out"));
    dialog.CloseButtonText(Loc("cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);  // Enter must not sign out
    TextBlock body;
    body.Text(H(confirmation.body));
    body.FontSize(14);
    body.TextWrapping(TextWrapping::Wrap);
    body.MinWidth(320);
    dialog.Content(body);
    confirmed = co_await dialog.ShowAsync() == ContentDialogResult::Primary;
  } catch (...) {
  }
  w_.SetSheetOpen(false);
  if (confirmed) SignOutOthers(generation);
  RestoreFocus({FocusKind::Bulk, {}});
}

void SessionsPage::SignOut(std::string const& sessionId, uint64_t generation) {
  // Closed, or another controller, while the dialog was up: it was for a list
  // that is no longer on screen. Repeats go through the controller, which
  // reuses its operation and ignores one while a request is out or pending.
  if (!controller_ || !fence_.Admits(generation)) return;
  controller_->revokeSession(sessionId);
}

void SessionsPage::SignOutOthers(uint64_t generation) {
  if (!controller_ || !fence_.Admits(generation)) return;
  controller_->revokeOtherSessions();
}

}  // namespace urnw
