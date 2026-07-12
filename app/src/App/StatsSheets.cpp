// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "StatsSheets.h"

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include <algorithm>
#include <chrono>

#include "Localization.h"
#include "StatsFormat.h"
#include "Strings.h"  // Widen: the sdk's utf-8 data into the utf-16 ui
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Shapes;

// NOTE on captures: control event handlers capture the owning sheet weakly.
// A strong capture would cycle (sheet -> dialog -> handler -> sheet) and leak
// the dialog tree on every open. The window holds the sheet's shared_ptr while
// the dialog is showing, so lock() always succeeds during interaction.

namespace urnw {
namespace {

// wingdi.h declares ::Ellipse and ::Rectangle functions; alias the XAML shapes
// so unqualified lookup under the using-directives stays unambiguous
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
using ShapeRectangle = winrt::Microsoft::UI::Xaml::Shapes::Rectangle;

constexpr winrt::Windows::UI::Color kTransparent{0, 0, 0, 0};
// contract circle transitions (disc growth + swap fade/slide)
constexpr double kCircleTransitionSeconds = 0.5;

hstring H(std::string const& s) { return winrt::to_hstring(s); }

// A UI string from the shared localization store, by key id (Localization.h).
// Every user-facing string in these sheets comes through Loc, Format or Plural.
hstring Loc(std::string_view key) { return hstring{Localized(key)}; }
IInspectable LocBox(std::string_view key) { return winrt::box_value(Loc(key)); }

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

double NowSeconds() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

double EaseOutCubic(double progress) {
  progress = std::clamp(progress, 0.0, 1.0);
  return 1 - std::pow(1 - progress, 3);
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string ToUpper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return s;
}

std::string TrimWhitespace(std::string const& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

// values of a then b, deduped, order preserved
std::vector<std::string> OrderedUnion(const std::vector<std::string>& a,
                                      const std::vector<std::string>& b) {
  std::set<std::string> seen;
  std::vector<std::string> values;
  for (const auto* list : {&a, &b}) {
    for (const auto& value : *list) {
      if (seen.insert(value).second) values.push_back(value);
    }
  }
  return values;
}

// brand theme brushes (UrColors.h)
Brush MutedBrush() { return colors::MutedBrush(); }
Brush FaintBrush() { return colors::FaintBrush(); }
Brush DangerBrush() { return colors::DangerBrush(); }

std::optional<Style> AccentButtonStyle() {
  auto resources = Application::Current().Resources();
  auto key = winrt::box_value(hstring(L"AccentButtonStyle"));
  if (resources.HasKey(key)) {
    if (auto style = resources.Lookup(key).try_as<Style>()) return style;
  }
  return std::nullopt;
}

TextBlock MakeText(hstring const& text, double fontSize, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock tb;
  tb.Text(text);
  tb.FontSize(fontSize);
  if (brush) tb.Foreground(brush);
  if (wrap) tb.TextWrapping(TextWrapping::Wrap);
  return tb;
}

TextBlock SectionHeader(hstring const& text) {
  auto tb = MakeText(text, 12, MutedBrush());
  tb.Margin(Thickness{0, 8, 0, 0});
  return tb;
}

// "AABBCC" / "#AABBCC" / "AARRGGBB" -> Color (fallback muted gray)
winrt::Windows::UI::Color ColorFromHex(std::string hex) {
  if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
  auto parse = [&](size_t offset) {
    return static_cast<uint8_t>(std::stoul(hex.substr(offset, 2), nullptr, 16));
  };
  try {
    if (hex.size() == 6) return {255, parse(0), parse(2), parse(4)};
    if (hex.size() == 8) return {parse(0), parse(2), parse(4), parse(6)};
  } catch (...) {
  }
  return colors::kTextMuted;
}

ShapeEllipse MakeDot(winrt::Windows::UI::Color color, double size) {
  ShapeEllipse dot;
  dot.Width(size);
  dot.Height(size);
  dot.Fill(SolidColorBrush(color));
  dot.VerticalAlignment(VerticalAlignment::Center);
  return dot;
}

// A small capsule state chip; highlighted fills with the color, idle tints it.
Border MakeChip(hstring const& text, winrt::Windows::UI::Color color, bool highlighted) {
  Border chip;
  chip.CornerRadius(CornerRadius{9, 9, 9, 9});
  chip.Padding(Thickness{7, 3, 7, 3});
  chip.VerticalAlignment(VerticalAlignment::Center);
  chip.Background(SolidColorBrush(highlighted ? color : colors::WithAlpha(color, 36)));
  TextBlock label = MakeText(text, 10);
  label.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  label.Foreground(SolidColorBrush(highlighted ? colors::kInverseText : color));
  chip.Child(label);
  return chip;
}

Button MakeSubtleButton(hstring const& text) {
  Button button;
  button.Content(winrt::box_value(text));
  button.Background(SolidColorBrush(kTransparent));
  button.BorderThickness(Thickness{0, 0, 0, 0});
  return button;
}

ContentDialog MakeDialog(XamlRoot const& root, hstring const& title) {
  ContentDialog dialog;
  dialog.XamlRoot(root);
  dialog.Title(winrt::box_value(title));
  dialog.CloseButtonText(Loc("close"));
  // brand sheet surface (macOS sheet background)
  dialog.Background(colors::BackgroundBrush());
  return dialog;
}

ScrollViewer MakeSheetScroll(UIElement const& content) {
  ScrollViewer scroll;
  scroll.Content(content);
  scroll.MaxHeight(520);
  scroll.MinWidth(440);
  return scroll;
}

Grid MakeStarAutoRow() {
  Grid row;
  ColumnDefinition c0, c1;
  c0.Width(GridLength{1, GridUnitType::Star});
  c1.Width(GridLength{0, GridUnitType::Auto});
  row.ColumnDefinitions().Append(c0);
  row.ColumnDefinitions().Append(c1);
  return row;
}

ToggleSwitch MakeBareToggle() {
  ToggleSwitch toggle;
  toggle.OnContent(winrt::box_value(hstring(L"")));
  toggle.OffContent(winrt::box_value(hstring(L"")));
  toggle.MinWidth(0);
  toggle.HorizontalAlignment(HorizontalAlignment::Right);
  toggle.VerticalAlignment(VerticalAlignment::Center);
  return toggle;
}

}  // namespace

// ---- ClientContractsSheet --------------------------------------------------

std::shared_ptr<ClientContractsSheet> ClientContractsSheet::Create(XamlRoot const& root) {
  auto sheet = std::shared_ptr<ClientContractsSheet>(new ClientContractsSheet());
  sheet->Build(root);
  return sheet;
}

void ClientContractsSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, Loc("client_contracts"));

  list_ = StackPanel();
  scroll_ = MakeSheetScroll(list_);

