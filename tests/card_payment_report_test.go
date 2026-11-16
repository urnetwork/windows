// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The card payment refusal outlives the upgrade sheet (owner 2026-11-16,
// macOS, Linux and Android parity). The sheet is built for each opening, and
// the buyer can close it while the card payment report is still asking (a
// hosted checkout's report retries for about 21 minutes), so the balance store
// makes the report and keeps the refusal (App/CardPaymentReport.h, whose spec
// runs here). An open sheet shows it at once; a closed one's refusal shows the
// next time Get Pro opens, on its products with Try again, and is announced
// once the dialog is up. Only the newest purchase attempt's refusal is kept;
// Try again (a new attempt), another plan or method, a credit, Pro and a
// sign-out clear it, and a Pro account never sees it. The store and the sheet
// cannot be built off Windows, so their use of the rule is read here.

func TestCardPaymentReport(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("card payment report tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "card-payment-report-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "card-payment-report-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build card payment report tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("card payment report: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="CardPaymentReport.h" />`) {
		t.Error("App.vcxproj does not list CardPaymentReport.h")
	}
}

// The store's code without comments and whitespace, by definition.
func balanceStoreBody(t *testing.T, signature string) string {
	t.Helper()
	store := stripComments(readAppSource(t, "SubscriptionBalance.cpp"))
	return cryptoPayCode(definitionBody(t, "SubscriptionBalance.cpp", store, signature))
}

// The store numbers the purchase attempts, makes the report, keeps the newest
// attempt's refusal (ending that attempt's confirmation), publishes it with
// the confirmation state, and clears it for a credit, another plan or method,
// Pro and a sign-out.
func TestBalanceStoreKeepsTheCardRefusalPastTheSheet(t *testing.T) {
	header := cryptoPayCode(readAppSource(t, "SubscriptionBalance.h"))
	requireCryptoPay(t, "SubscriptionBalance.h", header,
		`#include"CardPaymentReport.h"`,
		"std::optional<CardRefusal>cardRefusal;",
		"uint64_tBeginPurchaseAttempt(boolyearly);",
		"voidReportCardPayment(uint64_tattempt,std::stringconst&subscriptionId,std::stringconst&sessionId);",
		"voidKeepCardRefusal(uint64_tattempt,std::stringconst&code,PaymentRefusalTextconst&text);",
		"voidClearCardRefusal();",
		"voidMarkCardRefusalEventSent(uint64_tattempt);",
		"BalancePollStateCurrentPoll()const{return{gate_.Confirming(),timedOut_,cardRefusals_.Shown(snapshot_.isPro)};}",
		"CardRefusalKeepercardRefusals_;")

	requireCryptoPayOrder(t, "SubscriptionBalanceStore::BeginPurchaseAttempt",
		balanceStoreBody(t, "uint64_t SubscriptionBalanceStore::BeginPurchaseAttempt("),
		"constboolhadRefusal=cardRefusals_.Has();",
		"constuint64_tattempt=cardRefusals_.BeginAttempt(yearly);",
		"if(hadRefusal)Publish();",
		"returnattempt;")

	report := balanceStoreBody(t, "void SubscriptionBalanceStore::ReportCardPayment(")
	requireCryptoPayOrder(t, "SubscriptionBalanceStore::ReportCardPayment", report,
		"if(!sdk_.IsLoggedIn())return;",
		"if(!subscriptionId.empty()){args.subscription_id=subscriptionId;}elseif(!sessionId.empty()){args.session_id=sessionId;}else{return;}",
		"constuint32_tgeneration=generation_;",
		"sdk_.api().reportStripePurchase(",
		`result?PurchaseReportRefusalFor(*result,"something_went_wrong"):std::nullopt;`,
		"queue.TryEnqueue([this,generation,attempt,credited,text,code]{",
		"if(generation!=generation_)return;",
		"switch(CardReportOutcomeFor(credited,text.has_value())){",
		"caseCardReportOutcome::Credited:ClearCardRefusal();Fetch();return;",
		"caseCardReportOutcome::Refused:KeepCardRefusal(attempt,code,*text);return;",
		"caseCardReportOutcome::Confirming:return;")
	if strings.Contains(report, "error->message") {
		t.Error("SubscriptionBalanceStore::ReportCardPayment reads the refusal's message itself")
	}

	requireCryptoPayOrder(t, "SubscriptionBalanceStore::KeepCardRefusal",
		balanceStoreBody(t, "void SubscriptionBalanceStore::KeepCardRefusal("),
		"if(!cardRefusals_.Keep(attempt,code,text,snapshot_.isPro))return;",
		"StopConfirmation(false);",
		"timedOut_=false;",
		"EnsureBackgroundPolling();",
		"Publish();")
	requireCryptoPay(t, "SubscriptionBalanceStore::ClearCardRefusal",
		balanceStoreBody(t, "void SubscriptionBalanceStore::ClearCardRefusal("),
		"if(cardRefusals_.Clear())Publish();")
	requireCryptoPay(t, "SubscriptionBalanceStore::MarkCardRefusalEventSent",
		balanceStoreBody(t, "void SubscriptionBalanceStore::MarkCardRefusalEventSent("),
		"cardRefusals_.MarkEventSent(attempt);")
	// Pro, by this purchase or any other, clears it for good; every push
	// carries it with the confirmation state
	requireCryptoPayOrder(t, "SubscriptionBalanceStore::Publish",
		balanceStoreBody(t, "void SubscriptionBalanceStore::Publish("),
		"if(snapshot_.isPro)cardRefusals_.Clear();",
		"if(onChange_)onChange_(snapshot_,CurrentPoll());")
	// a session's refusal is its network's own
	for _, signature := range []string{"void SubscriptionBalanceStore::Start(", "void SubscriptionBalanceStore::Stop("} {
		requireCryptoPayOrder(t, signature, balanceStoreBody(t, signature), "cardRefusals_.Clear();", "Publish();")
	}
	// a running confirmation is still kept by StartConfirmationPolling
	requireCryptoPayOrder(t, "SubscriptionBalanceStore::StartConfirmationPolling",
		balanceStoreBody(t, "void SubscriptionBalanceStore::StartConfirmationPolling("),
		"if(gate_.Confirming())return;",
		"timedOut_=false;",
		"gate_.Start(NowMillis());")
}

