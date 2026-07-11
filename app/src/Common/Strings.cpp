// SPDX-License-Identifier: MPL-2.0
#include "Strings.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace urnw {

std::wstring Widen(std::string_view utf8) {
  if (utf8.empty()) return {};
  int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                  static_cast<int>(utf8.size()), nullptr, 0);
  if (len <= 0) return {};
  std::wstring out(static_cast<size_t>(len), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), len);
  return out;
}

std::string Narrow(std::wstring_view utf16) {
  if (utf16.empty()) return {};
  int len = ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(),
                                  static_cast<int>(utf16.size()), nullptr, 0,
                                  nullptr, nullptr);
  if (len <= 0) return {};
  std::string out(static_cast<size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()),
                        out.data(), len, nullptr, nullptr);
  return out;
}

}  // namespace urnw
