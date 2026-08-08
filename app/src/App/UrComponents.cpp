// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "UrComponents.h"

#include <chrono>
#include <string>

#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.UI.Text.h>

#include "Log.h"
#include "UrColors.h"

using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace urnw::kit {
namespace {

// Segoe Fluent Icons ships with Windows 11. Naming it matters: FontIcon
// defaults to the older Segoe MDL2 Assets, whose metrics differ, so a screen
// that mixes the default with an explicit family comes out with two icon
// weights. App.xaml carries the same family under UrIconFontFamily.
Media::FontFamily IconFont() { return Media::FontFamily(L"Segoe Fluent Icons"); }

}  // namespace

void SetTextOrCollapse(TextBlock const& line, winrt::hstring const& text) {
  if (!line) return;
  line.Text(text);
  line.Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
}

Controls::Border MakeDivider() {
  Controls::Border rule;
  rule.Height(1);
  rule.HorizontalAlignment(HorizontalAlignment::Stretch);
  rule.Background(urnw::colors::BorderBrush());
  return rule;
}

FrameworkElement MakeSectionHeader(winrt::hstring const& glyph, winrt::hstring const& text) {
  StackPanel row;
  row.Orientation(Controls::Orientation::Horizontal);
  row.Spacing(8);

  FontIcon icon;
  icon.FontFamily(IconFont());
  icon.Glyph(glyph);
  icon.FontSize(16);
  icon.Foreground(urnw::colors::MutedBrush());
  icon.VerticalAlignment(VerticalAlignment::Center);
  // decoration beside a label that already carries the word
  Automation::AutomationProperties::SetAccessibilityView(
      icon, Automation::Peers::AccessibilityView::Raw);
  row.Children().Append(icon);

  TextBlock label;
  label.Text(text);
  label.FontSize(16);
  label.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  label.Foreground(urnw::colors::TextBrush());
  label.VerticalAlignment(VerticalAlignment::Center);
  row.Children().Append(label);
  return row;
}

namespace {

// A style out of the app dictionary, by key, or null if it is missing. Applying
// styles by key rather than by hand is what keeps the strip in step with
// App.xaml; a missing key must not throw a layout away.
Style StyleByKey(wchar_t const* key) {
  auto app = Application::Current();
  if (!app) return nullptr;
  auto boxed = winrt::box_value(winrt::hstring{key});
  if (!app.Resources().HasKey(boxed)) return nullptr;
  return app.Resources().Lookup(boxed).try_as<Style>();
}

}  // namespace

StatusField MakeStatusField(winrt::hstring const& label, bool withDot,
                            winrt::hstring const& accessibleName) {
  StackPanel row;
  row.Orientation(Controls::Orientation::Horizontal);
  row.Spacing(6);
  row.VerticalAlignment(VerticalAlignment::Center);

  StatusField field;
  if (withDot) {
    field.dot = winrt::Microsoft::UI::Xaml::Shapes::Ellipse();
    field.dot.Width(8);
    field.dot.Height(8);
    field.dot.VerticalAlignment(VerticalAlignment::Center);
    // the colour IS the information, and the text beside it says the same
    // thing in words, so the shape itself is decoration to a screen reader
    Automation::AutomationProperties::SetAccessibilityView(
        field.dot, Automation::Peers::AccessibilityView::Raw);
    row.Children().Append(field.dot);
  }

  if (!label.empty()) {
    field.caption = TextBlock();
    field.caption.Text(label);
    if (auto style = StyleByKey(L"UrStatusFieldLabelStyle")) field.caption.Style(style);
    // The caption is the field's NAME and the value is its content: announcing
    // "Selected provider" as a separate static-text item beside "Berlin" makes
    // the strip six unrelated fragments instead of three facts. This is also
    // why hiding the caption at narrow widths costs a screen reader nothing.
    Automation::AutomationProperties::SetAccessibilityView(
        field.caption, Automation::Peers::AccessibilityView::Raw);
    row.Children().Append(field.caption);
  }

  field.value = TextBlock();
  if (auto style = StyleByKey(L"UrStatusFieldValueStyle")) field.value.Style(style);
  field.name = accessibleName.empty() ? label : accessibleName;
  // Seeded here and rewritten by SetStatusFieldValue on every update; a field
  // with no value yet announces at least what it is.
  Automation::AutomationProperties::SetName(field.value, field.name);
  row.Children().Append(field.value);

  field.root = row;
  return field;
}

void SetStatusFieldValue(StatusField const& field, winrt::hstring const& value) {
  if (!field.value) return;
  field.value.Text(value);
  Automation::AutomationProperties::SetName(
      field.value,
      field.name.empty() ? value : field.name + winrt::hstring{L", "} + value);
}

Controls::Border MakeStatusSeparator() {
  Controls::Border rule;
  rule.Width(1);
  rule.Height(14);
  rule.Margin(ThicknessHelper::FromLengths(14, 0, 14, 0));
  rule.VerticalAlignment(VerticalAlignment::Center);
  rule.Background(urnw::colors::BorderBrush());
  return rule;
}

// ---- the pane shell's dynamic rows (R3) ------------------------------------

Controls::Border MakePaneRow(double height) {
  Controls::Border row;
  // Height, not MinHeight. A minimum is what lets one row of a list grow when
  // its content happens to be longer, and a list whose rows are mostly-but-not-
  // always the same height is precisely the defect this layout answers.
  row.Height(height);
  row.HorizontalAlignment(HorizontalAlignment::Stretch);
  row.Padding(ThicknessHelper::FromLengths(12, 0, 12, 0));
  row.BorderBrush(urnw::colors::BorderBrush());
  row.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  return row;
}

PaneKeyValueRow MakePaneKeyValueRow(winrt::hstring const& key, winrt::hstring const& value,
                                    double height) {
  PaneKeyValueRow out;
  out.root = MakePaneRow(height);

  Controls::Grid grid;
  grid.ColumnSpacing(8);
  Controls::ColumnDefinition keyColumn, valueColumn;
  keyColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  valueColumn.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(keyColumn);
  grid.ColumnDefinitions().Append(valueColumn);

  out.key = TextBlock();
  out.key.Text(key);
  if (auto style = StyleByKey(L"UrKeyTextStyle")) out.key.Style(style);
  grid.Children().Append(out.key);

  out.value = TextBlock();
  out.value.Text(value);
  if (auto style = StyleByKey(L"UrValueTextStyle")) out.value.Style(style);
  Controls::Grid::SetColumn(out.value, 1);
  grid.Children().Append(out.value);

  // The key names the value; announcing it as its own static-text item beside
  // the value makes one fact into two fragments. Same treatment, same reason, as
  // MakeStatusField's caption.
  Automation::AutomationProperties::SetAccessibilityView(
      out.key, Automation::Peers::AccessibilityView::Raw);
  Automation::AutomationProperties::SetName(
      out.value, winrt::hstring{std::wstring{key} + L", " + std::wstring{value}});

  out.root.Child(grid);
  return out;
}

PaneListRow MakePaneListRow(double height) {
  PaneListRow out;
  out.root = MakePaneRow(height);

  Controls::Grid grid;
  grid.ColumnSpacing(10);
  Controls::ColumnDefinition dotColumn, titleColumn, metaColumn;
  dotColumn.Width(GridLengthHelper::Auto());
  titleColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  metaColumn.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(dotColumn);
  grid.ColumnDefinitions().Append(titleColumn);
  grid.ColumnDefinitions().Append(metaColumn);

  out.dot = winrt::Microsoft::UI::Xaml::Shapes::Ellipse();
  out.dot.Width(7);
  out.dot.Height(7);
  out.dot.VerticalAlignment(VerticalAlignment::Center);
  // the colour is a restatement of what the row's text already says
  Automation::AutomationProperties::SetAccessibilityView(
      out.dot, Automation::Peers::AccessibilityView::Raw);
  grid.Children().Append(out.dot);

  out.title = TextBlock();
  if (auto style = StyleByKey(L"UrRowTitleStyle")) out.title.Style(style);
  Controls::Grid::SetColumn(out.title, 1);
  grid.Children().Append(out.title);

  out.meta = TextBlock();
  if (auto style = StyleByKey(L"UrValueTextStyle")) out.meta.Style(style);
  out.meta.Foreground(urnw::colors::MutedBrush());
  Controls::Grid::SetColumn(out.meta, 2);
  grid.Children().Append(out.meta);

  out.root.Child(grid);
  return out;
}

FrameworkElement MakeEmptyState(winrt::hstring const& glyph, winrt::hstring const& text) {
  StackPanel column;
  column.Spacing(8);
  column.HorizontalAlignment(HorizontalAlignment::Center);
  column.Padding(ThicknessHelper::FromLengths(16, 24, 16, 24));

  FontIcon icon;
  icon.FontFamily(IconFont());
  icon.Glyph(glyph);
  icon.FontSize(28);
  icon.Foreground(urnw::colors::FaintBrush());
  icon.HorizontalAlignment(HorizontalAlignment::Center);
  Automation::AutomationProperties::SetAccessibilityView(
      icon, Automation::Peers::AccessibilityView::Raw);
  column.Children().Append(icon);

  TextBlock line;
  line.Text(text);
  line.FontSize(12);
  line.TextWrapping(TextWrapping::Wrap);
  line.TextAlignment(TextAlignment::Center);
  line.Foreground(urnw::colors::MutedBrush());
  line.HorizontalAlignment(HorizontalAlignment::Center);
  column.Children().Append(line);
  return column;
}

FrameworkElement MakeEmptyStateCard(winrt::hstring const& glyph, winrt::hstring const& text) {
  Controls::Border card;
  // by key rather than by hand, so the empty state inherits whatever UrCardStyle
  // says a card is - including the hairline it grew in this pass
  if (auto style = StyleByKey(L"UrCardStyle")) card.Style(style);
  card.Child(MakeEmptyState(glyph, text));
  return card;
}

// ---- the Wave-2 component kit (spec §12) -----------------------------------

FrameworkElement MakePageHeader(winrt::hstring const& title,
                                winrt::hstring const& description) {
  StackPanel column;
  column.Spacing(4);
  column.VerticalAlignment(VerticalAlignment::Top);

  TextBlock titleBlock;
  titleBlock.Text(title);
  if (auto style = StyleByKey(L"UrTitleTextStyle")) titleBlock.Style(style);
  column.Children().Append(titleBlock);

  if (!description.empty()) {
    TextBlock desc;
    desc.Text(description);
    if (auto style = StyleByKey(L"UrBodyTextStyle")) desc.Style(style);
    desc.Foreground(urnw::colors::MutedBrush());
    desc.TextWrapping(TextWrapping::Wrap);
    // ~60ch reading measure, per the spec's page-description rule
    desc.MaxWidth(560);
    desc.HorizontalAlignment(HorizontalAlignment::Left);
    column.Children().Append(desc);
  }
  return column;
}

MetricCard MakeMetricCard(winrt::hstring const& label, winrt::hstring const& value) {
  MetricCard card;
  card.root = Controls::Border();
  if (auto style = StyleByKey(L"UrStatTileStyle")) card.root.Style(style);

  StackPanel column;
  card.label = TextBlock();
  card.label.Text(label);
  if (auto style = StyleByKey(L"UrStatLabelStyle")) card.label.Style(style);
  column.Children().Append(card.label);

  card.value = TextBlock();
  card.value.Text(value);
  if (auto style = StyleByKey(L"UrStatValueStyle")) card.value.Style(style);
  column.Children().Append(card.value);

  card.root.Child(column);
  return card;
}

SettingsCard MakeSettingsCard(winrt::hstring const& glyph, winrt::hstring const& title,
                              winrt::hstring const& description) {
  SettingsCard card;
  card.root = Controls::Border();
  if (auto style = StyleByKey(L"UrCardStyle")) card.root.Style(style);

  Grid row;
  row.ColumnSpacing(12);
  { Controls::ColumnDefinition c; c.Width(GridLengthHelper::Auto()); row.ColumnDefinitions().Append(c); }
  { Controls::ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); row.ColumnDefinitions().Append(c); }
  { Controls::ColumnDefinition c; c.Width(GridLengthHelper::Auto()); row.ColumnDefinitions().Append(c); }

  FontIcon icon;
  icon.FontFamily(IconFont());
  icon.Glyph(glyph);
  icon.FontSize(20);
  icon.Foreground(urnw::colors::MutedBrush());
  icon.VerticalAlignment(VerticalAlignment::Center);
  Automation::AutomationProperties::SetAccessibilityView(
      icon, Automation::Peers::AccessibilityView::Raw);
  Grid::SetColumn(icon, 0);
  row.Children().Append(icon);

  StackPanel text;
  text.Spacing(2);
  text.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(text, 1);
  card.title = TextBlock();
  card.title.Text(title);
  if (auto style = StyleByKey(L"UrBodyTextStyle")) card.title.Style(style);
  card.title.TextWrapping(TextWrapping::Wrap);
  text.Children().Append(card.title);
  card.description = TextBlock();
  card.description.Text(description);
  if (auto style = StyleByKey(L"UrCaptionTextStyle")) card.description.Style(style);
  card.description.TextWrapping(TextWrapping::Wrap);
  // an empty description must not spend the panel's spacing on nothing
  card.description.Visibility(description.empty() ? Visibility::Collapsed
                                                  : Visibility::Visible);
  text.Children().Append(card.description);
  row.Children().Append(text);

  card.trailing = Grid();
  card.trailing.HorizontalAlignment(HorizontalAlignment::Right);
  card.trailing.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(card.trailing, 2);
  row.Children().Append(card.trailing);

  card.root.Child(row);
  return card;
}

