// Everything the Licenses page (LicensesPage.h) decides BEFORE it touches a
// XAML object: which section an entry belongs to, the secondary line under its
// name, whether it carries a notice, which project links may be opened, and how
// a text block is trimmed for display.
//
// Pure for the reason ExtenderPresentation.h gives: a WinUI 3 app cannot be
// built off Windows, so every decision that is a function of plain values is
// verified by tools/license-tests.cpp on any host with a C++20 compiler, and
// only the drawing is unverified. urnet::LicenseInfo is mirrored here as a
// plain view rather than included, so the tests need neither
// urnetwork_sdk.hpp nor the SDK dll; LicensesPage.cpp copies the fields across
// at its boundary.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace urnw {

// The SDK's LicenseKind tokens. Mirrors URNET_LICENSE_KIND_DATA so a build
// that has not seen the SDK header still agrees with it. "software" and "font"
// both read as open source software; only "data" is its own section.
inline constexpr const char* kLicenseKindData = "data";

// urnet::LicenseInfo, field for field (sdk/license.go).
struct LicenseView {
  std::string name;
  std::string version;
  std::string kind;
  std::string origin;
  std::string url;
  std::string spdx;
  std::string copyright;  // newline separated
  std::string notice;     // shown verbatim when non-empty
  std::string text;       // the full license text
};

// Data attributions ("GeoLite2 by MaxMind", ...) are their own section, above
// the software.
inline bool IsDataAttribution(LicenseView const& entry) {
  return entry.kind == kLicenseKindData;
}

inline bool IsBlank(std::string_view value) {
  return value.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

// A notice is what the entry's license REQUIRES the app to show (the MaxMind
// attribution on GeoLite2). Whitespace alone is not a notice.
inline bool HasNotice(LicenseView const& entry) { return !IsBlank(entry.notice); }

// The row's secondary line: "version · spdx", either half omitted when empty,
// and empty when both are. No label words, so nothing here needs a string.
inline std::string LicenseSecondaryLine(std::string_view version, std::string_view spdx) {
  std::string line;
  if (!IsBlank(version)) line.append(version);
  if (!IsBlank(spdx)) {
    if (!line.empty()) line.append(" · ");
    line.append(spdx);
  }
  return line;
}

inline std::string LicenseSecondaryLine(LicenseView const& entry) {
  return LicenseSecondaryLine(entry.version, entry.spdx);
}

// The two sections, as indices into the SDK's list. The SDK already returns
// data attributions first and software by name, and that ORDER is kept inside
// each section: the page never re-sorts, so it cannot disagree with the other
// apps about which entry comes first.
struct LicenseSections {
  std::vector<std::size_t> data;
  std::vector<std::size_t> software;
};

inline LicenseSections SplitLicenseSections(std::vector<LicenseView> const& entries) {
  LicenseSections sections;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    (IsDataAttribution(entries[i]) ? sections.data : sections.software).push_back(i);
  }
  return sections;
}

// The "Project page" link is only offered for a web URL. The value comes out of
// a generated file, and handing an arbitrary scheme to the shell launcher is
// not something a license list should be able to do.
inline bool IsOpenableLicenseUrl(std::string_view url) {
  auto startsWith = [url](std::string_view prefix) {
    if (url.size() <= prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
      const char c = url[i];
      const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
      if (lower != prefix[i]) return false;
    }
    return true;
  };
  if (url.find_first_of(" \t\r\n") != std::string_view::npos) return false;
  return startsWith("https://") || startsWith("http://");
}

// A block (copyright, notice, license text) as it is displayed: CRLF folded to
// LF and trailing whitespace dropped, so a text that ends in blank lines does
// not leave a band of empty space at the foot of the detail pane. Leading
// whitespace is kept - some license texts indent their title.
inline std::string TrimLicenseBlock(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\r' && i + 1 < value.size() && value[i + 1] == '\n') continue;
    out.push_back(value[i]);
  }
  const std::size_t end = out.find_last_not_of(" \t\r\n");
  out.erase(end == std::string::npos ? 0 : end + 1);
  return out;
}

}  // namespace urnw
