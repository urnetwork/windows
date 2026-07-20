// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "StatsSheets.h"

#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>  // RichTextBlock chip-flow inlines
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

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
namespace documents = winrt::Microsoft::UI::Xaml::Documents;  // chip-flow inlines

constexpr winrt::Windows::UI::Color kTransparent{0, 0, 0, 0};
// contract stack transitions (macOS ContractStackView / ContractBlock parity)
constexpr double kRingSlot = 56.0;                 // fixed circle slot + block height
constexpr double kMinDiameter = 16.0;              // smallest outer ring
constexpr double kStreamRingGap = 4.0;             // stream contract: 2nd ring sits this far radially outside the main ring; applied to the diameter doubled (4px radial = 8px diameter delta) (Apple streamRingGap)
constexpr double kDiscEaseSeconds = 0.5;           // outer-ring + inner-disc size ease
constexpr double kSlideOffSeconds = 0.4;           // leaver slide-out + fade (Apple slideDuration)
constexpr double kFadeInSeconds = 0.35;            // arrival drop-in fade
constexpr double kOffscreen = kRingSlot * 4.0;     // slide distance to clear the row
constexpr double kEnterDropPx = 10.0;              // arrival slides down from this offset

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

// cubic ease-in-out, matching SwiftUI's .easeInOut used for the ring eject slide
// and fade-in (ContractRing)
double EaseInOutCubic(double progress) {
  progress = std::clamp(progress, 0.0, 1.0);
  return progress < 0.5 ? 4 * progress * progress * progress
                        : 1 - std::pow(-2 * progress + 2, 3) / 2;
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

// Core WinUI has no WrapPanel, so host/ip chips flow inline in a RichTextBlock,
// which wraps to the next line when the next chip would overflow the column --
// the "chip flow" the mobile apps get from SwiftUI ChipFlowLayout / Compose
// FlowRow. AppendChip adds a MakeChip() border; each chip carries a right +
// bottom margin for the inter-chip and wrap-line gaps.
RichTextBlock MakeChipFlow() {
  RichTextBlock flow;
  flow.TextWrapping(TextWrapping::Wrap);
  flow.IsTextSelectionEnabled(false);  // the row owns Tapped; no text caret/selection
  flow.Blocks().Append(documents::Paragraph());
  return flow;
}

void AppendChip(RichTextBlock const& flow, FrameworkElement const& chip) {
  chip.Margin(Thickness{0, 0, 6, 6});
  documents::InlineUIContainer container;
  container.Child(chip);
  flow.Blocks().GetAt(0).as<documents::Paragraph>().Inlines().Append(container);
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

namespace anim = winrt::Microsoft::UI::Xaml::Media::Animation;

// a transition collection that animates a panel's children gliding to new
// layout positions -- the stack "settling" when a circle leaves, and the rows
// re-ordering on an activity resort or a "N new" merge
anim::TransitionCollection RepositionTransitions() {
  anim::TransitionCollection trans;
  trans.Append(anim::RepositionThemeTransition());
  return trans;
}

std::shared_ptr<ClientContractsSheet> ClientContractsSheet::Create(XamlRoot const& root,
                                                                   SdkHost& sdk,
                                                                   ContractDetailsMode mode) {
  auto sheet = std::shared_ptr<ClientContractsSheet>(new ClientContractsSheet(sdk, mode));
  sheet->Build(root);
  return sheet;
}

void ClientContractsSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, Loc(mode_ == ContractDetailsMode::Client ? "client_contracts"
                                                                      : "provider_contracts"));

  list_ = StackPanel();
  list_.ChildrenTransitions(RepositionTransitions());  // animate row resort / merge
  scroll_ = MakeSheetScroll(list_);
  {
    std::weak_ptr<ClientContractsSheet> weak = weak_from_this();
    scroll_.ViewChanged([weak](IInspectable const&, auto const&) {
      if (auto self = weak.lock()) self->OnScrollViewChanged();
    });
  }

  empty_ = StackPanel();
  empty_.Spacing(8);
  empty_.Padding(Thickness{0, 48, 0, 48});
  empty_.HorizontalAlignment(HorizontalAlignment::Center);
  auto emptyTitle = MakeText(Loc("no_open_contracts"), 14, MutedBrush());
  emptyTitle.HorizontalAlignment(HorizontalAlignment::Center);
  auto emptyBody = MakeText(Loc(mode_ == ContractDetailsMode::Client
                                    ? "contracts_appear_connected"
                                    : "contracts_appear_providing"),
                            12, FaintBrush());
  emptyBody.HorizontalAlignment(HorizontalAlignment::Center);
  emptyBody.TextAlignment(TextAlignment::Center);
  empty_.Children().Append(emptyTitle);
  empty_.Children().Append(emptyBody);

  copiedNote_ = MakeText(L"", 11, MutedBrush());
  copiedNote_.Visibility(Visibility::Collapsed);
  copiedNote_.Margin(Thickness{0, 8, 0, 0});

  // the "N new" chip: floats over the top of the list while scrolled away, and
  // collects newly prepended rows; tapping it merges + resorts + scrolls to top
  chip_ = Button();
  chip_.Visibility(Visibility::Collapsed);
  chip_.HorizontalAlignment(HorizontalAlignment::Center);
  chip_.VerticalAlignment(VerticalAlignment::Top);
  chip_.Margin(Thickness{0, 8, 0, 0});
  chip_.Padding(Thickness{10, 6, 10, 6});
  chip_.Background(colors::AccentBrush());
  chip_.BorderThickness(Thickness{0, 0, 0, 0});
  chip_.CornerRadius(CornerRadius{12, 12, 12, 12});
  {
    StackPanel chipContent;
    chipContent.Orientation(Orientation::Horizontal);
    chipContent.Spacing(4);
    auto up = MakeText(L"↑", 10, SolidColorBrush(colors::kInverseText));
    up.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    up.VerticalAlignment(VerticalAlignment::Center);
    chipContent.Children().Append(up);
    chipText_ = MakeText(L"", 12, SolidColorBrush(colors::kInverseText));
    chipText_.FontFamily(FontFamily(L"Consolas"));
    chipText_.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
    chipContent.Children().Append(chipText_);
    chip_.Content(chipContent);
  }
  {
    std::weak_ptr<ClientContractsSheet> weak = weak_from_this();
    chip_.Click([weak](IInspectable const&, RoutedEventArgs const&) {
      if (auto self = weak.lock()) self->ScrollToTop();
    });
  }

  StackPanel body;
  body.MinWidth(440);
  body.Children().Append(scroll_);
  body.Children().Append(empty_);
  body.Children().Append(copiedNote_);

  // overlay the chip on top of the list (single-cell grid)
  Grid overlay;
  overlay.Children().Append(body);
  overlay.Children().Append(chip_);
  dialog_.Content(overlay);

  // a fresh dialog opens scrolled to the top; report it so the VC merges +
  // re-sorts to the at-top order (macOS ContractDetailsView.onAppear setAtTop parity)
  sdk_.SetContractsAtTop(atTop_);
}

