// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The app's own payment screens word the server's refusal by its code
// (App/PaymentRefusal.h, the rule of ur.io's src/lib/paymentFailure.js): the
// upgrade sheet's hosted checkout session and Manage subscription in Settings.
// The C++ spec runs here, alone and against the generated sdk header; the two
// screens cannot be built off Windows, so their use of it is read from their
// sources.

// The keys of the codes' own lines (PaymentRefusal.h).
var paymentRefusalCodeKeys = []string{
	"site_payment_error_already_subscribed",
	"site_payment_error_plan_unavailable",
	"checkout_error_unavailable",
	"checkout_error_incorrect_cvc",
	"site_subscription_error_no_customer",
	"site_subscription_error_store_unavailable",
}

// The generated header's billing portal refusal with the code beside the
// message (sdk fix/app-checkout-error-codes).
var paymentRefusalPortalCodePattern = regexp.MustCompile(
	`struct StripeCreateCustomerPortalError \{\s*std::optional<std::string> code;`)

// The generated header's card payment report answer with the refusal beside
// its status (sdk feature/trial-card-check): a member line of the struct, in
// any position (each starts with a tab; the closing "};" does not).
var paymentRefusalReportErrorPattern = regexp.MustCompile(
	`struct VerifyStorePurchaseResult \{\r?\n(?:\t[^\n]*\n)*\tstd::optional<OnboardingError> error;`)

// Compile the payment refusal spec (app/tools/payment-refusal-tests.cpp,
// header-only). extra adds compiler arguments ahead of the source.
func buildPaymentRefusalTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("payment refusal tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "payment-refusal-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(root, "app", "src", "App")}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(root, "app", "tools", "payment-refusal-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build payment refusal tests: %v\n%s", err, output)
	}
	return program
}

// Run a built spec and log what it printed.
func runPaymentRefusalTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("payment refusal: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The code of a source without its whitespace, so a statement reads the same
// however it is wrapped.
func paymentRefusalCode(source string) string {
	return strings.Join(strings.Fields(stripComments(source)), "")
}

// A code with a line of its own reads that line alone; invalid_request and
// start_failed read the screen's own line alone; any other code, or none (an
// older server), reads the screen's own line with the server's words under it.
func TestPaymentRefusalText(t *testing.T) {
	runPaymentRefusalTests(t, buildPaymentRefusalTests(t))
}

// The same spec on the generated sdk header's own refusals, parsed from the
// server's json. The header is git-ignored (fetch-deps unpacks it into
// app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE names another
// directory) and needs nlohmann/json, so a host without them skips this, and
// so does a header whose billing portal refusal has no code yet, or whose card
// payment report answer has no refusal yet.
func TestPaymentRefusalTextAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := paymentRefusalSdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp whose billing portal refusal has a code and whose card payment report has a refusal (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_PAYMENT_REFUSAL_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runPaymentRefusalTests(t, buildPaymentRefusalTests(t, extra...))
}

// The directory of a urnetwork_sdk.hpp whose billing portal refusal keeps the
// code and whose card payment report answer keeps the refusal, or "".
func paymentRefusalSdkHeaderDir(t *testing.T, root string) string {
	t.Helper()
	explicit := os.Getenv("URNETWORK_SDK_INCLUDE")
	candidates := []string{}
	if explicit != "" {
		candidates = append(candidates, explicit)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		candidates = append(candidates, filepath.Join(root, "app", "third_party", "urnetwork-sdk", arch))
	}
	for _, dir := range candidates {
		header, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
		if err != nil {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: %v", explicit, err)
			}
			continue
		}
		if !paymentRefusalPortalCodePattern.Match(header) {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnet::StripeCreateCustomerPortalError has no code", explicit)
			}
			t.Logf("%s: urnet::StripeCreateCustomerPortalError predates the code", dir)
			continue
		}
		if !paymentRefusalReportErrorPattern.Match(header) {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnet::VerifyStorePurchaseResult has no error", explicit)
			}
			t.Logf("%s: urnet::VerifyStorePurchaseResult predates the card payment report's refusal", dir)
			continue
		}
		return dir
	}
	return ""
}

