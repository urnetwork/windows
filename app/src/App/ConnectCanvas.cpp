// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ConnectCanvas.h"

#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

// The disc tracks the host width. The floor keeps it legible in the 480-dip
// default window; the ceiling stops it becoming a billboard when maximized (the
// connect column is MaxWidth-capped anyway, so in practice this is the range the
// hero actually moves through).
constexpr double kDiscFraction = 0.45;
constexpr double kMinDisc = 140;
constexpr double kMaxDisc = 300;
// vertical breathing room around the disc inside the hero band
constexpr double kBandPadding = 26;

// The outer rim, at rest and under the pointer. A 3.5% scale lift alone was not
// legible in a side-by-side capture at this size, and a hover affordance nobody
// can see is not one; brightening the rim is what actually reads.
constexpr double kRimAlpha = 0.11;
constexpr double kRimAlphaHover = 0.34;

// How long the idle invitation runs before the hero goes still. See the long
// note with the measurements in StartIdle: a running storyboard costs real CPU
// whatever it animates, so these are budgets, not aesthetics. Bounded in BOTH
// directions — hovering restarts the burst, it does not pin it on, because
// "pointer resting on the hero" is not a reason to animate forever either.
constexpr double kIdlePulseBursts = 3;   // ~5.7s of pulse on show / on hover
constexpr double kConnectedBreaths = 2;  // ~17s of wash breathe on connect

// A grid with no reported width still gets a substrate, so the empty state has
// structure. 12 is the shape a real connect grid settles near.
constexpr int32_t kDefaultCols = 12;
// Above this the substrate is more noise than structure and the element count
// stops being free; the live points are still drawn.
constexpr int32_t kMaxLatticeCells = 1600;
// Hard cap on live dots. A grid this large is not something the SDK produces;
// the cap exists so a malformed push cannot spawn unbounded elements.
constexpr size_t kMaxPoints = 1024;

// 10 fps * 0.12 = ~0.83s to settle, the same duration iOS reaches at
// 60 fps * 0.05.
constexpr double kPointStep = 0.12;

anim::RepeatBehavior Forever() {
  anim::RepeatBehavior r{};
  r.Type = anim::RepeatBehaviorType::Forever;
  return r;
}

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

// Add `animation` to `sb`, targeting `property` on `target`.
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

// A soft blob / wash: coloured at the centre, zero alpha at the rim. Used for
// both the ambient wash and the connected-state blobs, which is why nothing in
// this file needs a circular clip.
RadialGradientBrush SoftBrush(winrt::Windows::UI::Color color, uint8_t centerAlpha,
                              double midStop, uint8_t midAlpha) {
  RadialGradientBrush brush;
  brush.Center(Point{0.5f, 0.5f});
  brush.GradientOrigin(Point{0.5f, 0.5f});
  brush.RadiusX(0.5);
  brush.RadiusY(0.5);
  auto stop = [&](double offset, uint8_t alpha) {
    GradientStop s;
    s.Offset(offset);
    s.Color(colors::WithAlpha(color, alpha));
    brush.GradientStops().Append(s);
  };
  stop(0, centerAlpha);
  stop(midStop, midAlpha);
  stop(1, 0);
  return brush;
}

shapes::Ellipse MakeEllipse(double size) {
  shapes::Ellipse e;
  e.Width(size);
  e.Height(size);
  e.HorizontalAlignment(HorizontalAlignment::Center);
  e.VerticalAlignment(VerticalAlignment::Center);
  e.IsHitTestVisible(false);
  return e;
}

// A single arc drawn as a dashed circle: one dash `fraction` of the way round
// and one gap covering the rest. StrokeDashArray is in units of the stroke
// thickness, so the dash length is (fraction * circumference / thickness).
// Cheaper and far less fragile than a PathGeometry ArcSegment, and it rotates
// about the element's own centre with no geometry rebuild.
void SetArc(shapes::Ellipse const& e, double diameter, double thickness, double fraction) {
  e.Width(diameter);
  e.Height(diameter);
  e.StrokeThickness(thickness);
  const double circumference = 3.14159265358979 * diameter / (std::max)(thickness, 0.1);
  DoubleCollection dashes;
  dashes.Append(circumference * fraction);
  dashes.Append(circumference * (1 - fraction));
  e.StrokeDashArray(dashes);
}

}  // namespace

