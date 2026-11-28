// Executable spec for App/PluralCategory.h: the CLDR category Plural() selects
// a .resw entry with, for the languages the localizations store ships. Persian
// counts 0 and 1 as one, as French does; Turkish counts only 1 as one. Runs on
// any host with a C++20 compiler:
//
//   c++ -std=c++20 -I ../src/App plural-category-tests.cpp -o /tmp/plural-category-tests && /tmp/plural-category-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include "PluralCategory.h"

namespace {

int gFailures = 0;
int gCases = 0;

void Expect(std::string_view lang, int64_t n, std::string_view want) {
  ++gCases;
  const std::string_view got = urnw::PluralCategory(lang, n);
  if (got != want) {
    ++gFailures;
    std::cout << "  FAIL " << lang << " " << n << ": " << got << ", want " << want << "\n";
  }
}

}  // namespace

int main() {
  // Persian (CLDR: one is i = 0 or n = 1)
  for (const char* lang : {"fa", "fa-IR"}) {
    Expect(lang, 0, "one");
    Expect(lang, 1, "one");
    Expect(lang, 2, "other");
    Expect(lang, 5, "other");
    Expect(lang, 100, "other");
  }
  // Turkish (CLDR: one is n = 1)
  for (const char* lang : {"tr", "tr-TR"}) {
    Expect(lang, 0, "other");
    Expect(lang, 1, "one");
    Expect(lang, 2, "other");
    Expect(lang, 21, "other");
  }
  // the other languages keep their rules
  Expect("fr", 0, "one");
  Expect("fr", 2, "other");
  Expect("pt-BR", 1, "one");
  Expect("en", 0, "other");
  Expect("en", 1, "one");
  Expect("ar", 0, "zero");
  Expect("ar", 2, "two");
  Expect("ar", 3, "few");
  Expect("ar", 11, "many");
  Expect("ar", 100, "other");
  Expect("he", 2, "two");
  Expect("he", 20, "many");
  Expect("ru", 21, "one");
  Expect("ru", 3, "few");
  Expect("ru", 11, "many");
  Expect("pl", 22, "few");
  Expect("pl", 12, "many");
  Expect("cs", 3, "few");
  Expect("cs", 5, "other");
  Expect("ja", 1, "other");
  Expect("vi", 1, "other");

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
