// Executable spec for the bundled release note (App/ReleaseNotes.h): the shape
// the localization store generates, the one locale of it shown for the UI
// language by the rule every app follows (the store's README, "Release notes"),
// the direction it reads in, its day, and every way a file is no note at all.
// Run against the same header the app compiles, on any host with a C++20
// compiler and nlohmann/json.
//
//   c++ -std=c++20 -I ../src/App -I <nlohmann include dir> release-notes-tests.cpp -o /tmp/release-notes-tests && /tmp/release-notes-tests ../src/App/Assets/ReleaseNotes.json
//
// The last case reads the asset the app ships (the path given as the first
// argument).
//
// SPDX-License-Identifier: MPL-2.0

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ReleaseNotes.h"

using namespace urnw::releasenotes;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << std::endl;  // flushed: a spec that dies a moment later still says what failed
  }
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// a note as the store writes it: en first, every field present
constexpr std::string_view kNote = R"({
  "version": "2026.11",
  "date": "2026-11-19",
  "locales": {
    "en": {
      "title": "What's new",
      "intro": "This release is about speed.",
      "sections": [
        {"heading": "Faster", "items": ["Connects sooner", "Quicker lists"]},
        {"items": ["Fixes"]}
      ]
    },
    "pt": {"title": "Novidades", "sections": []},
    "pt-BR": {"title": "Novidades no Brasil", "sections": []},
    "zh": {"title": "新功能", "sections": []},
    "zh-HK": {"title": "新功能 HK", "sections": []},
    "es-419": {"title": "Novedades LatAm", "sections": []}
  }
})";

std::string LocaleFor(std::string_view language) {
  const auto note = Parse(kNote, language);
  return note ? note->locale : std::string("<none>");
}

