// Executable spec for the redeem sheet's answer (App/BalanceCodeRedeem.h): the
// SDK's urnet::classifyBalanceCodeRedeem outcome picks the notice, and only a
// credited redeem skips the redeemed-code list. The server refuses an unknown
// data code and one this network already redeemed with the SAME payload, so a
// retry after a lost-but-credited response must read "already redeemed", never
// "invalid" (UPGRADE.md D2). A redeemed code of kind "pro_gift" is a gift
// code's year of Pro, which the list shows instead of its 0 bytes, and which
// confirms as the gift when the list is all a lost answer leaves; the list is
// searched for the code as the server reads a typed one.
//
//   c++ -std=c++20 -I ../src/App balance-code-redeem-tests.cpp -o /tmp/balance-code-redeem-tests && /tmp/balance-code-redeem-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "BalanceCodeRedeem.h"

using urnw::BalanceCodeRedeemNeedsCodeList;
using urnw::BalanceCodeRedeemNotice;
using urnw::BalanceCodeRedeemNoticeAfterList;
using urnw::BalanceCodeRedeemNoticeFor;
using urnw::IsProGiftCode;
using urnw::NormalizeBalanceCode;
using urnw::RedeemedCodeKind;
using urnw::RefusalSaysAlreadyRedeemed;
using urnw::ShowsQueuedGiftLine;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

// a row of the network's redeemed list, as the sdk's RedeemedBalanceCode has it
struct Listed {
  std::string secret;
  std::string kind;
};

const std::string kCode = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";  // 26 characters, base32 letters
const std::string kGift = "GIFTCODEQRSTUVWXYZ234567AB";

}  // namespace

