// The transport distribution bar (port of the apple TransportDistributionBar,
// TRANSPORTSTATS): the remote traffic of the stats window partitioned by the
// transport that carried it, as one full-width stacked bar under the Remote
// transfer chart. Each transport with traffic in the window is a segment
// proportional to its share, in the SDK's stable order, so the bar reads as
// "this much of the window's transfer went over each carrier". The segments
// always tile exactly 100% of the width -- also mid-tween -- because the
// geometry is derived from ONE animated vector of the SDK's cumulative
// boundaries rather than from independently animated widths. Enabled transports
// that carried nothing are listed in an "unused" footer instead of drawing a
// zero-width segment; when the window has no remote traffic at all the segments
// fade out in place, holding their last shape, leaving a faint empty track.
//
// All the numbers (shares, boundaries, percents, used, enabled) come from the
// SDK view controller through SdkHost (TransportDistributionSnapshot); this
// class only maps names/colors and draws. Builds its visuals into a host Grid,
// like TransferChart, and redraws on Tick() (~10 fps) only while a tween or the
// fade is in flight. The whole component is one Button (pane row style) that
// opens the transport settings editor. UI thread only.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "SdkHost.h"

namespace urnw {

// ---- the transport vocabulary: presentation only ---------------------------
// The SDK transport ids double as the mode ids for the selectable carriers
// (h3, h1, dns, dnspump); p2p and unknown are observable carriers only. Names,
// descriptions and brand colors live here; every RULE (the stable order, the
// selectable modes and their preference order, which carriers a policy enables,
// the Auto editing constraints) is the SDK's. The DNS carriers carry their
// product names -- "whodis" / "whodis pump" -- which are not localized; the
// queued bucket ("unknown": admitted for sending, not yet written to a physical
// carrier) is a plain word and is.
winrt::hstring TransportName(std::string const& transportType);
// one line for the settings editor rows; empty for p2p / unknown
winrt::hstring TransportDetail(std::string const& transportType);
// the brand color of the carrier (bar segment, legend dot, editor dot)
winrt::Windows::UI::Color TransportColor(std::string const& transportType);

class TransportBar {
 public:
  // `host` receives the whole component; `onOpen` runs on click (the transport
  // settings editor).
  TransportBar(winrt::Microsoft::UI::Xaml::Controls::Grid const& host,
               std::function<void()> onOpen);

  // Replace the distribution (from SdkHost's transport-distribution feed). Starts
  // the 1s boundary tween from wherever the previous tween currently is.
  void SetDistribution(const TransportDistributionSnapshot& distribution);
  // Redraw if animating (a boundary tween or the empty fade in flight, or dirty).
  void Tick();

 private:
  void BuildVisuals(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
  void EnsureSegments(size_t count);
  void Redraw(double now, double width, double height);
  void RebuildLegend();
  void RebuildUnused();
  // the interpolated boundary vector at `now`
  std::vector<double> BoundariesAt(double now) const;
  bool TweenInFlight(double now) const;

  std::function<void()> onOpen_;

  TransportDistributionSnapshot distribution_;
  // the animated vector of cumulative boundaries (fractions of the width, SDK
  // stable order): eased element-wise from `from` to `to` over the tween, so
  // every segment edge is read from one interpolated vector and the segments
  // tile 100% at every frame
  std::vector<double> fromBoundaries_;
  std::vector<double> toBoundaries_;
  double tweenStart_ = 0;  // unix seconds; 0 = never tweened
  // the segments' opacity: fades out (holding the last shape) while the window
  // is empty, back in from that shape when traffic resumes
  double fadeFrom_ = 0;
  double fadeTo_ = 0;
  double fadeStart_ = 0;
  bool dirty_ = true;
  bool wasAnimating_ = false;

  // visuals
  winrt::Microsoft::UI::Xaml::Controls::Button root_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Canvas canvas_{nullptr};  // the segment layer
  std::vector<winrt::Microsoft::UI::Xaml::Shapes::Path> segments_;
  // hairline separators in the pane color, one per segment (drawn between a
  // visible segment and the previous visible one)
  std::vector<winrt::Microsoft::UI::Xaml::Shapes::Rectangle> separators_;
  std::vector<std::string> segmentTypes_;  // the transport id each path is colored for
  winrt::Microsoft::UI::Xaml::Controls::RichTextBlock legend_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::RichTextBlock unused_{nullptr};
  // the used / unused transport ids the two rows were last built for, so a tick
  // that only moves percents updates labels rather than rebuilding inlines
  std::vector<std::string> legendKey_;
  std::vector<std::string> unusedKey_;
  std::vector<winrt::Microsoft::UI::Xaml::Controls::TextBlock> percentLabels_;
  winrt::Microsoft::UI::Xaml::Media::Brush textBrush_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Brush mutedBrush_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Brush faintBrush_{nullptr};
};

}  // namespace urnw
