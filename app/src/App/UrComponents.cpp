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

// ---- the pane shell's dynamic GROUPS and rows (R4) -------------------------
//
// Additive to the R3 block above. Each of these is the App.xaml style of the
// same name applied in code, so a group built here is byte-for-byte the group
// Home declares in markup - which is the whole reason they are here rather than
// hand-built at each of the four Wave-2 call sites.

namespace {

// A double out of the app dictionary (the pane metrics live there so every pane
// is one construction), with a fallback for a renamed key.
double MetricByKey(wchar_t const* key, double fallback) {
  auto app = Application::Current();
  if (!app) return fallback;
  auto boxed = winrt::box_value(winrt::hstring{key});
  if (!app.Resources().HasKey(boxed)) return fallback;
  return winrt::unbox_value_or<double>(app.Resources().Lookup(boxed), fallback);
}

// The title/note column shared by the two-line row and its Button twin. Trimmed,
// never wrapped: a wrapping note would grow the row and break the one thing the
// fixed height is for.
StackPanel MakeTwoLineText(TextBlock& title, TextBlock& note, winrt::hstring const& titleText,
                           winrt::hstring const& noteText) {
  StackPanel text;
  text.Spacing(1);
  text.VerticalAlignment(VerticalAlignment::Center);

  title = TextBlock();
  title.Text(titleText);
  if (auto style = StyleByKey(L"UrRowTitleStyle")) title.Style(style);
  text.Children().Append(title);

  note = TextBlock();
  if (auto style = StyleByKey(L"UrRowNoteStyle")) note.Style(style);
  // SetTextOrCollapse, not Text: a StackPanel spends its Spacing on a child that
  // drew nothing, so an empty note would push the title off centre by a pixel on
  // every row that has none - across a list, a visibly ragged left column.
  SetTextOrCollapse(note, noteText);
  text.Children().Append(note);
  return text;
}

}  // namespace

PaneGroupHeader MakePaneGroupHeader(winrt::hstring const& title, winrt::hstring const& meta) {
  PaneGroupHeader out;
  out.root = Controls::Border();
  if (auto style = StyleByKey(L"UrGroupHeaderStyle")) {
    out.root.Style(style);
  } else {
    out.root.Height(28);
    out.root.Background(urnw::colors::SheetBrush());
    out.root.Padding(ThicknessHelper::FromLengths(12, 0, 12, 0));
  }

  Controls::Grid grid;
  grid.ColumnSpacing(8);
  Controls::ColumnDefinition titleColumn, metaColumn, actionColumn;
  titleColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  metaColumn.Width(GridLengthHelper::Auto());
  actionColumn.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(titleColumn);
  grid.ColumnDefinitions().Append(metaColumn);
  grid.ColumnDefinitions().Append(actionColumn);

  out.title = TextBlock();
  out.title.Text(title);
  if (auto style = StyleByKey(L"UrGroupHeaderTextStyle")) out.title.Style(style);
  grid.Children().Append(out.title);

  out.meta = TextBlock();
  if (auto style = StyleByKey(L"UrPaneMetaStyle")) out.meta.Style(style);
  SetTextOrCollapse(out.meta, meta);
  Controls::Grid::SetColumn(out.meta, 1);
  grid.Children().Append(out.meta);

  out.trailing = Controls::Grid();
  out.trailing.VerticalAlignment(VerticalAlignment::Center);
  Controls::Grid::SetColumn(out.trailing, 2);
  grid.Children().Append(out.trailing);

  // A group header is a heading, not a list item: without this a screen reader
  // reads twelve section names as twelve unrelated strings between the rows.
  Automation::AutomationProperties::SetHeadingLevel(out.title,
                                                    Automation::Peers::AutomationHeadingLevel::Level3);

  out.root.Child(grid);
  return out;
}

