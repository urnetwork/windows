// Executable spec for what the app's own payment screens say when the server
// refuses (App/PaymentRefusal.h): the upgrade sheet's hosted checkout session
// and card payment report, and Manage subscription in Settings. A code with a
// line of its own reads that line alone, invalid_request and start_failed read
// the screen's own line alone, and any other code, or none, reads the screen's
// own line with the server's words under it. The card payment report's answer
// is a refusal exactly when it carries an error, whatever its status, and a
// refusal of the card itself (incorrect_cvc) ends the attempt at any stage.
//
//   c++ -std=c++20 -I ../src/App payment-refusal-tests.cpp -o /tmp/payment-refusal-tests && /tmp/payment-refusal-tests
//
// With URNW_PAYMENT_REFUSAL_TESTS_SDK the refusals are the generated header's
// own structs (urnet::StripeCreateCheckoutSessionError,
// urnet::StripeCreateCustomerPortalError and the card payment report's
// urnet::VerifyStorePurchaseResult), parsed from the server's json through the
// header's conversions, and the refused card's code is the header's. The
// header needs nlohmann/json; both are system includes because the generated
// code does not build with -Wextra -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_PAYMENT_REFUSAL_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> payment-refusal-tests.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>

#include "PaymentRefusal.h"

#if defined(URNW_PAYMENT_REFUSAL_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

using urnw::PaymentRefusalEndsAttempt;
using urnw::PaymentRefusalMessage;
using urnw::PaymentRefusalText;
using urnw::PaymentRefusalTextFor;
using urnw::PurchaseReportRefusalFor;

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

void CheckEq(const std::string& want, const std::string& got, const std::string& what) {
  Check(want == got, what + ": want \"" + want + "\", got \"" + got + "\"");
}

// The spec's wide text is ASCII.
std::string Narrow(const std::wstring& value) {
  std::string narrow;
  for (const wchar_t character : value) narrow += static_cast<char>(character);
  return narrow;
}

void CheckText(const PaymentRefusalText& want, const PaymentRefusalText& got,
               const std::string& what) {
  CheckEq(want.key, got.key, what + ", the line");
  CheckEq(want.detail, got.detail, what + ", the words under it");
}

// A refusal text that should be there.
void CheckRefusal(const PaymentRefusalText& want, const std::optional<PaymentRefusalText>& got,
                  const std::string& what) {
  Check(got.has_value(), what + ": want a refusal, got none");
  if (got) CheckText(want, *got, what);
}

#if defined(URNW_PAYMENT_REFUSAL_TESTS_SDK)
using CheckoutSessionError = urnet::StripeCreateCheckoutSessionError;
using CustomerPortalError = urnet::StripeCreateCustomerPortalError;
using ReportError = urnet::OnboardingError;
using ReportResult = urnet::VerifyStorePurchaseResult;
#else
// The fields of the sdk's refusals, by the generated wrapper's names and types.
struct CheckoutSessionError {
  std::optional<std::string> code;
  std::string message;
};
struct CustomerPortalError {
  std::optional<std::string> code;
  std::string message;
};
// The card payment report's answer: its status, and the refusal beside it.
struct ReportError {
  std::optional<std::string> code;
  std::string message;
};
struct ReportResult {
  std::string status;
  std::optional<std::string> expiry_time;
  std::optional<ReportError> error;
};
#endif

// A report answer with the status and, when `code` or `message` is set, an
// error.
ReportResult Report(const std::string& status, std::optional<std::string> code = std::nullopt,
                    std::optional<std::string> message = std::nullopt) {
  ReportResult result;
  result.status = status;
  if (code || message) {
    ReportError error;
    error.code = std::move(code);
    error.message = message.value_or(std::string());
    result.error = error;
  }
  return result;
}

}  // namespace

