// UI strings, from the shared localization store (@urnetwork/localizations).
//
// Strings/<locale>/Resources.resw is GENERATED -- edit the store, not the resw:
//   cd urnetwork/localizations && npm run gen
//
// Keys are the store's snake_case ids ("host_count", "connect", ...). MRT picks
// the resw for the user's language automatically, falling back to en.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace urnw {

// The localized string for `key`, or `key` itself if it is missing (so a typo is
// visible rather than silent).
std::wstring Localized(std::string_view key);

// std::format against the localized string. Placeholders in the store lower to
// "{}" for a single argument and "{0}"/"{1}"... when there are several, so that
// a translation may reorder them.
//
//   Format("connect_to_location", location)  ->  L"Connect to Berlin"
template <typename... Args>
std::wstring Format(std::string_view key, const Args&... args) {
  return std::vformat(Localized(key), std::make_wformat_args(args...));
}

// The plural form of `key` for `count`, selected with the CLDR rule for the
// current language, then formatted with `count`.
//
//   Plural("host_count", 1)  ->  L"1 host"
//   Plural("host_count", 4)  ->  L"4 hosts"
std::wstring Plural(std::string_view key, int64_t count);

}  // namespace urnw
