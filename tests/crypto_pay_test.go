// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"
)

// Crypto payments (crypto-pay CONTRACT §4-§6, mmm/BTCPAY.md §17.6): the
// payment-method row Card · USDC on Solana · Zcash · Monero · More coins (TAO,
// Bitcoin, ETH, SOL) on the upgrade sheet and the onboarding plan and offer
// pages, the native invoice on the upgrade sheet, and the Account rows. The
// rules are App/CryptoPayPresentation.h, run here by
// app/tools/crypto-pay-tests.cpp; the sdk adapter App/CryptoPaySdk.h is built
// against a generated header that has the crypto-pay surface wherever one is;
// and the WinRT surfaces, which only a Windows build compiles, are read from
// their sources.

// The keys the crypto surfaces look up by name (CONTRACT §5, the lead's
// additions: crypto_payment_in_progress, crypto_does_not_renew; the More coins
// words of 2026-11-10). crypto_fine_print_data is the data packs' fine print,
// and Windows sells no data pack. Each coin's own lines are the sdk's
// (cryptoPaySdkLineKeys).
var cryptoPayKeys = []string{
	"pay_method_label", "pay_method_card", "pay_method_card_only_monthly", "pay_method_unavailable",
	"pay_method_more_coins_a11y", "crypto_pay_button", "crypto_fine_print_yearly",
	"crypto_invoice_send_exactly", "crypto_invoice_to_address", "crypto_invoice_memo",
	"crypto_invoice_open_wallet", "crypto_invoice_copy", "crypto_invoice_copied",
	"crypto_invoice_expires_in", "crypto_invoice_refresh_quote", "crypto_invoice_quote_expired",
	"crypto_invoice_remaining", "crypto_status_waiting", "crypto_status_seen", "crypto_status_confirming",
	"crypto_status_paid", "crypto_status_review", "crypto_invoice_pay_another_way",
	"crypto_invoice_wallets", "crypto_invoice_zcash_exchange_note", "crypto_invoice_unavailable",
	"crypto_term_ended_notice", "crypto_term_renew",
	"crypto_invoice_send_exactly_to_address", "crypto_invoice_copy_address", "crypto_invoice_copy_amount",
	"crypto_payment_in_progress", "crypto_does_not_renew",
}

// The labels the sdk hands out with each tab and coin, a store key with its
// English (CryptoPayViews.cpp PaymentLabel looks each up): the coins'
// (cryptoAssetCoins) and More coins' (cryptoPaymentTabLayouts) in the sdk's
// crypto_pay.go.
var cryptoPaySdkLabelKeys = []string{
	"pay_method_usdc_solana", "pay_method_zcash", "pay_method_monero", "pay_method_more_coins",
	"pay_method_tao", "pay_method_bitcoin", "pay_method_eth", "pay_method_sol",
}

// Each coin's own lines, which the sdk hands out as a store key with its
// English (urnet::cryptoAssetLinesFor; CryptoPayViews.cpp PaymentLabel looks
// each up): its network (the invoice's first line, and the line under it in
// the More coins list), its network's warning beside the address, and TAO's
// note under its address-only QR, as cryptoAssetCoins in the sdk's
// crypto_pay.go spells them.
var cryptoPaySdkLineKeys = []string{
	"crypto_network_usdc", "crypto_network_zcash", "crypto_network_monero", "crypto_network_tao",
	"crypto_network_bitcoin", "crypto_network_eth", "crypto_network_sol",
	"usdc_solana_network_only", "crypto_invoice_tao_network_only", "crypto_invoice_bitcoin_network_only",
	"crypto_invoice_eth_network_only", "crypto_invoice_sol_network_only",
	"crypto_invoice_tao_no_payment_link",
}

// The four marks of More coins, by the sdk's icon keys (CryptoIconKey*).
var cryptoPayMoreCoinsMarks = []string{"coin_tao", "coin_btc", "coin_eth", "coin_sol"}

// The sources of the crypto surfaces.
var cryptoPaySources = []string{
	"CryptoPayPresentation.h", "CryptoPaySdk.h", "CryptoInvoice.h", "CryptoInvoice.cpp",
	"CryptoPayViews.h", "CryptoPayViews.cpp", "BalanceSheets.cpp", "Onboarding.cpp",
	"PlanPicker.cpp", "MainWindow.xaml.cpp",
}

// Compile the crypto pay spec (app/tools/crypto-pay-tests.cpp, header-only,
// with the vendored QR encoder). extra adds compiler arguments ahead of the
// sources.
func buildCryptoPayTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("crypto pay tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app")
	program := filepath.Join(t.TempDir(), "crypto-pay-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(appDir, "src", "App"),
		"-I" + filepath.Join(appDir, "third_party", "qrcodegen")}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(appDir, "tools", "crypto-pay-tests.cpp"),
		filepath.Join(appDir, "third_party", "qrcodegen", "qrcodegen.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build crypto pay tests: %v\n%s", err, output)
	}
	return program
}

func runCryptoPayTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("crypto pay: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The method row's rules, the invoice's display for every state of the sdk's
// controller, the countdown, the uri gate, the QR's layout for real payment
// uris, the "Payment in progress" row and a crypto year's plan rows.
func TestCryptoPayPresentation(t *testing.T) {
	runCryptoPayTests(t, buildCryptoPayTests(t))
}

// The same spec with App/CryptoPaySdk.h built against a generated
// urnetwork_sdk.hpp that has the crypto-pay surface, which pins every field
// and function the adapter names to the wrapper: URNETWORK_SDK_INCLUDE, the
// vendored sdk (app/third_party/urnetwork-sdk/<arch>, git-ignored), or the sdk
// checkout beside this repository. A host with none of them, or only headers
// from before crypto pay, skips.
func TestCryptoPayAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := cryptoPaySdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the crypto-pay surface (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	extra := []string{"-DURNW_CRYPTO_PAY_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runCryptoPayTests(t, buildCryptoPayTests(t, extra...))
}

// The directory of a urnetwork_sdk.hpp that declares the crypto invoice's
// state, or "".
func cryptoPaySdkHeaderDir(t *testing.T, root string) string {
	t.Helper()
	explicit := os.Getenv("URNETWORK_SDK_INCLUDE")
	candidates := []string{}
	if explicit != "" {
		candidates = append(candidates, explicit)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		candidates = append(candidates, filepath.Join(root, "app", "third_party", "urnetwork-sdk", arch))
	}
	// the monoroot: the sdk's own checkout beside this repository, when there is one
	candidates = append(candidates, filepath.Join(filepath.Dir(root), "sdk", "cgo", "include"))
	for _, dir := range candidates {
		header, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
		if err != nil {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: %v", explicit, err)
			}
			continue
		}
		if !strings.Contains(string(header), "struct CryptoInvoiceState {") {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnet::CryptoInvoiceState is not declared", explicit)
			}
			t.Logf("%s: predates the crypto invoice", dir)
			continue
		}
		return dir
	}
	return ""
}

// The code of a source without comments and whitespace, so a statement reads
// the same however it is wrapped.
func cryptoPayCode(source string) string {
	return strings.Join(strings.Fields(stripComments(source)), "")
}

// Each of `wants`, in this order, in `body` (already through cryptoPayCode).
func requireCryptoPayOrder(t *testing.T, where, body string, wants ...string) {
	t.Helper()
	at := 0
	for _, want := range wants {
		next := strings.Index(body[at:], want)
		if next < 0 {
			if strings.Contains(body, want) {
				t.Errorf("%s: %s is out of order", where, want)
			} else {
				t.Errorf("%s: missing %s", where, want)
			}
			return
		}
		at += next + len(want)
	}
}

func requireCryptoPay(t *testing.T, where, body string, wants ...string) {
	t.Helper()
	for _, want := range wants {
		if !strings.Contains(body, want) {
			t.Errorf("%s: missing %s", where, want)
		}
	}
}

// The new units are part of the app build.
func TestCryptoPayIsInTheProject(t *testing.T) {
	project := readAppSource(t, "App.vcxproj")
	for _, want := range []string{
		`<ClCompile Include="CryptoInvoice.cpp" />`,
		`<ClCompile Include="CryptoPayViews.cpp" />`,
		`<ClInclude Include="CryptoInvoice.h" />`,
		`<ClInclude Include="CryptoPayViews.h" />`,
		`<ClInclude Include="CryptoPayPresentation.h" />`,
		`<ClInclude Include="CryptoPaySdk.h" />`,
	} {
		if !strings.Contains(project, want) {
			t.Errorf("App.vcxproj: missing %s", want)
		}
	}
}