  empty_ = StackPanel();
  empty_.Spacing(8);
  empty_.Padding(Thickness{0, 48, 0, 48});
  empty_.HorizontalAlignment(HorizontalAlignment::Center);
  auto emptyTitle = MakeText(Loc("no_open_contracts"), 14, MutedBrush());
  emptyTitle.HorizontalAlignment(HorizontalAlignment::Center);
  auto emptyBody = MakeText(Loc("contracts_appear_connected"), 12, FaintBrush());
  emptyBody.HorizontalAlignment(HorizontalAlignment::Center);
  empty_.Children().Append(emptyTitle);
  empty_.Children().Append(emptyBody);

  copiedNote_ = MakeText(L"", 11, MutedBrush());
  copiedNote_.Visibility(Visibility::Collapsed);
  copiedNote_.Margin(Thickness{0, 8, 0, 0});

  StackPanel content;
  content.MinWidth(440);
  content.Children().Append(scroll_);
  content.Children().Append(empty_);
  content.Children().Append(copiedNote_);
  dialog_.Content(content);
}

ClientContractsSheet::RowUi ClientContractsSheet::BuildRow(const std::string& clientId) {
  RowUi ui;
  ui.root = StackPanel();
  ui.root.Padding(Thickness{0, 16, 0, 0});
  ui.root.Spacing(16);

  // header: the full client id (tap to copy) + the pair count
  Grid header = MakeStarAutoRow();
  TextBlock idText = MakeText(H(clientId), 13, nullptr, true);
  idText.FontFamily(FontFamily(L"Consolas"));
  idText.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  {
    std::weak_ptr<ClientContractsSheet> weak = weak_from_this();
    std::string copyId = clientId;
    idText.Tapped([weak, copyId](IInspectable const&, auto const&) {
      if (auto self = weak.lock()) self->CopyClientId(copyId);
    });
  }
  Grid::SetColumn(idText, 0);
  header.Children().Append(idText);
  ui.pairCount = MakeText(L"", 12, MutedBrush());
  ui.pairCount.Visibility(Visibility::Collapsed);
  Grid::SetColumn(ui.pairCount, 1);
  header.Children().Append(ui.pairCount);
  ui.root.Children().Append(header);

  // viz: contract circle | transfer lines | companion circle
  Grid viz;
  {
    ColumnDefinition v0, v1, v2;
    v0.Width(GridLength{0, GridUnitType::Auto});
    v1.Width(GridLength{1, GridUnitType::Star});
    v2.Width(GridLength{0, GridUnitType::Auto});
    viz.ColumnDefinitions().Append(v0);
    viz.ColumnDefinitions().Append(v1);
    viz.ColumnDefinitions().Append(v2);
  }

  auto buildCircle = [](winrt::Windows::UI::Color color, hstring const& label,
                        CircleUi& circle) {
    StackPanel panel;
    panel.Width(92);
    panel.Spacing(8);
    circle.ring = Grid();
    circle.ring.Width(56);
    circle.ring.Height(56);
    circle.ring.HorizontalAlignment(HorizontalAlignment::Center);
    // swap transition: the replaced circle slides/fades back in
    circle.ringShift = TranslateTransform();
    circle.ring.RenderTransform(circle.ringShift);
    ShapeEllipse outer;
    outer.Width(56);
    outer.Height(56);
    outer.Stroke(SolidColorBrush(colors::WithAlpha(color, 204)));  // 0.8
    outer.StrokeThickness(1);
    circle.ring.Children().Append(outer);
    // area-proportional inner disc; grows as the contract is used
    circle.inner = ShapeEllipse();
    circle.inner.Width(0);
    circle.inner.Height(0);
    circle.inner.HorizontalAlignment(HorizontalAlignment::Center);
    circle.inner.VerticalAlignment(VerticalAlignment::Center);
    circle.inner.Fill(SolidColorBrush(colors::WithAlpha(color, 77)));     // 0.3
    circle.inner.Stroke(SolidColorBrush(colors::WithAlpha(color, 153)));  // 0.6
    circle.inner.StrokeThickness(0.5);
    circle.ring.Children().Append(circle.inner);
    panel.Children().Append(circle.ring);

    StackPanel labels;
    labels.Spacing(2);
    labels.HorizontalAlignment(HorizontalAlignment::Center);
    circle.used = MakeText(L"", 11);
    circle.used.FontFamily(FontFamily(L"Consolas"));
    circle.used.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
    circle.used.HorizontalAlignment(HorizontalAlignment::Center);
    circle.total = MakeText(L"", 10, MutedBrush());
    circle.total.FontFamily(FontFamily(L"Consolas"));
    circle.total.HorizontalAlignment(HorizontalAlignment::Center);
    auto caption = MakeText(label, 10, FaintBrush());
    caption.HorizontalAlignment(HorizontalAlignment::Center);
    labels.Children().Append(circle.used);
    labels.Children().Append(circle.total);
    labels.Children().Append(caption);
    panel.Children().Append(labels);
    return panel;
  };

  auto buildLine = [](winrt::Windows::UI::Color color, bool pointsRight, LineUi& line) {
    StackPanel panel;
    panel.Spacing(3);
    line.rate = MakeText(L" ", 9, SolidColorBrush(color));
    line.rate.FontFamily(FontFamily(L"Consolas"));
    line.rate.HorizontalAlignment(HorizontalAlignment::Center);
    panel.Children().Append(line.rate);

    line.line = MakeStarAutoRow();
    // rebuild the columns as [Auto |*| Auto] with the arrow head on one side
    line.line.ColumnDefinitions().Clear();
    ColumnDefinition l0, l1, l2;
    l0.Width(GridLength{0, GridUnitType::Auto});
    l1.Width(GridLength{1, GridUnitType::Star});
    l2.Width(GridLength{0, GridUnitType::Auto});
    line.line.ColumnDefinitions().Append(l0);
    line.line.ColumnDefinitions().Append(l1);
    line.line.ColumnDefinitions().Append(l2);
    if (!pointsRight) {
      auto head = MakeText(L"◀", 7, SolidColorBrush(color));
      head.VerticalAlignment(VerticalAlignment::Center);
      Grid::SetColumn(head, 0);
      line.line.Children().Append(head);
    }
    ShapeRectangle bar;
    bar.Height(1);
    bar.Fill(SolidColorBrush(color));
    bar.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(bar, 1);
    line.line.Children().Append(bar);
    if (pointsRight) {
      auto head = MakeText(L"▶", 7, SolidColorBrush(color));
      head.VerticalAlignment(VerticalAlignment::Center);
      Grid::SetColumn(head, 2);
      line.line.Children().Append(head);
    }
    line.line.Opacity(0.25);
    panel.Children().Append(line.line);
    return panel;
  };

  auto contractPanel = buildCircle(colors::kUrGreen, Loc("contract"), ui.contract);
  ui.contract.swapFromLeft = true;  // replaced contract slides in from the left edge
  Grid::SetColumn(contractPanel, 0);
  viz.Children().Append(contractPanel);

  StackPanel lines;
  lines.Spacing(12);
  lines.Margin(Thickness{8, 10, 8, 0});
  lines.VerticalAlignment(VerticalAlignment::Top);
  // top line: contract transfer, points right (green); bottom: companion, left
  lines.Children().Append(buildLine(colors::kUrGreen, true, ui.contractLine));
  lines.Children().Append(buildLine(colors::kUrPink, false, ui.companionLine));
  Grid::SetColumn(lines, 1);
  viz.Children().Append(lines);

  auto companionPanel = buildCircle(colors::kUrPink, Loc("companion"), ui.companion);
  Grid::SetColumn(companionPanel, 2);
  viz.Children().Append(companionPanel);

  ui.root.Children().Append(viz);

  // separator under the row
  Border separator;
  separator.Height(1);
  separator.Margin(Thickness{0, 16, 0, 0});
  separator.Background(colors::BorderBrush());
  ui.root.Children().Append(separator);
  return ui;
}

