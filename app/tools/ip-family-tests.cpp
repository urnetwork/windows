// Executable spec for the dual-stack pure logic (connect/IPV6.md): the
// IP-family histogram grouping and dot-size rule (App/IpFamilyGroups.h), the
// provider row's family fields (App/ProviderLocations.h), and the IPv6 half of
// the tunnel's route/firewall table (Service/NetPolicy.h) — run against the
// SAME sources the app and the service compile, on any host with a C++20
// compiler. The Windows-only halves (NetworkConfig's settings validation and
// the WFP filter set) are covered by urnetworkd's own selftest.
//
//   c++ -std=c++20 -I ../src/App -I ../src/Service ip-family-tests.cpp \
//       ../src/App/IpFamilyGroups.cpp ../src/App/ProviderLocations.cpp \
//       -o /tmp/ip-family-tests && /tmp/ip-family-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "IpFamilyGroups.h"
#include "NetPolicy.h"
#include "ProviderLocations.h"

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

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

IpFamilyDot Dot(const char* id, const char* state, const char* family) {
  return IpFamilyDot{id, state, family};
}

std::string Text6(const net::V6Prefix& p) {
  const auto b = p.Bytes();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%02x%02x:%02x%02x::/%u", b[0], b[1], b[2], b[3],
                static_cast<unsigned>(p.prefix));
  return buf;
}

// ---- IpFamilyGroups.h ------------------------------------------------------

void GroupingTests() {
  {
    TEST_CASE("categoryMapsToRow");
    Check(IpFamilyGroupFor("dualstack") == IpFamilyGroup::Both, "dualstack -> Both");
    Check(IpFamilyGroupFor("v4-only") == IpFamilyGroup::V4, "v4-only -> V4");
    Check(IpFamilyGroupFor("v6-only") == IpFamilyGroup::V6, "v6-only -> V6");
    Check(IpFamilyGroupFor("") == IpFamilyGroup::V4, "legacy (empty) -> V4");
    Check(IpFamilyGroupFor("v7-only") == IpFamilyGroup::V4,
          "an unknown category is v4-only, never a v6 claim");
  }
  {
    TEST_CASE("tokensRoundTrip");
    for (IpFamilyGroup g : {IpFamilyGroup::Both, IpFamilyGroup::V4, IpFamilyGroup::V6}) {
      Check(IpFamilyGroupForToken(IpFamilyGroupToken(g)) == g, "token round trip");
    }
    Check(std::string(IpFamilyGroupToken(IpFamilyGroup::Both)) == "both", "both token");
    Check(std::string(IpFamilyGroupToken(IpFamilyGroup::V4)) == "v4", "v4 token");
    Check(std::string(IpFamilyGroupToken(IpFamilyGroup::V6)) == "v6", "v6 token");
    Check(IpFamilyGroupForToken("") == IpFamilyGroup::V4, "empty token -> V4");
    Check(IpFamilyGroupForToken("dualstack") == IpFamilyGroup::V4,
          "a category is not a token; unknown tokens read as v4");
  }
  {
    TEST_CASE("onlyAddedProvidersCount");
    const IpFamilyGroups g = GroupAddedProvidersByIpFamily({
        Dot("a", "Added", "dualstack"),
        Dot("b", "InEvaluation", "dualstack"),
        Dot("c", "EvaluationFailed", "v4-only"),
        Dot("d", "NotAdded", "v6-only"),
        Dot("e", "Removed", "dualstack"),
        Dot("f", "Added", "v6-only"),
        Dot("g", "Added", "v4-only"),
        Dot("h", "SomethingNew", "dualstack"),
    });
    Check(g.both == std::vector<std::string>{"a"}, "both = a");
    Check(g.v4 == std::vector<std::string>{"g"}, "v4 = g");
    Check(g.v6 == std::vector<std::string>{"f"}, "v6 = f");
    Check(g.Total() == 3, "three added providers");
  }
  {
    TEST_CASE("legacyAndUnknownFamiliesLandUnderV4");
    const IpFamilyGroups g = GroupAddedProvidersByIpFamily({
        Dot("legacy", "Added", ""),
        Dot("future", "Added", "v4-v6-v7"),
        Dot("plain", "Added", "v4-only"),
    });
    Check(g.v4 == std::vector<std::string>{"legacy", "future", "plain"},
          "all three under v4, in input order");
    Check(g.both.empty() && g.v6.empty(), "nothing elsewhere");
  }
  {
    TEST_CASE("bareCellsAreNotProviders");
    const IpFamilyGroups g = GroupAddedProvidersByIpFamily({
        Dot("", "Added", "dualstack"),
        Dot("x", "Added", "dualstack"),
    });
    Check(g.both == std::vector<std::string>{"x"}, "the empty-id cell is skipped");
  }
  {
    TEST_CASE("inputOrderIsPreservedPerRow");
    const IpFamilyGroups g = GroupAddedProvidersByIpFamily({
        Dot("3", "Added", "dualstack"),
        Dot("1", "Added", "dualstack"),
        Dot("2", "Added", "dualstack"),
    });
    Check(g.both == std::vector<std::string>{"3", "1", "2"}, "not sorted, not deduped by value");
    Check(&g.For(IpFamilyGroup::Both) == &g.both && &g.For(IpFamilyGroup::V4) == &g.v4 &&
              &g.For(IpFamilyGroup::V6) == &g.v6,
          "For() addresses the matching row");
  }
  {
    TEST_CASE("groupsCompareByValue");
    const IpFamilyGroups a = GroupAddedProvidersByIpFamily({Dot("a", "Added", "dualstack")});
    const IpFamilyGroups b = GroupAddedProvidersByIpFamily({Dot("a", "Added", "dualstack")});
    const IpFamilyGroups c = GroupAddedProvidersByIpFamily({Dot("a", "Added", "v6-only")});
    Check(a == b, "same input, equal groups");
    Check(a != c, "a provider moving rows is a change");
  }
  {
    TEST_CASE("emptyGridIsThreeEmptyRows");
    const IpFamilyGroups g = GroupAddedProvidersByIpFamily({});
    Check(g.Total() == 0 && g.both.empty() && g.v4.empty() && g.v6.empty(), "all empty");
  }
}