// The upgrade sheet: the row after the plan cards and directly above the buy
// button; a coin buys through its invoice (the welcome offer while active),
// waiting on the products page; a start refused for a guest goes to the
// conversion, asset_unavailable marks the row and stays selectable, anything
// else is the sheet's refusal line; the invoice page runs the controller only
// while it shows; a paid invoice takes the card's success path; one invoice at
// a time, and Close only hides it (CONTRACT §6.2.1); and every event names the
// method's store.
func TestCryptoPayUpgradeSheetWiring(t *testing.T) {
	sheets := stripComments(readAppSource(t, "BalanceSheets.cpp"))
	body := func(signature string) string {
		return cryptoPayCode(definitionBody(t, "BalanceSheets.cpp", sheets, signature))
	}

	build := body("void UpgradeSheet::Build(")
	requireCryptoPayOrder(t, "UpgradeSheet::Build", build,
		"autoplans=plans_.Build();",
		"automethodRow=methods_.Build();",
		"productsPanel_.Children().Append(methodRow);",
		"subscribeButton_=Button();",
		"productsPanel_.Children().Append(subscribeRow);",
		`cryptoFinePrint_=MakeText(Loc("crypto_fine_print_yearly")`,
		"ApplyMethods();",
		"invoicePanel_=invoiceView_.Build(",
		"content.Children().Append(invoicePanel_);")
	requireCryptoPay(t, "UpgradeSheet::Build", build,
		"if(cryptopay::IsCoin(self->methods_.Method())){self->BeginCryptoInvoice();}else{self->BeginCheckout();}",
		"self->invoice_.SetPaymentMethod(method);",
		"if(autoself=weak.lock())self->OpenInWallet();",
		"if(autoself=weak.lock())self->invoice_.Requote();",
		"if(autoself=weak.lock())self->PayAnotherWay();")
	// Close only hides the invoice: it stays saved, "Payment in progress" brings
	// it back, and only Pay another way gives it up (CONTRACT §6.2.1)
	closed := cryptoPayCode(handlerSource(t, "BalanceSheets.cpp", sheets, "dialog_.Closed("))
	requireCryptoPayOrder(t, "UpgradeSheet::Build's Closed", closed,
		"self->closed_=true;",
		"self->cryptoStarting_=false;",
		"self->invoice_.SetShowing(false);",
		"self->invoiceView_.Stop();")
	if strings.Contains(closed, "Dismiss()") {
		t.Error("UpgradeSheet's Closed dismisses the invoice; Close only hides it, Pay another way gives it up")
	}

	// one invoice at a time (§6.2.1): one in progress is shown, never replaced,
	// on both coin paths (the sheet's coin button and onboarding's)
	inProgress := body("bool UpgradeSheet::ShowInvoiceInProgress(")
	requireCryptoPayOrder(t, "UpgradeSheet::ShowInvoiceInProgress", inProgress,
		"if(!invoice_.Resume()||!cryptopay::InvoiceInProgress(invoice_.Current()))returnfalse;",
		"invoiceResumed_=true;",
		"purchaseMethod_=invoice_.Current().asset;",
		"ShowInvoice();returntrue;")
	if strings.Contains(inProgress, "StartInvoice(") || strings.Contains(inProgress, "EmitPurchase(") {
		t.Error("ShowInvoiceInProgress starts or reports a purchase; the invoice it shows began earlier")
	}
	begin := body("void UpgradeSheet::BeginCryptoInvoice(")
	requireCryptoPayOrder(t, "UpgradeSheet::BeginCryptoInvoice", begin,
		"if(checkingOut_||!cryptopay::IsCoin(asset)||!sdk_.IsLoggedIn())return;",
		"if(ShowInvoiceInProgress())return;",
		"cryptoStarting_=true;",
		"purchaseMethod_=asset;",
		`EmitPurchase("started");`,
		"invoice_.SetShowing(true);",
		"constchar*plan=cryptopay::CryptoPlanFor(balance_.Current().offer.active);",
		"if(!invoice_.StartInvoice(asset,plan)){",
		"OnInvoice(invoice_.Current());")

	onInvoice := body("void UpgradeSheet::OnInvoice(")
	requireCryptoPayOrder(t, "UpgradeSheet::OnInvoice", onInvoice,
		"if(closed_)return;",
		"if(cryptoStarting_){",
		"if(route==InvoiceRoute::Starting||route==InvoiceRoute::None)return;",
		"view.asset!=purchaseMethod_",
		"if(route==InvoiceRoute::Unavailable||route==InvoiceRoute::Refused||route==InvoiceRoute::Unknown){RefuseCryptoStart(view);return;}",
		"ShowPage(Page::Invoice);",
		"if(page_!=Page::Invoice)return;",
		"RenderInvoice(view);")

	refuse := body("void UpgradeSheet::RefuseCryptoStart(")
	requireCryptoPayOrder(t, "UpgradeSheet::RefuseCryptoStart", refuse,
		"if(PurchaseRefusalFor(view.errorCode)==PurchaseRefusal::AddSignIn){RefuseForGuest();return;}",
		"if(view.errorCode==cryptopay::kErrorAssetUnavailable){ResetPurchaseAffordances();methods_.SetUnavailable(purchaseMethod_);return;}",
		`PaymentRefusalTextFor(view.errorCode,view.errorMessage,"something_went_wrong");`,
		"ShowCheckoutError(hstring{PaymentRefusalMessage(Localized(refusal.key),Widen(refusal.detail))});")

	render := body("void UpgradeSheet::RenderInvoice(")
	requireCryptoPayOrder(t, "UpgradeSheet::RenderInvoice", render,
		"if(route==InvoiceRoute::None){ShowPage(Page::Products);ApplyPrices();return;}",
		"if(route==InvoiceRoute::Paid){",
		"invoiceView_.Render(view,false,{});",
		"invoicePaid_=true;balance_.StartConfirmationPolling();",
		"uriAllowed_=cryptopay::UriAllowed(view);",
		"invoiceView_.Render(view,uriAllowed_,wallets_);")

	// only a uri the sdk's check passed reaches the OS, and its answer is read
	open := body("winrt::fire_and_forget UpgradeSheet::OpenInWallet(")
	requireCryptoPayOrder(t, "UpgradeSheet::OpenInWallet", open,
		"if(!cryptopay::UriAllowed(view))co_return;",
		"launched=co_awaitwinrt::Windows::System::Launcher::LaunchUriAsync(Uri(H(view.uri)));")
	if strings.Contains(open, "ShowCheckoutError") || strings.Contains(open, "errorText") {
		t.Error("UpgradeSheet::OpenInWallet says something when no wallet takes the uri; the QR stays and nothing alarming is said")
	}

	another := body("void UpgradeSheet::PayAnotherWay(")
	requireCryptoPayOrder(t, "UpgradeSheet::PayAnotherWay", another,
		"wantedMethod_=cryptopay::kMethodCard;",
		"invoice_.SetPaymentMethod(cryptopay::kMethodCard);",
		"methods_.Select(cryptopay::kMethodCard);",
		"ShowPage(Page::Products);",
		"invoice_.Dismiss();")

	show := body("void UpgradeSheet::ShowPage(")
	requireCryptoPay(t, "UpgradeSheet::ShowPage", show,
		"invoice_.SetShowing(page==Page::Invoice||cryptoStarting_);")

	balance := body("void UpgradeSheet::OnBalance(")
	requireCryptoPay(t, "UpgradeSheet::OnBalance", balance,
		"constboolpaidInvoice=page_==Page::Invoice&&invoicePaid_;",
		"if(snapshot.isPro){ShowPage(Page::Success);return;}",
		"if((page_==Page::Waiting||paidInvoice)&&poll.timedOut)ShowPage(Page::TimedOut);")

	method := body("void UpgradeSheet::ApplyMethod(")
	requireCryptoPay(t, "UpgradeSheet::ApplyMethod", method,
		"plans_.SetCardOnlyMonthly(cryptopay::MonthlyLocked(method));",
		// no trial or renewal wording while a coin is selected (§6.1)
		"constboolcardTerms=cryptopay::CardTermsShown(method);plans_.SetCardTermsShown(cardTerms);offerLine_.SetCardTermsShown(cardTerms);",
		"subscribeButton_.Content(winrt::box_value(CtaLabel()));",
		"cryptoFinePrint_.Visibility(coin?Visibility::Visible:Visibility::Collapsed);")
	// the offer's deadline stays for a coin: only its terms go
	requireCryptoPay(t, "UpgradeSheet::ApplyPrices", body("void UpgradeSheet::ApplyPrices("),
		"root.Visibility(snapshot.offer.active?Visibility::Visible:Visibility::Collapsed);")
	requireCryptoPay(t, "UpgradeSheet::CtaLabel", body("hstring UpgradeSheet::CtaLabel("),
		`Format("crypto_pay_button",PaymentMethodName(method))`,
		"returnPlanPicker::CtaLabel(plans_.Yearly());")
	// the balance's two lists: a coin that cannot be quoted now greys out in its
	// place instead of vanishing (with More coins, when all four are)
	requireCryptoPay(t, "UpgradeSheet::ApplyMethods", body("void UpgradeSheet::ApplyMethods("),
		"cryptopay::PaymentTabsFor(snapshot.cryptoAssets,snapshot.cryptoUnavailableAssets,snapshot.offer.active);",
		"methods_.SetTabs(tabs,wantedMethod_.empty()?invoice_.PaymentMethod():wantedMethod_);")

	emit := body("void UpgradeSheet::EmitPurchase(")
	requireCryptoPay(t, "UpgradeSheet::EmitPurchase", emit,
		"conststd::stringstore=cryptopay::MethodEventStore(purchaseMethod_);",
		// a coin's product is the sdk's (pro_yearly_<ticker>), Card's its plan's
		"conststd::stringcoinProduct=cryptopay::MethodEventProduct(purchaseMethod_);",
		"conststd::stringproduct=coinProduct.empty()?std::string(PlanProduct(yearly)):coinProduct;",
		"constbooltrial=cryptopay::PurchaseHasTrial(purchaseMethod_,yearly);",
		"if(coin&&0<purchasePriceUsd_)price=purchasePriceUsd_;",
		"events.PurchaseStarted(store,product,", "events.PurchaseCompleted(store,product,",
		"events.PurchaseCancelled(store,product,", "events.PurchaseFailed(store,product,")
	if strings.Contains(emit, "(store,PlanProduct(") {
		t.Error("UpgradeSheet::EmitPurchase names the plan's product for every method; a coin's purchase names the sdk's pro_yearly_<ticker>")
	}
	if strings.Contains(emit, `"stripe"`) || strings.Contains(sheets, "kStoreStripe") {
		t.Error("UpgradeSheet::EmitPurchase names the Stripe store itself; the method's store comes from cryptopay::MethodEventStore")
	}
	requireCryptoPay(t, "UpgradeSheet::BeginCheckout", body("void UpgradeSheet::BeginCheckout("),
		"purchaseMethod_=cryptopay::kMethodCard;")

	create := body("std::shared_ptr<UpgradeSheet> UpgradeSheet::CreateForCheckout(")
	requireCryptoPayOrder(t, "UpgradeSheet::CreateForCheckout", create,
		"sheet->methods_.Select(method);",
		"if(cryptopay::IsCoin(method)){if(cryptopay::IsCoin(sheet->methods_.Method())){sheet->BeginCryptoInvoice();}else{sheet->ShowInvoiceInProgress();}returnsheet;}",
		"sheet->BeginCheckout();")
}

