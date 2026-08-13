// Executable spec for the provider-locations pure logic: a port of the android
// JVM tests GlobeGeometryTest.kt (25 cases) and WorldTopologyTest.kt (7 cases),
// plus the row-label cases, run against the SAME C++ sources the app compiles.
//
// The windows app has no test project (the solution is Common/Service/App/
// SplitTunnel/Installer), and a WinUI 3 app cannot be built or run on a
// non-Windows host — but GlobeGeometry / WorldTopology / ProviderLocations are
// deliberately WinRT-free, so this harness builds and runs anywhere with a
// C++20 compiler and nlohmann/json. That is what keeps the projection math,
// the TopoJSON stitching and the labels verified without a Windows machine.
//
//   c++ -std=c++20 -I ../src/App -I <dir containing nlohmann/json.hpp> \
//       globe-tests.cpp ../src/App/GlobeGeometry.cpp \
//       ../src/App/WorldTopology.cpp ../src/App/ProviderLocations.cpp \
//       -o /tmp/globe-tests && /tmp/globe-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "GlobeGeometry.h"
#include "ProviderLocations.h"
#include "WorldTopology.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

void CheckNear(double expected, double actual, double tolerance, const std::string& what) {
  if (!(std::fabs(expected - actual) <= tolerance)) {
    std::ostringstream out;
    out << what << ": expected " << expected << " +/- " << tolerance << ", got " << actual;
    Fail(out.str());
  }
}

void CheckEqualInt(long long expected, long long actual, const std::string& what) {
  if (expected != actual) {
    std::ostringstream out;
    out << what << ": expected " << expected << ", got " << actual;
    Fail(out.str());
  }
}

