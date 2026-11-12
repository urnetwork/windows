// Executable spec for the extender UI's pure logic (connect/EXTENDER.md K2,
// K3, K4, K6, K7, K9, N7, O8): the ring geometry a provider dot is drawn with
// (App/ExtenderRingGeometry.h), the status mapping, settings form, QR
// placement and import decision the panel and the sheets act on, the provider
// extender row's reading and its switch guess, the router port mapping
// switch's status line, and which statistics sections the Earnings page shows
// (App/ExtenderPresentation.h), and a build+encode of
// the vendored Nayuki encoder the share screen renders with — run against the
// SAME sources the app compiles, on any host with a C++20 compiler.
//
// The WinUI halves (ExtenderPanel, ExtenderSheets, the ConnectCanvas drawing,
// the IpFamilyStatusRow, the two extender rows and the statistics groups)
// cannot be built off Windows at all; what is verified here is every decision
// they make before they touch a XAML object.
//
//   c++ -std=c++20 -I ../src/App -I ../third_party/qrcodegen extender-tests.cpp ../src/App/ExtenderPresentation.cpp ../third_party/qrcodegen/qrcodegen.cpp -o /tmp/extender-tests && /tmp/extender-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "ExtenderPresentation.h"
#include "ExtenderRingGeometry.h"
#include "SdkErrorId.h"
#include "qrcodegen.hpp"

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

