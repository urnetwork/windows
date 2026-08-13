// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ProviderGlobe.h"

// PointerRoutedEventArgs::GetCurrentPoint returns a Microsoft::UI::Input::
// PointerPoint; pch.h does not pull that projection in.
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <memory>
#include <mutex>

#include "Log.h"
#include "Sdk.h"
#include "UrColors.h"
#include "resource.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Xaml::Shapes;

namespace urnw {
namespace {

// wingdi.h declares ::Ellipse; alias the XAML shape so unqualified lookup under
// the using-directives stays unambiguous
using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;

constexpr winrt::Windows::UI::Color kTransparent{0, 0, 0, 0};

// Visual constants matching the /ip globe on ur.io and the android port (see
// sdk/PROVIDERLOCATIONS.md). All lengths are in the 600-unit virtual space.
constexpr winrt::Windows::UI::Color kGraticuleColor{0x60, 0xCC, 0xCC, 0xCC};
// the web/android land fill; the brand kText white is a shade brighter
constexpr winrt::Windows::UI::Color kLandColor{255, 0xF8, 0xF8, 0xF8};
constexpr double kLandStrokeWidth = 0.3;
constexpr double kGraticuleStrokeWidth = 0.5;
constexpr float kDotRadius = 7.0f;
// the selected provider keeps its solid dot; the ring is an outline sitting
// kSelectedRingGap outside the dot's edge (radii are stroke centerlines)
constexpr float kSelectedRingGap = 4.0f;
constexpr float kSelectedRingStroke = 1.5f;
constexpr float kSelectedRingRadius = kDotRadius + kSelectedRingGap + kSelectedRingStroke / 2.0f;
constexpr winrt::Windows::UI::Color kUnknownCountryColor{255, 0x00, 0x99, 0xFF};
// The sphere is sized to fit its box with room for a selected dot's ring at the
// limb, so the globe never paints outside the component. (The web zooms past
// its frame and crops; here the globe sits fully inside instead.)
constexpr float kGlobeScale =
    GlobeGeometry::kCenter - kDotRadius - kSelectedRingGap - kSelectedRingStroke;
// Recentering is a per-selection interaction (every wheel step recenters), so
// it is much snappier than the web's 1000ms.
//
// It is a critically damped SPRING rather than a timing curve because those
// recenters overlap: a second one lands while the first is still running, and a
// spring continues from the rotation's current angle AND velocity, where a
// curve restarts from a standstill and reads as a stutter on every step.
// kRecenterResponseSeconds is the spring's period -- the same 0.45s as apple's
// Animation.spring(response: 0.45) and android's Spring.StiffnessLow, which
// lands the settle about where the old 500ms curve was.
//
// This clock is the window's shared ~10 fps drawer tick
// (ConnectPage::OnChartTick), so the spring is stepped ANALYTICALLY rather than
// by Euler: at 100ms omega*dt is ~1.4, close enough to the explicit
// integrator's stability limit that one hitched tick would ring or diverge.
constexpr double kRecenterResponseSeconds = 0.45;
// the spring is settled when it is this close to the target and this slow, in
// degrees and degrees per second
constexpr double kRecenterSettleDegrees = 0.01;
constexpr double kRecenterSettleDegreesPerSecond = 0.05;
// click slop for hitting a dot, in virtual px
constexpr float kTapSlop = 28.0f;
// how far the pointer travels to advance one provider, as a fraction of the
// globe's width
constexpr float kWheelStepWidthFraction = 0.18f;
// one mouse-wheel notch is WHEEL_DELTA; one notch steps one provider
constexpr float kWheelNotch = 120.0f;
// The selected provider's dot is its own country color darkened toward black.
// Same factor on every platform (see PROVIDERLOCATIONS.md), so the selection
// reads the same everywhere.
constexpr double kSelectedDotDarken = 0.55;

// The same color, darkened toward black for the selected provider's dot.
winrt::Windows::UI::Color DarkenForSelection(winrt::Windows::UI::Color const& color) {
  const auto scale = [](uint8_t channel) {
    return static_cast<uint8_t>(static_cast<double>(channel) * kSelectedDotDarken);
  };
  return winrt::Windows::UI::Color{color.A, scale(color.R), scale(color.G), scale(color.B)};
}

double NowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
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

// ---- the world topology, decoded once per process --------------------------
// ~100 KB of TopoJSON to parse and stitch: decoded on a background thread so
// opening the sheet never blocks the UI. The globe renders (sphere, graticule,
// dots) while the land is still loading, exactly like the android port. The
// decode touches only these statics, so it is safe however long it outlives any
// particular sheet.

std::mutex gTopologyMutex;
std::shared_ptr<const WorldTopology> gTopology;
bool gTopologyStarted = false;
bool gTopologyDone = false;

std::string LoadTopologyResource() {
  // world-110m.json is linked in as RCDATA (App.rc) rather than deployed beside
  // the exe: the app is unpackaged, so an embedded resource has no install-path
  // or working-directory question to get wrong.
  HRSRC found = ::FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_WORLD_TOPOLOGY), RT_RCDATA);
  if (!found) return {};
  HGLOBAL loaded = ::LoadResource(nullptr, found);
  if (!loaded) return {};
  const void* data = ::LockResource(loaded);
  const DWORD size = ::SizeofResource(nullptr, found);
  if (!data || size == 0) return {};
  return std::string(static_cast<const char*>(data), size);
}