// ---------------------------------------------------------------------------

ConnectCanvas::ConnectCanvas(Grid const& host) { BuildVisuals(host); }

ConnectCanvas::~ConnectCanvas() { StopAll(); }

bool ConnectCanvas::AnimationsEnabled() const {
  // "Show animations in Windows" off means the user wants motion gone, not
  // reduced. Every repeating storyboard and the progress ring honour it.
  try {
    return winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled();
  } catch (...) {
    return true;
  }
}

void ConnectCanvas::BuildVisuals(Grid const& host) {
  host_ = host;

  // ---- ambient wash: the widest element, and the one that stops the hero
  // reading as a circle floating in black on a desktop-width window ----
  auto makeWash = [] {
    shapes::Ellipse e;
    e.HorizontalAlignment(HorizontalAlignment::Center);
    e.VerticalAlignment(VerticalAlignment::Center);
    e.IsHitTestVisible(false);
    e.Opacity(0);
    return e;
  };
  washA_ = makeWash();
  washB_ = makeWash();
  activeWash_ = washA_;
  host_.Children().Append(washA_);
  host_.Children().Append(washB_);

  // ---- the disc ----
  disc_ = Grid();
  disc_.HorizontalAlignment(HorizontalAlignment::Center);
  disc_.VerticalAlignment(VerticalAlignment::Center);
  disc_.IsHitTestVisible(false);
  discScale_ = ScaleTransform();
  discScale_.ScaleX(1);
  discScale_.ScaleY(1);
  disc_.RenderTransformOrigin(Point{0.5f, 0.5f});
  disc_.RenderTransform(discScale_);
  host_.Children().Append(disc_);

  // The grid substrate is a separate layer from the glyph/progress/focus ring
  // so the error and processing states can dim the grid WITHOUT dimming the
  // thing they are asking the user to read.
  substrate_ = Grid();
  substrate_.IsHitTestVisible(false);
  disc_.Children().Append(substrate_);

  auto makeRing = [&](double alpha) {
    shapes::Ellipse e = MakeEllipse(0);
    e.Stroke(colors::MakeBrush(
        colors::WithAlpha(colors::kOffWhite, static_cast<uint8_t>(alpha * 255))));
    e.StrokeThickness(1);
    substrate_.Children().Append(e);
    return e;
  };
  ring0_ = makeRing(kRimAlpha);
  ring1_ = makeRing(0.08);
  ring2_ = makeRing(0.06);

  latticeCanvas_ = Canvas();
  latticeCanvas_.HorizontalAlignment(HorizontalAlignment::Center);
  latticeCanvas_.VerticalAlignment(VerticalAlignment::Center);
  latticeCanvas_.IsHitTestVisible(false);
  substrate_.Children().Append(latticeCanvas_);

  // Connected-state blobs, under the points so the providers stay readable.
  // The colours and the sliding entrance are iOS's ConnectCanvasConnectedState;
  // the entry vectors are FIXED here rather than shuffled per connect, because
  // a desktop window is re-shown far more often than a phone screen is opened
  // and a hero that rearranges itself on every glance reads as instability.
  const std::array<winrt::Windows::UI::Color, 5> blobColors = {
      colors::kUrCoral, colors::kUrGreen, colors::kToggleAccent, colors::kAccent,
      colors::kUrPink};
  // Kept inside the rim on purpose: max extent is blobRadius (0.31) + offset
  // (0.18) = 0.49 of the diameter, just short of 0.5. The first version used
  // iOS's proportions, which rely on a globe MASK this canvas does not have, and
  // the capture showed the colour smeared well past the disc.
  // Spread, not stacked. iOS's circles are OPAQUE and occlude one another, so
  // overlap there produces clean colour fields; these are additive gradients, so
  // five of them piled near the centre blend to near-white — which is what the
  // capture showed. The fifth sits out to the right (iOS's own (w/4, 0)) instead
  // of at the middle.
  const std::array<std::pair<double, double>, 5> finals = {
      std::pair{-0.17, -0.15}, std::pair{0.16, -0.17}, std::pair{-0.17, 0.15},
      std::pair{0.14, 0.18}, std::pair{0.19, 0.0}};
  // The colour blooms OUTWARD FROM THE CENTRE, where iOS slides it in from off
  // the canvas. iOS can do that because its canvas is masked by the globe
  // symbol; this one is not, and the motion burst showed the blobs plainly
  // visible outside the disc for ~300ms of the entrance. Starting them stacked
  // at the centre and letting them spread keeps the gesture — colour arriving
  // and filling the grid — with nothing ever outside the rim.
  const std::array<std::pair<double, double>, 5> entries = {
      std::pair{0.0, 0.0}, std::pair{0.0, 0.0}, std::pair{0.0, 0.0},
      std::pair{0.0, 0.0}, std::pair{0.0, 0.0}};
  for (size_t i = 0; i < blobColors.size(); ++i) {
    Blob blob;
    blob.shape = MakeEllipse(0);
    blob.shape.Fill(SoftBrush(blobColors[i], 132, 0.45, 52));
    blob.shift = TranslateTransform();
    blob.shape.RenderTransform(blob.shift);
    blob.shape.Opacity(0);
    blob.fx = finals[i].first;
    blob.fy = finals[i].second;
    blob.ix = entries[i].first;
    blob.iy = entries[i].second;
    substrate_.Children().Append(blob.shape);
    blobs_.push_back(blob);
  }

  pointCanvas_ = Canvas();
  pointCanvas_.HorizontalAlignment(HorizontalAlignment::Center);
  pointCanvas_.VerticalAlignment(VerticalAlignment::Center);
  pointCanvas_.IsHitTestVisible(false);
  substrate_.Children().Append(pointCanvas_);

  // ---- connecting arcs ----
  auto makeArc = [&](winrt::Windows::UI::Color color, RotateTransform& spin) {
    shapes::Ellipse e = MakeEllipse(0);
    e.Stroke(colors::MakeBrush(color));
    e.StrokeDashCap(PenLineCap::Round);
    e.Visibility(Visibility::Collapsed);
    spin = RotateTransform();
    e.RenderTransformOrigin(Point{0.5f, 0.5f});
    e.RenderTransform(spin);
    substrate_.Children().Append(e);
    return e;
  };
  arcOuter_ = makeArc(colors::kAccent, arcOuterSpin_);
  arcInner_ = makeArc(colors::kToggleAccent, arcInnerSpin_);

  // ---- disconnected core ----
  pulse_ = MakeEllipse(0);
  pulse_.Fill(colors::MakeBrush(colors::WithAlpha(colors::kStatusIdle, 150)));
  pulseScale_ = ScaleTransform();
  pulseScale_.ScaleX(1);
  pulseScale_.ScaleY(1);
  pulse_.RenderTransformOrigin(Point{0.5f, 0.5f});
  pulse_.RenderTransform(pulseScale_);
  pulse_.Opacity(0);
  substrate_.Children().Append(pulse_);

  // iOS draws the idle core as an electric-blue disc inside an electric-blue
  // ring with a background-coloured gap between them; the ring here is the same
  // idea, with the gap coming from the stroke sitting outside the fill.
  coreRing_ = MakeEllipse(0);
  coreRing_.Stroke(colors::MakeBrush(colors::kUrElectricBlue));
  coreRing_.StrokeThickness(3);
  substrate_.Children().Append(coreRing_);

  core_ = MakeEllipse(0);
  core_.Fill(colors::MakeBrush(colors::kStatusIdle));
  substrate_.Children().Append(core_);

  // ---- error / processing (siblings of the substrate, never dimmed) ----
  glyph_ = FontIcon();
  glyph_.FontFamily(FontFamily(L"Segoe Fluent Icons"));
  glyph_.Glyph(L"\uE7BA");  // Segoe Fluent "Warning" (escaped: private-use area)
  glyph_.Foreground(colors::MakeBrush(colors::kUrCoral));
  glyph_.HorizontalAlignment(HorizontalAlignment::Center);
  glyph_.VerticalAlignment(VerticalAlignment::Center);
  glyph_.IsHitTestVisible(false);
  glyph_.Opacity(0);
  disc_.Children().Append(glyph_);

  // the native Windows idiom for "we are waiting on something", instead of
  // iOS's static hourglass glyph
  progress_ = ProgressRing();
  progress_.IsActive(false);
  progress_.Visibility(Visibility::Collapsed);
  progress_.HorizontalAlignment(HorizontalAlignment::Center);
  progress_.VerticalAlignment(VerticalAlignment::Center);
  progress_.IsHitTestVisible(false);
  progress_.Foreground(colors::MakeBrush(colors::kAccent));
  disc_.Children().Append(progress_);

  // ---- keyboard focus ring ----
  focusRing_ = MakeEllipse(0);
  focusRing_.Stroke(colors::MakeBrush(colors::kOffWhite));
  focusRing_.StrokeThickness(2);
  focusRing_.Visibility(Visibility::Collapsed);
  disc_.Children().Append(focusRing_);

  host_.SizeChanged([this](IInspectable const&, SizeChangedEventArgs const& args) {
    // width only: Layout() writes the host's Height, and reacting to that would
    // recurse. Height is derived from width, so this converges in one pass.
    if (std::abs(args.NewSize().Width - width_) < 0.5) return;
    width_ = args.NewSize().Width;
    Layout();
  });

  ApplyStateVisuals();
}