// The sheet's views: the QR drawn only for a uri the gate passed, at level M,
// one rectangle per run; Copy through the app's clipboard helper with Copied
// for a moment; the term notice the sdk's rule; "Payment in progress" for an
// open invoice.
func TestCryptoPayViewsWiring(t *testing.T) {
	views := stripComments(readAppSource(t, "CryptoPayViews.cpp"))
	body := func(signature string) string {
		return cryptoPayCode(definitionBody(t, "CryptoPayViews.cpp", views, signature))
	}
	render := body("void CryptoInvoicePanel::Render(")
	requireCryptoPayOrder(t, "CryptoInvoicePanel::Render", render,
		"constcryptopay::CryptoInvoiceDisplaydisplay=cryptopay::DisplayFor(view,uriAllowed);",
		// the coin's lines are the sdk's, looked up as its labels are
		"state.networkLine.Text(hstring{LineText(display.networkLine)});",
		"Show(state.busyLine,display.busy);",
		"Show(state.byHandLine,display.showByHandLine);",
		// the QR is the sdk's qr text: a checked uri, or TAO's address alone
		"if(display.showQr){if(state.qrText!=display.qrText)DrawQr(state,display.qrText);}",
		"state.noLinkNote.Text(hstring{LineText(display.noPaymentLinkNote)});",
		"Show(state.noLinkNote,qrDrawn&&!display.noPaymentLinkNote.key.empty());",
		"Show(state.openWallet,display.showOpenWallet);",
		"Show(state.amountBlock,display.showAmount);",
		"Show(state.addressBlock,display.showAddress);",
		"state.networkWarning.Text(hstring{LineText(display.networkWarning)});",
		"Show(state.networkWarning,display.showNetworkWarning);",
		"Show(state.memoBlock,display.showMemo);",
		"Show(state.refreshButton,display.showRefresh);",
		// the sdk's steps, row by row, each drawn by its progress
		"Show(state.steps,display.showSteps);",
		"constboollisted=index<display.steps.size();",
		"constcryptopay::PaymentStepView&step=display.steps[index];",
		"constbooldone=step.progress==cryptopay::kStepDone;",
		"constboolcurrent=step.progress==cryptopay::kStepCurrent;",
		"constintannounced=cryptopay::AnnouncedStep(display.steps);",
		"Show(state.payAnotherWay,display.showPayAnotherWay);")
	// Open in wallet hands over a uri, never the qr text: TAO's address-only QR
	// has no Open in wallet
	if strings.Contains(render, "Show(state.openWallet,display.showQr)") {
		t.Error("CryptoInvoicePanel::Render shows Open in wallet with the QR; a coin with no payment link has a QR and nothing to open")
	}
	// which step is where is the sdk's: nothing here counts steps by index
	for _, gone := range []string{"StepsFor(", "StatusStep("} {
		if strings.Contains(render, gone) {
			t.Errorf("CryptoInvoicePanel::Render decides the steps itself (%s); they are the sdk's (CryptoInvoiceView.steps)", gone)
		}
	}
	requireCryptoPay(t, "CryptoInvoicePanel::LabelCopyButtons", body("void CryptoInvoicePanel::LabelCopyButtons("),
		`Loc(state.copySaysWhat?"crypto_invoice_copy_amount":"crypto_invoice_copy")`,
		`Loc(state.copySaysWhat?"crypto_invoice_copy_address":"crypto_invoice_copy")`)
	requireCryptoPay(t, "CryptoInvoicePanel::DrawQr", body("void CryptoInvoicePanel::DrawQr("),
		"qrcodegen::QrCode::encodeText(text.c_str(),qrcodegen::QrCode::Ecc::MEDIUM);",
		"cryptopay::QrLayoutFor(moduleCount,state.qrMaxSide);",
		"ExtenderQrRunsFor(moduleCount,dark,0,0)")
	requireCryptoPay(t, "CopyText", cryptoPayCode(views), "rows::CopyToClipboard(text);")
	requireCryptoPay(t, "CryptoInvoicePanel::ShowCopied", body("void CryptoInvoicePanel::ShowCopied("),
		`Loc("crypto_invoice_copied")`, "state.copiedTimer.Start();")
	requireCryptoPay(t, "CryptoAccountRows::ApplyBalance", body("void CryptoAccountRows::ApplyBalance("),
		"constboolnotice=!balance.guest&&cryptopay::TermNoticeDue(balance.cryptoTerm);",
		`Format("crypto_term_ended_notice",date)`,
		"cryptopay::DoesNotRenewShows(balance.subscriptionStoreFamily,notice)")
	requireCryptoPayOrder(t, "CryptoAccountRows::ApplyInvoice", body("void CryptoAccountRows::ApplyInvoice("),
		"constboolinProgress=cryptopay::ShowsPaymentInProgress(view);",
		"conststd::stringnoteKey=cryptopay::InProgressNoteKey(view);",
		"state.inProgressValue.Text(hstring{LineText(view.lines.networkLine)});")
	requireCryptoPayOrder(t, "LineText", body("std::wstring LineText("),
		"if(line.key.empty())returnstd::wstring();",
		"returnPaymentLabel(line.key,line.english);")
	requireCryptoPay(t, "PaymentMethodRow::Build", body("StackPanel PaymentMethodRow::Build("),
		`automation::AutomationProperties::SetName(state.bar,Loc("pay_method_label"));`,
		"if(!self||self->applying)return;")
}