void CheckEq(const std::string& expected, const std::string& actual,
             const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

void CheckEq(long long expected, long long actual, const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected " + std::to_string(expected) + ", got " +
         std::to_string(actual));
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

std::vector<ExtenderMark> Marks(std::initializer_list<const char*> colorHexes) {
  std::vector<ExtenderMark> marks;
  int i = 0;
  for (const char* hex : colorHexes) {
    ExtenderMark mark;
    mark.ip = "192.0.2." + std::to_string(++i);
    mark.colorHex = hex;
    marks.push_back(std::move(mark));
  }
  return marks;
}

// ---- ExtenderRingGeometry.h: colours ---------------------------------------

void ColorTests() {
  {
    TEST_CASE("theSdkHexParses");
    ExtenderRgb rgb{};
    // the pinned values of the SDK's GetExtenderColorHex (K3)
    Check(ParseExtenderColorHex("3cdd67", rgb), "192.0.2.1 -> 3cdd67 parses");
    Check(rgb == ExtenderRgb{0x3c, 0xdd, 0x67}, "3cdd67 channels");
    Check(ParseExtenderColorHex("dd4f3c", rgb), "2001:db8::1 -> dd4f3c parses");
    Check(rgb == ExtenderRgb{0xdd, 0x4f, 0x3c}, "dd4f3c channels");
    Check(ParseExtenderColorHex("#DD4F3C", rgb), "a leading # and upper case parse");
    Check(rgb == ExtenderRgb{0xdd, 0x4f, 0x3c}, "case folds");
  }
  {
    TEST_CASE("anythingElseIsNotAColor");
    ExtenderRgb rgb{0x11, 0x22, 0x33};
    const ExtenderRgb before = rgb;
    for (const char* bad : {"", "fff", "3cdd6", "3cdd677", "3cdd6g", "ff00ff00", "  "}) {
      Check(!ParseExtenderColorHex(bad, rgb), std::string("rejects \"") + bad + "\"");
    }
    Check(rgb == before, "a rejected value leaves the output alone");
  }
}

// ---- ExtenderRingGeometry.h: the comma-separated pair ------------------------

void PairingTests() {
  {
    TEST_CASE("theTwoListsPairByPosition");
    const auto marks = PairExtenderMarks("192.0.2.1,2001:db8::1", "3cdd67,dd4f3c");
    CheckEq(2, static_cast<long long>(marks.size()), "two marks");
    CheckEq("192.0.2.1", marks[0].ip, "first ip");
    CheckEq("3cdd67", marks[0].colorHex, "first colour");
    CheckEq("2001:db8::1", marks[1].ip, "second ip");
    CheckEq("dd4f3c", marks[1].colorHex, "second colour");
  }
  {
    TEST_CASE("aDirectRouteHasNoRings");
    Check(PairExtenderMarks("", "").empty(), "empty fields -> no marks");
    Check(PairExtenderMarks(",", ",").empty(), "separators alone -> no marks");
    Check(PairExtenderMarks("  ", "  ").empty(), "whitespace alone -> no marks");
  }
  {
    TEST_CASE("theAddressListDecidesHowManyRings");
    // an SDK that sent no colours (or fewer than it sent addresses) must not
    // lose a ring, and must not gain one from a stray colour either
    auto marks = PairExtenderMarks("192.0.2.1,192.0.2.2", "3cdd67");
    CheckEq(2, static_cast<long long>(marks.size()), "two addresses, one colour");
    CheckEq("", marks[1].colorHex, "the unpaired address has no colour");
    marks = PairExtenderMarks("192.0.2.1", "3cdd67,dd4f3c");
    CheckEq(1, static_cast<long long>(marks.size()), "one address, two colours");
  }
  {
    TEST_CASE("entriesAreTrimmed");
    const auto marks = PairExtenderMarks(" 192.0.2.1 , 2001:db8::1 ", " 3cdd67 , dd4f3c ");
    CheckEq("192.0.2.1", marks[0].ip, "leading/trailing space is not part of an address");
    CheckEq("dd4f3c", marks[1].colorHex, "nor of a colour");
  }
}

// ---- ExtenderRingGeometry.h: K2's geometry ----------------------------------

void RingGeometryTests() {
  {
    TEST_CASE("noExtendersIsTheDotWeAlreadyDraw");
    const auto layout = ExtenderRingsFor(64, {});
    CheckNear(64, layout.dotDiameter, 1e-9, "the dot fills the cell");
    Check(layout.rings.empty(), "no rings");
    Check(!layout.Collapsed() && !layout.scaled, "nothing collapsed, nothing scaled");
    CheckEq(0, layout.totalCount, "no extenders");
  }
  {
    TEST_CASE("k2NumbersHoldAtAWorkableCellSize");
    // 64px: three rings need 24px of the diameter, leaving 40 - well over the
    // floor, so this is K2 verbatim
    for (int n = 1; n <= 3; ++n) {
      const auto layout =
          ExtenderRingsFor(64, Marks(n == 1   ? std::initializer_list<const char*>{"3cdd67"}
                                     : n == 2 ? std::initializer_list<const char*>{"3cdd67",
                                                                                   "dd4f3c"}
                                              : std::initializer_list<const char*>{
                                                    "3cdd67", "dd4f3c", "67dd3c"}));
      const std::string at = " (n=" + std::to_string(n) + ")";
      Check(!layout.scaled, "not scaled" + at);
      CheckEq(n, static_cast<long long>(layout.rings.size()), "one ring per extender" + at);
      // the filled dot shrinks inward by 4px per ring => 8px of diameter
      CheckNear(64 - 8.0 * n, layout.dotDiameter, 1e-9, "dot diameter" + at);
      for (int k = 0; k < n; ++k) {
        const ExtenderRing& ring = layout.rings[static_cast<std::size_t>(k)];
        CheckNear(2.0, ring.stroke, 1e-9, "stroke is 2px" + at);
        // the outermost ring's OUTER EDGE is the cell edge; each ring in is 4px
        CheckNear(64 - 8.0 * k, ring.OuterDiameter(), 1e-9,
                  "ring " + std::to_string(k) + " outer edge" + at);
      }
      // 2px of air between the dot and the first ring in, and between rings
      const ExtenderRing& innermost = layout.rings[static_cast<std::size_t>(n - 1)];
      CheckNear(2.0, (innermost.InnerDiameter() - layout.dotDiameter) / 2, 1e-9,
                "gap between the dot and the innermost ring" + at);
      for (int k = 1; k < n; ++k) {
        const double outer = layout.rings[static_cast<std::size_t>(k - 1)].InnerDiameter();
        const double inner = layout.rings[static_cast<std::size_t>(k)].OuterDiameter();
        CheckNear(2.0, (outer - inner) / 2, 1e-9,
                  "gap between rings " + std::to_string(k - 1) + " and " +
                      std::to_string(k) + at);
      }
    }
  }
  {
    TEST_CASE("theFootprintNeverGrowsIntoANeighbour");
    for (double cell : {10.0, 12.0, 18.285714, 20.0, 32.0, 64.0, 120.0}) {
      for (int n = 1; n <= 6; ++n) {
        std::vector<ExtenderMark> marks;
        for (int i = 0; i < n; ++i) marks.push_back(ExtenderMark{"ip", "3cdd67"});
        const auto layout = ExtenderRingsFor(cell, marks);
        const std::string at =
            " (cell=" + std::to_string(cell) + " n=" + std::to_string(n) + ")";
        CheckNear(cell, layout.rings.front().OuterDiameter(), 1e-9,
                  "the outermost ring sits exactly on the cell edge" + at);
        Check(0 < layout.dotDiameter, "the filled dot is still visible" + at);
        Check(layout.dotDiameter <= cell, "the dot never exceeds the cell" + at);
        Check(layout.rings.back().InnerDiameter() >= layout.dotDiameter - 1e-9,
              "the dot never touches the innermost ring" + at);
      }
    }
  }
  {
    TEST_CASE("theHeroCellScalesRatherThanLosingTheDot");
    // the connect hero's own cell: a 256pt canvas over a 14-wide grid
    const double cell = 256.0 / 14;
    const auto layout = ExtenderRingsFor(cell, Marks({"3cdd67", "dd4f3c", "67dd3c"}));
    Check(layout.scaled, "three rings do not fit at 4px each on an 18px cell");
    CheckNear(cell / 3, layout.dotDiameter, 1e-9, "the dot keeps a third of the cell");
    CheckNear(cell, layout.rings[0].OuterDiameter(), 1e-9, "outer edge still the cell edge");
    Check(0 < layout.rings[0].stroke && layout.rings[0].stroke < 2,
          "the stroke scaled with the gap rather than being dropped");
    // stroke and gap keep their 1:1 ratio through the scale
    const double gap = (layout.rings[2].InnerDiameter() - layout.dotDiameter) / 2;
    CheckNear(layout.rings[0].stroke, gap, 1e-9, "stroke and gap stay equal");
  }
  {
    TEST_CASE("oneRingStillFitsUnscaledOnAHeroCell");
    const double cell = 256.0 / 14;
    const auto layout = ExtenderRingsFor(cell, Marks({"3cdd67"}));
    Check(!layout.scaled, "a single ring costs 4px of an 18px cell, which fits");
    CheckNear(2.0, layout.rings[0].stroke, 1e-9, "K2's 2px stroke");
    CheckNear(cell - 8, layout.dotDiameter, 1e-9, "the dot shrank by 4px of radius");
  }
  {
    TEST_CASE("fourOrMoreCollapseIntoADashedThirdRing");
    const auto three = ExtenderRingsFor(64, Marks({"3cdd67", "dd4f3c", "67dd3c"}));
    Check(!three.Collapsed(), "three extenders are three plain rings");
    for (const auto& ring : three.rings) Check(!ring.dashed, "none of them is dashed");

    for (int n = 4; n <= 9; ++n) {
      std::vector<ExtenderMark> marks;
      for (int i = 0; i < n; ++i) marks.push_back(ExtenderMark{"ip", "3cdd67"});
      const auto layout = ExtenderRingsFor(64, marks);
      const std::string at = " (n=" + std::to_string(n) + ")";
      CheckEq(3, static_cast<long long>(layout.rings.size()), "still three rings" + at);
      CheckEq(n, layout.totalCount, "the total is preserved for the caller" + at);
      Check(layout.Collapsed(), "collapsed" + at);
      Check(!layout.rings[0].dashed && !layout.rings[1].dashed,
            "the outer two stay solid" + at);
      Check(layout.rings[2].dashed, "the THIRD ring is the dashed one" + at);
      CheckNear(64 - 24.0, layout.dotDiameter, 1e-9,
                "the dot shrinks for three rings, not for n" + at);
    }
  }
  {
    TEST_CASE("ringsAreOutermostFirstAndKeepTheirPairing");
    const auto layout = ExtenderRingsFor(64, Marks({"3cdd67", "dd4f3c", "67dd3c"}));
    CheckEq("192.0.2.1", layout.rings[0].ip, "the first extender is the outermost ring");
    CheckEq("3cdd67", layout.rings[0].colorHex, "with its own colour");
    CheckEq("192.0.2.3", layout.rings[2].ip, "the third is the innermost");
    for (const auto& ring : layout.rings) Check(ring.hasColor, "every colour parsed");
    Check(layout.rings[1].color == ExtenderRgb{0xdd, 0x4f, 0x3c}, "ring 1 rgb");
  }
  {
    TEST_CASE("aMissingColourIsReportedNotInvented");
    std::vector<ExtenderMark> marks{ExtenderMark{"192.0.2.1", ""},
                                    ExtenderMark{"192.0.2.2", "nope"}};
    const auto layout = ExtenderRingsFor(64, marks);
    CheckEq(2, static_cast<long long>(layout.rings.size()), "both rings are drawn");
    Check(!layout.rings[0].hasColor && !layout.rings[1].hasColor,
          "neither claims a colour it was not given");
    Check(layout.rings[0].color == ExtenderRgb{}, "and none was invented");
  }
  {
    TEST_CASE("anUnmeasuredCellDrawsNothing");
    for (double cell : {0.0, -1.0}) {
      const auto layout = ExtenderRingsFor(cell, Marks({"3cdd67"}));
      Check(layout.rings.empty(), "no rings before the canvas has a size");
      CheckNear(0, layout.dotDiameter, 1e-9, "and no dot");
    }
  }
}

// ---- ExtenderRingGeometry.h: the Drop fit and the scaled glyph (K9) ---------

void RingFitTests() {
  {
    TEST_CASE("aDropCellKeepsK2sNumbersAndDashesTheLastRingItHolds");
    // 14 units, the provider globe's dot: a third of it is the floor, which
    // leaves room for one 4px ring and not two
    const auto one = ExtenderRingsFor(14, Marks({"3cdd67"}), ExtenderRingFit::Drop);
    CheckEq(1, static_cast<long long>(one.rings.size()), "one extender, one ring");
    Check(!one.scaled && !one.Collapsed() && !one.rings[0].dashed, "K2 verbatim, solid");
    CheckNear(2.0, one.rings[0].stroke, 1e-9, "K2's stroke");
    CheckNear(14, one.rings[0].OuterDiameter(), 1e-9, "the outer edge is the cell edge");
    CheckNear(14 - 8.0, one.dotDiameter, 1e-9, "the dot shrank by one ring");

    for (int n = 2; n <= 5; ++n) {
      std::vector<ExtenderMark> marks = Marks({"3cdd67", "dd4f3c", "67dd3c", "3c67dd", "dd3c67"});
      marks.resize(static_cast<std::size_t>(n));
      const auto layout = ExtenderRingsFor(14, marks, ExtenderRingFit::Drop);
      const std::string at = " (n=" + std::to_string(n) + ")";
      CheckEq(1, static_cast<long long>(layout.rings.size()), "still the one ring" + at);
      Check(layout.rings[0].dashed, "dashed for the extenders without one" + at);
      Check(layout.Collapsed(), "collapsed" + at);
      CheckEq(n, layout.totalCount, "the total is preserved" + at);
      CheckEq("3cdd67", layout.rings[0].colorHex, "in the first extender's colour" + at);
      Check(!layout.scaled, "nothing thinned" + at);
      CheckNear(2.0, layout.rings[0].stroke, 1e-9, "K2's stroke" + at);
      CheckNear(14 - 8.0, layout.dotDiameter, 1e-9, "the dot shrank for one ring" + at);
    }
  }
  {
    TEST_CASE("aDropCellThatHoldsEveryRingIsTheScaleOne");
    // where K2's numbers fit, the two fits are the same layout, the dashed
    // third ring included
    for (int n = 1; n <= 5; ++n) {
      std::vector<ExtenderMark> marks = Marks({"3cdd67", "dd4f3c", "67dd3c", "3c67dd", "dd3c67"});
      marks.resize(static_cast<std::size_t>(n));
      const auto drop = ExtenderRingsFor(64, marks, ExtenderRingFit::Drop);
      const auto scale = ExtenderRingsFor(64, marks);
      const std::string at = " (n=" + std::to_string(n) + ")";
      CheckEq(static_cast<long long>(scale.rings.size()), static_cast<long long>(drop.rings.size()),
              "as many rings" + at);
      CheckNear(scale.dotDiameter, drop.dotDiameter, 1e-9, "the same dot" + at);
      for (std::size_t k = 0; k < drop.rings.size() && k < scale.rings.size(); ++k) {
        CheckNear(scale.rings[k].diameter, drop.rings[k].diameter, 1e-9, "the same ring" + at);
        Check(scale.rings[k].dashed == drop.rings[k].dashed, "the same dash" + at);
      }
    }
  }
  {
    TEST_CASE("aDropCellDropsWhereAScaleCellThins");
    // the connect hero's cell: Scale keeps three thinned rings, Drop one full one
    const double cell = 256.0 / 14;
    const auto marks = Marks({"3cdd67", "dd4f3c", "67dd3c"});
    const auto scale = ExtenderRingsFor(cell, marks);
    const auto drop = ExtenderRingsFor(cell, marks, ExtenderRingFit::Drop);
    CheckEq(3, static_cast<long long>(scale.rings.size()), "Scale keeps the count");
    Check(scale.scaled, "by thinning");
    CheckEq(1, static_cast<long long>(drop.rings.size()), "Drop keeps the stroke");
    Check(drop.rings[0].dashed && !drop.scaled, "and dashes the ring it keeps");
    Check(cell / 3 <= drop.dotDiameter, "the dot keeps its floor either way");
  }
  {
    TEST_CASE("aDropCellTooSmallForOneRingIsThePlainDot");
    const auto layout = ExtenderRingsFor(10, Marks({"3cdd67", "dd4f3c"}), ExtenderRingFit::Drop);
    Check(layout.rings.empty(), "no ring fits a 10px cell at K2's numbers");
    CheckNear(10, layout.dotDiameter, 1e-9, "the dot fills the cell");
    Check(!layout.Collapsed(), "nothing to dash");
    CheckEq(2, layout.totalCount, "the extenders are still counted");
  }
  {
    TEST_CASE("theGlyphScalesAsOne");
    const auto layout = ExtenderRingsFor(64, Marks({"3cdd67", "dd4f3c", "67dd3c", "3c67dd"}));
    const auto half = layout.ScaledBy(0.5);
    CheckNear(layout.dotDiameter / 2, half.dotDiameter, 1e-9, "the dot");
    CheckEq(static_cast<long long>(layout.rings.size()), static_cast<long long>(half.rings.size()),
            "every ring");
    for (std::size_t k = 0; k < layout.rings.size() && k < half.rings.size(); ++k) {
      CheckNear(layout.rings[k].diameter / 2, half.rings[k].diameter, 1e-9, "the ring");
      CheckNear(layout.rings[k].stroke / 2, half.rings[k].stroke, 1e-9, "the stroke");
      Check(layout.rings[k].dashed == half.rings[k].dashed, "the dash");
      CheckEq(layout.rings[k].colorHex, half.rings[k].colorHex, "the colour");
    }
    CheckNear(32, half.rings[0].OuterDiameter(), 1e-9, "the outer edge is the scaled cell's");
    CheckEq(layout.totalCount, half.totalCount, "the count");
    CheckNear(0, layout.ScaledBy(-1).dotDiameter, 1e-9, "a negative scale draws nothing");
  }
}

// ---- ExtenderPresentation.h: the gossip dot ---------------------------------

void GossipStateTests() {
  {
    TEST_CASE("theThreeStatesMapToTheThreeColours");
    Check(ExtenderGossipToneFor("connected") == GossipTone::Green, "connected -> green");
    Check(ExtenderGossipToneFor("connecting") == GossipTone::Yellow,
          "connecting -> yellow");
    Check(ExtenderGossipToneFor("disconnected") == GossipTone::Red,
          "disconnected -> red");
    CheckEq("connected", ExtenderGossipStateKey("connected"), "connected key");
    // the store's yellow word is the gossip network's own, not the shared one
    CheckEq("gossip_connecting", ExtenderGossipStateKey("connecting"), "connecting key");
    CheckEq("disconnected", ExtenderGossipStateKey("disconnected"), "disconnected key");
  }
  {
    TEST_CASE("anUnknownStateIsNeverReassuring");
    for (const char* unknown : {"", "CONNECTED", "up", "degraded", "member"}) {
      Check(ExtenderGossipToneFor(unknown) == GossipTone::Red,
            std::string("\"") + unknown + "\" is red, not green");
      CheckEq("disconnected", ExtenderGossipStateKey(unknown),
              std::string("\"") + unknown + "\" says disconnected");
    }
  }
}

// ---- ExtenderPresentation.h: the panel's model ------------------------------

void PanelModelTests() {
  ExtenderStatusView status;
  status.gossipState = "connected";
  status.activeCount = 2;
  status.reserveCount = 7;
  status.eventCountLastMinute = 3;
  status.extenders = {
      ExtenderInfoView{"192.0.2.1", "3cdd67", 2},
      ExtenderInfoView{"198.51.100.7", "dd4f3c", 0},
      ExtenderInfoView{"2001:db8::1", "67dd3c", 1},
  };
  {
    TEST_CASE("onlyInUseExtendersGetARing");
    const auto model = ExtenderPanelModelFor(status);
    CheckEq(2, static_cast<long long>(model.activeMarks.size()), "two active extenders");
    CheckEq("192.0.2.1", model.activeMarks[0].ip, "in status order");
    CheckEq("2001:db8::1", model.activeMarks[1].ip, "the idle one is skipped");
    CheckEq("67dd3c", model.activeMarks[1].colorHex, "each ring keeps its colour");
  }
  {
    TEST_CASE("theCountIsActiveOfReserve");
    const auto model = ExtenderPanelModelFor(status);
    CheckEq(2, model.activeCount, "N = carrying a live connection now");
    CheckEq(7, model.reserveCount, "M = every usable entry");
    CheckEq(3, model.eventCountLastMinute, "the trailing-60s event count");
    Check(model.tone == GossipTone::Green, "green");
    CheckEq("connected", model.stateKey, "connected");
  }
  {
    TEST_CASE("anEmptyStatusIsARedDotAndZeroOfZero");
    const auto model = ExtenderPanelModelFor(ExtenderStatusView{});
    Check(model.tone == GossipTone::Red, "red before anything is known");
    CheckEq("disconnected", model.stateKey, "disconnected");
    CheckEq(0, model.activeCount, "0");
    CheckEq(0, model.reserveCount, "of 0");
    Check(model.activeMarks.empty(), "no rings");
  }
  {
    TEST_CASE("negativeCountsAreNeverRendered");
    ExtenderStatusView broken;
    broken.activeCount = -1;
    broken.reserveCount = -4;
    broken.eventCountLastMinute = -9;
    const auto model = ExtenderPanelModelFor(broken);
    CheckEq(0, model.activeCount, "clamped");
    CheckEq(0, model.reserveCount, "clamped");
    CheckEq(0, model.eventCountLastMinute, "clamped");
  }
  {
    TEST_CASE("anExtenderWithNoAddressIsNotARing");
    ExtenderStatusView anonymous;
    anonymous.extenders = {ExtenderInfoView{"", "3cdd67", 4}};
    Check(ExtenderPanelModelFor(anonymous).activeMarks.empty(),
          "there is nothing to identify, so there is nothing to draw");
  }
  {
    TEST_CASE("theStatusViewComparesByValueSoTheFeedCanDedup");
    // the SDK fires once a second whether or not anything moved, and SdkHost
    // drops the repeat on exactly this comparison
    ExtenderStatusView same = status;
    Check(same == status, "an identical push is not a change");
    same.eventCountLastMinute = 4;
    Check(same != status, "a new event rate is");
    ExtenderStatusView other = status;
    other.extenders.pop_back();
    Check(other != status, "so is an extender leaving the directory");
    Check(ExtenderStatusView{} == ExtenderStatusView{}, "two empty statuses agree");
  }
  {
    TEST_CASE("theModelComparesByValueSoAnUnchangedPushCostsNothing");
    const auto a = ExtenderPanelModelFor(status);
    const auto b = ExtenderPanelModelFor(status);
    Check(a == b, "same status, equal model");
    ExtenderStatusView moved = status;
    moved.extenders[1].inUse = 1;  // an extender started carrying traffic
    Check(a != ExtenderPanelModelFor(moved), "a new ring is a change");
    ExtenderStatusView recoloured = status;
    recoloured.extenders[0].colorHex = "ffffff";
    Check(a != ExtenderPanelModelFor(recoloured), "so is a colour");
  }
}

// ---- ExtenderPresentation.h: the settings form ------------------------------

void SettingsFormTests() {
  {
    TEST_CASE("aDefaultValueIsAPlaceholderNotAValue");
    ExtenderSettingsView settings;
    settings.dnsName = "extender.bringyour.com";
    settings.dnsNameDefault = true;
    settings.gossipUrl = "wss://gossip.bringyour.com";
    settings.gossipUrlDefault = true;
    const auto form = ExtenderSettingsFormFor(settings);
    CheckEq("", form.dnsNameText, "the box is empty, because empty MEANS the default");
    CheckEq("extender.bringyour.com", form.dnsNameDefaultValue,
            "and the default is named in the placeholder");
    CheckEq("", form.gossipUrlText, "same for the gossip url");
    CheckEq("wss://gossip.bringyour.com", form.gossipUrlDefaultValue, "same placeholder");
  }
  {
    TEST_CASE("aConfiguredValueIsShownInTheBox");
    ExtenderSettingsView settings;
    settings.dnsName = "ext.example.com";
    settings.dnsNameDefault = false;
    settings.gossipUrl = "wss://g.example.com";
    settings.gossipUrlDefault = false;
    const auto form = ExtenderSettingsFormFor(settings);
    CheckEq("ext.example.com", form.dnsNameText, "the configured name");
    CheckEq("", form.dnsNameDefaultValue,
            "no placeholder: the box is not empty, so one would never be seen, and "
            "the derived default is not in this reading");
    CheckEq("wss://g.example.com", form.gossipUrlText, "the configured url");
  }
  {
    TEST_CASE("hostsAreOnePerLine");
    ExtenderSettingsView settings;
    settings.hosts = {"ext1.example.com", "203.0.113.9", "  ", "2001:db8::5"};
    const auto form = ExtenderSettingsFormFor(settings);
    CheckEq("ext1.example.com\n203.0.113.9\n2001:db8::5", form.hostsText,
            "in order, blanks dropped");
  }
  {
    TEST_CASE("theHostsBoxRoundTrips");
    ExtenderSettingsView settings;
    settings.hosts = {"ext1.example.com", "203.0.113.9", "2001:db8::5"};
    const auto form = ExtenderSettingsFormFor(settings);
    Check(ParseExtenderHostLines(form.hostsText) == settings.hosts,
          "what the form shows is what SetSettings gets back");
  }
  {
    TEST_CASE("aPastedListIsAccepted");
    // a user with a comma-separated list in the clipboard should not have to
    // reformat it by hand
    const auto hosts = ParseExtenderHostLines("a.example.com, b.example.com\r\nc.example.com,");
    CheckEq(3, static_cast<long long>(hosts.size()), "three hosts");
    CheckEq("a.example.com", hosts[0], "first");
    CheckEq("b.example.com", hosts[1], "second, trimmed");
    CheckEq("c.example.com", hosts[2], "third, past the CRLF; the trailing comma is not a host");
  }
  {
    TEST_CASE("anEmptyBoxIsAnEmptyList");
    Check(ParseExtenderHostLines("").empty(), "nothing typed");
    Check(ParseExtenderHostLines("\n\n  \r\n").empty(), "nothing but whitespace");
  }
}

// ---- ExtenderPresentation.h: share ------------------------------------------

void ShareTests() {
  {
    TEST_CASE("theShapeCheckIsThePayloadPrefix");
    Check(LooksLikeExtenderShare("ur-ext:1:AAAA"), "a payload");
    Check(LooksLikeExtenderShare("  ur-ext:1:AAAA \n"), "pasted with its whitespace");
    Check(!LooksLikeExtenderShare("ur-ext:1:"), "the prefix alone carries nothing");
    Check(!LooksLikeExtenderShare(""), "empty");
    Check(!LooksLikeExtenderShare("https://ur.io/c/abc"), "a connect link is not a share");
    Check(!LooksLikeExtenderShare("UR-EXT:1:AAAA"), "the scheme is lower case");
  }
  {
    TEST_CASE("pastedTextIsTrimmed");
    CheckEq("ur-ext:1:AAAA", TrimShareText("\n  ur-ext:1:AAAA\t\r\n "), "trimmed");
    CheckEq("", TrimShareText("   \n"), "whitespace alone is nothing");
  }
}

void QrLayoutTests() {
  {
    TEST_CASE("theGlyphIsCentredOnWholeModules");
    // version 5 (37 modules) drawn into 296px
    const auto layout = ExtenderQrLayoutFor(37, 296);
    CheckEq(37, layout.moduleCount, "37 modules");
    CheckNear(8, layout.moduleSize, 1e-9, "8px per module");
    CheckNear(296, layout.side, 1e-9, "the code fills the box exactly");
    // the cleared run is centred, so the margins on both sides are equal
    CheckEq(layout.clearFrom, layout.moduleCount - layout.clearTo,
            "the cleared run is centred");
    Check(layout.clearFrom < layout.clearTo, "something is cleared");
    CheckNear(layout.glyphLeft, layout.glyphTop, 1e-9, "square, so both offsets agree");
    CheckNear(layout.side - layout.glyphLeft - layout.glyphSide, layout.glyphLeft, 1e-9,
              "the glyph is centred in the code");
  }
  {
    TEST_CASE("theOutlineIsClearedToo");
    const auto layout = ExtenderQrLayoutFor(37, 296);
    CheckNear(4.0, layout.outlineThickness, 1e-9, "K7's 4px outline");
    const double cleared = (layout.clearTo - layout.clearFrom) * layout.moduleSize;
    Check(layout.glyphSide + 2 * layout.outlineThickness <= cleared + 1e-9,
          "the glyph AND its outline fit inside the blanked modules");
  }
  {
    TEST_CASE("theOcclusionStaysInsideLevelHsBudget");
    // level H recovers ~30% of the codewords; a centred patch far below that
    // is what makes the glyph safe at all
    for (int modules : {21, 25, 33, 37, 45, 57, 77}) {
      const auto layout = ExtenderQrLayoutFor(modules, 320);
      const double cleared = static_cast<double>(layout.clearTo - layout.clearFrom);
      const double areaFraction = (cleared * cleared) / (modules * modules);
      Check(areaFraction < 0.15,
            "occlusion at " + std::to_string(modules) + " modules is " +
                std::to_string(areaFraction) + ", well inside level H");
      Check(0 < cleared, "and something is actually cleared");
    }
  }
  {
    TEST_CASE("nothingToDrawIsAZeroLayout");
    for (const auto& layout :
         {ExtenderQrLayoutFor(0, 296), ExtenderQrLayoutFor(37, 0), ExtenderQrLayoutFor(37, -5)}) {
      CheckEq(0, layout.moduleCount, "no modules");
      CheckNear(0, layout.side, 1e-9, "no side");
      CheckEq(0, layout.clearTo - layout.clearFrom, "nothing cleared");
    }
  }
}

void QrRunTests() {
  {
    TEST_CASE("runsMergeAcrossARow");
    //  . # # .
    //  # . . #
    //  . . . .
    //  # # # #
    const int n = 4;
    const std::vector<bool> dark = {false, true,  true,  false, true,  false, false, true,
                                    false, false, false, false, true,  true,  true,  true};
    const auto runs = ExtenderQrRunsFor(n, dark, 0, 0);
    CheckEq(4, static_cast<long long>(runs.size()), "four runs, not eight modules");
    Check(runs[0] == ExtenderQrRun{1, 0, 2}, "row 0 merges into one run of 2");
    Check(runs[1] == ExtenderQrRun{0, 1, 1}, "row 1 first run");
    Check(runs[2] == ExtenderQrRun{3, 1, 1}, "row 1 second run, past the gap");
    Check(runs[3] == ExtenderQrRun{0, 3, 4}, "a full row is one run");
  }
  {
    TEST_CASE("theGlyphPatchIsBlankedNotOverdrawn");
    const int n = 5;
    const std::vector<bool> dark(static_cast<std::size_t>(n * n), true);
    const auto runs = ExtenderQrRunsFor(n, dark, 2, 3);  // the centre module
    // rows 0, 1, 3, 4 are one full run each; row 2 is split either side
    CheckEq(6, static_cast<long long>(runs.size()), "six runs");
    long long modules = 0;
    for (const auto& run : runs) modules += run.length;
    CheckEq(n * n - 1, modules, "exactly the cleared module is missing");
    for (const auto& run : runs) {
      Check(!(run.y == 2 && run.x <= 2 && 2 < run.x + run.length),
            "nothing is drawn across the cleared column on the cleared row");
    }
  }
  {
    TEST_CASE("aDegenerateInputDrawsNothing");
    Check(ExtenderQrRunsFor(0, {}, 0, 0).empty(), "no modules");
    Check(ExtenderQrRunsFor(4, {true, true}, 0, 0).empty(),
          "a matrix shorter than the code draws nothing rather than reading past it");
    Check(ExtenderQrRunsFor(3, std::vector<bool>(9, false), 0, 0).empty(),
          "an all-light code has no runs");
  }
}

// The vendored encoder itself: this is the only place on a non-Windows host
// where third_party/qrcodegen is compiled and run, so it is what catches a
// re-vendor that does not build or an API that moved under us.
void QrEncoderTests() {
  {
    TEST_CASE("theShareTextEncodesAtLevelH");
    const std::string payload =
        "ur-ext:1:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4vMDEyMzQ1Njc4OTo7PD0-P0A";
    const qrcodegen::QrCode code =
        qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::HIGH);
    Check(0 < code.getSize(), "the encoder produced a code");
    Check(code.getSize() % 2 == 1, "every QR version has an odd module count");
    Check(code.getErrorCorrectionLevel() == qrcodegen::QrCode::Ecc::HIGH,
          "at the level K7 asks for");
    // the three finder patterns are where a decoder expects them
    Check(code.getModule(0, 0) && code.getModule(6, 0) && code.getModule(0, 6),
          "top-left finder");
    Check(code.getModule(code.getSize() - 1, 0), "top-right finder");
    Check(code.getModule(0, code.getSize() - 1), "bottom-left finder");
    Check(!code.getModule(-1, 0) && !code.getModule(code.getSize(), 0),
          "out of range reads as light, so the quiet zone needs no special case");

    // and the layout the sheet draws it with lands on that size
    const auto layout = ExtenderQrLayoutFor(code.getSize(), 296);
    CheckEq(code.getSize(), layout.moduleCount, "the layout follows the encoder");
    Check(0 < layout.moduleSize, "with a drawable module size");

    // and the runs the sheet actually draws come out of that same pairing
    std::vector<bool> dark(static_cast<std::size_t>(code.getSize() * code.getSize()), false);
    for (int y = 0; y < code.getSize(); ++y) {
      for (int x = 0; x < code.getSize(); ++x) {
        dark[static_cast<std::size_t>(y * code.getSize() + x)] = code.getModule(x, y);
      }
    }
    const auto runs =
        ExtenderQrRunsFor(code.getSize(), dark, layout.clearFrom, layout.clearTo);
    Check(!runs.empty(), "the code has dark runs to draw");
    long long drawn = 0;
    for (const auto& run : runs) {
      drawn += run.length;
      Check(run.x + run.length <= code.getSize(), "no run runs off the right edge");
      const bool inClearedRows = layout.clearFrom <= run.y && run.y < layout.clearTo;
      if (inClearedRows) {
        Check(run.x + run.length <= layout.clearFrom || layout.clearTo <= run.x,
              "no run crosses the glyph patch");
      }
    }
    long long total = 0;
    for (bool on : dark) total += on ? 1 : 0;
    Check(drawn < total, "the glyph patch really did remove modules");
    Check(static_cast<long long>(runs.size()) < total,
          "and merging really did cut the shape count");
  }
}

