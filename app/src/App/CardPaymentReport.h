// What becomes of the upgrade sheet's card payment report (the sdk's
// Api::reportStripePurchase, which calls back once, after its retries; server
// POST /subscription/verify-stripe-checkout), and the refusal it can answer.
// Linux CardPaymentReport.hpp parity:
//
//   - credited or already_credited: the server holds the credit, so the
//     balance is read now. A credit wins whatever else came with it.
//   - an answer that carries an error is a refusal, whatever its status: the
//     server's last word on the purchase, which every later report of it
//     answers the same. Today that is a yearly trial whose card failed its CVC
//     check (status invalid, error.code incorrect_cvc): no Pro, and the card
//     was detached. The balance never carries a refusal, so the confirmation
//     the attempt started ends with it.
//   - anything else (pending after the retries, an older server's invalid, no
//     answer): the confirmation poll decides, as before.
//
// The refusal outlives the sheet. The upgrade sheet is built for each opening,
// and the buyer can close it while the report is still asking (a hosted
// checkout's report starts when the browser opens and retries for about 21
// minutes), so the balance store, which lives for the process, makes the
// report and keeps its refusal (CardRefusalKeeper). A sheet on screen shows it
// at once, from waiting, timed out or its products; a closed one's refusal
// shows the next time Get Pro opens, on its products with Try again. Only the
// newest purchase attempt's refusal is kept. A new attempt (Try again, another
// checkout, a coin's invoice), another plan or method, a credit, Pro and a
// sign-out clear it, and a Pro account never sees it.
//
// WinRT/SDK-free so tools/card-payment-report-tests.cpp runs it on any host.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "PaymentRefusal.h"

namespace urnw {

enum class CardReportOutcome {
  // the confirmation poll decides
  Confirming,
  // the server holds the credit: read the balance now
  Credited,
  // the attempt is over: the refusal, with Try again
  Refused,
};

// `credited`: the status is credited or already_credited. `refused`: the
// answer carries an error.
inline CardReportOutcome CardReportOutcomeFor(bool credited, bool refused) {
  if (credited) return CardReportOutcome::Credited;
  if (refused) return CardReportOutcome::Refused;
  return CardReportOutcome::Confirming;
}

// A refused card payment, kept for the upgrade sheet.
struct CardRefusal {
  uint64_t attempt = 0;  // the purchase attempt it refused
  // that attempt's plan: the sheet shows the refusal on it, and Try again
  // repeats it
  bool yearly = true;
  // the server's error.code as sent ("" for none): the purchase event's class
  std::string code;
  // the line (a catalog key) and the server's words under it (PaymentRefusal.h)
  PaymentRefusalText text;
  // its purchase.failed event was sent: once per refusal, whichever sheet
  // shows it first
  bool eventSent = false;
};

// The balance store's half of the refusal: the purchase attempts, and the
// newest one's refusal.
class CardRefusalKeeper {
 public:
  // A new purchase attempt: a checkout (the buy button, Try again, a checkout
  // an onboarding page starts) or a coin's invoice. Any earlier attempt's
  // refusal goes. Returns the attempt's id, which is never 0 and only grows.
  uint64_t BeginAttempt(bool yearly) {
    ++attempt_;
    attemptYearly_ = yearly;
    refusal_.reset();
    return attempt_;
  }
  uint64_t Attempt() const { return attempt_; }

  // The report of `attempt` was refused. It is kept only for the newest
  // attempt, only once, and never for a Pro account. True when it was kept.
  bool Keep(uint64_t attempt, std::string code, PaymentRefusalText text, bool pro) {
    if (attempt == 0 || attempt != attempt_ || pro) return false;
    if (refusal_ && refusal_->attempt == attempt) return false;
    refusal_ = CardRefusal{attempt, attemptYearly_, std::move(code), std::move(text), false};
    return true;
  }

  // Nothing to show any more: another plan or method, a credit, Pro, a
  // sign-out. True when a refusal was kept.
  bool Clear() {
    const bool had = refusal_.has_value();
    refusal_.reset();
    return had;
  }
  bool Has() const { return refusal_.has_value(); }

  // The kept refusal's purchase event was sent.
  void MarkEventSent(uint64_t attempt) {
    if (refusal_ && refusal_->attempt == attempt) refusal_->eventSent = true;
  }

  // What the sheet shows: the kept refusal, never for a Pro account.
  std::optional<CardRefusal> Shown(bool pro) const {
    if (pro) return std::nullopt;
    return refusal_;
  }

 private:
  uint64_t attempt_ = 0;
  bool attemptYearly_ = true;
  std::optional<CardRefusal> refusal_;
};

}  // namespace urnw