winrt::fire_and_forget DecodeTopologyAsync() {
  co_await winrt::resume_background();
  std::shared_ptr<const WorldTopology> decoded;
  try {
    const std::string text = LoadTopologyResource();
    if (text.empty()) {
      LogWarn("providerglobe: world-110m.json resource missing");
    } else if (auto world = WorldTopology::Decode(text)) {
      decoded = std::make_shared<const WorldTopology>(std::move(*world));
    } else {
      LogWarn("providerglobe: world-110m.json failed to decode");
    }
  } catch (const std::exception& e) {
    LogWarn("providerglobe: world topology decode failed: {}", e.what());
  }
  std::scoped_lock lock(gTopologyMutex);
  gTopology = std::move(decoded);
  gTopologyDone = true;
}

std::shared_ptr<const WorldTopology> CurrentTopology() {
  std::scoped_lock lock(gTopologyMutex);
  return gTopology;
}

bool TopologyDone() {
  std::scoped_lock lock(gTopologyMutex);
  return gTopologyDone;
}

// The dot color is the provider's country color (the same palette the location
// list uses). Providers whose country is unknown fall back to the web globe's
// neutral blue.
winrt::Windows::UI::Color ProviderColor(const ProviderLocationRow& row) {
  if (row.countryCode.empty()) return kUnknownCountryColor;
  return ColorFromHex(urnet::getColorHex(row.countryCode));
}

// Advances one axis of a critically damped spring by `dt` seconds, in place.
// The closed form of x'' = -w^2 x - 2 w x' about the target:
//   x(t) = (x0 + (v0 + w x0) t) e^-wt,  v(t) = (v0 - w (v0 + w x0) t) e^-wt
// Exact at any step, so a slow or hitched tick cannot overshoot the way an
// Euler step would at this clock's 100ms. Returns whether it is still moving.
bool StepRecenterSpring(float& value, float& velocity, float target, double dt) {
  constexpr double omega = 2.0 * std::numbers::pi / kRecenterResponseSeconds;
  const double offset = static_cast<double>(value) - static_cast<double>(target);
  const double speed = static_cast<double>(velocity);
  const double slope = speed + omega * offset;
  const double decay = std::exp(-omega * dt);
  const double nextOffset = (offset + slope * dt) * decay;
  const double nextSpeed = (speed - omega * slope * dt) * decay;
  if (std::fabs(nextOffset) < kRecenterSettleDegrees &&
      std::fabs(nextSpeed) < kRecenterSettleDegreesPerSecond) {
    value = target;
    velocity = 0;
    return false;
  }
  value = static_cast<float>(static_cast<double>(target) + nextOffset);
  velocity = static_cast<float>(nextSpeed);
  return true;
}

// One closed ring / open polyline as a PathFigure. A single ReplaceAll sets the
// whole point run, which is what keeps a redraw of ~10,500 land points down to a
// few hundred WinRT calls.
PathFigure MakeFigure(const std::vector<Point>& points, bool closed) {
  PathFigure figure;
  figure.StartPoint(points.front());
  PolyLineSegment segment;
  segment.Points().ReplaceAll(
      winrt::array_view<Point const>{points.data() + 1, points.data() + points.size()});
  figure.Segments().Append(segment);
  figure.IsClosed(closed);
  figure.IsFilled(closed);
  return figure;
}

}  // namespace

