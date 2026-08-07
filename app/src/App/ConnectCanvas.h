// The connect hero canvas — the app's signature visual, and the consumer for
// `ConnectGrid::getProviderGridPointList()`.
//
// This is a port of iOS's `Main/Connect/ConnectButton/`, not a reinterpretation
// of it. Every metric below is expressed in iOS's own 256pt canvas space and
// scaled by `s = side_ / 256`, so a reader can diff this file against
// ConnectButtonView.swift line for line:
//
//   ConnectButtonView            -> the globe mask, the layer order, the states
//   ConnectCanvasDisconnected    -> 48/50/52/56pt electric-blue core + pulse
//   ConnectCanvasConnecting(+VM) -> GlobeConnector lines + the provider grid
//   ConnectCanvasConnectedState  -> five 180pt brand circles that slide in
//   ConnectErrorState            -> a faint warning glyph, 500ms delayed
//   ConnectProcessingSubscription-> a faint waiting glyph, 500ms delayed
//
// THE MASK. iOS writes `.mask { Image("ur.symbols.globe") }` around the whole
// ZStack; android draws `R.drawable.connect_mask` over the whole Box after
// `clipToBounds()`. WinUI 3 has neither: `UIElement.Clip` takes only a
// RectangleGeometry, and `CompositionGeometricClip` needs a `CompositionPath`,
// which needs an `IGeometrySource2D`, which only Win2D implements — and this
// app does not carry Win2D. So the canvas uses android's construction exactly,
// which turns out to be the same two primitives WinUI does have:
//
//   * `globe_.Clip = RectangleGeometry(0,0,side,side)` is android's
//     `clipToBounds()`. This is what makes iOS's slide-in motion possible at
//     all: the connected-state circles start a full canvas-width off-centre and
//     the square clip eats them until they arrive.
//   * `mask_` is android's `connect_mask`: one Path, fill rule EvenOdd, data =
//     an outer square MINUS the globe silhouette, filled opaque kBackground and
//     drawn last. The four scalloped corners of the square are painted back out
//     to the page colour, leaving the globe.
//
// The globe outline is `kGlobePath`, the same 32x32 path LoginCarousel already
// uses for the sign-in globe (Assets.xcassets/Icons/ur.symbols.globe.svg), so
// the two globes in this app are the same shape by construction.
//
// The cost of the overlay is that it assumes the hero sits on kBackground. It
// does, on every page this app has; if the hero is ever moved onto a card, this
// one Fill is the thing to change.
//
// Motion budget. This runs behind a tray icon, so an idle animation is a real
// cost. iOS animates its idle pulse `repeatForever` on a phone screen that
// sleeps; a desktop tray app cannot:
//
//   * Every repeating animation is a `Storyboard` over independently animatable
//     properties, which the compositor runs off the UI thread.
//   * `Tick()` is the ONLY per-frame path and returns immediately unless a grid
//     point transition is actually in flight (TransferChart's rule).
//   * The disconnected pulse runs a BOUNDED burst and then the hero is still.
//     The connected state settles the instant its circles land — iOS does not
//     animate it either. Connecting is the one genuinely transient state, and
//     it animates for exactly as long as the grid is churning.
//   * `SetPresentationActive(false)` stops every storyboard, so a hidden window
//     animates nothing.
//   * `UISettings::AnimationsEnabled() == false` means no repeating animation is
//     ever started and states land on their settled values immediately.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "Sdk.h"

namespace urnw {

class ConnectCanvas {
 public:
  // The five states the hero renders, in iOS's own branch order: Processing and
  // Error are balance states that REPLACE the connection canvas (they are the
  // first two arms of ConnectButtonView's if/else), and the remaining three are
  // the connection status (DESTINATION_SET folds into Connecting, exactly as
  // android's ConnectStatus does).
  enum class State { Disconnected, Connecting, Connected, Error, Processing };