// ---- the dot size rule (ConnectCanvas::Layout parity) ----------------------

void DotDiameterTests() {
  {
    TEST_CASE("diameterIsSideOverColumns");
    CheckNear(256.0 / 14, IpFamilyDotDiameter(256, 14, 14), 1e-9, "square 14 grid on 256");
    CheckNear(288.0 / 16, IpFamilyDotDiameter(288, 16, 16), 1e-9, "square 16 grid on 288");
    CheckNear(168.0 / 10, IpFamilyDotDiameter(168, 10, 10), 1e-9, "square 10 grid on 168");
  }
  {
    TEST_CASE("nonSquareGridUsesTheLargerDimension");
    CheckNear(256.0 / 20, IpFamilyDotDiameter(256, 12, 20), 1e-9, "taller than wide");
    CheckNear(256.0 / 20, IpFamilyDotDiameter(256, 20, 12), 1e-9, "wider than tall");
    CheckNear(256.0 / 12, IpFamilyDotDiameter(256, 12, 0), 1e-9, "no height reported");
  }
  {
    TEST_CASE("unmeasuredCanvasUsesTheIosCanvas");
    CheckNear(256.0 / 14, IpFamilyDotDiameter(0, 14, 14), 1e-9, "side 0 -> 256");
    CheckNear(256.0 / 14, IpFamilyDotDiameter(-5, 14, 14), 1e-9, "negative side -> 256");
  }
  {
    TEST_CASE("shapelessGridUsesTheDefaultColumns");
    CheckNear(256.0 / kIpFamilyDefaultGridWidth, IpFamilyDotDiameter(0, 0, 0), 1e-9,
              "nothing known");
    CheckNear(288.0 / kIpFamilyDefaultGridWidth, IpFamilyDotDiameter(288, 0, 0), 1e-9,
              "side known, grid not");
    Check(0 < IpFamilyDotDiameter(0, 0, 0), "never zero");
  }
}

// ---- ProviderLocations.h: the family rides the row --------------------------

void ProviderRowTests() {
  {
    TEST_CASE("rowEqualityIncludesTheFamily");
    ProviderLocationRow a;
    a.clientId = "c";
    a.ipFamily = "dualstack";
    a.ipFamilyLabel = "both";
    ProviderLocationRow b = a;
    Check(a == b, "copies are equal");
    b.ipFamily = "v4-only";
    b.ipFamilyLabel = "v4";
    Check(a != b, "a family change (the local downgrade) is a row change, so the "
                  "sheet re-renders it");
    ProviderLocationRow legacy;
    legacy.clientId = "c";
    Check(legacy.ipFamily.empty() && legacy.ipFamilyLabel.empty(),
          "an SDK without the field leaves both empty");
  }
}

// ---- NetPolicy.h: the IPv6 half of THE table --------------------------------