ProviderGlobe::ProviderGlobe(Grid const& host) { BuildVisuals(host); }

ProviderGlobe::~ProviderGlobe() = default;

void ProviderGlobe::BuildVisuals(Grid const& host) {
  canvas_ = Canvas();
  canvas_.HorizontalAlignment(HorizontalAlignment::Stretch);
  canvas_.VerticalAlignment(VerticalAlignment::Stretch);
  // a Canvas only receives pointer input where it has a brush
  canvas_.Background(SolidColorBrush(kTransparent));
  sizeChangedRevoker_ = canvas_.SizeChanged(
      winrt::auto_revoke, [this](IInspectable const&, SizeChangedEventArgs const& args) {
        RectangleGeometry clip;
        clip.Rect(Rect{0, 0, args.NewSize().Width, args.NewSize().Height});
        canvas_.Clip(clip);
        Redraw();
      });
  host.Children().Append(canvas_);

  // the sphere: a dark disc the land and graticule sit on
  sphere_ = ShapeEllipse();
  sphere_.Fill(colors::BackgroundBrush());
  sphere_.IsHitTestVisible(false);
  canvas_.Children().Append(sphere_);

  // All land in one Path: filled countries with a hairline border. One Path
  // means one fill rule for everything -- XAML's PathGeometry has no FillRule
  // (unlike WPF), so this is the default EvenOdd, under which the dataset's
  // genuine interior rings (Lesotho inside South Africa) render as real holes.
  // Countries never overlap, so nothing else is affected. Android fills each
  // ring separately and so has no holes at all.
  land_ = Path();
  land_.Fill(colors::MakeBrush(kLandColor));
  land_.Stroke(colors::BackgroundBrush());
  land_.IsHitTestVisible(false);
  canvas_.Children().Append(land_);

  graticule_ = Path();
  graticule_.Stroke(colors::MakeBrush(kGraticuleColor));
  graticule_.IsHitTestVisible(false);
  canvas_.Children().Append(graticule_);

  // provider dots live in their own layer so a redraw clears only them
  dots_ = Canvas();
  dots_.IsHitTestVisible(false);
  canvas_.Children().Append(dots_);

  pointerPressedRevoker_ =
      canvas_.PointerPressed(winrt::auto_revoke, {this, &ProviderGlobe::OnPointerPressed});
  pointerMovedRevoker_ =
      canvas_.PointerMoved(winrt::auto_revoke, {this, &ProviderGlobe::OnPointerMoved});
  pointerReleasedRevoker_ =
      canvas_.PointerReleased(winrt::auto_revoke, {this, &ProviderGlobe::EndDrag});
  pointerCanceledRevoker_ =
      canvas_.PointerCanceled(winrt::auto_revoke, {this, &ProviderGlobe::EndDrag});
  pointerCaptureLostRevoker_ =
      canvas_.PointerCaptureLost(winrt::auto_revoke, {this, &ProviderGlobe::EndDrag});
  pointerWheelRevoker_ =
      canvas_.PointerWheelChanged(winrt::auto_revoke, {this, &ProviderGlobe::OnPointerWheel});
  tappedRevoker_ = canvas_.Tapped(
      winrt::auto_revoke, [this](IInspectable const&, Input::TappedRoutedEventArgs const& args) {
        const Point position = args.GetPosition(canvas_);
        OnTapped(position.X, position.Y);
      });

  EnsureTopology();
}

void ProviderGlobe::EnsureTopology() {
  {
    std::scoped_lock lock(gTopologyMutex);
    if (gTopologyStarted) return;
    gTopologyStarted = true;
  }
  // started outside the lock: the coroutine's tail reclaims it on the thread pool
  DecodeTopologyAsync();
}

