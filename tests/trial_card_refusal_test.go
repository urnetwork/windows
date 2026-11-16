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
// {code: incorrect_cvc, message}. An answer that carries an error is a
// refusal unless it is a credit (the Linux app's rule). The sdk stops asking
// at it, and the balance never shows it, so the attempt ends here: the
// confirmation poll stops, and the waiting page, or the timeout page a hosted
// checkout's late answer finds, goes back to the products with the refusal's
// line and the button reading Try again, which starts a new payment sheet or
// checkout. The balance store makes the report and keeps the refusal past the
// sheet (card_payment_report_test.go); the sheet shows it. The words are
// PaymentRefusal.h's (TestPaymentRefusalText runs them); the sheet cannot be
// built off Windows, so its use of them is read here.

// The upgrade sheet's code without comments and whitespace, by definition.
func trialCardSheetBody(t *testing.T, signature string) string {
	t.Helper()
	sheets := stripComments(readAppSource(t, "BalanceSheets.cpp"))
	return cryptoPayCode(definitionBody(t, "BalanceSheets.cpp", sheets, signature))
}

// The sheet hands the attempt's Stripe id to the store, which reports it: the
// sheet makes no report of its own and never reads the server's words itself.
func TestUpgradeSheetReportsTheCardPaymentThroughTheStore(t *testing.T) {
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
	if strings.Contains(stripComments(sheets), "reportStripePurchase(") {
		t.Error("BalanceSheets.cpp reports the card payment itself; the balance store does, so its refusal outlives the sheet")
	}
	report := trialCardSheetBody(t, "void UpgradeSheet::ReportCardPayment(")
	requireCryptoPayOrder(t, "UpgradeSheet::ReportCardPayment", report,
		"if(cardPaymentReported_||!sdk_.IsLoggedIn())return;",
		"conststd::string&id=paySheet?paySheetSubscriptionId_:checkoutSessionId_;if(id.empty())return;",
		"cardPaymentReported_=true;",
		"balance_.ReportCardPayment(cardAttempt_,paySheet?id:std::string(),paySheet?std::string():id);")
	if strings.Contains(report, "error->message") {
		t.Error("UpgradeSheet::ReportCardPayment reads the refusal's message itself")
	}
}

