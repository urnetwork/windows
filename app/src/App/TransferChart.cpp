// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "TransferChart.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "StatsFormat.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Shapes;

namespace urnw {
namespace {

// duration of the newest-bucket lerp and the axis-scale ease
constexpr double kTransitionSeconds = 0.5;
// reserved bands: average stats on top, sliding peak labels top and bottom
constexpr double kStatsBand = 30;
constexpr double kPeakBand = 13;
// the top-right stats average over the last N buckets (~N seconds)
constexpr int kAverageBucketCount = 5;

double NowSeconds() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

hstring H(std::string const& s) { return winrt::to_hstring(s); }

TextBlock MakeLabel(double fontSize, Brush const& brush, bool monospace) {
  TextBlock tb;
  tb.FontSize(fontSize);
  tb.FontWeight(winrt::Windows::UI::Text::FontWeight{500});
  if (brush) tb.Foreground(brush);
  if (monospace) tb.FontFamily(FontFamily(L"Consolas"));
  return tb;
}

}  // namespace

double TransferChart::ValueTransition::ValueAt(double now) const {
  if (start <= 0) return to;
  double progress = std::clamp((now - start) / kTransitionSeconds, 0.0, 1.0);
  double eased = 1 - std::pow(1 - progress, 3);
  return from + (to - from) * eased;
}

bool TransferChart::ValueTransition::InFlight(double now) const {
  return start > 0 && now - start < kTransitionSeconds;
}

TransferChart::TransferChart(Grid const& host, std::wstring title, ThroughputRoute route,
                             winrt::Windows::UI::Color byteColor,
                             winrt::Windows::UI::Color packetColor)
    : route_(route), byteColor_(byteColor), packetColor_(packetColor) {
  BuildVisuals(host, std::move(title));
}

TransferChart::SeriesUi TransferChart::BuildSeries(winrt::Windows::UI::Color color,
                                                   double strokeWidth, bool withFill) {
  SeriesUi series;
  if (withFill) {
    // soft area fill from the curve down to the center axis
    series.fill = Path();
    series.fill.Fill(colors::MakeBrush(colors::WithAlpha(color, 18)));  // 0.07 alpha
    series.fillClip = RectangleGeometry();
    series.fill.Clip(series.fillClip);
    canvas_.Children().Append(series.fill);
  }
  series.stroke = Path();
  series.stroke.Stroke(colors::MakeBrush(colors::WithAlpha(color, 230)));  // 0.9 alpha
  series.stroke.StrokeThickness(strokeWidth);
  series.stroke.StrokeLineJoin(PenLineJoin::Round);
  series.stroke.StrokeStartLineCap(PenLineCap::Round);
  series.stroke.StrokeEndLineCap(PenLineCap::Round);
  series.strokeClip = RectangleGeometry();
  series.stroke.Clip(series.strokeClip);
  canvas_.Children().Append(series.stroke);
  return series;
}

void TransferChart::BuildVisuals(Grid const& host, std::wstring title) {
  textBrush_ = colors::TextBrush();
  mutedBrush_ = colors::MutedBrush();

  // the plot canvas fills the host card; clipped to its own bounds
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
  host.Children().Append(canvas_);

  // fills sit under the strokes; byte series carry the area fill, packets don't
  egressBytes_ = BuildSeries(byteColor_, 1.5, /*withFill=*/true);
  ingressBytes_ = BuildSeries(byteColor_, 1.5, /*withFill=*/true);
  egressPackets_ = BuildSeries(packetColor_, 1.0, /*withFill=*/false);
  ingressPackets_ = BuildSeries(packetColor_, 1.0, /*withFill=*/false);

  // center zero axis, drawn over the series so it always reads consistently
  axis_ = Line();
  axis_.Stroke(colors::BorderBrush());
  axis_.StrokeThickness(1);
  canvas_.Children().Append(axis_);

  // sliding peak byte-rate labels (top: egress, bottom: ingress)
  peakEgressLabel_ = MakeLabel(9, mutedBrush_, true);
  peakEgressLabel_.Visibility(Visibility::Collapsed);
  canvas_.Children().Append(peakEgressLabel_);
  peakIngressLabel_ = MakeLabel(9, mutedBrush_, true);
  peakIngressLabel_.Visibility(Visibility::Collapsed);
  canvas_.Children().Append(peakIngressLabel_);

  // title, top left
  if (!title.empty()) {
    TextBlock titleLabel = MakeLabel(11, mutedBrush_, false);
    titleLabel.Text(hstring(title));
    titleLabel.HorizontalAlignment(HorizontalAlignment::Left);
    titleLabel.VerticalAlignment(VerticalAlignment::Top);
    host.Children().Append(titleLabel);
  }

  // average labels, top right: egress row over ingress row
  StackPanel stats;
  stats.Orientation(Orientation::Vertical);
  stats.Spacing(3);
  stats.HorizontalAlignment(HorizontalAlignment::Right);
  stats.VerticalAlignment(VerticalAlignment::Top);
  auto makeRow = [&](TextBlock& byteAvg, TextBlock& packetAvg, TextBlock& arrow,
                     const wchar_t* arrowGlyph) {
    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.Spacing(5);
    byteAvg = MakeLabel(10, colors::MakeBrush(byteColor_), true);
    packetAvg = MakeLabel(10, colors::MakeBrush(packetColor_), true);
    arrow = MakeLabel(8, mutedBrush_, false);
    arrow.Text(arrowGlyph);
    row.Children().Append(byteAvg);
    row.Children().Append(packetAvg);
    row.Children().Append(arrow);
    stats.Children().Append(row);
  };
  makeRow(egressByteAvg_, egressPacketAvg_, egressArrow_, L"▲");
  makeRow(ingressByteAvg_, ingressPacketAvg_, ingressArrow_, L"▼");
  host.Children().Append(stats);

  UpdateAverageLabels();
}

TransferChart::Sample TransferChart::Lerp(const Sample& a, const Sample& b, double t) {
  auto lerp = [t](int64_t x, int64_t y) {
    return x + static_cast<int64_t>(std::llround(static_cast<double>(y - x) * t));
  };
  Sample s;
  s.egressBytes = lerp(a.egressBytes, b.egressBytes);
  s.ingressBytes = lerp(a.ingressBytes, b.ingressBytes);
  s.egressPackets = lerp(a.egressPackets, b.egressPackets);
  s.ingressPackets = lerp(a.ingressPackets, b.ingressPackets);
  return s;
}

bool TransferChart::IsActive(const Sample& s) {
  return 0 < s.egressBytes || 0 < s.ingressBytes || 0 < s.egressPackets ||
         0 < s.ingressPackets;
}

void TransferChart::UpdateTransition(ValueTransition& t, double target, double now) {
  if (t.start <= 0) {
    // first observation: land on the target without easing
    t = ValueTransition{target, target, now - kTransitionSeconds};
    return;
  }
  if (target != t.to) {
    // ease from wherever the previous transition left off, never jump
    t = ValueTransition{t.ValueAt(now), target, now};
  }
}

void TransferChart::SetPoints(const std::vector<urnet::ThroughputPoint>& points,
                              int64_t windowSeconds) {
  entries_.clear();
  entries_.reserve(points.size());
  for (const auto& point : points) {
    const std::optional<urnet::ThroughputSample>* sample = nullptr;
    switch (route_) {
      case ThroughputRoute::Remote: sample = &point.Remote; break;
      case ThroughputRoute::Local: sample = &point.Local; break;
      case ThroughputRoute::Block: sample = &point.Block; break;
    }
    Entry entry;
    entry.time = static_cast<double>(point.Time) / 1000.0;  // ms -> s
    if (sample && *sample) {
      entry.sample.egressBytes = (*sample)->EgressByteCount;
      entry.sample.ingressBytes = (*sample)->IngressByteCount;
      entry.sample.egressPackets = (*sample)->EgressPacketCount;
      entry.sample.ingressPackets = (*sample)->IngressPacketCount;
    }
    entries_.push_back(entry);
  }
  if (0 < windowSeconds) window_ = static_cast<double>(windowSeconds);
  dirty_ = true;
}

void TransferChart::Tick() {
  const double width = canvas_.ActualWidth();
  const double height = canvas_.ActualHeight();
  if (width <= 0 || height <= 0) return;

  const double now = NowSeconds();

  // the axis scales ease toward the window peaks (floors 1024 bytes / 8 packets)
  int64_t peakBytes = 0, peakPackets = 0;
  for (const auto& e : entries_) {
    peakBytes = (std::max)(peakBytes, (std::max)(e.sample.egressBytes, e.sample.ingressBytes));
    peakPackets =
        (std::max)(peakPackets, (std::max)(e.sample.egressPackets, e.sample.ingressPackets));
  }
  UpdateTransition(byteScale_, (std::max)(static_cast<double>(peakBytes), 1024.0), now);
  UpdateTransition(packetScale_, (std::max)(static_cast<double>(peakPackets), 8.0), now);

  // redraw only while something is actually moving: recent traffic still
  // scrolling through the window, a scale ease in flight, or new data / resize
  bool hasRecentActivity = false;
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (IsActive(it->sample)) {
      hasRecentActivity = now - it->time < window_;
      break;
    }
  }
  const bool animating = hasRecentActivity || byteScale_.InFlight(now) ||
                         packetScale_.InFlight(now) || dirty_;
  if (!animating && !wasAnimating_) return;
  wasAnimating_ = animating;  // when it just stopped, draw one settling frame
  dirty_ = false;