void NetPolicyV6Tests() {
  {
    TEST_CASE("captureSetIsTheValidatedEightPrefixes");
    const net::V6Prefix expected[] = {
        {0x0000'0000'0000'0000ull, 0, 1}, {0x8000'0000'0000'0000ull, 0, 2},
        {0xC000'0000'0000'0000ull, 0, 3}, {0xE000'0000'0000'0000ull, 0, 4},
        {0xF000'0000'0000'0000ull, 0, 5}, {0xF800'0000'0000'0000ull, 0, 6},
        {0xFE00'0000'0000'0000ull, 0, 9}, {0xFEC0'0000'0000'0000ull, 0, 10},
    };
    Check(net::kTunCaptureV6Count == 8, "eight prefixes");
    for (std::size_t i = 0; i < net::kTunCaptureV6Count && i < 8; ++i) {
      Check(net::kTunCaptureV6[i] == expected[i],
            "index " + std::to_string(i) + ": " + Text6(net::kTunCaptureV6[i]));
    }
  }
  {
    TEST_CASE("captureUnionBypassCoversEverythingExactly");
    long double covered = 0;
    for (const auto& p : net::kTunCaptureV6) covered += std::ldexp(1.0L, -int(p.prefix));
    for (const auto& p : net::kLocalBypassV6) covered += std::ldexp(1.0L, -int(p.prefix));
    Check(covered == 1.0L, "sum of 2^-prefix over capture and bypass is exactly 1");
    for (const auto& c : net::kTunCaptureV6) {
      for (const auto& b : net::kLocalBypassV6) {
        Check(!net::detail::Contains(b, c) && !net::detail::Contains(c, b),
              Text6(c) + " must not intersect " + Text6(b));
      }
    }
  }
  {
    TEST_CASE("capturePrefixesAreSortedAndMinimal");
    for (std::size_t i = 1; i < net::kTunCaptureV6Count; ++i) {
      const auto& a = net::kTunCaptureV6[i - 1];
      const auto& b = net::kTunCaptureV6[i];
      Check(a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo), "ascending");
    }
    Check(net::kTunCaptureV6[0].prefix == 1, "::/1 is one prefix, not split further");
  }
  {
    TEST_CASE("bypassProbes");
    struct { uint8_t addr[16]; bool bypass; const char* label; } probes[] = {
        {{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0x65, 0, 0x49, 0, 0x70, 0, 0x65}, false,
         "2001:db8::65:49:70:65 (the SDK's in-tunnel resolver) is captured"},
        {{0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x11}, false,
         "2606:4700:4700::1111 is captured"},
        {{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, true, "fe80::1 bypasses"},
        {{0xfe, 0xbf, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, true,
         "febf::1 (top of fe80::/10) bypasses"},
        {{0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, false,
         "fec0::1 (just above fe80::/10) is captured"},
        {{0xfc, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, true, "fc00::1 bypasses"},
        {{0xfd, 0x00, 0x75, 0x72, 0x6e, 0x65, 0x12, 0x34, 0, 0, 0, 0, 0, 0, 0, 1}, true,
         "fd00:7572:6e65:1234::1 (our own tun ULA) bypasses; its on-link /64 "
         "still wins"},
        {{0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff}, true, "top of fc00::/7 bypasses"},
        {{0xfe, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, false,
         "fe00::1 (just above fc00::/7) is captured"},
        {{0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, true, "ff02::1 bypasses"},
        {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff}, true, "ffff::ffff bypasses"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, true, "::1 bypasses (loopback)"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, ":: is captured"},
        {{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}, false, "::2 is captured"},
        {{0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, false,
         "64:ff9b::1 (NAT64) is captured"},
        {{0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
          0xff, 0xff}, false, "top of ::/1 is captured"},
        {{0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, false, "8000:: is captured"},
    };
    for (const auto& p : probes) {
      Check(net::IsLocalBypassV6(p.addr) == p.bypass, p.label);
    }
  }
  {
    TEST_CASE("bytesAndFromBytesRoundTrip");
    for (const auto& p : net::kLocalBypassV6) {
      const auto bytes = p.Bytes();
      uint8_t raw[16];
      for (int i = 0; i < 16; ++i) raw[i] = bytes[static_cast<std::size_t>(i)];
      Check(net::V6Prefix::FromBytes(raw, p.prefix) == p, "round trip " + Text6(p));
    }
    const uint8_t full[16] = {0x20, 0x01, 0x0d, 0xb8, 0x11, 0x22, 0x33, 0x44,
                              0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};
    const net::V6Prefix p = net::V6Prefix::FromBytes(full, 128);
    Check(p.hi == 0x2001'0db8'1122'3344ull && p.lo == 0x5566'7788'99aa'bbccull,
          "both halves populated in network order");
    const auto back = p.Bytes();
    bool same = true;
    for (int i = 0; i < 16; ++i)
      if (back[static_cast<std::size_t>(i)] != full[i]) same = false;
    Check(same, "Bytes() reproduces the input");
  }
  {
    TEST_CASE("v4TableUnchanged");
    Check(net::kTunCaptureV4Count == 31, "the v4 capture set still has 31 prefixes");
  }
}

}  // namespace

int main() {
  std::cout << "IpFamilyGroups\n";
  GroupingTests();
  std::cout << "IpFamilyDotDiameter\n";
  DotDiameterTests();
  std::cout << "ProviderLocationRow\n";
  ProviderRowTests();
  std::cout << "NetPolicy v6\n";
  NetPolicyV6Tests();

  std::cout << "\n" << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
