// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "UrComponents.h"

#include <chrono>

#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
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