void TestTheStoresShapeReads() {
  const auto note = Parse(kNote, "en-US");
  Check(note.has_value(), "the store's shape parses");
  if (!note) return;
  Check(note->version == "2026.11", "version");
  Check(note->date == "2026-11-19", "date");
  Check(note->locale == "en", "en-US reads en");
  Check(note->title == "What's new", "title");
  Check(note->intro == "This release is about speed.", "intro");
  Check(note->sections.size() == 2, "two sections");
  if (note->sections.size() != 2) return;
  Check(note->sections[0].heading == "Faster", "the first section's heading");
  Check(note->sections[0].items == std::vector<std::string>{"Connects sooner", "Quicker lists"},
        "the first section's bullets, in order");
  Check(note->sections[1].heading.empty(), "a section without a heading has none");
  Check(note->sections[1].items == std::vector<std::string>{"Fixes"}, "the second section's bullet");

  const auto bare = Parse(R"({"version":"2026.11","date":"2026-11-19","locales":{"en":{"title":"What's new","sections":[]}}})", "en");
  Check(bare.has_value(), "a note with no intro and no sections parses");
  if (bare) {
    Check(bare->intro.empty(), "no intro reads as none");
    Check(bare->sections.empty(), "no sections");
  }
  const auto noSections = Parse(R"({"locales":{"en":{"title":"What's new"}}})", "en");
  Check(noSections.has_value() && noSections->sections.empty(), "a note without a sections key parses, with none");
  Check(noSections.has_value() && noSections->version.empty() && noSections->date.empty(),
        "a note without a version or a date reads them as empty");
  const auto utf8 = Parse(kNote, "zh-CN");
  Check(utf8.has_value() && utf8->title == "新功能", "UTF-8 text survives");
}

void TestTheUiLanguagePicksTheLocale() {
  // that exact store locale
  Check(LocaleFor("pt-BR") == "pt-BR", "the tag itself first");
  Check(LocaleFor("pt") == "pt", "a bare language itself");
  Check(LocaleFor("es-419") == "es-419", "a region of digits itself");
  Check(LocaleFor("zh-HK") == "zh-HK", "zh-HK itself");
  Check(LocaleFor("zh") == "zh", "zh itself");
  // the same language in another form: the bare language first, then another region
  Check(LocaleFor("pt-PT") == "pt", "pt-PT: the bare pt before pt-BR");
  Check(LocaleFor("es-MX") == "es-419", "es-MX: no es-MX and no es, so another region of es");
  Check(LocaleFor("es") == "es-419", "a bare es the note does not carry: another region of es");
  Check(PickLocale({"en", "pt-BR"}, "pt-PT") == "pt-BR", "pt-PT -> pt-BR when the note has no pt");
  Check(PickLocale({"pt-BR", "pt"}, "pt-PT") == "pt", "the bare language first, wherever it is listed");
  Check(PickLocale({"en", "pt-BR", "pt-PT"}, "pt-AO") == "pt-BR", "another region: the first listed");
  Check(PickLocale({"en", "pt-PT", "pt-BR"}, "pt-AO") == "pt-PT", "another region: the first listed, either way");
  Check(PickLocale({"en", "es-419"}, "pt-BR") == "en", "another region of another language is not one");
  // never across Chinese scripts: zh is Simplified, zh-HK Traditional
  Check(LocaleFor("zh-Hans") == "zh", "zh-Hans (the app's zh resw): zh");
  Check(LocaleFor("zh-CN") == "zh", "zh-CN: zh");
  Check(LocaleFor("zh-Hans-CN") == "zh", "zh-Hans-CN: zh");
  Check(LocaleFor("zh-SG") == "zh", "zh-SG: zh");
  Check(LocaleFor("zh-TW") == "zh-HK", "zh-TW: zh-HK");
  Check(LocaleFor("zh-Hant") == "zh-HK", "zh-Hant: zh-HK");
  Check(LocaleFor("zh-Hant-TW") == "zh-HK", "zh-Hant-TW: zh-HK");
  Check(LocaleFor("zh-MO") == "zh-HK", "zh-MO: zh-HK");
  Check(LocaleFor("zh-Hans-HK") == "zh", "an explicit script decides over the region");
  Check(PickLocale({"en", "zh-HK"}, "zh-CN") == "en", "Simplified never reads the Traditional note");
  Check(PickLocale({"en", "zh"}, "zh-TW") == "en", "Traditional never reads the Simplified note");
  Check(PickLocale({"en", "zh-HK"}, "zh") == "en", "a bare zh is Simplified");
  // else en
  Check(LocaleFor("ja-JP") == "en", "a language the note does not carry: en");
  Check(LocaleFor("") == "en", "no language: en");
  Check(LocaleFor("e") == "en", "a prefix of a locale is not that locale");
  // the tag as Windows may spell it
  Check(LocaleFor("PT-br") == "pt-BR", "whatever the case");
  Check(LocaleFor("pt_BR") == "pt-BR", "an underscore reads as a hyphen");
  Check(LocaleFor("pt-BR;en-US") == "pt-BR", "of a list, the first");
  Check(LocaleFor("ja-JP;pt-BR") == "en", "of a list, only the first: the UI is in that one");

  Check(PickLocale({"en", "fr"}, "fr-CA") == "fr", "PickLocale: the bare language");
  Check(PickLocale({"fr"}, "de-DE").empty(), "PickLocale: neither the language nor en");
  Check(PickLocale({}, "en").empty(), "PickLocale: no locales");
}

void TestTheTagSplits() {
  using urnw::releasenotes::detail::Split;
  const auto traditional = Split("zh-Hant-TW");
  Check(traditional.language == "zh" && traditional.script == "hant" && traditional.region == "tw",
        "zh-Hant-TW: language, script, region");
  const auto latin = Split("es-419");
  Check(latin.language == "es" && latin.script.empty() && latin.region == "419", "es-419: a region of digits");
  const auto upper = Split("EN_us");
  Check(upper.language == "en" && upper.region == "us", "EN_us: lowercase, either separator");
  const auto variant = Split("de-DE-1996");
  Check(variant.language == "de" && variant.region == "de" && variant.script.empty(), "a variant is dropped");
  const auto bare = Split("he");
  Check(bare.language == "he" && bare.script.empty() && bare.region.empty(), "a bare language");
  Check(Split("").language.empty(), "an empty tag");
}

void TestTheNoteReadsInItsOwnDirection() {
  Check(IsRightToLeft("ar"), "Arabic reads right to left");
  Check(IsRightToLeft("he"), "Hebrew reads right to left");
  Check(IsRightToLeft("AR-sa"), "whatever the case and region");
  Check(IsRightToLeft("fa") && IsRightToLeft("ur"), "the other right-to-left languages");
  Check(!IsRightToLeft("en"), "English reads left to right");
  Check(!IsRightToLeft("pt-BR") && !IsRightToLeft("zh-HK") && !IsRightToLeft("hi"), "and the rest");
  Check(!IsRightToLeft(""), "no locale: left to right");
  // the English fallback in an Arabic or Hebrew UI is laid out as English
  const auto fallback = Parse(kNote, "ar-SA");
  Check(fallback.has_value() && fallback->locale == "en" && !IsRightToLeft(fallback->locale),
        "an Arabic UI reads the English fallback left to right");
}

void TestAFileThatIsNotTheShapeIsNoNote() {
  Check(!Parse("", "en"), "an empty file");
  Check(!Parse("not json", "en"), "not JSON");
  Check(!Parse(R"({"locales":{"en":{"title":"What's new"}})", "en"), "truncated JSON");
  Check(!Parse("[]", "en"), "an array");
  Check(!Parse(R"("What's new")", "en"), "a string");
  Check(!Parse(R"({"version":"2026.11"})", "en"), "no locales");
  Check(!Parse(R"({"locales":[]})", "en"), "locales not an object");
  Check(!Parse(R"({"locales":{}})", "en"), "no locale at all");
  Check(!Parse(R"({"locales":{"fr":{"title":"Nouveautés"}}})", "de"), "neither the UI language nor en");
  Check(!Parse(R"({"locales":{"en":"What's new"}})", "en"), "a locale that is not an object");
  Check(!Parse(R"({"locales":{"en":{}}})", "en"), "no title");
  Check(!Parse(R"({"locales":{"en":{"title":""}}})", "en"), "an empty title");
  Check(!Parse(R"({"locales":{"en":{"title":7}}})", "en"), "a title that is not a string");
  Check(!Parse(R"({"locales":{"en":{"title":null}}})", "en"), "a null title");

  // a locale that is not the shape is not offered: the next fit is
  const auto skipped = Parse(R"({"locales":{"en":{"title":"What's new"},"pt-BR":"Novidades"}})", "pt-BR");
  Check(skipped.has_value() && skipped->locale == "en", "a locale that is not an object falls back to en");
}

void TestWithinANoteTheMistypedReadsAsEmpty() {
  const auto note = Parse(R"({
    "version": 2026, "date": null,
    "locales": {"en": {
      "title": "What's new", "intro": ["not", "a", "string"],
      "sections": [
        "not a section",
        {"heading": 7, "items": ["kept", 3, null, "", {"a": 1}, "also kept"]},
        {"heading": "Only a heading", "items": []},
        {"heading": "Items not a list", "items": "one"},
        {"heading": "No items"}
      ]
    }}
  })", "en");
  Check(note.has_value(), "a note with mistyped fields still parses");
  if (!note) return;
  Check(note->version.empty(), "a version that is not a string reads as empty");
  Check(note->date.empty(), "a null date reads as empty");
  Check(note->intro.empty(), "an intro that is not a string reads as empty");
  Check(note->sections.size() == 1, "a section that is not an object, or has no bullet, is skipped");
  if (note->sections.size() != 1) return;
  Check(note->sections[0].heading.empty(), "a heading that is not a string reads as none");
  Check(note->sections[0].items == std::vector<std::string>{"kept", "also kept"},
        "bullets that are not strings, or are empty, are skipped");

  const auto sectionsMistyped = Parse(R"({"locales":{"en":{"title":"What's new","sections":{"items":["x"]}}}})", "en");
  Check(sectionsMistyped.has_value() && sectionsMistyped->sections.empty(), "sections that are not a list: none");
}

void TestTheDateReadsAsADay() {
  const auto day = ParseDay("2026-11-19");
  Check(day.has_value() && day->year == 2026 && day->month == 11 && day->day == 19, "YYYY-MM-DD");
  const auto first = ParseDay("2027-01-01");
  Check(first.has_value() && first->year == 2027 && first->month == 1 && first->day == 1, "leading zeros");
  Check(!ParseDay(""), "empty");
  Check(!ParseDay("2026-11-1"), "a one-digit day");
  Check(!ParseDay("2026/11/19"), "slashes");
  Check(!ParseDay("2026-11-19T00:00:00Z"), "a timestamp");
  Check(!ParseDay("20261119xx"), "no hyphens");
  Check(!ParseDay("2026-1a-19"), "a letter");
  Check(!ParseDay("2026-00-19"), "month zero");
  Check(!ParseDay("2026-13-19"), "month thirteen");
  Check(!ParseDay("2026-11-00"), "day zero");
  Check(!ParseDay("2026-11-32"), "day thirty-two");
  Check(!ParseDay("0000-11-19"), "year zero");
  // a day the calendar does not have
  Check(!ParseDay("2026-11-31"), "November 31");
  Check(!ParseDay("2026-02-29"), "February 29 of a common year");
  Check(ParseDay("2024-02-29").has_value(), "February 29 of a leap year");
  Check(!ParseDay("1900-02-29"), "February 29 of a century that is not a leap year");
  Check(ParseDay("2000-02-29").has_value(), "February 29 of a century that is one");
  Check(DaysInMonth(2026, 1) == 31 && DaysInMonth(2026, 4) == 30 && DaysInMonth(2026, 2) == 28 &&
            DaysInMonth(2028, 2) == 29 && DaysInMonth(2026, 12) == 31,
        "the days of a month");
  static_assert(ParseDay("2026-11-19").has_value(), "ParseDay is constexpr");
}

// The formatter is handed midnight UTC of the day and formats it in UTC, so the
// day's count from the epoch must be the calendar's own (python3 date arithmetic).
void TestTheDayCountsFromTheEpoch() {
  static_assert(EpochDays(Day{1970, 1, 1}) == 0, "the epoch");
  static_assert(EpochDays(Day{1969, 12, 31}) == -1, "the day before");
  static_assert(EpochDays(Day{2000, 3, 1}) == 11017, "after a leap century's February");
  static_assert(EpochDays(Day{2024, 2, 29}) == 19782, "a leap day");
  static_assert(EpochDays(Day{2026, 11, 19}) == 20776, "the shipped note's day");
  static_assert(EpochDays(Day{1601, 1, 1}) == -134774, "the start of the Windows epoch");
  static_assert(EpochDays(Day{9999, 12, 31}) == 2932896, "the last four-digit day");
  // consecutive days are one apart across every month and year boundary of a span
  bool consecutive = true;
  std::int64_t previous = EpochDays(Day{2023, 12, 31});
  for (int year = 2024; year <= 2029; ++year) {
    for (int month = 1; month <= 12; ++month) {
      for (int day = 1; day <= DaysInMonth(year, month); ++day) {
        const std::int64_t count = EpochDays(Day{year, month, day});
        consecutive = consecutive && count == previous + 1;
        previous = count;
      }
    }
  }
  Check(consecutive, "every day of 2024 to 2029 is one after the day before");
}

void TestTheShippedAssetReads(const std::string& path) {
  const std::string body = ReadFile(path);
  Check(!body.empty(), "read " + path);
  const auto note = Parse(body, "en");
  Check(note.has_value(), "the shipped asset parses for en");
  if (!note) return;
  Check(note->locale == "en", "the shipped asset carries en");
  Check(!note->title.empty(), "the shipped en note has a title");
  Check(!note->version.empty(), "the shipped asset names its version");
  Check(ParseDay(note->date).has_value(), "the shipped asset's date is a YYYY-MM-DD day");
  // every locale the asset carries is the note for its own tag
  const nlohmann::json document = nlohmann::json::parse(body, nullptr, false);
  const auto locales = document.find("locales");
  Check(locales != document.end() && locales->is_object(), "the shipped asset's locales are an object");
  if (locales == document.end() || !locales->is_object()) return;
  for (auto it = locales->begin(); it != locales->end(); ++it) {
    const auto localized = Parse(body, it.key());
    Check(localized.has_value() && localized->locale == it.key() && !localized->title.empty(),
          "the shipped asset's " + it.key() + " note reads for " + it.key());
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string asset = 1 < argc ? argv[1] : std::string("../src/App/Assets/ReleaseNotes.json");
  TestTheStoresShapeReads();
  TestTheUiLanguagePicksTheLocale();
  TestTheTagSplits();
  TestTheNoteReadsInItsOwnDirection();
  TestAFileThatIsNotTheShapeIsNoNote();
  TestWithinANoteTheMistypedReadsAsEmpty();
  TestTheDateReadsAsADay();
  TestTheDayCountsFromTheEpoch();
  TestTheShippedAssetReads(asset);
  std::cout << (gCases - gFailures) << "/" << gCases << " release notes checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