// The session's invoice store: the listener only hops to the UI thread, the
// state is read there under the controller's generation, the controller runs
// only while an invoice shows in a presenting window, the listener goes before
// the controller closes, and a sign-out takes the saved invoice with it.
func TestCryptoInvoiceStoreWiring(t *testing.T) {
	store := stripComments(readAppSource(t, "CryptoInvoice.cpp"))
	body := func(signature string) string {
		return cryptoPayCode(definitionBody(t, "CryptoInvoice.cpp", store, signature))
	}
	start := body("void CryptoInvoiceStore::Start(")
	requireCryptoPayOrder(t, "CryptoInvoiceStore::Start", start,
		"Close();",
		"if(!queue_||!sdk_.apiReady()||!sdk_.IsLoggedIn()){",
		"controller_.emplace(urnet::newCryptoInvoiceViewController(sdk_.api(),storage));",
		"constuint64_tgeneration=++generation_;",
		"sub_.emplace(controller_->addCryptoInvoiceListener([this,queue,alive,generation]{queue.TryEnqueue([this,alive,generation]{if(!*alive||generation!=generation_)return;Read();});}));",
		"Resume();",
		"ApplyRunning();")
	requireCryptoPayOrder(t, "CryptoInvoiceStore::Close", body("void CryptoInvoiceStore::Close("),
		"++generation_;", "sub_.reset();", "controller_->close();")
	requireCryptoPay(t, "CryptoInvoiceStore::ApplyRunning", body("void CryptoInvoiceStore::ApplyRunning("),
		"constboolrun=controller_.has_value()&&presenting_&&showing_;",
		"controller_->start();", "controller_->stop();")
	requireCryptoPayOrder(t, "CryptoInvoiceStore::Stop", body("void CryptoInvoiceStore::Stop("),
		"Close();", "if(signedOut){", "std::filesystem::remove_all(StorageDir(),error);")
	requireCryptoPay(t, "CryptoInvoiceStore::StartInvoice", body("bool CryptoInvoiceStore::StartInvoice("),
		"controller_->startInvoice(asset,plan,std::string(),std::string());")
	read := body("void CryptoInvoiceStore::Read(")
	requireCryptoPay(t, "CryptoInvoiceStore::Read", read, "view=cryptopay::ViewFromSdk(*state);")
	// never the intent, its token, an address or an amount in the log
	for _, leak := range []string{"intentId", "address", "amount", "uri", "token"} {
		if regexp.MustCompile(`Log(Info|Warn|Error)\([^;]*view\.` + leak).MatchString(read) {
			t.Errorf("CryptoInvoiceStore::Read logs the invoice's %s", leak)
		}
	}
}

// The store's life follows the session and the window, and the window follows
// the store: AppController opens it at sign-in and closes it at sign-out (the
// saved invoice going with the sign-in) and at quit (the invoice staying), and
// runs it only while the window presents; the window paints the Account rows
// and the open sheet from it, resumes it when the Account page appears, and
// sends a guest to the conversion before any crypto sheet.
func TestCryptoPayAppAndWindowWiring(t *testing.T) {
	app := stripComments(readAppSource(t, "AppController.cpp"))
	auth := cryptoPayCode(definitionBody(t, "AppController.cpp", app,
		"void AppController::OnAuthState(AuthState state, const std::string& error)"))
	requireCryptoPayOrder(t, "AppController::OnAuthState", auth,
		"if(state==AuthState::LoggedIn){balance_.Start();signedOutNotice_.Clear();}elseif(state==AuthState::LoggedOut&&wasLoggedIn){balance_.Stop();}",
		"if(state==AuthState::LoggedIn){cryptoInvoice_.Start();}elseif(state==AuthState::LoggedOut&&wasLoggedIn){cryptoInvoice_.Stop(true);}")
	requireCryptoPay(t, "AppController::ReconcileWindowPresentation",
		cryptoPayCode(definitionBody(t, "AppController.cpp", app, "void AppController::ReconcileWindowPresentation(")),
		"balance_.SetVisible(active);cryptoInvoice_.SetPresenting(active);")
	requireCryptoPay(t, "AppController.cpp", cryptoPayCode(app),
		"cryptoInvoice_.Initialize(uiThread_);",
		"self->OnCryptoInvoiceChanged(view);",
		"balance_.Stop();cryptoInvoice_.Stop(false);")
	if !strings.Contains(cryptoPayCode(readAppSource(t, "AppController.h")),
		"SubscriptionBalanceStorebalance_{sdk_};CryptoInvoiceStorecryptoInvoice_{sdk_};") {
		t.Error("AppController.h: the crypto invoice store is not owned beside the balance store, after the sdk host")
	}

	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	body := func(signature string) string {
		return cryptoPayCode(definitionBody(t, "MainWindow.xaml.cpp", window, signature))
	}
	requireCryptoPay(t, "MainWindow::LoadCurrentDestination", body("void MainWindow::LoadCurrentDestination("),
		"urnw::pages::CryptoInvoice().Resume();")
	requireCryptoPay(t, "MainWindow.xaml.cpp", cryptoPayCode(window),
		"AccountPlanExtraHost().Children().InsertAt(0,cryptoAccount_->Build());",
		"if(autoself=weak.get())self->ShowCryptoInvoice();",
		"if(urnw::cryptopay::IsCoin(method)){self->ShowCryptoCheckout(method);}else{self->ShowUpgradeCheckout(yearly);}")
	changed := body("void MainWindow::OnCryptoInvoiceChanged(")
	requireCryptoPay(t, "MainWindow::OnCryptoInvoiceChanged", changed,
		"if(cryptoAccount_)cryptoAccount_->ApplyInvoice(view);",
		"if(upgradeSheet_)upgradeSheet_->OnInvoice(view);",
		"Balance().StartConfirmationPolling();")
	requireCryptoPay(t, "MainWindow::SetPresentationActive", body("void MainWindow::SetPresentationActive("),
		"if(active)OnCryptoInvoiceChanged(urnw::pages::CryptoInvoice().Current());")
	requireCryptoPay(t, "MainWindow::ApplyBalance", body("void MainWindow::ApplyBalance("),
		"if(cryptoAccount_)cryptoAccount_->ApplyBalance(balance_);")
	for _, signature := range []string{
		"winrt::fire_and_forget MainWindow::ShowCryptoCheckout(",
		"winrt::fire_and_forget MainWindow::ShowCryptoInvoice(",
	} {
		show := definitionBody(t, "MainWindow.xaml.cpp", window, signature)
		if !strings.Contains(show, "if (balance_.guest)") || !strings.Contains(show, "DivertGuestToConversion(") {
			t.Errorf("%s: a guest is not diverted to the conversion", signature)
		}
		asked := strings.Index(show, "guestSignInRequired = self->upgradeSheet_->GuestSignInRequired();")
		diverted := strings.LastIndex(show, "self->DivertGuestToConversion(")
		closed := strings.Index(show, "self->sheetOpen_ = false;")
		if asked < 0 || diverted < 0 || closed < 0 || diverted < closed {
			t.Errorf("%s does not divert to the conversion once the refused sheet is gone", signature)
		}
	}
}

