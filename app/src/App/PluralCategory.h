// The CLDR plural category of an integer count (Localization.cpp's Plural and
// PluralFormat). Pure and portable, no Windows header, so
// tools/plural-category-tests.cpp runs it on any host.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace urnw {

// The CLDR cardinal category of a count, for the languages the store ships:
// Plural() picks a .resw entry "<id>.<category>" with it. Keep in step with
// CLDR_CATEGORIES in localizations/gen/store.mjs. `lang` is a BCP-47 tag (MRT's
// language, "pt-BR"), matched on its leading language.
inline std::string_view PluralCategory(std::string_view lang, int64_t n) {
  const auto is = [&](std::string_view p) {
    return lang.size() >= p.size() && lang.compare(0, p.size(), p) == 0;
  };
  const int64_t i = n < 0 ? -n : n;
  const int64_t mod10 = i % 10, mod100 = i % 100;

  // one category only
  if (is("ja") || is("ko") || is("zh") || is("th") || is("vi") || is("id"))
    return "other";

  if (is("ar")) {
    if (i == 0) return "zero";
    if (i == 1) return "one";
    if (i == 2) return "two";
    if (mod100 >= 3 && mod100 <= 10) return "few";
    if (mod100 >= 11) return "many";
    return "other";
  }
  if (is("he")) {
    if (i == 1) return "one";
    if (i == 2) return "two";
    if (mod10 == 0 && i != 0) return "many";
    return "other";
  }
  if (is("ru") || is("uk")) {
    if (mod10 == 1 && mod100 != 11) return "one";
    if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return "few";
    return "many";
  }
  if (is("pl")) {
    if (i == 1) return "one";
    if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return "few";
    return "many";
  }
  if (is("cs")) {
    if (i == 1) return "one";
    if (i >= 2 && i <= 4) return "few";
    return "other";
  }
  // CLDR: one is 0 and 1 (Persian's "i = 0 or n = 1", French's "i = 0,1")
  if (is("fr") || is("pt") || is("fa")) return i > 1 ? "other" : "one";

  // en, de, el, es, it, nl, sv, sw, hi, tr, ...
  return i == 1 ? "one" : "other";
}

}  // namespace urnw