  Redraw(now, width, height);
  UpdateAverageLabels();
}

PathGeometry TransferChart::SmoothGeometry(std::vector<Point> const& points, bool closeToAxis,
                                           double axisY) {
  PathGeometry geometry;
  if (points.size() < 2) return geometry;
  PathFigure figure;
  figure.StartPoint(points[0]);
  const size_t n = points.size();
  if (n == 2) {
    LineSegment line;
    line.Point(points[1]);
    figure.Segments().Append(line);
  } else {
    // Catmull-Rom through the sample points, as cubic beziers
    for (size_t i = 1; i < n; ++i) {
      const Point p0 = points[i >= 2 ? i - 2 : 0];
      const Point p1 = points[i - 1];
      const Point p2 = points[i];
      const Point p3 = points[(std::min)(i + 1, n - 1)];
      BezierSegment segment;
      segment.Point1(Point{p1.X + (p2.X - p0.X) / 6, p1.Y + (p2.Y - p0.Y) / 6});
      segment.Point2(Point{p2.X - (p3.X - p1.X) / 6, p2.Y - (p3.Y - p1.Y) / 6});
      segment.Point3(p2);
      figure.Segments().Append(segment);
    }
  }
  if (closeToAxis) {
    LineSegment down;
    down.Point(Point{points[n - 1].X, static_cast<float>(axisY)});
    figure.Segments().Append(down);
    LineSegment back;
    back.Point(Point{points[0].X, static_cast<float>(axisY)});
    figure.Segments().Append(back);
    figure.IsClosed(true);
  }
  geometry.Figures().Append(figure);
  return geometry;
}