// The onboarding plan and offer pages: each its own row directly above its
// CTA, one selection, and a coin's CTA opens its invoice with the method's
// store on the offer event.
func TestCryptoPayOnboardingWiring(t *testing.T) {
	onboarding := stripComments(readAppSource(t, "Onboarding.cpp"))
	body := func(signature string) string {
		return cryptoPayCode(definitionBody(t, "Onboarding.cpp", onboarding, signature))
	}
	requireCryptoPayOrder(t, "Onboarding::BuildWelcome", body("StackPanel Onboarding::BuildWelcome("),
		"autoplans=plans_.Build();",
		"automethods=welcomeMethods_.Build();",
		"if(autoself=weak.lock())self->ChooseMethod(method);",
		"checkoutButton_=MakePrimaryButton(",
		`welcomeFinePrint_=MakeText(Loc("crypto_fine_print_yearly")`)
	requireCryptoPayOrder(t, "Onboarding::BuildOffer", body("StackPanel Onboarding::BuildOffer("),
		"automethods=offerMethods_.Build();",
		"if(autoself=weak.lock())self->ChooseMethod(method);",
		"offerCta_=MakePrimaryButton(hstring{});",
		`offerFinePrint_=MakeText(Loc("crypto_fine_print_yearly")`)
	requireCryptoPay(t, "Onboarding::StartCheckout", body("void Onboarding::StartCheckout("),
		"Sdk().events().OfferCtaTapped(PlanName(true),cryptopay::MethodEventStore(method));",
		"actions_.startCheckout(yearly||coin,method);")
	requireCryptoPay(t, "Onboarding::ChooseMethod", body("void Onboarding::ChooseMethod("),
		"CryptoInvoice().SetPaymentMethod(method);",
		"welcomeMethods_.Select(method);offerMethods_.Select(method);")
	requireCryptoPay(t, "Onboarding::ApplyMethods", body("void Onboarding::ApplyMethods("),
		"cryptopay::PaymentTabsFor(balance_.cryptoAssets,balance_.cryptoUnavailableAssets,balance_.offer.active);",
		"welcomeMethods_.SetTabs(tabs,wanted);offerMethods_.SetTabs(tabs,wanted);")
	for _, signature := range []string{"void Onboarding::ApplyOfferCta(", "void Onboarding::ApplyPlanCta("} {
		requireCryptoPay(t, signature, body(signature), `Format("crypto_pay_button",PaymentMethodName(method_))`)
	}
	requireCryptoPayOrder(t, "Onboarding::ApplyMethod", body("void Onboarding::ApplyMethod("),
		"plans_.SetCardOnlyMonthly(cryptopay::MonthlyLocked(method));",
		// no trial or renewal wording while a coin is selected (§6.1)
		"constboolcardTerms=cryptopay::CardTermsShown(method);",
		"plans_.SetCardTermsShown(cardTerms);welcomeOffer_.SetCardTermsShown(cardTerms);offerLines_.SetCardTermsShown(cardTerms);",
		"if(offerThen_)offerThen_.Visibility(cardOnly);if(offerTrial_)offerTrial_.Visibility(cardOnly);",
		"if(welcomeFinePrint_)welcomeFinePrint_.Visibility(coinOnly);if(offerFinePrint_)offerFinePrint_.Visibility(coinOnly);")
	// the offer's deadline stays for a coin: only its trial lines go
	requireCryptoPay(t, "Onboarding::ApplyPrices", body("void Onboarding::ApplyPrices("),
		"root.Visibility(showOffer?Visibility::Visible:Visibility::Collapsed);")
	if strings.Contains(onboarding, `OfferCtaTapped(PlanName(true), "stripe")`) {
		t.Error(`Onboarding.cpp: the offer CTA's event still names "stripe" for every method`)
	}
}

// Monthly is card only: a coin greys the monthly card out, says so on it,
// takes it out of hit testing, and moves the selection to yearly.
func TestCryptoPayPlanPickerLocksMonthly(t *testing.T) {
	picker := stripComments(readAppSource(t, "PlanPicker.cpp"))
	body := func(signature string) string {
		return cryptoPayCode(definitionBody(t, "PlanPicker.cpp", picker, signature))
	}
	print := body("void PlanPicker::Print(")
	requireCryptoPayOrder(t, "PlanPicker::Print", print,
		// no trial or renewal wording while a coin is selected (§6.1), the
		// lines' room kept so the cards keep their height
		"constautoslots=cryptopay::YearlyCardSlots(state.cardTermsShown,texts.yearlySecondaryRenews);",
		"set(block,textOf(slot.line));",
		"block.Opacity(slot.shown?1.0:0.0);",
		"slot.shown?winrt::Microsoft::UI::Xaml::Automation::Peers::AccessibilityView::Content:winrt::Microsoft::UI::Xaml::Automation::Peers::AccessibilityView::Raw",
		"set(state.reserved[index],textOf(slots[index].line));",
		`state.cardOnlyMonthly?Loc("pay_method_card_only_monthly"):texts.monthlyLine`)
	for _, gone := range []string{"set(state.yearlyTrial,", "set(state.yearlySecondary,", "set(state.yearlyEquivalent,"} {
		if strings.Contains(print, gone) {
			t.Errorf("PlanPicker::Print prints a yearly line outside cryptopay::YearlyCardSlots (%q): a coin's card would show trial or renewal wording", gone)
		}
	}
	requireCryptoPay(t, "PlanPicker::SetCardTermsShown", body("void PlanPicker::SetCardTermsShown("),
		"state_->cardTermsShown=shown;Print(*state_);")
	requireCryptoPay(t, "PlanPicker::ApplyHitTest", body("void PlanPicker::ApplyHitTest("),
		"state.monthlyCard.IsHitTestVisible(state.enabled&&!state.cardOnlyMonthly);")
	requireCryptoPay(t, "PlanPicker::SetCardOnlyMonthly", body("void PlanPicker::SetCardOnlyMonthly("),
		"if(cardOnly)state_->yearly=true;")
	requireCryptoPay(t, "PlanPicker::BuildCard", body("Border PlanPicker::BuildCard("),
		"if(!yearly&&self->cardOnlyMonthly)return;")
}

// No renewal or trial wording while a coin is selected (CONTRACT §6.1): a coin
// buys one year once. The plan card loses its trial line and the offer's
// renewal price (PlanPicker, above), the offer blocks their trial timeline and
// terms but not the deadline, and the offer page its renewal and trial lines
// (Onboarding, above). The trial and renewal strings are printed only where
// that rule reaches, and the welcome-offer year is "yearly" to every event.
func TestCryptoPayHidesTrialAndRenewalWordingForACoin(t *testing.T) {
	card := stripComments(readAppSource(t, "OfferCard.cpp"))
	body := func(signature string) string {
		return cryptoPayCode(definitionBody(t, "OfferCard.cpp", card, signature))
	}
	requireCryptoPayOrder(t, "ComposePlanCardTexts", body("PlanCardTexts ComposePlanCardTexts("),
		"if(offer.active){",
		`texts.yearlySecondary=hstring{Format("offer_then_regular_price",`,
		"texts.yearlySecondaryRenews=true;",
		"}else{")
	terms := body("void OfferLines::SetCardTermsShown(")
	requireCryptoPay(t, "OfferLines::SetCardTermsShown", terms,
		"if(timeline_)timeline_.Visibility(visibility);",
		"if(terms_)terms_.Visibility(visibility);")
	if strings.Contains(terms, "deadline_") {
		t.Error("OfferLines::SetCardTermsShown hides the deadline: a coin pays the offer's price until then, so it stays")
	}
	requireCryptoPay(t, "OfferLines::Build", body("StackPanel OfferLines::Build("),
		"timeline_=StackPanel();",
		"root_.Children().Append(timeline_);")

	// the trial and renewal lines are printed only on the cards the rule reaches
	printedAt := map[string][]string{
		`"offer_then_regular_price"`:    {"App/OfferCard.cpp", "App/Onboarding.cpp"},
		`"offer_terms_first_year"`:      {"App/OfferCard.cpp"},
		`"includes_free_trial_days"`:    {"App/OfferCard.cpp", "App/Onboarding.cpp"},
		`"offer_timeline_`:              {"App/OfferCard.cpp"},
		`"plan_terms_yearly"`:           nil,
		`"plan_terms_first_year_price"`: nil,
	}
	for name, code := range authLinkSourceFiles(t) {
		code = stripComments(code)
		for key, files := range printedAt {
			if !strings.Contains(code, key) {
				continue
			}
			allowed := false
			for _, file := range files {
				allowed = allowed || name == file
			}
			if !allowed {
				t.Errorf("%s prints %s: a trial or renewal line outside the cards that hide it while a coin is selected (§6.1)", name, key)
			}
		}
	}

	// events: the welcome-offer year is "yearly" (the server's event schema
	// takes only yearly and monthly); yearly_onboarding is asked of the sdk only
	sheets := stripComments(readAppSource(t, "BalanceSheets.cpp"))
	onboarding := stripComments(readAppSource(t, "Onboarding.cpp"))
	emit := cryptoPayCode(definitionBody(t, "BalanceSheets.cpp", sheets, "void UpgradeSheet::EmitPurchase("))
	requireCryptoPay(t, "UpgradeSheet::EmitPurchase", emit,
		"constboolyearly=coin||plans_.Yearly();",
		"events.PurchaseStarted(store,product,PlanName(yearly),")
	checkout := cryptoPayCode(definitionBody(t, "Onboarding.cpp", onboarding, "void Onboarding::StartCheckout("))
	for name, code := range map[string]string{"UpgradeSheet::EmitPurchase": emit, "Onboarding::StartCheckout": checkout} {
		for _, plan := range []string{"CryptoPlanFor(", "kPlanYearlyOnboarding", `"yearly_onboarding"`} {
			if strings.Contains(code, plan) {
				t.Errorf("%s names %s in an event: the welcome-offer year is reported as yearly", name, plan)
			}
		}
	}
	for name, code := range authLinkSourceFiles(t) {
		if strings.Contains(stripComments(code), "CryptoPlanFor(") && name != "App/BalanceSheets.cpp" &&
			name != "App/CryptoPayPresentation.h" {
			t.Errorf("%s asks for CryptoPlanFor: only the sheet's start (and the method list's items) use the plan a coin buys", name)
		}
	}
}

