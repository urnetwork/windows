// How the redeem sheet answers a balance-code redeem, from the SDK's
// classification (urnet::classifyBalanceCodeRedeem, sdk payment_catalog.go).
//
// The server sends ONE payload ("Unknown balance code.") for a code that never
// existed and for a data code this network already redeemed, and a lost
// response looks like a transport failure even when the redeem committed. So a
// first classification that is not "redeemed" is classified again with the
// network's redeemed-code list before the sheet says anything: a retry after
// a lost-but-credited response reads "already redeemed", never "invalid"
// (UPGRADE.md D2). An answer with neither a transfer balance nor an error is
// "unknown" (the SDK's rule), the same as a transport failure.
//
// A gift code grants a year of Pro instead of data. Its redeem answer carries
// the year (RedeemBalanceCodeResult.pro) beside a 0-byte transfer balance, and
// its row in the redeemed-code list has kind "pro_gift": the redeem confirms
// "A year of Pro is yours." (gift_pro_redeemed), or the day a year queued after
// a paid Pro period starts (gift_pro_redeemed_queued), and the list says "A
// year of Pro" (balance_code_pro_year), never "+0 B". A gift code this network
// already holds, found in its list after a lost answer or a refusal, confirms
// as the gift too (BalanceCodeRedeemNoticeAfterList).
//
// Dependency-free so tools/balance-code-redeem-tests.cpp runs it without the SDK.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace urnw {

enum class BalanceCodeRedeemNotice {
  Redeemed,         // credited by this call: the success state
  GiftRedeemed,     // a gift code this network already holds: the gift's confirmation
  AlreadyRedeemed,  // this network already has it, or the server says it is redeemed
  Invalid,          // the server refused it and this network never redeemed it
  Unknown,          // no answer: the redeem may have committed, check the balance
};

// the SDK's BalanceCodeRedeemOutcome* values
inline BalanceCodeRedeemNotice BalanceCodeRedeemNoticeFor(const std::string& outcome) {
  if (outcome == "redeemed") return BalanceCodeRedeemNotice::Redeemed;
  if (outcome == "already_redeemed") return BalanceCodeRedeemNotice::AlreadyRedeemed;
  if (outcome == "invalid") return BalanceCodeRedeemNotice::Invalid;
  return BalanceCodeRedeemNotice::Unknown;
}

// Whether the sheet must read the redeemed-code list and classify again
// before it shows anything (every outcome but a credited redeem).
inline bool BalanceCodeRedeemNeedsCodeList(const std::string& outcome) {
  return BalanceCodeRedeemNoticeFor(outcome) != BalanceCodeRedeemNotice::Redeemed;
}

// A redeemed code's kind (RedeemedBalanceCode.kind) for a gift code's year of
// Pro (BalanceSheets.cpp asserts it is the sdk's). Any other kind is data, and
// so is a code without one, from a server that predates gift codes.
inline constexpr const char* kBalanceCodeKindProGift = "pro_gift";  // urnet::BalanceCodeKindProGift

inline bool IsProGiftCode(const std::string& kind) {
  return kind == kBalanceCodeKindProGift;
}

// ---- a typed code as the server reads it ----------------------------------------