CopyField MakeCopyField(winrt::hstring const& label, winrt::hstring const& value,
                        bool masked) {
  CopyField field;
  StackPanel column;
  column.Spacing(2);

  TextBlock caption;
  caption.Text(label);
  if (auto style = StyleByKey(L"UrLabelStyle")) caption.Style(style);
  column.Children().Append(caption);

  Grid row;
  row.ColumnSpacing(8);
  { Controls::ColumnDefinition c; c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star)); row.ColumnDefinitions().Append(c); }
  { Controls::ColumnDefinition c; c.Width(GridLengthHelper::Auto()); row.ColumnDefinitions().Append(c); }

  field.value = TextBlock();
  // masked DISPLAY only; the real value is captured by the copy button below
  field.value.Text(masked ? winrt::hstring{L"••••••••"} : value);
  if (auto style = StyleByKey(L"UrBodyTextStyle")) field.value.Style(style);
  field.value.TextTrimming(TextTrimming::CharacterEllipsis);
  field.value.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(field.value, 0);
  row.Children().Append(field.value);

  field.copy = Button();
  field.copy.Background(nullptr);
  field.copy.BorderThickness(ThicknessHelper::FromUniformLength(0));
  field.copy.Padding(ThicknessHelper::FromLengths(8, 4, 8, 4));
  field.copy.VerticalAlignment(VerticalAlignment::Center);
  FontIcon copyGlyph;
  copyGlyph.FontFamily(IconFont());
  copyGlyph.Glyph(L"");  // Copy
  copyGlyph.FontSize(16);
  copyGlyph.Foreground(urnw::colors::MutedBrush());
  field.copy.Content(copyGlyph);
  // the button carries the action name a screen reader announces
  Automation::AutomationProperties::SetName(field.copy, winrt::hstring{L"Copy "} + label);
  // the full value is copied, never the mask
  field.copy.Click([value](auto const&, auto const&) {
    winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
    package.SetText(value);
    winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
  });
  Grid::SetColumn(field.copy, 1);
  row.Children().Append(field.copy);

  column.Children().Append(row);
  field.value.VerticalAlignment(VerticalAlignment::Center);
  field.root = column;
  return field;
}