ClientContractsSheet::RowUi ClientContractsSheet::BuildRow(const std::string& clientId) {
  RowUi ui;
  ui.root = StackPanel();
  ui.root.Padding(Thickness{0, 16, 0, 16});
  ui.root.Spacing(16);

  // the full client id, tap to copy
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
  ui.root.Children().Append(idText);

  // two top-anchored stacks laid out as four columns mirrored around the row
  // center: send stats | send circles | receive circles | receive stats
  Grid stacks;
  {
    ColumnDefinition c0, c1;
    c0.Width(GridLength{1, GridUnitType::Star});
    c1.Width(GridLength{1, GridUnitType::Star});
    stacks.ColumnDefinitions().Append(c0);
    stacks.ColumnDefinitions().Append(c1);
    stacks.ColumnSpacing(20);
  }
  ui.send = BuildStack(/*mirrored*/ true, Loc("send"), colors::kUrGreen);
  ui.send.removalLeading = true;  // send circles slide off toward the left edge
  Grid::SetColumn(ui.send.root, 0);
  stacks.Children().Append(ui.send.root);

  ui.receive = BuildStack(/*mirrored*/ false, Loc("receive"), colors::kUrPink);
  ui.receive.removalLeading = false;  // receive circles slide off toward the right edge
  Grid::SetColumn(ui.receive.root, 1);
  stacks.Children().Append(ui.receive.root);

  ui.root.Children().Append(stacks);

  // separator under the row
  Border separator;
  separator.Height(1);
  separator.Background(colors::BorderBrush());
  ui.root.Children().Append(separator);
  return ui;
}

