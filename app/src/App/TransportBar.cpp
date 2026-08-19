// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "TransportBar.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>  // RichTextBlock inline flow (wrapping legend)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>

#include "PageContext.h"  // pages::Adv / pages::Loc
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Shapes;

namespace urnw {
namespace {

// wingdi.h declares ::Ellipse and ::Rectangle functions; alias the XAML shapes
// so unqualified lookup under the using-directives stays unambiguous
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;
using ShapeRectangle = winrt::Microsoft::UI::Xaml::Shapes::Rectangle;
namespace documents = winrt::Microsoft::UI::Xaml::Documents;
namespace automation = winrt::Microsoft::UI::Xaml::Automation;

// the app's general tween: segment resizing, the empty fade (apple: 1s easeInOut)
constexpr double kTweenSeconds = 1.0;
constexpr double kBarHeight = 8.0;
constexpr double kDotSize = 6.0;

// ---- the not-yet-in-store strings ------------------------------------------
// The transport surface is new on every platform at once; its strings were
// added to the apple catalog by hand and reach the shared store with the next
// localization sync. Until the ids land, each call carries the id the store
// should have AND the English it renders (PageContext.h Adv: the store wins the
// moment a key appears). The ids extract mechanically with the other pending
// families:  grep -ohE '"transport_[a-z0-9_]+"' app/src/App/*.cpp | sort -u
// The product names whodis / whodis pump / H3 / H1 / P2P are not translated
// (the store carries such literals as translatable: false), only routed.
hstring TransportText(std::string_view key, const wchar_t* english) {
  return pages::Adv(key, english);
}

double NowSeconds() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// cubic ease-in-out, matching SwiftUI's .easeInOut (the general tween)
double EaseInOutCubic(double progress) {
  progress = std::clamp(progress, 0.0, 1.0);
  return progress < 0.5 ? 4 * progress * progress * progress
                        : 1 - std::pow(-2 * progress + 2, 3) / 2;
}

TextBlock MakeLabel(hstring const& text, double fontSize, Brush const& brush) {
  TextBlock tb;
  tb.Text(text);
  tb.FontSize(fontSize);
  tb.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  if (brush) tb.Foreground(brush);
  tb.VerticalAlignment(VerticalAlignment::Center);
  return tb;
}

ShapeEllipse MakeDot(winrt::Windows::UI::Color color) {
  ShapeEllipse dot;
  dot.Width(kDotSize);
  dot.Height(kDotSize);
  dot.Fill(SolidColorBrush(color));
  dot.VerticalAlignment(VerticalAlignment::Center);
  return dot;
}

// a hollow dot in the transport's color: the color mapping stays legible while
// the transport is idle (the unused footer)
ShapeEllipse MakeHollowDot(winrt::Windows::UI::Color color) {
  ShapeEllipse dot;
  dot.Width(kDotSize);
  dot.Height(kDotSize);
  dot.Stroke(SolidColorBrush(colors::WithAlpha(color, 153)));  // 0.6 alpha
  dot.StrokeThickness(1);
  dot.VerticalAlignment(VerticalAlignment::Center);
  return dot;
}

// Core WinUI has no WrapPanel, so the legend / footer items flow inline in a
// RichTextBlock, which wraps to the next line when the next item would overflow
// the pane -- the same chip flow the sheets use (StatsSheets MakeChipFlow) and
// what apple's FlowRow does for the same rows.
RichTextBlock MakeFlow() {
  RichTextBlock flow;
  flow.TextWrapping(TextWrapping::Wrap);
  flow.IsTextSelectionEnabled(false);  // the whole component is the click target
  flow.Blocks().Append(documents::Paragraph());
  return flow;
}

documents::Paragraph FlowParagraph(RichTextBlock const& flow) {
  return flow.Blocks().GetAt(0).as<documents::Paragraph>();
}

void ClearFlow(RichTextBlock const& flow) { FlowParagraph(flow).Inlines().Clear(); }

void AppendInline(RichTextBlock const& flow, FrameworkElement const& item) {
  // right margin = the inter-item gap, bottom = the wrap-line gap
  item.Margin(Thickness{0, 0, 12, 4});
  documents::InlineUIContainer container;
  container.Child(item);
  FlowParagraph(flow).Inlines().Append(container);
}

// The legend percent. The SDK's whole percents sum to exactly 100, so a used
// sliver can round to 0; label it "<1%" rather than a zero next to a visible
// segment. Numeric formatting only (StatsFormat parity: units are not
// localized either).
hstring PercentText(const TransportShareRow& share) {
  if (share.used && share.percent == 0) return hstring{L"<1%"};
  return hstring{std::format(L"{}%", share.percent)};
}

// One segment of the bar: the rectangle [x0, x1] x [0, height] with the left
// and/or right end rounded (a semi-ellipse of the given radius, capped at half
// the width). The first visible segment takes the left cap and the last visible
// the right cap, so the colored region has the bar's rounded ends whatever the
// segments are, without depending on the host clipping to a CornerRadius.
PathGeometry SegmentGeometry(double x0, double x1, double height, double leftRadius,
                             double rightRadius) {
  const double width = std::max(0.0, x1 - x0);
  const double rl = std::min(leftRadius, width / 2);
  const double rr = std::min(rightRadius, width / 2);
  const float top = 0;
  const float bottom = static_cast<float>(height);
  const float ry = static_cast<float>(height / 2);
  PathGeometry geometry;
  PathFigure figure;
  figure.IsClosed(true);
  figure.IsFilled(true);
  figure.StartPoint(Point{static_cast<float>(x0 + rl), top});
  // top edge to the right cap
  {
    LineSegment line;
    line.Point(Point{static_cast<float>(x1 - rr), top});
    figure.Segments().Append(line);
  }
  // right end: a clockwise half-arc bulging right (12 -> 3 -> 6 o'clock), or a
  // straight drop when square
  if (0 < rr) {
    ArcSegment arc;
    arc.Point(Point{static_cast<float>(x1 - rr), bottom});
    arc.Size(Size{static_cast<float>(rr), ry});
    arc.RotationAngle(0);
    arc.IsLargeArc(false);
    arc.SweepDirection(SweepDirection::Clockwise);
    figure.Segments().Append(arc);
  } else {
    LineSegment line;
    line.Point(Point{static_cast<float>(x1), bottom});
    figure.Segments().Append(line);
  }
  // bottom edge back to the left cap
  {
    LineSegment line;
    line.Point(Point{static_cast<float>(x0 + rl), bottom});
    figure.Segments().Append(line);
  }
  // left end: clockwise again (6 -> 9 -> 12 o'clock) bulging left, or square
  if (0 < rl) {
    ArcSegment arc;
    arc.Point(Point{static_cast<float>(x0 + rl), top});
    arc.Size(Size{static_cast<float>(rl), ry});
    arc.RotationAngle(0);
    arc.IsLargeArc(false);
    arc.SweepDirection(SweepDirection::Clockwise);
    figure.Segments().Append(arc);
  } else {
    LineSegment line;
    line.Point(Point{static_cast<float>(x0), top});
    figure.Segments().Append(line);
  }
  geometry.Figures().Append(figure);
  return geometry;
}

}  // namespace

// ---- the transport vocabulary -----------------------------------------------

hstring TransportName(std::string const& transportType) {
  if (transportType == urnet::TransportTypeH3) return TransportText("transport_h3", L"H3");
  if (transportType == urnet::TransportTypeH1) return TransportText("transport_h1", L"H1");
  if (transportType == urnet::TransportTypeDns) {
    return TransportText("transport_dns", L"whodis");
  }
  if (transportType == urnet::TransportTypeDnsPump) {
    return TransportText("transport_dnspump", L"whodis pump");
  }
  if (transportType == urnet::TransportTypeP2p) return TransportText("transport_p2p", L"P2P");
  if (transportType == urnet::TransportTypeUnknown) {
    return TransportText("transport_queued", L"queued");
  }
  // a newer sdk vocabulary this app does not know: show the id rather than nothing
  return winrt::to_hstring(transportType);
}

hstring TransportDetail(std::string const& transportType) {
  if (transportType == urnet::TransportTypeH3) {
    return TransportText("transport_h3_description",
                         L"Direct over QUIC. Fastest where it is not filtered.");
  }
  if (transportType == urnet::TransportTypeH1) {
    return TransportText("transport_h1_description", L"Direct over TLS. Works on most networks.");
  }
  if (transportType == urnet::TransportTypeDns) {
    return TransportText("transport_dns_description",
             L"Disguised as DNS traffic. For networks that filter direct connections.");
  }
  if (transportType == urnet::TransportTypeDnsPump) {
    return TransportText("transport_dnspump_description",
             L"Disguised as DNS traffic with a constant reply pump. Lowest bandwidth, "
             L"highest availability.");
  }
  return hstring{};
}

winrt::Windows::UI::Color TransportColor(std::string const& transportType) {
  if (transportType == urnet::TransportTypeH3) return colors::kUrGreen;
  if (transportType == urnet::TransportTypeH1) return colors::kUrLightBlue;
  if (transportType == urnet::TransportTypeDns) return colors::kUrPink;
  if (transportType == urnet::TransportTypeDnsPump) return colors::kUrYellow;
  if (transportType == urnet::TransportTypeP2p) return colors::kUrElectricBlue;
  // unknown (queued) and any newer vocabulary: neutral
  return colors::kTextMuted;
}

// ---- TransportBar -----------------------------------------------------------

TransportBar::TransportBar(Grid const& host, std::function<void()> onOpen)
    : onOpen_(std::move(onOpen)) {
  BuildVisuals(host);
}

void TransportBar::BuildVisuals(Grid const& host) {
  textBrush_ = colors::TextBrush();
  mutedBrush_ = colors::MutedBrush();
  faintBrush_ = colors::FaintBrush();

  // The whole component is one Button on the pane-row style (hover fill,
  // hairline, focus visuals) rather than a Border with Tapped, so it keeps the
  // pane rhythm and is reachable from the keyboard like every other row. Its
  // click opens the transport settings editor.
  root_ = Button();
  if (auto app = Application::Current()) {
    auto key = winrt::box_value(hstring{L"UrPaneRowButtonStyle"});
    if (app.Resources().HasKey(key)) {
      root_.Style(app.Resources().Lookup(key).try_as<Style>());
    }
  }
  root_.HorizontalAlignment(HorizontalAlignment::Stretch);
  root_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  root_.Padding(Thickness{12, 8, 12, 8});
  root_.Click([this](IInspectable const&, RoutedEventArgs const&) {
    if (onOpen_) onOpen_();
  });

  StackPanel body;
  body.Spacing(6);
  body.HorizontalAlignment(HorizontalAlignment::Stretch);

  // title row, styled like the chart title, with a disclosure so the nested
  // click target reads as its own control under the chart
  {
    Grid titleRow;
    ColumnDefinition c0, c1;
    c0.Width(GridLength{1, GridUnitType::Star});
    c1.Width(GridLength{0, GridUnitType::Auto});
    titleRow.ColumnDefinitions().Append(c0);
    titleRow.ColumnDefinitions().Append(c1);
    const hstring title = TransportText("transports", L"Transports");
    TextBlock titleLabel = MakeLabel(title, 11, mutedBrush_);
    titleLabel.HorizontalAlignment(HorizontalAlignment::Left);
    Grid::SetColumn(titleLabel, 0);
    titleRow.Children().Append(titleLabel);
    FontIcon chevron;
    chevron.Glyph(L"\uE76C");  // ChevronRight, as the markup rows use
    chevron.FontSize(10);
    chevron.Foreground(faintBrush_);
    chevron.VerticalAlignment(VerticalAlignment::Center);
    chevron.HorizontalAlignment(HorizontalAlignment::Right);
    Grid::SetColumn(chevron, 1);
    titleRow.Children().Append(chevron);
    body.Children().Append(titleRow);
    // A Button whose Content is a panel has NO accessible name; name it with the
    // title it shows, and take the title and the chevron out of the tree so the
    // name is not read twice. The legend stays readable: it is the row's data.
    automation::AutomationProperties::SetName(root_, title);
    automation::AutomationProperties::SetAccessibilityView(
        titleLabel, automation::Peers::AccessibilityView::Raw);
    automation::AutomationProperties::SetAccessibilityView(
        chevron, automation::Peers::AccessibilityView::Raw);
  }

  // the bar: a faint empty track with the animated segment layer on top
  {
    Grid bar;
    bar.Height(kBarHeight);
    bar.HorizontalAlignment(HorizontalAlignment::Stretch);
    Border track;
    track.CornerRadius(CornerRadius{kBarHeight / 2, kBarHeight / 2, kBarHeight / 2,
                                    kBarHeight / 2});
    track.Background(colors::BorderBrush());
    track.HorizontalAlignment(HorizontalAlignment::Stretch);
    track.VerticalAlignment(VerticalAlignment::Stretch);
    bar.Children().Append(track);

    canvas_ = Canvas();
    canvas_.HorizontalAlignment(HorizontalAlignment::Stretch);
    canvas_.VerticalAlignment(VerticalAlignment::Stretch);
    canvas_.IsHitTestVisible(false);
    canvas_.SizeChanged([this](IInspectable const&, SizeChangedEventArgs const& args) {
      RectangleGeometry clip;
      clip.Rect(Rect{0, 0, args.NewSize().Width, args.NewSize().Height});
      canvas_.Clip(clip);
      dirty_ = true;
    });
    bar.Children().Append(canvas_);
    body.Children().Append(bar);
  }

  // legend: the transports with traffic and their share
  legend_ = MakeFlow();
  legend_.Visibility(Visibility::Collapsed);
  body.Children().Append(legend_);

  // unused footer: enabled transports that carried nothing in the window
  unused_ = MakeFlow();
  unused_.Visibility(Visibility::Collapsed);
  body.Children().Append(unused_);

  root_.Content(body);
  host.Children().Append(root_);
}

void TransportBar::EnsureSegments(size_t count) {
  while (segments_.size() < count) {
    // paths stay BELOW the separators in z-order: insert them before the
    // separators already in the canvas
    Path path;
    path.IsHitTestVisible(false);
    canvas_.Children().InsertAt(static_cast<uint32_t>(segments_.size()), path);
    segments_.push_back(path);
    segmentTypes_.emplace_back();

    ShapeRectangle separator;
    separator.Fill(colors::BackgroundBrush());  // the pane color, like apple's card color
    separator.Height(kBarHeight);
    separator.Visibility(Visibility::Collapsed);
    separator.IsHitTestVisible(false);
    canvas_.Children().Append(separator);
    separators_.push_back(separator);
  }
  // recolor whatever the current distribution says each index is
  for (size_t i = 0; i < count && i < distribution_.shares.size(); ++i) {
    const std::string& type = distribution_.shares[i].transportType;
    if (segmentTypes_[i] != type) {
      segmentTypes_[i] = type;
      segments_[i].Fill(SolidColorBrush(TransportColor(type)));
    }
  }
}

std::vector<double> TransportBar::BoundariesAt(double now) const {
  const size_t count = std::max(fromBoundaries_.size(), toBoundaries_.size());
  std::vector<double> values;
  values.reserve(count);
  const double progress =
      tweenStart_ <= 0 ? 1.0 : std::clamp((now - tweenStart_) / kTweenSeconds, 0.0, 1.0);
  const double eased = EaseInOutCubic(progress);
  for (size_t i = 0; i < count; ++i) {
    // vectors of different lengths combine element-wise, missing elements as 0
    const double from = i < fromBoundaries_.size() ? fromBoundaries_[i] : 0.0;
    const double to = i < toBoundaries_.size() ? toBoundaries_[i] : 0.0;
    values.push_back(from + (to - from) * eased);
  }
  return values;
}

bool TransportBar::TweenInFlight(double now) const {
  return 0 < tweenStart_ && now - tweenStart_ < kTweenSeconds;
}

void TransportBar::SetDistribution(const TransportDistributionSnapshot& distribution) {
  const double now = NowSeconds();
  std::vector<double> target;
  target.reserve(distribution.shares.size());
  for (const auto& share : distribution.shares) {
    target.push_back(std::clamp(share.boundary, 0.0, 1.0));
  }

  // The boundary vector: retargeted from wherever the current tween is, so a
  // change mid-tween never jumps. While the window is EMPTY the last boundaries
  // are held (the bar fades out in place, and back in from its last shape),
  // except before any traffic has been seen, when the live all-zero vector is
  // what there is. The first observation lands without easing.
  if (distribution.active || tweenStart_ <= 0) {
    if (tweenStart_ <= 0) {
      fromBoundaries_ = target;
      toBoundaries_ = target;
      tweenStart_ = now - kTweenSeconds;
    } else if (target != toBoundaries_) {
      fromBoundaries_ = BoundariesAt(now);
      toBoundaries_ = target;
      tweenStart_ = now;
    }
  }

  // the empty fade: segments to 0 while inactive, back to 1 with traffic
  const double fadeTarget = distribution.active ? 1.0 : 0.0;
  if (fadeStart_ <= 0) {
    fadeFrom_ = fadeTarget;
    fadeTo_ = fadeTarget;
    fadeStart_ = now - kTweenSeconds;
  } else if (fadeTarget != fadeTo_) {
    const double progress = std::clamp((now - fadeStart_) / kTweenSeconds, 0.0, 1.0);
    fadeFrom_ = fadeFrom_ + (fadeTo_ - fadeFrom_) * EaseInOutCubic(progress);
    fadeTo_ = fadeTarget;
    fadeStart_ = now;
  }

  distribution_ = distribution;
  EnsureSegments(distribution_.shares.size());
  RebuildLegend();
  RebuildUnused();
  dirty_ = true;
}

void TransportBar::Tick() {
  const double width = canvas_.ActualWidth();
  const double height = canvas_.ActualHeight();
  if (width <= 0 || height <= 0) return;

  const double now = NowSeconds();
  const bool fadeInFlight = 0 < fadeStart_ && now - fadeStart_ < kTweenSeconds;
  const bool animating = TweenInFlight(now) || fadeInFlight || dirty_;
  if (!animating && !wasAnimating_) return;
  wasAnimating_ = animating;  // when it just stopped, draw one settling frame
  dirty_ = false;

  Redraw(now, width, height);
}

void TransportBar::Redraw(double now, double width, double height) {
  const std::vector<double> values = BoundariesAt(now);
  EnsureSegments(values.size());

  // the fade, eased like the boundaries
  {
    const double progress =
        fadeStart_ <= 0 ? 1.0 : std::clamp((now - fadeStart_) / kTweenSeconds, 0.0, 1.0);
    canvas_.Opacity(fadeFrom_ + (fadeTo_ - fadeFrom_) * EaseInOutCubic(progress));
  }

  // every segment edge comes from the one interpolated vector: segment i spans
  // [boundary(i-1), boundary(i)] of the width, so the visible segments tile
  // exactly 100% at every frame and a transport entering or leaving grows or
  // shrinks between its neighbours without any neighbour jumping
  struct Span {
    double x0 = 0;
    double x1 = 0;
    bool visible = false;
  };
  std::vector<Span> spans(values.size());
  double start = 0;
  std::optional<size_t> firstVisible;
  std::optional<size_t> lastVisible;
  for (size_t i = 0; i < values.size(); ++i) {
    const double end = width * std::clamp(values[i], 0.0, 1.0);
    spans[i].x0 = start;
    spans[i].x1 = end;
    spans[i].visible = start < end;
    if (spans[i].visible) {
      if (!firstVisible) firstVisible = i;
      lastVisible = i;
    }
    start = std::max(start, end);
  }

  const double radius = height / 2;
  std::optional<double> previousVisibleEnd;
  for (size_t i = 0; i < spans.size(); ++i) {
    Path& path = segments_[i];
    ShapeRectangle& separator = separators_[i];
    if (!spans[i].visible) {
      path.Data(nullptr);
      separator.Visibility(Visibility::Collapsed);
      continue;
    }
    const double segmentWidth = spans[i].x1 - spans[i].x0;
    path.Data(SegmentGeometry(spans[i].x0, spans[i].x1, height,
                              firstVisible && *firstVisible == i ? radius : 0.0,
                              lastVisible && *lastVisible == i ? radius : 0.0));
    // a hairline in the pane color between this segment and the previous
    // visible one. Its width eases in with the narrower of the two so a segment
    // sliding in from zero width does not pop a full separator.
    if (previousVisibleEnd) {
      const double separatorWidth = std::min(1.0, segmentWidth / 4);
      separator.Width(separatorWidth);
      Canvas::SetLeft(separator, *previousVisibleEnd - separatorWidth / 2);
      Canvas::SetTop(separator, 0);
      separator.Visibility(Visibility::Visible);
    } else {
      separator.Visibility(Visibility::Collapsed);
    }
    previousVisibleEnd = spans[i].x1;
  }
  // segments beyond the current vector (a shorter vocabulary than before): hidden
  for (size_t i = spans.size(); i < segments_.size(); ++i) {
    segments_[i].Data(nullptr);
    separators_[i].Visibility(Visibility::Collapsed);
  }
}

void TransportBar::RebuildLegend() {
  std::vector<std::string> key;
  std::vector<const TransportShareRow*> used;
  for (const auto& share : distribution_.shares) {
    if (!share.used) continue;
    key.push_back(share.transportType);
    used.push_back(&share);
  }
  if (used.empty()) {
    legend_.Visibility(Visibility::Collapsed);
    if (!legendKey_.empty()) {
      ClearFlow(legend_);
      legendKey_.clear();
      percentLabels_.clear();
    }
    return;
  }
  legend_.Visibility(Visibility::Visible);
  // the same transports as last time: only the percents move
  if (key == legendKey_ && percentLabels_.size() == used.size()) {
    for (size_t i = 0; i < used.size(); ++i) percentLabels_[i].Text(PercentText(*used[i]));
    return;
  }
  ClearFlow(legend_);
  percentLabels_.clear();
  for (const TransportShareRow* share : used) {
    StackPanel item;
    item.Orientation(Orientation::Horizontal);
    item.Spacing(5);
    item.Children().Append(MakeDot(TransportColor(share->transportType)));
    item.Children().Append(MakeLabel(TransportName(share->transportType), 11, textBrush_));
    TextBlock percent = MakeLabel(PercentText(*share), 11, mutedBrush_);
    percent.FontFamily(FontFamily(L"Consolas"));  // the chart's numeric labels' face
    item.Children().Append(percent);
    percentLabels_.push_back(percent);
    AppendInline(legend_, item);
  }
  legendKey_ = key;
}

void TransportBar::RebuildUnused() {
  std::vector<std::string> key;
  for (const auto& share : distribution_.shares) {
    if (share.enabled && !share.used) key.push_back(share.transportType);
  }
  if (key.empty()) {
    unused_.Visibility(Visibility::Collapsed);
    if (!unusedKey_.empty()) {
      ClearFlow(unused_);
      unusedKey_.clear();
    }
    return;
  }
  unused_.Visibility(Visibility::Visible);
  if (key == unusedKey_) return;
  ClearFlow(unused_);
  AppendInline(unused_, MakeLabel(TransportText("transport_unused", L"unused"), 11, faintBrush_));
  for (const std::string& type : key) {
    StackPanel item;
    item.Orientation(Orientation::Horizontal);
    item.Spacing(5);
    item.Children().Append(MakeHollowDot(TransportColor(type)));
    item.Children().Append(MakeLabel(TransportName(type), 11, faintBrush_));
    AppendInline(unused_, item);
  }
  unusedKey_ = key;
}

}  // namespace urnw
