// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "PageContext.h"

#include "AppController.h"

namespace urnw::pages {

SdkHost& Sdk() { return urnw::App().sdk(); }
SubscriptionBalanceStore& Balance() { return urnw::App().balance(); }

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