ClientContractsSheet::StackUi ClientContractsSheet::BuildStack(bool mirrored, hstring const& title,
                                                               winrt::Windows::UI::Color color) {
  StackUi stack;
  stack.color = color;
  stack.mirrored = mirrored;
  const auto side = mirrored ? HorizontalAlignment::Right : HorizontalAlignment::Left;

  stack.root = StackPanel();
  stack.root.Spacing(12);
  stack.root.HorizontalAlignment(side);

  // direction header: title, arrow, and the summed bit rate, ordered so the rate
  // always lands against the row center (over its own circle column): send reads
  // "title arrow rate", receive reads "rate arrow title".
  StackPanel header;
  header.Orientation(Orientation::Horizontal);
  header.Spacing(5);
  header.HorizontalAlignment(side);
  auto titleText = MakeText(title, 11, MutedBrush());
  titleText.VerticalAlignment(VerticalAlignment::Center);
  auto arrow = MakeText(mirrored ? L"→" : L"←", 9, SolidColorBrush(color));  // -> / <-
  arrow.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
  arrow.VerticalAlignment(VerticalAlignment::Center);
  stack.rate = MakeText(L" ", 9, SolidColorBrush(color));
  stack.rate.FontFamily(FontFamily(L"Consolas"));
  stack.rate.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  stack.rate.VerticalAlignment(VerticalAlignment::Center);
  stack.rate.Opacity(0);
  if (mirrored) {
    header.Children().Append(titleText);
    header.Children().Append(arrow);
    header.Children().Append(stack.rate);
  } else {
    header.Children().Append(stack.rate);
    header.Children().Append(arrow);
    header.Children().Append(titleText);
  }
  stack.root.Children().Append(header);

  // the pile, newest first; children glide as the stack settles / grows down
  stack.pile = StackPanel();
  stack.pile.Spacing(4);
  stack.pile.HorizontalAlignment(side);
  stack.pile.ChildrenTransitions(RepositionTransitions());
  stack.root.Children().Append(stack.pile);

  // the scale anchor ("max N"): every circle is sized relative to this
  stack.maxLabel = MakeText(L"", 10, FaintBrush());
  stack.maxLabel.FontFamily(FontFamily(L"Consolas"));
  stack.maxLabel.HorizontalAlignment(side);
  stack.maxLabel.Opacity(0);
  stack.root.Children().Append(stack.maxLabel);
  return stack;
}

