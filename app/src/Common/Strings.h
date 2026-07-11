// UTF-8 <-> UTF-16 conversion. The SDK C ABI is UTF-8 (char*); the Win32 and
// WinRT APIs are UTF-16 (wchar_t). Everything crossing that boundary goes
// through here.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace urnw {

std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view utf16);

}  // namespace urnw