// ---- ExtenderPresentation.h: import -----------------------------------------

ExtenderShareDecodeView GoodDecode() {
  ExtenderShareDecodeView decoded;
  decoded.ok = true;
  decoded.networkHost = "bringyour.com";
  decoded.count = 12;
  return decoded;
}

void ImportTests() {
  {
    TEST_CASE("aPlainPayloadImports");
    const auto decision = DecideExtenderImport(GoodDecode(), false);
    Check(decision.canImport, "importable");
    Check(!decision.showSettingsToggle, "no settings block, no toggle");
    Check(!decision.showForeignHost, "our own operator");
    Check(!decision.needsConfirm, "nothing to confirm");
    CheckEq("", decision.messageKey, "and nothing to say");
  }
  {
    TEST_CASE("aSettingsBlockOffersTheToggleWithoutForcingIt");
    auto decoded = GoodDecode();
    decoded.hasSettings = true;
    decoded.settingsHost = "bringyour.com";
    const auto off = DecideExtenderImport(decoded, false);
    Check(off.showSettingsToggle, "the toggle is offered");
    Check(off.canImport && !off.needsConfirm,
          "importing the addresses alone takes no confirmation");
    const auto on = DecideExtenderImport(decoded, true);
    Check(on.canImport, "still importable");
    Check(on.needsConfirm, "replacing the operator settings does take one");
    CheckEq("bringyour.com", on.confirmArg,
            "and the confirmation names the operator host it would switch to");
  }
  {
    TEST_CASE("aForeignCodeIsRefusedUntilItsSettingsAreTaken");
    auto decoded = GoodDecode();
    decoded.foreignHost = true;
    decoded.networkHost = "other.example";
    // no settings block at all: nothing can rescue it
    const auto bare = DecideExtenderImport(decoded, true);
    Check(!bare.canImport, "a foreign code with no settings can never be imported");
    Check(bare.showForeignHost, "and says so");
    CheckEq("import_extenders_foreign_host", bare.messageKey, "with the store's line");
    CheckEq("other.example", bare.messageArg, "naming the other network");

    decoded.hasSettings = true;
    decoded.settingsHost = "other.example";
    const auto refused = DecideExtenderImport(decoded, false);
    Check(!refused.canImport, "refused while the toggle is off");
    Check(refused.showSettingsToggle, "but the way through is offered");
    const auto accepted = DecideExtenderImport(decoded, true);
    Check(accepted.canImport, "taking the settings is what lets it through");
    Check(accepted.needsConfirm, "and it is confirmed first");
    Check(accepted.showForeignHost, "the foreign host stays on screen throughout");
    CheckEq("other.example", accepted.confirmArg, "the confirmation names it");
  }
  {
    TEST_CASE("aFailedDecodeReportsTheSdksOwnKey");
    ExtenderShareDecodeView bad;
    bad.ok = false;
    bad.error = "import_extenders_invalid";
    const auto decision = DecideExtenderImport(bad, false);
    Check(!decision.canImport, "nothing to import");
    CheckEq("import_extenders_invalid", decision.messageKey, "the SDK's key, unchanged");
    Check(!decision.showSettingsToggle, "and no toggle for a payload that did not parse");
  }
  {
    TEST_CASE("aFailedDecodeWithNoReasonStillSaysSomething");
    ExtenderShareDecodeView bad;
    bad.ok = false;
    const auto decision = DecideExtenderImport(bad, false);
    CheckEq("import_extenders_invalid", decision.messageKey,
            "silence is not a state a user can act on");
  }
  {
    TEST_CASE("aForeignHostRejectedAtDecodeTimeStillNamesTheHost");
    ExtenderShareDecodeView bad;
    bad.ok = false;
    bad.error = "import_extenders_foreign_host";
    bad.networkHost = "other.example";
    const auto decision = DecideExtenderImport(bad, false);
    Check(decision.showForeignHost, "shown as a foreign host, not as a parse failure");
    CheckEq("other.example", decision.messageArg, "named");
  }
  {
    TEST_CASE("theOutcomeIsAPluralOnSuccessAndAKeyOnFailure");
    ExtenderImportResultView ok;
    ok.ok = true;
    ok.importedCount = 7;
    const auto good = ExtenderImportOutcomeFor(ok);
    Check(good.ok && good.isPlural, "a plural line");
    CheckEq("import_extenders_imported", good.messageKey, "the store's key");
    CheckEq(7, good.count, "with the count the SDK reported");

    ExtenderImportResultView failed;
    failed.error = "import_extenders_foreign_host";
    const auto bad = ExtenderImportOutcomeFor(failed);
    Check(!bad.ok && !bad.isPlural, "not a plural");
    CheckEq("import_extenders_foreign_host", bad.messageKey, "the SDK's key");

    ExtenderImportResultView zero;
    zero.ok = true;
    const auto none = ExtenderImportOutcomeFor(zero);
    Check(none.ok, "importing nothing is still a success");
    CheckEq(0, none.count, "\"Imported 0 extenders\" is a real answer");
  }
}

// ---- ExtenderPresentation.h: the provider extender row (N7) -----------------

// U+00B7, the separator N7 draws between the active line and the other
// family's failure, spelled as its UTF-8 bytes.
constexpr const char* kMiddleDot = "\xC2\xB7";

// The en store the app ships (Strings/en/Resources.resw), read from the tree
// this file is built in: the header's command runs from app/tools. The row's
// English is composed from it rather than from a copy, so a store change the
// row cannot take (a dropped "{}", a renamed key) fails here.
constexpr const char* kEnglishStorePath = "../src/App/Strings/en/Resources.resw";