PlanUsageCard MakePlanUsageCard(winrt::hstring const& planLabel,
                                winrt::hstring const& planValue) {
  PlanUsageCard card;
  card.root = Controls::Border();
  if (auto style = StyleByKey(L"UrCardStyle")) card.root.Style(style);

  StackPanel column;
  column.Spacing(8);

  TextBlock caption;
  caption.Text(planLabel);
  if (auto style = StyleByKey(L"UrLabelStyle")) caption.Style(style);
  column.Children().Append(caption);

  card.planValue = TextBlock();
  card.planValue.Text(planValue);
  if (auto style = StyleByKey(L"UrStatValueStyle")) card.planValue.Style(style);
  card.planValue.FontSize(22);
  column.Children().Append(card.planValue);

  // the caller constructs a urnw::UsageBar into this host (the bar is not a XAML
  // control), and its legend into the panel below - the same wiring the connect
  // drawer and Account already do around their own hosts.
  card.usageBarHost = Grid();
  card.usageBarHost.Height(32);
  column.Children().Append(card.usageBarHost);

  card.legend = StackPanel();
  card.legend.Orientation(Controls::Orientation::Horizontal);
  card.legend.Spacing(16);
  column.Children().Append(card.legend);

  card.root.Child(column);
  return card;
}

void ApplySupportingText(TextBlock const& line, winrt::hstring const& text,
                         ValidationState state) {
  if (!line) return;
  line.Text(text);
  switch (state) {
    case ValidationState::Invalid:
      line.Foreground(urnw::colors::DangerBrush());
      break;
    case ValidationState::Valid:
      line.Foreground(urnw::colors::MakeBrush(urnw::colors::kUrGreen));
      break;
    case ValidationState::NotChecked:
    case ValidationState::Validating:
      line.Foreground(urnw::colors::MutedBrush());
      break;
  }
}

