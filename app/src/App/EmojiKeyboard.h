// The points leaderboard's emoji tag editor, the part that is pure logic:
// the emoji-only keyboard's contents, the grapheme walk behind its backspace,
// and the small rules the editor and the infinitely scrolling list follow.
// Header-only and free of any UI toolkit, so the Windows and Linux apps share
// it byte for byte and the same file unit-tests on either. The tag RULES
// (what counts as one emoji, the 1-6 limit, the random suggestion) live in the
// SDK (validateEmojiTag / suggestEmojiTag); nothing here decides validity.
//
// The keyboard is a curated grid, not a full Unicode picker: every entry is a
// single code point with default emoji presentation (no VS16, no flags, no
// skin tones, no ZWJ sequences), so it renders as a coloured glyph on every
// system font the two apps ship on. The list is spelled as code points rather
// than UTF-8 literals so the source stays ASCII (MSVC does not read UTF-8
// narrow literals without a flag; the Linux toolchain does not need one).
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace urnw::emoji {

// One group of the keyboard: the glyph that names it in the group strip, and
// its emoji in the order they are laid out.
struct Group {
  char32_t icon;
  std::vector<char32_t> emoji;
};

inline std::string EncodeUtf8(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

// UTF-8 -> code points. Malformed bytes decode as U+FFFD one byte at a time,
// so a bad string can never make the walk below run past the end.
inline std::vector<char32_t> DecodeUtf8(const std::string& s) {
  std::vector<char32_t> out;
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n = 0;
    char32_t cp = 0;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      n = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      n = 4;
    } else {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    if (i + n > s.size()) {
      out.push_back(0xFFFD);
      break;
    }
    bool ok = true;
    for (size_t k = 1; k < n; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) {
      out.push_back(0xFFFD);
      ++i;
      continue;
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}

inline bool IsRegionalIndicator(char32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
inline bool IsSkinTone(char32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; }
inline bool IsTagChar(char32_t cp) { return cp >= 0xE0020 && cp <= 0xE007F; }
inline bool IsVariationSelector(char32_t cp) { return cp == 0xFE0E || cp == 0xFE0F; }
inline constexpr char32_t kZwj = 0x200D;
inline constexpr char32_t kKeycap = 0x20E3;

// The tag as the emoji it is made of, one string per emoji: a skin-toned
// hand, a flag pair, a ZWJ family or a keycap is ONE element, never split
// inside. This is the walk the editor's backspace and its counter rely on,
// and it agrees with the SDK's segmentation for every tag the SDK accepts
// (the SDK is still the judge; this only finds the boundaries).
inline std::vector<std::string> SplitEmoji(const std::string& tag) {
  std::vector<std::string> out;
  const std::vector<char32_t> cps = DecodeUtf8(tag);
  std::string cluster;
  bool prevZwj = false;
  int regionalInCluster = 0;
  for (const char32_t cp : cps) {
    bool joins = false;
    if (cluster.empty()) {
      joins = true;  // the first code point opens the first cluster
    } else if (prevZwj || cp == kZwj || IsVariationSelector(cp) || IsSkinTone(cp) ||
               cp == kKeycap || IsTagChar(cp)) {
      joins = true;
    } else if (IsRegionalIndicator(cp) && regionalInCluster == 1) {
      joins = true;  // the second letter of a flag pair
    }
    if (!joins) {
      out.push_back(cluster);
      cluster.clear();
      regionalInCluster = 0;
    }
    cluster += EncodeUtf8(cp);
    if (IsRegionalIndicator(cp)) ++regionalInCluster;
    prevZwj = cp == kZwj;
  }
  if (!cluster.empty()) out.push_back(cluster);
  return out;
}

// The tag without its last emoji: the editor's backspace.
inline std::string DropLastEmoji(const std::string& tag) {
  std::vector<std::string> parts = SplitEmoji(tag);
  if (parts.empty()) return std::string();
  parts.pop_back();
  std::string out;
  for (const std::string& part : parts) out += part;
  return out;
}

inline std::string Join(const std::vector<std::string>& parts) {
  std::string out;
  for (const std::string& part : parts) out += part;
  return out;
}

// ---- the list ---------------------------------------------------------------

// rows from the end at which the next page is requested (the Android
// reference's PointsLeaderboardPaging.LOAD_MORE_THRESHOLD)
inline constexpr int64_t kLoadMoreThreshold = 10;

// True when the list has scrolled close enough to its end that the next page
// should be requested. `lastVisibleRow` indexes the ROWS (header and footer
// excluded); -1 when no row is visible. The controller itself refuses a
// second in-flight page and a page past the end; this only avoids asking.
inline bool ShouldLoadMore(int64_t lastVisibleRow, int64_t rowCount, bool loading,
                           bool endReached, int64_t threshold = kLoadMoreThreshold) {
  if (rowCount <= 0 || loading || endReached || lastVisibleRow < 0) return false;
  return lastVisibleRow >= rowCount - 1 - threshold;
}

// The last visible row of a list of fixed-height rows, from the scroll
// position: `offset` is the scrolled distance, `viewport` the visible height,
// `rowsTop` where the first row starts in the scrolled content (the header
// above the rows), `rowHeight` the height every row has.
inline int64_t LastVisibleRow(double offset, double viewport, double rowsTop, double rowHeight,
                              int64_t rowCount) {
  if (rowCount <= 0 || rowHeight <= 0) return -1;
  const double bottom = offset + viewport - rowsTop;
  if (bottom <= 0) return -1;
  int64_t row = static_cast<int64_t>(bottom / rowHeight);
  if (row >= rowCount) row = rowCount - 1;
  return row;
}

// ---- the editor ---------------------------------------------------------------

// Why the SDK rejected a tag; mirrors the SDK's EmojiTagReason* strings.
enum class TagError { None, Empty, TooMany, NotEmoji };

inline TagError ErrorFor(bool ok, const std::string& reason) {
  if (ok) return TagError::None;
  if (reason == "empty") return TagError::Empty;
  if (reason == "too_many") return TagError::TooMany;
  // an unknown reason from a newer SDK still reads as "not emoji": the only
  // other way a tag is rejected
  return TagError::NotEmoji;
}

// Save is offered only for a valid tag that differs from what is stored. The
// SDK's normalized form is what gets sent, so the comparison is on it.
inline bool CanSave(bool ok, const std::string& normalized, const std::string& current,
                    bool saving) {
  return ok && !saving && !normalized.empty() && normalized != current;
}

// An empty draft is not an error while the user is still composing (or
// clearing): the counter reads "0 / max" instead of "add an emoji".
inline bool ShowsError(const std::string& draft, TagError error) {
  return error != TagError::None && !(draft.empty() && error == TagError::Empty);
}

// ---- the keyboard -------------------------------------------------------------

inline const std::vector<Group>& Groups() {
  static const std::vector<Group> groups = {
      // smileys and creatures
      {0x1F600,
       {0x1F600, 0x1F603, 0x1F604, 0x1F601, 0x1F606, 0x1F605, 0x1F602, 0x1F642, 0x1F609,
        0x1F60A, 0x1F60D, 0x1F618, 0x1F60B, 0x1F60E, 0x1F929, 0x1F973, 0x1F914, 0x1F917,
        0x1F607, 0x1F970, 0x1F634, 0x1F92F, 0x1F976, 0x1F975, 0x1F624, 0x1F62D, 0x1F920,
        0x1F916, 0x1F47D, 0x1F47B, 0x1F480, 0x1F921, 0x1F383, 0x1F4A9, 0x1F63A, 0x1F638,
        0x1F63B, 0x1F640, 0x1F648, 0x1F649, 0x1F64A}},
      // animals
      {0x1F43B,
       {0x1F436, 0x1F431, 0x1F42D, 0x1F439, 0x1F430, 0x1F98A, 0x1F43B, 0x1F43C, 0x1F428,
        0x1F42F, 0x1F981, 0x1F42E, 0x1F437, 0x1F438, 0x1F435, 0x1F414, 0x1F427, 0x1F426,
        0x1F424, 0x1F986, 0x1F985, 0x1F989, 0x1F987, 0x1F43A, 0x1F417, 0x1F434, 0x1F984,
        0x1F41D, 0x1F41B, 0x1F98B, 0x1F40C, 0x1F41E, 0x1F41C, 0x1F982, 0x1F422, 0x1F40D,
        0x1F98E, 0x1F419, 0x1F991, 0x1F990, 0x1F980, 0x1F421, 0x1F420, 0x1F41F, 0x1F42C,
        0x1F433, 0x1F40B, 0x1F988, 0x1F40A, 0x1F405, 0x1F406, 0x1F993, 0x1F98D, 0x1F418,
        0x1F98F, 0x1F42A, 0x1F992, 0x1F998, 0x1F403, 0x1F411, 0x1F410, 0x1F98C, 0x1F413,
        0x1F983, 0x1F99A, 0x1F99C, 0x1F9A2, 0x1F41A, 0x1F432}},
      // plants, weather and sky
      {0x1F338,
       {0x1F338, 0x1F33A, 0x1F33B, 0x1F339, 0x1F337, 0x1F33C, 0x1F331, 0x1F332, 0x1F333,
        0x1F334, 0x1F335, 0x1F33E, 0x1F340, 0x1F341, 0x1F342, 0x1F343, 0x1F344, 0x1F330,
        0x1F30D, 0x1F30E, 0x1F30F, 0x1F310, 0x1F30B, 0x1F5FB, 0x1F305, 0x1F304, 0x1F307,
        0x1F306, 0x1F309, 0x1F30C, 0x1F386, 0x1F387, 0x1F308, 0x1F31E, 0x1F31D, 0x1F31B,
        0x1F31C, 0x1F319, 0x2B50, 0x1F31F, 0x2728, 0x1F320, 0x26C5, 0x26C4, 0x1F525,
        0x1F4A7, 0x1F30A, 0x26A1, 0x1F4A5, 0x1F4AB}},
      // food and drink
      {0x1F34E,
       {0x1F34E, 0x1F350, 0x1F34A, 0x1F34B, 0x1F34C, 0x1F349, 0x1F347, 0x1F353, 0x1F352,
        0x1F351, 0x1F96D, 0x1F34D, 0x1F965, 0x1F95D, 0x1F345, 0x1F951, 0x1F346, 0x1F955,
        0x1F33D, 0x1F966, 0x1F95C, 0x1F35E, 0x1F950, 0x1F968, 0x1F9C0, 0x1F357, 0x1F354,
        0x1F35F, 0x1F355, 0x1F32D, 0x1F32E, 0x1F32F, 0x1F35C, 0x1F363, 0x1F364, 0x1F359,
        0x1F366, 0x1F369, 0x1F36A, 0x1F382, 0x1F370, 0x1F9C1, 0x1F36B, 0x1F36C, 0x1F36D,
        0x1F36F, 0x2615, 0x1F375, 0x1F9C3, 0x1F964}},
      // sports, games and celebration
      {0x26BD,
       {0x26BD, 0x1F3C0, 0x1F3C8, 0x26BE, 0x1F94E, 0x1F3BE, 0x1F3D0, 0x1F3C9, 0x1F3B1,
        0x1F3D3, 0x1F3F8, 0x1F94A, 0x1F94B, 0x1F3AF, 0x1F3B3, 0x1F3AE, 0x1F3B2, 0x1F9E9,
        0x1F3A8, 0x1F3AD, 0x1F3A4, 0x1F3A7, 0x1F3B8, 0x1F3B9, 0x1F3BA, 0x1F3BB, 0x1F941,
        0x1F3C6, 0x1F947, 0x1F948, 0x1F949, 0x1F3C5, 0x1F3AA, 0x1F3AB, 0x1F381, 0x1F388,
        0x1F389, 0x1F38A, 0x1F380, 0x1F3F9, 0x1F6F9, 0x1F6F7, 0x1F3C2, 0x1F3C4, 0x1F6B4,
        0x1F3CA, 0x1F93F, 0x1F94C}},
      // travel and places
      {0x1F680,
       {0x1F680, 0x1F6F8, 0x1F681, 0x1F6EB, 0x1F6EC, 0x1F682, 0x1F684, 0x1F686, 0x1F687,
        0x1F68C, 0x1F68E, 0x1F691, 0x1F692, 0x1F693, 0x1F695, 0x1F697, 0x1F699, 0x1F69A,
        0x1F69B, 0x1F69C, 0x1F6B2, 0x1F6F5, 0x1F6F4, 0x1F6A4, 0x26F5, 0x1F6A2, 0x2693,
        0x1F5FD, 0x1F5FC, 0x1F3F0, 0x1F3EF, 0x1F3A1, 0x1F3A2, 0x1F3A0, 0x1F9ED, 0x1F3E0,
        0x1F3E1, 0x1F3E2, 0x1F3ED, 0x1F3E5, 0x1F3EB, 0x1F3E6, 0x1F3E8, 0x26EA, 0x1F54C,
        0x26FA, 0x1F3EC, 0x1F3E9}},
      // objects
      {0x1F4A1,
       {0x1F4A1, 0x1F526, 0x1F50B, 0x1F50C, 0x1F4BB, 0x1F4F1, 0x231A, 0x1F4F7, 0x1F4F8,
        0x1F4F9, 0x1F3A5, 0x1F4FA, 0x1F4FB, 0x23F0, 0x23F3, 0x231B, 0x1F4E1, 0x1F52D,
        0x1F52C, 0x1F9EC, 0x1F9EA, 0x1F9F2, 0x1F527, 0x1F528, 0x1F529, 0x1F9F0, 0x1F511,
        0x1F512, 0x1F513, 0x1F510, 0x1F9F1, 0x1F48E, 0x1F4B0, 0x1F4B5, 0x1F4B3, 0x1F9FE,
        0x1F4E6, 0x1F4EB, 0x1F4EE, 0x1F4E7, 0x1F4DD, 0x1F4DA, 0x1F4D6, 0x1F393, 0x1F4CC,
        0x1F4CE, 0x1F517, 0x1F9F7, 0x1F9F5, 0x1F9F6, 0x1F392, 0x1F451, 0x1F3A9, 0x1F9E2,
        0x1F453, 0x1F97D, 0x1F9F3, 0x2614, 0x1F9F8, 0x1F4E3, 0x1F514, 0x1F4CD}},
      // hearts and symbols
      {0x1F49C,
       {0x1F49B, 0x1F49A, 0x1F499, 0x1F49C, 0x1F5A4, 0x1F496, 0x1F497, 0x1F493, 0x1F49E,
        0x1F495, 0x1F498, 0x1F49D, 0x1F49F, 0x1F4AF, 0x1F4A2, 0x1F4A6, 0x1F4A8, 0x1F4AC,
        0x1F4AD, 0x1F3B5, 0x1F3B6, 0x1F531, 0x2B55, 0x2705, 0x274C, 0x274E, 0x2795,
        0x2796, 0x2797, 0x1F4B2, 0x2753, 0x2754, 0x2755, 0x2757, 0x1F506, 0x1F505,
        0x1F197, 0x1F192, 0x1F195, 0x1F193, 0x1F199, 0x1F51D, 0x1F51B, 0x1F51C, 0x1F51A,
        0x1F519, 0x1F3C1, 0x1F6A9, 0x1F38C, 0x1F3F4, 0x1F534, 0x1F535, 0x26AB, 0x26AA,
        0x1F536, 0x1F537, 0x1F538, 0x1F539, 0x1F53A, 0x1F53B, 0x1F4A0, 0x1F518, 0x1F532,
        0x1F533, 0x2B1B, 0x2B1C, 0x1F503, 0x1F504, 0x267F, 0x1F6BB}},
  };
  return groups;
}

}  // namespace urnw::emoji