void ClientContractsSheet::UpdateRow(RowUi& ui, const ContractClientRow& row) {
  const double now = NowSeconds();
  if (1 < row.pairCount) {
    ui.pairCount.Text(hstring{Format("contract_count", row.pairCount)});
    ui.pairCount.Visibility(Visibility::Visible);
  } else {
    ui.pairCount.Visibility(Visibility::Collapsed);
  }

  auto updateCircle = [now](CircleUi& circle, int64_t used, int64_t total,
                            const std::string& signature) {
    // inner disc area proportional to the used fraction, minimum visible size
    const double fraction =
        0 < total ? (std::min)(1.0, static_cast<double>(used) / static_cast<double>(total))
                  : 0;
    const double target = 0 < fraction ? (std::max)(6.0, 56.0 * std::sqrt(fraction)) : 0;
    if (target != circle.sizeTo) {
      // ease the disc toward its new size from wherever it currently is
      const double progress = circle.sizeStart <= 0
                                  ? 1.0
                                  : EaseOutCubic((now - circle.sizeStart) /
                                                 kCircleTransitionSeconds);
      circle.sizeFrom = circle.sizeFrom + (circle.sizeTo - circle.sizeFrom) * progress;
      circle.sizeTo = target;
      circle.sizeStart = now;
    }
    // a changed contract id signature means the contract was replaced: fade and
    // slide the circle back in rather than snapping (macOS swap parity)
    if (signature != circle.signature) {
      if (!circle.signature.empty() && !signature.empty()) circle.swapStart = now;
      circle.signature = signature;
    }
    circle.used.Text(H(FormatByteCountCompact(used)));
    circle.total.Text(hstring{Format("of_total", Widen(FormatByteCountCompact(total)))});
    AnimateCircle(circle, now);
  };
  updateCircle(ui.contract, row.contractUsedByteCount, row.contractByteCount, row.contractId);
  updateCircle(ui.companion, row.companionContractUsedByteCount,
               row.companionContractByteCount, row.companionContractId);

  auto updateLine = [](LineUi& line, int64_t bitRate) {
    if (0 < bitRate) {
      line.rate.Text(H(FormatBitRate(bitRate)));
      line.rate.Opacity(1);
      line.line.Opacity(0.9);
    } else {
      line.rate.Text(L" ");
      line.rate.Opacity(0);
      line.line.Opacity(0.25);
    }
  };
  updateLine(ui.contractLine, row.contractBitRate);
  updateLine(ui.companionLine, row.companionContractBitRate);
}

void ClientContractsSheet::AnimateCircle(CircleUi& circle, double now) {
  // eased inner disc size
  const double progress = circle.sizeStart <= 0
                              ? 1.0
                              : EaseOutCubic((now - circle.sizeStart) /
                                             kCircleTransitionSeconds);
  const double size = circle.sizeFrom + (circle.sizeTo - circle.sizeFrom) * progress;
  circle.inner.Width((std::max)(0.0, size));
  circle.inner.Height((std::max)(0.0, size));
  // swap fade/slide
  if (0 < circle.swapStart) {
    const double swap = EaseOutCubic((now - circle.swapStart) / kCircleTransitionSeconds);
    circle.ring.Opacity(0.2 + 0.8 * swap);
    circle.ringShift.X((circle.swapFromLeft ? -10.0 : 10.0) * (1 - swap));
    if (1.0 <= swap) circle.swapStart = 0;
  }
}

void ClientContractsSheet::Tick() {
  const double now = NowSeconds();
  for (auto& [id, ui] : rowUis_) {
    AnimateCircle(ui.contract, now);
    AnimateCircle(ui.companion, now);
  }
}

void ClientContractsSheet::Update(const std::vector<ContractClientRow>& rows) {
  empty_.Visibility(rows.empty() ? Visibility::Visible : Visibility::Collapsed);
  scroll_.Visibility(rows.empty() ? Visibility::Collapsed : Visibility::Visible);

  std::vector<std::string> ids;
  ids.reserve(rows.size());
  for (const auto& row : rows) ids.push_back(row.clientId);

  if (ids != renderedIds_) {
    // membership or order changed: re-append in order, reusing built rows so
    // in-place data updates don't reset the scroll position every second
    list_.Children().Clear();
    std::unordered_map<std::string, RowUi> kept;
    for (const auto& row : rows) {
      auto it = rowUis_.find(row.clientId);
      RowUi ui = it != rowUis_.end() ? it->second : BuildRow(row.clientId);
      list_.Children().Append(ui.root);
      kept[row.clientId] = ui;
    }
    rowUis_ = std::move(kept);
    renderedIds_ = ids;
  }
  for (const auto& row : rows) {
    auto it = rowUis_.find(row.clientId);
    if (it != rowUis_.end()) UpdateRow(it->second, row);
  }
}

void ClientContractsSheet::CopyClientId(const std::string& clientId) {
  winrt::Windows::ApplicationModel::DataTransfer::DataPackage package;
  package.SetText(H(clientId));
  winrt::Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(package);
  // label, then the id that was copied (data, not prose)
  copiedNote_.Text(hstring{Localized("client_id_copied") + L": " + Widen(clientId)});
  copiedNote_.Visibility(Visibility::Visible);
}

// ---- SplitRulesSheet -------------------------------------------------------

std::shared_ptr<SplitRulesSheet> SplitRulesSheet::Create(XamlRoot const& root, SdkHost& sdk) {
  auto sheet = std::shared_ptr<SplitRulesSheet>(new SplitRulesSheet(sdk));
  sheet->Build(root);
  return sheet;
}

void SplitRulesSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, Loc("split_rules"));
  std::weak_ptr<SplitRulesSheet> weak = weak_from_this();

  // ---- list page ----
  StackPanel listBody;
  listBody.Spacing(8);

  // info banner: how exclusions work
  Border banner;
  banner.CornerRadius(CornerRadius{12, 12, 12, 12});
  banner.Padding(Thickness{12, 12, 12, 12});
  banner.Background(colors::CardBrush());
  banner.Child(MakeText(Loc("split_rules_info_note"), 12, MutedBrush(), true));
  listBody.Children().Append(banner);

  listBody.Children().Append(SectionHeader(Loc("rules")));
  rulesList_ = StackPanel();
  rulesList_.Spacing(4);
  listBody.Children().Append(rulesList_);

  Grid activityHeader = MakeStarAutoRow();
  auto activityTitle = SectionHeader(Loc("activity"));
  Grid::SetColumn(activityTitle, 0);
  activityHeader.Children().Append(activityTitle);
  countsText_ = MakeText(L"", 11, FaintBrush());
  countsText_.FontFamily(FontFamily(L"Consolas"));
  countsText_.VerticalAlignment(VerticalAlignment::Bottom);
  Grid::SetColumn(countsText_, 1);
  activityHeader.Children().Append(countsText_);
  listBody.Children().Append(activityHeader);

  activityList_ = StackPanel();
  activityList_.Spacing(4);
  listBody.Children().Append(activityList_);

  listPage_ = MakeSheetScroll(listBody);

  // ---- editor page ----
  editorPage_ = StackPanel();
  editorPage_.Spacing(12);
  editorPage_.MinWidth(400);
  editorPage_.Visibility(Visibility::Collapsed);
  editorPage_.Children().Append(MakeText(Loc("split_rule_description"), 12, MutedBrush(), true));

  checklist_ = StackPanel();
  checklist_.Spacing(2);
  ScrollViewer checklistScroll;
  checklistScroll.Content(checklist_);
  checklistScroll.MaxHeight(320);
  editorPage_.Children().Append(checklistScroll);

  applyButton_ = Button();
  applyButton_.HorizontalAlignment(HorizontalAlignment::Stretch);
  if (auto style = AccentButtonStyle()) applyButton_.Style(*style);
  applyButton_.Click([weak](IInspectable const&, RoutedEventArgs const&) {
    if (auto self = weak.lock()) self->ApplyEditor();
  });
  editorPage_.Children().Append(applyButton_);

  removeButton_ = MakeSubtleButton(Loc("remove_rule"));
  removeButton_.Foreground(DangerBrush());
  removeButton_.HorizontalAlignment(HorizontalAlignment::Center);
  removeButton_.Click([weak](IInspectable const&, RoutedEventArgs const&) {
    auto self = weak.lock();
    if (!self) return;
    if (!self->editRuleId_.empty()) self->sdk_.RemoveSplitRule(self->editRuleId_);
    self->ShowList();
  });
  editorPage_.Children().Append(removeButton_);

  Button backButton = MakeSubtleButton(Loc("back"));
  backButton.HorizontalAlignment(HorizontalAlignment::Center);
  backButton.Click([weak](IInspectable const&, RoutedEventArgs const&) {
    if (auto self = weak.lock()) self->ShowList();
  });
  editorPage_.Children().Append(backButton);

  // both pages live in one dialog (only one ContentDialog can be open at a
  // time, so the rule editor swaps in place instead of stacking a sheet)
  Grid pages;
  pages.Children().Append(listPage_);
  pages.Children().Append(editorPage_);
  dialog_.Content(pages);

  RenderRules();
  RenderActivity();
}

void SplitRulesSheet::Update(std::vector<SplitRule> rules, std::vector<BlockActionItem> actions,
                             int64_t allowed, int64_t blocked) {
  const bool rulesChanged = rules != rules_;
  const bool actionsChanged = actions != actions_;
  rules_ = std::move(rules);
  actions_ = std::move(actions);
  allowed_ = allowed;
  blocked_ = blocked;
  if (0 < allowed_ || 0 < blocked_) {
    countsText_.Text(hstring{Format("allowed_blocked_counts", allowed_, blocked_)});
  } else {
    countsText_.Text(L"");
  }
  if (rulesChanged) RenderRules();
  if (actionsChanged) RenderActivity();
}

void SplitRulesSheet::RenderRules() {
  rulesList_.Children().Clear();
  if (rules_.empty()) {
    rulesList_.Children().Append(MakeText(Loc("split_rules_hint"), 12, FaintBrush(), true));
    return;
  }
  std::weak_ptr<SplitRulesSheet> weak = weak_from_this();
  for (const auto& rule : rules_) {
    Grid row;
    ColumnDefinition c0, c1, c2;
    c0.Width(GridLength{1, GridUnitType::Star});
    c1.Width(GridLength{0, GridUnitType::Auto});
    c2.Width(GridLength{0, GridUnitType::Auto});
    row.ColumnDefinitions().Append(c0);
    row.ColumnDefinitions().Append(c1);
    row.ColumnDefinitions().Append(c2);
    row.ColumnSpacing(8);
    row.Padding(Thickness{0, 4, 0, 4});
    row.Background(SolidColorBrush(kTransparent));  // hit-testable for Tapped

    // split-rule hosts can mix host names and raw ips; cluster them like a row
    std::vector<std::string> hostNames, ips;
    for (const auto& value : rule.hosts) {
      (IsIpAddressValue(value) ? ips : hostNames).push_back(value);
    }
    StackPanel text;
    text.Spacing(2);
    text.Children().Append(
        MakeText(H(FormatHostClusterText(hostNames, ips)), 13, nullptr, true));
    // CLDR plural from the store; never inflect the count here
    text.Children().Append(
        MakeText(hstring{Plural("host_count", static_cast<int64_t>(rule.hosts.size()))}, 11,
                 FaintBrush()));
    Grid::SetColumn(text, 0);
    row.Children().Append(text);

    auto chip = MakeChip(Loc("local"), colors::kUrGreen, true);
    Grid::SetColumn(chip, 1);
    row.Children().Append(chip);

    Button remove = MakeSubtleButton(L"✕");
    remove.FontSize(11);
    remove.Padding(Thickness{6, 2, 6, 2});
    remove.VerticalAlignment(VerticalAlignment::Center);
    {
      std::string ruleId = rule.overrideId;
      remove.Click([weak, ruleId](IInspectable const&, RoutedEventArgs const&) {
        if (auto self = weak.lock()) self->sdk_.RemoveSplitRule(ruleId);
      });
    }
    Grid::SetColumn(remove, 2);
    row.Children().Append(remove);

    {
      SplitRule tapped = rule;
      row.Tapped([weak, tapped](IInspectable const&, auto const&) {
        if (auto self = weak.lock()) self->OpenEditorForRule(tapped);
      });
    }
    rulesList_.Children().Append(row);
  }
}

