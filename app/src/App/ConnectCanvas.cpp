// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ConnectCanvas.h"

#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numeric>
#include <utility>

#include "UrColors.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
// NOT `using namespace ...::Shapes`: Shapes::Ellipse collides with the Win32
// GDI function ::Ellipse(HDC, int, int, int, int) from wingdi.h, and every
// unqualified use becomes ambiguous.
namespace shapes = winrt::Microsoft::UI::Xaml::Shapes;
namespace anim = winrt::Microsoft::UI::Xaml::Media::Animation;

namespace urnw {
namespace {

// The ur globe silhouette in its own 32x32 box — byte-identical to the copy in
// LoginCarousel.cpp, which is Assets.xcassets/Icons/ur.symbols.globe.svg. The
// 256-box GlobeMask/GlobeConnector assets are this same path times eight.
constexpr const wchar_t* kGlobePath =
    L"M30 8C28.8955 8 28 7.10453 28 6C28 4.89547 27.1045 4 26 4C24.8955 4 24 3.10453 24 2C24 "
    L"0.895469 23.1045 0 22 0H10C8.89547 0 8 0.895469 8 2C8 3.10453 7.10453 4 6 4C4.89547 4 4 "
    L"4.89547 4 6C4 7.10453 3.10453 8 2 8C0.895469 8 0 8.89547 0 10V22C0 23.1045 0.895469 24 2 "
    L"24C3.10453 24 4 24.8955 4 26C4 27.1045 4.89547 28 6 28C7.10453 28 8 28.8955 8 30C8 31.1045 "
    L"8.89547 32 10 32H22C23.1045 32 24 31.1045 24 30C24 28.8955 24.8955 28 26 28C27.1045 28 28 "
    L"27.1045 28 26C28 24.8955 28.8955 24 30 24C31.1045 24 32 23.1045 32 22V10C32 8.89547 31.1045 "
    L"8 30 8Z";

// android's connect_mask, rebuilt rather than shipped: the 32x32 square with the
// globe punched out of it (F0 = EvenOdd). Filled opaque and drawn last, this is
// the mask. `F0` and the figure syntax are the XAML path mini-language, which is
// why it goes through XamlReader below — there is no runtime Geometry.Parse.
constexpr const wchar_t* kInverseGlobeRect = L"F0 M0,0 L32,0 L32,32 L0,32 Z ";

// iOS's canvas is a fixed 256pt (ConnectButtonView.canvasWidth), and every
// metric in ConnectButton/ is written against it. Everything here is expressed
// in those units and scaled by side_/kIosCanvas, so this file diffs against the
// Swift one.
constexpr double kIosCanvas = 256.0;

// A desktop window is elastic where a phone is not. The globe tracks the host
// width between these bounds: below the floor the 12x12 provider grid stops
// resolving into distinct dots, above the ceiling it becomes a billboard.
constexpr double kMinSide = 168;
constexpr double kMaxSide = 288;
constexpr double kSidePad = 4;    // horizontal breathing room inside the host
constexpr double kBandPad = 12;   // vertical, iOS's `.padding()`

// iOS metrics, in 256-space.
constexpr double kCoreD = 48;        // the electric-blue disc
constexpr double kCoreGapD = 50;     // stroke 2, in tintedBackgroundBase
constexpr double kCoreRingD = 52;    // stroke 4, in urElectricBlue
constexpr double kPulseD = 56;       // fill urElectricBlue, scales to 1.5
constexpr double kPulseScaleTo = 1.5;
constexpr double kPulseOpacityFrom = 0.5;  // = 1.5 - 1.0
constexpr int64_t kPulseMs = 1500;
constexpr double kBlobD = 180;       // the five connected-state circles
constexpr int64_t kBlobMs = 1000;    // .easeInOut(duration: 1)
constexpr double kEquatorY = 126;    // GlobeConnector "Line 1": y=127, width 2
constexpr double kEquatorH = 2;
constexpr double kMeridianW = 103;   // "Ellipse 54": rx 51.5
constexpr double kMeridianH = 254;   // ry 127
constexpr double kConnectorStroke = 2;
constexpr double kGlyphSize = 32;    // .font(.system(size: 32))
constexpr int64_t kGlyphDelayMs = 500;
constexpr int64_t kGlyphFadeMs = 300;
// `.animation(.easeInOut(duration: 0.5), value: connectionStatus)`
constexpr int64_t kStateFadeMs = 500;

// How long the idle invitation runs before the hero goes still. iOS writes
// `.repeatForever(autoreverses: false)`; see the measurements in StartIdle for
// why a tray app cannot. Bounded in BOTH directions — hovering restarts the
// burst, it does not pin it on.
constexpr double kIdlePulseBursts = 3;

// Hard cap on live dots. A grid this large is not something the SDK produces;
// the cap exists so a malformed push cannot spawn unbounded elements.
constexpr size_t kMaxPoints = 1024;

// 10 fps * 0.15 = ~0.67s to settle. iOS's grid runs at 60fps * 0.05 = 0.33s,
// but its animation is per-point-arrival and ours is sampled off the page's
// existing chart clock; a step this size keeps the grow-in legible at 10fps
// instead of snapping in five frames.
constexpr double kPointStep = 0.15;

anim::RepeatBehavior Times(double count) {
  anim::RepeatBehavior r{};
  r.Type = anim::RepeatBehaviorType::Count;
  r.Count = count;
  return r;
}

Duration Millis(int64_t ms) {
  return Duration{std::chrono::duration_cast<TimeSpan>(std::chrono::milliseconds(ms)),
                  DurationType::TimeSpan};
}

anim::EasingFunctionBase Ease(anim::EasingMode mode) {
  anim::CubicEase e;
  e.EasingMode(mode);
  return e;
}

anim::DoubleAnimation MakeDouble(double from, double to, int64_t ms, int64_t beginMs,
                                 anim::EasingFunctionBase const& ease) {
  anim::DoubleAnimation a;
  a.From(from);
  a.To(to);
  a.Duration(Millis(ms));
  if (0 < beginMs) {
    a.BeginTime(std::chrono::duration_cast<TimeSpan>(std::chrono::milliseconds(beginMs)));
  }
  if (ease) a.EasingFunction(ease);
  return a;
}

void Add(anim::Storyboard const& sb, anim::Timeline const& animation,
         DependencyObject const& target, wchar_t const* property) {
  anim::Storyboard::SetTarget(animation, target);
  anim::Storyboard::SetTargetProperty(animation, property);
  sb.Children().Append(animation);
}

void Stop(anim::Storyboard& sb) {
  if (!sb) return;
  sb.Stop();
  sb = nullptr;
}

// Path.Data's mini-language is parsed by the XAML markup compiler, not by any
// runtime API a C++ caller can reach: there is no Geometry.Parse, and
// PathGeometry takes a figure collection rather than a string. Loading a
// one-element document through XamlReader is how LoginCarousel reaches the same
// parser at run time, and it is how the globe gets here.
shapes::Path ParsePath(std::wstring const& data) {
  const std::wstring markup =
      L"<Path xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Data='" + data +
      L"'/>";
  auto path = Markup::XamlReader::Load(winrt::hstring{markup}).as<shapes::Path>();
  // Fill, not Uniform: the geometry box is exactly the square the element is
  // sized to, so this is an exact 1:1 map and never letterboxes.
  path.Stretch(Stretch::Fill);
  path.HorizontalAlignment(HorizontalAlignment::Center);
  path.VerticalAlignment(VerticalAlignment::Center);
  path.IsHitTestVisible(false);
  return path;
}

shapes::Ellipse MakeEllipse() {
  shapes::Ellipse e;
  e.HorizontalAlignment(HorizontalAlignment::Center);
  e.VerticalAlignment(VerticalAlignment::Center);
  e.IsHitTestVisible(false);
  return e;
}

void SizeSquare(FrameworkElement const& e, double size) {
  e.Width(size);
  e.Height(size);
}

// iOS's tintedBackgroundBase (#1C1C1C) is exactly its page background (#101010)
// lifted 0x0C on every channel. Deriving the globe from the ground rather than
// hard-coding kCard means the silhouette reads whether the hero sits on the page
// — where this returns kCard exactly, as iOS does — or inside a card.
winrt::Windows::UI::Color Lift(winrt::Windows::UI::Color c, int by) {
  auto up = [by](uint8_t v) {
    return static_cast<uint8_t>(std::clamp(static_cast<int>(v) + by, 0, 255));
  };
  return winrt::Windows::UI::Color{c.A, up(c.R), up(c.G), up(c.B)};
}
constexpr int kGlobeLift = 0x0C;
constexpr int kGlobeLiftHover = 0x14;

bool SameColor(winrt::Windows::UI::Color a, winrt::Windows::UI::Color b) {
  return a.A == b.A && a.R == b.R && a.G == b.G && a.B == b.B;
}

}  // namespace

// ---------------------------------------------------------------------------

ConnectCanvas::ConnectCanvas(Grid const& host) { BuildVisuals(host); }

ConnectCanvas::~ConnectCanvas() { StopAll(); }

bool ConnectCanvas::AnimationsEnabled() const {
  // "Show animations in Windows" off means the user wants motion gone, not
  // reduced. Every repeating storyboard honours it.
  try {
    return winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled();
  } catch (...) {
    return true;
  }
}

void ConnectCanvas::BuildVisuals(Grid const& host) {
  host_ = host;

  // ---- the globe container -------------------------------------------------
  // android's `Modifier.size(256.dp).clipToBounds()`. The rect clip is the half
  // of the mask that WinUI gives directly, and it is what lets the connected
  // circles start a whole canvas-width off-centre without leaking.
  globe_ = Grid();
  globe_.HorizontalAlignment(HorizontalAlignment::Center);
  globe_.VerticalAlignment(VerticalAlignment::Center);
  globe_.IsHitTestVisible(false);
  globeClip_ = RectangleGeometry();
  globe_.Clip(globeClip_);
  globeScale_ = ScaleTransform();
  globeScale_.ScaleX(1);
  globeScale_.ScaleY(1);
  globe_.RenderTransformOrigin(Point{0.5f, 0.5f});
  globe_.RenderTransform(globeScale_);
  host_.Children().Append(globe_);

  // iOS: `.background(themeManager.currentTheme.tintedBackgroundBase)` under the
  // whole ZStack, which the globe mask then cuts to shape.
  globeFill_ = ParsePath(kGlobePath);
  globeFill_.Fill(colors::CardBrush());
  globe_.Children().Append(globeFill_);

  // ---- GlobeConnector + the provider grid ---------------------------------
  // One layer, because iOS mounts them as one view and cross-fades that view.
  gridLayer_ = Grid();
  gridLayer_.IsHitTestVisible(false);
  gridLayer_.Opacity(0);
  gridLayer_.Visibility(Visibility::Collapsed);
  globe_.Children().Append(gridLayer_);

  connectorBg_ = ParsePath(kGlobePath);
  connectorBg_.Fill(colors::MakeBrush(
      winrt::Windows::UI::Color{0x0A, 0xFF, 0xFF, 0xFF}));  // fill-opacity 0.04
  gridLayer_.Children().Append(connectorBg_);

  // The two connector lines are drawn in the PAGE colour, not a light one: in
  // GlobeConnector.svg they are stroke="#101010" over the 4% white wash, so they
  // read as grooves cut out of the globe rather than as lines laid on it.
  equator_ = shapes::Rectangle();
  equator_.Fill(colors::BackgroundBrush());
  equator_.HorizontalAlignment(HorizontalAlignment::Stretch);
  equator_.VerticalAlignment(VerticalAlignment::Top);
  equator_.IsHitTestVisible(false);
  gridLayer_.Children().Append(equator_);

  meridian_ = MakeEllipse();
  meridian_.Stroke(colors::BackgroundBrush());
  gridLayer_.Children().Append(meridian_);

  pointCanvas_ = Canvas();
  pointCanvas_.HorizontalAlignment(HorizontalAlignment::Center);
  pointCanvas_.VerticalAlignment(VerticalAlignment::Center);
  pointCanvas_.IsHitTestVisible(false);
  gridLayer_.Children().Append(pointCanvas_);

  // ---- the connected state -------------------------------------------------
  // ConnectCanvasConnectedStateView: five 180pt circles, OPAQUE, that slide in
  // from off-canvas and occlude one another. Above the grid, exactly as iOS
  // stacks them (globe -> dots -> circles).
  blobLayer_ = Grid();
  blobLayer_.IsHitTestVisible(false);
  globe_.Children().Append(blobLayer_);
  for (int i = 0; i < 5; ++i) {
    Blob blob;
    blob.shape = MakeEllipse();
    blob.fill = colors::MakeBrush(colors::kUrCoral);
    blob.shape.Fill(blob.fill);
    blob.shift = TranslateTransform();
    blob.shape.RenderTransform(blob.shift);
    blob.shape.Visibility(Visibility::Collapsed);
    blobLayer_.Children().Append(blob.shape);
    blobs_.push_back(blob);
  }
  ShuffleBlobs();

  // ---- the disconnected state ---------------------------------------------
  idleLayer_ = Grid();
  idleLayer_.IsHitTestVisible(false);
  globe_.Children().Append(idleLayer_);

  pulse_ = MakeEllipse();
  pulse_.Fill(colors::MakeBrush(colors::kUrElectricBlue));
  pulseScale_ = ScaleTransform();
  pulseScale_.ScaleX(1);
  pulseScale_.ScaleY(1);
  pulse_.RenderTransformOrigin(Point{0.5f, 0.5f});
  pulse_.RenderTransform(pulseScale_);
  pulse_.Opacity(0);
  idleLayer_.Children().Append(pulse_);

  // 52pt blue ring, then a 50pt ring in the base colour, then the 48pt disc:
  // the middle one is what puts a hairline of background between the disc and
  // its ring. Three elements because iOS draws three.
  coreRing_ = MakeEllipse();
  coreRing_.Stroke(colors::MakeBrush(colors::kUrElectricBlue));
  idleLayer_.Children().Append(coreRing_);

  coreGap_ = MakeEllipse();
  coreGap_.Stroke(colors::CardBrush());
  idleLayer_.Children().Append(coreGap_);

  core_ = MakeEllipse();
  core_.Fill(colors::MakeBrush(colors::kUrElectricBlue));
  idleLayer_.Children().Append(core_);

  // ---- error / processing --------------------------------------------------
  // Both are a single faint glyph on the bare globe: on iOS these two states
  // REPLACE the canvas rather than overlaying it, so nothing else is drawn.
  glyph_ = FontIcon();
  glyph_.FontFamily(FontFamily(L"Segoe Fluent Icons"));
  glyph_.Glyph(L"\uE7BA");  // Segoe Fluent "Warning" (escaped: private-use area)
  glyph_.Foreground(colors::FaintBrush());
  glyph_.HorizontalAlignment(HorizontalAlignment::Center);
  glyph_.VerticalAlignment(VerticalAlignment::Center);
  glyph_.IsHitTestVisible(false);
  glyph_.Opacity(0);
  glyph_.Visibility(Visibility::Collapsed);
  globe_.Children().Append(glyph_);

  // ---- the mask, last ------------------------------------------------------
  mask_ = ParsePath(std::wstring(kInverseGlobeRect) + kGlobePath);
  mask_.Fill(colors::BackgroundBrush());
  globe_.Children().Append(mask_);

  // ---- keyboard focus, OUTSIDE the mask ------------------------------------
  focusRing_ = ParsePath(kGlobePath);
  focusRing_.Fill(nullptr);
  focusRing_.Stroke(colors::MakeBrush(colors::kOffWhite));
  focusRing_.StrokeThickness(2);
  focusRing_.Visibility(Visibility::Collapsed);
  host_.Children().Append(focusRing_);

  host_.SizeChanged([this](IInspectable const&, SizeChangedEventArgs const& args) {
    // width only: Layout() writes the host's Height, and reacting to that would
    // recurse. Height is derived from width, so this converges in one pass.
    if (std::abs(args.NewSize().Width - width_) < 0.5) return;
    width_ = args.NewSize().Width;
    Layout();
  });

  ApplyStateVisuals();
}

// ---- the ground -----------------------------------------------------------

winrt::Windows::UI::Color ConnectCanvas::ResolveGround() const {
  // Walk out until something opaque is painting behind us. The hero Button
  // itself is deliberately transparent (its chrome is suppressed in the
  // markup), so alpha is the test, not the first Background we meet.
  DependencyObject node = host_;
  for (int depth = 0; depth < 16 && node; ++depth) {
    node = VisualTreeHelper::GetParent(node);
    if (!node) break;
    Brush background{nullptr};
    if (auto panel = node.try_as<Panel>()) {
      background = panel.Background();
    } else if (auto border = node.try_as<Border>()) {
      background = border.Background();
    } else if (auto control = node.try_as<Control>()) {
      background = control.Background();
    }
    if (!background) continue;
    if (auto solid = background.try_as<SolidColorBrush>()) {
      const auto color = solid.Color();
      if (color.A == 255) return color;
    }
  }
  // Nothing opaque found (the hero is not parented yet, or the page paints
  // through to the window): the app background is what the window is cleared to.
  return colors::kBackground;
}

void ConnectCanvas::ApplyGround() {
  const auto ground = ResolveGround();
  if (SameColor(ground, ground_)) return;
  ground_ = ground;
  // The overlay must vanish into whatever is behind the hero. This is the one
  // line to change if the hero is ever moved onto a surface this walk cannot
  // see — a gradient, or an image.
  mask_.Fill(colors::MakeBrush(ground));
  const auto base = Lift(ground, hovered_ ? kGlobeLiftHover : kGlobeLift);
  globeFill_.Fill(colors::MakeBrush(base));
  // iOS strokes the 50pt gap ring in tintedBackgroundBase, i.e. in the globe's
  // own base colour, so it reads as a slot cut between the disc and its ring.
  coreGap_.Stroke(colors::MakeBrush(base));
}

// ---- layout ---------------------------------------------------------------

void ConnectCanvas::Layout() {
  if (width_ <= 0) return;
  ApplyGround();
  side_ = std::clamp(width_ - kSidePad * 2, kMinSide, kMaxSide);
  const double s = side_ / kIosCanvas;

  // The band height is derived from the globe, not the other way round: the host
  // is stretched horizontally by its parent, so width is the driven dimension
  // and height is ours to set.
  const double bandHeight = side_ + kBandPad * 2;
  // An unset FrameworkElement.Height is NaN, and EVERY comparison against NaN is
  // false — so a plain `abs(current - target) > 0.5` guard silently never fires
  // on the first pass, and the band then auto-sizes to its tallest child.
  const double currentHeight = host_.Height();
  if (std::isnan(currentHeight) || std::abs(currentHeight - bandHeight) > 0.5) {
    host_.Height(bandHeight);
  }

  SizeSquare(globe_, side_);
  globeClip_.Rect(Rect{0, 0, static_cast<float>(side_), static_cast<float>(side_)});
  SizeSquare(globeFill_, side_);
  SizeSquare(connectorBg_, side_);
  SizeSquare(mask_, side_);

  equator_.Height(kEquatorH * s);
  equator_.Margin(Thickness{0, kEquatorY * s, 0, 0});
  meridian_.Width(kMeridianW * s);
  meridian_.Height(kMeridianH * s);
  meridian_.StrokeThickness(kConnectorStroke * s);

  SizeSquare(pointCanvas_, side_);

  const double blobD = kBlobD * s;
  for (auto& blob : blobs_) {
    SizeSquare(blob.shape, blobD);
    blob.shift.X((blobsIn_ ? blob.fx : blob.ix) * side_);
    blob.shift.Y((blobsIn_ ? blob.fy : blob.iy) * side_);
  }

  SizeSquare(pulse_, kPulseD * s);
  SizeSquare(coreRing_, kCoreRingD * s);
  coreRing_.StrokeThickness(4 * s);
  SizeSquare(coreGap_, kCoreGapD * s);
  coreGap_.StrokeThickness(2 * s);
  SizeSquare(core_, kCoreD * s);

  glyph_.FontSize(kGlyphSize * s);

  SizeSquare(focusRing_, side_ + 8);
  focusRing_.StrokeThickness(2);

  cols_ = 0 < gridWidth_ ? static_cast<int32_t>(gridWidth_) : 0;
  if (0 < gridHeight_ && cols_ < static_cast<int32_t>(gridHeight_)) {
    // iOS scales by gridWidth alone; taking the larger of the two means a
    // non-square grid still lands entirely inside the globe rather than running
    // off the bottom of it.
    cols_ = static_cast<int32_t>(gridHeight_);
  }
  cell_ = 0 < cols_ ? side_ / cols_ : 0;

  LayoutPoints();
}

void ConnectCanvas::LayoutPoints() {
  if (cell_ <= 0) return;
  for (auto& entry : points_) {
    GridDot& p = entry.second;
    // ConnectCanvasConnectingStateView: centre = x * maxPointSize +
    // maxPointSize/2, and the dot's diameter IS maxPointSize, so neighbouring
    // dots touch. Nothing is culled at the rim — the mask does that now.
    const double cx = p.x * cell_ + cell_ / 2;
    const double cy = p.y * cell_ + cell_ / 2;
    SizeSquare(p.dot, cell_);
    Canvas::SetLeft(p.dot, cx - cell_ / 2);
    Canvas::SetTop(p.dot, cy - cell_ / 2);
    ApplyPoint(p);
  }
}

// ---- grid feed ------------------------------------------------------------

ConnectCanvas::PointState ConnectCanvas::ParsePointState(std::string const& value) {
  if (value == "Added") return PointState::Added;
  if (value == "InEvaluation") return PointState::InEvaluation;
  if (value == "EvaluationFailed") return PointState::EvaluationFailed;
  if (value == "NotAdded") return PointState::NotAdded;
  if (value == "Removed") return PointState::Removed;
  // an unrecognised state is a provider the SDK has not accepted; it must not
  // render as one that it has
  return PointState::InEvaluation;
}

winrt::Windows::UI::Color ConnectCanvas::ColorForPointState(PointState state) {
  // ConnectCanvasConnectingStateViewModel.colorForState, exactly. NotAdded and
  // EvaluationFailed really are the same coral on iOS: the grid says "this cell
  // is not carrying traffic", not why.
  switch (state) {
    case PointState::InEvaluation: return colors::kAccent;  // urLightYellow #EFF7BB
    case PointState::EvaluationFailed: return colors::kUrCoral;
    case PointState::NotAdded: return colors::kUrCoral;
    case PointState::Added: return colors::kUrGreen;
    // .urBlack.opacity(0) — the RGB matters, because the blend runs through it
    case PointState::Removed: return winrt::Windows::UI::Color{0, 0x10, 0x10, 0x10};
  }
  return colors::kAccent;
}

winrt::Windows::UI::Color ConnectCanvas::Blend(winrt::Windows::UI::Color from,
                                               winrt::Windows::UI::Color to, double t) {
  auto mix = [t](uint8_t a, uint8_t b) {
    return static_cast<uint8_t>(
        std::lround(static_cast<double>(a) + (static_cast<double>(b) - a) * t));
  };
  return winrt::Windows::UI::Color{mix(from.A, to.A), mix(from.R, to.R), mix(from.G, to.G),
                                   mix(from.B, to.B)};
}

void ConnectCanvas::SetGrid(std::vector<urnet::ProviderGridPoint> const& incoming,
                            int64_t width, int64_t height) {
  // iOS freezes the grid the moment the connection lands: the connecting view
  // stays mounted as the background layer under the connected circles, but stops
  // taking updates ("if isConnecting"). The dots are almost entirely occluded by
  // then, and freezing is also what lets the connected state settle to no work
  // at all on a machine that leaves the window open all day.
  if (state_ != State::Connecting) return;

  const bool shapeChanged = gridWidth_ != width || gridHeight_ != height;
  gridWidth_ = width;
  gridHeight_ = height;

  for (auto& entry : points_) entry.second.seen = false;

  for (auto const& point : incoming) {
    // a point with no client id is still a cell; key it by position so it is
    // diffed as one point rather than churning in and out every push
    const std::string key = point.ClientId && !point.ClientId->empty()
                                ? *point.ClientId
                                : std::to_string(point.X) + "," + std::to_string(point.Y);
    const PointState state = ParsePointState(point.State);
    auto it = points_.find(key);
    if (it == points_.end()) {
      if (kMaxPoints <= points_.size()) continue;
      GridDot p;
      p.dot = shapes::Ellipse();
      p.dot.IsHitTestVisible(false);
      p.brush = SolidColorBrush(ColorForPointState(state));
      p.dot.Fill(p.brush);
      p.scale = ScaleTransform();
      p.scale.ScaleX(0);
      p.scale.ScaleY(0);
      p.dot.RenderTransformOrigin(Point{0.5f, 0.5f});
      p.dot.RenderTransform(p.scale);
      p.x = point.X;
      p.y = point.Y;
      p.state = state;
      p.previous = state;
      p.colorProgress = 1;
      p.sizeProgress = 0;  // grow in
      p.seen = true;
      pointCanvas_.Children().Append(p.dot);
      points_.emplace(key, p);
    } else {
      GridDot& p = it->second;
      p.seen = true;
      p.x = point.X;
      p.y = point.Y;
      if (p.state != state) {
        p.previous = p.state;
        p.state = state;
        p.colorProgress = 0;
      }
    }
  }

  // points the SDK stopped reporting fade out through the Removed state rather
  // than disappearing between frames
  for (auto& entry : points_) {
    GridDot& p = entry.second;
    if (p.seen || p.state == PointState::Removed) continue;
    p.previous = p.state;
    p.state = PointState::Removed;
    p.colorProgress = 0;
  }

  if (shapeChanged) {
    Layout();
  } else {
    LayoutPoints();
  }

  animating_ = false;
  for (auto const& entry : points_) animating_ = animating_ || entry.second.Animating();
}

void ConnectCanvas::ApplyPoint(GridDot& p) {
  if (!p.brush || !p.scale) return;
  const auto target = ColorForPointState(p.state);
  p.brush.Color(p.colorProgress < 1
                    ? Blend(ColorForPointState(p.previous), target, p.colorProgress)
                    : target);
  // the view model's own easeInOut, applied to the grow-in
  const double t = p.sizeProgress;
  const double eased = t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
  p.scale.ScaleX(eased);
  p.scale.ScaleY(eased);
}

void ConnectCanvas::Tick() {
  // TransferChart's rule: the per-frame path costs nothing unless something is
  // genuinely moving. Everything that repeats is a storyboard, not this.
  if (!animating_) return;

  bool stillAnimating = false;
  std::vector<std::string> settled;
  for (auto& entry : points_) {
    GridDot& p = entry.second;
    if (p.colorProgress < 1) p.colorProgress = (std::min)(1.0, p.colorProgress + kPointStep);
    if (p.sizeProgress < 1) p.sizeProgress = (std::min)(1.0, p.sizeProgress + kPointStep);
    ApplyPoint(p);
    if (p.state == PointState::Removed && !p.Animating()) {
      settled.push_back(entry.first);
      continue;
    }
    stillAnimating = stillAnimating || p.Animating();
  }
  for (auto const& id : settled) {
    auto it = points_.find(id);
    if (it == points_.end()) continue;
    uint32_t index = 0;
    if (pointCanvas_.Children().IndexOf(it->second.dot, index)) {
      pointCanvas_.Children().RemoveAt(index);
    }
    points_.erase(it);
  }
  animating_ = stillAnimating;
}

void ConnectCanvas::ClearPoints() {
  pointCanvas_.Children().Clear();
  points_.clear();
  animating_ = false;
}

// ---- state ----------------------------------------------------------------

void ConnectCanvas::SetState(State state) {
  if (state_ == state) return;
  const bool wasLive = state_ == State::Connecting || state_ == State::Connected;
  state_ = state;
  const bool live = state_ == State::Connecting || state_ == State::Connected;
  // iOS unmounts the connecting view whenever the connection is neither in
  // flight nor up, which drops its whole animated-point map — so the next
  // connect grows the grid in from nothing instead of resuming a stale one.
  if (wasLive && !live) ClearPoints();
  ApplyStateVisuals();
}

void ConnectCanvas::Fade(UIElement const& element, double to, int64_t ms) {
  const bool show = 0 < to;
  if (show) element.Visibility(Visibility::Visible);
  if (!AnimationsEnabled() || !presentationActive_) {
    element.Opacity(to);
    // Collapsed, not merely Opacity 0: a zero-opacity element is still walked
    // and still composited. Measured on this hero, that difference was worth
    // several points of a core.
    if (!show) element.Visibility(Visibility::Collapsed);
    return;
  }
  if (std::abs(element.Opacity() - to) < 0.01) {
    if (!show) element.Visibility(Visibility::Collapsed);
    return;
  }
  anim::Storyboard sb;
  Add(sb, MakeDouble(element.Opacity(), to, ms, 0, Ease(anim::EasingMode::EaseInOut)), element,
      L"Opacity");
  if (!show) {
    sb.Completed([element](auto const&, auto const&) {
      if (element.Opacity() <= 0.01) element.Visibility(Visibility::Collapsed);
    });
  }
  sb.Begin();
}

void ConnectCanvas::ShuffleBlobs() {
  // ConnectCanvasConnectedStateView: five colours and five (initial, final)
  // offset pairs, both shuffled, so the overlaps differ on every connect. The
  // offsets are in canvas widths, which is how the Swift writes them
  // (canvasWidth / 3.5 and so on).
  std::array<winrt::Windows::UI::Color, 5> palette = {
      colors::kUrCoral,
      colors::kUrGreen,
      // urLightBlue (#D6E6F4) is the one blob colour with no token of its own in
      // UrColors.h, which is byte-matched across the clients and not ours to
      // extend. Two existing tokens blended land on #D9E1F8 — the same pale ice
      // blue to the eye, and no new palette entry.
      Blend(colors::kOffWhite, colors::kToggleAccent, 0.2),
      colors::kAccent,  // urLightYellow
      colors::kUrPink,  // .accent (#ED8FFF)
  };
  std::array<std::pair<double, double>, 5> initials = {
      std::pair{-1.0, -1.0}, std::pair{1.0, -1.0}, std::pair{-1.0, 1.0}, std::pair{1.0, 1.0},
      std::pair{1.0, 0.0}};
  std::array<std::pair<double, double>, 5> finals = {
      std::pair{-1.0 / 3.5, -1.0 / 4}, std::pair{1.0 / 4, -1.0 / 3},
      std::pair{-1.0 / 3, 1.0 / 4},    std::pair{1.0 / 5, 1.0 / 2.5},
      std::pair{1.0 / 4, 0.0}};

  std::array<int, 5> order = {0, 1, 2, 3, 4};
  std::shuffle(palette.begin(), palette.end(), rng_);
  std::shuffle(order.begin(), order.end(), rng_);
  for (size_t i = 0; i < blobs_.size() && i < order.size(); ++i) {
    blobs_[i].fill.Color(palette[i]);
    blobs_[i].ix = initials[order[i]].first;
    blobs_[i].iy = initials[order[i]].second;
    blobs_[i].fx = finals[order[i]].first;
    blobs_[i].fy = finals[order[i]].second;
  }
}

void ConnectCanvas::RunBlobs(bool in) {
  const bool wasIn = blobsIn_;
  blobsIn_ = in;
  Stop(blobSb_);
  if (side_ <= 0) return;
  // A fresh pairing on every connect, as iOS reshuffles in onChange(of:isActive)
  if (in && !wasIn) ShuffleBlobs();

  auto settle = [&] {
    for (auto& blob : blobs_) {
      blob.shift.X((in ? blob.fx : blob.ix) * side_);
      blob.shift.Y((in ? blob.fy : blob.iy) * side_);
      // The circles are opaque and never fade on iOS — off-canvas is how they
      // hide. Collapsing them there as well is ours: five sprites parked outside
      // the clip still cost a walk on every frame the window presents.
      blob.shape.Visibility(in ? Visibility::Visible : Visibility::Collapsed);
    }
  };
  if (wasIn == in || !AnimationsEnabled() || !presentationActive_) {
    settle();
    return;
  }
  for (auto& blob : blobs_) blob.shape.Visibility(Visibility::Visible);

  anim::Storyboard sb;
  const auto ease = Ease(anim::EasingMode::EaseInOut);
  for (auto& blob : blobs_) {
    const double fromX = (in ? blob.ix : blob.fx) * side_;
    const double toX = (in ? blob.fx : blob.ix) * side_;
    const double fromY = (in ? blob.iy : blob.fy) * side_;
    const double toY = (in ? blob.fy : blob.iy) * side_;
    Add(sb, MakeDouble(fromX, toX, kBlobMs, 0, ease), blob.shift, L"X");
    Add(sb, MakeDouble(fromY, toY, kBlobMs, 0, ease), blob.shift, L"Y");
  }
  if (!in) {
    sb.Completed([this](auto const&, auto const&) {
      if (blobsIn_) return;
      for (auto& blob : blobs_) blob.shape.Visibility(Visibility::Collapsed);
    });
  }
  blobSb_ = sb;
  sb.Begin();
}

void ConnectCanvas::ApplyStateVisuals() {
  const bool connecting = state_ == State::Connecting;
  const bool connected = state_ == State::Connected;
  const bool disconnected = state_ == State::Disconnected;
  const bool blocked = state_ == State::Error || state_ == State::Processing;

  // ConnectButtonView's branches, in its order. Error and Processing are not an
  // overlay on the canvas: they REPLACE it, so nothing else is drawn under them.
  Fade(idleLayer_, disconnected ? 1.0 : 0.0, kStateFadeMs);
  Fade(gridLayer_, (connecting || connected) ? 1.0 : 0.0, kStateFadeMs);

  RunBlobs(connected);

  if (blocked) {
    glyph_.Glyph(state_ == State::Error ? L"\uE7BA"     // Warning
                                        : L"\uE823");   // Recent (the waiting dial)
    glyph_.Visibility(Visibility::Visible);
    glyph_.Opacity(0);
    // iOS delays both glyphs 500ms and then fades them in, so a transient
    // balance blip never flashes a warning at anyone
    if (AnimationsEnabled() && presentationActive_) {
      anim::Storyboard sb;
      Add(sb, MakeDouble(0, 1, kGlyphFadeMs, kGlyphDelayMs, Ease(anim::EasingMode::EaseInOut)),
          glyph_, L"Opacity");
      sb.Begin();
    } else {
      glyph_.Opacity(1);
    }
  } else {
    glyph_.Opacity(0);
    glyph_.Visibility(Visibility::Collapsed);
  }

  StopAll();
  StartIdle();
}

void ConnectCanvas::StopAll() {
  Stop(pulseSb_);
  if (pulse_) pulse_.Opacity(0);
}

void ConnectCanvas::StartIdle() {
  // A hidden window animates nothing: this is a tray app and the common case is
  // that nobody is looking at it.
  if (!presentationActive_ || !AnimationsEnabled()) {
    if (pulse_) pulse_.Opacity(0);
    return;
  }
  if (state_ != State::Disconnected) return;

  // ConnectCanvasDisconnectedStateView: a 56pt disc that scales to 1.5 while its
  // opacity runs 1.5 - scale, easeOut over 1.5s, `.repeatForever`.
  //
  // NOT repeatForever here. A running storyboard keeps the XAML island
  // presenting every frame whatever it animates, and that cost is flat. Measured
  // by interleaving both conditions inside ONE process (absolute percentages on
  // this box swing several points between runs, so only a paired A/B is worth
  // anything): pointer parked off the hero against the pointer re-entering it
  // every 3s, which restarts this burst and keeps it running for the sample —
  //     settled ....... 0.42% / 0.62% of one core
  //     animating ..... +0.8 to +3.3 points on top
  // A phone screen sleeps; a tray app's window does not, so the invitation is
  // bounded: three cycles when the page is shown, and three more whenever the
  // pointer arrives over the hero — which is exactly when a desktop user is
  // deciding whether this thing is clickable. In between, the hero is still, and
  // still is fine: it is a lit globe, not an empty screen.
  const auto easeOut = Ease(anim::EasingMode::EaseOut);
  anim::Storyboard sb;
  auto repeating = [&](double from, double to) {
    auto a = MakeDouble(from, to, kPulseMs, 0, easeOut);
    a.RepeatBehavior(Times(kIdlePulseBursts));
    return a;
  };
  Add(sb, repeating(1.0, kPulseScaleTo), pulseScale_, L"ScaleX");
  Add(sb, repeating(1.0, kPulseScaleTo), pulseScale_, L"ScaleY");
  Add(sb, repeating(kPulseOpacityFrom, 0.0), pulse_, L"Opacity");
  pulseSb_ = sb;
  sb.Begin();
}

void ConnectCanvas::SetPresentationActive(bool active) {
  if (presentationActive_ == active) return;
  presentationActive_ = active;
  if (!active) {
    StopAll();
    Stop(blobSb_);
    return;
  }
  if (blobsIn_) {
    // re-shown while connected: settle the circles where they belong rather than
    // replaying the entrance every time the window comes back from the tray
    for (auto& blob : blobs_) {
      blob.shift.X(blob.fx * side_);
      blob.shift.Y(blob.fy * side_);
      blob.shape.Visibility(Visibility::Visible);
    }
  }
  StartIdle();
}

void ConnectCanvas::SetHovered(bool hovered) {
  if (hovered_ == hovered) return;
  hovered_ = hovered;
  // Neither phone client has a pointer, so this affordance is ours. It stays
  // inside the shape language: the globe warms one card step and lifts, rather
  // than growing chrome iOS does not have.
  globeFill_.Fill(
      colors::MakeBrush(Lift(ground_, hovered ? kGlobeLiftHover : kGlobeLift)));
  // Replay the bounded idle burst on ARRIVAL only. Leaving is not a reason to
  // start one more round nobody asked for.
  StopAll();
  if (hovered) StartIdle();
  const double target = hovered ? 1.03 : 1.0;
  if (!AnimationsEnabled()) {
    globeScale_.ScaleX(target);
    globeScale_.ScaleY(target);
    return;
  }
  anim::Storyboard sb;
  const auto ease = Ease(anim::EasingMode::EaseOut);
  Add(sb, MakeDouble(globeScale_.ScaleX(), target, 180, 0, ease), globeScale_, L"ScaleX");
  Add(sb, MakeDouble(globeScale_.ScaleY(), target, 180, 0, ease), globeScale_, L"ScaleY");
  sb.Begin();
}

void ConnectCanvas::SetFocusRingVisible(bool visible) {
  focusRing_.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
}

}  // namespace urnw