void TransferChart::DrawSeries(SeriesUi& series, std::vector<Point> const& points,
                               Rect const& clip, double axisY) {
  series.strokeClip.Rect(clip);
  series.stroke.Data(SmoothGeometry(points, /*closeToAxis=*/false, axisY));
  if (series.fill) {
    series.fillClip.Rect(clip);
    series.fill.Data(SmoothGeometry(points, /*closeToAxis=*/true, axisY));
  }
}

void TransferChart::Redraw(double now, double width, double height) {
  const double plotTop = kStatsBand + kPeakBand;
  const double plotBottom = height - kPeakBand;
  const double centerY = (plotTop + plotBottom) / 2;
  const double plotHalf = (std::max)((plotBottom - plotTop) / 2, 8.0);

  axis_.X1(0);
  axis_.Y1(centerY);
  axis_.X2(width);
  axis_.Y2(centerY);

  if (entries_.empty()) {
    for (SeriesUi* series : {&egressBytes_, &ingressBytes_, &egressPackets_, &ingressPackets_}) {
      series->stroke.Data(nullptr);
      if (series->fill) series->fill.Data(nullptr);
    }
    peakEgressLabel_.Visibility(Visibility::Collapsed);
    peakIngressLabel_.Visibility(Visibility::Collapsed);
    return;
  }

  // ease the newest bucket's value in from the previous bucket's value so a
  // changed rightmost value transitions smoothly rather than hopping
  std::vector<Entry> entries = entries_;
  {
    Entry& last = entries.back();
    const Sample prev =
        2 <= entries.size() ? entries[entries.size() - 2].sample : Sample{};
    double progress = std::clamp((now - last.time) / kTransitionSeconds, 0.0, 1.0);
    double eased = 1 - std::pow(1 - progress, 3);
    last.sample = Lerp(prev, last.sample, eased);
  }

  // pad the series so the baseline spans the full width: zeros back to the
  // window start on the left, and a hold of the latest value out to now
  const double windowStart = now - window_;
  std::vector<Entry> padded;
  padded.reserve(entries.size() + 3);
  if (windowStart < entries.front().time) {
    const double step =
        2 <= entries.size() ? (std::max)(0.2, entries[1].time - entries[0].time) : 1.0;
    const double rampTime = entries.front().time - step;
    padded.push_back(Entry{windowStart, Sample{}});
    if (windowStart < rampTime) padded.push_back(Entry{rampTime, Sample{}});
  }
  padded.insert(padded.end(), entries.begin(), entries.end());
  if (padded.back().time < now) padded.push_back(Entry{now, entries.back().sample});

  const double scaleBytes = byteScale_.ValueAt(now);
  const double scalePackets = packetScale_.ValueAt(now);
  auto x = [&](double time) { return width * (1 - (now - time) / window_); };
  auto offset = [&](int64_t value, double scale) {
    return plotHalf * (std::min)(1.0, static_cast<double>(value) / (std::max)(scale, 1.0));
  };
  auto mapPoints = [&](bool egress, bool bytes, double scale) {
    std::vector<Point> points;
    points.reserve(padded.size());
    for (const auto& e : padded) {
      const int64_t value =
          bytes ? (egress ? e.sample.egressBytes : e.sample.ingressBytes)
                : (egress ? e.sample.egressPackets : e.sample.ingressPackets);
      const double y =
          egress ? centerY - offset(value, scale) : centerY + offset(value, scale);
      points.push_back(Point{static_cast<float>(x(e.time)), static_cast<float>(y)});
    }
    return points;
  };

  // clip each direction to its half so the curve smoothing never crosses the
  // center axis (macOS parity)
  const Rect topHalf{0, 0, static_cast<float>(width), static_cast<float>(centerY)};
  const Rect bottomHalf{0, static_cast<float>(centerY), static_cast<float>(width),
                        static_cast<float>((std::max)(0.0, height - centerY))};
  DrawSeries(egressBytes_, mapPoints(true, true, scaleBytes), topHalf, centerY);
  DrawSeries(ingressBytes_, mapPoints(false, true, scaleBytes), bottomHalf, centerY);
  DrawSeries(egressPackets_, mapPoints(true, false, scalePackets), topHalf, centerY);
  DrawSeries(ingressPackets_, mapPoints(false, false, scalePackets), bottomHalf, centerY);

  // sliding peak byte labels track the peak buckets (from the raw series)
  const Entry* peakEgress = nullptr;
  const Entry* peakIngress = nullptr;
  for (const auto& e : entries_) {
    if (!peakEgress || peakEgress->sample.egressBytes < e.sample.egressBytes) peakEgress = &e;
    if (!peakIngress || peakIngress->sample.ingressBytes < e.sample.ingressBytes)
      peakIngress = &e;
  }
  UpdatePeakLabel(peakEgressLabel_, peakEgress ? peakEgress->sample.egressBytes : 0,
                  peakEgress ? peakEgress->time : 0, now, width, kStatsBand, true);
  UpdatePeakLabel(peakIngressLabel_, peakIngress ? peakIngress->sample.ingressBytes : 0,
                  peakIngress ? peakIngress->time : 0, now, width, height - kPeakBand, false);
}