// RefuseCard: only an attempt still on its way, from the products page (a
// creation stage, or a sheet opening on a kept refusal), Waiting, or TimedOut;
// the products come back on the refused attempt's plan with the card, the line
// and Try again; the attempt's ids and report are spent; its purchase.failed
// goes once; the line is announced now, or once the dialog is up. A late
// hand-back of the refused attempt waits for nothing.
func TestUpgradeSheetRefusedCardGoesBackToTheProducts(t *testing.T) {
	refuse := trialCardSheetBody(t, "void UpgradeSheet::RefuseCard(")
	requireCryptoPayOrder(t, "UpgradeSheet::RefuseCard", refuse,
		"refusalAttemptShown_=refusal.attempt;",
		"if(closed_||(page_!=Page::Products&&page_!=Page::Waiting&&page_!=Page::TimedOut)){return;}",
		"ShowPage(Page::Products);",
		"plans_.Select(refusal.yearly);wantedMethod_=cryptopay::kMethodCard;methods_.Select(cryptopay::kMethodCard);",
		`if(!refusal.eventSent){EmitPurchase("failed",refusal.code.empty()?std::string("refused"):refusal.code);balance_.MarkCardRefusalEventSent(refusal.attempt);}`,
		"purchaseEmitted_=true;",
		"cardRefused_=true;paySheetSubscriptionId_.clear();checkoutSessionId_.clear();cardPaymentReported_=false;",
		"ShowCheckoutError(hstring{PaymentRefusalMessage(Localized(refusal.text.key),Widen(refusal.text.detail))});",
		"ApplyPrices();",
		"subscribeButton_.Content(winrt::box_value(CtaLabel()));",
		"if(opened_){AnnounceCheckoutError();}else{announceAtOpen_=true;}")
	for _, unwanted := range []string{"Page::Success", "ShowPage(Page::Waiting)", "StopConfirmationPolling"} {
		if strings.Contains(refuse, unwanted) {
			t.Errorf("UpgradeSheet::RefuseCard: %s (it leaves Success alone, never waits again, and the store ends the confirmation)", unwanted)
		}
	}
	announce := trialCardSheetBody(t, "void UpgradeSheet::AnnounceCheckoutError(")
	requireCryptoPayOrder(t, "UpgradeSheet::AnnounceCheckoutError", announce,
		"peers::FrameworkElementAutomationPeer::FromElement(checkoutErrorText_)",
		"peer.RaiseAutomationEvent(peers::AutomationEvents::LiveRegionChanged);")
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
		// another plan or method is a fresh start, not a Try again, and the
		// store forgets the refusal
		"self->cardRefused_=false;self->balance_.ClearCardRefusal();self->subscribeButton_.Content(winrt::box_value(self->CtaLabel()));",
		"self->checkoutErrorText_.Visibility(Visibility::Collapsed);self->cardRefused_=false;self->balance_.ClearCardRefusal();self->ApplyMethod(method);",
		// a refusal shown as the sheet was built is announced once it is up
		"self->opened_=true;if(self->announceAtOpen_){self->announceAtOpen_=false;self->AnnounceCheckoutError();}")

	cta := trialCardSheetBody(t, "hstring UpgradeSheet::CtaLabel(")
	requireCryptoPayOrder(t, "UpgradeSheet::CtaLabel", cta,
		`Format("crypto_pay_button",PaymentMethodName(method))`,
		`if(cardRefused_)returnLoc("try_again");`,
		"returnPlanPicker::CtaLabel(plans_.Yearly());")

	// Try again is the buy button's BeginCheckout: a new attempt, numbered by
	// the store (which drops the kept refusal), with a new payment sheet or
	// checkout session and its own report
	begin := trialCardSheetBody(t, "void UpgradeSheet::BeginCheckout(")
	requireCryptoPayOrder(t, "UpgradeSheet::BeginCheckout", begin,
		"if(checkingOut_||!sdk_.IsLoggedIn())return;",
		"cardPaymentReported_=false;",
		"cardAttempt_=balance_.BeginPurchaseAttempt(plans_.Yearly());",
		"cardRefused_=false;",
		"checkoutErrorText_.Visibility(Visibility::Collapsed);")
}

// A refusal of the card itself is final for the attempt at every stage: the
// payment sheet's does not fall through to the embedded checkout, nor the
// embedded session's to the hosted one. It is kept by the store like the
// report's (so it outlives the sheet) and shown now; one the store does not
// keep still ends the attempt. A guest's refusal still comes first.
func TestUpgradeSheetCardRefusalNeverFallsThrough(t *testing.T) {
	sheet := trialCardSheetBody(t, "void UpgradeSheet::RequestPaymentSheet(")
	requireCryptoPayOrder(t, "UpgradeSheet::RequestPaymentSheet", sheet,
		"PaymentRefusalEndsAttempt(refusalCode)?std::optional<PaymentRefusalText>(PaymentRefusalTextFor(*result->error,\"something_went_wrong\")):std::nullopt;",
		"self->RefuseForGuest();return;}",
		"if(endsAttempt){self->RefuseCardAtStage(refusalCode,*endsAttempt);return;}",
		"if(clientSecret.empty()||publishableKey.empty()){self->RequestSession(true);return;}",
		"self->OpenPaySheet(clientSecret,publishableKey);")

	session := trialCardSheetBody(t, "void UpgradeSheet::RequestSession(")
	requireCryptoPayOrder(t, "UpgradeSheet::RequestSession", session,
		"self->RefuseForGuest();return;}",
		"if(refused&&PaymentRefusalEndsAttempt(refusalCode)){self->RefuseCardAtStage(refusalCode,refusalText);return;}",
		"if(embedded){",
		"self->RequestSession(false);",
		"self->LaunchHosted(url);")

	stage := trialCardSheetBody(t, "void UpgradeSheet::RefuseCardAtStage(")
	requireCryptoPayOrder(t, "UpgradeSheet::RefuseCardAtStage", stage,
		"balance_.KeepCardRefusal(cardAttempt_,code,text);",
		"conststd::optional<CardRefusal>kept=balance_.CurrentPoll().cardRefusal;",
		"if(kept&&kept->attempt==cardAttempt_){ApplyCardRefusal(kept);return;}",
		"ShowCheckoutError(hstring{PaymentRefusalMessage(Localized(text.key),Widen(text.detail))});")
}