// The sdk's crypto surface is named in one place: CryptoPaySdk.h reads the
// state, the balance's coins and term, and calls the sdk's rules; the store
// alone holds the controller; the balance store reads through the adapter.
func TestCryptoPaySdkIsReadInOnePlace(t *testing.T) {
	adapterOnly := []string{
		"urnet::paymentMethods(", "urnet::checkCryptoPaymentUri(", "urnet::cryptoWalletSuggestions(",
		"urnet::cryptoTermNoticeDue(", "urnet::paymentMethodAllowsPlan(", ".crypto_assets", ".crypto_term",
		".seconds_left", ".error_code", "urnet::cryptoPaymentTabs(", "urnet::cryptoPaymentTabId(",
		"urnet::cryptoAssetLabelKey(", "urnet::cryptoAssetLabel(", "urnet::cryptoAssetMarket(",
		"urnet::CryptoPaymentTab", ".qr_text", ".icon_keys", ".label_key",
		// the rules every app kept a copy of until crypto wave 3
		"urnet::cryptoAssetLinesFor(", "urnet::CryptoAssetLines", "urnet::cryptoPaymentTabCoin(",
		"urnet::cryptoPaymentShowsMemo(", "urnet::cryptoPaymentUriScheme(", "urnet::cryptoPaymentSteps(",
		"urnet::CryptoPaymentStep", "urnet::cryptoPaymentEventProduct(", "urnet::cryptoPaidAsset(",
		".crypto_unavailable_assets", ".network_line_key", ".network_warning_key",
		".no_payment_link_note_key", ".confirmations_required",
	}
	storeOnly := []string{"urnet::newCryptoInvoiceViewController(", "urnet::CryptoInvoiceViewController"}
	for name, source := range appSourceFiles(t, ".cpp", ".h") {
		code := stripComments(source)
		for _, call := range adapterOnly {
			if strings.Contains(code, call) && name != "CryptoPaySdk.h" {
				t.Errorf("%s names %s: the sdk's crypto surface is read in CryptoPaySdk.h alone", name, call)
			}
		}
		for _, call := range storeOnly {
			if strings.Contains(code, call) && name != "CryptoInvoice.cpp" && name != "CryptoInvoice.h" {
				t.Errorf("%s names %s: only the session's invoice store holds the controller", name, call)
			}
		}
	}
	balance := stripComments(readAppSource(t, "SubscriptionBalance.cpp"))
	requireCryptoPay(t, "SubscriptionBalanceStore::Apply", cryptoPayCode(definitionBody(t, "SubscriptionBalance.cpp", balance,
		"void SubscriptionBalanceStore::Apply(urnet::SubscriptionBalanceResult const& result)")),
		"snapshot_.cryptoAssets=cryptopay::AssetsFromSdk(result);",
		"snapshot_.cryptoUnavailableAssets=cryptopay::UnavailableAssetsFromSdk(result);",
		"snapshot_.cryptoTerm=cryptopay::TermFromSdk(result);")

	// the sdk decides whether there is a selector: no "no tabs" of this app's
	// while the balance lists no coin it can sell, so a coin it cannot quote now
	// still shows greyed
	tabsFor := cryptoPayCode(definitionBody(t, "CryptoPaySdk.h", stripComments(readAppSource(t, "CryptoPaySdk.h")),
		"inline std::vector<PaymentTabView> PaymentTabsFor("))
	requireCryptoPay(t, "PaymentTabsFor", tabsFor,
		"urnet::cryptoPaymentTabs(assets,unavailableAssets,items)",
		"coin.networkLine=LinesFor(coin.asset).networkLine;")
	if strings.Contains(tabsFor, "assets.empty()") || strings.Contains(tabsFor, "CryptoPaymentAssetList{}") {
		t.Error("PaymentTabsFor decides itself that there are no tabs, or passes no unavailable list; the sdk decides from both of the balance's lists")
	}
}

// Every crypto string the surfaces show is looked up from the crypto sources,
// is in the neutral catalog and tagged windows (the catalog tests pin the
// last two for every key; this names the feature's own), and the data packs'
// fine print, which Windows has no surface for, is not.
func TestCryptoPayStrings(t *testing.T) {
	root := repositoryRoot(t)
	code := ""
	for _, name := range cryptoPaySources {
		code += stripComments(readAppSource(t, name))
	}
	tagged, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "Strings", "windows-keys.txt"))
	if err != nil {
		t.Fatal(err)
	}
	taggedKeys := readCatalogTaggedKeys(string(tagged))
	for _, key := range cryptoPayKeys {
		if !strings.Contains(code, `"`+key+`"`) {
			t.Errorf("the crypto surfaces never look up %s", key)
		}
		if reswValue(t, root, "en", key) == "" {
			t.Errorf("en/Resources.resw has no %s", key)
		}
		if !taggedKeys[key] {
			t.Errorf("%s is not tagged windows", key)
		}
	}
	if strings.Contains(code, `"crypto_fine_print_data"`) {
		t.Error("crypto_fine_print_data is looked up, but Windows sells no data pack")
	}
	// the tabs' and coins' labels and each coin's lines are the sdk's keys,
	// looked up as it hands them out: in the catalog, tagged windows, and named
	// where the catalog scan cannot see them; USDC's tab reads USDC on Solana,
	// so the bare USDC label is no longer this app's
	for _, key := range append(slices.Clone(cryptoPaySdkLabelKeys), cryptoPaySdkLineKeys...) {
		if reswValue(t, root, "en", key) == "" {
			t.Errorf("en/Resources.resw has no %s", key)
		}
		if !taggedKeys[key] {
			t.Errorf("%s is not tagged windows", key)
		}
		if catalogUnscannedTaggedKeyReasons[key] == "" {
			t.Errorf("%s is not in catalogUnscannedTaggedKeyReasons, where the keys the sdk hands out are named", key)
		}
		if strings.Contains(code, `"`+key+`"`) {
			t.Errorf("the crypto surfaces name %s themselves: a label or a coin's line comes from the sdk", key)
		}
	}
	if taggedKeys["pay_method_usdc"] || strings.Contains(code, `"pay_method_usdc"`) {
		t.Error("pay_method_usdc is still looked up or tagged windows: the USDC tab is the sdk's pay_method_usdc_solana")
	}
	views := cryptoPayCode(stripComments(readAppSource(t, "CryptoPayViews.cpp")))
	requireCryptoPayOrder(t, "PaymentLabel", views,
		"std::wstringPaymentLabel(std::stringconst&key,std::stringconst&english){",
		"if(key.empty())returnWiden(english);",
		"std::wstringtext=Localized(key);",
		"if(text==Widen(key))returnWiden(english);",
		"returntext;")
	requireCryptoPay(t, "PaymentMethodName", views,
		"constcryptopay::MethodLabelTextlabel=cryptopay::MethodLabel(method);returnPaymentLabel(label.key,label.english);")
	// the two counted lines format their counts with Format, never by hand;
	// the confirming step's are the step's own (the sdk's: its full count once
	// it is done)
	for _, want := range []string{
		`Format("crypto_status_confirming", step.confirmations,`,
		`Format("crypto_invoice_expires_in", Widen(display.countdown))`,
	} {
		if !strings.Contains(stripComments(readAppSource(t, "CryptoPayViews.cpp")), want) {
			t.Errorf("CryptoPayViews.cpp: missing %s", want)
		}
	}
}

