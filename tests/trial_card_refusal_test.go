// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"strings"
	"testing"
)

// The trial card check (server feature/trial-card-check, owner 2026-11-16): a
// yearly trial whose card fails its CVC check is refused, and the card payment
// report (POST /subscription/verify-stripe-checkout, the sdk's
// Api::reportStripePurchase) answers status invalid with error
// {code: incorrect_cvc, message}. Any answer that carries an error is a
// refusal (the Linux app's rule). The sdk stops asking at it, and the balance
// never shows it, so the upgrade sheet ends the attempt itself: the
// confirmation poll stops, and the waiting page, or the timeout page a hosted
// checkout's late answer finds, goes back to the products with the refusal's
// line and the button reading Try again, which starts a new payment sheet or
// checkout. The rule is PaymentRefusal.h's (TestPaymentRefusalText runs it);
// the sheet cannot be built off Windows, so its use of the rule is read here.

// The upgrade sheet's code without comments and whitespace, by definition.
func trialCardSheetBody(t *testing.T, signature string) string {
	t.Helper()
	sheets := stripComments(readAppSource(t, "BalanceSheets.cpp"))
	return cryptoPayCode(definitionBody(t, "BalanceSheets.cpp", sheets, signature))
}

// The report's answer: a refusal ends the attempt it was made for, before the
// credit check; a credit still reads the balance.
func TestUpgradeSheetReadsTheCardPaymentReportsRefusal(t *testing.T) {
	sheets := readAppSource(t, "BalanceSheets.cpp")
	for _, want := range []string{
		`#include "PaymentRefusal.h"`,
		// PaymentRefusal.h's code is the sdk's
		"static_assert(std::string_view(urnet::PurchaseErrorCodeIncorrectCvc) == kPurchaseErrorCodeIncorrectCvc);",
	} {
		if !strings.Contains(sheets, want) {
			t.Errorf("BalanceSheets.cpp: missing %s", want)
		}
	}
	report := trialCardSheetBody(t, "void UpgradeSheet::ReportCardPayment(")
	requireCryptoPayOrder(t, "UpgradeSheet::ReportCardPayment", report,
		"cardPaymentReported_=true;",
		"constuint32_tattempt=cardAttempt_;",
		"sdk_.api().reportStripePurchase(",
		`result?PurchaseReportRefusalFor(*result,"something_went_wrong"):std::nullopt;`,
		"queue.TryEnqueue([weak,attempt,status,refusal,refusalCode]{",
		"if(refusal){if(attempt!=self->cardAttempt_)return;self->RefuseCard(hstring{PaymentRefusalMessage(Localized(refusal->key),Widen(refusal->detail))},",
		"return;}",
		"self->balance_.Refresh();")
	// the server's words reach the line only through PaymentRefusal.h
	if strings.Contains(report, "error->message") {
		t.Error("UpgradeSheet::ReportCardPayment reads the refusal's message itself")
	}
}

// RefuseCard: only an attempt still on its way, from the products page (a
// creation stage), Waiting, or TimedOut; the products come back with the line
// and Try again, the attempt's ids and report spent, and then the poll stops
// (its balance push finds the products, not a wait to decide); the line is
// announced. A late hand-back of the refused attempt waits for nothing.
func TestUpgradeSheetRefusedCardGoesBackToTheProducts(t *testing.T) {
	refuse := trialCardSheetBody(t, "void UpgradeSheet::RefuseCard(")
	requireCryptoPayOrder(t, "UpgradeSheet::RefuseCard", refuse,
		"if(closed_||(page_!=Page::Products&&page_!=Page::Waiting&&page_!=Page::TimedOut)){return;}",
		`if(!purchaseEmitted_){EmitPurchase("failed",errorClass);purchaseEmitted_=true;}`,
		"ShowPage(Page::Products);",
		"cardRefused_=true;",
		"paySheetSubscriptionId_.clear();checkoutSessionId_.clear();cardPaymentReported_=false;",
		"ShowCheckoutError(message);",
		"balance_.StopConfirmationPolling();",
		"ApplyPrices();",
		"subscribeButton_.Content(winrt::box_value(CtaLabel()));",
		"peers::FrameworkElementAutomationPeer::FromElement(checkoutErrorText_)",
		"peer.RaiseAutomationEvent(peers::AutomationEvents::LiveRegionChanged);")
	if strings.Contains(refuse, "Page::Success") || strings.Contains(refuse, "ShowPage(Page::Waiting)") {
		t.Error("UpgradeSheet::RefuseCard must leave Success alone and never wait again")
	}
	callback := trialCardSheetBody(t, "void UpgradeSheet::HandleCheckoutCallback(")
	requireCryptoPayOrder(t, "UpgradeSheet::HandleCheckoutCallback", callback,
		"if(complete){if(cardRefused_)return;",
		"ReportCardPayment(!checkoutRedirect);",
		"balance_.StartConfirmationPolling();",
		"ShowPage(Page::Waiting);")

	build := trialCardSheetBody(t, "void UpgradeSheet::Build(")
	requireCryptoPay(t, "UpgradeSheet::Build", build,
		// the line is a live region, which RefuseCard announces
		"AutomationProperties::SetLiveSetting(checkoutErrorText_,winrt::Microsoft::UI::Xaml::Automation::Peers::AutomationLiveSetting::Assertive);",
		// another plan or method is a fresh start, not a Try again
		"self->cardRefused_=false;self->subscribeButton_.Content(winrt::box_value(self->CtaLabel()));",
		"self->checkoutErrorText_.Visibility(Visibility::Collapsed);self->cardRefused_=false;self->ApplyMethod(method);")

	cta := trialCardSheetBody(t, "hstring UpgradeSheet::CtaLabel(")
	requireCryptoPayOrder(t, "UpgradeSheet::CtaLabel", cta,
		`Format("crypto_pay_button",PaymentMethodName(method))`,
		`if(cardRefused_)returnLoc("try_again");`,
		"returnPlanPicker::CtaLabel(plans_.Yearly());")

	// Try again is the buy button's BeginCheckout: a new attempt, with a new
	// payment sheet or checkout session, and its own report
	begin := trialCardSheetBody(t, "void UpgradeSheet::BeginCheckout(")
	requireCryptoPayOrder(t, "UpgradeSheet::BeginCheckout", begin,
		"if(checkingOut_||!sdk_.IsLoggedIn())return;",
		"cardPaymentReported_=false;",
		"++cardAttempt_;",
		"cardRefused_=false;",
		"checkoutErrorText_.Visibility(Visibility::Collapsed);")
}