PaneTwoLineRow MakePaneTwoLineRow(winrt::hstring const& title, winrt::hstring const& note,
                                  double height) {
  PaneTwoLineRow out;
  out.root = MakePaneRow(height);

  Controls::Grid grid;
  grid.ColumnSpacing(10);
  Controls::ColumnDefinition textColumn, trailingColumn;
  textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  trailingColumn.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(textColumn);
  grid.ColumnDefinitions().Append(trailingColumn);

  grid.Children().Append(MakeTwoLineText(out.title, out.note, title, note));

  out.trailing = Controls::Grid();
  out.trailing.VerticalAlignment(VerticalAlignment::Center);
  out.trailing.HorizontalAlignment(HorizontalAlignment::Right);
  Controls::Grid::SetColumn(out.trailing, 1);
  grid.Children().Append(out.trailing);

  out.root.Child(grid);
  return out;
}

PaneTwoLineRowButton MakePaneTwoLineRowButton(winrt::hstring const& title,
                                              winrt::hstring const& note, double height) {
  PaneTwoLineRowButton out;
  out.root = Controls::Button();
  if (auto style = StyleByKey(L"UrPaneRowButtonStyle")) out.root.Style(style);
  out.root.Height(height);
  out.root.MinHeight(height);

  Controls::Grid grid;
  grid.ColumnSpacing(10);
  Controls::ColumnDefinition textColumn, valueColumn, chevronColumn;
  textColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  valueColumn.Width(GridLengthHelper::Auto());
  chevronColumn.Width(GridLengthHelper::Auto());
  grid.ColumnDefinitions().Append(textColumn);
  grid.ColumnDefinitions().Append(valueColumn);
  grid.ColumnDefinitions().Append(chevronColumn);

  grid.Children().Append(MakeTwoLineText(out.title, out.note, title, note));

  out.value = TextBlock();
  if (auto style = StyleByKey(L"UrValueTextStyle")) out.value.Style(style);
  out.value.Foreground(urnw::colors::MutedBrush());
  out.value.MaxWidth(240);
  Controls::Grid::SetColumn(out.value, 1);
  grid.Children().Append(out.value);

  FontIcon chevron;
  chevron.FontFamily(IconFont());
  chevron.Glyph(L"");
  chevron.FontSize(11);
  chevron.Foreground(urnw::colors::MutedBrush());
  chevron.VerticalAlignment(VerticalAlignment::Center);
  Automation::AutomationProperties::SetAccessibilityView(
      chevron, Automation::Peers::AccessibilityView::Raw);
  Controls::Grid::SetColumn(chevron, 2);
  grid.Children().Append(chevron);

  // A Button whose Content is a Panel gets NO automatic automation name. The
  // title is the row's name, and the note and the value are announced as its
  // description rather than as two more list items beside it.
  Automation::AutomationProperties::SetAccessibilityView(
      out.title, Automation::Peers::AccessibilityView::Raw);
  Automation::AutomationProperties::SetAccessibilityView(
      out.note, Automation::Peers::AccessibilityView::Raw);
  Automation::AutomationProperties::SetAccessibilityView(
      out.value, Automation::Peers::AccessibilityView::Raw);
  Automation::AutomationProperties::SetName(out.root, title);
  if (!note.empty()) Automation::AutomationProperties::SetFullDescription(out.root, note);

  out.root.Content(grid);
  return out;
}

PaneTableRow MakePaneTableRow(std::vector<double> const& weights, double height) {
  PaneTableRow out;
  out.root = MakePaneRow(height);

  Controls::Grid grid;
  grid.ColumnSpacing(12);
  for (double weight : weights) {
    Controls::ColumnDefinition column;
    column.Width(GridLengthHelper::FromValueAndType(weight, GridUnitType::Star));
    // A star column with nothing else said will happily go to zero and clip the
    // cell to nothing at narrow widths. A minimum makes the table NARROW instead
    // of vanishing, which is the responsive behaviour a data table wants.
    column.MinWidth(56);
    grid.ColumnDefinitions().Append(column);
  }

  for (size_t index = 0; index < weights.size(); ++index) {
    TextBlock cell;
    if (auto style = StyleByKey(L"UrRowTitleStyle")) cell.Style(style);
    // Cell 0 is the row's subject and reads as text; every later cell is a
    // figure and reads right, which is what makes a column of numbers scannable.
    if (0 < index) {
      if (auto style = StyleByKey(L"UrValueTextStyle")) cell.Style(style);
      cell.Foreground(urnw::colors::MutedBrush());
    }
    Controls::Grid::SetColumn(cell, static_cast<int32_t>(index));
    grid.Children().Append(cell);
    out.cells.push_back(cell);
  }

  out.root.Child(grid);
  return out;
}

