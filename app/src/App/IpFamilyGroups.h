// The pure logic behind the connect drawer's IP-family histogram (connect/
// IPV6.md D2): which providers land under Both / v4 / v6, and how big a dot
// is. Shared with the provider-locations rows, which show the same family
// label per provider.
//
// Pure standard C++ — no WinRT, no localization, no SDK header — so
// tools/ip-family-tests.cpp runs it on any host (App.vcxproj compiles this
// with PrecompiledHeader=NotUsing, like ProviderLocations.cpp). The WinUI
// layer (IpFamilyHistogram.cpp) converts the SDK's ProviderGridPoint into the
// input struct below and draws the result.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace urnw {

// The three histogram rows, in display order.
enum class IpFamilyGroup { Both, V4, V6 };

// The SDK category ("dualstack" / "v4-only" / "v6-only") -> the row. Legacy
// (empty) and any category this build does not know read as v4-only, which is
// the normalization the SDK itself applies at its boundary: an unknown
// category must not claim v6 it cannot prove.
IpFamilyGroup IpFamilyGroupFor(std::string_view ipFamily);

// The label token the apps share for a row ("both" / "v4" / "v6"), which is
// also what the SDK's ipFamilyLabel carries per provider.
const char* IpFamilyGroupToken(IpFamilyGroup group);

// The token -> the row, for the provider rows (which get the label from the
// SDK) and the tests. Unknown tokens read as v4, like unknown categories.
IpFamilyGroup IpFamilyGroupForToken(std::string_view token);

// One grid point, as much of it as the grouping needs.
struct IpFamilyDot {
  std::string clientId;  // the window-local id; empty for a bare cell
  std::string state;     // the SDK's ProviderGridPoint::State
  std::string ipFamily;  // the SDK's ProviderGridPoint::IpFamily
};

// ADDED providers only — the ones carrying traffic, which is what the widget's
// green dots are — bucketed by family, in input order, one entry per provider.
struct IpFamilyGroups {
  std::vector<std::string> both;
  std::vector<std::string> v4;
  std::vector<std::string> v6;

  std::size_t Total() const { return both.size() + v4.size() + v6.size(); }
  const std::vector<std::string>& For(IpFamilyGroup group) const;
  bool operator==(const IpFamilyGroups& o) const {
    return both == o.both && v4 == o.v4 && v6 == o.v6;
  }
  bool operator!=(const IpFamilyGroups& o) const { return !(*this == o); }
};

IpFamilyGroups GroupAddedProvidersByIpFamily(const std::vector<IpFamilyDot>& dots);

// The connect widget's dot diameter, by the canvas's own rule
// (ConnectCanvas::Layout): the globe's side over the larger grid dimension, so
// a non-square grid still fits inside the globe. `canvasSide` is the live
// side (168..288 on windows) or 0 before the first layout, when iOS's 256pt
// canvas stands in; a grid with no shape yet uses the default column count.
// Never zero, so a dot always has a size to be drawn at.
double IpFamilyDotDiameter(double canvasSide, int64_t gridWidth, int64_t gridHeight);

inline constexpr double kIpFamilyDefaultCanvasSide = 256.0;
inline constexpr int64_t kIpFamilyDefaultGridWidth = 14;

}  // namespace urnw
