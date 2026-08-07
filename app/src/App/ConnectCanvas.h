// The connect hero canvas — the app's signature visual, and the consumer for
// `ConnectGrid::getProviderGridPointList()`, which until now had none anywhere
// in the client (the grid listener has been subscribed since the first build and
// `SdkHost::ReadStats` kept only `getWindowCurrentSize()`).
//
// Built the way `TransferChart` is built: XAML shapes assembled into a host Grid
// from code, laid out in code on SizeChanged, and stepped from the ConnectPage's
// existing ~10 fps `chartTimer_` rather than a second clock. It renders the five
// states iOS's `ConnectButton/` carries — disconnected, connecting, connected,
// error, processing-subscription — from the same brand palette.
//
// It is NOT a transliteration of the phone screen. The differences are
// deliberate, and each is a desktop property the phone does not have:
//
//   * It scales with the window instead of sitting at iOS's fixed 256pt. The
//     disc tracks the host width and clamps to [kMinDisc, kMaxDisc] so it stays
//     legible at the 480-dip default and does not become a billboard maximized.
//   * A wide ambient wash spans the full hero band. This is the part that
//     answers "it feels like buttons on a black screen": a phone canvas is
//     nearly as wide as its screen, a desktop window is not, and a bare circle
//     floating in black leaves the width empty.
//   * Hover and keyboard focus exist here and are drawn (the disc lifts on
//     pointer-over; a real focus ring appears on keyboard focus only).
//   * A faint lattice always underlies the live points, so an EMPTY grid — the
//     normal state for a session that carries no traffic, including every
//     rpc-only session — reads as "the grid is there and nothing is in it yet"
//     rather than as a blank or a failure.
//
// Nothing here is clipped to the disc, deliberately: `UIElement.Clip` takes only
// a RectangleGeometry, so a circular clip would need an opaque donut mask over
// the wash. Instead the two things that could overflow are contained at the
// source — grid dots outside the inscribed circle are culled in
// `LayoutPoints`, and the connected-state blobs are radial gradients that reach
// zero alpha before the rim.
//
// Motion budget. This runs behind a tray icon, so an idle animation is a real
// cost and the rules are strict:
//
//   * Every repeating animation is a `Storyboard` over independently animatable
//     properties (Opacity, RotateTransform.Angle, ScaleTransform.Scale*), which
//     the compositor runs off the UI thread. Nothing repeating is per-frame.
//   * `Tick()` is the ONLY per-frame path and returns immediately unless a grid
//     point transition is actually in flight (TransferChart's rule).
//   * `SetPresentationActive(false)` stops every storyboard and the progress
//     ring. The page already tracks presentation state for the charts; the hero
//     uses the same signal, so a hidden window animates nothing.
//   * `UISettings::AnimationsEnabled() == false` means no repeating animation is
//     ever started and states land on their settled values immediately.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
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
  // The five states the hero renders. The first three are the connect status
  // (DESTINATION_SET folds into Connecting, exactly as android's ConnectStatus
  // does); Error and Processing are the balance states iOS's ConnectButtonView
  // layers on top, and they take priority over the connection state.
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
  // state, not an error, and it renders as the bare lattice.
  void SetGrid(std::vector<urnet::ProviderGridPoint> const& points, int64_t width,
               int64_t height);

  // Window hidden / another page selected: stops every storyboard. Idempotent.
  void SetPresentationActive(bool active);

  // Desktop affordances. Hover lifts the disc; the focus ring is drawn only for
  // KEYBOARD focus, matching how the platform draws focus visuals elsewhere.
  void SetHovered(bool hovered);
  void SetFocusRingVisible(bool visible);

  // ~10 fps, from ConnectPage::OnChartTick. Returns immediately unless a point
  // transition is in flight.
  void Tick();

 private:
  // The SDK's ProviderGridPoint::State strings. `Removed` is ours: a point that
  // has left the grid fades out over one transition rather than vanishing.
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
    bool seen = false;   // marked during a SetGrid diff
    bool culled = false; // outside the inscribed circle; kept in the map, not drawn
    bool Animating() const { return colorProgress < 1 || sizeProgress < 1; }
  };

  void BuildVisuals(winrt::Microsoft::UI::Xaml::Controls::Grid const& host);
  void Layout();        // recompute all geometry for the current host size
  void LayoutLattice(); // rebuild the faint substrate for the current cell size
  void LayoutPoints();  // place and size the live dots
  void ApplyPoint(GridDot& p);  // push colour + scale for the point's progress
  void ApplyStateVisuals();
  void StopAll();
  void StartIdle();     // start whatever the current state repeats, if allowed
  void RunBlobs(bool in);
  bool AnimationsEnabled() const;

  static PointState ParsePointState(std::string const& value);
  static winrt::Windows::UI::Color ColorForPointState(PointState state);
  static winrt::Windows::UI::Color Blend(winrt::Windows::UI::Color from,
                                         winrt::Windows::UI::Color to, double t);
  static winrt::Windows::UI::Color AccentForState(State state);

  winrt::Microsoft::UI::Xaml::Controls::Grid host_{nullptr};

  // ---- layers (bottom to top) ----
  // two wash ellipses cross-faded on a state change: RadialGradientBrush derives
  // from XamlCompositionBrushBase, so its stops are not a Storyboard target —
  // the brush is rebuilt and the two elements swap opacity instead
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse washA_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse washB_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse activeWash_{nullptr};  // the visible one

  // everything inside the disc, so hover scales the whole thing at once
  winrt::Microsoft::UI::Xaml::Controls::Grid disc_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::ScaleTransform discScale_{nullptr};
  // the grid layer, dimmed by the error / processing states. A separate layer
  // from the glyph and the progress ring so dimming the grid does not dim the
  // thing those states are asking the user to read.
  winrt::Microsoft::UI::Xaml::Controls::Grid substrate_{nullptr};

  winrt::Microsoft::UI::Xaml::Shapes::Ellipse ring0_{nullptr};  // outer
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse ring1_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse ring2_{nullptr};  // inner

  winrt::Microsoft::UI::Xaml::Controls::Canvas latticeCanvas_{nullptr};
  std::vector<winrt::Microsoft::UI::Xaml::Shapes::Ellipse> lattice_;

  // connected: five soft brand blobs that slide in (iOS ConnectCanvasConnected
  // parity). Radial gradients, so they need no clip.
  struct Blob {
    winrt::Microsoft::UI::Xaml::Shapes::Ellipse shape{nullptr};
    winrt::Microsoft::UI::Xaml::Media::TranslateTransform shift{nullptr};
    double fx = 0, fy = 0;  // settled offset, as a fraction of the disc diameter
    double ix = 0, iy = 0;  // off-disc entry offset, same units
  };
  std::vector<Blob> blobs_;

  winrt::Microsoft::UI::Xaml::Controls::Canvas pointCanvas_{nullptr};
  std::unordered_map<std::string, GridDot> points_;

  // connecting: two counter-rotating arcs. Drawn as stroked Ellipses with a
  // single long dash rather than Path/ArcSegment, so the rotation centre is the
  // element's own centre and no geometry has to be rebuilt to spin them.
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse arcOuter_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse arcInner_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::RotateTransform arcOuterSpin_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::RotateTransform arcInnerSpin_{nullptr};

  // disconnected: a solid core with an expanding pulse ring behind it
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse pulse_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::ScaleTransform pulseScale_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse coreRing_{nullptr};
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse core_{nullptr};

  winrt::Microsoft::UI::Xaml::Controls::FontIcon glyph_{nullptr};        // error
  winrt::Microsoft::UI::Xaml::Controls::ProgressRing progress_{nullptr}; // processing
  winrt::Microsoft::UI::Xaml::Shapes::Ellipse focusRing_{nullptr};

  // ---- storyboards ----
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard pulseSb_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard spinSb_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard breatheSb_{nullptr};
  winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard blobSb_{nullptr};

  State state_ = State::Disconnected;
  bool presentationActive_ = false;
  bool hovered_ = false;
  bool blobsIn_ = false;  // the connected blobs sit at their settled offsets

  // layout, recomputed on SizeChanged
  double width_ = 0;
  double disc_d_ = 0;   // disc diameter
  double cell_ = 0;     // grid cell edge
  double originX_ = 0;  // grid origin inside the point canvas
  double originY_ = 0;
  int32_t cols_ = 0;
  int32_t rows_ = 0;

  int64_t gridWidth_ = 0;
  int64_t gridHeight_ = 0;
  bool animating_ = false;  // a point transition is in flight
};

}  // namespace urnw