ClientContractsSheet::BlockUi ClientContractsSheet::BuildBlock(StackUi const& stack,
                                                               const ContractEntry& entry) {
  BlockUi block;
  block.contractId = entry.contractId;
  block.usedByteCount = entry.usedByteCount;
  block.totalByteCount = entry.totalByteCount;
  block.bitRate = entry.bitRate;
  block.diaFrom = kMinDiameter;
  block.diaTo = kMinDiameter;

  // a mirrored block lays out stats-then-circle so the circle column sits against
  // the row center; an unmirrored block is circle-then-stats. A fixed height keeps
  // the stack falling in uniform increments.
  block.root = StackPanel();
  block.root.Orientation(Orientation::Horizontal);
  block.root.Spacing(10);
  block.root.Height(kRingSlot);
  block.root.HorizontalAlignment(stack.mirrored ? HorizontalAlignment::Right
                                                : HorizontalAlignment::Left);
  block.shift = TranslateTransform();
  block.root.RenderTransform(block.shift);

  // the circle: outer total ring + inner used-fraction disc, centered in a slot
  Grid circle;
  circle.Width(kRingSlot);
  circle.Height(kRingSlot);
  // a stream contract (one carrying a stream id) reads as a double concentric
  // ring: a second ring ~2px outside the main outer ring (sized diameter +
  // kStreamRingGap), kept outside the inner disc so it stays visible when full.
  // Appended first so it sits behind the main ring/disc; AnimateStack keeps its
  // diameter and stroke in step with the main ring.
  if (entry.hasStream) {
    block.streamRing = ShapeEllipse();
    block.streamRing.Stroke(SolidColorBrush(colors::WithAlpha(stack.color, 140)));
    block.streamRing.StrokeThickness(1);
    block.streamRing.Width(kMinDiameter + 2 * kStreamRingGap);
    block.streamRing.Height(kMinDiameter + 2 * kStreamRingGap);
    block.streamRing.HorizontalAlignment(HorizontalAlignment::Center);
    block.streamRing.VerticalAlignment(VerticalAlignment::Center);
    circle.Children().Append(block.streamRing);
  }
  block.ring = ShapeEllipse();
  block.ring.Stroke(SolidColorBrush(colors::WithAlpha(stack.color, 140)));
  block.ring.StrokeThickness(1);
  block.ring.Width(kMinDiameter);
  block.ring.Height(kMinDiameter);
  block.ring.HorizontalAlignment(HorizontalAlignment::Center);
  block.ring.VerticalAlignment(VerticalAlignment::Center);
  circle.Children().Append(block.ring);
  block.inner = ShapeEllipse();
  block.inner.Fill(SolidColorBrush(colors::WithAlpha(stack.color, 77)));     // 0.3
  block.inner.Stroke(SolidColorBrush(colors::WithAlpha(stack.color, 153)));  // 0.6
  block.inner.StrokeThickness(0.5);
  block.inner.Width(0);
  block.inner.Height(0);
  block.inner.HorizontalAlignment(HorizontalAlignment::Center);
  block.inner.VerticalAlignment(VerticalAlignment::Center);
  circle.Children().Append(block.inner);

  // used / of-total counts, beside the circle (away from the row center)
  StackPanel stats;
  stats.Spacing(2);
  stats.VerticalAlignment(VerticalAlignment::Center);
  block.used = MakeText(H(FormatByteCountCompact(entry.usedByteCount)), 11);
  block.used.FontFamily(FontFamily(L"Consolas"));
  block.used.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  block.total =
      MakeText(hstring{Format("of_total", Widen(FormatByteCountCompact(entry.totalByteCount)))}, 10,
               MutedBrush());
  block.total.FontFamily(FontFamily(L"Consolas"));
  const auto textAlign = stack.mirrored ? TextAlignment::Right : TextAlignment::Left;
  block.used.TextAlignment(textAlign);
  block.total.TextAlignment(textAlign);
  stats.Children().Append(block.used);
  stats.Children().Append(block.total);

  if (stack.mirrored) {
    block.root.Children().Append(stats);
    block.root.Children().Append(circle);
  } else {
    block.root.Children().Append(circle);
    block.root.Children().Append(stats);
  }
  return block;
}

void ClientContractsSheet::SyncStack(StackUi& stack, const std::vector<ContractEntry>& truth,
                                     int64_t byteCount, double now) {
  // header run total: cumulative bytes moved on this stack since the peer last
  // went idle (sits over this stack's circle column)
  if (0 < byteCount) {
    stack.rate.Text(H(FormatByteCountCompact(byteCount)));
    stack.rate.Opacity(1);
  } else {
    stack.rate.Text(L" ");
    stack.rate.Opacity(0);
  }

  std::unordered_map<std::string, const ContractEntry*> truthById;
  for (const auto& e : truth) truthById.emplace(e.contractId, &e);

  // 1. surviving blocks track their values live; a departed block starts leaving;
  //    a leaving block that reappears before it is gone is re-admitted
  std::unordered_set<std::string> onScreen;
  for (auto& block : stack.blocks) {
    auto it = truthById.find(block.contractId);
    if (it != truthById.end()) {
      onScreen.insert(block.contractId);
      const ContractEntry& e = *it->second;
      block.usedByteCount = e.usedByteCount;
      block.totalByteCount = e.totalByteCount;
      block.bitRate = e.bitRate;
      block.used.Text(H(FormatByteCountCompact(e.usedByteCount)));
      block.total.Text(
          hstring{Format("of_total", Widen(FormatByteCountCompact(e.totalByteCount)))});
      if (block.leaving) {  // reappeared: cancel the slide-off
        block.leaving = false;
        block.animStart = 0;
        block.shift.X(0);
        block.root.Opacity(1);
      }
    } else if (!block.leaving) {
      block.leaving = true;
      block.animStart = now;
    }
  }

  // 2. arrivals: truth ids with no block yet, admitted newest-on-top. truth is
  //    newest first, so inserting each missing id at the top in reverse order
  //    lands the newest contract at the very top of the pile.
  for (auto it = truth.rbegin(); it != truth.rend(); ++it) {
    if (onScreen.count(it->contractId)) continue;
    BlockUi block = BuildBlock(stack, *it);
    block.entering = true;
    block.animStart = now;
    block.root.Opacity(0);
    stack.pile.Children().InsertAt(0, block.root);
    stack.blocks.insert(stack.blocks.begin(), std::move(block));
  }

  // 3. size everything against the new stack max and advance one frame
  AnimateStack(stack, now);
}