// Get Pro opens on a kept refusal (not over a running confirmation); the
// sheets an onboarding page or the invoice row opens drop it (a new attempt,
// or a coin); an open sheet takes it from every push, once; a coin's purchase
// is a new attempt.
func TestUpgradeSheetShowsAKeptRefusalWhenGetProOpens(t *testing.T) {
	create := trialCardSheetBody(t, "std::shared_ptr<UpgradeSheet> UpgradeSheet::Create(")
	requireCryptoPayOrder(t, "UpgradeSheet::Create", create,
		"autosheet=Make(root,sdk,balance,invoice,freeRefresh);",
		"constBalancePollStatepoll=balance.CurrentPoll();",
		"if(!poll.confirming)sheet->ApplyCardRefusal(poll.cardRefusal);",
		"returnsheet;")

	checkout := trialCardSheetBody(t, "std::shared_ptr<UpgradeSheet> UpgradeSheet::CreateForCheckout(")
	requireCryptoPayOrder(t, "UpgradeSheet::CreateForCheckout", checkout,
		"autosheet=Make(root,sdk,balance,invoice,false);",
		"balance.ClearCardRefusal();",
		"sheet->BeginCheckout();")
	invoice := trialCardSheetBody(t, "std::shared_ptr<UpgradeSheet> UpgradeSheet::CreateForInvoice(")
	requireCryptoPayOrder(t, "UpgradeSheet::CreateForInvoice", invoice,
		"autosheet=Make(root,sdk,balance,invoice,false);",
		"balance.ClearCardRefusal();",
		"sheet->ShowInvoice();")
	for name, body := range map[string]string{"CreateForCheckout": checkout, "CreateForInvoice": invoice} {
		if strings.Contains(body, "Create(root") {
			t.Errorf("UpgradeSheet::%s builds through Create, which would show a kept refusal it then drops", name)
		}
	}

	requireCryptoPay(t, "UpgradeSheet::ApplyCardRefusal",
		trialCardSheetBody(t, "void UpgradeSheet::ApplyCardRefusal("),
		"if(!refusal||refusal->attempt==refusalAttemptShown_)return;RefuseCard(*refusal);")
	onBalance := trialCardSheetBody(t, "void UpgradeSheet::OnBalance(")
	if !strings.Contains(onBalance, "BalancePollStateconst&poll){ApplyCardRefusal(poll.cardRefusal);") {
		t.Error("UpgradeSheet::OnBalance does not take the store's kept refusal first")
	}
	requireCryptoPayOrder(t, "UpgradeSheet::BeginCryptoInvoice",
		trialCardSheetBody(t, "void UpgradeSheet::BeginCryptoInvoice("),
		"if(checkingOut_||!cryptopay::IsCoin(asset)||!sdk_.IsLoggedIn())return;",
		"cardRefused_=false;balance_.BeginPurchaseAttempt(true);",
		"if(ShowInvoiceInProgress())return;")

	// the window hands every push's confirmation state, refusal and all, to
	// the open sheet
	window := readAppSource(t, "MainWindow.xaml.cpp")
	for _, want := range []string{"balancePoll_ = poll;", "if (upgradeSheet_) upgradeSheet_->OnBalance(balance_, balancePoll_);"} {
		if !strings.Contains(window, want) {
			t.Errorf("MainWindow.xaml.cpp: missing %s", want)
		}
	}
}