namespace balance_code_detail {

// The code point of UTF-8 `text` at `at`, which advances past it; U+FFFD for a
// malformed sequence (as Go reads one: an overlong form, a surrogate or a point
// past U+10FFFF is malformed too, and only its first byte is taken), which no
// code has.
inline char32_t NextCodePoint(const std::string& text, size_t& at) {
  const auto byte = [&](size_t index) { return static_cast<unsigned char>(text[index]); };
  const unsigned char lead = byte(at);
  size_t length = 0;
  char32_t point = 0;
  if (lead < 0x80) {
    ++at;
    return lead;
  } else if ((lead & 0xE0) == 0xC0) {
    length = 2;
    point = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    length = 3;
    point = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    length = 4;
    point = lead & 0x07;
  } else {
    ++at;
    return 0xFFFD;
  }
  if (text.size() < at + length) {
    ++at;
    return 0xFFFD;
  }
  for (size_t index = 1; index < length; ++index) {
    if ((byte(at + index) & 0xC0) != 0x80) {
      ++at;
      return 0xFFFD;
    }
    point = (point << 6) | (byte(at + index) & 0x3F);
  }
  const char32_t least = length == 2 ? 0x80 : length == 3 ? 0x800 : 0x10000;
  if (point < least || (0xD800 <= point && point <= 0xDFFF) || 0x10FFFF < point) {
    ++at;
    return 0xFFFD;
  }
  at += length;
  return point;
}

inline void AppendUtf8(std::string& out, char32_t point) {
  if (point < 0x80) {
    out += static_cast<char>(point);
  } else if (point < 0x800) {
    out += static_cast<char>(0xC0 | (point >> 6));
    out += static_cast<char>(0x80 | (point & 0x3F));
  } else if (point < 0x10000) {
    out += static_cast<char>(0xE0 | (point >> 12));
    out += static_cast<char>(0x80 | ((point >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (point & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (point >> 18));
    out += static_cast<char>(0x80 | ((point >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((point >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (point & 0x3F));
  }
}

// Go's unicode.IsSpace
inline bool IsSpace(char32_t point) {
  return (0x09 <= point && point <= 0x0D) || point == 0x20 || point == 0x85 || point == 0xA0 ||
         point == 0x1680 || (0x2000 <= point && point <= 0x200A) || point == 0x2028 ||
         point == 0x2029 || point == 0x202F || point == 0x205F || point == 0x3000;
}

// Go's unicode.Pd, the dash punctuation (Unicode 17)
inline bool IsDash(char32_t point) {
  switch (point) {
    case 0x002D: case 0x058A: case 0x05BE: case 0x1400: case 0x1806: case 0x2E17:
    case 0x2E1A: case 0x2E3A: case 0x2E3B: case 0x2E40: case 0x2E5D: case 0x301C:
    case 0x3030: case 0x30A0: case 0xFE31: case 0xFE32: case 0xFE58: case 0xFE63:
    case 0xFF0D: case 0x10D6E: case 0x10EAD:
      return true;
    default:
      return 0x2010 <= point && point <= 0x2015;
  }
}

}  // namespace balance_code_detail

// A typed code as the server reads it (server model normalizeBalanceCodeSecret
// and its lookalikes; the sdk's normalizeBalanceCodeSecret, which the sdk does
// not export): without whitespace, dashes or dots, in upper case, and with 0,
// 1 and 8, which base32 never has, read as the O, I and B they pass for on a
// card. Only for finding a code in the network's redeemed list: a redeem sends
// the code as typed. Go's upper case turns no other non-ASCII letter into an
// ASCII one, and only ASCII can match a code, so the rest stay as typed.
inline std::string NormalizeBalanceCode(const std::string& secret) {
  using namespace balance_code_detail;
  std::string normalized;
  for (size_t at = 0; at < secret.size();) {
    char32_t point = NextCodePoint(secret, at);
    if (IsSpace(point) || IsDash(point) || point == U'.') continue;
    if (U'a' <= point && point <= U'z') {
      point = point - U'a' + U'A';
    } else if (point == 0x0131) {
      point = U'I';  // dotless i
    } else if (point == 0x017F) {
      point = U'S';  // long s
    }
    if (point == U'0') point = U'O';
    if (point == U'1') point = U'I';
    if (point == U'8') point = U'B';
    AppendUtf8(normalized, point);
  }
  return normalized;
}

// The kind of the code in `codes` (the network's redeemed list: each has a
// secret and a kind) that `secret`, as typed, names, or nothing when none does.
template <typename Codes>
std::optional<std::string> RedeemedCodeKind(const std::string& secret, const Codes& codes) {
  const std::string typed = NormalizeBalanceCode(secret);
  if (typed.empty()) return std::nullopt;
  for (const auto& code : codes) {
    if (NormalizeBalanceCode(code.secret) == typed) return code.kind;
  }
  return std::nullopt;
}

// Whether a refused redeem's message says the code is already redeemed: the
// server's "Balance code already redeemed." for a gift code, whoever redeemed
// it, read as the server's own subscription controller reads it.
inline bool RefusalSaysAlreadyRedeemed(const std::string& message) {
  std::string lower = message;
  for (char& c : lower) {
    if ('A' <= c && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return lower.find("already redeemed") != std::string::npos;
}

// What the sheet says for a redeem that did not credit on this call, once the
// network's redeemed-code list was read: `outcome` is the SDK's, classified
// with the list; `listedKind` is RedeemedCodeKind (nothing when the code is not
// in the list, or the list did not load); `refusalSaysRedeemed` is
// RefusalSaysAlreadyRedeemed of the server's refusal. A gift code in the list
// confirms as the gift (the list does not say whether its year was queued); a
// data code in the list keeps "already redeemed"; a code the server calls
// redeemed that is not in the list (another network has it) says "already
// redeemed" too, in the user's language; anything else is as before.
inline BalanceCodeRedeemNotice BalanceCodeRedeemNoticeAfterList(
    const std::string& outcome, const std::optional<std::string>& listedKind,
    bool refusalSaysRedeemed) {
  if (listedKind && IsProGiftCode(*listedKind)) return BalanceCodeRedeemNotice::GiftRedeemed;
  const BalanceCodeRedeemNotice notice = BalanceCodeRedeemNoticeFor(outcome);
  if (notice == BalanceCodeRedeemNotice::Invalid && refusalSaysRedeemed) {
    return BalanceCodeRedeemNotice::AlreadyRedeemed;
  }
  return notice;
}

// The gift confirmation's line: the day a year queued after the network's paid
// Pro period starts (gift_pro_redeemed_queued), when the answer says it is
// queued and its start reads as a date, else "A year of Pro is yours."
// (gift_pro_redeemed).
inline bool ShowsQueuedGiftLine(bool queued, bool startDateRead) {
  return queued && startDateRead;
}

}  // namespace urnw