int main() {
  Check(BalanceCodeRedeemNoticeFor("redeemed") == BalanceCodeRedeemNotice::Redeemed,
        "redeemed is the success state");
  Check(BalanceCodeRedeemNoticeFor("already_redeemed") == BalanceCodeRedeemNotice::AlreadyRedeemed,
        "already_redeemed says the data is on the balance");
  Check(BalanceCodeRedeemNoticeFor("invalid") == BalanceCodeRedeemNotice::Invalid,
        "invalid is a refusal");
  Check(BalanceCodeRedeemNoticeFor("unknown") == BalanceCodeRedeemNotice::Unknown,
        "unknown keeps the check-your-balance copy");
  Check(BalanceCodeRedeemNoticeFor("") == BalanceCodeRedeemNotice::Unknown,
        "an empty outcome is never success or invalid");
  Check(BalanceCodeRedeemNoticeFor("something_new") == BalanceCodeRedeemNotice::Unknown,
        "an outcome this build does not know is unknown");

  Check(!BalanceCodeRedeemNeedsCodeList("redeemed"), "a credited redeem needs no list");
  Check(BalanceCodeRedeemNeedsCodeList("invalid"), "a refusal consults the redeemed-code list");
  Check(BalanceCodeRedeemNeedsCodeList("unknown"), "a lost response consults the redeemed-code list");

  // the sdk's BalanceCodeKind* values
  Check(IsProGiftCode("pro_gift"), "a gift code's row is its year of Pro");
  Check(!IsProGiftCode("data"), "a data code's row is its data");
  Check(!IsProGiftCode(""), "a code from a server before gift codes is data");
  Check(!IsProGiftCode("something_new"), "a kind this build does not know shows its data");

  // a typed code as the server reads it: no whitespace, dashes or dots, upper
  // case, and 0, 1, 8 as O, I, B (server normalizeBalanceCodeSecret and its
  // lookalikes; the sdk's twin)
  Check(NormalizeBalanceCode(kCode) == kCode, "a code as minted is itself");
  Check(NormalizeBalanceCode("abcd efgh ijkl mnop qrst uvwx yz") == kCode,
        "in groups of four, lower case, as a card prints it");
  Check(NormalizeBalanceCode(" ABCD-EFGH-IJKL-MNOP-QRST-UVWX-YZ\n") == kCode, "hyphens, a newline");
  Check(NormalizeBalanceCode("ABCD.EFGH\tIJKL–MNOP—QRST UVWX　YZ") == kCode,
        "dots, a tab, an en and an em dash, a no-break and an ideographic space");
  Check(NormalizeBalanceCode("ABCD－EFGH‐IJKL‑MNOP‒QRST―UVWX⸺YZ") == kCode,
        "every kind of dash punctuation");
  Check(NormalizeBalanceCode("0O1I8B") == "OOIIBB", "0, 1 and 8 read as O, I and B");
  Check(NormalizeBalanceCode("2345679") == "2345679", "the other digits stay");
  Check(NormalizeBalanceCode("ıſ") == "IS", "a dotless i and a long s upper-case to I and S");
  Check(NormalizeBalanceCode("AB−CD") == "AB−CD", "a minus sign is not a dash");
  Check(NormalizeBalanceCode("AB​CD") == "AB​CD", "a zero-width space is not a space");
  Check(NormalizeBalanceCode("\xC1\x81") == "\xEF\xBF\xBD\xEF\xBF\xBD",
        "an overlong A is two malformed bytes, never an A");
  Check(NormalizeBalanceCode("A\xED\xA0\x80" "B") == "A\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" "B",
        "a surrogate is malformed");
  Check(NormalizeBalanceCode("").empty() && NormalizeBalanceCode(" - . ").empty(),
        "nothing but separators is no code");

  // the code in the network's redeemed list
  const std::vector<Listed> list{{kCode, "data"}, {kGift, "pro_gift"}};
  Check(RedeemedCodeKind(kCode, list) == std::optional<std::string>("data"), "a listed data code");
  Check(RedeemedCodeKind("gift-code-qrst-uvwx-yz23-4567-ab", list) ==
            std::optional<std::string>("pro_gift"),
        "a listed gift code, typed as the card prints it");
  Check(RedeemedCodeKind("G1FTC0DEQRSTUVWXYZ234567A8", list) == std::optional<std::string>("pro_gift"),
        "a listed gift code, typed with 1, 0 and 8 for I, O and B");
  Check(!RedeemedCodeKind("ZZZZEFGHIJKLMNOPQRSTUVWXYZ", list), "a code that is not listed");
  Check(!RedeemedCodeKind(kCode, std::vector<Listed>{}), "an empty list");
  Check(!RedeemedCodeKind(" ", std::vector<Listed>{{"", "pro_gift"}}), "an empty code finds nothing");

  // a refusal that says the code is redeemed: the server's words for a gift code
  Check(RefusalSaysAlreadyRedeemed("Balance code already redeemed."), "the server's words");
  Check(RefusalSaysAlreadyRedeemed("ALREADY REDEEMED"), "in any case");
  Check(!RefusalSaysAlreadyRedeemed("Unknown balance code."), "an unknown code");
  Check(!RefusalSaysAlreadyRedeemed("Balance code expired."), "an expired gift");
  Check(!RefusalSaysAlreadyRedeemed("Balance code was refunded."), "a refunded gift");
  Check(!RefusalSaysAlreadyRedeemed(""), "no message");

  // what a redeem that did not credit says once the list is read
  using Notice = BalanceCodeRedeemNotice;
  const std::optional<std::string> none;
  const std::optional<std::string> gift("pro_gift");
  const std::optional<std::string> data("data");
  Check(BalanceCodeRedeemNoticeAfterList("already_redeemed", gift, true) == Notice::GiftRedeemed,
        "a refused gift this network holds: the gift's confirmation");
  Check(BalanceCodeRedeemNoticeAfterList("already_redeemed", gift, false) == Notice::GiftRedeemed,
        "a lost answer for a gift this network now holds: the gift's confirmation");
  Check(BalanceCodeRedeemNoticeAfterList("already_redeemed", data, false) == Notice::AlreadyRedeemed,
        "a data code this network holds: already redeemed, as before");
  Check(BalanceCodeRedeemNoticeAfterList("invalid", none, true) == Notice::AlreadyRedeemed,
        "a code the server calls redeemed that is not this network's: already redeemed");
  Check(BalanceCodeRedeemNoticeAfterList("invalid", none, false) == Notice::Invalid,
        "any other refusal of a code this network never had: invalid, as before");
  Check(BalanceCodeRedeemNoticeAfterList("unknown", none, false) == Notice::Unknown,
        "no answer and not in the list: the check-your-balance copy, as before");
  Check(BalanceCodeRedeemNoticeAfterList("already_redeemed", none, false) == Notice::AlreadyRedeemed,
        "the sdk's own find stands");

  // the gift confirmation's line
  Check(ShowsQueuedGiftLine(true, true), "a queued year with its start date says when it starts");
  Check(!ShowsQueuedGiftLine(true, false), "a queued year without a readable start: a year of Pro is yours");
  Check(!ShowsQueuedGiftLine(false, true), "a year that starts now: a year of Pro is yours");

  std::cout << (gCases - gFailures) << "/" << gCases << " balance code redeem checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
