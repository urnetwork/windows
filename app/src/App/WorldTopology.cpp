// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose — see WorldTopology.h. App.vcxproj compiles this with
// PrecompiledHeader=NotUsing.
#include "WorldTopology.h"

#include <nlohmann/json.hpp>

namespace urnw {
namespace {

using nlohmann::json;

// Concatenates the referenced arcs into one closed ring. A negative index i
// references arc ~i reversed. After orientation, each arc's first point equals
// the previous arc's last point, so the duplicate is dropped when stitching.
std::vector<float> StitchRing(const json& arcIndexes,
                              const std::vector<std::vector<float>>& arcs) {
  size_t pointCount = 0;
  for (const auto& element : arcIndexes) {
    const int index = element.get<int>();
    const size_t resolved = static_cast<size_t>(index >= 0 ? index : ~index);
    if (resolved >= arcs.size()) return {};
    pointCount += arcs[resolved].size() / 2;
  }
  if (pointCount < arcIndexes.size() - 1) return {};
  pointCount -= arcIndexes.size() - 1;

  std::vector<float> ring;
  ring.reserve(pointCount * 2);
  for (size_t k = 0; k < arcIndexes.size(); ++k) {
    const int index = arcIndexes[k].get<int>();
    const bool skipSharedEndpoint = 0 < k;
    if (index >= 0) {
      const std::vector<float>& arc = arcs[static_cast<size_t>(index)];
      const size_t from = skipSharedEndpoint ? 1 : 0;
      for (size_t p = from; p < arc.size() / 2; ++p) {
        ring.push_back(arc[2 * p]);
        ring.push_back(arc[2 * p + 1]);
      }
    } else {
      const std::vector<float>& arc = arcs[static_cast<size_t>(~index)];
      const size_t count = arc.size() / 2;
      if (count == 0) continue;
      // walk backward from the last point (or one before it when the shared
      // endpoint is already in the ring)
      size_t from = count - 1;
      if (skipSharedEndpoint) {
        if (from == 0) continue;
        --from;
      }
      for (size_t p = from + 1; p-- > 0;) {
        ring.push_back(arc[2 * p]);
        ring.push_back(arc[2 * p + 1]);
      }
    }
  }
  return ring;
}

}  // namespace

std::optional<WorldTopology> WorldTopology::Decode(const std::string& text) {
  json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded() || !root.is_object()) return std::nullopt;

  const auto transform = root.find("transform");
  if (transform == root.end() || !transform->is_object()) return std::nullopt;
  const auto scale = transform->find("scale");
  const auto translate = transform->find("translate");
  if (scale == transform->end() || translate == transform->end() || !scale->is_array() ||
      !translate->is_array() || scale->size() < 2 || translate->size() < 2) {
    return std::nullopt;
  }
  const double scaleX = (*scale)[0].get<double>();
  const double scaleY = (*scale)[1].get<double>();
  const double translateX = (*translate)[0].get<double>();
  const double translateY = (*translate)[1].get<double>();

  // Decode every arc once. Each arc is a list of [x, y] integer points where
  // the first point is absolute (quantized) and every later point is a delta;
  // the running sums dequantize to degrees as lon = x * scale[0] + translate[0],
  // lat = y * scale[1] + translate[1]. Packed as [lon0, lat0, lon1, lat1, ...].
  const auto arcsJson = root.find("arcs");
  if (arcsJson == root.end() || !arcsJson->is_array()) return std::nullopt;
  std::vector<std::vector<float>> arcs;
  arcs.reserve(arcsJson->size());
  for (const auto& arcJson : *arcsJson) {
    if (!arcJson.is_array()) return std::nullopt;
    std::vector<float> points(arcJson.size() * 2);
    long long x = 0;
    long long y = 0;
    for (size_t j = 0; j < arcJson.size(); ++j) {
      const auto& point = arcJson[j];
      if (!point.is_array() || point.size() < 2) return std::nullopt;
      x += point[0].get<long long>();
      y += point[1].get<long long>();
      points[2 * j] = static_cast<float>(static_cast<double>(x) * scaleX + translateX);
      points[2 * j + 1] = static_cast<float>(static_cast<double>(y) * scaleY + translateY);
    }
    arcs.push_back(std::move(points));
  }

  const auto objects = root.find("objects");
  if (objects == root.end() || !objects->is_object()) return std::nullopt;
  const auto countriesObject = objects->find("countries");
  if (countriesObject == objects->end() || !countriesObject->is_object()) return std::nullopt;
  const auto geometries = countriesObject->find("geometries");
  if (geometries == countriesObject->end() || !geometries->is_array()) return std::nullopt;

  WorldTopology world;
  world.countries_.reserve(geometries->size());
  for (const auto& geometry : *geometries) {
    if (!geometry.is_object()) return std::nullopt;
    CountryShape country;
    if (const auto id = geometry.find("id"); id != geometry.end()) {
      // ids are strings in this vintage of world-110m; tolerate numbers
      country.isoNumeric = id->is_string() ? id->get<std::string>() : id->dump();
    }
    const auto type = geometry.find("type");
    const auto arcIndexes = geometry.find("arcs");
    if (type == geometry.end() || !type->is_string() || arcIndexes == geometry.end() ||
        !arcIndexes->is_array()) {
      return std::nullopt;
    }
    const std::string typeName = type->get<std::string>();
    if (typeName == "Polygon") {
      // a Polygon is a list of rings, each a list of arc indexes
      for (const auto& ring : *arcIndexes) {
        if (!ring.is_array()) return std::nullopt;
        country.rings.push_back(StitchRing(ring, arcs));
      }
    } else if (typeName == "MultiPolygon") {
      // a MultiPolygon is a list of polygons
      for (const auto& polygon : *arcIndexes) {
        if (!polygon.is_array()) return std::nullopt;
        for (const auto& ring : polygon) {
          if (!ring.is_array()) return std::nullopt;
          country.rings.push_back(StitchRing(ring, arcs));
        }
      }
    } else {
      return std::nullopt;  // unsupported geometry type
    }
    world.countries_.push_back(std::move(country));
  }
  return world;
}

}  // namespace urnw
