// Live transfer chart for one throughput route (port of the macOS
// TransferChart). Mirrored around a center horizontal axis: egress above,
// ingress below, newest at the right edge. Byte and packet series draw as
// Catmull-Rom smoothed Path geometry (bytes with a soft area fill down to the
// axis) on independent auto-scales (floors 1024 / 8) that ease toward new
// window peaks (cubic ease-out, 0.5s) instead of jumping. Rolling 5-bucket
// averages label the top right; sliding peak labels track the peak byte
// buckets. Builds its visuals into a host Grid (a Canvas plus overlay labels)
// and redraws on Tick() (~10 fps) only while there is activity in the window
// or a scale ease in flight.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "Sdk.h"

namespace urnw {

enum class ThroughputRoute { Remote, Local, Block };

class TransferChart {
 public:
  TransferChart(winrt::Microsoft::UI::Xaml::Controls::Grid const& host,
                std::wstring title, ThroughputRoute route,
                winrt::Windows::UI::Color byteColor,
                winrt::Windows::UI::Color packetColor);

  // Replace the point series (SDK point times are unix milliseconds).
  void SetPoints(const std::vector<urnet::ThroughputPoint>& points, int64_t windowSeconds);
  // Redraw if animating (activity in window, scale ease in flight, or dirty).
  void Tick();

 private:
  struct Sample {
    int64_t egressBytes = 0;
    int64_t ingressBytes = 0;
    int64_t egressPackets = 0;
    int64_t ingressPackets = 0;
  };
  struct Entry {
    double time = 0;  // unix seconds
    Sample sample;
  };
  // a time-based cubic ease-out of a scalar from `from` to `to`
  struct ValueTransition {
    double from = 0;
    double to = 0;
    double start = 0;
    double ValueAt(double now) const;
    bool InFlight(double now) const;
  };
  // one drawn series: a smoothed stroke path, an optional area fill path, and
  // the half-plane clips that keep the curve from crossing the center axis
  struct SeriesUi {
    winrt::Microsoft::UI::Xaml::Shapes::Path stroke{nullptr};
    winrt::Microsoft::UI::Xaml::Shapes::Path fill{nullptr};  // byte series only
    winrt::Microsoft::UI::Xaml::Media::RectangleGeometry strokeClip{nullptr};
    winrt::Microsoft::UI::Xaml::Media::RectangleGeometry fillClip{nullptr};
  };

  void BuildVisuals(winrt::Microsoft::UI::Xaml::Controls::Grid const& host, std::wstring title);
  SeriesUi BuildSeries(winrt::Windows::UI::Color color, double strokeWidth, bool withFill);
  void Redraw(double now, double width, double height);
  void DrawSeries(SeriesUi& series, std::vector<winrt::Windows::Foundation::Point> const& points,
                  winrt::Windows::Foundation::Rect const& clip, double axisY);
  void UpdateAverageLabels();
  void UpdatePeakLabel(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& label,
                       int64_t value, double time, double now, double width, double y,
                       bool pointsUp);
  static winrt::Microsoft::UI::Xaml::Media::PathGeometry SmoothGeometry(
      std::vector<winrt::Windows::Foundation::Point> const& points, bool closeToAxis,
      double axisY);
  static void UpdateTransition(ValueTransition& t, double target, double now);
  static Sample Lerp(const Sample& a, const Sample& b, double t);
  static bool IsActive(const Sample& s);

  ThroughputRoute route_;
  winrt::Windows::UI::Color byteColor_;
  winrt::Windows::UI::Color packetColor_;

  std::vector<Entry> entries_;   // ascending by time
  double window_ = 60;           // seconds
  bool dirty_ = true;            // data or size changed since last draw
  bool wasAnimating_ = false;    // draw one settling frame after activity drains

  ValueTransition byteScale_;
  ValueTransition packetScale_;

  // visuals (children of canvas_ / host overlays)
  winrt::Microsoft::UI::Xaml::Controls::Canvas canvas_{nullptr};
  SeriesUi egressBytes_;
  SeriesUi ingressBytes_;
  SeriesUi egressPackets_;
  SeriesUi ingressPackets_;
  winrt::Microsoft::UI::Xaml::Shapes::Line axis_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock peakEgressLabel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock peakIngressLabel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock egressByteAvg_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock egressPacketAvg_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock egressArrow_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock ingressByteAvg_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock ingressPacketAvg_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock ingressArrow_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Brush textBrush_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Brush mutedBrush_{nullptr};
};

}  // namespace urnw