void ClientContractsSheet::AnimateStack(StackUi& stack, double now) {
  // scale reference: the largest total on screen (leavers included, so survivors
  // rescale as the pile settles rather than mid-slide)
  int64_t stackMax = 0;
  for (const auto& b : stack.blocks) stackMax = (std::max)(stackMax, b.totalByteCount);

  const double dir = stack.removalLeading ? -1.0 : 1.0;
  for (auto it = stack.blocks.begin(); it != stack.blocks.end();) {
    BlockUi& block = *it;

    // target outer diameter: area-proportional to the stack max
    double diaTarget = kMinDiameter;
    if (0 < stackMax && 0 < block.totalByteCount) {
      double d = kRingSlot * std::sqrt(static_cast<double>(block.totalByteCount) /
                                       static_cast<double>(stackMax));
      diaTarget = (std::max)(kMinDiameter, (std::min)(kRingSlot, d));
    }
    if (diaTarget != block.diaTo) {  // re-ease from wherever the diameter is now
      double p =
          block.diaStart <= 0 ? 1.0 : EaseOutCubic((now - block.diaStart) / kDiscEaseSeconds);
      block.diaFrom = block.diaFrom + (block.diaTo - block.diaFrom) * p;
      block.diaTo = diaTarget;
      block.diaStart = now;
    }
    double diaP =
        block.diaStart <= 0 ? 1.0 : EaseOutCubic((now - block.diaStart) / kDiscEaseSeconds);
    double dia = block.diaFrom + (block.diaTo - block.diaFrom) * diaP;
    block.ring.Width(dia);
    block.ring.Height(dia);

    // inner disc: area-proportional to the used fraction of this contract
    double fraction = 0 < block.totalByteCount
                          ? (std::min)(1.0, static_cast<double>(block.usedByteCount) /
                                                static_cast<double>(block.totalByteCount))
                          : 0;
    double innerTarget = 0 < fraction ? (std::max)(4.0, dia * std::sqrt(fraction)) : 0;
    if (innerTarget != block.innerTo) {
      double p =
          block.innerStart <= 0 ? 1.0 : EaseOutCubic((now - block.innerStart) / kDiscEaseSeconds);
      block.innerFrom = block.innerFrom + (block.innerTo - block.innerFrom) * p;
      block.innerTo = innerTarget;
      block.innerStart = now;
    }
    double innerP =
        block.innerStart <= 0 ? 1.0 : EaseOutCubic((now - block.innerStart) / kDiscEaseSeconds);
    double inner = block.innerFrom + (block.innerTo - block.innerFrom) * innerP;
    block.inner.Width((std::max)(0.0, inner));
    block.inner.Height((std::max)(0.0, inner));

    // a contract moving bytes brightens its ring
    const bool active = 0 < block.bitRate;
    block.ring.Stroke(SolidColorBrush(colors::WithAlpha(stack.color, active ? 255 : 140)));
    block.ring.StrokeThickness(active ? 1.5 : 1);

    // a stream contract's second ring tracks the main ring: same color + width,
    // sized a 4px radial gap outside it (8px diameter delta; the double concentric ring)
    if (block.streamRing) {
      block.streamRing.Width(dia + 2 * kStreamRingGap);
      block.streamRing.Height(dia + 2 * kStreamRingGap);
      block.streamRing.Stroke(SolidColorBrush(colors::WithAlpha(stack.color, active ? 255 : 140)));
      block.streamRing.StrokeThickness(active ? 1.5 : 1);
    }

    if (block.leaving) {
      double p = EaseInOutCubic((now - block.animStart) / kSlideOffSeconds);
      block.shift.X(dir * kOffscreen * p);
      block.root.Opacity(1.0 - p);
      if (1.0 <= p) {
        uint32_t index = 0;
        if (stack.pile.Children().IndexOf(block.root, index)) {
          // the survivors below fall into the space via the pile's reposition
          // transition
          stack.pile.Children().RemoveAt(index);
        }
        it = stack.blocks.erase(it);
        continue;
      }
    } else if (block.entering) {
      double p = EaseInOutCubic((now - block.animStart) / kFadeInSeconds);
      block.root.Opacity(p);
      block.shift.Y((1.0 - p) * -kEnterDropPx);  // drops down into place
      if (1.0 <= p) {
        block.entering = false;
        block.root.Opacity(1);
        block.shift.Y(0);
      }
    }
    ++it;
  }

  // the scale anchor
  if (stack.blocks.empty()) {
    stack.maxLabel.Opacity(0);
  } else {
    stack.maxLabel.Text(
        hstring{Format("contract_stack_max", Widen(FormatByteCountCompact(stackMax)))});
    stack.maxLabel.Opacity(1);
  }
}

