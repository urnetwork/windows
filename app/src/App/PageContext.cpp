// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "PageContext.h"

#include "AppController.h"
#include "../Common/Strings.h"

namespace urnw::pages {

SdkHost& Sdk() { return urnw::App().sdk(); }
SubscriptionBalanceStore& Balance() { return urnw::App().balance(); }

// The miss test IS the equality: Localized() returns the key id itself when the
// store has no such key, so "the lookup gave me back what I asked for" means
// "not translated yet". Plural() has used the same test since it was written.
winrt::hstring Adv(std::string_view key, const wchar_t* english) {
  std::wstring value = urnw::Localized(key);
  if (value == urnw::Widen(key)) return winrt::hstring{english};
  return winrt::hstring{value};
}

std::wstring AdvW(std::string_view key, const wchar_t* english) {
  return std::wstring{Adv(key, english).c_str()};
}

std::string TrimWhitespace(std::string const& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

bool LooksLikeUserAuth(std::string const& value) {
  if (value.find('@') != std::string::npos) return value.size() >= 3;
  size_t digits = 0;
  for (char c : value) {
    if (c >= '0' && c <= '9') ++digits;
  }
  return digits >= 7;
}

}  // namespace urnw::pages