void SplitRulesSheet::RenderActivity() {
  activityList_.Children().Clear();
  actionTimeLabels_.clear();
  if (actions_.empty()) {
    activityList_.Children().Append(
        MakeText(Loc("split_rules_activity_hint"), 12, FaintBrush(), true));
    return;
  }
  const int64_t now = NowMillis();
  std::weak_ptr<SplitRulesSheet> weak = weak_from_this();
  for (const auto& action : actions_) {
    Grid row = MakeStarAutoRow();
    row.ColumnSpacing(8);
    row.Padding(Thickness{0, 4, 0, 4});
    row.Background(SolidColorBrush(kTransparent));

    StackPanel text;
    text.Spacing(2);
    text.Children().Append(
        MakeText(H(FormatHostClusterText(action.hosts, action.ips)), 13, nullptr, true));
    StackPanel caption;
    caption.Orientation(Orientation::Horizontal);
    caption.Spacing(6);
    auto timeLabel = MakeText(H(RelativeTime(action.timeMillis, now)), 11, FaintBrush());
    timeLabel.FontFamily(FontFamily(L"Consolas"));
    caption.Children().Append(timeLabel);
    actionTimeLabels_.emplace_back(action.timeMillis, timeLabel);
    if (0 < action.byteCount) {
      auto bytesLabel = MakeText(H(FormatByteCountCompact(action.byteCount)), 11, FaintBrush());
      bytesLabel.FontFamily(FontFamily(L"Consolas"));
      caption.Children().Append(bytesLabel);
    }
    text.Children().Append(caption);
    Grid::SetColumn(text, 0);
    row.Children().Append(text);

    StackPanel chips;
    chips.Orientation(Orientation::Horizontal);
    chips.Spacing(4);
    chips.VerticalAlignment(VerticalAlignment::Center);
    chips.Children().Append(MakeChip(action.block ? Loc("blocked") : Loc("allowed"),
                                     action.block ? colors::kUrCoral : colors::kTextMuted,
                                     action.hasBlockOverride));
    chips.Children().Append(MakeChip(action.local ? Loc("local") : Loc("remote"),
                                     action.local ? colors::kUrGreen : colors::kTextMuted,
                                     action.hasRouteOverride));
    Grid::SetColumn(chips, 1);
    row.Children().Append(chips);

    {
      BlockActionItem tapped = action;
      row.Tapped([weak, tapped](IInspectable const&, auto const&) {
        if (auto self = weak.lock()) self->OpenEditorForAction(tapped);
      });
    }
    activityList_.Children().Append(row);
  }
}

void SplitRulesSheet::RefreshTimes() {
  const int64_t now = NowMillis();
  for (auto& [timeMillis, label] : actionTimeLabels_) {
    label.Text(H(RelativeTime(timeMillis, now)));
  }
}

void SplitRulesSheet::OpenEditorForRule(const SplitRule& rule) {
  OpenEditor(rule.overrideId, rule.hosts,
             std::set<std::string>(rule.hosts.begin(), rule.hosts.end()));
}

void SplitRulesSheet::OpenEditorForAction(const BlockActionItem& action) {
  // an action decided by a still-existing rule edits that rule
  const SplitRule* rule = nullptr;
  if (!action.overrideId.empty()) {
    for (const auto& r : rules_) {
      if (r.overrideId == action.overrideId) {
        rule = &r;
        break;
      }
    }
  }
  std::vector<std::string> hostValues = action.hosts;
  hostValues.insert(hostValues.end(), action.ips.begin(), action.ips.end());
  if (rule) {
    OpenEditor(rule->overrideId, OrderedUnion(rule->hosts, hostValues),
               std::set<std::string>(rule->hosts.begin(), rule->hosts.end()));
  } else {
    // new rule from the action's host values, host names pre-selected
    const auto& preselect = action.hosts.empty() ? action.ips : action.hosts;
    OpenEditor("", hostValues, std::set<std::string>(preselect.begin(), preselect.end()));
  }
}

void SplitRulesSheet::OpenEditor(std::string ruleId, std::vector<std::string> candidates,
                                 std::set<std::string> selected) {
  editing_ = !ruleId.empty();
  editRuleId_ = std::move(ruleId);
  candidates_ = std::move(candidates);
  selected_ = std::move(selected);

  dialog_.Title(editing_ ? LocBox("edit_split_rule") : LocBox("new_split_rule"));
  applyButton_.Content(editing_ ? LocBox("update") : LocBox("create"));
  removeButton_.Visibility(editing_ ? Visibility::Visible : Visibility::Collapsed);

  checklist_.Children().Clear();
  std::weak_ptr<SplitRulesSheet> weak = weak_from_this();
  auto updateApply = [](std::shared_ptr<SplitRulesSheet> const& self) {
    self->applyButton_.IsEnabled(self->editing_ || !self->selected_.empty());
  };
  for (const auto& candidate : candidates_) {
    CheckBox check;
    check.Content(winrt::box_value(H(candidate)));
    check.IsChecked(selected_.count(candidate) != 0);
    std::string value = candidate;
    check.Checked([weak, value, updateApply](IInspectable const&, RoutedEventArgs const&) {
      if (auto self = weak.lock()) {
        self->selected_.insert(value);
        updateApply(self);
      }
    });
    check.Unchecked([weak, value, updateApply](IInspectable const&, RoutedEventArgs const&) {
      if (auto self = weak.lock()) {
        self->selected_.erase(value);
        updateApply(self);
      }
    });
    checklist_.Children().Append(check);
  }
  applyButton_.IsEnabled(editing_ || !selected_.empty());

  listPage_.Visibility(Visibility::Collapsed);
  editorPage_.Visibility(Visibility::Visible);
}

void SplitRulesSheet::ShowList() {
  dialog_.Title(LocBox("split_rules"));
  editorPage_.Visibility(Visibility::Collapsed);
  listPage_.Visibility(Visibility::Visible);
}

void SplitRulesSheet::ApplyEditor() {
  // keep candidate order for the stored hosts
  std::vector<std::string> hosts;
  for (const auto& candidate : candidates_) {
    if (selected_.count(candidate)) hosts.push_back(candidate);
  }
  if (editing_) {
    sdk_.UpdateSplitRule(editRuleId_, hosts);  // empty selection removes the rule
  } else if (!hosts.empty()) {
    sdk_.CreateSplitRule(hosts);
  }
  ShowList();
}

// ---- DnsEditorSheet --------------------------------------------------------