void ProviderGlobe::SetProviders(std::vector<ProviderLocationRow> rows,
                                 std::string selectedClientId) {
  rows_ = std::move(rows);
  selectedClientId_ = std::move(selectedClientId);

  plottable_.clear();
  for (const auto& row : rows_) {
    if (row.Plottable()) plottable_.push_back(row);
  }

  // Where the globe belongs: the selected provider's position. Until it has
  // been placed once the first plottable provider stands in for a selection
  // that cannot be plotted, so the sheet opens on a provider rather than on the
  // empty Atlantic at (0, 0); after that the globe follows the selection only,
  // because chasing the first row as the window turns over would yank the globe
  // out from under the user.
  const ProviderLocationRow* target = nullptr;
  for (const auto& row : plottable_) {
    if (row.clientId == selectedClientId_) target = &row;
  }
  if (!target && !centeredOnce_ && !plottable_.empty()) target = &plottable_.front();
  if (target) {
    centeredOnce_ = true;
    // Compared as client id AND coordinates, not the id alone: a provider whose
    // position arrives after its row did must still pull the globe over.
    const GlobeCenterTarget next{target->clientId, target->lat, target->lon};
    if (next != centerTarget_) {
      centerTarget_ = next;
      CenterOn(*target);
    }
  }

  dirty_ = true;
  Redraw();
}

void ProviderGlobe::CenterOn(const ProviderLocationRow& row) {
  const GlobeRotation to = GlobeGeometry::RotationCentering(static_cast<float>(row.lon),
                                                            static_cast<float>(row.lat));
  // resolve the target to its nearest equivalent angle so the globe spins the
  // short way around
  const GlobeRotation shortest = GlobeGeometry::LerpRotation(rotation_, to, 1.0f);
  if (!animating_ && std::fabs(shortest.lambda - rotation_.lambda) < 1e-3f &&
      std::fabs(shortest.phi - rotation_.phi) < 1e-3f) {
    // already there, and nothing in flight. The `animating_` half is load
    // bearing: a spring passing through the new target on its way to the old
    // one is momentarily "already there", and returning would leave it running
    // to the provider that is no longer selected.
    return;
  }
  // Retarget only: the spring keeps whatever velocity the in-flight recenter
  // had, which is what makes a step landing on top of another one continuous.
  animTo_ = shortest;
  animLastSeconds_ = NowSeconds();
  animating_ = true;
}

void ProviderGlobe::Tick() {
  // the land arrives from the background decode; pick it up on the next frame
  if (!landLoaded_ && TopologyDone() && CurrentTopology()) dirty_ = true;
  if (animating_) {
    const double now = NowSeconds();
    const double dt = std::clamp(now - animLastSeconds_, 0.0, 1.0);
    animLastSeconds_ = now;
    const bool lambdaMoving =
        StepRecenterSpring(rotation_.lambda, animVelocity_.lambda, animTo_.lambda, dt);
    const bool phiMoving = StepRecenterSpring(rotation_.phi, animVelocity_.phi, animTo_.phi, dt);
    animating_ = lambdaMoving || phiMoving;
    dirty_ = true;
  }
  if (dirty_) Redraw();
}

void ProviderGlobe::Redraw() {
  if (!canvas_) return;
  const float width = static_cast<float>(canvas_.ActualWidth());
  const float height = static_cast<float>(canvas_.ActualHeight());
  if (width <= 0 || height <= 0) return;
  dirty_ = false;

  const float unit = GlobeGeometry::UnitFor(width, height);

  // the sphere, fit-centered
  const double diameter = 2.0 * kGlobeScale * unit;
  sphere_.Width(diameter);
  sphere_.Height(diameter);
  Canvas::SetLeft(sphere_, width / 2.0 - diameter / 2.0);
  Canvas::SetTop(sphere_, height / 2.0 - diameter / 2.0);

  const std::shared_ptr<const WorldTopology> world = CurrentTopology();
  landLoaded_ = static_cast<bool>(world);
  land_.StrokeThickness(kLandStrokeWidth * unit);
  land_.Data(BuildLandGeometry(world, width, height));

  graticule_.StrokeThickness(kGraticuleStrokeWidth * unit);
  graticule_.Data(BuildGraticuleGeometry(width, height));

  RedrawDots(width, height, unit);
}