void CheckEqualString(const std::string& expected, const std::string& actual,
                      const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

void CheckPoint(float expectedX, float expectedY, const std::optional<GlobePoint>& actual,
                const std::string& what) {
  if (!actual) {
    Fail(what + ": expected a point, got none");
    return;
  }
  CheckNear(expectedX, actual->x, 1e-3, what + ".x");
  CheckNear(expectedY, actual->y, 1e-3, what + ".y");
}

double DistanceToCenter(GlobePoint p) {
  return std::hypot(p.x - GlobeGeometry::kCenter, p.y - GlobeGeometry::kCenter);
}

bool AllEqual(const GlobeLine& line, size_t offset) {
  for (size_t i = offset + 2; i < line.size(); i += 2) {
    if (std::fabs(line[i] - line[offset]) > 1e-4f) return false;
  }
  return true;
}

// ---- GlobeGeometryTest.kt -------------------------------------------------

void GlobeGeometryTests() {
  {
    TEST_CASE("projectsCardinalPointsAtIdentityRotation");
    // orthographic at rotation (0, 0), scale 300: the view center (0, 0) lands
    // at (300, 300); (90, 0) is the right limb, (0, 90) the top
    CheckPoint(300, 300, GlobeGeometry::Project(0, 0, 0, 0, 300), "center");
    CheckPoint(600, 300, GlobeGeometry::Project(90, 0, 0, 0, 300), "right limb");
    CheckPoint(0, 300, GlobeGeometry::Project(-90, 0, 0, 0, 300), "left limb");
    CheckPoint(300, 0, GlobeGeometry::Project(0, 90, 0, 0, 300), "north pole");
    CheckPoint(300, 600, GlobeGeometry::Project(0, -90, 0, 0, 300), "south pole");
  }
  {
    TEST_CASE("backHemisphereProjectsToNull");
    Check(!GlobeGeometry::Project(180, 0, 0, 0, 300).has_value(), "antipode projects to none");
    Check(!GlobeGeometry::Project(0, 0, 180, 0, 300).has_value(), "rotated antipode is none");
    CheckNear(-1, GlobeGeometry::CosAngleToCenter(180, 0, 0, 0), 1e-6, "cos at antipode");
    CheckNear(1, GlobeGeometry::CosAngleToCenter(0, 0, 0, 0), 1e-6, "cos at center");
  }
  {
    TEST_CASE("rotationCenteringLandsThePointAtScreenCenter");
    const GlobeRotation rotation = GlobeGeometry::RotationCentering(-122.4f, 37.8f);
    CheckNear(122.4, rotation.lambda, 1e-4, "lambda");
    CheckNear(-37.8, rotation.phi, 1e-4, "phi");
    CheckPoint(300, 300,
               GlobeGeometry::Project(-122.4f, 37.8f, rotation.lambda, rotation.phi, 300),
               "centered point");
    CheckNear(1, GlobeGeometry::CosAngleToCenter(-122.4f, 37.8f, rotation.lambda, rotation.phi),
              1e-6, "cos at centered point");
  }
  {
    TEST_CASE("projectClampedMatchesProjectOnTheVisibleHemisphere");
    const auto projected = GlobeGeometry::Project(30, 40, 10, -20, 300);
    const GlobePoint clamped = GlobeGeometry::ProjectClamped(30, 40, 10, -20, 300);
    Check(projected.has_value(), "front point projects");
    if (projected) {
      CheckNear(projected->x, clamped.x, 1e-4, "clamped.x");
      CheckNear(projected->y, clamped.y, 1e-4, "clamped.y");
    }
  }
  {
    TEST_CASE("projectClampedPutsBackPointsOnTheSilhouetteCircle");
    // the exact antipode has no azimuthal direction; clamps to (600, 300)
    const GlobePoint antipode = GlobeGeometry::ProjectClamped(180, 0, 0, 0, 300);
    CheckNear(300, DistanceToCenter(antipode), 1e-2, "antipode radius");
    CheckNear(600, antipode.x, 1e-2, "antipode.x");
    CheckNear(300, antipode.y, 1e-2, "antipode.y");

    // (135, 45) at rotation (0, 0): rotated vector is
    // x = cos(135) cos(45) = -0.5 (behind), y = sin(135) cos(45) = 0.5,
    // z = sin(45) = 0.7071068, so the azimuthal direction (y, z) normalized by
    // sqrt(0.5^2 + 0.7071068^2) = 0.8660254 gives
    // px = 300 + 300 * 0.5 / 0.8660254 = 473.205
    // py = 300 - 300 * 0.7071068 / 0.8660254 = 55.051
    const GlobePoint back = GlobeGeometry::ProjectClamped(135, 45, 0, 0, 300);
    CheckNear(300, DistanceToCenter(back), 1e-2, "back radius");
    CheckNear(473.205, back.x, 1e-2, "back.x");
    CheckNear(55.051, back.y, 1e-2, "back.y");
  }
  {
    TEST_CASE("lerpRotationTakesTheShortWayAroundTheDateLine");
    // 170 -> -170 is 20 degrees through the date line, not 340 back; the
    // midpoint is the date line itself (180 and -180 are the same)
    const GlobeRotation mid = GlobeGeometry::LerpRotation({170, 0}, {-170, 0}, 0.5f);
    CheckNear(180, std::fabs(mid.lambda), 1e-4, "|mid lambda|");
    CheckNear(0, mid.phi, 1e-6, "mid phi");
  }
  {
    TEST_CASE("lerpRotationTreatsLongitudesModulo360");
    // 350 is -10: from 10 the short way is backward 20 degrees
    CheckNear(0, GlobeGeometry::LerpRotation({10, 0}, {350, 0}, 0.5f).lambda, 1e-4, "mid");
    CheckNear(-10, GlobeGeometry::LerpRotation({10, 0}, {350, 0}, 1.0f).lambda, 1e-4, "end");
    const GlobeRotation start = GlobeGeometry::LerpRotation({10, 20}, {350, -40}, 0.0f);
    CheckNear(10, start.lambda, 1e-6, "start lambda");
    CheckNear(20, start.phi, 1e-6, "start phi");
  }
  {
    TEST_CASE("lerpRotationInterpolatesAndClampsPhi");
    CheckNear(10, GlobeGeometry::LerpRotation({0, -30}, {0, 50}, 0.5f).phi, 1e-4, "mid phi");
    // phi never leaves [-90, 90] even for out-of-range endpoints
    CheckNear(90, GlobeGeometry::LerpRotation({0, 80}, {0, 120}, 1.0f).phi, 1e-6, "clamped phi");
  }
  {
    TEST_CASE("dragSensitivityMatchesTheWebFormula");
    // Globe.jsx: k = width / projection.scale() / (3 * Math.PI), applied to
    // projection.rotate() which takes degrees. At scale 300:
    // 600 / 300 / (3 * pi) = 2 / (3 * pi) = 0.2122066 degrees per px.
    CheckNear(0.2122066, GlobeGeometry::DragDegreesPerVirtualPx(300), 1e-4, "k at 300");
    // doubling the zoom halves the sensitivity
    CheckNear(GlobeGeometry::DragDegreesPerVirtualPx(300) / 2.0,
              GlobeGeometry::DragDegreesPerVirtualPx(600), 1e-6, "k at 600");
  }
  {
    TEST_CASE("graticuleHasTheD3DefaultLineStructure");
    const auto& lines = GlobeGeometry::Graticule();
    CheckEqualInt(53, static_cast<long long>(lines.size()), "line count");
    int meridians = 0;
    int parallels = 0;
    for (const auto& line : lines) {
      Check(line.size() >= 4, "line has at least 2 points");
      Check(line.size() % 2 == 0, "line float count is even");
      const bool constantLon = AllEqual(line, 0);
      const bool constantLat = AllEqual(line, 1);
      Check(constantLon || constantLat, "line is neither a meridian nor a parallel");
      if (constantLon) {
        ++meridians;
      } else {
        ++parallels;
      }
    }
    // 4 major meridians (-180, -90, 0, 90) + 32 minor (every 10 degrees
    // skipping multiples of 90); the equator + 16 minor parallels
    CheckEqualInt(36, meridians, "meridians");
    CheckEqualInt(17, parallels, "parallels");
  }
  {
    TEST_CASE("graticuleStaysInWorldBoundsWithD3Extents");
    int fullMeridians = 0;
    int minorMeridians = 0;
    for (const auto& line : GlobeGeometry::Graticule()) {
      float minLat = 90;
      float maxLat = -90;
      for (size_t i = 0; i < line.size(); i += 2) {
        Check(line[i] >= -180.0001f && line[i] <= 180.0001f, "lon in bounds");
        Check(line[i + 1] >= -90.0001f && line[i + 1] <= 90.0001f, "lat in bounds");
        minLat = (std::min)(minLat, line[i + 1]);
        maxLat = (std::max)(maxLat, line[i + 1]);
      }
      if (AllEqual(line, 0)) {
        // major meridians run pole to pole, minor ones stop at 80
        if (maxLat > 85.0f) {
          ++fullMeridians;
        } else {
          ++minorMeridians;
          CheckNear(80, maxLat, 1e-3, "minor meridian maxLat");
          CheckNear(-80, minLat, 1e-3, "minor meridian minLat");
        }
      } else {
        // parallels span the full longitude range
        CheckNear(-180, line[0], 1e-3, "parallel first lon");
        CheckNear(180, line[line.size() - 2], 1e-3, "parallel last lon");
      }
    }
    CheckEqualInt(4, fullMeridians, "full meridians");
    CheckEqualInt(32, minorMeridians, "minor meridians");
  }
  {
    TEST_CASE("graticuleIsSampledEvery2Point5Degrees");
    for (const auto& line : GlobeGeometry::Graticule()) {
      const size_t varyingOffset = AllEqual(line, 0) ? 1 : 0;
      float maxStep = 0;
      for (size_t i = varyingOffset + 2; i < line.size(); i += 2) {
        const float step = line[i] - line[i - 2];
        // monotone, never a gap wider than the 2.5 degree precision (the final
        // segment may be shorter where the span is not an exact multiple)
        Check(step >= -1e-4f, "graticule sampling is monotone");
        Check(step <= 2.5f + 1e-3f, "graticule sampling never exceeds 2.5 degrees");
        maxStep = (std::max)(maxStep, step);
      }
      CheckNear(2.5, maxStep, 1e-3, "max sampling step");
    }
  }
  {
    TEST_CASE("nearestWithinPicksTheClosestPointInRange");
    const std::vector<GlobePoint> points = {{100, 100}, {200, 200}, {105, 100}};
    CheckEqualInt(0, GlobeGeometry::NearestWithin(101, 100, points, 10), "near first");
    CheckEqualInt(2, GlobeGeometry::NearestWithin(104, 100, points, 10), "near third");
    CheckEqualInt(1, GlobeGeometry::NearestWithin(201, 199, points, 10), "near second");
  }
  {
    TEST_CASE("nearestWithinRespectsTheRadius");
    const std::vector<GlobePoint> points = {{0, 0}};
    CheckEqualInt(-1, GlobeGeometry::NearestWithin(300, 300, points, 5), "far away");
    // the radius is inclusive: distance from (3, 4) to (0, 0) is 5
    CheckEqualInt(0, GlobeGeometry::NearestWithin(3, 4, points, 5), "exactly at radius");
    CheckEqualInt(-1, GlobeGeometry::NearestWithin(3, 4.01f, points, 5), "just outside radius");
    CheckEqualInt(-1, GlobeGeometry::NearestWithin(0, 0, {}, 100), "empty point list");
  }

  // The globe is a scroll wheel when providers are present: a horizontal drag
  // steps the selection once it passes the hysteresis threshold.
  {
    TEST_CASE("aDragShorterThanTheThresholdDoesNotStep");
    const WheelStep step = GlobeGeometry::ResolveWheelStep(-49, 50);
    CheckEqualInt(0, step.steps, "steps");
    // the travel is carried, so continuing the same drag still steps
    CheckNear(-49, step.remainingTravel, 1e-4, "remaining travel");
  }
  {
    TEST_CASE("swipingLeftAdvancesAndSwipingRightGoesBack");
    CheckEqualInt(1, GlobeGeometry::ResolveWheelStep(-50, 50).steps, "left advances");
    CheckEqualInt(-1, GlobeGeometry::ResolveWheelStep(50, 50).steps, "right goes back");
  }
  {
    TEST_CASE("aFastDragCrossesSeveralStepsAtOnce");
    const WheelStep step = GlobeGeometry::ResolveWheelStep(-170, 50);
    CheckEqualInt(3, step.steps, "steps");
    // 20px of the drag is left over toward the next step
    CheckNear(-20, step.remainingTravel, 1e-4, "remaining travel");
  }
  {
    // the hysteresis: after stepping, another full threshold is required, so a
    // pointer resting at the boundary cannot flicker between two providers
    TEST_CASE("steppingConsumesExactlyOneThresholdOfTravel");
    float travel = -50;
    const WheelStep first = GlobeGeometry::ResolveWheelStep(travel, 50);
    CheckEqualInt(1, first.steps, "first step");
    CheckNear(0, first.remainingTravel, 1e-4, "travel consumed");

    // jitter back and forth around the boundary must not step again
    travel = first.remainingTravel;
    for (const float jitter : {-20.0f, 15.0f, -18.0f, 12.0f}) {
      travel += jitter;
      const WheelStep step = GlobeGeometry::ResolveWheelStep(travel, 50);
      CheckEqualInt(0, step.steps, "jitter does not step");
      travel = step.remainingTravel;
    }
  }
  {
    TEST_CASE("aNonPositiveThresholdNeverSteps");
    CheckEqualInt(0, GlobeGeometry::ResolveWheelStep(-1000, 0).steps, "zero threshold");
  }
  // The wheel order and the step clamping are the SDK's
  // ProviderLocationsViewController (provider_locations_view_controller.go,
  // tested there); this module only converts drag travel to step counts.

  // fit center: the globe scales to the smaller canvas dimension and centers in
  // both, so a wide (non-square) box neither crops nor offsets it
  {
    TEST_CASE("unitFitsTheSmallerDimension");
    CheckNear(1, GlobeGeometry::UnitFor(600, 600), 1e-4, "square");
    // 800x600 -> fits the 600 height
    CheckNear(1, GlobeGeometry::UnitFor(800, 600), 1e-4, "wide");
    // 600x450 (the 0.75 height ratio) -> fits the 450 height
    CheckNear(0.75, GlobeGeometry::UnitFor(600, 450), 1e-4, "0.75 height");
  }
  {
    TEST_CASE("virtualCenterMapsToTheCanvasCenterOfAWideBox");
    const GlobePoint center =
        GlobeGeometry::ToCanvas({GlobeGeometry::kCenter, GlobeGeometry::kCenter}, 800, 600);
    CheckNear(400, center.x, 1e-3, "center.x");
    CheckNear(300, center.y, 1e-3, "center.y");
  }
  {
    TEST_CASE("theGlobeEdgesStayInsideAWideBox");
    const float width = 800;
    const float height = 600;
    // the extreme points of the virtual space (the sphere's bounding box)
    const GlobePoint left = GlobeGeometry::ToCanvas({0, GlobeGeometry::kCenter}, width, height);
    const GlobePoint right =
        GlobeGeometry::ToCanvas({GlobeGeometry::kVirtualSize, GlobeGeometry::kCenter}, width, height);
    const GlobePoint top = GlobeGeometry::ToCanvas({GlobeGeometry::kCenter, 0}, width, height);
    const GlobePoint bottom =
        GlobeGeometry::ToCanvas({GlobeGeometry::kCenter, GlobeGeometry::kVirtualSize}, width, height);
    // fits the height exactly, and is inset horizontally (centered)
    CheckNear(0, top.y, 1e-3, "top.y");
    CheckNear(height, bottom.y, 1e-3, "bottom.y");
    CheckNear(100, left.x, 1e-3, "left.x");
    CheckNear(700, right.x, 1e-3, "right.x");
    CheckNear(width / 2 - left.x, right.x - width / 2, 1e-3, "horizontally centered");
  }
  {
    TEST_CASE("toVirtualInvertsToCanvas");
    const std::vector<std::pair<float, float>> sizes = {{600, 600}, {800, 600}, {400, 700}};
    const std::vector<GlobePoint> points = {
        {GlobeGeometry::kCenter, GlobeGeometry::kCenter}, {120, 480}, {590, 10}};
    for (const auto& size : sizes) {
      for (const auto& point : points) {
        const GlobePoint canvas = GlobeGeometry::ToCanvas(point, size.first, size.second);
        const GlobePoint back =
            GlobeGeometry::ToVirtual(canvas.x, canvas.y, size.first, size.second);
        CheckNear(point.x, back.x, 1e-2, "round trip x");
        CheckNear(point.y, back.y, 1e-2, "round trip y");
      }
    }
  }
}

// ---- WorldTopologyTest.kt --------------------------------------------------

std::string ReadAsset(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void WorldTopologyTests(const std::string& assetPath) {
  const std::string text = ReadAsset(assetPath);
  if (text.empty()) {
    std::cout << "  SKIP world-110m.json not found at " << assetPath << "\n";
    return;
  }
  const auto decoded = WorldTopology::Decode(text);
  {
    TEST_CASE("decodesAll177Countries");
    if (!decoded) {
      Fail("decode returned nullopt");
      return;
    }
    CheckEqualInt(177, static_cast<long long>(decoded->Countries().size()), "country count");
    for (const auto& country : decoded->Countries()) {
      Check(!country.isoNumeric.empty(), "country has an iso numeric id");
    }
  }
  {
    TEST_CASE("knownCountriesArePresent");
    std::set<std::string> ids;
    for (const auto& country : decoded->Countries()) ids.insert(country.isoNumeric);
    Check(ids.count("840") == 1, "USA missing");
    Check(ids.count("036") == 1, "Australia missing");
  }
  {
    TEST_CASE("ringsAreWellFormedPolylines");
    int ringCount = 0;
    for (const auto& country : decoded->Countries()) {
      Check(!country.rings.empty(), "country has rings");
      for (const auto& ring : country.rings) {
        ++ringCount;
        Check(ring.size() % 2 == 0, "odd float count");
        Check(ring.size() >= 8, "ring under 4 points");
      }
    }
    Check(ringCount >= 100, "at least 100 rings");
  }
  {
    TEST_CASE("coordinatesAreWithinWorldBounds");
    for (const auto& country : decoded->Countries()) {
      for (const auto& ring : country.rings) {
        for (size_t i = 0; i < ring.size(); i += 2) {
          Check(ring[i] >= -180.0001f && ring[i] <= 180.0001f, "lon in bounds");
          Check(ring[i + 1] >= -90.0001f && ring[i + 1] <= 90.0001f, "lat in bounds");
        }
      }
    }
  }
  {
    // TopoJSON polygon rings close: the first point of the first arc equals the
    // last point of the last arc; a stitching bug (dropped or duplicated shared
    // endpoints) breaks this
    TEST_CASE("everyRingCloses");
    for (const auto& country : decoded->Countries()) {
      for (const auto& ring : country.rings) {
        CheckNear(ring[0], ring[ring.size() - 2], 1e-3, country.isoNumeric + " ring close lon");
        CheckNear(ring[1], ring[ring.size() - 1], 1e-3, country.isoNumeric + " ring close lat");
      }
    }
  }
  {
    // world-110m decodes to ~10,500 ring points; far fewer means dropped arcs,
    // far more means shared endpoints were not deduplicated
    TEST_CASE("totalPointCountIsInTheExpectedBand");
    long long totalPoints = 0;
    for (const auto& country : decoded->Countries()) {
      for (const auto& ring : country.rings) totalPoints += static_cast<long long>(ring.size() / 2);
    }
    Check(5000 <= totalPoints && totalPoints <= 60000,
          "total points " + std::to_string(totalPoints));
    std::cout << "  (total ring points: " << totalPoints << ")\n";
  }
  {
    // independently decoded (python): the first ring of the USA MultiPolygon
    // (Hawaii) has 17 points and starts at (-155.541355, 19.084175)
    TEST_CASE("dequantizesKnownUsaCoordinates");
    const CountryShape* usa = nullptr;
    for (const auto& country : decoded->Countries()) {
      if (country.isoNumeric == "840") usa = &country;
    }
    if (!usa) {
      Fail("USA missing");
      return;
    }
    const auto& firstRing = usa->rings.front();
    CheckEqualInt(17 * 2, static_cast<long long>(firstRing.size()), "first ring float count");
    CheckNear(-155.541355, firstRing[0], 5e-4, "first lon");
    CheckNear(19.084175, firstRing[1], 5e-4, "first lat");
    Check(std::fabs(firstRing[0] - firstRing[firstRing.size() - 2]) < 1e-3f, "ring closes");
  }
}

// ---- row labels (android ProviderLocations label tests) --------------------

ProviderLocationRow MakeRow(const char* city, const char* region, const char* country) {
  ProviderLocationRow row;
  row.city = city;
  row.region = region;
  row.country = country;
  return row;
}

void ProviderLocationsTests() {
  {
    TEST_CASE("placeLabelJoinsCityRegionCountry");
    CheckEqualString("San Francisco, California, United States",
                     PlaceLabel(MakeRow("San Francisco", "California", "United States")), "full");
    CheckEqualString("California, United States", PlaceLabel(MakeRow("", "California", "United States")),
                     "no city");
    CheckEqualString("United States", PlaceLabel(MakeRow("", "", "United States")), "country only");
    CheckEqualString("", PlaceLabel(MakeRow("", "", "")), "nothing known");
  }
  {
    TEST_CASE("coordinatesLabelUsesFourDecimalsAndAnEmDashWhenUnknown");
    ProviderLocationRow row;
    row.hasCoordinates = true;
    row.lat = 37.77493;
    row.lon = -122.41942;
    CheckEqualString("37.7749, -122.4194", CoordinatesLabel(row), "four decimals");
    ProviderLocationRow unknown;
    CheckEqualString("\xE2\x80\x94", CoordinatesLabel(unknown), "em dash");
    // 0,0 is a legitimate coordinate, not "unknown"
    ProviderLocationRow nullIsland;
    nullIsland.hasCoordinates = true;
    CheckEqualString("0.0000, 0.0000", CoordinatesLabel(nullIsland), "null island");
  }
  {
    TEST_CASE("splitConnectedDurationBucketsElapsedTime");
    const int64_t now = 1'700'000'000'000LL;
    ConnectedDuration d = SplitConnectedDuration(now - 45'000, now);
    Check(d.valid, "valid");
    CheckEqualInt(0, d.hours, "45s hours");
    CheckEqualInt(0, d.minutes, "45s minutes");
    CheckEqualInt(45, d.seconds, "45s seconds");

    d = SplitConnectedDuration(now - (3 * 60 + 7) * 1000, now);
    CheckEqualInt(0, d.hours, "3m7s hours");
    CheckEqualInt(3, d.minutes, "3m7s minutes");
    CheckEqualInt(7, d.seconds, "3m7s seconds");

    d = SplitConnectedDuration(now - (2 * 3600 + 5 * 60 + 9) * 1000, now);
    CheckEqualInt(2, d.hours, "2h5m hours");
    CheckEqualInt(5, d.minutes, "2h5m minutes");

    // an unknown stamp is not a zero duration
    Check(!SplitConnectedDuration(0, now).valid, "zero stamp is invalid");
    // clock skew (a remote viewer) must never produce a negative duration
    d = SplitConnectedDuration(now + 10'000, now);
    Check(d.valid, "future stamp is still valid");
    CheckEqualInt(0, d.seconds, "future stamp clamps to zero");
  }
  // The globe's wheel order and its clamped stepping are the SDK's
  // ProviderLocationsViewController (provider_locations_view_controller.go,
  // tested there), so there is nothing to order here.
}

}  // namespace

int main(int argc, char** argv) {
  const std::string assetPath =
      1 < argc ? argv[1] : std::string("../src/App/Assets/world-110m.json");

  std::cout << "GlobeGeometry\n";
  GlobeGeometryTests();
  std::cout << "WorldTopology\n";
  WorldTopologyTests(assetPath);
  std::cout << "ProviderLocations\n";
  ProviderLocationsTests();

  std::cout << "\n" << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