// ---- layout ---------------------------------------------------------------

void ConnectCanvas::Layout() {
  if (width_ <= 0) return;
  disc_d_ = std::clamp(width_ * kDiscFraction, kMinDisc, kMaxDisc);

  // The band height is derived from the disc, not the other way round: the host
  // is stretched horizontally by its parent, so width is the driven dimension
  // and height is ours to set.
  const double bandHeight = disc_d_ + kBandPadding * 2;
  // An unset FrameworkElement.Height is NaN, and EVERY comparison against NaN is
  // false — so a plain `abs(current - target) > 0.5` guard silently never fires
  // on the first pass, and the band then auto-sizes to its tallest child (the
  // wash), which is half again too tall. Found by looking at the first capture,
  // not by reading this back.
  const double currentHeight = host_.Height();
  if (std::isnan(currentHeight) || std::abs(currentHeight - bandHeight) > 0.5) {
    host_.Height(bandHeight);
  }

  // The wash fills the band exactly and no more. A Grid does not clip its
  // children, so an oversized wash bleeds into the rows above and below; and
  // letting the ScrollViewer trim it instead would cut a gradient mid-fade and
  // leave a hard edge.
  for (auto const& wash : {washA_, washB_}) {
    wash.Width(width_);
    wash.Height(bandHeight);
  }

  disc_.Width(disc_d_);
  disc_.Height(disc_d_);
  ring0_.Width(disc_d_);
  ring0_.Height(disc_d_);
  ring1_.Width(disc_d_ * 0.72);
  ring1_.Height(disc_d_ * 0.72);
  ring2_.Width(disc_d_ * 0.44);
  ring2_.Height(disc_d_ * 0.44);

  SetArc(arcOuter_, disc_d_ * 0.9, 5, 0.3);
  SetArc(arcInner_, disc_d_ * 0.64, 3.5, 0.2);

  const double coreD = (std::max)(disc_d_ * 0.19, 34.0);
  core_.Width(coreD);
  core_.Height(coreD);
  coreRing_.Width(coreD + 9);
  coreRing_.Height(coreD + 9);
  pulse_.Width(coreD);
  pulse_.Height(coreD);

  const double blobD = disc_d_ * 0.62;
  for (auto& blob : blobs_) {
    blob.shape.Width(blobD);
    blob.shape.Height(blobD);
    blob.shift.X((blobsIn_ ? blob.fx : blob.ix) * disc_d_);
    blob.shift.Y((blobsIn_ ? blob.fy : blob.iy) * disc_d_);
    blob.shape.Opacity(blobsIn_ ? 1.0 : 0.0);
  }

  glyph_.FontSize((std::max)(disc_d_ * 0.21, 32.0));
  progress_.Width(disc_d_ * 0.34);
  progress_.Height(disc_d_ * 0.34);

  focusRing_.Width(disc_d_ + 10);
  focusRing_.Height(disc_d_ + 10);

  latticeCanvas_.Width(disc_d_);
  latticeCanvas_.Height(disc_d_);
  pointCanvas_.Width(disc_d_);
  pointCanvas_.Height(disc_d_);

  const int32_t cols = 0 < gridWidth_ ? static_cast<int32_t>(gridWidth_) : kDefaultCols;
  const int32_t rows = 0 < gridHeight_ ? static_cast<int32_t>(gridHeight_) : cols;
  cols_ = (std::max)(cols, 1);
  rows_ = (std::max)(rows, 1);
  cell_ = disc_d_ / (std::max)(cols_, rows_);
  originX_ = (disc_d_ - cols_ * cell_) / 2;
  originY_ = (disc_d_ - rows_ * cell_) / 2;

  LayoutLattice();
  LayoutPoints();
}