void ClientContractsSheet::Update(const std::vector<ContractPeerRow>& rows) {
  // the SDK view controller hands back the FINAL, already-ordered rows (the
  // at-top activity sort and the scrolled-away freeze both happen inside it)
  rows_ = rows;
  const bool empty = rows.empty();
  empty_.Visibility(empty ? Visibility::Visible : Visibility::Collapsed);
  scroll_.Visibility(empty ? Visibility::Collapsed : Visibility::Visible);
  RenderList();
  UpdateChip();
}

void ClientContractsSheet::Tick() {
  const double now = NowSeconds();
  for (auto& [id, ui] : rowUis_) {
    AnimateStack(ui.send, now);
    AnimateStack(ui.receive, now);
  }
}

void ClientContractsSheet::OnScrollViewChanged() {
  const bool atTop = scroll_.VerticalOffset() <= 4.0;
  if (atTop != atTop_) {
    atTop_ = atTop;
    // report scroll to the SDK VC: at the top it re-sorts active-above-idle and
    // merges pending rows; scrolled away it freezes membership + order and
    // collects new rows into the pending count. A rows-changed push follows when
    // the ordering actually changed.
    sdk_.SetContractsAtTop(atTop);
  }
  UpdateChip();
}

void ClientContractsSheet::RenderList() {
  // render the SDK's rows in order as-is; the sheet holds no ordering state
  std::vector<std::string> ids;
  ids.reserve(rows_.size());
  for (const auto& row : rows_) ids.push_back(row.clientId);

  // build any newly shown peer
  for (const auto& id : ids)
    if (!rowUis_.count(id)) rowUis_.emplace(id, BuildRow(id));

  // drop rows no longer shown (their circles already slid off when the SDK
  // emptied the stacks; now the row itself leaves)
  for (auto it = rowUis_.begin(); it != rowUis_.end();) {
    if (std::find(ids.begin(), ids.end(), it->first) == ids.end()) {
      uint32_t index = 0;
      if (list_.Children().IndexOf(it->second.root, index)) list_.Children().RemoveAt(index);
      it = rowUis_.erase(it);
    } else {
      ++it;
    }
  }

  // reconcile the child order to `ids` by moving in place, so a resort / merge
  // glides the rows via the list's RepositionThemeTransition
  for (uint32_t i = 0; i < ids.size(); ++i) {
    auto root = rowUis_.at(ids[i]).root;
    uint32_t cur = 0;
    if (list_.Children().IndexOf(root, cur)) {
      if (cur != i) {
        list_.Children().RemoveAt(cur);
        list_.Children().InsertAt(i, root);
      }
    } else {
      list_.Children().InsertAt(i, root);
    }
  }
  renderedIds_ = ids;

  // update every shown row's stacks
  const double now = NowSeconds();
  for (const auto& row : rows_) {
    auto it = rowUis_.find(row.clientId);
    if (it == rowUis_.end()) continue;
    SyncStack(it->second.send, row.send, row.sendByteCount, now);
    SyncStack(it->second.receive, row.receive, row.receiveByteCount, now);
  }
}