  // Builds every visual into `host` (a Grid). `host` is expected to be
  // AccessibilityView=Raw in the markup: the canvas is decorative and the hero
  // Button around it carries the name and the click.
  explicit ConnectCanvas(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
  ~ConnectCanvas();

  void SetState(State state);
  State state() const { return state_; }

  // The live provider grid. `points` may legitimately be empty (no session, an
  // rpc-only session, a connection carrying no traffic yet) — that is a normal
  // state, not an error, and it renders as the bare GlobeConnector lines, which
  // is exactly what iOS shows in the same situation.
  void SetGrid(std::vector<urnet::ProviderGridPoint> const& points, int64_t width,
               int64_t height);

  // Window hidden / another page selected: stops every storyboard. Idempotent.
  void SetPresentationActive(bool active);

  // Desktop affordances neither phone client has. Hover lifts and warms the
  // globe; the focus ring is drawn only for KEYBOARD focus, matching how the
  // platform draws focus visuals elsewhere.
  void SetHovered(bool hovered);
  void SetFocusRingVisible(bool visible);

  // ~10 fps, from ConnectPage::OnChartTick. Returns immediately unless a point
  // transition is in flight.
  void Tick();

 private:
  // The SDK's ProviderGridPoint::State strings. `Removed` is iOS's own extra
  // case: a point that has left the grid fades out over one transition rather
  // than vanishing between frames.
  enum class PointState { InEvaluation, EvaluationFailed, NotAdded, Added, Removed };

  // named GridDot, not Point: `Point` is winrt::Windows::Foundation::Point
  // inside every member function of this class.
  struct GridDot {
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse dot{nullptr};
    winrt::Microsoft::UI::Xaml::Media::SolidColorBrush brush{nullptr};
    // scale, not Width/Height: a per-frame size change on a Canvas child would
    // invalidate layout every tick; a render transform does not
    winrt::Microsoft::UI::Xaml::Media::ScaleTransform scale{nullptr};
    int32_t x = 0;
    int32_t y = 0;
    PointState state = PointState::InEvaluation;
    PointState previous = PointState::InEvaluation;
    double colorProgress = 1;  // 0..1, 1 = settled
    double sizeProgress = 1;
    bool seen = false;  // marked during a SetGrid diff
    bool Animating() const { return colorProgress < 1 || sizeProgress < 1; }
  };

  // One of iOS's five connected-state circles. Opaque, canvas-width offsets,
  // colour and entry vector re-paired on every connect (ConnectCanvasConnected
  // shuffles both arrays in `onChange(of: isActive)`).
  struct Blob {
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse shape{nullptr};
    winrt::Microsoft::UI::Xaml::Media::SolidColorBrush fill{nullptr};
    winrt::Microsoft::UI::Xaml::Media::TranslateTransform shift{nullptr};
    double fx = 0, fy = 0;  // settled offset, in canvas widths
    double ix = 0, iy = 0;  // off-canvas entry offset, same units
  };

  void BuildVisuals(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
  // The colour the hero is actually drawn on, read off the visual tree. The
  // mask is an opaque overlay, so it has to be painted in the ground colour or
  // it reads as four black notches; and iOS's globe is one surface step ABOVE
  // its ground, so the silhouette has to be derived from the same value. Both
  // are resolved at layout time rather than assumed, because the page around
  // this hero is not ours and moves.
  void ApplyGround();
  winrt::Windows::UI::Color ResolveGround() const;
  void Layout();       // recompute all geometry for the current host size
  void LayoutPoints(); // place and size the live dots
  void ApplyPoint(GridDot& p);  // push colour + scale for the point's progress
  void ApplyStateVisuals();
  void ClearPoints();
  void Fade(winrt::Microsoft::UI::Xaml::UIElement const& element, double to, int64_t ms);
  void ShuffleBlobs();
  void RunBlobs(bool in);
  void StopAll();
  void StartIdle();
  bool AnimationsEnabled() const;

  static PointState ParsePointState(std::string const& value);
  static winrt::Windows::UI::Color ColorForPointState(PointState state);
  static winrt::Windows::UI::Color Blend(winrt::Windows::UI::Color from,
                                         winrt::Windows::UI::Color to, double t);

  winrt::Microsoft::UI::Xaml::Controls::Grid host_{nullptr};

  // ---- the globe, and everything the mask contains ----
  winrt::Microsoft::UI::Xaml::Controls::Grid globe_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::ScaleTransform globeScale_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::RectangleGeometry globeClip_{nullptr};
  // the globe silhouette in tintedBackgroundBase — iOS's `.background(...)`
  winrt::Microsoft::UI::Xaml::Shapes::Path globeFill_{nullptr};

  // GlobeConnector.svg + the live grid, as one fadeable layer: iOS mounts them
  // together in ConnectCanvasConnectingStateView and fades that view in and out
  winrt::Microsoft::UI::Xaml::Controls::Grid gridLayer_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Path connectorBg_{nullptr};   // white @ 4%
  winrt::Microsoft::UI::Xaml::Shapes::Rectangle equator_{nullptr};  // the y=127 line
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse meridian_{nullptr};   // the r=51.5 ellipse
  winrt::Microsoft::UI::Xaml::Controls::Canvas pointCanvas_{nullptr};
  std::unordered_map<std::string, GridDot> points_;

  winrt::Microsoft::UI::Xaml::Controls::Grid blobLayer_{nullptr};
  std::vector<Blob> blobs_;

  // disconnected: the electric-blue core, its two rings, and the pulse behind it
  winrt::Microsoft::UI::Xaml::Controls::Grid idleLayer_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse pulse_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::ScaleTransform pulseScale_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse coreRing_{nullptr};  // 52pt, blue, 4pt
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse coreGap_{nullptr};   // 50pt, base, 2pt
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse core_{nullptr};      // 48pt, blue

  winrt::Microsoft::UI::Xaml::Controls::FontIcon glyph_{nullptr};  // error / processing

  // android's connect_mask: the square minus the globe, opaque, drawn last
  winrt::Microsoft::UI::Xaml::Shapes::Path mask_{nullptr};
  // outside the mask, so the ring is not eaten by it
  winrt::Microsoft::UI::Xaml::Shapes::Path focusRing_{nullptr};

  // ---- storyboards ----
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard pulseSb_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard blobSb_{nullptr};

  State state_ = State::Disconnected;
  bool presentationActive_ = false;
  bool hovered_ = false;
  bool blobsIn_ = false;  // the connected circles sit at their settled offsets

  // layout, recomputed on SizeChanged
  double width_ = 0;
  double side_ = 0;  // the globe's edge, iOS's `canvasWidth`
  double cell_ = 0;  // iOS's `maxPointSize` = side_ / gridWidth
  int32_t cols_ = 0;

  // the resolved page/card colour under the hero, and a sentinel that means
  // "not resolved yet" (alpha 0 never occurs on a real background)
  winrt::Windows::UI::Color ground_{0, 0, 0, 0};

  int64_t gridWidth_ = 0;
  int64_t gridHeight_ = 0;
  bool animating_ = false;  // a point transition is in flight

  std::mt19937 rng_{0x5EED0BE};
};

}  // namespace urnw