void ConnectCanvas::LayoutLattice() {
  latticeCanvas_.Children().Clear();
  lattice_.clear();
  if (cell_ <= 0) return;
  if (kMaxLatticeCells < cols_ * rows_) return;

  const double dotD = (std::max)(cell_ * 0.22, 1.5);
  const double radius = disc_d_ / 2;
  const auto brush = colors::MakeBrush(colors::WithAlpha(colors::kTextFaint, 130));
  for (int32_t y = 0; y < rows_; ++y) {
    for (int32_t x = 0; x < cols_; ++x) {
      const double cx = originX_ + (x + 0.5) * cell_;
      const double cy = originY_ + (y + 0.5) * cell_;
      // culled rather than clipped: UIElement.Clip takes a RectangleGeometry only
      if (radius - dotD < std::hypot(cx - radius, cy - radius)) continue;
      shapes::Ellipse dot;
      dot.Width(dotD);
      dot.Height(dotD);
      dot.Fill(brush);
      dot.IsHitTestVisible(false);
      Canvas::SetLeft(dot, cx - dotD / 2);
      Canvas::SetTop(dot, cy - dotD / 2);
      latticeCanvas_.Children().Append(dot);
      lattice_.push_back(dot);
    }
  }
}

void ConnectCanvas::LayoutPoints() {
  if (cell_ <= 0) return;
  const double dotD = (std::max)(cell_ * 0.66, 2.0);
  const double radius = disc_d_ / 2;
  for (auto& entry : points_) {
    GridDot& p = entry.second;
    const double cx = originX_ + (p.x + 0.5) * cell_;
    const double cy = originY_ + (p.y + 0.5) * cell_;
    p.culled = radius - dotD / 2 < std::hypot(cx - radius, cy - radius);
    p.dot.Visibility(p.culled ? Visibility::Collapsed : Visibility::Visible);
    if (p.culled) continue;
    p.dot.Width(dotD);
    p.dot.Height(dotD);
    Canvas::SetLeft(p.dot, cx - dotD / 2);
    Canvas::SetTop(p.dot, cy - dotD / 2);
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
  switch (state) {
    // amber: under evaluation. iOS uses its light yellow here; kUrAmber is this
    // palette's equivalent step and is already the app's "working on it" colour
    // (the peers line at zero uses it).
    case PointState::InEvaluation: return colors::kUrAmber;
    case PointState::EvaluationFailed: return colors::kUrCoral;
    // iOS paints NotAdded the same coral as EvaluationFailed. A desktop canvas
    // has the room to tell them apart, and they mean different things — "this
    // provider failed" against "this provider was fine and not chosen" — so
    // NotAdded takes the muted coral step.
    case PointState::NotAdded: return colors::kUrMutedCoral;
    case PointState::Added: return colors::kUrGreen;
    case PointState::Removed: return colors::WithAlpha(colors::kUrGreen, 0);
  }
  return colors::kUrAmber;
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

winrt::Windows::UI::Color ConnectCanvas::AccentForState(State state) {
  switch (state) {
    // the same tokens the status dot directly above the hero uses, so the two
    // can never disagree about what colour a state is
    case State::Disconnected: return colors::kStatusIdle;
    case State::Connecting: return colors::kStatusConnecting;
    case State::Connected: return colors::kUrGreen;
    case State::Error: return colors::kUrCoral;
    case State::Processing: return colors::kTextMuted;
  }
  return colors::kStatusIdle;
}

void ConnectCanvas::SetGrid(std::vector<urnet::ProviderGridPoint> const& incoming,
                            int64_t width, int64_t height) {
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
  // cubic ease-in-out on the grow-in, matching iOS's easeInOut point animation
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
    if (!p.culled) ApplyPoint(p);
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

// ---- state ----------------------------------------------------------------

void ConnectCanvas::SetState(State state) {
  if (state_ == state) return;
  state_ = state;
  ApplyStateVisuals();
}

void ConnectCanvas::ApplyStateVisuals() {
  const bool connecting = state_ == State::Connecting;
  const bool connected = state_ == State::Connected;
  const bool disconnected = state_ == State::Disconnected;
  const bool blocked = state_ == State::Error || state_ == State::Processing;

  // the wash cross-fade: the new brush is built on the hidden ellipse and the
  // two swap opacity. RadialGradientBrush derives from XamlCompositionBrushBase,
  // so its stops are not Storyboard targets and the colour cannot be animated
  // in place.
  auto const outgoing = activeWash_;
  auto const incoming = activeWash_ == washA_ ? washB_ : washA_;
  activeWash_ = incoming;
  incoming.Fill(
      SoftBrush(AccentForState(state_), connected ? 92 : 68, 0.5, connected ? 34 : 24));
  incoming.Visibility(Visibility::Visible);
  if (AnimationsEnabled()) {
    anim::Storyboard sb;
    Add(sb, MakeDouble(outgoing.Opacity(), 0, 420, 0, Ease(anim::EasingMode::EaseOut)),
        outgoing, L"Opacity");
    Add(sb, MakeDouble(0, 1, 420, 0, Ease(anim::EasingMode::EaseOut)), incoming, L"Opacity");
    // Collapse the faded-out wash rather than leaving it at Opacity 0. A
    // RadialGradientBrush is a composition effect brush and a zero-opacity
    // element still carries one; Collapsed takes it out of the render pass
    // entirely. Measured: the hero's static cost was 7.5% of a core against a
    // 3.0% page without it, and seven of these brushes were the reason.
    sb.Completed([outgoing](auto const&, auto const&) {
      if (outgoing.Opacity() <= 0.01) outgoing.Visibility(Visibility::Collapsed);
    });
    sb.Begin();
  } else {
    outgoing.Opacity(0);
    outgoing.Visibility(Visibility::Collapsed);
    incoming.Opacity(1);
  }

  // error / processing dim the GRID, not the glyph: the substrate is a separate
  // layer precisely so the thing being read stays at full contrast
  substrate_.Opacity(blocked ? 0.45 : 1.0);

  // the live points are the SDK's providers: they mean nothing while
  // disconnected, and nothing while a balance state is blocking the connection
  pointCanvas_.Visibility(connecting || connected ? Visibility::Visible
                                                  : Visibility::Collapsed);

  arcOuter_.Visibility(connecting ? Visibility::Visible : Visibility::Collapsed);
  arcInner_.Visibility(connecting ? Visibility::Visible : Visibility::Collapsed);

  core_.Visibility(disconnected ? Visibility::Visible : Visibility::Collapsed);
  coreRing_.Visibility(disconnected ? Visibility::Visible : Visibility::Collapsed);
  pulse_.Opacity(0);

  glyph_.Opacity(0);
  glyph_.Visibility(state_ == State::Error ? Visibility::Visible : Visibility::Collapsed);
  progress_.Visibility(state_ == State::Processing ? Visibility::Visible
                                                   : Visibility::Collapsed);

  if (state_ == State::Error) {
    // iOS delays the warning 500ms and then fades it in, so a transient balance
    // blip never flashes a warning triangle at anyone
    if (AnimationsEnabled()) {
      anim::Storyboard sb;
      Add(sb, MakeDouble(0, 1, 300, 500, Ease(anim::EasingMode::EaseInOut)), glyph_,
          L"Opacity");
      sb.Begin();
    } else {
      glyph_.Opacity(1);
    }
  }

  RunBlobs(connected);
  StopAll();
  StartIdle();
}

void ConnectCanvas::RunBlobs(bool in) {
  const bool wasIn = blobsIn_;
  blobsIn_ = in;
  Stop(blobSb_);
  if (disc_d_ <= 0) return;
  // Settle immediately when there is nothing to animate between, when the user
  // has animations off, or when nobody is looking.
  auto settle = [&] {
    for (auto& blob : blobs_) {
      blob.shift.X((in ? blob.fx : blob.ix) * disc_d_);
      blob.shift.Y((in ? blob.fy : blob.iy) * disc_d_);
      blob.shape.Opacity(in ? 1.0 : 0.0);
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
  int index = 0;
  for (auto& blob : blobs_) {
    const double fromX = (in ? blob.ix : blob.fx) * disc_d_;
    const double toX = (in ? blob.fx : blob.ix) * disc_d_;
    const double fromY = (in ? blob.iy : blob.fy) * disc_d_;
    const double toY = (in ? blob.fy : blob.iy) * disc_d_;
    const int64_t begin = in ? 60 * index : 0;
    Add(sb, MakeDouble(fromX, toX, 1000, begin, ease), blob.shift, L"X");
    Add(sb, MakeDouble(fromY, toY, 1000, begin, ease), blob.shift, L"Y");
    Add(sb, MakeDouble(in ? 0.0 : 1.0, in ? 1.0 : 0.0, in ? 500 : 350, begin, ease),
        blob.shape, L"Opacity");
    ++index;
  }
  if (!in) {
    // Out: take the five radial-gradient brushes out of the render pass once
    // they have faded, not merely down to Opacity 0. Same reason as the wash.
    sb.Completed([this](auto const&, auto const&) {
      if (blobsIn_) return;
      for (auto& blob : blobs_) blob.shape.Visibility(Visibility::Collapsed);
    });
  }
  blobSb_ = sb;
  sb.Begin();
}

void ConnectCanvas::StopAll() {
  Stop(pulseSb_);
  Stop(spinSb_);
  Stop(breatheSb_);
  if (progress_) progress_.IsActive(false);
}

void ConnectCanvas::StartIdle() {
  // A hidden window animates nothing: this is a tray app and the common case is
  // that nobody is looking at it.
  if (!presentationActive_ || !AnimationsEnabled()) {
    pulse_.Opacity(0);
    if (activeWash_) activeWash_.Opacity(1);
    return;
  }

  if (state_ == State::Disconnected) {
    // An expanding, fading ring behind the core — iOS's idle pulse, but NOT
    // repeatForever.
    //
    // Measured on this machine, connect page, window visible and idle:
    //     no hero at all ................ 2.95% of one core
    //     hero, nothing animating ....... 7.54%
    //     hero + a repeatForever pulse .. 16.05%
    //
    // The +8.5 points is not the ring; it is the flat cost of ANY running
    // storyboard, which keeps the island presenting every frame. A tray app
    // that burns a third of a core to draw one breathing dot at somebody who
    // has walked away is not paying for what it costs.
    //
    // So the invitation is bounded: kIdlePulseBursts cycles when the page is
    // shown, and again whenever the pointer comes over the hero — which is
    // exactly when a desktop user is deciding whether this thing is clickable.
    // Between those, the hero is still, and still is fine: it is a dense grid
    // with a lit core, not an empty screen.
    const auto easeOut = Ease(anim::EasingMode::EaseOut);
    anim::Storyboard sb;
    auto repeating = [&](double from, double to) {
      auto a = MakeDouble(from, to, 1900, 0, easeOut);
      a.RepeatBehavior(Times(kIdlePulseBursts));
      return a;
    };
    Add(sb, repeating(1.0, 2.7), pulseScale_, L"ScaleX");
    Add(sb, repeating(1.0, 2.7), pulseScale_, L"ScaleY");
    Add(sb, repeating(0.55, 0.0), pulse_, L"Opacity");
    pulseSb_ = sb;
    sb.Begin();
    return;
  }

  if (state_ == State::Connecting) {
    // Two counter-rotating arcs. Angle on a RotateTransform is independently
    // animatable, so the compositor runs this off the UI thread and it costs
    // nothing per frame.
    anim::Storyboard sb;
    auto spin = [](double from, double to, int64_t ms) {
      // linear on purpose: an eased spin visibly stutters once per revolution
      auto a = MakeDouble(from, to, ms, 0, anim::EasingFunctionBase{nullptr});
      a.RepeatBehavior(Forever());
      return a;
    };
    Add(sb, spin(0, 360, 2600), arcOuterSpin_, L"Angle");
    Add(sb, spin(360, 0, 3900), arcInnerSpin_, L"Angle");
    spinSb_ = sb;
    sb.Begin();
    return;
  }

  if (state_ == State::Connected) {
    // A slow wash breathe. Bounded for the same measured reason as the pulse,
    // and bounded harder: connected is the state this app spends most of its
    // life in, and it is already the richest thing on the screen — five brand
    // fields and a live provider grid. It does not need to shimmer at anyone
    // indefinitely to prove it is working.
    anim::Storyboard sb;
    auto a = MakeDouble(1.0, 0.78, 4200, 0, Ease(anim::EasingMode::EaseInOut));
    a.AutoReverse(true);
    a.RepeatBehavior(Times(kConnectedBreaths));
    Add(sb, a, activeWash_, L"Opacity");
    breatheSb_ = sb;
    sb.Begin();
    return;
  }

  if (state_ == State::Processing) progress_.IsActive(true);
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
    // re-shown while connected: settle the blobs where they belong rather than
    // replaying the entrance every time the window comes back from the tray
    for (auto& blob : blobs_) {
      blob.shift.X(blob.fx * disc_d_);
      blob.shift.Y(blob.fy * disc_d_);
      blob.shape.Opacity(1);
    }
  }
  StartIdle();
}

void ConnectCanvas::SetHovered(bool hovered) {
  if (hovered_ == hovered) return;
  hovered_ = hovered;
  // the rim brightens (the legible part) and the disc lifts (the tactile part)
  ring0_.Stroke(colors::MakeBrush(colors::WithAlpha(
      colors::kOffWhite,
      static_cast<uint8_t>((hovered ? kRimAlphaHover : kRimAlpha) * 255))));
  // Replay the bounded idle burst on ARRIVAL only. Hover is precisely when the
  // invitation is worth paying for; leaving is not, so the pointer going away
  // just stops it rather than starting one more round nobody asked for.
  StopAll();
  if (hovered) StartIdle();
  const double target = hovered ? 1.05 : 1.0;
  if (!AnimationsEnabled()) {
    discScale_.ScaleX(target);
    discScale_.ScaleY(target);
    return;
  }
  anim::Storyboard sb;
  const auto ease = Ease(anim::EasingMode::EaseOut);
  Add(sb, MakeDouble(discScale_.ScaleX(), target, 180, 0, ease), discScale_, L"ScaleX");
  Add(sb, MakeDouble(discScale_.ScaleY(), target, 180, 0, ease), discScale_, L"ScaleY");
  sb.Begin();
}

void ConnectCanvas::SetFocusRingVisible(bool visible) {
  focusRing_.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
}

}  // namespace urnw