void ClientContractsSheet::UpdateChip() {
  // the SDK owns the "N new" count (rows that arrived while scrolled away)
  const int64_t pending = sdk_.ContractsPendingCount();
  if (!atTop_ && 0 < pending) {
    chipText_.Text(hstring{Plural("new_items_count", pending)});
    chip_.Visibility(Visibility::Visible);
  } else {
    chip_.Visibility(Visibility::Collapsed);
  }
}

void ClientContractsSheet::ScrollToTop() {
  // chip tap: report we're back at the top (the VC merges + re-sorts and resets
  // the pending count), then scroll -- ViewChanged then refreshes the chip
  sdk_.SetContractsAtTop(true);
  scroll_.ChangeView(nullptr, winrt::Windows::Foundation::IReference<double>{0.0}, nullptr);
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

    // split-rule hosts can mix host names and raw ips; the whole rule is active,
    // so show its host base names + exact ips as green chips (mirrors iOS
    // SplitRuleRowView / Android SplitRuleRow)
    std::vector<std::string> hostNames, ips;
    for (const auto& value : rule.hosts) {
      (IsIpAddressValue(value) ? ips : hostNames).push_back(value);
    }
    RichTextBlock flow = MakeChipFlow();
    for (const auto& name : urnet::collapseHostNames(hostNames)) {
      AppendChip(flow, MakeChip(H(name), colors::kUrGreen, true));
    }
    for (const auto& ip : ips) {
      AppendChip(flow, MakeChip(H(ip), colors::kUrGreen, true));
    }
    Grid::SetColumn(flow, 0);
    row.Children().Append(flow);

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

    // host/ip chips (wrap): the exact matched hosts + ips (green), then the
    // remaining hosts collapsed to base names (muted), then a single "X IPs" pill
    // for the remaining ips. Mirrors iOS BlockActionRowView / Android BlockActionRow.
    RichTextBlock flow = MakeChipFlow();
    for (const auto& name : action.matchedHosts) {
      AppendChip(flow, MakeChip(H(name), colors::kUrGreen, true));
    }
    for (const auto& ip : action.matchedIps) {
      AppendChip(flow, MakeChip(H(ip), colors::kUrGreen, true));
    }
    for (const auto& name : urnet::collapseHostNames(action.hosts)) {
      AppendChip(flow, MakeChip(H(name), colors::kTextMuted, false));
    }
    if (!action.ips.empty()) {
      AppendChip(flow,
                 MakeChip(hstring{Plural("ip_count", static_cast<int64_t>(action.ips.size()))},
                          colors::kTextMuted, false));
    }
    text.Children().Append(flow);

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
  // every host value (matched + unmatched), host names first, then ips (iOS
  // BlockActionItem.hostValues) so the editor's checklist sees everything
  std::vector<std::string> hostValues = action.matchedHosts;
  hostValues.insert(hostValues.end(), action.hosts.begin(), action.hosts.end());
  hostValues.insert(hostValues.end(), action.matchedIps.begin(), action.matchedIps.end());
  hostValues.insert(hostValues.end(), action.ips.begin(), action.ips.end());
  if (rule) {
    OpenEditor(rule->overrideId, OrderedUnion(rule->hosts, hostValues),
               std::set<std::string>(rule->hosts.begin(), rule->hosts.end()));
  } else {
    // create a rule from the action's host values, all initially UNSELECTED: the
    // common case is picking one or a few server names, so pre-selecting
    // everything just makes the user uncheck the rest (iOS/Android parity)
    OpenEditor("", hostValues, std::set<std::string>{});
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

// ---- Per-app split tunnel --------------------------------------------------

std::shared_ptr<AppRulesSheet> AppRulesSheet::Create(XamlRoot const& root, SdkHost& sdk) {
  auto sheet = std::shared_ptr<AppRulesSheet>(new AppRulesSheet(sdk));
  sheet->Build(root);
  return sheet;
}

void AppRulesSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, hstring{L"App split tunnel"});
  installed_ = EnumerateInstalledApps();

  StackPanel body;
  body.Spacing(8);

  Border banner;
  banner.CornerRadius(CornerRadius{12, 12, 12, 12});
  banner.Padding(Thickness{12, 12, 12, 12});
  banner.Background(colors::CardBrush());
  banner.Child(MakeText(
      hstring{L"Choose which apps use the VPN. \"Include\" routes an app through the "
              L"tunnel; \"Bypass\" sends it direct. If any app is included, only "
              L"included apps use the tunnel."},
      12, MutedBrush(), true));
  body.Children().Append(banner);

  appsList_ = StackPanel();
  appsList_.Spacing(2);
  body.Children().Append(appsList_);

  dialog_.Content(MakeSheetScroll(body));
  RenderList();
}

