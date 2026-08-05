// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose: this translation unit is pure standard C++ so it also
// compiles on the build host for tools/globe-tests.cpp. App.vcxproj compiles it
// with PrecompiledHeader=NotUsing.
#include "GlobeGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace urnw {
namespace GlobeGeometry {
namespace {

constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

// Rotation, ported from d3-geo rotation.js rotateRadians(dl, dp, 0):
// rotationLambda(dl) first adds dl to the longitude, then rotationPhiGamma(dp, 0)
// takes the unit vector
//     x = cos(lambda1) * cos(phi1)   // toward the viewer
//     y = sin(lambda1) * cos(phi1)   // screen right
//     z = sin(phi1)                  // screen up (north)
// and returns [atan2(y, x * cos(dp) - z * sin(dp)),
//              asin(z * cos(dp) + x * sin(dp))]
// which is the rotation about the screen-right axis mapping
//     (x, y, z) -> (x cos dp - z sin dp, y, z cos dp + x sin dp).
// The orthographic raw projection of the rotated (lambda2, phi2) is
//     [cos(phi2) * sin(lambda2), sin(phi2)] = (rotated y, rotated z)
// and cos(angular distance to the view center) = cos(phi2) * cos(lambda2)
// = rotated x, so the three component functions below are the whole pipeline
// with no inverse trig.

// Rotated x: cosine of the angular distance to the view center.
double RotateTowardViewer(float lonDeg, float latDeg, float rotLambdaDeg, float rotPhiDeg) {
  const double lambda = (static_cast<double>(lonDeg) + rotLambdaDeg) * kDegreesToRadians;
  const double phi = static_cast<double>(latDeg) * kDegreesToRadians;
  const double deltaPhi = static_cast<double>(rotPhiDeg) * kDegreesToRadians;
  return std::cos(lambda) * std::cos(phi) * std::cos(deltaPhi) -
         std::sin(phi) * std::sin(deltaPhi);
}

// Rotated y: the raw orthographic screen-right coordinate, [-1, 1].
double RotateRight(float lonDeg, float latDeg, float rotLambdaDeg) {
  const double lambda = (static_cast<double>(lonDeg) + rotLambdaDeg) * kDegreesToRadians;
  const double phi = static_cast<double>(latDeg) * kDegreesToRadians;
  return std::sin(lambda) * std::cos(phi);
}

// Rotated z: the raw orthographic screen-up coordinate, [-1, 1].
double RotateUp(float lonDeg, float latDeg, float rotLambdaDeg, float rotPhiDeg) {
  const double lambda = (static_cast<double>(lonDeg) + rotLambdaDeg) * kDegreesToRadians;
  const double phi = static_cast<double>(latDeg) * kDegreesToRadians;
  const double deltaPhi = static_cast<double>(rotPhiDeg) * kDegreesToRadians;
  return std::sin(phi) * std::cos(deltaPhi) + std::cos(lambda) * std::cos(phi) * std::sin(deltaPhi);
}

// Graticule construction, ported from d3-geo graticule.js defaults:
// extentMajor [[-180, -90 + eps], [180, 90 - eps]], extentMinor
// [[-180, -80 - eps], [180, 80 + eps]], stepMinor [10, 10], stepMajor [90, 360],
// precision 2.5. d3's lines() emits major meridians range(-180, 180, 90), major
// parallels range(0, 90 - eps, 360) (the equator only), minor meridians
// range(-180, 180, 10) filtered to abs(lon % 90) > eps, and minor parallels
// range(-80, 80 + eps, 10) filtered to abs(lat % 360) > eps.

constexpr double kGraticuleEpsilon = 1e-6;
constexpr double kGraticulePrecision = 2.5;

// Sample positions along [start, end] every kGraticulePrecision degrees with d3
// range semantics: start + i * step for i in 0 until
// ceil((end - eps - start) / step), then the exact end appended.
std::vector<double> SampleSpan(double start, double end) {
  const int steps = static_cast<int>(std::ceil((end - kGraticuleEpsilon - start) / kGraticulePrecision));
  std::vector<double> values;
  values.reserve(static_cast<size_t>(steps) + 1);
  for (int i = 0; i < steps; ++i) values.push_back(start + i * kGraticulePrecision);
  values.push_back(end);
  return values;
}

GlobeLine Meridian(double lon, double latStart, double latEnd) {
  const std::vector<double> lats = SampleSpan(latStart, latEnd);
  GlobeLine line(lats.size() * 2);
  for (size_t i = 0; i < lats.size(); ++i) {
    line[2 * i] = static_cast<float>(lon);
    line[2 * i + 1] = static_cast<float>(lats[i]);
  }
  return line;
}

GlobeLine Parallel(double lat) {
  const std::vector<double> lons = SampleSpan(-180.0, 180.0);
  GlobeLine line(lons.size() * 2);
  for (size_t i = 0; i < lons.size(); ++i) {
    line[2 * i] = static_cast<float>(lons[i]);
    line[2 * i + 1] = static_cast<float>(lat);
  }
  return line;
}

std::vector<GlobeLine> BuildGraticule() {
  std::vector<GlobeLine> lines;
  lines.reserve(53);
  // major meridians every 90 degrees, pole to pole
  for (double lon = -180.0; lon < 180.0; lon += 90.0) {
    lines.push_back(Meridian(lon, -90.0 + kGraticuleEpsilon, 90.0 - kGraticuleEpsilon));
  }
  // the equator, the only major parallel
  lines.push_back(Parallel(0.0));
  // minor meridians every 10 degrees, spanning lat -80..80
  for (double lon = -180.0; lon < 180.0; lon += 10.0) {
    if (std::abs(std::fmod(lon, 90.0)) > kGraticuleEpsilon) {
      lines.push_back(Meridian(lon, -80.0 - kGraticuleEpsilon, 80.0 + kGraticuleEpsilon));
    }
  }
  // minor parallels every 10 degrees from -80..80, skipping the equator
  for (double lat = -80.0; lat <= 80.0 + kGraticuleEpsilon; lat += 10.0) {
    if (std::abs(lat) > kGraticuleEpsilon) lines.push_back(Parallel(lat));
  }
  return lines;
}

}  // namespace

std::optional<GlobePoint> Project(float lonDeg, float latDeg, float rotLambdaDeg,
                                  float rotPhiDeg, float scale) {
  if (RotateTowardViewer(lonDeg, latDeg, rotLambdaDeg, rotPhiDeg) < 0.0) return std::nullopt;
  const double right = RotateRight(lonDeg, latDeg, rotLambdaDeg);
  const double up = RotateUp(lonDeg, latDeg, rotLambdaDeg, rotPhiDeg);
  return GlobePoint{static_cast<float>(kCenter + scale * right),
                    static_cast<float>(kCenter - scale * up)};
}

GlobePoint ProjectClamped(float lonDeg, float latDeg, float rotLambdaDeg, float rotPhiDeg,
                          float scale) {
  const double towardViewer = RotateTowardViewer(lonDeg, latDeg, rotLambdaDeg, rotPhiDeg);
  const double right = RotateRight(lonDeg, latDeg, rotLambdaDeg);
  const double up = RotateUp(lonDeg, latDeg, rotLambdaDeg, rotPhiDeg);
  if (towardViewer >= 0.0) {
    return GlobePoint{static_cast<float>(kCenter + scale * right),
                      static_cast<float>(kCenter - scale * up)};
  }
  const double length = std::sqrt(right * right + up * up);
  if (length < 1e-9) return GlobePoint{kCenter + scale, kCenter};
  return GlobePoint{static_cast<float>(kCenter + scale * right / length),
                    static_cast<float>(kCenter - scale * up / length)};
}

float CosAngleToCenter(float lonDeg, float latDeg, float rotLambdaDeg, float rotPhiDeg) {
  return static_cast<float>(RotateTowardViewer(lonDeg, latDeg, rotLambdaDeg, rotPhiDeg));
}

GlobeRotation RotationCentering(float lonDeg, float latDeg) { return GlobeRotation{-lonDeg, -latDeg}; }

GlobeRotation LerpRotation(GlobeRotation from, GlobeRotation to, float t) {
  float deltaLambda = std::fmod(to.lambda - from.lambda, 360.0f);
  if (deltaLambda > 180.0f) deltaLambda -= 360.0f;
  if (deltaLambda < -180.0f) deltaLambda += 360.0f;
  return GlobeRotation{from.lambda + deltaLambda * t,
                       std::clamp(from.phi + (to.phi - from.phi) * t, -90.0f, 90.0f)};
}

float DragDegreesPerVirtualPx(float scale) {
  return static_cast<float>(kVirtualSize / scale / (3.0 * 3.14159265358979323846));
}

const std::vector<GlobeLine>& Graticule() {
  static const std::vector<GlobeLine> cached = BuildGraticule();
  return cached;
}

WheelStep ResolveWheelStep(float travel, float threshold) {
  if (threshold <= 0.0f) return WheelStep{0, 0.0f};
  // truncates toward zero, so a fast drag can cross several steps at once
  const int steps = static_cast<int>(-travel / threshold);
  return WheelStep{steps, travel + steps * threshold};
}

int WrapIndex(int index, int steps, int count) {
  if (count <= 0) return -1;
  const int next = (index + steps) % count;
  return next < 0 ? next + count : next;
}

float UnitFor(float canvasWidth, float canvasHeight) {
  return (std::min)(canvasWidth, canvasHeight) / kVirtualSize;
}

GlobePoint ToCanvas(GlobePoint point, float canvasWidth, float canvasHeight) {
  const float unit = UnitFor(canvasWidth, canvasHeight);
  return GlobePoint{canvasWidth / 2.0f + (point.x - kCenter) * unit,
                    canvasHeight / 2.0f + (point.y - kCenter) * unit};
}

GlobePoint ToVirtual(float x, float y, float canvasWidth, float canvasHeight) {
  const float unit = UnitFor(canvasWidth, canvasHeight);
  if (unit <= 0.0f) return GlobePoint{kCenter, kCenter};
  return GlobePoint{kCenter + (x - canvasWidth / 2.0f) / unit,
                    kCenter + (y - canvasHeight / 2.0f) / unit};
}

int NearestWithin(float x, float y, const std::vector<GlobePoint>& points, float radius) {
  const float radiusSquared = radius * radius;
  int best = -1;
  float bestDistanceSquared = (std::numeric_limits<float>::max)();
  for (size_t i = 0; i < points.size(); ++i) {
    const float dx = points[i].x - x;
    const float dy = points[i].y - y;
    const float distanceSquared = dx * dx + dy * dy;
    if (distanceSquared <= radiusSquared && distanceSquared < bestDistanceSquared) {
      best = static_cast<int>(i);
      bestDistanceSquared = distanceSquared;
    }
  }
  return best;
}

}  // namespace GlobeGeometry
}  // namespace urnw