// The upgrade sheet's hosted checkout session words the server's refusal with
// PaymentRefusal.h. A guest still goes to the add-sign-in flow first, an
// embedded session's refusal still falls through to the hosted session, and a
// failure with no answer from the server still shows what it did; the
// server's words are never the whole line.
func TestUpgradeSheetWordsTheHostedRefusal(t *testing.T) {
	sheets := readAppSource(t, "BalanceSheets.cpp")
	body := paymentRefusalCode(functionBody(sheets, "void UpgradeSheet::RequestSession("))
	if body == "" {
		t.Fatal("BalanceSheets.cpp no longer defines UpgradeSheet::RequestSession")
	}
	if !strings.Contains(sheets, `#include "PaymentRefusal.h"`) {
		t.Error(`BalanceSheets.cpp does not include "PaymentRefusal.h"`)
	}
	refusalText := `refusalText=refused?PaymentRefusalTextFor(*result->error,"something_went_wrong"):PaymentRefusalText{};`
	shown := `self->ShowCheckoutError(hstring{PaymentRefusalMessage(Localized(refusalText.key),Widen(refusalText.detail))});`
	for _, want := range []string{
		"constboolrefused=result&&result->error;",
		refusalText,
		// an embedded session's refusal retries as hosted
		"if(!refused&&transportError.empty()&&!clientSecret.empty()){self->OpenEmbedded(clientSecret);}else{self->RequestSession(false);}",
		"if(refused){" + shown + "return;}",
		// no answer from the server: as before
		`self->ShowCheckoutError(transportError.empty()?Loc("something_went_wrong"):H(transportError));`,
	} {
		if !strings.Contains(body, want) {
			t.Errorf("UpgradeSheet::RequestSession: missing %s", want)
		}
	}
	order := []string{"self->RefuseForGuest();", "self->RequestSession(false);", shown, "self->LaunchHosted(url);"}
	for index := 1; index < len(order); index++ {
		before, after := strings.Index(body, order[index-1]), strings.Index(body, order[index])
		if before < 0 || after < 0 || after < before {
			t.Errorf("UpgradeSheet::RequestSession: %s must come before %s", order[index-1], order[index])
		}
	}
	// the server's words reach the line only through PaymentRefusal.h
	if strings.Contains(body, "error->message") {
		t.Error("UpgradeSheet::RequestSession reads the refusal's message itself")
	}
}

// Manage subscription on the Account plan pane words the billing portal's
// refusal with PaymentRefusal.h; a portal url still opens first, and a failure
// with no answer from the server still shows the sdk's words. Both show on the
// window-level bar (TestSubscriptionRowsReportOnTheWindowLevelBar).
func TestManageSubscriptionWordsThePortalRefusal(t *testing.T) {
	settings := readAppSource(t, "SettingsPage.cpp")
	body := paymentRefusalCode(functionBody(settings, "winrt::fire_and_forget SettingsPage::OpenCustomerPortal("))
	if body == "" {
		t.Fatal("SettingsPage.cpp no longer defines SettingsPage::OpenCustomerPortal")
	}
	if !strings.Contains(settings, `#include "PaymentRefusal.h"`) {
		t.Error(`SettingsPage.cpp does not include "PaymentRefusal.h"`)
	}
	shown := `page.accountSnackbar_.Show(hstring{PaymentRefusalMessage(Localized(refusal->key),Widen(refusal->detail))},InfoBarSeverity::Error);`
	unanswered := `page.accountSnackbar_.Show(error.empty()?Loc("something_went_wrong"):winrt::to_hstring(error),InfoBarSeverity::Error);`
	for _, want := range []string{
		`if(result&&result->error){refusal=PaymentRefusalTextFor(*result->error,"something_went_wrong");}elseif(err){error=*err;}`,
		"if(refusal){" + shown + "return;}",
		unanswered,
	} {
		if !strings.Contains(body, want) {
			t.Errorf("SettingsPage::OpenCustomerPortal: missing %s", want)
		}
	}
	order := []string{"page.LaunchCustomerPortal(url);", shown, unanswered}
	for index := 1; index < len(order); index++ {
		before, after := strings.Index(body, order[index-1]), strings.Index(body, order[index])
		if before < 0 || after < 0 || after < before {
			t.Errorf("SettingsPage::OpenCustomerPortal: %s must come before %s", order[index-1], order[index])
		}
	}
	if strings.Contains(body, "error->message") {
		t.Error("SettingsPage::OpenCustomerPortal reads the refusal's message itself")
	}
}

// The header is part of the app build, and every line a refusal can read (the
// codes' own lines, the screens' own line, and the refused card's Try again)
// is in the catalog in every language.
func TestPaymentRefusalLinesAreTranslated(t *testing.T) {
	root := repositoryRoot(t)
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="PaymentRefusal.h" />`) {
		t.Error("App.vcxproj does not list PaymentRefusal.h")
	}
	refusalSource := stripComments(readAppSource(t, "PaymentRefusal.h"))
	for _, name := range paymentRefusalCodeKeys {
		if !strings.Contains(refusalSource, `"`+name+`"`) {
			t.Errorf("PaymentRefusal.h does not name %s", name)
		}
	}
	entries, err := os.ReadDir(filepath.Join(root, "app", "src", "App", "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	lineKeys := append([]string{"something_went_wrong", "try_again"}, paymentRefusalCodeKeys...)
	for _, name := range lineKeys {
		englishValue := reswValue(t, root, "en", name)
		if englishValue == "" {
			t.Errorf("en/Resources.resw has no %s", name)
			continue
		}
		for _, entry := range entries {
			if !entry.IsDir() || entry.Name() == "en" {
				continue
			}
			if value := reswValue(t, root, entry.Name(), name); value == "" {
				t.Errorf("%s/Resources.resw has no %s", entry.Name(), name)
			} else if value == englishValue {
				t.Errorf("%s/Resources.resw %s is English", entry.Name(), name)
			}
		}
	}
}
