// The small string formats the update checker must never get wrong, parsed
// pure.
//
// All of them sit on the trust boundary between "fetched from the release" and
// "about to touch the running installation", which is why they live here as
// header-only pure functions rather than inline in the checker: the service
// selftest exercises exactly the code the app will run, the same arrangement
// VersionGrammar.h already has for the tag grammar.
//
//   DigestHexFromAssetDigest
//                        extracts the expected hash from a GitHub release
//                        asset's `digest` field (`sha256:<64 hex>`). A value
//                        that "almost" matches — another algorithm, truncated
//                        hex — must come back empty, not close, because the
//                        caller compares it against a hash it computed itself
//                        and empty can never accidentally equal anything.
//   IsAllowedPayloadName the zip-slip defence's allowlist. Archive entry paths
//                        are NEVER trusted: after extraction the checker takes
//                        only top-level files whose bare names pass this test
//                        (the exe/dll/pri payload) and ignores everything else,
//                        so a hostile archive member named `..\evil.exe` or
//                        `C:\x.dll` cannot become a swap target even if the
//                        extractor were to misbehave.
//   IsStaleRenamedName   the startup cleanup's matcher for the swap's OWN
//                        leftovers (`<name>.old`, `<name>.old-<code>`). It
//                        gates a DeleteFile in the user's install folder — an
//                        ordinary folder they unzipped themselves — so a match
//                        that is merely close (`report.old-2024.xlsx`) is not
//                        hygiene, it is data loss.
//
// No Windows headers, no allocation beyond the returned string, total on all
// inputs.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace urnw::update {

namespace format_detail {

inline constexpr bool IsHexDigit(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

inline constexpr char AsciiLower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace format_detail

// True when the two strings are equal under ASCII case folding. Enough for the
// two uses here — hex digits and Windows file extensions — and deliberately not
// a general Unicode casefold, which neither needs.
inline constexpr bool EqualsAsciiCaseless(std::string_view a,
                                          std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (format_detail::AsciiLower(a[i]) != format_detail::AsciiLower(b[i]))
      return false;
  }
  return true;
}

// The lowercase SHA-256 hex out of a GitHub release asset's `digest` field,
// or empty when the value is not exactly `sha256:<64 hex chars>`. The releases
// API stamps every asset with the hash GitHub computed at upload time, so the
// checker verifies against the SAME JSON object whose browser_download_url it
// fetches — no second document to download, no filename lookup to misalign.
//
// Strict on the shape because empty means "this release cannot be verified,
// skip it": another algorithm (`sha512:`), a missing prefix, truncated or
// padded hex, and a non-hex character are all refusals, never best-effort. The
// HEX is canonicalized to lowercase so the caller can compare it against its
// own formatting without a second folding step; the PREFIX is matched exactly
// as the API mints it, lowercase.
inline std::string DigestHexFromAssetDigest(std::string_view digest) {
  constexpr std::string_view kPrefix = "sha256:";
  if (digest.size() != kPrefix.size() + 64) return {};
  if (digest.substr(0, kPrefix.size()) != kPrefix) return {};
  std::string hex;
  hex.reserve(64);
  for (const char c : digest.substr(kPrefix.size())) {
    if (!format_detail::IsHexDigit(c)) return {};
    hex.push_back(format_detail::AsciiLower(c));
  }
  return hex;
}

// Whether an extracted file's bare name is one the swap may touch: a single
// path component (no separators, no drive colon, not a dot name) whose
// extension is .exe, .dll or .pri — the payload classes the portable zip's
// root actually carries. Assets in subdirectories are outside the swap on
// purpose; see the checker for what that costs and why it is the right trade.
inline bool IsAllowedPayloadName(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") return false;
  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':') return false;
  }
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return false;  // no empty stem
  const std::string_view ext = name.substr(dot);
  return EqualsAsciiCaseless(ext, ".exe") || EqualsAsciiCaseless(ext, ".dll") ||
         EqualsAsciiCaseless(ext, ".pri");
}

// Whether a file name is one of the rename-swap's own leftovers, and nothing
// else: `<stem>.old`, or `<stem>.old-<digits>` (the code-suffixed fallback the
// swap parks a rename under when the plain .old name is still locked), with a
// non-empty stem and NOTHING after the match. The names are minted by the swap
// itself from `name + L".old"` and `std::to_wstring(code)`, so digits-to-the-
// end is the exact minted shape, not an approximation of it. Everything looser
// deletes user files: ".old-" anywhere in the middle admits `report.old-2024
// .xlsx`, and a bare substring test admits `URnetwork.exe.old-backup`.
inline constexpr bool IsStaleRenamedName(std::string_view name) noexcept {
  const std::size_t pos = name.rfind(".old");
  if (pos == std::string_view::npos || pos == 0) return false;  // no empty stem
  const std::string_view tail = name.substr(pos + 4);
  if (tail.empty()) return true;  // `<stem>.old`
  if (tail.size() < 2 || tail.front() != '-') return false;
  for (std::size_t i = 1; i < tail.size(); ++i) {
    if (tail[i] < '0' || tail[i] > '9') return false;
  }
  return true;  // `<stem>.old-<digits>`
}

}  // namespace urnw::update