// A refusal of the card itself is final for the attempt at every stage: the
// payment sheet's does not fall through to the embedded checkout, nor the
// embedded session's to the hosted one. A guest's refusal still comes first.
func TestUpgradeSheetCardRefusalNeverFallsThrough(t *testing.T) {
	sheet := trialCardSheetBody(t, "void UpgradeSheet::RequestPaymentSheet(")
	requireCryptoPayOrder(t, "UpgradeSheet::RequestPaymentSheet", sheet,
		"PaymentRefusalEndsAttempt(refusalCode)?std::optional<PaymentRefusalText>(PaymentRefusalTextFor(*result->error,\"something_went_wrong\")):std::nullopt;",
		"self->RefuseForGuest();return;}",
		"if(endsAttempt){self->RefuseCard(hstring{PaymentRefusalMessage(Localized(endsAttempt->key),Widen(endsAttempt->detail))},refusalCode);return;}",
		"if(clientSecret.empty()||publishableKey.empty()){self->RequestSession(true);return;}",
		"self->OpenPaySheet(clientSecret,publishableKey);")

	session := trialCardSheetBody(t, "void UpgradeSheet::RequestSession(")
	requireCryptoPayOrder(t, "UpgradeSheet::RequestSession", session,
		"self->RefuseForGuest();return;}",
		"if(refused&&PaymentRefusalEndsAttempt(refusalCode)){self->RefuseCard(hstring{PaymentRefusalMessage(Localized(refusalText.key),Widen(refusalText.detail))},refusalCode);return;}",
		"if(embedded){",
		"self->RequestSession(false);",
		"self->LaunchHosted(url);")
}

// The confirmation the refused attempt started ends with it: no give-up to
// show, and the background poll takes over, so the retry's
// StartConfirmationPolling starts afresh with the full budget.
func TestBalanceStoreStopsTheConfirmationOnARefusal(t *testing.T) {
	if !strings.Contains(readAppSource(t, "SubscriptionBalance.h"), "void StopConfirmationPolling();") {
		t.Error("SubscriptionBalance.h does not declare StopConfirmationPolling")
	}
	store := stripComments(readAppSource(t, "SubscriptionBalance.cpp"))
	stop := cryptoPayCode(definitionBody(t, "SubscriptionBalance.cpp", store,
		"void SubscriptionBalanceStore::StopConfirmationPolling("))
	requireCryptoPayOrder(t, "SubscriptionBalanceStore::StopConfirmationPolling", stop,
		"if(!gate_.Confirming()&&!timedOut_)return;",
		"StopConfirmation(false);",
		"timedOut_=false;",
		"EnsureBackgroundPolling();",
		"Publish();")
	// StartConfirmationPolling still keeps a running confirmation
	start := cryptoPayCode(definitionBody(t, "SubscriptionBalance.cpp", store,
		"void SubscriptionBalanceStore::StartConfirmationPolling("))
	requireCryptoPayOrder(t, "SubscriptionBalanceStore::StartConfirmationPolling", start,
		"if(gate_.Confirming())return;",
		"timedOut_=false;",
		"gate_.Start(NowMillis());")
}
