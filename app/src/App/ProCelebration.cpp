// SPDX-License-Identifier: MPL-2.0
#include "pch.h"
#include "ProCelebration.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using winrt::Windows::Foundation::Point;
using ShapePolygon = winrt::Microsoft::UI::Xaml::Shapes::Polygon;
using ShapeRectangle = winrt::Microsoft::UI::Xaml::Shapes::Rectangle;

namespace urnw {
namespace {

constexpr double kPi = 3.14159265358979323846;

// a steady stream: a new sprite about every 150 ms (with a little jitter)
// until the last one can still leave the window before the confetti ends
constexpr double kSpawnIntervalSeconds = 0.15;
constexpr double kSpawnJitterSeconds = 0.03;
// time to cross the window, fast to slow
constexpr double kMinCrossingSeconds = 1.2;
constexpr double kMaxCrossingSeconds = 1.9;
// the most sprites in the air at once; the schedule skips a take-off that
// would exceed it (the interval and crossing times keep it near a dozen)
constexpr int kMaxLiveSprites = 30;
constexpr double kPitchDegrees = 20;
constexpr int kTrailCount = 2;
constexpr double kTrailStepPx = 18;
constexpr int kFrameMillis = 16;

// The Compose FastOutSlowIn curve (a cubic bezier 0.4,0 / 0.2,1), sampled by
// Newton iteration on the x axis: the sprites' horizontal ease and the veil
// ramps use it so the port moves exactly like android.
double FastOutSlowIn(double x) {
  x = std::clamp(x, 0.0, 1.0);
  constexpr double x1 = 0.4, y1 = 0.0, x2 = 0.2, y2 = 1.0;
  double t = x;
  for (int i = 0; i < 8; ++i) {
    const double mt = 1 - t;
    const double bx = 3 * mt * mt * t * x1 + 3 * mt * t * t * x2 + t * t * t;
    const double dbx = 3 * mt * mt * x1 + 6 * mt * t * (x2 - x1) + 3 * t * t * (1 - x2);
    if (dbx < 1e-6) break;
    t -= (bx - x) / dbx;
    t = std::clamp(t, 0.0, 1.0);
  }
  const double mt = 1 - t;
  return 3 * mt * mt * t * y1 + 3 * mt * t * t * y2 + t * t * t;
}

// ---- the pixel sprites (android pro_flight_sunglasses / pixel_eye_cover /
// pixel_face_cover), in cells of the 12-unit grid ----------------------------

struct Cell {
  int x, y;
};

// the sunglasses outline: 22 x 5 cells, the privacy-glasses staircase
const Cell kSunglassesOutline[] = {
    {0, 0},  {0, 2},  {1, 2},  {1, 3},  {2, 3},  {2, 4},  {3, 4},  {3, 5},  {8, 5},  {8, 4},
    {9, 4},  {9, 3},  {10, 3}, {10, 2}, {12, 2}, {12, 3}, {13, 3}, {13, 4}, {14, 4}, {14, 5},
    {19, 5}, {19, 4}, {20, 4}, {20, 3}, {21, 3}, {21, 2}, {22, 2}, {22, 0}};
// the black lens highlights
const Cell kSunglassesGlints[] = {{2, 1},  {4, 1},  {3, 2},  {5, 2},  {4, 3},  {6, 3},
                                  {13, 1}, {15, 1}, {14, 2}, {16, 2}, {15, 3}, {17, 3}};
// the square eye cover: a 22 x 8 censor bar with the same stepped corners
const Cell kEyeCoverOutline[] = {{2, 0},  {20, 0}, {20, 1}, {21, 1}, {21, 2}, {22, 2}, {22, 6},
                                 {21, 6}, {21, 7}, {20, 7}, {20, 8}, {2, 8},  {2, 7},  {1, 7},
                                 {1, 6},  {0, 6},  {0, 2},  {1, 2},  {1, 1},  {2, 1}};
const Cell kEyeCoverGlints[] = {{3, 1}, {18, 6}};
// the circular face cover: a 12 x 12 stepped disc
const Cell kFaceCoverOutline[] = {{4, 0},  {8, 0},  {8, 1},  {10, 1}, {10, 2}, {11, 2}, {11, 4},
                                  {12, 4}, {12, 8}, {11, 8}, {11, 10}, {10, 10}, {10, 11}, {8, 11},
                                  {8, 12}, {4, 12}, {4, 11}, {2, 11}, {2, 10}, {1, 10}, {1, 8},
                                  {0, 8},  {0, 4},  {1, 4},  {1, 2},  {2, 2},  {2, 1},  {4, 1}};
const Cell kFaceCoverGlints[] = {{3, 2}, {2, 3}};

struct SpriteShape {
  const Cell* outline;
  int outlineCount;
  const Cell* glints;
  int glintCount;
  winrt::Windows::UI::Color body;
  winrt::Windows::UI::Color glint;
  double cellsWide;
  double cellsHigh;
  // the on-screen size at scale 1, in px (android: 96x22, 96x35, 64x64 dp)
  double widthPx;
  double heightPx;
};

const SpriteShape& ShapeOf(ProFlightSprite::Kind kind) {
  static const SpriteShape sunglasses{kSunglassesOutline, 28, kSunglassesGlints, 12, colors::kUrPink,
                                      colors::kBackground, 22, 5, 96, 22};
  static const SpriteShape eyeCover{kEyeCoverOutline, 20, kEyeCoverGlints, 2, colors::kBackground,
                                    colors::kUrPink, 22, 8, 96, 35};
  static const SpriteShape faceCover{kFaceCoverOutline, 28, kFaceCoverGlints, 2, colors::kProGold,
                                     colors::kBackground, 12, 12, 64, 64};
  switch (kind) {
    case ProFlightSprite::Kind::Sunglasses: return sunglasses;
    case ProFlightSprite::Kind::EyeCover: return eyeCover;
    case ProFlightSprite::Kind::FaceCover: return faceCover;
  }
  return sunglasses;
}

// One copy of a shape as XAML: a canvas of the outline polygon and the glint
// squares at the shape's natural size, moved and pitched by its transform.
Canvas MakeShapeVisual(const SpriteShape& shape, CompositeTransform const& transform) {
  Canvas visual;
  visual.Width(shape.widthPx);
  visual.Height(shape.heightPx);
  visual.IsHitTestVisible(false);
  visual.RenderTransform(transform);
  const double cell = shape.widthPx / shape.cellsWide;
  ShapePolygon body;
  auto points = body.Points();
  for (int i = 0; i < shape.outlineCount; ++i) {
    points.Append(Point{static_cast<float>(shape.outline[i].x * cell),
                        static_cast<float>(shape.outline[i].y * cell)});
  }
  body.Fill(colors::MakeBrush(shape.body));
  visual.Children().Append(body);
  for (int i = 0; i < shape.glintCount; ++i) {
    ShapeRectangle glint;
    glint.Width(cell);
    glint.Height(cell);
    glint.Fill(colors::MakeBrush(shape.glint));
    Canvas::SetLeft(glint, shape.glints[i].x * cell);
    Canvas::SetTop(glint, shape.glints[i].y * cell);
    visual.Children().Append(glint);
  }
  return visual;
}

int64_t NowTicks() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// ---- the burst ------------------------------------------------------------

std::vector<ProFlightSprite> ProFlightBurst(uint64_t seed) {
  std::mt19937_64 random(seed);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  std::vector<ProFlightSprite> sprites;
  double takeOff = 0;
  const double lastTakeOff = kProFlightConfettiSeconds - kMaxCrossingSeconds;
  while (takeOff <= lastTakeOff) {
    ProFlightSprite sprite;
    const int kind = static_cast<int>(unit(random) * 5);  // 40 / 40 / 20
    sprite.kind = kind < 2   ? ProFlightSprite::Kind::Sunglasses
                  : kind < 4 ? ProFlightSprite::Kind::EyeCover
                             : ProFlightSprite::Kind::FaceCover;
    sprite.delaySeconds = takeOff;
    sprite.crossingSeconds = kMinCrossingSeconds + unit(random) * (kMaxCrossingSeconds - kMinCrossingSeconds);
    sprite.lane = 0.08 + unit(random) * 0.84;
    sprite.bobAmplitude = 12 + std::floor(unit(random) * 29);
    sprite.bobCycles = 2 + unit(random) * 2;
    sprite.phaseOffset = unit(random) * 2 * kPi;
    sprite.scale = 0.6 + unit(random) * 0.6;
    // the small ones fly behind
    sprite.behind = sprite.scale < 0.85;
    const auto live = std::count_if(sprites.begin(), sprites.end(), [takeOff](const ProFlightSprite& s) {
      return s.delaySeconds + s.crossingSeconds > takeOff;
    });
    if (live < kMaxLiveSprites) sprites.push_back(sprite);
    takeOff += kSpawnIntervalSeconds + (unit(random) * 2 - 1) * kSpawnJitterSeconds;
  }
  // in take-off order: the canvas z-order puts the ones behind first as they
  // are added, and the front layer is added on top of them (see Tick)
  return sprites;
}

double ProFlightVeilStrength(double seconds) {
  const double outEnd = kProFlightPixelateOutStartSeconds + kProFlightPixelateOutSeconds;
  if (seconds < kProFlightPixelateInSeconds) return FastOutSlowIn(seconds / kProFlightPixelateInSeconds);
  if (seconds > kProFlightPixelateOutStartSeconds) {
    return FastOutSlowIn(std::clamp((outEnd - seconds) / kProFlightPixelateOutSeconds, 0.0, 1.0));
  }
  return 1;
}

// ---- the flight -----------------------------------------------------------

// One sprite in the air: the front copy and its trail copies (the trail is
// drawn behind, at a step's distance and fading), each on its own transform.
struct ProCelebrationFlight::Live {
  size_t sprite = 0;
  Canvas copies[kTrailCount + 1]{nullptr, nullptr, nullptr};
  CompositeTransform transforms[kTrailCount + 1]{nullptr, nullptr, nullptr};
};

ProCelebrationFlight::ProCelebrationFlight(Canvas const& canvas, ShapeRectangle const& veil)
    : canvas_(canvas), veil_(veil) {}

ProCelebrationFlight::~ProCelebrationFlight() {
  if (timer_) timer_.Stop();
}

double ProCelebrationFlight::Seconds() const { return (NowTicks() - startTicks_) / 1000.0; }

void ProCelebrationFlight::Launch() {
  // skipped when the system has animations off (UISettings, the same gate the
  // referral crowning uses), and while a flight is already in the air
  if (!winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled()) return;
  if (active_) return;
  active_ = true;
  ++sequence_;
  startTicks_ = NowTicks();
  sprites_ = ProFlightBurst(sequence_ * 0x9E3779B97F4A7C15ULL + static_cast<uint64_t>(startTicks_));
  nextSprite_ = 0;
  live_.clear();
  canvas_.Children().Clear();
  canvas_.Visibility(Visibility::Visible);
  veil_.Opacity(0);
  veil_.Visibility(Visibility::Visible);
  if (!timer_) {
    timer_ = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
    timer_.Interval(std::chrono::milliseconds(kFrameMillis));
    timer_.IsRepeating(true);
    timer_.Tick([this](auto const&, auto const&) { Tick(); });
  }
  timer_.Start();
  Tick();
}

void ProCelebrationFlight::Finish() {
  if (timer_) timer_.Stop();
  active_ = false;
  live_.clear();
  canvas_.Children().Clear();
  canvas_.Visibility(Visibility::Collapsed);
  veil_.Opacity(0);
  veil_.Visibility(Visibility::Collapsed);
}

void ProCelebrationFlight::Retire(Live& live) {
  for (auto& copy : live.copies) {
    if (!copy) continue;
    uint32_t index = 0;
    if (canvas_.Children().IndexOf(copy, index)) canvas_.Children().RemoveAt(index);
    copy = nullptr;
  }
}

void ProCelebrationFlight::Tick() {
  const double seconds = Seconds();
  if (seconds >= kProFlightTotalSeconds) {
    Finish();
    return;
  }
  // the veil follows the envelope: in over 5 s, hold, out over the 5 s after
  // the confetti has finished
  veil_.Opacity(ProFlightVeilStrength(seconds));

  const double width = canvas_.ActualWidth();
  const double height = canvas_.ActualHeight();
  if (width <= 0 || height <= 0) return;

  // take-offs: every sprite whose time has come gets its XAML copies; the
  // ones behind are added first so the front layer draws over them
  while (nextSprite_ < sprites_.size() && sprites_[nextSprite_].delaySeconds <= seconds) {
    const ProFlightSprite& sprite = sprites_[nextSprite_];
    const SpriteShape& shape = ShapeOf(sprite.kind);
    Live live;
    live.sprite = nextSprite_;
    for (int i = 0; i <= kTrailCount; ++i) {
      CompositeTransform transform;
      transform.CenterX(shape.widthPx / 2);
      transform.CenterY(shape.heightPx / 2);
      live.transforms[i] = transform;
      auto copy = MakeShapeVisual(shape, transform);
      copy.Opacity(0);
      live.copies[i] = copy;
    }
    // the trail copies go under the front copy
    for (int i = kTrailCount; i >= 1; --i) {
      if (sprite.behind) canvas_.Children().InsertAt(0, live.copies[i]);
      else canvas_.Children().Append(live.copies[i]);
    }
    if (sprite.behind) canvas_.Children().InsertAt(0, live.copies[0]);
    else canvas_.Children().Append(live.copies[0]);
    live_.push_back(std::move(live));
    ++nextSprite_;
  }

  // the frame: place every sprite in the air, retire the ones that landed
  for (size_t index = 0; index < live_.size();) {
    Live& live = live_[index];
    const ProFlightSprite& sprite = sprites_[live.sprite];
    const double local = (seconds - sprite.delaySeconds) / sprite.crossingSeconds;
    if (local >= 1) {
      Retire(live);
      live_.erase(live_.begin() + static_cast<std::ptrdiff_t>(index));
      continue;
    }
    const double t = FastOutSlowIn(std::max(0.0, local));
    const SpriteShape& shape = ShapeOf(sprite.kind);
    const double spriteWidth = shape.widthPx * sprite.scale;
    const double travel = width + 2 * spriteWidth;
    const double laneY = height * sprite.lane;
    const double layerAlpha = sprite.behind ? 0.55 : 1.0;

    auto place = [&](int copyIndex, double progress, double alpha, double scale) {
      const double phase = 2 * kPi * sprite.bobCycles * progress + sprite.phaseOffset;
      // the sprite's centre on screen: the transform scales and pitches about
      // the copy's own centre, so the translation carries that centre minus
      // the unscaled half size
      const double centerX = -spriteWidth + progress * travel + spriteWidth / 2;
      // the vertical velocity sets the pitch: nose up while rising (y
      // decreasing on screen), nose down while falling
      const double centerY = laneY + sprite.bobAmplitude * std::sin(phase);
      const double pitch = -kPitchDegrees * std::cos(phase);
      auto& transform = live.transforms[copyIndex];
      transform.TranslateX(centerX - shape.widthPx / 2);
      transform.TranslateY(centerY - shape.heightPx / 2);
      transform.ScaleX(scale);
      transform.ScaleY(scale);
      transform.Rotation(pitch);
      live.copies[copyIndex].Opacity(alpha);
    };

    // the trail: fading copies a step behind
    for (int i = kTrailCount; i >= 1; --i) {
      const double trailT = std::max(0.0, t - i * kTrailStepPx / travel);
      place(i, trailT, layerAlpha * (0.28 - 0.1 * i), sprite.scale * (1 - 0.08 * i));
    }
    place(0, t, layerAlpha, sprite.scale);
    ++index;
  }
}

}  // namespace urnw