bool operator==(const DnsEditorSheet::Draft& a, const DnsEditorSheet::Draft& b) {
  return a.enableRemoteDoh == b.enableRemoteDoh && a.enableLocalDoh == b.enableLocalDoh &&
         a.enableRemoteDns == b.enableRemoteDns && a.enableLocalDns == b.enableLocalDns &&
         a.enableFallback == b.enableFallback &&
         a.remoteDohUrlsIpv4 == b.remoteDohUrlsIpv4 &&
         a.remoteDohUrlsIpv6 == b.remoteDohUrlsIpv6 &&
         a.localDohUrlsIpv4 == b.localDohUrlsIpv4 &&
         a.localDohUrlsIpv6 == b.localDohUrlsIpv6 && a.remoteDnsIpv4 == b.remoteDnsIpv4 &&
         a.remoteDnsIpv6 == b.remoteDnsIpv6 && a.localDnsIpv4 == b.localDnsIpv4 &&
         a.localDnsIpv6 == b.localDnsIpv6;
}

DnsEditorSheet::Draft DnsEditorSheet::FromSettings(
    std::optional<urnet::DnsResolverSettings> const& settings) {
  Draft draft;
  if (!settings) return draft;
  draft.enableRemoteDoh = settings->EnableRemoteDoh;
  draft.enableLocalDoh = settings->EnableLocalDoh;
  draft.enableRemoteDns = settings->EnableRemoteDns;
  draft.enableLocalDns = settings->EnableLocalDns;
  draft.enableFallback = settings->EnableFallback;
  auto list = [](std::optional<urnet::StringList> const& values) {
    return values ? *values : std::vector<std::string>{};
  };
  draft.remoteDohUrlsIpv4 = list(settings->RemoteDohUrlsIpv4);
  draft.remoteDohUrlsIpv6 = list(settings->RemoteDohUrlsIpv6);
  draft.localDohUrlsIpv4 = list(settings->LocalDohUrlsIpv4);
  draft.localDohUrlsIpv6 = list(settings->LocalDohUrlsIpv6);
  draft.remoteDnsIpv4 = list(settings->RemoteDnsIpv4);
  draft.remoteDnsIpv6 = list(settings->RemoteDnsIpv6);
  draft.localDnsIpv4 = list(settings->LocalDnsIpv4);
  draft.localDnsIpv6 = list(settings->LocalDnsIpv6);
  return draft;
}

urnet::DnsResolverSettings DnsEditorSheet::ToSettings(const Draft& draft) {
  urnet::DnsResolverSettings settings;
  settings.EnableRemoteDoh = draft.enableRemoteDoh;
  settings.EnableLocalDoh = draft.enableLocalDoh;
  settings.EnableRemoteDns = draft.enableRemoteDns;
  settings.EnableLocalDns = draft.enableLocalDns;
  settings.EnableFallback = draft.enableFallback;
  settings.RemoteDohUrlsIpv4 = draft.remoteDohUrlsIpv4;
  settings.RemoteDohUrlsIpv6 = draft.remoteDohUrlsIpv6;
  settings.LocalDohUrlsIpv4 = draft.localDohUrlsIpv4;
  settings.LocalDohUrlsIpv6 = draft.localDohUrlsIpv6;
  settings.RemoteDnsIpv4 = draft.remoteDnsIpv4;
  settings.RemoteDnsIpv6 = draft.remoteDnsIpv6;
  settings.LocalDnsIpv4 = draft.localDnsIpv4;
  settings.LocalDnsIpv6 = draft.localDnsIpv6;
  return settings;
}

std::shared_ptr<DnsEditorSheet> DnsEditorSheet::Create(
    XamlRoot const& root, SdkHost& sdk,
    std::optional<urnet::DnsResolverSettings> const& current, std::string countryCode,
    std::string countryName) {
  auto sheet = std::shared_ptr<DnsEditorSheet>(new DnsEditorSheet(sdk));
  sheet->countryCode_ = ToLower(std::move(countryCode));
  sheet->countryName_ = std::move(countryName);
  sheet->draft_ = FromSettings(current);
  sheet->original_ = sheet->draft_;
  if (!sheet->countryCode_.empty()) {
    if (auto rec = urnet::getRecommendedDnsResolverSettings(sheet->countryCode_)) {
      sheet->recommendation_ = FromSettings(rec);
    }
  }
  if (auto defaults = urnet::getDefaultDnsResolverSettings()) {
    sheet->defaults_ = FromSettings(defaults);
  }
  sheet->Build(root);
  return sheet;
}

void DnsEditorSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, Loc("custom_dns"));
  dialog_.PrimaryButtonText(Loc("update"));
  dialog_.DefaultButton(ContentDialogButton::Primary);
  dialog_.IsPrimaryButtonEnabled(false);
  {
    std::weak_ptr<DnsEditorSheet> weak = weak_from_this();
    dialog_.PrimaryButtonClick(
        [weak](ContentDialog const&, ContentDialogButtonClickEventArgs const&) {
          // apply the draft together; the dialog closes after this handler
          if (auto self = weak.lock()) self->sdk_.ApplyDnsSettings(ToSettings(self->draft_));
        });
  }

  StackPanel body;
  body.Spacing(12);

  recPanel_ = StackPanel();
  recPanel_.Spacing(12);
  body.Children().Append(recPanel_);

  BuildResolverSection(body);

  // local dns fallback + footer
  body.Children().Append(SectionHeader(Loc("local_dns_fallback")));
  {
    Grid row = MakeStarAutoRow();
    auto label = MakeText(Loc("local_dns_fallback"), 13);
    label.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(label, 0);
    row.Children().Append(label);
    fallbackToggle_ = MakeBareToggle();
    {
      std::weak_ptr<DnsEditorSheet> weak = weak_from_this();
      fallbackToggle_.Toggled([weak](IInspectable const&, RoutedEventArgs const&) {
        auto self = weak.lock();
        if (!self || self->updating_) return;
        self->draft_.enableFallback = self->fallbackToggle_.IsOn();
        self->OnDraftChanged();
      });
    }
    Grid::SetColumn(fallbackToggle_, 1);
    row.Children().Append(fallbackToggle_);
    body.Children().Append(row);
    body.Children().Append(
        MakeText(Loc("local_dns_fallback_description"), 11, FaintBrush(), true));
  }

  BuildSuggestionSection(body);

  // "https://" is the required URL scheme, not prose: the store carries it as a
  // non-translatable literal so this file has none.
  const hstring dohUrl = Loc("doh_url_placeholder");
  const hstring ipAddress = Loc("ip_address");
  BuildListSection(body, Loc("remote_doh_urls"), dohUrl, true, &Draft::remoteDohUrlsIpv4,
                   &Draft::remoteDohUrlsIpv6);
  BuildListSection(body, Loc("local_doh_urls"), dohUrl, true, &Draft::localDohUrlsIpv4,
                   &Draft::localDohUrlsIpv6);
  BuildListSection(body, Loc("remote_dns_servers"), ipAddress, false, &Draft::remoteDnsIpv4,
                   &Draft::remoteDnsIpv6);
  BuildListSection(body, Loc("local_dns_servers"), ipAddress, false, &Draft::localDnsIpv4,
                   &Draft::localDnsIpv6);

  dialog_.Content(MakeSheetScroll(body));
  SyncFromDraft();
}

