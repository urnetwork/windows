// Executable spec for the card payment report's outcome and the refusal the
// balance store keeps past the upgrade sheet (App/CardPaymentReport.h): a
// credit wins, an answer with an error is a refusal, anything else leaves the
// confirmation poll to decide; only the newest purchase attempt's refusal is
// kept, a new attempt and a clear drop it, a Pro account never sees it, and its
// purchase event is sent once.
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App card-payment-report-tests.cpp -o /tmp/card-payment-report-tests && /tmp/card-payment-report-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <string>

#include "CardPaymentReport.h"

using urnw::CardRefusal;
using urnw::CardRefusalKeeper;
using urnw::CardReportOutcome;
using urnw::CardReportOutcomeFor;
using urnw::PaymentRefusalText;

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

const PaymentRefusalText kCvcLine{"checkout_error_incorrect_cvc", ""};

}  // namespace

int main() {
  // the report's outcome: a credit wins, then an error, else the poll decides
  Check(CardReportOutcomeFor(true, false) == CardReportOutcome::Credited, "a credit");
  Check(CardReportOutcomeFor(true, true) == CardReportOutcome::Credited,
        "a credit with an error is still a credit");
  Check(CardReportOutcomeFor(false, true) == CardReportOutcome::Refused, "an error is a refusal");
  Check(CardReportOutcomeFor(false, false) == CardReportOutcome::Confirming,
        "no credit and no error: the poll decides");

  // the newest attempt's refusal is kept, with its plan, code and line
  {
    CardRefusalKeeper keeper;
    Check(!keeper.Shown(false), "nothing before an attempt");
    const uint64_t first = keeper.BeginAttempt(/*yearly=*/true);
    Check(first != 0, "an attempt's id is never 0");
    Check(keeper.Attempt() == first, "the newest attempt");
    Check(keeper.Keep(first, "incorrect_cvc", kCvcLine, /*pro=*/false), "the newest attempt's refusal is kept");
    const std::optional<CardRefusal> shown = keeper.Shown(false);
    Check(shown && shown->attempt == first && shown->yearly && shown->code == "incorrect_cvc" &&
              shown->text.key == "checkout_error_incorrect_cvc" && !shown->eventSent,
          "it shows as kept, its event not sent yet");
    Check(!keeper.Keep(first, "incorrect_cvc", kCvcLine, false), "the same refusal is kept once");
  }

  // an older attempt's refusal is never kept; a new attempt drops the old one
  {
    CardRefusalKeeper keeper;
    const uint64_t first = keeper.BeginAttempt(true);
    const uint64_t second = keeper.BeginAttempt(true);
    Check(second > first, "ids only grow");
    Check(!keeper.Keep(first, "incorrect_cvc", kCvcLine, false), "an older attempt's refusal is dropped");
    Check(!keeper.Shown(false), "nothing kept for it");
    Check(keeper.Keep(second, "incorrect_cvc", kCvcLine, false), "the newest one's is kept");
    keeper.BeginAttempt(true);  // Try again
    Check(!keeper.Shown(false), "a new attempt (Try again) drops the kept refusal");
    Check(!keeper.Keep(second, "incorrect_cvc", kCvcLine, false),
          "and the refused attempt's later answers stay dropped");
    Check(!keeper.Keep(0, "incorrect_cvc", kCvcLine, false), "no attempt has id 0");
  }

  // a Pro account: never kept, never shown
  {
    CardRefusalKeeper keeper;
    const uint64_t attempt = keeper.BeginAttempt(true);
    Check(!keeper.Keep(attempt, "incorrect_cvc", kCvcLine, /*pro=*/true), "not kept for a Pro account");
    Check(keeper.Keep(attempt, "incorrect_cvc", kCvcLine, false), "kept while Free");
    Check(!keeper.Shown(/*pro=*/true), "never shown to a Pro account");
    Check(keeper.Shown(false).has_value(), "still there while Free");
  }

  // clearing: another plan or method, a credit, Pro, a sign-out
  {
    CardRefusalKeeper keeper;
    const uint64_t attempt = keeper.BeginAttempt(false);
    Check(!keeper.Clear(), "nothing to clear");
    Check(keeper.Keep(attempt, "card_declined", PaymentRefusalText{"something_went_wrong", "The words."}, false),
          "kept");
    const std::optional<CardRefusal> shown = keeper.Shown(false);
    Check(shown && !shown->yearly && shown->text.detail == "The words.", "the monthly attempt's own plan and words");
    Check(keeper.Has(), "it has one");
    Check(keeper.Clear(), "a clear says there was one");
    Check(!keeper.Has() && !keeper.Shown(false), "and it is gone");
    Check(keeper.Attempt() == attempt, "a clear keeps the attempt");
  }

  // the purchase event goes once, for the refusal it was sent for
  {
    CardRefusalKeeper keeper;
    const uint64_t attempt = keeper.BeginAttempt(true);
    keeper.Keep(attempt, "incorrect_cvc", kCvcLine, false);
    keeper.MarkEventSent(attempt + 1);
    Check(!keeper.Shown(false)->eventSent, "another attempt's mark changes nothing");
    keeper.MarkEventSent(attempt);
    Check(keeper.Shown(false)->eventSent, "marked sent");
    Check(!keeper.Keep(attempt, "incorrect_cvc", kCvcLine, false) && keeper.Shown(false)->eventSent,
          "a repeat of the refusal does not send it again");
  }

  std::cout << (gCases - gFailures) << "/" << gCases << " card payment report checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