void AppRulesSheet::RenderList() {
  appsList_.Children().Clear();

  auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
  };

  // current per-app rules from the SDK, keyed by lowercased image path
  std::vector<AppRule> rules = sdk_.CurrentAppRules();
  std::unordered_map<std::string, bool> ruleFor;  // lower(path) -> includeInTunnel
  for (const auto& r : rules) ruleFor[lower(r.imagePath)] = r.includeInTunnel;

  // rows = enumerated apps UNION any ruled app not enumerated, so an existing rule
  // stays visible + changeable even if the app wasn't found in the registry.
  std::vector<InstalledApp> rows = installed_;
  std::unordered_map<std::string, bool> present;
  for (const auto& a : installed_) present[lower(a.exePath)] = true;
  for (const auto& r : rules) {
    if (present.count(lower(r.imagePath))) continue;
    std::string name = r.imagePath;
    if (auto slash = name.find_last_of("\\/"); slash != std::string::npos)
      name = name.substr(slash + 1);
    rows.push_back({name, r.imagePath});
  }

  if (rows.empty()) {
    appsList_.Children().Append(MakeText(
        hstring{L"No installed apps found. (Store apps aren't listed.)"}, 12,
        FaintBrush(), true));
    return;
  }

  std::weak_ptr<AppRulesSheet> weak = weak_from_this();
  for (const auto& app : rows) {
    Grid row;
    ColumnDefinition c0, c1;
    c0.Width(GridLength{1, GridUnitType::Star});
    c1.Width(GridLength{0, GridUnitType::Auto});
    row.ColumnDefinitions().Append(c0);
    row.ColumnDefinitions().Append(c1);
    row.ColumnSpacing(8);
    row.Padding(Thickness{0, 4, 0, 4});

    StackPanel labels;
    labels.Children().Append(MakeText(H(app.name), 13, nullptr, true));
    labels.Children().Append(MakeText(H(app.exePath), 10, FaintBrush(), true));
    Grid::SetColumn(labels, 0);
    row.Children().Append(labels);

    ComboBox combo;
    combo.MinWidth(130);
    combo.VerticalAlignment(VerticalAlignment::Center);
    combo.Items().Append(box_value(hstring{L"Default"}));         // 0 = no rule
    combo.Items().Append(box_value(hstring{L"Include in VPN"}));  // 1 -> Local=false
    combo.Items().Append(box_value(hstring{L"Bypass VPN"}));      // 2 -> Local=true
    int sel = 0;
    if (auto it = ruleFor.find(lower(app.exePath)); it != ruleFor.end())
      sel = it->second ? 1 : 2;
    combo.SelectedIndex(sel);
    std::string path = app.exePath;
    combo.SelectionChanged(
        [weak, path](IInspectable const& sender, SelectionChangedEventArgs const&) {
          auto self = weak.lock();
          if (!self) return;
          int idx = sender.as<ComboBox>().SelectedIndex();
          if (idx <= 0) self->sdk_.RemoveAppRule(path);
          else self->sdk_.SetAppRule(path, idx == 1);  // 1 = include, 2 = bypass
        });
    Grid::SetColumn(combo, 1);
    row.Children().Append(combo);

    appsList_.Children().Append(row);
  }
}

}  // namespace urnw