void DnsEditorSheet::BuildResolverSection(StackPanel const& parent) {
  parent.Children().Append(SectionHeader(Loc("resolvers")));
  std::weak_ptr<DnsEditorSheet> weak = weak_from_this();
  auto makeToggleRow = [&](hstring const& title, hstring const& detail, ToggleSwitch& toggle,
                           bool Draft::*flag) {
    Grid row = MakeStarAutoRow();
    StackPanel label;
    label.Orientation(Orientation::Horizontal);
    label.Spacing(6);
    label.VerticalAlignment(VerticalAlignment::Center);
    label.Children().Append(MakeText(title, 13));
    label.Children().Append(MakeText(detail, 12, MutedBrush()));
    Grid::SetColumn(label, 0);
    row.Children().Append(label);
    toggle = MakeBareToggle();
    ToggleSwitch captured = toggle;
    toggle.Toggled([weak, captured, flag](IInspectable const&, RoutedEventArgs const&) {
      auto self = weak.lock();
      if (!self || self->updating_) return;
      self->draft_.*flag = captured.IsOn();
      self->OnDraftChanged();
    });
    Grid::SetColumn(toggle, 1);
    row.Children().Append(toggle);
    parent.Children().Append(row);
  };
  const hstring doh = Loc("dns_over_https");
  const hstring udns = Loc("unencrypted_dns");
  const hstring remote = Loc("remote_lowercase");
  const hstring local = Loc("local_lowercase");
  makeToggleRow(doh, remote, remoteDohToggle_, &Draft::enableRemoteDoh);
  makeToggleRow(doh, local, localDohToggle_, &Draft::enableLocalDoh);
  makeToggleRow(udns, remote, remoteDnsToggle_, &Draft::enableRemoteDns);
  makeToggleRow(udns, local, localDnsToggle_, &Draft::enableLocalDns);
}

void DnsEditorSheet::BuildSuggestionSection(StackPanel const& parent) {
  auto servers = urnet::getRegionalDnsServers();
  if (!servers || servers->empty()) return;

  // suggestions for the connected country first, then by code, then name
  std::vector<urnet::RegionalDnsServer> sorted = *servers;
  std::sort(sorted.begin(), sorted.end(),
            [this](const urnet::RegionalDnsServer& a, const urnet::RegionalDnsServer& b) {
              const bool aMatch = !countryCode_.empty() && ToLower(a.CountryCode) == countryCode_;
              const bool bMatch = !countryCode_.empty() && ToLower(b.CountryCode) == countryCode_;
              if (aMatch != bMatch) return aMatch;
              if (a.CountryCode != b.CountryCode) return a.CountryCode < b.CountryCode;
              return a.Name < b.Name;
            });

  parent.Children().Append(SectionHeader(Loc("suggested_remote_dns_servers")));
  std::weak_ptr<DnsEditorSheet> weak = weak_from_this();
  for (const auto& server : sorted) {
    Grid row = MakeStarAutoRow();

    StackPanel label;
    label.Orientation(Orientation::Horizontal);
    label.Spacing(10);
    label.VerticalAlignment(VerticalAlignment::Center);
    if (!countryCode_.empty() && ToLower(server.CountryCode) == countryCode_) {
      // suggested for the connected country, marked with its color
      label.Children().Append(MakeDot(ColorFromHex(urnet::getColorHex(countryCode_)), 10));
    }
    StackPanel text;
    text.Spacing(2);
    StackPanel titleRow;
    titleRow.Orientation(Orientation::Horizontal);
    titleRow.Spacing(6);
    titleRow.Children().Append(MakeText(H(server.Name), 13));
    auto code = MakeText(H(ToUpper(server.CountryCode)), 10, FaintBrush());
    code.VerticalAlignment(VerticalAlignment::Center);
    titleRow.Children().Append(code);
    text.Children().Append(titleRow);
    auto ip = MakeText(H(server.Ipv4), 12, MutedBrush());
    ip.FontFamily(FontFamily(L"Consolas"));
    text.Children().Append(ip);
    label.Children().Append(text);
    Grid::SetColumn(label, 0);
    row.Children().Append(label);

    SuggestionUi suggestion;
    suggestion.server = server;
    suggestion.toggle = MakeBareToggle();
    {
      std::string ipv4 = server.Ipv4;
      ToggleSwitch toggle = suggestion.toggle;
      suggestion.toggle.Toggled([weak, ipv4, toggle](IInspectable const&,
                                                     RoutedEventArgs const&) {
        auto self = weak.lock();
        if (!self || self->updating_) return;
        auto& values = self->draft_.remoteDnsIpv4;
        if (toggle.IsOn()) {
          // on adds the server to the remote dns list and enables remote dns
          if (std::find(values.begin(), values.end(), ipv4) == values.end()) {
            values.push_back(ipv4);
          }
          self->draft_.enableRemoteDns = true;
        } else {
          values.erase(std::remove(values.begin(), values.end(), ipv4), values.end());
        }
        self->SyncFromDraft();
      });
    }
    Grid::SetColumn(suggestion.toggle, 1);
    row.Children().Append(suggestion.toggle);
    suggestions_.push_back(suggestion);
    parent.Children().Append(row);
  }
  parent.Children().Append(
      MakeText(Loc("suggested_remote_dns_servers_description"), 11, FaintBrush(), true));
}

void DnsEditorSheet::BuildListSection(StackPanel const& parent, hstring const& title,
                                      hstring const& placeholder, bool doh,
                                      std::vector<std::string> Draft::*ipv4,
                                      std::vector<std::string> Draft::*ipv6) {
  parent.Children().Append(SectionHeader(title));
  AddSublist(parent, Loc("ipv4"), placeholder, doh, ipv4);
  AddSublist(parent, Loc("ipv6"), placeholder, doh, ipv6);
}