void TransferChart::UpdatePeakLabel(TextBlock const& label, int64_t value, double time,
                                    double now, double width, double y, bool pointsUp) {
  if (value <= 0) {
    label.Visibility(Visibility::Collapsed);
    return;
  }
  label.Text(H(FormatByteRate(value)) + (pointsUp ? L" ▲" : L" ▼"));
  label.Visibility(Visibility::Visible);
  label.Measure(Size{INFINITY, INFINITY});
  const double labelWidth = label.DesiredSize().Width;
  const double labelHeight = label.DesiredSize().Height;
  // the label slides each frame to track the peak bucket's x position
  const double rawX = width * (1 - (now - time) / window_);
  const double halfWidth = labelWidth / 2 + 2;
  const double centerX = std::clamp(rawX, halfWidth, (std::max)(halfWidth, width - halfWidth));
  Canvas::SetLeft(label, centerX - labelWidth / 2);
  Canvas::SetTop(label, y + (kPeakBand - labelHeight) / 2);
}

void TransferChart::UpdateAverageLabels() {
  // rolling mean over the last N raw buckets (~ per-second average)
  int64_t egressBytes = 0, ingressBytes = 0, egressPackets = 0, ingressPackets = 0;
  const size_t n = std::min<size_t>(entries_.size(), kAverageBucketCount);
  if (0 < n) {
    for (size_t i = entries_.size() - n; i < entries_.size(); ++i) {
      egressBytes += entries_[i].sample.egressBytes;
      ingressBytes += entries_[i].sample.ingressBytes;
      egressPackets += entries_[i].sample.egressPackets;
      ingressPackets += entries_[i].sample.ingressPackets;
    }
    egressBytes /= static_cast<int64_t>(n);
    ingressBytes /= static_cast<int64_t>(n);
    egressPackets /= static_cast<int64_t>(n);
    ingressPackets /= static_cast<int64_t>(n);
  }
  auto apply = [this](TextBlock const& byteAvg, TextBlock const& packetAvg,
                      TextBlock const& arrow, int64_t bytes, int64_t packets) {
    byteAvg.Text(H(FormatByteRate(bytes)));
    byteAvg.Opacity(0 < bytes ? 1.0 : 0.4);
    packetAvg.Text(H(FormatPacketRate(packets)));
    packetAvg.Opacity(0 < packets ? 1.0 : 0.4);
    // the arrow lights up like a link light when this direction is active
    arrow.Foreground(0 < bytes || 0 < packets ? textBrush_ : mutedBrush_);
  };
  apply(egressByteAvg_, egressPacketAvg_, egressArrow_, egressBytes, egressPackets);
  apply(ingressByteAvg_, ingressPacketAvg_, ingressArrow_, ingressBytes, ingressPackets);
}

}  // namespace urnw
