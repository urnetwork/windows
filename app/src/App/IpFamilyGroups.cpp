// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose — see IpFamilyGroups.h. App.vcxproj compiles this with
// PrecompiledHeader=NotUsing.
#include "IpFamilyGroups.h"

#include <algorithm>

namespace urnw {

IpFamilyGroup IpFamilyGroupFor(std::string_view ipFamily) {
  if (ipFamily == "dualstack") return IpFamilyGroup::Both;
  if (ipFamily == "v6-only") return IpFamilyGroup::V6;
  // "v4-only", legacy (empty) and anything newer than this build
  return IpFamilyGroup::V4;
}

const char* IpFamilyGroupToken(IpFamilyGroup group) {
  switch (group) {
    case IpFamilyGroup::Both: return "both";
    case IpFamilyGroup::V6: return "v6";
    case IpFamilyGroup::V4: break;
  }
  return "v4";
}

IpFamilyGroup IpFamilyGroupForToken(std::string_view token) {
  if (token == "both") return IpFamilyGroup::Both;
  if (token == "v6") return IpFamilyGroup::V6;
  return IpFamilyGroup::V4;
}

const std::vector<std::string>& IpFamilyGroups::For(IpFamilyGroup group) const {
  switch (group) {
    case IpFamilyGroup::Both: return both;
    case IpFamilyGroup::V6: return v6;
    case IpFamilyGroup::V4: break;
  }
  return v4;
}

IpFamilyGroups GroupAddedProvidersByIpFamily(const std::vector<IpFamilyDot>& dots) {
  IpFamilyGroups groups;
  for (const IpFamilyDot& dot : dots) {
    // a bare cell (no client id) is a position, not a provider
    if (dot.clientId.empty()) continue;
    // the same vocabulary ConnectCanvas::ParsePointState reads; only Added
    // carries traffic, and an unrecognised state is a provider the SDK has not
    // accepted
    if (dot.state != "Added") continue;
    switch (IpFamilyGroupFor(dot.ipFamily)) {
      case IpFamilyGroup::Both: groups.both.push_back(dot.clientId); break;
      case IpFamilyGroup::V6: groups.v6.push_back(dot.clientId); break;
      case IpFamilyGroup::V4: groups.v4.push_back(dot.clientId); break;
    }
  }
  return groups;
}

double IpFamilyDotDiameter(double canvasSide, int64_t gridWidth, int64_t gridHeight) {
  const double side = 0 < canvasSide ? canvasSide : kIpFamilyDefaultCanvasSide;
  // ConnectCanvas::Layout: iOS scales by gridWidth alone; taking the larger of
  // the two keeps a non-square grid inside the globe
  int64_t cols = 0 < gridWidth ? gridWidth : 0;
  if (0 < gridHeight && cols < gridHeight) cols = gridHeight;
  if (cols <= 0) cols = kIpFamilyDefaultGridWidth;
  return side / static_cast<double>(cols);
}

}  // namespace urnw