size_t DnsEditorSheet::AddSublist(StackPanel const& parent, hstring const& label,
                                  hstring const& placeholder, bool doh,
                                  std::vector<std::string> Draft::*member) {
  auto list = std::make_unique<ListUi>();
  list->member = member;
  list->doh = doh;

  StackPanel host;
  host.Spacing(6);
  host.Children().Append(MakeText(label, 11, FaintBrush()));

  list->rows = StackPanel();
  list->rows.Spacing(4);
  host.Children().Append(list->rows);

  Grid addRow = MakeStarAutoRow();
  addRow.ColumnSpacing(8);
  list->input = TextBox();
  list->input.PlaceholderText(placeholder);
  list->input.FontSize(13);
  Grid::SetColumn(list->input, 0);
  addRow.Children().Append(list->input);
  list->add = Button();
  list->add.Content(LocBox("add"));
  list->add.IsEnabled(false);
  Grid::SetColumn(list->add, 1);
  addRow.Children().Append(list->add);
  host.Children().Append(addRow);

  lists_.push_back(std::move(list));
  // the ListUi lives behind a unique_ptr, so its address stays stable while
  // this sheet (which owns lists_) is alive; guard with the weak sheet ref
  ListUi* ui = lists_.back().get();
  std::weak_ptr<DnsEditorSheet> weak = weak_from_this();
  ui->input.TextChanged([weak, ui](IInspectable const&, TextChangedEventArgs const&) {
    if (auto self = weak.lock()) self->UpdateAddEnabled(*ui);
  });
  ui->input.KeyDown([weak, ui](IInspectable const&,
                               winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& e) {
    if (e.Key() != winrt::Windows::System::VirtualKey::Enter) return;
    if (auto self = weak.lock()) self->AddValue(*ui);
  });
  ui->add.Click([weak, ui](IInspectable const&, RoutedEventArgs const&) {
    if (auto self = weak.lock()) self->AddValue(*ui);
  });

  parent.Children().Append(host);
  return lists_.size() - 1;
}

void DnsEditorSheet::RenderRecommendationPanel() {
  recPanel_.Children().Clear();
  std::weak_ptr<DnsEditorSheet> weak = weak_from_this();

  auto statusRow = [&](hstring const& text, bool showsCountryColor) {
    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.Spacing(10);
    if (showsCountryColor) {
      row.Children().Append(MakeDot(ColorFromHex(urnet::getColorHex(countryCode_)), 14));
    } else {
      auto check = MakeText(L"✔", 13, SolidColorBrush(colors::kUrGreen));
      check.VerticalAlignment(VerticalAlignment::Center);
      row.Children().Append(check);
    }
    auto label = MakeText(text, 13);
    label.VerticalAlignment(VerticalAlignment::Center);
    row.Children().Append(label);
    recPanel_.Children().Append(row);
  };
  auto applyPanel = [&](hstring const& text, hstring const& buttonText,
                        bool showsCountryColor, const Draft& target) {
    recPanel_.Children().Append(MakeText(text, 12, nullptr, true));
    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.Spacing(10);
    if (showsCountryColor) {
      row.Children().Append(MakeDot(ColorFromHex(urnet::getColorHex(countryCode_)), 14));
    }
    Button apply;
    apply.Content(winrt::box_value(buttonText));
    if (auto style = AccentButtonStyle()) apply.Style(*style);
    Draft applied = target;
    apply.Click([weak, applied](IInspectable const&, RoutedEventArgs const&) {
      if (auto self = weak.lock()) {
        self->draft_ = applied;
        self->SyncFromDraft();
      }
    });
    row.Children().Append(apply);
    recPanel_.Children().Append(row);
  };

  std::wstring countryDisplayName = Localized("this_region");
  if (!countryName_.empty()) {
    countryDisplayName = Widen(countryName_);
  } else if (!countryCode_.empty()) {
    countryDisplayName = Widen(ToUpper(countryCode_));
  }

  if (recommendation_) {
    if (draft_ == *recommendation_) {
      statusRow(Loc("dns_using_recommended"), true);
    } else {
      applyPanel(hstring{Format("dns_recommendation_message", countryDisplayName)},
                 Loc("use_recommended_settings"), true, *recommendation_);
    }
  } else if (defaults_) {
    if (draft_ == *defaults_) {
      statusRow(Loc("dns_using_secure"), false);
    } else {
      applyPanel(Loc("dns_restore_secure_message"), Loc("restore_most_secure_settings"), false,
                 *defaults_);
    }
  }
}

void DnsEditorSheet::RenderListRows(ListUi& list) {
  list.rows.Children().Clear();
  std::weak_ptr<DnsEditorSheet> weak = weak_from_this();
  for (const auto& value : draft_.*(list.member)) {
    Grid row = MakeStarAutoRow();
    row.ColumnSpacing(8);
    auto text = MakeText(H(value), 13);
    text.FontFamily(FontFamily(L"Consolas"));
    text.VerticalAlignment(VerticalAlignment::Center);
    text.TextTrimming(TextTrimming::CharacterEllipsis);
    Grid::SetColumn(text, 0);
    row.Children().Append(text);
    Button remove = MakeSubtleButton(L"✕");
    remove.FontSize(11);
    remove.Padding(Thickness{6, 2, 6, 2});
    ListUi* ui = &list;
    std::string removed = value;
    remove.Click([weak, ui, removed](IInspectable const&, RoutedEventArgs const&) {
      auto self = weak.lock();
      if (!self) return;
      auto& values = self->draft_.*(ui->member);
      values.erase(std::remove(values.begin(), values.end(), removed), values.end());
      self->SyncFromDraft();
    });
    Grid::SetColumn(remove, 1);
    row.Children().Append(remove);
    list.rows.Children().Append(row);
  }
}

void DnsEditorSheet::UpdateAddEnabled(ListUi& list) {
  const std::string value = TrimWhitespace(winrt::to_string(list.input.Text()));
  const auto& values = draft_.*(list.member);
  const bool valid = list.doh ? IsValidDohUrl(value) : IsIpAddressValue(value);
  const bool duplicate = std::find(values.begin(), values.end(), value) != values.end();
  list.add.IsEnabled(valid && !duplicate);
}

void DnsEditorSheet::AddValue(ListUi& list) {
  const std::string value = TrimWhitespace(winrt::to_string(list.input.Text()));
  auto& values = draft_.*(list.member);
  const bool valid = list.doh ? IsValidDohUrl(value) : IsIpAddressValue(value);
  if (!valid || std::find(values.begin(), values.end(), value) != values.end()) return;
  values.push_back(value);
  list.input.Text(L"");
  SyncFromDraft();
}

void DnsEditorSheet::SyncFromDraft() {
  // programmatic control updates must not re-enter the Toggled handlers
  updating_ = true;
  remoteDohToggle_.IsOn(draft_.enableRemoteDoh);
  localDohToggle_.IsOn(draft_.enableLocalDoh);
  remoteDnsToggle_.IsOn(draft_.enableRemoteDns);
  localDnsToggle_.IsOn(draft_.enableLocalDns);
  fallbackToggle_.IsOn(draft_.enableFallback);
  for (auto& suggestion : suggestions_) {
    const auto& values = draft_.remoteDnsIpv4;
    suggestion.toggle.IsOn(std::find(values.begin(), values.end(), suggestion.server.Ipv4) !=
                           values.end());
  }
  for (auto& list : lists_) {
    RenderListRows(*list);
    UpdateAddEnabled(*list);
  }
  updating_ = false;
  OnDraftChanged();
}

void DnsEditorSheet::OnDraftChanged() {
  dialog_.IsPrimaryButtonEnabled(!(draft_ == original_));
  RenderRecommendationPanel();
}

}  // namespace urnw