PathGeometry ProviderGlobe::BuildLandGeometry(const std::shared_ptr<const WorldTopology>& world,
                                              float width, float height) const {
  PathGeometry geometry;
  if (!world) return geometry;

  std::vector<Point> points;
  for (const auto& country : world->Countries()) {
    for (const auto& ring : country.rings) {
      points.clear();
      points.reserve(ring.size() / 2);
      bool anyVisible = false;
      for (size_t i = 0; i + 1 < ring.size(); i += 2) {
        const float lon = ring[i];
        const float lat = ring[i + 1];
        if (0.0f <= GlobeGeometry::CosAngleToCenter(lon, lat, rotation_.lambda, rotation_.phi)) {
          anyVisible = true;
        }
        // clamped, so rings crossing the horizon stay on the visible disk
        const GlobePoint projected =
            GlobeGeometry::ProjectClamped(lon, lat, rotation_.lambda, rotation_.phi, kGlobeScale);
        const GlobePoint mapped = GlobeGeometry::ToCanvas(projected, width, height);
        points.push_back(Point{mapped.x, mapped.y});
      }
      // a ring entirely on the back hemisphere would otherwise smear along the
      // silhouette circle
      if (!anyVisible || points.size() < 2) continue;
      geometry.Figures().Append(MakeFigure(points, /*closed=*/true));
    }
  }
  return geometry;
}

PathGeometry ProviderGlobe::BuildGraticuleGeometry(float width, float height) const {
  PathGeometry geometry;
  std::vector<Point> run;
  // break each line wherever it crosses the horizon, so the back half is not
  // drawn as a chord across the sphere
  auto flush = [&] {
    if (2 <= run.size()) geometry.Figures().Append(MakeFigure(run, /*closed=*/false));
    run.clear();
  };
  for (const auto& line : GlobeGeometry::Graticule()) {
    run.clear();
    for (size_t i = 0; i + 1 < line.size(); i += 2) {
      const auto projected = GlobeGeometry::Project(line[i], line[i + 1], rotation_.lambda,
                                                    rotation_.phi, kGlobeScale);
      if (!projected) {
        flush();
        continue;
      }
      const GlobePoint mapped = GlobeGeometry::ToCanvas(*projected, width, height);
      run.push_back(Point{mapped.x, mapped.y});
    }
    flush();
  }
  return geometry;
}

void ProviderGlobe::RedrawDots(float width, float height, float unit) {
  dots_.Children().Clear();
  // The selected dot is held back and appended last so it is never covered by a
  // dot that happens to sit on top of it -- providers in one city land on the
  // same pixel, and a Canvas paints in child order.
  bool hasSelected = false;
  GlobePoint selectedCenter{};
  winrt::Windows::UI::Color selectedColor{};
  for (const auto& row : plottable_) {
    const auto projected =
        GlobeGeometry::Project(static_cast<float>(row.lon), static_cast<float>(row.lat),
                               rotation_.lambda, rotation_.phi, kGlobeScale);
    if (!projected) continue;  // back hemisphere
    const GlobePoint center = GlobeGeometry::ToCanvas(*projected, width, height);
    const winrt::Windows::UI::Color color = ProviderColor(row);

    if (row.clientId == selectedClientId_) {
      hasSelected = true;
      selectedCenter = center;
      selectedColor = color;
      continue;
    }

    ShapeEllipse dot;
    const double diameter = 2.0 * kDotRadius * unit;
    dot.Width(diameter);
    dot.Height(diameter);
    dot.Fill(SolidColorBrush(color));
    dot.IsHitTestVisible(false);
    Canvas::SetLeft(dot, center.x - diameter / 2.0);
    Canvas::SetTop(dot, center.y - diameter / 2.0);
    dots_.Children().Append(dot);
  }

  if (hasSelected) {
    const GlobePoint center = selectedCenter;
    const winrt::Windows::UI::Color color = selectedColor;
    // a darker core inside its own full-strength ring: the selection reads at a
    // glance without changing which country color it is
    ShapeEllipse dot;
    const double diameter = 2.0 * kDotRadius * unit;
    dot.Width(diameter);
    dot.Height(diameter);
    dot.Fill(SolidColorBrush(DarkenForSelection(color)));
    dot.IsHitTestVisible(false);
    Canvas::SetLeft(dot, center.x - diameter / 2.0);
    Canvas::SetTop(dot, center.y - diameter / 2.0);
    dots_.Children().Append(dot);

    ShapeEllipse ring;
    const double ringDiameter = 2.0 * kSelectedRingRadius * unit;
    ring.Width(ringDiameter);
    ring.Height(ringDiameter);
    ring.Stroke(SolidColorBrush(color));
    ring.StrokeThickness(kSelectedRingStroke * unit);
    ring.IsHitTestVisible(false);
    Canvas::SetLeft(ring, center.x - ringDiameter / 2.0);
    Canvas::SetTop(ring, center.y - ringDiameter / 2.0);
    dots_.Children().Append(ring);
  }
}