// The row lays out the sdk's tabs after Card (mmm/BTCPAY.md §17.6): each tab
// labelled by the sdk's key, More coins with its coins' marks each its own
// with space between them and its coins named for a screen reader, a list of
// those coins under the bar while it is chosen, each with the sdk's network
// line; an unavailable coin or tab cannot be chosen and greys its label, never
// its mark; a tap on a tab takes the sdk's coin for it, asked with the coin
// chosen last in that tab (kept per tab, so a choice in another tab does not
// replace it) and the remembered method; and the selection's tab is the sdk's
// tab id.
func TestCryptoPayMoreCoinsRowWiring(t *testing.T) {
	views := stripComments(readAppSource(t, "CryptoPayViews.cpp"))
	body := func(signature string) string {
		return cryptoPayCode(definitionBody(t, "CryptoPayViews.cpp", views, signature))
	}
	rebuild := body("void PaymentMethodRow::Rebuild(")
	requireCryptoPayOrder(t, "PaymentMethodRow::Rebuild", rebuild,
		"if(cryptopay::MethodRowShows(state.tabs)){",
		"card.Text(hstring{PaymentMethodName(cryptopay::kMethodCard)});",
		"for(cryptopay::PaymentTabViewconst&tab:state.tabs){",
		"consthstringtabName{PaymentLabel(tab.labelKey,tab.label)};",
		// a coin's tab that cannot be paid with now: greyed, with the note
		"if(!tab.available&&!cryptopay::TabOpensCoins(tab)){",
		"lines.Children().Append(MakeText(tabName,14,colors::FaintBrush()));",
		`lines.Children().Append(MakeText(Loc("pay_method_unavailable"),11,colors::MutedBrush()));`,
		"item.Child(lines);",
		"item.Text(tabName);",
		"if(cryptopay::TabOpensCoins(tab)){",
		"marks.Spacing(8);",
		"for(std::stringconst&iconKey:tab.iconKeys){if(PathIconmark=CoinMark(iconKey,kTabMarkSize))marks.Children().Append(mark);}",
		"if(0<marks.Children().Size())item.Child(marks);",
		`Format("pay_method_more_coins_a11y",Widen(cryptopay::JoinNames(names)))`,
		// the list, titled with the tab's name
		"state.coinsPanel.Children().Append(MakeText(tabName,12,colors::MutedBrush()));",
		"for(cryptopay::PaymentCoinViewconst&coin:tab.coins){",
		"if(PathIconmark=CoinMark(coin.iconKey,kCoinMarkSize))content.Children().Append(mark);",
		"coin.available?colors::TextBrush():colors::FaintBrush()",
		"conststd::wstringnetwork=LineText(coin.networkLine);",
		`consthstringunavailable=Loc("pay_method_unavailable");`,
		"choice.IsEnabled(state.enabled&&coin.available);",
		"self->selected=asset;self->lastChosen.Choose(self->tabs,asset);",
		"item.IsEnabled(tab.available);")
	// a mark is never dimmed, and the onboarding's two rows never share a
	// radio group
	for _, gone := range []string{"Opacity(", "GroupName("} {
		if strings.Contains(rebuild, gone) {
			t.Errorf("PaymentMethodRow::Rebuild calls %s", gone)
		}
	}
	requireCryptoPayOrder(t, "PaymentMethodRow::Build", body("StackPanel PaymentMethodRow::Build("),
		"method=cryptopay::TabCoinFor(tab,self->lastChosen.In(tab.id),self->remembered);",
		"self->selected=method;self->lastChosen.Choose(self->tabs,method);",
		"state.root.Children().Append(state.coinsPanel);")
	// every selection counts as the coin chosen last in its tab, a
	// programmatic one too (the remembered method, the onboarding's shared
	// choice), and what the row was given is the remembered method
	requireCryptoPayOrder(t, "PaymentMethodRow::SetTabs", body("void PaymentMethodRow::SetTabs("),
		"state.remembered=wanted;",
		"state.selected=cryptopay::MethodToSelect(state.tabs,wanted);",
		"state.lastChosen.Choose(state.tabs,state.selected);")
	requireCryptoPayOrder(t, "PaymentMethodRow::Select", body("void PaymentMethodRow::Select("),
		"state_->remembered=method;",
		"state_->selected=cryptopay::MethodToSelect(state_->tabs,method);",
		"state_->lastChosen.Choose(state_->tabs,state_->selected);")
	// one coin chosen last anywhere was the bug: More coins fell back to TAO
	// after a tap on USDC
	if strings.Contains(cryptoPayCode(views), "->wanted") || strings.Contains(cryptoPayCode(views), "state.wanted") {
		t.Error("CryptoPayViews.cpp keeps a single coin chosen last; the row keeps it per tab (cryptopay::LastChosenCoins)")
	}
	requireCryptoPayOrder(t, "PaymentMethodRow::Apply", body("void PaymentMethodRow::Apply("),
		"conststd::stringtabId=cryptopay::TabIdFor(state.selected);",
		"if(segment<state.items.size())state.bar.SelectedItem(state.items[segment]);",
		"coin.button.IsChecked(coin.asset==state.selected);",
		"state.coinsPanel.Visibility(coins?Visibility::Visible:Visibility::Collapsed);")

	// the marks: white on this dark app, whatever the coin's state
	requireCryptoPay(t, "CryptoPayViews.cpp", cryptoPayCode(views),
		"constexprwinrt::Windows::UI::ColorkMarkOnDark{255,0xFF,0xFF,0xFF};")
	requireCryptoPay(t, "CoinMark", body("PathIcon CoinMark("),
		"PathIconCoinMark(std::stringconst&iconKey,doublesize){",
		"returnkit::MakeRowPathIcon(data,cryptopay::kMarkViewBox,size,colors::MakeBrush(kMarkOnDark));")
	requireCryptoPayOrder(t, "MarkPathData", body("std::wstring const& MarkPathData("),
		"if(cryptopay::IsMarkKey(iconKey)){",
		`std::ifstreamin(dir/L"Assets"/(Widen(iconKey)+L".svg"),std::ios::binary);`,
		"data=Widen(cryptopay::SvgPathData(svg.str()));")
	// the PathIcon's markup fills nonzero (an even-odd fill hollows ETH's
	// facets and TAO's stem)
	components := stripComments(readAppSource(t, "UrComponents.cpp"))
	if !strings.Contains(components, `Data='F1 " +`) {
		t.Error("kit::MakeRowPathIcon no longer prefixes the path data with F1: XAML fills even-odd by default")
	}
	if !strings.Contains(stripComments(readAppSource(t, "App.xaml")), `RequestedTheme="Dark"`) {
		t.Error(`App.xaml is no longer RequestedTheme="Dark": the marks' white (kMarkOnDark) is for a dark theme only`)
	}
}

// One plain path of an svg (CryptoPayPresentation.h SvgPathData's rules): its
// d attribute, or "" when the file has none, several, or data the PathIcon's
// markup must not carry.
func cryptoPayMarkPathData(svg string) string {
	code := regexp.MustCompile(`(?s)<!--.*?-->`).ReplaceAllString(svg, "")
	paths := regexp.MustCompile(`<path[\s/>][^>]*>`).FindAllString(code, -1)
	if len(paths) != 1 {
		return ""
	}
	match := regexp.MustCompile(`\sd="([^"]*)"`).FindStringSubmatch(paths[0])
	if match == nil || !regexp.MustCompile(`^[0-9A-Za-z .,+-]+$`).MatchString(match[1]) {
		return ""
	}
	return match[1]
}

