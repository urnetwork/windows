// The world map decoded from quantized TopoJSON (Assets/world-110m.json, shipped
// as an RCDATA resource) — a port of the android WorldTopology.kt.
//
// TopoJSON stores shared borders once as delta-encoded quantized integer arcs;
// polygons reference arcs by index, with a negative index i meaning arc ~i
// traversed in reverse. See https://github.com/topojson/topojson.
//
// Only `objects.countries` is decoded; `land` and `bbox` are ignored.
//
// Like GlobeGeometry, this is pure standard C++ (nlohmann/json only, no WinRT)
// so the android WorldTopologyTest cases can run on the build host; App.vcxproj
// compiles it with PrecompiledHeader=NotUsing.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace urnw {

// One country from the world topology: its ISO-3166-1 numeric id (zero padded,
// e.g. "840" is the USA) and its outline rings in lon/lat degrees. Each ring is
// packed [lon0, lat0, lon1, lat1, ...] and is closed (first point equals last
// point). MultiPolygon countries contribute all of their rings, flattened.
struct CountryShape {
  std::string isoNumeric;
  std::vector<std::vector<float>> rings;
};

class WorldTopology {
 public:
  // Returns nullopt when the document is not the expected quantized TopoJSON
  // (malformed json, missing transform/arcs/objects.countries, or an
  // unsupported geometry type). The globe renders without land in that case
  // rather than failing to open.
  static std::optional<WorldTopology> Decode(const std::string& json);

  const std::vector<CountryShape>& Countries() const { return countries_; }

 private:
  std::vector<CountryShape> countries_;
};

}  // namespace urnw