// ---- interaction -----------------------------------------------------------

void ProviderGlobe::OnPointerPressed(IInspectable const&,
                                     Input::PointerRoutedEventArgs const& args) {
  const Point position = args.GetCurrentPoint(canvas_).Position();
  dragging_ = true;
  lastPointerX_ = position.X;
  lastPointerY_ = position.Y;
  dragTravel_ = 0;
  canvas_.CapturePointer(args.Pointer());
}

void ProviderGlobe::OnPointerMoved(IInspectable const&,
                                   Input::PointerRoutedEventArgs const& args) {
  if (!dragging_) return;
  const Point position = args.GetCurrentPoint(canvas_).Position();
  const float dx = position.X - lastPointerX_;
  const float dy = position.Y - lastPointerY_;
  lastPointerX_ = position.X;
  lastPointerY_ = position.Y;

  if (plottable_.empty()) {
    // nothing to traverse: rotate freely, at the web's drag sensitivity
    const float width = static_cast<float>(canvas_.ActualWidth());
    const float height = static_cast<float>(canvas_.ActualHeight());
    const float unit = GlobeGeometry::UnitFor(width, height);
    if (unit <= 0) return;
    const float k = GlobeGeometry::DragDegreesPerVirtualPx(kGlobeScale);
    // a drag takes over from any in-flight recenter, velocity and all
    animating_ = false;
    animVelocity_ = GlobeRotation{};
    rotation_.lambda += dx / unit * k;
    rotation_.phi = std::clamp(rotation_.phi - dy / unit * k, -90.0f, 90.0f);
    dirty_ = true;
    Redraw();
    return;
  }

  // scroll wheel: horizontal travel steps the selection past a hysteresis
  // threshold, and the step consumes exactly one threshold of travel
  dragTravel_ += dx;
  const float threshold = static_cast<float>(canvas_.ActualWidth()) * kWheelStepWidthFraction;
  const WheelStep step = GlobeGeometry::ResolveWheelStep(dragTravel_, threshold);
  if (step.steps == 0) return;
  dragTravel_ = step.remainingTravel;
  if (onStep_) onStep_(step.steps);
}

void ProviderGlobe::EndDrag(IInspectable const&, Input::PointerRoutedEventArgs const& args) {
  if (!dragging_) return;
  dragging_ = false;
  dragTravel_ = 0;  // travel resets between gestures, so each swipe starts fresh
  canvas_.ReleasePointerCapture(args.Pointer());
}

void ProviderGlobe::OnPointerWheel(IInspectable const&,
                                   Input::PointerRoutedEventArgs const& args) {
  if (plottable_.empty()) return;
  const int32_t delta = args.GetCurrentPoint(canvas_).Properties().MouseWheelDelta();
  // same sign convention as the drag: scrolling down (negative delta) advances
  // east, matching the globe spinning east under the pointer
  wheelTravel_ += static_cast<float>(delta);
  const WheelStep step = GlobeGeometry::ResolveWheelStep(wheelTravel_, kWheelNotch);
  if (step.steps == 0) return;
  wheelTravel_ = step.remainingTravel;
  if (onStep_) onStep_(step.steps);
  args.Handled(true);
}

void ProviderGlobe::OnTapped(float x, float y) {
  if (!onSelect_ || plottable_.empty()) return;
  const float width = static_cast<float>(canvas_.ActualWidth());
  const float height = static_cast<float>(canvas_.ActualHeight());
  const GlobePoint tap = GlobeGeometry::ToVirtual(x, y, width, height);

  std::vector<GlobePoint> visible;
  std::vector<size_t> indexes;
  for (size_t i = 0; i < plottable_.size(); ++i) {
    const auto projected = GlobeGeometry::Project(
        static_cast<float>(plottable_[i].lon), static_cast<float>(plottable_[i].lat),
        rotation_.lambda, rotation_.phi, kGlobeScale);
    if (!projected) continue;
    visible.push_back(*projected);
    indexes.push_back(i);
  }
  const int hit = GlobeGeometry::NearestWithin(tap.x, tap.y, visible, kTapSlop);
  if (0 <= hit) onSelect_(plottable_[indexes[static_cast<size_t>(hit)]].clientId);
}

}  // namespace urnw