// The marks ship beside the exe, where CoinMark reads them: App.vcxproj copies
// each to $(OutDir)Assets, and each is one path on a 24 x 24 box, filled
// nonzero, that SvgPathData reads.
func TestCryptoPayCoinMarksShip(t *testing.T) {
	root := repositoryRoot(t)
	project := cryptoPayCode(stripComments(readAppSource(t, "App.vcxproj")))
	for _, mark := range cryptoPayMoreCoinsMarks {
		want := `<CopyFileToFoldersInclude="Assets\` + mark + `.svg"><DestinationFolders>$(OutDir)Assets</DestinationFolders></CopyFileToFolders>`
		if !strings.Contains(project, want) {
			t.Errorf("App.vcxproj does not copy Assets\\%s.svg beside the exe", mark)
		}
		data, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "Assets", mark+".svg"))
		if err != nil {
			t.Errorf("%s: %v", mark, err)
			continue
		}
		svg := string(data)
		if cryptoPayMarkPathData(svg) == "" {
			t.Errorf("Assets/%s.svg is not one plain path SvgPathData reads", mark)
		}
		if !strings.Contains(svg, `viewBox="0 0 24 24"`) {
			t.Errorf("Assets/%s.svg is not on the 24 x 24 box kMarkViewBox draws", mark)
		}
		if strings.Contains(svg, "evenodd") {
			t.Errorf("Assets/%s.svg fills even-odd; the PathIcon fills nonzero", mark)
		}
	}
	if cryptoPayMarkPathData(`<svg><path d="M0 0Z"/><path d="M1 1Z"/></svg>`) != "" ||
		cryptoPayMarkPathData(`<svg><!-- <path d="M9 9Z"/> --><path d="M0 0'Z"/></svg>`) != "" ||
		cryptoPayMarkPathData(`<svg><path fill="currentColor" d="M0 0L1 1Z"/></svg>`) != "M0 0L1 1Z" {
		t.Error("cryptoPayMarkPathData does not follow SvgPathData's rules")
	}
}

// The coins this app takes (kCoins) are the sdk's coins, by the same asset
// ids; More coins' marks are the files that ship; and the labels and lines the
// sdk hands out are the ones this app names, each line's English the
// catalog's. Nothing else about a coin is this app's: its payment uri's scheme
// and the check in front of it are the sdk's alone. Read from the sdk's
// crypto_pay.go beside this repository (URNETWORK_SDK_SOURCE); skipped
// without one.
func TestCryptoPayAgreesWithTheSdk(t *testing.T) {
	root := repositoryRoot(t)
	sdkFile := filepath.Join(filepath.Dir(root), "sdk", "crypto_pay.go")
	if dir := os.Getenv("URNETWORK_SDK_SOURCE"); dir != "" {
		sdkFile = filepath.Join(dir, "crypto_pay.go")
	}
	data, err := os.ReadFile(sdkFile)
	if err != nil {
		t.Skipf("no sdk crypto_pay.go beside this repository (set URNETWORK_SDK_SOURCE): %v", err)
	}
	sdk := string(data)

	sdkAssets := map[string]string{} // CryptoAssetTao -> "tao"
	for _, match := range regexp.MustCompile(`\n\t(CryptoAsset\w+)\s*=\s*"([a-z_]+)"`).FindAllStringSubmatch(sdk, -1) {
		sdkAssets[match[1]] = match[2]
	}
	iconKeys := map[string]string{} // CryptoIconKeyTao -> "coin_tao"
	for _, match := range regexp.MustCompile(`\n\t(CryptoIconKey\w+)\s*=\s*"([a-z_]+)"`).FindAllStringSubmatch(sdk, -1) {
		iconKeys[match[1]] = match[2]
	}
	coins := regexp.MustCompile(`(?s)\n\t(CryptoAsset\w+): \{(.*?)\n\t\},`).FindAllStringSubmatch(sdk, -1)
	if len(sdkAssets) < 7 || len(coins) < 7 {
		t.Fatalf("%s: read %d assets and %d coins; update this contract", sdkFile, len(sdkAssets), len(coins))
	}

	presentation := stripComments(readAppSource(t, "CryptoPayPresentation.h"))
	appAssets := map[string]string{} // kAssetTao -> "tao"
	for _, match := range regexp.MustCompile(`inline constexpr const char\* (kAsset\w+) = "([a-z_]+)";`).FindAllStringSubmatch(presentation, -1) {
		appAssets[match[1]] = match[2]
	}
	appCoinList := regexp.MustCompile(`(?s)kCoins = \{(.*?)\};`).FindStringSubmatch(presentation)
	if appCoinList == nil {
		t.Fatal("CryptoPayPresentation.h declares no kCoins; update this contract")
	}
	appCoins := []string{} // "tao", ... in kCoins' order
	for _, name := range regexp.MustCompile(`kAsset\w+`).FindAllString(appCoinList[1], -1) {
		appCoins = append(appCoins, appAssets[name])
	}
	sdkCoins := []string{}
	for _, coin := range coins {
		name := strings.TrimPrefix(coin[1], "CryptoAsset")
		appName := "kAsset" + name
		if appAssets[appName] != sdkAssets[coin[1]] {
			t.Errorf("%s is %q, the sdk's %s %q", appName, appAssets[appName], coin[1], sdkAssets[coin[1]])
		}
		sdkCoins = append(sdkCoins, sdkAssets[coin[1]])
	}
	if !slices.Equal(slices.Sorted(slices.Values(appCoins)), slices.Sorted(slices.Values(sdkCoins))) {
		t.Errorf("kCoins is %v, the sdk's coins %v", appCoins, sdkCoins)
	}
	// no scheme of the app's: the sdk's check is the whole uri gate
	for _, scheme := range []string{`"solana:"`, `"zcash:"`, `"monero:"`, `"bitcoin:"`, `"ethereum:"`} {
		if strings.Contains(presentation, scheme) || strings.Contains(stripComments(readAppSource(t, "CryptoPaySdk.h")), scheme) {
			t.Errorf("the crypto pay headers name the scheme %s; the sdk's urnet::checkCryptoPaymentUri is the whole gate", scheme)
		}
	}

	// the labels the sdk hands out are the ones TestCryptoPayStrings holds
	sdkLabels := []string{}
	for _, match := range regexp.MustCompile(`labelKey:\s*"([a-z_]+)"`).FindAllStringSubmatch(sdk, -1) {
		sdkLabels = append(sdkLabels, match[1])
	}
	if !slices.Equal(slices.Sorted(slices.Values(sdkLabels)), slices.Sorted(slices.Values(cryptoPaySdkLabelKeys))) {
		t.Errorf("the sdk's labels are %v, cryptoPaySdkLabelKeys %v", sdkLabels, cryptoPaySdkLabelKeys)
	}

	// and so are its coins' lines, each English the catalog's (PaymentLabel
	// shows it while the catalog lacks the key)
	sdkLines := []string{}
	linePattern := regexp.MustCompile(`(?s)(networkLine|networkWarning|noPaymentLinkNote):\s*"((?:[^"\\]|\\.)*)",\s*(?:networkLine|networkWarning|noPaymentLinkNote)Key:\s*"([a-z_]+)"`)
	for _, coin := range coins {
		for _, match := range linePattern.FindAllStringSubmatch(coin[2], -1) {
			english, key := match[2], match[3]
			sdkLines = append(sdkLines, key)
			if catalog := reswValue(t, root, "en", key); strings.ReplaceAll(catalog, "&amp;", "&") != english {
				t.Errorf("the sdk's %s reads %q, en/Resources.resw %q", key, english, catalog)
			}
		}
	}
	if !slices.Equal(slices.Sorted(slices.Values(sdkLines)), slices.Sorted(slices.Values(cryptoPaySdkLineKeys))) {
		t.Errorf("the sdk's coin lines are %v, cryptoPaySdkLineKeys %v", sdkLines, cryptoPaySdkLineKeys)
	}

	// More coins' marks, in its order, are the files that ship
	moreCoins := regexp.MustCompile(`(?s)id:\s*CryptoPaymentTabMoreCoins,.*?assets:\s*\[\]string\{([^}]*)\}`).FindStringSubmatch(sdk)
	if moreCoins == nil {
		t.Fatalf("%s: no More coins layout; update this contract", sdkFile)
	}
	marks := []string{}
	for _, asset := range strings.Split(moreCoins[1], ",") {
		asset = strings.TrimSpace(asset)
		for _, coin := range coins {
			if coin[1] != asset {
				continue
			}
			if icon := regexp.MustCompile(`iconKey:\s*(CryptoIconKey\w+)`).FindStringSubmatch(coin[2]); icon != nil {
				marks = append(marks, iconKeys[icon[1]])
			}
		}
	}
	if strings.Join(marks, ",") != strings.Join(cryptoPayMoreCoinsMarks, ",") {
		t.Errorf("More coins' marks are %v in the sdk, %v here", marks, cryptoPayMoreCoinsMarks)
	}
}