Controls::Border MakePaneTableHeader(std::vector<double> const& weights,
                                     std::vector<winrt::hstring> const& titles) {
  Controls::Border header;
  if (auto style = StyleByKey(L"UrGroupHeaderStyle")) {
    header.Style(style);
  } else {
    header.Height(28);
    header.Background(urnw::colors::SheetBrush());
    header.Padding(ThicknessHelper::FromLengths(12, 0, 12, 0));
  }

  Controls::Grid grid;
  grid.ColumnSpacing(12);
  for (double weight : weights) {
    Controls::ColumnDefinition column;
    column.Width(GridLengthHelper::FromValueAndType(weight, GridUnitType::Star));
    column.MinWidth(56);
    grid.ColumnDefinitions().Append(column);
  }
  for (size_t index = 0; index < weights.size() && index < titles.size(); ++index) {
    TextBlock cell;
    if (auto style = StyleByKey(L"UrPaneColumnTextStyle")) cell.Style(style);
    cell.Text(titles[index]);
    if (0 < index) {
      cell.HorizontalAlignment(HorizontalAlignment::Right);
      cell.TextAlignment(TextAlignment::Right);
    }
    Controls::Grid::SetColumn(cell, static_cast<int32_t>(index));
    grid.Children().Append(cell);
  }
  header.Child(grid);
  return header;
}

FrameworkElement MakePaneEmptyLine(winrt::hstring const& text) {
  TextBlock line;
  line.Text(text);
  if (auto style = StyleByKey(L"UrSupportingTextStyle")) line.Style(style);
  line.Foreground(urnw::colors::FaintBrush());
  line.TextAlignment(TextAlignment::Center);
  line.TextWrapping(TextWrapping::Wrap);
  line.MaxWidth(320);
  line.HorizontalAlignment(HorizontalAlignment::Center);
  line.VerticalAlignment(VerticalAlignment::Center);
  line.Margin(ThicknessHelper::FromLengths(16, 16, 16, 16));
  // No glyph and no card. In a pane layout "nothing here" is ONE line in the
  // middle of a full-height column - MakeEmptyStateCard's surface would put a
  // rounded island back inside a pane, which is the thing the model deleted.
  return line;
}

PaneSearchRow MakePaneSearchRow(winrt::hstring const& placeholder) {
  PaneSearchRow out;
  out.root = MakePaneRow(MetricByKey(L"UrPaneHeaderHeight", 40));
  out.root.Background(urnw::colors::BackgroundBrush());

  Controls::Grid grid;
  grid.ColumnSpacing(8);
  Controls::ColumnDefinition iconColumn, boxColumn;
  iconColumn.Width(GridLengthHelper::Auto());
  boxColumn.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  grid.ColumnDefinitions().Append(iconColumn);
  grid.ColumnDefinitions().Append(boxColumn);

  FontIcon glyph;
  glyph.FontFamily(IconFont());
  glyph.Glyph(L"");  // Search
  glyph.FontSize(13);
  glyph.Foreground(urnw::colors::MutedBrush());
  glyph.VerticalAlignment(VerticalAlignment::Center);
  Automation::AutomationProperties::SetAccessibilityView(
      glyph, Automation::Peers::AccessibilityView::Raw);
  grid.Children().Append(glyph);

  out.box = Controls::TextBox();
  if (auto style = StyleByKey(L"UrPaneSearchStyle")) out.box.Style(style);
  out.box.PlaceholderText(placeholder);
  // A TextBox's placeholder is NOT its accessible name; without this the field
  // reaches a screen reader as an unnamed edit box.
  Automation::AutomationProperties::SetName(out.box, placeholder);
  Controls::Grid::SetColumn(out.box, 1);
  grid.Children().Append(out.box);

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