Snackbar::Snackbar(InfoBar bar,
                   winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
                   int durationMs)
    : bar_(std::move(bar)),
      defaultDurationMs_(durationMs),
      self_(std::make_shared<Snackbar*>(this)) {
  if (!bar_ || !queue) {
    // A snackbar with no bar is a message that goes nowhere. Say so once here
    // rather than let every later Show() silently do nothing.
    urnw::LogError("kit: snackbar constructed without {}",
                   bar_ ? "a dispatcher queue" : "an InfoBar");
    return;
  }
  timer_ = queue.CreateTimer();
  timer_.IsRepeating(false);
  // `self_` rather than a raw `this`: the destructor nulls it, so a tick that
  // outlives its Snackbar does nothing instead of writing through a dangling
  // pointer. The interval is set per Show(), since it depends on severity.
  timer_.Tick([self = self_](auto const&, auto const&) {
    if (auto* snackbar = *self) snackbar->Hide();
  });
}

Snackbar::~Snackbar() {
  if (timer_) timer_.Stop();
  if (self_) *self_ = nullptr;
}

void Snackbar::Show(winrt::hstring const& message, InfoBarSeverity severity,
                    std::optional<int> durationMs) {
  if (!bar_) return;
  bar_.Severity(severity);
  bar_.Message(message);
  bar_.IsOpen(true);
  if (!timer_) return;
  timer_.Stop();  // a second message restarts the window rather than inheriting it

  // An error is usually the only diagnostic the user gets; it waits for them.
  const bool safeToMiss = (severity == InfoBarSeverity::Informational ||
                           severity == InfoBarSeverity::Success);
  const int duration =
      durationMs.value_or(safeToMiss ? defaultDurationMs_ : kPersistent);
  if (duration <= kPersistent) return;
  timer_.Interval(std::chrono::milliseconds(duration));
  timer_.Start();
}

void Snackbar::Hide() {
  if (timer_) timer_.Stop();
  if (bar_) bar_.IsOpen(false);
}

}  // namespace urnw::kit
