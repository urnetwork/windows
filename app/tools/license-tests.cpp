// Executable spec for the Licenses page's pure logic (App/LicensePresentation.h):
// which section an entry belongs to, the "version · spdx" line under its name,
// what counts as a notice, which project links may be opened, and how a text
// block is trimmed for display - run against the SAME header the app compiles,
// on any host with a C++20 compiler.
//
// The WinUI half (App/LicensesPage.cpp) cannot be built off Windows; what is
// verified here is every decision it makes before it touches a XAML object.
//
//   c++ -std=c++20 -I ../src/App license-tests.cpp -o /tmp/license-tests \
//       && /tmp/license-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>
#include <vector>

#include "LicensePresentation.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

void CheckEq(const std::string& expected, const std::string& actual, const std::string& what) {
  Check(expected == actual, what + ": expected \"" + expected + "\", got \"" + actual + "\"");
}

LicenseView Entry(std::string name, std::string kind, std::string version = {},
                  std::string spdx = {}, std::string notice = {}) {
  LicenseView view;
  view.name = std::move(name);
  view.kind = std::move(kind);
  view.version = std::move(version);
  view.spdx = std::move(spdx);
  view.notice = std::move(notice);
  return view;
}

}  // namespace

int main() {
  // ---- the secondary line ----
  CheckEq("v1.2.3 \u00B7 MIT", LicenseSecondaryLine("v1.2.3", "MIT"), "both halves");
  CheckEq("v1.2.3", LicenseSecondaryLine("v1.2.3", ""), "version only");
  CheckEq("Apache-2.0", LicenseSecondaryLine("", "Apache-2.0"), "spdx only");
  CheckEq("", LicenseSecondaryLine("", ""), "neither");
  CheckEq("MIT", LicenseSecondaryLine("  ", "MIT"), "a blank version is omitted");
  CheckEq("\xC2\xB7", std::string("\u00B7"), "the separator is UTF-8 U+00B7");

  // ---- notices ----
  Check(HasNotice(Entry("GeoLite2", "data", "", "", "This product includes GeoLite2 data")),
        "a notice is a notice");
  Check(!HasNotice(Entry("x", "software")), "empty is not a notice");
  Check(!HasNotice(Entry("x", "software", "", "", " \n\t")), "whitespace is not a notice");

  // ---- sections: data first, SDK order kept inside each ----
  const std::vector<LicenseView> entries = {
      Entry("GeoLite2 by MaxMind", "data"),
      Entry("zeta", "software"),
      Entry("Some Blocklist", "data"),
      Entry("Inter", "font"),
      Entry("alpha", "software"),
  };
  const LicenseSections sections = SplitLicenseSections(entries);
  Check(sections.data == std::vector<std::size_t>{0, 2}, "data section indices");
  Check(sections.software == std::vector<std::size_t>{1, 3, 4},
        "software section keeps the SDK order and takes fonts");
  Check(SplitLicenseSections({}).data.empty() && SplitLicenseSections({}).software.empty(),
        "an empty list has empty sections");
  Check(IsDataAttribution(entries[0]) && !IsDataAttribution(entries[3]), "kind data only");

  // ---- project links ----
  Check(IsOpenableLicenseUrl("https://github.com/golang/go"), "https opens");
  Check(IsOpenableLicenseUrl("HTTP://example.com"), "scheme is case-insensitive");
  Check(!IsOpenableLicenseUrl(""), "empty does not open");
  Check(!IsOpenableLicenseUrl("https://"), "a bare scheme does not open");
  Check(!IsOpenableLicenseUrl("file:///C:/Windows/System32/calc.exe"), "file: does not open");
  Check(!IsOpenableLicenseUrl("ms-settings:privacy"), "a shell protocol does not open");
  Check(!IsOpenableLicenseUrl("javascript:alert(1)"), "javascript: does not open");
  Check(!IsOpenableLicenseUrl("https://example.com/a b"), "whitespace does not open");

  // ---- block trimming ----
  CheckEq("MIT License\n\nCopyright", TrimLicenseBlock("MIT License\r\n\r\nCopyright\r\n\r\n"),
          "CRLF folded, trailing blank lines dropped");
  CheckEq("   Apache License", TrimLicenseBlock("   Apache License\n"),
          "leading indentation kept");
  CheckEq("", TrimLicenseBlock(" \n \n"), "all whitespace is empty");
  CheckEq("a\rb", TrimLicenseBlock("a\rb"), "a lone CR is left alone");

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " license-tests: " << gCases
            << " checks, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
