// Orthographic globe projection math for the provider-locations globe — a port
// of the android GlobeGeometry.kt, which is itself a port of the web globe
// (d3.geoOrthographic with clipAngle 90; see ur.io Globe.jsx and
// sdk/PROVIDERLOCATIONS.md "ur.io /ip globe").
//
// Deliberately PURE standard C++: no WinRT, no windows.h, no pch. That is what
// lets the whole module be compiled and run on a non-Windows host, which is how
// the 25 android GlobeGeometryTest cases are kept as an executable spec (see
// tools/globe-tests.cpp). The project therefore compiles it with
// PrecompiledHeader=NotUsing.
//
// Everything is in a 600x600 virtual space with the globe centered at
// (300, 300); the view maps virtual space to its canvas with UnitFor/ToCanvas.
//
// Rotation is the d3 `projection.rotate([lambda, phi])` convention, in degrees:
// the globe is rotated by (+lambda, +phi), so the point centered on screen is
// (lon = -lambda, lat = -phi).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <utility>
#include <vector>

namespace urnw {

// A point in the 600x600 virtual drawing space.
struct GlobePoint {
  float x = 0;
  float y = 0;
};

// A globe rotation in the d3 convention, degrees.
struct GlobeRotation {
  float lambda = 0;
  float phi = 0;
};

// The result of resolving a wheel drag; see GlobeGeometry::ResolveWheelStep.
struct WheelStep {
  int steps = 0;
  float remainingTravel = 0;
};

// One graticule polyline in lon/lat degrees, packed [lon0, lat0, lon1, lat1, ...].
using GlobeLine = std::vector<float>;

namespace GlobeGeometry {

inline constexpr float kVirtualSize = 600.0f;
inline constexpr float kCenter = 300.0f;

// Projects (lon, lat) under rotation (lambda, phi) at the given scale (globe
// radius in virtual px; the web starts at 300). Returns nullopt for points on
// the back hemisphere (angular distance to the view center greater than 90
// degrees), matching d3 clipAngle(90).
std::optional<GlobePoint> Project(float lonDeg, float latDeg, float rotLambdaDeg,
                                  float rotPhiDeg, float scale);

// Like Project, but never empty: back-hemisphere points are clamped to the
// silhouette circle of radius `scale` in their azimuthal direction, so polygon
// fills that cross the horizon stay on the visible disk. The exact antipode of
// the view center has no direction; it clamps to (kCenter + scale, kCenter)
// deterministically.
GlobePoint ProjectClamped(float lonDeg, float latDeg, float rotLambdaDeg, float rotPhiDeg,
                          float scale);

// Cosine of the angular distance from (lon, lat) to the view center under the
// given rotation. The point is on the visible hemisphere iff >= 0.
float CosAngleToCenter(float lonDeg, float latDeg, float rotLambdaDeg, float rotPhiDeg);

// The rotation that centers (lon, lat) on screen: (-lon, -lat).
GlobeRotation RotationCentering(float lonDeg, float latDeg);

// Componentwise interpolation between two rotations with longitude taking the
// shorter way around (so 170 -> -170 passes through 180, not 0) and phi clamped
// to [-90, 90].
GlobeRotation LerpRotation(GlobeRotation from, GlobeRotation to, float t);

// Drag sensitivity in degrees of rotation per virtual px of drag at the given
// scale. Web parity (Globe.jsx drag handler):
//
//     const k = width / projection.scale() / (3 * Math.PI);
//     projection.rotate([r[0] + event.dx * k, r[1] - event.dy * k]);
//
// projection.rotate() takes degrees, so k is degrees per px: at the initial
// scale 300, k = 600 / 300 / (3 * pi) = 2 / (3 * pi) = 0.21221 degrees per px,
// i.e. dragging across the full 600 px width turns the globe by ~127 degrees.
// Callers apply +dx * k to lambda and -dy * k to phi, as the web does.
float DragDegreesPerVirtualPx(float scale);

// Graticule polylines in lon/lat degrees matching d3.geoGraticule() defaults
// (d3-geo graticule.js): minor meridians every 10 degrees of lon (skipping
// multiples of 90) spanning lat -80..80, minor parallels every 10 degrees of
// lat from -80..80 (skipping the equator) spanning lon -180..180, major
// meridians at -180, -90, 0 and 90 spanning the full lat range, and the equator
// as the single major parallel. Lines are sampled every 2.5 degrees (d3's
// default precision; d3 emits sparse meridians and lets projected adaptive
// resampling curve them, which comes to the same drawn shape).
//
// Rotation-independent, computed once; Project applies rotation at draw time.
const std::vector<GlobeLine>& Graticule();

// One horizontal drag resolved against the wheel's hysteresis threshold: how
// many providers to advance, and the travel carried into the next step. Swiping
// left (negative travel) advances forward, matching the globe spinning east
// under the finger.
//
// The leftover travel is what makes it hysteretic: after a step the user must
// drag another full threshold to step again, so a pointer resting near the
// boundary cannot flicker between two providers.
//
// Only the travel-to-steps conversion lives here: the wheel itself -- the
// centroid-relative provider order and the clamping at its ends -- is the SDK's
// ProviderLocationsViewController, shared by every platform.
WheelStep ResolveWheelStep(float travel, float threshold);

// Fit-center layout: the virtual space is scaled to the SMALLER canvas
// dimension and centered in both, so the globe fits whole and stays centered
// whatever the box's aspect ratio.
float UnitFor(float canvasWidth, float canvasHeight);

// A point in the 600-unit virtual space -> canvas px, fit-centered.
GlobePoint ToCanvas(GlobePoint point, float canvasWidth, float canvasHeight);

// Canvas px -> the 600-unit virtual space; the inverse of ToCanvas.
GlobePoint ToVirtual(float x, float y, float canvasWidth, float canvasHeight);

// Index of the point nearest to (x, y) within `radius` (inclusive, in virtual
// px), or -1 if none is in range. Ties keep the earliest index.
int NearestWithin(float x, float y, const std::vector<GlobePoint>& points, float radius);

}  // namespace GlobeGeometry
}  // namespace urnw