std::string XmlUnescape(std::string text) {
  // &amp; last, so an escaped entity comes out as the entity rather than its
  // character
  static const std::pair<const char*, const char*> kEntities[] = {
      {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
  for (const auto& entity : kEntities) {
    std::size_t at = 0;
    while ((at = text.find(entity.first, at)) != std::string::npos) {
      text.replace(at, std::strlen(entity.first), entity.second);
      at += std::strlen(entity.second);
    }
  }
  return text;
}

// Every <data name="..."><value>...</value></data> entry of the store, parsed
// once; empty when the file cannot be opened.
const std::map<std::string, std::string>& EnglishStore() {
  static const std::map<std::string, std::string> store = [] {
    std::map<std::string, std::string> entries;
    std::ifstream file(kEnglishStorePath, std::ios::binary);
    if (!file) return entries;
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    static const std::string kDataOpen = "<data name=\"";
    static const std::string kValueOpen = "<value>";
    std::size_t at = 0;
    while ((at = text.find(kDataOpen, at)) != std::string::npos) {
      const std::size_t nameStart = at + kDataOpen.size();
      const std::size_t nameEnd = text.find('"', nameStart);
      const std::size_t dataEnd = text.find("</data>", nameStart);
      if (nameEnd == std::string::npos || dataEnd == std::string::npos) break;
      const std::size_t valueOpen = text.find(kValueOpen, nameEnd);
      const std::size_t valueEnd =
          valueOpen == std::string::npos ? std::string::npos : text.find("</value>", valueOpen);
      if (valueEnd != std::string::npos && valueEnd < dataEnd) {
        const std::size_t valueStart = valueOpen + kValueOpen.size();
        entries[text.substr(nameStart, nameEnd - nameStart)] =
            XmlUnescape(text.substr(valueStart, valueEnd - valueStart));
      }
      at = dataEnd;
    }
    return entries;
  }();
  return store;
}

// The store's text for a key. A key missing from it answers itself, which is
// what Localized() does in the app, so a key the model invented shows up in a
// failure as its own id.
std::string EnglishFor(const std::string& key) {
  const auto& store = EnglishStore();
  const auto found = store.find(key);
  return found == store.end() ? key : found->second;
}

// Format() with one argument: the store's text with its "{}" filled.
std::string FilledFor(const std::string& key, const std::string& argument) {
  std::string text = EnglishFor(key);
  const std::size_t at = text.find("{}");
  if (at != std::string::npos) text.replace(at, 2, argument);
  return text;
}

// The row's line as the app composes it, in English.
std::string RowText(const ExtenderProvideRowModel& model) {
  return ComposeExtenderProvideText<std::string>(
      model, std::string(" ") + kMiddleDot + " ",
      [](const std::string& key) { return EnglishFor(key); },
      [](const std::string& key, const std::string& argument) {
        return FilledFor(key, argument);
      },
      [](const std::string& utf8) { return utf8; });
}

std::string Dotted(const std::string& a, const std::string& b) {
  return a + " " + kMiddleDot + " " + b;
}

ExtenderProvideStatusView ProvideStatus(const char* state, const char* errorCase = "",
                                        const char* reason = "") {
  ExtenderProvideStatusView view;
  view.supported = true;
  view.state = state;
  view.errorCase = errorCase;
  view.reason = reason;
  view.provideExtender = true;
  return view;
}

const char* ToneName(ExtenderProvideTone tone) {
  switch (tone) {
    case ExtenderProvideTone::Grey: return "grey";
    case ExtenderProvideTone::Green: return "green";
    case ExtenderProvideTone::Yellow: return "yellow";
    case ExtenderProvideTone::Red: return "red";
  }
  return "unknown";
}

void CheckTone(ExtenderProvideTone expected, ExtenderProvideTone actual,
               const std::string& what) {
  CheckEq(ToneName(expected), ToneName(actual), what);
}

void ProvideRowTests() {
  {
    TEST_CASE("theStoreHasEveryKeyTheRowNames");
    Check(!EnglishStore().empty(),
          std::string("the en store opens and parses from app/tools: ") + kEnglishStorePath);
    const char* const kTakesArgument[] = {"extender_active", "extender_start_failed",
                                          "extender_tcp_unavailable", "extender_listen_failed",
                                          "extender_activation_refused",
                                          "extender_activation_unreachable",
                                          "extender_activation_failed"};
    const char* const kWholeText[] = {"off", "extender_not_providing", "extender_setting_up",
                                      "ipv4", "ipv6", "ipv4_and_ipv6", "extender_revoked"};
    for (const char* key : kTakesArgument) {
      const auto found = EnglishStore().find(key);
      Check(found != EnglishStore().end(), std::string("the store carries ") + key);
      if (found == EnglishStore().end()) continue;
      const std::size_t first = found->second.find("{}");
      Check(first != std::string::npos && found->second.find("{}", first + 2) == std::string::npos,
            std::string(key) + " holds exactly one {}");
    }
    for (const char* key : kWholeText) {
      const auto found = EnglishStore().find(key);
      Check(found != EnglishStore().end(), std::string("the store carries ") + key);
      if (found == EnglishStore().end()) continue;
      Check(found->second.find('{') == std::string::npos,
            std::string(key) + " is whole text, with no placeholder");
    }
  }
  {
    TEST_CASE("unsupportedIsHiddenInEveryState");
    Check(!ExtenderProvideRowModelFor(ExtenderProvideStatusView{}).visible,
          "no session is hidden");
    for (const char* state :
         {kExtenderProvideStateOff, kExtenderProvideStateNotProviding,
          kExtenderProvideStateSettingUp, kExtenderProvideStateActive,
          kExtenderProvideStateError}) {
      ExtenderProvideStatusView view = ProvideStatus(state);
      view.supported = false;
      Check(!ExtenderProvideRowModelFor(view).visible,
            std::string("unsupported ") + state + " is hidden, never drawn disabled");
      view.supported = true;
      Check(ExtenderProvideRowModelFor(view).visible,
            std::string("supported ") + state + " is shown");
    }
  }
  {
    TEST_CASE("theProviderOnlyDeviceShowsTheSwitchItsServiceWrites");
    // While disconnected the provider is the service's provider-only device.
    // The Earnings row, which has no switch, shows its status. The Connect
    // page's row is the switch: a session's device takes its write over the
    // device rpc, and with no session the service does (set_provide_extender)
    // when its answer said so. Over a service that did not, the switch stays
    // hidden (N1: never a dead switch).
    for (const char* state :
         {kExtenderProvideStateOff, kExtenderProvideStateNotProviding,
          kExtenderProvideStateSettingUp, kExtenderProvideStateActive,
          kExtenderProvideStateError}) {
      const ExtenderProvideStatusView session = ProvideStatus(state);
      const auto sessionModel = ExtenderProvideRowModelFor(session);
      Check(sessionModel.visible && sessionModel.switchVisible,
            std::string("a session's ") + state + " shows both rows");
      ExtenderProvideStatusView providerOnly = session;
      providerOnly.providerOnly = true;
      const auto older = ExtenderProvideRowModelFor(providerOnly);
      Check(older.visible, std::string("the provider-only device's ") + state +
                               " shows the Earnings row");
      Check(!older.switchVisible, std::string("an older service's provider-only ") + state +
                                      " keeps the switch's row hidden");
      providerOnly.serviceWritable = true;
      const auto model = ExtenderProvideRowModelFor(providerOnly);
      Check(model.visible && model.switchVisible,
            std::string("a provider-only ") + state + " the service writes shows both rows");
      Check(model.on == providerOnly.provideExtender,
            std::string("and its switch is that device's setting, ") + state);
      CheckEq(RowText(sessionModel), RowText(model),
              std::string("and reads as a session's ") + state + " does");
      CheckTone(sessionModel.tone, model.tone, std::string("in the same colour, ") + state);
      providerOnly.supported = false;
      const auto hidden = ExtenderProvideRowModelFor(providerOnly);
      Check(!hidden.visible && !hidden.switchVisible,
            std::string("an unsupported provider-only ") + state + " hides both rows");
    }
    Check(!ExtenderProvideRowModelFor(ExtenderProvideStatusView{}).switchVisible,
          "no session and no provider-only device hides the switch's row");
    ExtenderProvideStatusView writerAlone;
    writerAlone.serviceWritable = true;
    Check(!ExtenderProvideRowModelFor(writerAlone).switchVisible,
          "a writer with no status to show shows no switch");
  }
  {
    TEST_CASE("offAndNotProvidingAreGrey");
    const auto off = ExtenderProvideRowModelFor(ProvideStatus(kExtenderProvideStateOff));
    CheckTone(ExtenderProvideTone::Grey, off.tone, "off");
    CheckEq("off", off.textKey, "off reuses the shared off key");
    CheckEq("Off", RowText(off), "off text");
    const auto notProviding =
        ExtenderProvideRowModelFor(ProvideStatus(kExtenderProvideStateNotProviding));
    CheckTone(ExtenderProvideTone::Grey, notProviding.tone, "not providing");
    CheckEq("extender_not_providing", notProviding.textKey, "not providing key");
    CheckEq("Not providing", RowText(notProviding), "not providing text");
  }
  {
    TEST_CASE("settingUpIsYellow");
    const auto model = ExtenderProvideRowModelFor(ProvideStatus(kExtenderProvideStateSettingUp));
    CheckTone(ExtenderProvideTone::Yellow, model.tone, "setting up");
    CheckEq("extender_setting_up", model.textKey, "setting up key");
    CheckEq("Setting up", RowText(model), "setting up text");
  }
  {
    TEST_CASE("activeIsGreenWithItsFamilies");
    ExtenderProvideStatusView both = ProvideStatus(kExtenderProvideStateActive);
    both.activatedV4 = true;
    both.activatedV6 = true;
    const auto model = ExtenderProvideRowModelFor(both);
    CheckTone(ExtenderProvideTone::Green, model.tone, "active");
    CheckEq("extender_active", model.textKey, "active key");
    CheckEq("ipv4_and_ipv6", model.argument, "both families");
    Check(model.argumentKind == ExtenderProvideArgument::Key,
          "the families are a store key, localized before they are filled in");
    Check(model.suffixKey.empty(), "nothing follows when neither family failed");
    CheckEq(Dotted("Active", "IPv4 and IPv6"), RowText(model), "both families text");

    ExtenderProvideStatusView v4 = ProvideStatus(kExtenderProvideStateActive);
    v4.activatedV4 = true;
    CheckEq("ipv4", ExtenderProvideRowModelFor(v4).argument, "IPv4 alone");
    CheckEq(Dotted("Active", "IPv4"), RowText(ExtenderProvideRowModelFor(v4)), "IPv4 text");

    ExtenderProvideStatusView v6 = ProvideStatus(kExtenderProvideStateActive);
    v6.activatedV6 = true;
    CheckEq("ipv6", ExtenderProvideRowModelFor(v6).argument, "IPv6 alone");
    CheckEq(Dotted("Active", "IPv6"), RowText(ExtenderProvideRowModelFor(v6)), "IPv6 text");
  }
  {
    TEST_CASE("activeCarriesTheOtherFamilysRefusalOrFailure");
    ExtenderProvideStatusView refused =
        ProvideStatus(kExtenderProvideStateActive, "", "the operator refused the activation");
    refused.activatedV4 = true;
    refused.refused = true;
    const auto refusedModel = ExtenderProvideRowModelFor(refused);
    CheckTone(ExtenderProvideTone::Green, refusedModel.tone, "still green: a family is active");
    CheckEq("extender_activation_refused", refusedModel.suffixKey, "a refusal");
    CheckEq(Dotted(Dotted("Active", "IPv4"),
                   "Activation refused: the operator refused the activation"),
            RowText(refusedModel), "the refusal follows on the same line");

    ExtenderProvideStatusView failed = ProvideStatus(
        kExtenderProvideStateActive, "", "dial tcp4 192.0.2.1:443: connect: network is unreachable");
    failed.activatedV6 = true;
    const auto failedModel = ExtenderProvideRowModelFor(failed);
    CheckEq("extender_activation_failed", failedModel.suffixKey, "a failure");
    CheckEq(Dotted(Dotted("Active", "IPv6"),
                   "Activation failed: dial tcp4 192.0.2.1:443: connect: network is unreachable"),
            RowText(failedModel), "the failure follows on the same line");
  }
  {
    TEST_CASE("eachErrorCaseTakesItsLabelFromErrorCase");
    struct ErrorRow {
      const char* errorCase;
      const char* reason;
      const char* key;
      const char* text;
    };
    const ErrorRow rows[] = {
        {kExtenderProvideErrorStart, "no extender directory", "extender_start_failed",
         "Could not start: no extender directory"},
        {kExtenderProvideErrorTcpUnavailable, "listen tcp :443: bind: address already in use",
         "extender_tcp_unavailable",
         "TCP port 443 is in use by another program. Trying again every few minutes: listen tcp "
         ":443: bind: address already in use"},
        {kExtenderProvideErrorListen,
         "tcp 443: bind: permission denied; udp 443: bind: permission denied",
         "extender_listen_failed",
         "Could not listen: tcp 443: bind: permission denied; udp 443: bind: permission denied"},
        {kExtenderProvideErrorActivationRefused, "the operator refused the activation",
         "extender_activation_refused",
         "Activation refused: the operator refused the activation"},
        {kExtenderProvideErrorActivationUnreachable, "the tcp carrier failed after 0.05 s: no route to host",
         "extender_activation_unreachable",
         "The internet can't reach this device on TCP port 443. Allow or forward TCP 443 (and UDP 443 and 4053), or turn Extender off: the tcp carrier failed after 0.05 s: no route to host"},
        {kExtenderProvideErrorActivationFailed, "context deadline exceeded",
         "extender_activation_failed", "Activation failed: context deadline exceeded"},
    };
    for (const ErrorRow& row : rows) {
      const auto model = ExtenderProvideRowModelFor(
          ProvideStatus(kExtenderProvideStateError, row.errorCase, row.reason));
      const std::string at = std::string(" (") + row.errorCase + ")";
      CheckTone(ExtenderProvideTone::Red, model.tone, "red" + at);
      CheckEq(row.key, model.textKey, "key" + at);
      Check(model.argumentKind == ExtenderProvideArgument::Text, "the reason is raw text" + at);
      Check(model.suffixKey.empty(), "no second half in the error state" + at);
      CheckEq(row.text, RowText(model), "text" + at);
    }
    // ErrorCase decides the label; LastActivationRefused never relabels an error
    ExtenderProvideStatusView failedFlaggedRefused = ProvideStatus(
        kExtenderProvideStateError, kExtenderProvideErrorActivationFailed, "timeout");
    failedFlaggedRefused.refused = true;
    CheckEq("extender_activation_failed", ExtenderProvideRowModelFor(failedFlaggedRefused).textKey,
            "a failed case stays failed");
    ExtenderProvideStatusView refusedFlaggedFailed = ProvideStatus(
        kExtenderProvideStateError, kExtenderProvideErrorActivationRefused, "no capacity");
    refusedFlaggedFailed.refused = false;
    CheckEq("extender_activation_refused", ExtenderProvideRowModelFor(refusedFlaggedFailed).textKey,
            "a refused case stays refused");
  }
  {
    TEST_CASE("revokedIsTheWholeMessage");
    const auto model = ExtenderProvideRowModelFor(
        ProvideStatus(kExtenderProvideStateError, kExtenderProvideErrorRevoked));
    CheckTone(ExtenderProvideTone::Red, model.tone, "revoked is red");
    CheckEq("extender_revoked", model.textKey, "revoked key");
    Check(model.argumentKind == ExtenderProvideArgument::None, "no argument");
    CheckEq("Revoked by the operator", RowText(model), "revoked text");
    const auto stray = ExtenderProvideRowModelFor(
        ProvideStatus(kExtenderProvideStateError, kExtenderProvideErrorRevoked, "stray"));
    CheckEq("Revoked by the operator", RowText(stray), "a stray reason is not appended");
  }
  {
    TEST_CASE("anUnknownErrorCaseRendersTheReasonBareInRed");
    const auto model = ExtenderProvideRowModelFor(ProvideStatus(
        kExtenderProvideStateError, "quota", "the operator's quota is exhausted"));
    CheckTone(ExtenderProvideTone::Red, model.tone, "still the error colour");
    CheckEq("", model.textKey, "no label this build would have to invent");
    CheckEq("the operator's quota is exhausted", RowText(model), "the reason, bare");
  }
  {
    TEST_CASE("anUnknownStateNamesNoCase");
    const auto model =
        ExtenderProvideRowModelFor(ProvideStatus("draining", "", "held for maintenance"));
    Check(model.visible, "a supported device still shows its row");
    CheckTone(ExtenderProvideTone::Grey, model.tone, "grey: no error was reported");
    CheckEq("held for maintenance", RowText(model), "the reason, bare");
  }
  {
    TEST_CASE("theSwitchIsTheSetting");
    for (const char* state : {kExtenderProvideStateOff, kExtenderProvideStateNotProviding,
                              kExtenderProvideStateSettingUp, kExtenderProvideStateActive,
                              kExtenderProvideStateError}) {
      ExtenderProvideStatusView view = ProvideStatus(state);
      view.provideExtender = true;
      Check(ExtenderProvideRowModelFor(view).on, std::string("on in ") + state);
      view.provideExtender = false;
      Check(!ExtenderProvideRowModelFor(view).on, std::string("off in ") + state);
    }
  }
}

// ---- ExtenderPresentation.h: the SDK status onto the view (N7) ---------------

// Every field of urnet::ExtenderProvideStatus (urnetwork_sdk.hpp), by the SDK's
// names and types. ExtenderProvideStatusViewOf reads the same names in SdkHost,
// so a rename breaks the app build there; here the struct pins which of the
// thirty-five the view reads, and that the twenty-four it leaves alone change
// nothing. Of the router port mapping's own reading (PortMapping and the eight
// after it, connect EXTENDER.md S10) the view reads the state and the two texts
// an app may show, PortMappingMethodName and PortMappingPorts, and never
// PortMappingError or PortMappingExternalIp: the router's words can name an
// address. None of it is the switch: the switch is the setting, read beside the
// status.
struct FullStatus {
  bool Supported{};
  std::string State{};
  std::string ErrorCase{};
  std::string Reason{};
  bool Enabled{};
  std::string StartError{};
  std::string TcpUnavailableError{};
  bool Listening{};
  std::string ListenError{};
  bool ActivatedV4{};
  bool ActivatedV6{};
  std::string Ipv4{};
  std::string Ipv6{};
  int64_t LastActivationTime{};
  std::string LastActivationError{};
  bool LastActivationRefused{};
  int64_t RevokedTime{};
  std::string DnsPorts{};
  int64_t PeerPingCount{};
  int64_t PeerPingCosignedCount{};
  int64_t PeerPingRejectedCount{};
  int64_t PeerPingUnknownCount{};
  int64_t LastPeerPingTime{};
  int64_t LimitedBySubnetsCount{};
  int64_t LimitedBySourceCount{};
  std::string PortMapping{};
  std::string PortMappingMethod{};
  std::string PortMappingExternalIp{};
  int64_t PortMappingTcpPort{};
  int64_t PortMappingUdpPort{};
  int64_t PortMappingDnsPort{};
  std::string PortMappingError{};
  std::string PortMappingMethodName{};
  std::string PortMappingPorts{};
  int64_t ConnectionCount{};
};

FullStatus BaseStatus() {
  FullStatus status;
  status.Supported = true;
  status.State = kExtenderProvideStateActive;
  status.ActivatedV4 = true;
  status.Enabled = true;
  status.Reason = "dial tcp6 [2001:db8::1]:443: i/o timeout";
  return status;
}

ExtenderProvideStatusView ViewOf(const FullStatus& status, bool setting = true,
                                 bool measurements = true, bool portMapping = false) {
  return ExtenderProvideStatusViewOf(std::optional<FullStatus>(status),
                                     [setting] { return setting; },
                                     [measurements] { return measurements; },
                                     [portMapping] { return portMapping; });
}

void ProvideStatusViewTests() {
  const ExtenderProvideStatusView base = ViewOf(BaseStatus());
  {
    TEST_CASE("readsOnlyTheContractFields");
    FullStatus noisy = BaseStatus();
    noisy.StartError = "no extender directory";
    // the bind error reaches the row as the Reason of the tcp_unavailable case
    noisy.TcpUnavailableError = "listen tcp :443: bind: address already in use";
    noisy.Listening = true;
    noisy.ListenError = "udp 4053: bind: address already in use";
    noisy.Ipv4 = "192.0.2.10";
    noisy.Ipv6 = "2001:db8::10";
    noisy.LastActivationTime = 1757800000000;
    noisy.LastActivationError = "context deadline exceeded";
    noisy.RevokedTime = 1757800000001;
    noisy.DnsPorts = "53,4053";
    noisy.PeerPingCount = 9;
    noisy.PeerPingCosignedCount = 5;
    noisy.PeerPingRejectedCount = 2;
    noisy.PeerPingUnknownCount = 2;
    noisy.LastPeerPingTime = 1757800000002;
    noisy.LimitedBySubnetsCount = 3;
    noisy.LimitedBySourceCount = 4;
    noisy.PortMappingMethod = "pcp";
    noisy.PortMappingExternalIp = "198.51.100.7";
    noisy.PortMappingTcpPort = 443;
    noisy.PortMappingUdpPort = 443;
    noisy.PortMappingDnsPort = 4053;
    noisy.PortMappingError = "udp 4053: 192.168.1.1 refused the mapping";
    noisy.ConnectionCount = 12;
    Check(ViewOf(noisy) == base,
          "the twenty-four fields the view leaves alone change nothing");
    // a mapped router does not turn the switch on: the switch is the setting
    FullStatus mapped = BaseStatus();
    mapped.PortMapping = kExtenderPortMappingMapped;
    mapped.PortMappingMethodName = "PCP";
    mapped.PortMappingPorts = "TCP 443, UDP 443, UDP 4053";
    Check(!ViewOf(mapped).provideExtenderPortMapping,
          "the router port mapping's state is not the port mapping switch");
  }
  {
    TEST_CASE("readsThePortMappingsStateAndTextsNeverItsError");
    FullStatus mapped = BaseStatus();
    mapped.PortMapping = kExtenderPortMappingMapped;
    mapped.PortMappingMethod = "natpmp";
    mapped.PortMappingMethodName = "NAT-PMP";
    mapped.PortMappingPorts = "TCP 40443\xE2\x86\x92" "443, UDP 4053";
    mapped.PortMappingExternalIp = "198.51.100.7";
    mapped.PortMappingError = "udp 443: 192.168.1.1 answered 718 ConflictInMappingEntry";
    const ExtenderProvideStatusView view = ViewOf(mapped);
    CheckEq("mapped", view.portMapping, "the state comes across as the sdk's token");
    CheckEq("NAT-PMP", view.portMappingMethodName, "the method comes across as its name");
    CheckEq("TCP 40443\xE2\x86\x92" "443, UDP 4053", view.portMappingPorts,
            "the ports text comes across as it is, its arrow and all");
    // the router's words and its address reach no field of the view
    FullStatus quiet = mapped;
    quiet.PortMappingExternalIp.clear();
    quiet.PortMappingError.clear();
    Check(ViewOf(quiet) == view, "the router's error and external address change nothing");
    for (const std::string& field :
         {view.state, view.errorCase, view.reason, view.portMapping, view.portMappingMethodName,
          view.portMappingPorts}) {
      Check(field.find("192.168.1.1") == std::string::npos &&
                field.find("198.51.100.7") == std::string::npos &&
                field.find("ConflictInMappingEntry") == std::string::npos,
            "no field of the view carries the router's words or an address it names");
    }
    // nothing of it is read off a role the device does not report
    FullStatus unsupported = mapped;
    unsupported.Supported = false;
    const ExtenderProvideStatusView hidden = ViewOf(unsupported);
    Check(hidden.portMapping.empty() && hidden.portMappingMethodName.empty() &&
              hidden.portMappingPorts.empty(),
          "an unsupported role reads no port mapping state");
  }
  {
    TEST_CASE("eachReadFieldMovesTheView");
    struct Flip {
      const char* field;
      void (*apply)(FullStatus&);
    };
    const Flip flips[] = {
        {"Supported", [](FullStatus& s) { s.Supported = false; }},
        {"State", [](FullStatus& s) { s.State = kExtenderProvideStateSettingUp; }},
        {"ErrorCase", [](FullStatus& s) { s.ErrorCase = kExtenderProvideErrorListen; }},
        {"Reason", [](FullStatus& s) { s.Reason = "tcp 443: bind: permission denied"; }},
        {"ActivatedV4", [](FullStatus& s) { s.ActivatedV4 = false; }},
        {"ActivatedV6", [](FullStatus& s) { s.ActivatedV6 = true; }},
        {"LastActivationRefused", [](FullStatus& s) { s.LastActivationRefused = true; }},
        {"Enabled", [](FullStatus& s) { s.Enabled = false; }},
        {"PortMapping", [](FullStatus& s) { s.PortMapping = kExtenderPortMappingFailed; }},
        {"PortMappingMethodName", [](FullStatus& s) { s.PortMappingMethodName = "UPnP"; }},
        {"PortMappingPorts", [](FullStatus& s) { s.PortMappingPorts = "TCP 443"; }},
    };
    for (const Flip& flip : flips) {
      FullStatus status = BaseStatus();
      flip.apply(status);
      Check(ViewOf(status) != base,
            std::string(flip.field) + " alone is a change the feed publishes");
    }
    Check(ViewOf(BaseStatus(), false) != base, "the setting alone is a change");
    Check(ViewOf(BaseStatus(), true, false) != base,
          "the measurements setting alone is a change");
    Check(ViewOf(BaseStatus(), true, true, true) != base,
          "the port mapping setting alone is a change");
    // Supported alone: a device that starts reporting the role with its setting
    // off differs from the unsupported view in nothing else, and the feed must
    // still publish it, or the row would stay hidden
    FullStatus offStatus;
    offStatus.Supported = true;
    offStatus.State = kExtenderProvideStateOff;
    Check(ViewOf(offStatus, false) !=
              ExtenderProvideStatusViewOf(std::optional<FullStatus>(), [] { return false; },
                                          [] { return true; }, [] { return false; }),
          "Supported alone is a change the feed publishes");

    // and each lands where the row reads it
    FullStatus v6 = BaseStatus();
    v6.ActivatedV4 = false;
    v6.ActivatedV6 = true;
    CheckEq("ipv6", ExtenderProvideRowModelFor(ViewOf(v6)).argument, "IPv6 alone reads ipv6");
    FullStatus refused = BaseStatus();
    refused.LastActivationRefused = true;
    CheckEq("extender_activation_refused",
            ExtenderProvideRowModelFor(ViewOf(refused)).suffixKey,
            "a refusal beside the active line is labeled a refusal");
    FullStatus stopped = BaseStatus();
    stopped.Enabled = false;
    Check(base.enabled && !ViewOf(stopped).enabled, "enabled follows Enabled");
  }
  {
    TEST_CASE("theDeviceItIsOfIsAChange");
    // A session that takes over from the provider-only device can bring the
    // very same reading; the feed must still publish it, or the Connect page's
    // switch would stay hidden for the whole session.
    ExtenderProvideStatusView providerOnly = base;
    providerOnly.providerOnly = true;
    Check(providerOnly != base, "the provider-only device's reading alone is a change");
    Check(!base.providerOnly, "a status read off a device is a session's unless marked");
    // ...and so is whether the service takes its write: the switch comes or
    // goes with it
    ExtenderProvideStatusView writable = providerOnly;
    writable.serviceWritable = true;
    Check(writable != providerOnly, "whether the service writes it alone is a change");
    Check(!providerOnly.serviceWritable, "a status the service did not mark has no writer");
    ExtenderProvideStatusView measurementsWritable = writable;
    measurementsWritable.measurementsServiceWritable = true;
    Check(measurementsWritable != writable,
          "whether the service writes the measurements setting alone is a change");
    ExtenderProvideStatusView portMappingWritable = measurementsWritable;
    portMappingWritable.portMappingServiceWritable = true;
    Check(portMappingWritable != measurementsWritable,
          "whether the service writes the port mapping setting alone is a change");
  }
  {
    TEST_CASE("aHiddenRowReadsNoSetting");
    int reads = 0;
    const auto counting = [&reads] {
      ++reads;
      return true;
    };
    const ExtenderProvideStatusView none =
        ExtenderProvideStatusViewOf(std::optional<FullStatus>(), counting, counting, counting);
    Check(!ExtenderProvideRowModelFor(none).visible, "no status is hidden");
    FullStatus unsupported = BaseStatus();
    unsupported.Supported = false;
    const ExtenderProvideStatusView hidden = ExtenderProvideStatusViewOf(
        std::optional<FullStatus>(unsupported), counting, counting, counting);
    Check(!ExtenderProvideRowModelFor(hidden).visible, "an unsupported status is hidden");
    CheckEq(0, reads, "and neither asks the device for any setting");
    Check(!hidden.provideExtenderPortMapping, "and the port mapping setting reads off");
    int supportedReads = 0;
    int measurementsReads = 0;
    int portMappingReads = 0;
    const ExtenderProvideStatusView shown = ExtenderProvideStatusViewOf(
        std::optional<FullStatus>(BaseStatus()),
        [&supportedReads] {
          ++supportedReads;
          return false;
        },
        [&measurementsReads] {
          ++measurementsReads;
          return false;
        },
        [&portMappingReads] {
          ++portMappingReads;
          return true;
        });
    CheckEq(1, supportedReads, "a row that shows reads the setting once");
    Check(!shown.provideExtender, "and its switch is that setting");
    CheckEq(1, measurementsReads, "and the measurements setting once");
    Check(!shown.measurementsEnabled, "which the speed tests switch shows");
    CheckEq(1, portMappingReads, "and the port mapping setting once");
    Check(shown.provideExtenderPortMapping, "which the port mapping switch shows");
  }
}

void ProvideGuessTests() {
  ExtenderProvideStatusView listening = ProvideStatus(
      kExtenderProvideStateError, kExtenderProvideErrorListen, "tcp 443: bind: address in use");
  listening.activatedV4 = true;
  listening.refused = true;
  listening.enabled = true;
  {
    TEST_CASE("offIsGreyOff");
    const auto model = ExtenderProvideRowModelFor(ExtenderProvideGuessFor(listening, false, true));
    Check(model.visible, "the row stays shown");
    CheckTone(ExtenderProvideTone::Grey, model.tone, "grey");
    CheckEq("Off", RowText(model), "Off");
    Check(!model.on, "the switch reads off");
  }
  {
    TEST_CASE("onWhileProvidingIsYellowSettingUp");
    const auto guess = ExtenderProvideGuessFor(listening, true, true);
    const auto model = ExtenderProvideRowModelFor(guess);
    CheckTone(ExtenderProvideTone::Yellow, model.tone, "yellow");
    CheckEq("Setting up", RowText(model), "Setting up");
    Check(model.on, "the switch reads on");
    Check(guess.enabled, "the guessed role runs");
  }
  {
    TEST_CASE("onWhileNotProvidingIsGreyNotProviding");
    const auto guess = ExtenderProvideGuessFor(listening, true, false);
    const auto model = ExtenderProvideRowModelFor(guess);
    CheckTone(ExtenderProvideTone::Grey, model.tone, "grey");
    CheckEq("Not providing", RowText(model), "Not providing");
    Check(model.on, "the switch reads on");
    Check(!guess.enabled, "no role runs while not providing");
  }
  {
    TEST_CASE("theGuessDropsTheOldReading");
    const auto guess = ExtenderProvideGuessFor(listening, true, true);
    CheckEq("", guess.errorCase, "no error case");
    CheckEq("", guess.reason, "no reason");
    Check(!guess.activatedV4 && !guess.activatedV6 && !guess.refused, "no families, no refusal");
    Check(guess.supported, "and the device is still the one that supports the role");
  }
  {
    TEST_CASE("theGuessStaysOfItsDevice");
    Check(!ExtenderProvideGuessFor(listening, true, true).providerOnly,
          "a guess over a session's status stays a session's");
    ExtenderProvideStatusView providerOnly = listening;
    providerOnly.providerOnly = true;
    const auto guess = ExtenderProvideGuessFor(providerOnly, false, true);
    Check(guess.providerOnly, "a guess over the provider-only device's status stays its");
    Check(!ExtenderProvideRowModelFor(guess).switchVisible,
          "so the switch's row stays hidden over an older service's");
    providerOnly.serviceWritable = true;
    const auto written = ExtenderProvideGuessFor(providerOnly, false, true);
    Check(written.providerOnly && written.serviceWritable,
          "a guess over a status the service writes keeps its writer");
    const auto writtenModel = ExtenderProvideRowModelFor(written);
    Check(writtenModel.switchVisible && !writtenModel.on,
          "so the switch just flipped stays shown, in its new position");
  }
}

// ---- ExtenderPresentation.h: where the switch's write goes ------------------

void ProvideWriteRouteTests() {
  const ExtenderProvideStatusView session = ProvideStatus(kExtenderProvideStateActive);
  ExtenderProvideStatusView older = session;
  older.providerOnly = true;
  ExtenderProvideStatusView writable = older;
  writable.serviceWritable = true;
  {
    TEST_CASE("aSessionsDeviceTakesEveryWrite");
    // whatever was last on screen: the bound device is the one that runs
    for (const ExtenderProvideStatusView& shown :
         {session, older, writable, ExtenderProvideStatusView{}}) {
      Check(ExtenderProvideWriteRouteFor(true, shown) == ExtenderProvideWriteRoute::Device,
            "with a session the write goes through its device");
    }
  }
  {
    TEST_CASE("withNoSessionTheServiceTakesTheProviderOnlyDevicesWrite");
    Check(ExtenderProvideWriteRouteFor(false, writable) == ExtenderProvideWriteRoute::Service,
          "with no session the provider-only device's write goes to the service");
  }
  {
    TEST_CASE("withNoSessionNothingElseIsWritten");
    Check(ExtenderProvideWriteRouteFor(false, older) == ExtenderProvideWriteRoute::None,
          "an older service's provider-only status is never written");
    Check(ExtenderProvideWriteRouteFor(false, session) == ExtenderProvideWriteRoute::None,
          "a session's status left over with no session is never written");
    Check(ExtenderProvideWriteRouteFor(false, ExtenderProvideStatusView{}) ==
              ExtenderProvideWriteRoute::None,
          "no status is never written");
    ExtenderProvideStatusView unsupported = writable;
    unsupported.supported = false;
    Check(ExtenderProvideWriteRouteFor(false, unsupported) == ExtenderProvideWriteRoute::None,
          "an unsupported role is never written");
  }
  {
    TEST_CASE("theServiceIsWrittenExactlyWhereTheSwitchShows");
    // the switch's visibility and the write's route are one rule: no switch
    // the user can flip goes unwritten, and nothing hidden is written
    for (const ExtenderProvideStatusView& shown :
         {session, older, writable, ExtenderProvideStatusView{}}) {
      for (bool supported : {false, true}) {
        ExtenderProvideStatusView view = shown;
        view.supported = supported;
        if (!view.providerOnly) continue;
        Check(ExtenderProvideRowModelFor(view).switchVisible ==
                  (ExtenderProvideWriteRouteFor(false, view) == ExtenderProvideWriteRoute::Service),
              "the provider-only switch shows exactly when its write goes to the service");
      }
    }
  }
}

// ---- ExtenderPresentation.h: the lag.dev speed tests switch -----------------

void SpeedTestsRowTests() {
  const ExtenderProvideStatusView session = ProvideStatus(kExtenderProvideStateActive);
  ExtenderProvideStatusView older = session;
  older.providerOnly = true;
  ExtenderProvideStatusView extenderOnly = older;
  extenderOnly.serviceWritable = true;
  ExtenderProvideStatusView writable = extenderOnly;
  writable.measurementsServiceWritable = true;
  {
    TEST_CASE("onByDefault");
    Check(ExtenderProvideStatusView{}.measurementsEnabled,
          "the view starts with the setting on, as the sdk's");
    Check(SpeedTestsRowModel{}.on, "and so does the row");
  }
  {
    TEST_CASE("hiddenWithoutTheRole");
    Check(!SpeedTestsRowModelFor(ExtenderProvideStatusView{}).visible, "no status: hidden");
    ExtenderProvideStatusView unsupported = session;
    unsupported.supported = false;
    Check(!SpeedTestsRowModelFor(unsupported).visible, "an unsupported role: hidden");
  }
  {
    TEST_CASE("besideTheExtenderSwitch");
    Check(SpeedTestsRowModelFor(session).visible, "a session's status shows it");
    for (bool extenderOn : {false, true}) {
      ExtenderProvideStatusView view = session;
      view.provideExtender = extenderOn;
      view.state = extenderOn ? kExtenderProvideStateActive : kExtenderProvideStateOff;
      Check(SpeedTestsRowModelFor(view).visible,
            "it shows whether or not the Extender switch is on: the setting still applies");
    }
    // exactly where the Extender switch shows, never without it
    for (const ExtenderProvideStatusView& view :
         {session, older, extenderOnly, writable, ExtenderProvideStatusView{}}) {
      if (SpeedTestsRowModelFor(view).visible) {
        Check(ExtenderProvideRowModelFor(view).switchVisible,
              "the speed tests switch never shows without the Extender switch");
      }
    }
  }
  {
    TEST_CASE("theProviderOnlyStatusNeedsTheServicesSay");
    Check(!SpeedTestsRowModelFor(older).visible, "an older service's status: hidden");
    Check(!SpeedTestsRowModelFor(extenderOnly).visible,
          "a service that takes only the Extender switch's write: hidden");
    Check(SpeedTestsRowModelFor(writable).visible, "a service that takes both: shown");
  }
  {
    TEST_CASE("theSwitchIsTheSetting");
    ExtenderProvideStatusView off = session;
    off.measurementsEnabled = false;
    Check(!SpeedTestsRowModelFor(off).on, "off reads off");
    Check(SpeedTestsRowModelFor(session).on, "on reads on");
  }
  {
    TEST_CASE("itsGuessChangesOnlyTheSetting");
    const ExtenderProvideStatusView guess = SpeedTestsGuessFor(writable, false);
    Check(!guess.measurementsEnabled, "the guess holds the new position");
    ExtenderProvideStatusView expected = writable;
    expected.measurementsEnabled = false;
    Check(guess == expected, "and nothing else");
    Check(SpeedTestsRowModelFor(guess).visible, "so the switch just flipped stays shown");
  }
  {
    TEST_CASE("theExtenderSwitchsGuessKeepsIt");
    ExtenderProvideStatusView off = writable;
    off.measurementsEnabled = false;
    for (bool on : {false, true}) {
      const ExtenderProvideStatusView guess = ExtenderProvideGuessFor(off, on, true);
      Check(!guess.measurementsEnabled,
            "flipping the Extender switch leaves the speed tests switch where it was");
      Check(guess.measurementsServiceWritable, "and its writer");
      Check(SpeedTestsRowModelFor(guess).visible, "so it stays shown under the guess");
    }
  }
}

void SpeedTestsWriteRouteTests() {
  const ExtenderProvideStatusView session = ProvideStatus(kExtenderProvideStateActive);
  ExtenderProvideStatusView older = session;
  older.providerOnly = true;
  ExtenderProvideStatusView extenderOnly = older;
  extenderOnly.serviceWritable = true;
  ExtenderProvideStatusView writable = extenderOnly;
  writable.measurementsServiceWritable = true;
  {
    TEST_CASE("aSessionsDeviceTakesEveryWrite");
    for (const ExtenderProvideStatusView& shown :
         {session, older, extenderOnly, writable, ExtenderProvideStatusView{}}) {
      Check(SpeedTestsWriteRouteFor(true, shown) == ExtenderProvideWriteRoute::Device,
            "with a session the write goes through its device");
    }
  }
  {
    TEST_CASE("withNoSessionTheServiceTakesItOnlyWhenItSaidSo");
    Check(SpeedTestsWriteRouteFor(false, writable) == ExtenderProvideWriteRoute::Service,
          "a service that takes it is written");
    Check(SpeedTestsWriteRouteFor(false, extenderOnly) == ExtenderProvideWriteRoute::None,
          "a service that takes only the Extender switch's write is not");
    Check(SpeedTestsWriteRouteFor(false, older) == ExtenderProvideWriteRoute::None,
          "an older service is not");
    Check(SpeedTestsWriteRouteFor(false, session) == ExtenderProvideWriteRoute::None,
          "a session's status left over with no session is not");
    Check(SpeedTestsWriteRouteFor(false, ExtenderProvideStatusView{}) ==
              ExtenderProvideWriteRoute::None,
          "no status is not");
    ExtenderProvideStatusView unsupported = writable;
    unsupported.supported = false;
    Check(SpeedTestsWriteRouteFor(false, unsupported) == ExtenderProvideWriteRoute::None,
          "an unsupported role is not");
  }
  {
    TEST_CASE("theServiceIsWrittenExactlyWhereTheSwitchShows");
    for (const ExtenderProvideStatusView& shown : {older, extenderOnly, writable}) {
      for (bool supported : {false, true}) {
        ExtenderProvideStatusView view = shown;
        view.supported = supported;
        Check(SpeedTestsRowModelFor(view).visible ==
                  (SpeedTestsWriteRouteFor(false, view) == ExtenderProvideWriteRoute::Service),
              "the provider-only switch shows exactly when its write goes to the service");
      }
    }
  }
}

// ---- ExtenderPresentation.h: the router port mapping switch -----------------

// The provider-only statuses a service can send, from one too old to take any
// switch's write to one that takes all three, and one that takes the port
// mapping switch's write without the speed tests switch's, which no service
// sends: it pins that the switch never shows without the speed tests switch.
struct PortMappingStatuses {
  ExtenderProvideStatusView session;
  ExtenderProvideStatusView older;
  ExtenderProvideStatusView extenderOnly;
  ExtenderProvideStatusView speedTestsOnly;
  ExtenderProvideStatusView portMappingOnly;
  ExtenderProvideStatusView writable;
};

PortMappingStatuses PortMappingStatusesFor() {
  PortMappingStatuses statuses;
  statuses.session = ProvideStatus(kExtenderProvideStateActive);
  statuses.older = statuses.session;
  statuses.older.providerOnly = true;
  statuses.extenderOnly = statuses.older;
  statuses.extenderOnly.serviceWritable = true;
  statuses.speedTestsOnly = statuses.extenderOnly;
  statuses.speedTestsOnly.measurementsServiceWritable = true;
  statuses.portMappingOnly = statuses.extenderOnly;
  statuses.portMappingOnly.portMappingServiceWritable = true;
  statuses.writable = statuses.speedTestsOnly;
  statuses.writable.portMappingServiceWritable = true;
  return statuses;
}

void PortMappingRowTests() {
  const PortMappingStatuses s = PortMappingStatusesFor();
  {
    TEST_CASE("theStoreHasItsTitleAndDescription");
    for (const char* key : {"extender_port_mapping", "extender_port_mapping_description"}) {
      const auto found = EnglishStore().find(key);
      Check(found != EnglishStore().end(), std::string("the store carries ") + key);
      if (found == EnglishStore().end()) continue;
      Check(!found->second.empty() && found->second.find('{') == std::string::npos,
            std::string(key) + " is whole text, with no placeholder");
    }
  }
  {
    TEST_CASE("offByDefault");
    Check(!ExtenderProvideStatusView{}.provideExtenderPortMapping,
          "the view starts with the setting off, as the sdk's on every app");
    Check(!PortMappingRowModel{}.on, "and so does the row");
    Check(!PortMappingRowModelFor(s.writable).on,
          "a status read with the setting at its default reads off");
  }
  {
    TEST_CASE("hiddenWithoutTheRole");
    Check(!PortMappingRowModelFor(ExtenderProvideStatusView{}).visible, "no status: hidden");
    ExtenderProvideStatusView unsupported = s.session;
    unsupported.supported = false;
    Check(!PortMappingRowModelFor(unsupported).visible, "an unsupported role: hidden");
  }
  {
    TEST_CASE("underTheSpeedTestsSwitch");
    Check(PortMappingRowModelFor(s.session).visible, "a session's status shows it");
    for (bool extenderOn : {false, true}) {
      ExtenderProvideStatusView view = s.session;
      view.provideExtender = extenderOn;
      view.state = extenderOn ? kExtenderProvideStateActive : kExtenderProvideStateOff;
      Check(PortMappingRowModelFor(view).visible,
            "it shows whether or not the Extender switch is on: the setting still applies");
    }
    // only where the speed tests switch shows, never without it
    for (const ExtenderProvideStatusView& view :
         {s.session, s.older, s.extenderOnly, s.speedTestsOnly, s.portMappingOnly, s.writable,
          ExtenderProvideStatusView{}}) {
      if (PortMappingRowModelFor(view).visible) {
        Check(SpeedTestsRowModelFor(view).visible,
              "the port mapping switch never shows without the speed tests switch");
        Check(ExtenderProvideRowModelFor(view).switchVisible,
              "nor without the Extender switch");
      }
    }
  }
  {
    TEST_CASE("theProviderOnlyStatusNeedsTheServicesSay");
    Check(!PortMappingRowModelFor(s.older).visible, "an older service's status: hidden");
    Check(!PortMappingRowModelFor(s.extenderOnly).visible,
          "a service that takes only the Extender switch's write: hidden");
    Check(!PortMappingRowModelFor(s.speedTestsOnly).visible,
          "a service that takes the other switches' writes but not this one: hidden");
    Check(PortMappingRowModelFor(s.writable).visible, "a service that takes all three: shown");
  }
  {
    TEST_CASE("theSwitchIsTheSetting");
    ExtenderProvideStatusView on = s.session;
    on.provideExtenderPortMapping = true;
    Check(PortMappingRowModelFor(on).on, "on reads on");
    Check(!PortMappingRowModelFor(s.session).on, "off reads off");
  }
  {
    TEST_CASE("itsGuessChangesOnlyTheSetting");
    const ExtenderProvideStatusView guess = PortMappingGuessFor(s.writable, true);
    Check(guess.provideExtenderPortMapping, "the guess holds the new position");
    ExtenderProvideStatusView expected = s.writable;
    expected.provideExtenderPortMapping = true;
    Check(guess == expected, "and nothing else");
    Check(PortMappingRowModelFor(guess).visible, "so the switch just flipped stays shown");
    Check(!PortMappingGuessFor(guess, false).provideExtenderPortMapping,
          "and a flip back holds off");
  }
  {
    TEST_CASE("itsGuessDropsTheRolesLastReport");
    // A changed setting replaces a running role (connect EXTENDER.md S10), so
    // what the router did for the old one is no longer so: the line goes with
    // the flip, and the next pushed status brings the new role's.
    ExtenderProvideStatusView mapped = s.writable;
    mapped.provideExtenderPortMapping = true;
    mapped.portMapping = kExtenderPortMappingMapped;
    mapped.portMappingMethodName = "PCP";
    mapped.portMappingPorts = "TCP 443, UDP 443, UDP 4053";
    for (bool on : {false, true}) {
      const ExtenderProvideStatusView guess = PortMappingGuessFor(mapped, on);
      Check(guess.portMapping.empty() && guess.portMappingMethodName.empty() &&
                guess.portMappingPorts.empty(),
            "a flip drops the role's last port mapping report");
      Check(PortMappingRowModelFor(guess).statusKey.empty(),
            "so no line shows until the next pushed status");
      ExtenderProvideStatusView expected = mapped;
      expected.provideExtenderPortMapping = on;
      expected.portMapping.clear();
      expected.portMappingMethodName.clear();
      expected.portMappingPorts.clear();
      Check(guess == expected, "and nothing else of the reading changes");
    }
    // the Extender switch's guess starts or stops the role, so it drops it too
    const ExtenderProvideStatusView extenderGuess = ExtenderProvideGuessFor(mapped, true, true);
    Check(extenderGuess.portMapping.empty() &&
              PortMappingRowModelFor(extenderGuess).statusKey.empty(),
          "flipping the Extender switch drops the port mapping report");
    // the speed tests switch changes no role: the router still forwards
    const ExtenderProvideStatusView speedTestsGuess = SpeedTestsGuessFor(mapped, false);
    Check(speedTestsGuess.portMapping == kExtenderPortMappingMapped &&
              !PortMappingRowModelFor(speedTestsGuess).statusKey.empty(),
          "flipping the speed tests switch keeps the port mapping line");
  }
  {
    TEST_CASE("theOtherSwitchesGuessesKeepIt");
    ExtenderProvideStatusView on = s.writable;
    on.provideExtenderPortMapping = true;
    for (bool extenderOn : {false, true}) {
      const ExtenderProvideStatusView guess = ExtenderProvideGuessFor(on, extenderOn, true);
      Check(guess.provideExtenderPortMapping,
            "flipping the Extender switch leaves the port mapping switch where it was");
      Check(guess.portMappingServiceWritable, "and its writer");
      Check(PortMappingRowModelFor(guess).visible, "so it stays shown under the guess");
    }
    for (bool measurementsOn : {false, true}) {
      const ExtenderProvideStatusView guess = SpeedTestsGuessFor(on, measurementsOn);
      Check(guess.provideExtenderPortMapping && PortMappingRowModelFor(guess).visible,
            "flipping the speed tests switch leaves the port mapping switch where it was");
    }
    // and its own guess leaves the speed tests switch where it was
    ExtenderProvideStatusView speedTestsOff = s.writable;
    speedTestsOff.measurementsEnabled = false;
    Check(!PortMappingGuessFor(speedTestsOff, true).measurementsEnabled,
          "flipping the port mapping switch leaves the speed tests switch where it was");
  }
}

void PortMappingWriteRouteTests() {
  const PortMappingStatuses s = PortMappingStatusesFor();
  {
    TEST_CASE("aSessionsDeviceTakesEveryWrite");
    for (const ExtenderProvideStatusView& shown :
         {s.session, s.older, s.extenderOnly, s.speedTestsOnly, s.writable,
          ExtenderProvideStatusView{}}) {
      Check(PortMappingWriteRouteFor(true, shown) == ExtenderProvideWriteRoute::Device,
            "with a session the write goes through its device");
    }
  }
  {
    TEST_CASE("withNoSessionTheServiceTakesItOnlyWhenItSaidSo");
    Check(PortMappingWriteRouteFor(false, s.writable) == ExtenderProvideWriteRoute::Service,
          "a service that takes it is written");
    Check(PortMappingWriteRouteFor(false, s.speedTestsOnly) == ExtenderProvideWriteRoute::None,
          "a service that takes only the other switches' writes is not");
    Check(PortMappingWriteRouteFor(false, s.extenderOnly) == ExtenderProvideWriteRoute::None,
          "a service that takes only the Extender switch's write is not");
    Check(PortMappingWriteRouteFor(false, s.older) == ExtenderProvideWriteRoute::None,
          "an older service is not");
    Check(PortMappingWriteRouteFor(false, s.session) == ExtenderProvideWriteRoute::None,
          "a session's status left over with no session is not");
    Check(PortMappingWriteRouteFor(false, ExtenderProvideStatusView{}) ==
              ExtenderProvideWriteRoute::None,
          "no status is not");
    ExtenderProvideStatusView unsupported = s.writable;
    unsupported.supported = false;
    Check(PortMappingWriteRouteFor(false, unsupported) == ExtenderProvideWriteRoute::None,
          "an unsupported role is not");
  }
  {
    TEST_CASE("theServiceIsWrittenExactlyWhereTheSwitchShows");
    for (const ExtenderProvideStatusView& shown :
         {s.older, s.extenderOnly, s.speedTestsOnly, s.portMappingOnly, s.writable}) {
      for (bool supported : {false, true}) {
        ExtenderProvideStatusView view = shown;
        view.supported = supported;
        Check(PortMappingRowModelFor(view).visible ==
                  (PortMappingWriteRouteFor(false, view) == ExtenderProvideWriteRoute::Service),
              "the provider-only switch shows exactly when its write goes to the service");
      }
    }
  }
}

// ---- ExtenderPresentation.h: the router port mapping's status line ----------

// The line as the app composes it (ProvideModeVisual.h PortMappingStatusText),
// in English: the key through the store, and the mapped line through
// std::vformat with the sdk's two texts as its {0} and {1}, as urnw::Format
// fills a translation that may reorder them. Empty for no line.
std::string PortMappingLine(const PortMappingRowModel& model) {
  return ComposePortMappingStatusText<std::string>(
      model, [](const std::string& key) { return EnglishFor(key); },
      [](const std::string& key, const std::string& ports, const std::string& method) {
        return std::vformat(EnglishFor(key), std::make_format_args(ports, method));
      },
      [](const std::string& utf8) { return utf8; });
}

// A shown switch, turned on, over a role that reports `state` with the texts.
ExtenderProvideStatusView PortMappingReport(const char* state, const char* methodName = "",
                                            const char* ports = "") {
  ExtenderProvideStatusView view = ProvideStatus(kExtenderProvideStateActive);
  view.enabled = true;
  view.provideExtenderPortMapping = true;
  view.portMapping = state;
  view.portMappingMethodName = methodName;
  view.portMappingPorts = ports;
  return view;
}

void PortMappingStatusLineTests() {
  const PortMappingStatuses s = PortMappingStatusesFor();
  // "TCP 40443→443": the router's port, an arrow, the bound one
  const std::string moved = "TCP 40443\xE2\x86\x92" "443, UDP 4053";
  {
    TEST_CASE("theStoreHasEveryLine");
    for (const char* key : {"extender_port_mapping_not_needed", "extender_port_mapping_mapping",
                            "extender_port_mapping_failed"}) {
      const auto found = EnglishStore().find(key);
      Check(found != EnglishStore().end(), std::string("the store carries ") + key);
      if (found == EnglishStore().end()) continue;
      Check(!found->second.empty() && found->second.find('{') == std::string::npos,
            std::string(key) + " is whole text, with no placeholder");
    }
    const auto mapped = EnglishStore().find("extender_port_mapping_mapped");
    Check(mapped != EnglishStore().end(), "the store carries extender_port_mapping_mapped");
    if (mapped != EnglishStore().end()) {
      Check(mapped->second.find("{0}") != std::string::npos &&
                mapped->second.find("{1}") != std::string::npos &&
                mapped->second.find("{}") == std::string::npos,
            "extender_port_mapping_mapped takes the ports as {0} and the method as {1}");
    }
  }
  {
    TEST_CASE("eachStateReadsItsLine");
    CheckEq("Not needed: this device has a public address.",
            PortMappingLine(PortMappingRowModelFor(PortMappingReport(kExtenderPortMappingNotNeeded))),
            "not_needed reads extender_port_mapping_not_needed");
    CheckEq("Asking your router\xE2\x80\xA6",
            PortMappingLine(PortMappingRowModelFor(PortMappingReport(kExtenderPortMappingMapping))),
            "mapping reads extender_port_mapping_mapping");
    CheckEq("Your router didn't open the ports. Trying again later.",
            PortMappingLine(PortMappingRowModelFor(PortMappingReport(kExtenderPortMappingFailed))),
            "failed reads extender_port_mapping_failed");
    CheckEq("Your router forwards TCP 443, UDP 443, UDP 4053 (PCP).",
            PortMappingLine(PortMappingRowModelFor(PortMappingReport(
                kExtenderPortMappingMapped, "PCP", "TCP 443, UDP 443, UDP 4053"))),
            "mapped reads extender_port_mapping_mapped, the ports and then the method");
    CheckEq("Your router forwards " + moved + " (NAT-PMP).",
            PortMappingLine(PortMappingRowModelFor(
                PortMappingReport(kExtenderPortMappingMapped, "NAT-PMP", moved.c_str()))),
            "a port the router moved reads as the sdk wrote it");
    const PortMappingRowModel model = PortMappingRowModelFor(
        PortMappingReport(kExtenderPortMappingMapped, "UPnP", "TCP 443"));
    CheckEq("extender_port_mapping_mapped", model.statusKey, "the mapped line's key");
    CheckEq("TCP 443", model.statusPorts, "its {0}, the ports");
    CheckEq("UPnP", model.statusMethod, "its {1}, the method");
  }
  {
    TEST_CASE("offNoRoleOrAnUnknownStateReadsNoLine");
    for (const char* state : {kExtenderPortMappingOff, "", "renewing"}) {
      const PortMappingRowModel model =
          PortMappingRowModelFor(PortMappingReport(state, "PCP", "TCP 443"));
      Check(model.statusKey.empty() && PortMappingLine(model).empty(),
            std::string("no line for the state \"") + state + "\"");
      Check(model.visible && model.on, "and the switch still shows, on");
    }
  }
  {
    TEST_CASE("aMappedLineNeedsBothTexts");
    for (const auto& [method, ports] : std::initializer_list<std::pair<const char*, const char*>>{
             {"", "TCP 443"}, {"PCP", ""}, {"", ""}}) {
      const PortMappingRowModel model =
          PortMappingRowModelFor(PortMappingReport(kExtenderPortMappingMapped, method, ports));
      Check(model.statusKey.empty() && PortMappingLine(model).empty(),
            "a mapped status without its method or its ports shows no line, never empty "
            "parentheses");
    }
  }
  {
    TEST_CASE("onlyWhileTheSwitchIsOn");
    for (const char* state : {kExtenderPortMappingNotNeeded, kExtenderPortMappingMapping,
                              kExtenderPortMappingMapped, kExtenderPortMappingFailed}) {
      ExtenderProvideStatusView view = PortMappingReport(state, "PCP", "TCP 443");
      view.provideExtenderPortMapping = false;
      const PortMappingRowModel model = PortMappingRowModelFor(view);
      Check(model.visible && !model.on && model.statusKey.empty(),
            std::string("no line while the switch is off: ") + state);
    }
  }
  {
    TEST_CASE("onlyUnderASwitchThatShows");
    for (ExtenderProvideStatusView view : {s.older, s.extenderOnly, s.speedTestsOnly}) {
      view.provideExtenderPortMapping = true;
      view.portMapping = kExtenderPortMappingMapped;
      view.portMappingMethodName = "PCP";
      view.portMappingPorts = "TCP 443";
      const PortMappingRowModel model = PortMappingRowModelFor(view);
      Check(!model.visible && model.statusKey.empty(), "a hidden switch has no line under it");
    }
    ExtenderProvideStatusView unsupported = PortMappingReport(kExtenderPortMappingFailed);
    unsupported.supported = false;
    Check(PortMappingRowModelFor(unsupported).statusKey.empty(),
          "nor does an unsupported role");
    // the provider-only device's status, from a service that takes the write,
    // reads exactly as a session's
    for (const char* state : {kExtenderPortMappingNotNeeded, kExtenderPortMappingMapping,
                              kExtenderPortMappingMapped, kExtenderPortMappingFailed}) {
      ExtenderProvideStatusView providerOnly = s.writable;
      providerOnly.provideExtenderPortMapping = true;
      providerOnly.portMapping = state;
      providerOnly.portMappingMethodName = "PCP";
      providerOnly.portMappingPorts = "TCP 443";
      CheckEq(PortMappingLine(PortMappingRowModelFor(PortMappingReport(state, "PCP", "TCP 443"))),
              PortMappingLine(PortMappingRowModelFor(providerOnly)),
              std::string("the provider-only device's line: ") + state);
    }
  }
  {
    TEST_CASE("theRoutersWordsNeverShow");
    // what a device reports beside a failure or a mapping: its error names the
    // gateway and its external address the public one; neither field is read,
    // so no line can carry them, whatever the state
    for (const char* state : {kExtenderPortMappingNotNeeded, kExtenderPortMappingMapping,
                              kExtenderPortMappingMapped, kExtenderPortMappingFailed}) {
      FullStatus status = BaseStatus();
      status.PortMapping = state;
      status.PortMappingMethod = "upnp";
      status.PortMappingMethodName = "UPnP";
      status.PortMappingPorts = "TCP 443, UDP 4053";
      status.PortMappingExternalIp = "198.51.100.7";
      status.PortMappingError = "udp 443: 192.168.1.1 answered 718 ConflictInMappingEntry";
      const std::string line =
          PortMappingLine(PortMappingRowModelFor(ViewOf(status, true, true, true)));
      Check(!line.empty(), std::string("the line shows: ") + state);
      Check(line.find("192.168.1.1") == std::string::npos &&
                line.find("198.51.100.7") == std::string::npos &&
                line.find("ConflictInMappingEntry") == std::string::npos &&
                line.find("upnp") == std::string::npos,
            std::string("the line never carries the router's words or an address: ") + state);
    }
  }
  {
    TEST_CASE("theLineIsPartOfTheModel");
    const PortMappingRowModel failed =
        PortMappingRowModelFor(PortMappingReport(kExtenderPortMappingFailed));
    const PortMappingRowModel mapping =
        PortMappingRowModelFor(PortMappingReport(kExtenderPortMappingMapping));
    Check(failed != mapping, "a new line alone is a change");
    const PortMappingRowModel pcp = PortMappingRowModelFor(
        PortMappingReport(kExtenderPortMappingMapped, "PCP", "TCP 443"));
    const PortMappingRowModel upnp = PortMappingRowModelFor(
        PortMappingReport(kExtenderPortMappingMapped, "UPnP", "TCP 443"));
    const PortMappingRowModel morePorts = PortMappingRowModelFor(
        PortMappingReport(kExtenderPortMappingMapped, "PCP", "TCP 443, UDP 443"));
    Check(pcp != upnp, "so is a new method");
    Check(pcp != morePorts, "and new ports");
    Check(PortMappingRowModel{}.statusKey.empty(), "a default row has no line");
  }
}

// ---- ExtenderPresentation.h: the statistics sections (O8) --------------------

void StatsSectionsTests() {
  {
    TEST_CASE("everyCombination");
    // O8 as a table, not as the three expressions it is implemented with
    struct Row {
      int providing, stats, running;  // the inputs
      int provider, extender, meta;   // the provider rows, the extender group, the disabled meta
    };
    const Row table[] = {
        {0, 0, 0, 0, 0, 1},
        {0, 0, 1, 0, 0, 1},
        {0, 1, 0, 0, 0, 1},
        {0, 1, 1, 0, 0, 1},
        {1, 0, 0, 0, 0, 1},
        {1, 0, 1, 0, 0, 1},
        {1, 1, 0, 1, 0, 0},
        {1, 1, 1, 1, 1, 0},
    };
    for (const Row& row : table) {
      const auto sections = ExtenderStatsSectionsFor(row.providing != 0, row.stats != 0,
                                                     row.running != 0);
      const std::string at = " (providing=" + std::to_string(row.providing) +
                             " stats=" + std::to_string(row.stats) +
                             " running=" + std::to_string(row.running) + ")";
      Check(sections.providerVisible == (row.provider != 0), "the provider rows" + at);
      Check(sections.extenderVisible == (row.extender != 0), "the extender group" + at);
      Check(sections.disabledMeta == (row.meta != 0), "the disabled meta label" + at);
    }
  }
  {
    TEST_CASE("aRunningRoleNeverShowsTheSectionAlone");
    Check(!ExtenderStatsSectionsFor(false, true, true).extenderVisible, "provide mode never");
    Check(!ExtenderStatsSectionsFor(true, false, true).extenderVisible,
          "no provider stats reported yet");
    Check(!ExtenderStatsSectionsFor(true, true, false).extenderVisible, "the role is not running");
    Check(ExtenderStatsSectionsFor(true, true, true).extenderVisible, "all three hold");
  }
}

// ---- ExtenderPresentation.h: the bootstrap DNS-over-HTTPS servers ---------------

// A regional preset in the shape of the sdk's (connect RegionalControlDohUrls),
// as the box shows it after "Use China resolvers", on documentation addresses.
const std::vector<std::string> kPresetServers = {
    "https://192.0.2.53/dns-query",
    "https://192.0.2.54/dns-query",
    "https://198.51.100.53/dns-query",
    "https://203.0.113.53/dns-query",
};

void ControlDohTests() {
  {
    TEST_CASE("theBoxSplitsOnLineBreaksOnly");
    const std::string_view typed = "https://192.0.2.53/q\r\n  https://198.51.100.53/q \rhttps://203.0.113.4/a,b\n\n";
    const auto lines = ParseControlDohLines(typed);
    CheckEq(3, static_cast<long long>(lines.size()), "three servers");
    if (lines.size() == 3) {
      CheckEq("https://192.0.2.53/q", lines[0], "first, past its CRLF");
      CheckEq("https://198.51.100.53/q", lines[1], "second, trimmed, ended by a bare CR");
      CheckEq("https://203.0.113.4/a,b", lines[2],
              "a comma is part of a url, never a separator as in the hosts box");
    }
  }
  {
    TEST_CASE("theOrderAndTheRepeatsAreTheSdks");
    const auto lines = ParseControlDohLines(
        "https://192.0.2.54/dns-query\nhttps://192.0.2.53/dns-query\nhttps://192.0.2.54/dns-query");
    Check(lines == std::vector<std::string>{"https://192.0.2.54/dns-query",
                                            "https://192.0.2.53/dns-query",
                                            "https://192.0.2.54/dns-query"},
          "in the order typed, with the repeat left for the sdk to drop");
  }
  {
    TEST_CASE("anEmptyBoxIsTheBuiltInServersAlone");
    Check(ParseControlDohLines("").empty(), "nothing typed");
    Check(ParseControlDohLines(" \r\n\t\n\r").empty(), "nothing but whitespace and line breaks");
  }
  {
    TEST_CASE("theBoxShowsOneServerPerLine");
    CheckEq(R"(https://192.0.2.53/dns-query
https://192.0.2.54/dns-query
https://198.51.100.53/dns-query
https://203.0.113.53/dns-query)",
            ControlDohText(kPresetServers), "the preset, one per line, in the sdk's order");
    Check(ParseControlDohLines(ControlDohText(kPresetServers)) == kPresetServers,
          "what the box shows is what a save sends back");
    CheckEq("", ControlDohText({}), "no servers is an empty box, which means the built-in ones");
  }
  {
    TEST_CASE("everySdkErrorIdIsItsOwnStoreKey");
    // the sdk's literal ids (sdk control_doh_ui.go ControlDohError*)
    CheckEq("control_doh_error_url_invalid", kControlDohErrorUrlInvalid, "url invalid");
    CheckEq("control_doh_error_https_required", kControlDohErrorHttpsRequired, "https required");
    CheckEq("control_doh_error_ip_required", kControlDohErrorIpRequired, "ip required");
    CheckEq("control_doh_error_too_many", kControlDohErrorTooMany, "too many");
    for (const char* id : kControlDohErrorIds) {
      CheckEq(id, ControlDohErrorKey(id), std::string(id) + " is its own key");
      Check(EnglishStore().count(id) == 1, std::string("the store carries ") + id);
    }
  }
  {
    TEST_CASE("anyOtherIdIsSomethingWentWrong");
    // Loc() of a key the store does not carry shows the key itself, and none
    // of these is about the url the user typed
    CheckEq("something_went_wrong", ControlDohErrorKey(kSdkErrorIdInternal),
            "the call could not run (urnet::ErrorIdInternal)");
    CheckEq("something_went_wrong", ControlDohErrorKey("control_doh_error_from_a_newer_sdk"),
            "a newer sdk's id");
    CheckEq("something_went_wrong", ControlDohErrorKey("vless_error_link_invalid"),
            "another feature's id");
    CheckEq("something_went_wrong", ControlDohErrorKey(""), "no id at all");
    Check(EnglishStore().count("something_went_wrong") == 1,
          "the store carries something_went_wrong");
  }
  {
    TEST_CASE("onlyAnEmptyAnswerIsASave");
    const auto saved = ControlDohSaveOutcomeFor("");
    Check(saved.saved, "\"\" is saved");
    CheckEq("control_doh_urls_saved", saved.messageKey, "with the saved line");
    Check(!saved.nextConnectNote,
          "and no next-connect note: the running session takes the servers in place");
    for (const char* id : {kSdkErrorIdInternal, "control_doh_error_from_a_newer_sdk",
                           kControlDohErrorIpRequired, kControlDohErrorTooMany}) {
      const auto refused = ControlDohSaveOutcomeFor(id);
      Check(!refused.saved, std::string(id) + " is not a save");
      Check(!refused.nextConnectNote, std::string(id) + " promises nothing for the next connect");
      CheckEq(ControlDohErrorKey(id), refused.messageKey, std::string(id) + " says its own message");
    }
    CheckEq("something_went_wrong", ControlDohSaveOutcomeFor(kSdkErrorIdInternal).messageKey,
            "a save that could not run is something went wrong, never saved");
  }
  {
    TEST_CASE("theStoreHasEveryKeyTheBlockShows");
    for (const char* key :
         {"control_doh_urls", "control_doh_urls_description", "control_doh_urls_hint",
          "control_doh_use_china", "control_doh_use_china_hint", "control_doh_urls_reset",
          "control_doh_urls_saved", "control_doh_urls_next_connect", "save"}) {
      const auto found = EnglishStore().find(key);
      Check(found != EnglishStore().end(), std::string("the store carries ") + key);
      if (found == EnglishStore().end()) continue;
      Check(found->second.find('{') == std::string::npos,
            std::string(key) + " is whole text, with no placeholder");
    }
    const auto line = EnglishStore().find("import_extenders_control_doh_urls");
    Check(line != EnglishStore().end(), "the store carries import_extenders_control_doh_urls");
    if (line != EnglishStore().end()) {
      const std::size_t first = line->second.find("{}");
      Check(first != std::string::npos && line->second.find("{}", first + 2) == std::string::npos,
            "import_extenders_control_doh_urls holds exactly one {}");
    }
  }
  {
    TEST_CASE("aSettingsBlockWithServersNamesThemBeforeTheToggle");
    auto decoded = GoodDecode();
    decoded.hasSettings = true;
    decoded.settingsHost = "bringyour.com";
    decoded.controlDohUrls = {"https://192.0.2.53/dns-query", "https://198.51.100.53/dns-query"};
    const std::string servers = "https://192.0.2.53/dns-query, https://198.51.100.53/dns-query";
    for (const bool useSettings : {false, true}) {
      const auto decision = DecideExtenderImport(decoded, useSettings);
      CheckEq(servers, decision.controlDohUrlsArg,
              std::string("the servers, joined, with the toggle ") + (useSettings ? "on" : "off"));
      Check(decision.canImport, "and the import itself is unchanged");
    }
    const std::string shown = FilledFor("import_extenders_control_doh_urls",
                                        DecideExtenderImport(decoded, true).controlDohUrlsArg);
    Check(shown.find("They will see URnetwork's server lookups") != std::string::npos &&
              servers.size() < shown.size() &&
              shown.compare(shown.size() - servers.size(), servers.size(), servers) == 0,
          "the line says who will see the lookups and names them: " + shown);
  }
  {
    TEST_CASE("noServersInTheCodeSaysNothing");
    auto decoded = GoodDecode();
    decoded.hasSettings = true;
    CheckEq("", DecideExtenderImport(decoded, true).controlDohUrlsArg,
            "a block that names none leaves the importer's own servers alone");
    auto unsettled = GoodDecode();
    unsettled.controlDohUrls = kPresetServers;
    CheckEq("", DecideExtenderImport(unsettled, true).controlDohUrlsArg,
            "servers ride only in a settings block, and only it can apply them");
    ExtenderShareDecodeView bad;
    bad.hasSettings = true;
    bad.controlDohUrls = kPresetServers;
    CheckEq("", DecideExtenderImport(bad, true).controlDohUrlsArg, "a failed decode names nothing");
  }
}

}  // namespace

int main() {
  std::cout << "ExtenderRingGeometry colours\n";
  ColorTests();
  std::cout << "ExtenderRingGeometry pairing\n";
  PairingTests();
  std::cout << "ExtenderRingGeometry K2\n";
  RingGeometryTests();
  std::cout << "ExtenderRingGeometry fit and scale\n";
  RingFitTests();
  std::cout << "gossip state\n";
  GossipStateTests();
  std::cout << "extender panel model\n";
  PanelModelTests();
  std::cout << "extender settings form\n";
  SettingsFormTests();
  std::cout << "bootstrap DNS-over-HTTPS servers\n";
  ControlDohTests();
  std::cout << "share payload\n";
  ShareTests();
  std::cout << "share QR layout\n";
  QrLayoutTests();
  std::cout << "share QR runs\n";
  QrRunTests();
  std::cout << "vendored qrcodegen\n";
  QrEncoderTests();
  std::cout << "import decisions\n";
  ImportTests();
  std::cout << "provider extender row\n";
  ProvideRowTests();
  std::cout << "provider extender status view\n";
  ProvideStatusViewTests();
  std::cout << "provider extender switch guess\n";
  ProvideGuessTests();
  std::cout << "provider extender switch write\n";
  ProvideWriteRouteTests();
  std::cout << "lag.dev speed tests switch\n";
  SpeedTestsRowTests();
  std::cout << "lag.dev speed tests switch write\n";
  SpeedTestsWriteRouteTests();
  std::cout << "router port mapping switch\n";
  PortMappingRowTests();
  std::cout << "router port mapping switch write\n";
  PortMappingWriteRouteTests();
  std::cout << "router port mapping status line\n";
  PortMappingStatusLineTests();
  std::cout << "statistics sections\n";
  StatsSectionsTests();

  std::cout << "\n" << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