int main() {
  const std::string words = "The server's English words.";
  const std::string screenKey = "something_went_wrong";

  // the codes with a line of their own: that line alone, without the words
  struct Expected {
    const char* code;
    const char* key;
  };
  for (const Expected& c : {
           Expected{"already_subscribed", "site_payment_error_already_subscribed"},
           Expected{"plan_unavailable", "site_payment_error_plan_unavailable"},
           Expected{"checkout_unavailable", "checkout_error_unavailable"},
           Expected{"incorrect_cvc", "checkout_error_incorrect_cvc"},
           Expected{"no_customer", "site_subscription_error_no_customer"},
           Expected{"store_unavailable", "site_subscription_error_store_unavailable"},
       }) {
    CheckText({c.key, ""}, PaymentRefusalTextFor(c.code, words, screenKey),
              std::string("the code ") + c.code);
  }

  // a defect of this app's request, and a start to try again: the screen's
  // own line alone
  for (const char* code : {"invalid_request", "start_failed"}) {
    CheckText({screenKey, ""}, PaymentRefusalTextFor(code, words, screenKey),
              std::string("the code ") + code);
  }

  // any other code, none (an older server), or a code spelled otherwise (the
  // server sends them lowercase, and they compare exactly): the screen's own
  // line with the server's words under it
  for (const char* code : {"", "rate_limited", "item_unavailable", "offer_unavailable",
                           "guest_sign_in_required", "ALREADY_SUBSCRIBED", "No_Customer",
                           " start_failed", "INCORRECT_CVC", "incorrect_cvc "}) {
    CheckText({screenKey, words}, PaymentRefusalTextFor(code, words, screenKey),
              std::string("the code \"") + code + "\"");
  }

  // a refusal without words has nothing to put under the line
  CheckText({screenKey, ""}, PaymentRefusalTextFor("rate_limited", "", screenKey),
            "an unknown code without words");
  CheckText({screenKey, ""}, PaymentRefusalTextFor("", "", screenKey), "no code and no words");

  // the screen's own line is the caller's
  const std::string otherScreenKey = "sample_screen_line";
  CheckText({otherScreenKey, ""}, PaymentRefusalTextFor("start_failed", words, otherScreenKey),
            "another screen's start_failed");
  CheckText({otherScreenKey, words}, PaymentRefusalTextFor("", words, otherScreenKey),
            "another screen's refusal without a code");
  CheckText({"site_payment_error_already_subscribed", ""},
            PaymentRefusalTextFor("already_subscribed", words, otherScreenKey),
            "another screen's already_subscribed");

  // what the screen shows: the line alone, or the words on the line under it
  CheckEq("A line.", Narrow(PaymentRefusalMessage(L"A line.", L"")), "a line without words");
  CheckEq("A line.\nThe words.", Narrow(PaymentRefusalMessage(L"A line.", L"The words.")),
          "a line with words");

  // the sdk's refusals as the screens read them: an optional code beside the
  // message, none from an older server
  {
    CheckoutSessionError refused;
    refused.code = "already_subscribed";
    refused.message = words;
    CheckText({"site_payment_error_already_subscribed", ""},
              PaymentRefusalTextFor(refused, screenKey), "a checkout session refusal");
    CheckoutSessionError older;
    older.message = words;
    CheckText({screenKey, words}, PaymentRefusalTextFor(older, screenKey),
              "a checkout session refusal without a code");
    CustomerPortalError portal;
    portal.code = "no_customer";
    portal.message = words;
    CheckText({"site_subscription_error_no_customer", ""}, PaymentRefusalTextFor(portal, screenKey),
              "a billing portal refusal");
    CustomerPortalError olderPortal;
    olderPortal.message = words;
    CheckText({screenKey, words}, PaymentRefusalTextFor(olderPortal, screenKey),
              "a billing portal refusal without a code");
  }

  // the card payment report: any answer that carries an error is a refusal,
  // in the screen's words for its code (the Linux app's rule)
  {
    CheckRefusal({"checkout_error_incorrect_cvc", ""},
                 PurchaseReportRefusalFor(Report("invalid", "incorrect_cvc", words), screenKey),
                 "the report's incorrect_cvc");
    CheckRefusal({screenKey, words},
                 PurchaseReportRefusalFor(Report("invalid", "card_declined", words), screenKey),
                 "the report's code without a line of its own");
    CheckRefusal({screenKey, words},
                 PurchaseReportRefusalFor(Report("invalid", std::nullopt, words), screenKey),
                 "the report's refusal without a code");
    CheckRefusal({screenKey, ""}, PurchaseReportRefusalFor(Report("invalid", "", ""), screenKey),
                 "the report's refusal without a code or words");
    // whatever its status: the sdk stops asking at any answer that carries an
    // error, so a wait left running there would never hear again
    for (const char* status : {"pending", "wrong_network", "credited", "already_credited", ""}) {
      CheckRefusal({"checkout_error_incorrect_cvc", ""},
                   PurchaseReportRefusalFor(Report(status, "incorrect_cvc", words), screenKey),
                   std::string("a \"") + status + "\" answer with an error");
    }
    // without an error the confirmation poll decides, as before: an older
    // server's invalid among them
    for (const char* status : {"invalid", "pending", "credited", "already_credited", "wrong_network", ""}) {
      Check(!PurchaseReportRefusalFor(Report(status), screenKey),
            std::string("the report's \"") + status + "\" without an error is not a refusal");
    }
  }

  // a refusal of the card itself ends the attempt at any stage; every other
  // code falls through to the next stage as before
  Check(PaymentRefusalEndsAttempt("incorrect_cvc"), "incorrect_cvc ends the attempt");
  for (const char* code : {"", "already_subscribed", "plan_unavailable", "checkout_unavailable",
                           "start_failed", "invalid_request", "guest_sign_in_required",
                           "INCORRECT_CVC", " incorrect_cvc"}) {
    Check(!PaymentRefusalEndsAttempt(code),
          std::string("the code \"") + code + "\" does not end the attempt");
  }

#if defined(URNW_PAYMENT_REFUSAL_TESTS_SDK)
  // the server's json, through the header's conversions
  {
    const urnet::StripeCreateCustomerPortalResult portal = nlohmann::json::parse(R"({
      "error": {"code": "store_unavailable", "message": "The server's English words."}
    })").get<urnet::StripeCreateCustomerPortalResult>();
    Check(portal.error.has_value(), "the billing portal's refusal parses");
    if (portal.error) {
      CheckText({"site_subscription_error_store_unavailable", ""},
                PaymentRefusalTextFor(*portal.error, screenKey), "the billing portal's json");
    }
    const urnet::StripeCreateCustomerPortalResult olderPortal = nlohmann::json::parse(R"({
      "error": {"message": "The server's English words."}
    })").get<urnet::StripeCreateCustomerPortalResult>();
    Check(olderPortal.error.has_value(), "an older billing portal refusal parses");
    if (olderPortal.error) {
      CheckText({screenKey, words}, PaymentRefusalTextFor(*olderPortal.error, screenKey),
                "an older billing portal's json");
    }
    const urnet::StripeCreateCheckoutSessionResult session = nlohmann::json::parse(R"({
      "error": {"code": "plan_unavailable", "message": "The server's English words."}
    })").get<urnet::StripeCreateCheckoutSessionResult>();
    Check(session.error.has_value(), "the checkout session's refusal parses");
    if (session.error) {
      CheckText({"site_payment_error_plan_unavailable", ""},
                PaymentRefusalTextFor(*session.error, screenKey), "the checkout session's json");
    }

    // the card payment report's refusal, exactly as the server answers it
    const urnet::VerifyStorePurchaseResult cardRefused = nlohmann::json::parse(R"({
      "status": "invalid",
      "error": {"code": "incorrect_cvc", "message": "Your card's security code (CVC) didn't match. Check the card details and try again."}
    })").get<urnet::VerifyStorePurchaseResult>();
    Check(cardRefused.error.has_value(), "the card payment report's refusal parses");
    CheckRefusal({"checkout_error_incorrect_cvc", ""}, PurchaseReportRefusalFor(cardRefused, screenKey),
                 "the card payment report's json");
    const urnet::VerifyStorePurchaseResult olderInvalid =
        nlohmann::json::parse(R"({"status": "invalid"})").get<urnet::VerifyStorePurchaseResult>();
    Check(!PurchaseReportRefusalFor(olderInvalid, screenKey),
          "an older server's invalid, with nothing beside it, is not a refusal");
    const urnet::VerifyStorePurchaseResult credited =
        nlohmann::json::parse(R"({"status": "credited", "expiry_time": "2027-11-30T00:00:00Z"})")
            .get<urnet::VerifyStorePurchaseResult>();
    Check(!PurchaseReportRefusalFor(credited, screenKey), "the card payment report's credit is not a refusal");
  }
  // the code PaymentRefusal.h compares is the sdk's
  CheckEq(urnet::PurchaseErrorCodeIncorrectCvc, std::string(urnw::kPurchaseErrorCodeIncorrectCvc),
          "incorrect_cvc is the sdk's code");
#endif

  std::cout << (gCases - gFailures) << "/" << gCases << " payment refusal checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
