// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// The Solana wallet bridge that signing in with Phantom or Solflare uses: the
// C++ spec, and the words for its failures, which are the sdk's. And the
// Earnings Solana payout wallet, which stays gone.

// Compile and execute the Solana wallet bridge spec
// (tools/solana-wallet-tests.cpp): the message for each of the sdk's Solana
// wallet failures and the log line of the bridge page's own English
// (App/SolanaWalletPresentation.h), and where the bridge's returns go and when
// a late challenge may still open it (App/WalletBridgeRoute.h).
func TestSolanaWalletPresentation(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("solana wallet tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	program := filepath.Join(t.TempDir(), "solana-wallet-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+appDir,
		filepath.Join(root, "app", "tools", "solana-wallet-tests.cpp"),
		filepath.Join(appDir, "SolanaWalletPresentation.cpp"),
		"-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build solana wallet tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("solana wallet: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// USDC provider payouts ended on 2026-11-12, and the Earnings Solana payout
// wallet went with them (owner decision 2026-11-13): no connect, no payout
// wallet card, no USDC waiting line. The sdk drops its payout wallet api, so
// no app source may name it.
func TestNoUsdcPayoutWallet(t *testing.T) {
	retired := []string{
		"setPayoutWallet",
		"getPayoutWallet",
		"SetPayoutWalletArgs",
		"SetPayoutWalletResult",
		"GetPayoutWalletIdResult",
		"pending_payout_usd_nano_cents",
	}
	files := appSourceFiles(t, ".cpp", ".h")
	for _, name := range sortedNames(files) {
		code := stripComments(files[name])
		for _, symbol := range retired {
			if strings.Contains(code, symbol) {
				t.Errorf("%s names %s, the sdk's retired USDC payout wallet api", name, symbol)
			}
		}
	}
}

// The sdk's seven messages for a failed Solana wallet sign-in
// (urnet::SolanaWalletFailureMessage*), each with whether its string takes the
// wallet's name. Owner 2026-11-13: Solana sign-in failures are specific and
// consistent on every app, with the classification in the sdk.
var solanaWalletFailureMessages = map[string]bool{
	"bittensor_error_user_rejected":         false,
	"no_wallets_found_alert_content":        false,
	"couldnt_open_wallet":                   false,
	"bittensor_error_extension_not_found":   true,
	"bittensor_error_no_account":            true,
	"solana_wallet_error_session_not_found": false,
	"bittensor_error_wallet_failed":         true,
}

// Every failure of a Solana wallet sign-in, on the connect and the sign step,
// reads in the message the sdk names for it (urnet::solanaWalletFailureFor),
// as on every app: a code the ur.io bridge page hands back
// (SolanaWalletFailureSourceBridge), and each failure WalletConnect.cpp finds
// itself (SolanaWalletFailureSourceApp, wallet_error). Before, the app had its
// own words for four of the page's codes and showed the page's English, or its
// own, for the rest. The page's English is never shown: it goes to
// on_solana_error, which SdkHost logs as one bounded line only when a Solana
// flow takes the answer. A browser that would not open the bridge page says so,
// in the browser sign-in sheet's words.
func TestSolanaWalletFailuresShowTheSdksMessages(t *testing.T) {
	bridge := stripComments(readAppSource(t, "WalletConnect.cpp"))
	fail := functionBody(bridge, "void WalletConnect::FailSolana(")
	for _, want := range []string{
		"urnet::solanaWalletFailureFor(source, failureCode, walletId);",
		"} catch (const std::exception& e) {",
		"failure ? failure->message_key : std::string()",
		"failure ? failure->wallet_name.value_or(std::string()) : std::string()",
		"Narrow(Format(text.key, Widen(*text.walletName)))",
		"Narrow(Localized(text.key))",
		"ReportSolanaFailure(std::move(message), pageText);",
	} {
		if !strings.Contains(fail, want) {
			t.Errorf("WalletConnect::FailSolana: missing %s", want)
		}
	}
	if !regexp.MustCompile(`const solana::FailureText text = solana::FailureTextFor\(`).MatchString(fail) {
		t.Error("WalletConnect::FailSolana: the sdk's message is not shown through solana::FailureTextFor")
	}

	// the page's code goes to the sdk on both steps, and its text to the log only
	pageFailure := regexp.MustCompile(`FailSolana\(p, urnet::SolanaWalletFailureSourceBridge, params\["errorCode"\],\s*` +
		`"[^"]+",\s*params\.count\("errorMessage"\) \? params\["errorMessage"\] : std::string\(\)\);`)
	appFailure := regexp.MustCompile(`FailSolana\((?:p|currentProvider_), urnet::SolanaWalletFailureSourceApp,\s*` +
		`urnet::SolanaWalletFailureWallet,\s*"[^"]+"\);`)
	for step, appFailureCount := range map[string]int{
		"void WalletConnect::Connect(":           1,
		"void WalletConnect::SignMessage(":       3,
		"void WalletConnect::HandleConnect(":     4,
		"void WalletConnect::HandleSignMessage(": 5,
	} {
		body := functionBody(bridge, step)
		if body == "" {
			t.Errorf("WalletConnect.cpp: %s is missing", step)
			continue
		}
		// no Solana step reports a failure itself: each is the sdk's
		if strings.Contains(body, "on_error") || strings.Contains(body, "on_solana_error") || strings.Contains(body, "OpenUrl(") {
			t.Errorf("%s reports a failure itself, or opens the bridge without saying its failure: every one goes through FailSolana and OpenSolanaBridge", step)
		}
		if got := len(appFailure.FindAllString(body, -1)); got != appFailureCount {
			t.Errorf("%s: %d failures found by the app go to the sdk as wallet_error, want %d", step, got, appFailureCount)
		}
		if got := strings.Count(body, "FailSolana("); got != appFailureCount+len(pageFailure.FindAllString(body, -1)) {
			t.Errorf("%s: %d failures, not each of them one the app found or the page's code", step, got)
		}
	}
	for _, handler := range []string{"void WalletConnect::HandleConnect(", "void WalletConnect::HandleSignMessage("} {
		if len(pageFailure.FindAllString(functionBody(bridge, handler), -1)) != 1 {
			t.Errorf("WalletConnect.cpp %s: the bridge page's code does not reach the sdk", handler)
		}
	}
	for _, step := range []string{"void WalletConnect::Connect(", "void WalletConnect::SignMessage("} {
		if !strings.Contains(functionBody(bridge, step), "OpenSolanaBridge(") {
			t.Errorf("%s does not open the bridge through OpenSolanaBridge", step)
		}
	}
	open := functionBody(bridge, "void WalletConnect::OpenSolanaBridge(")
	for _, want := range []string{
		"if (TryOpenUrl(url)) return;",
		`ReportSolanaFailure(Narrow(Localized("sso_browser_open_failed")), std::string());`,
	} {
		if !strings.Contains(open, want) {
			t.Errorf("WalletConnect::OpenSolanaBridge: missing %s", want)
		}
	}
	report := functionBody(bridge, "void WalletConnect::ReportSolanaFailure(")
	if !regexp.MustCompile(`if \(on_solana_error\) \{\s*on_solana_error\(std::move\(message\), pageText\);\s*\} else if \(on_error\) \{\s*on_error\(std::move\(message\)\);\s*\}`).MatchString(report) {
		t.Error("WalletConnect::ReportSolanaFailure: the message does not reach on_solana_error, or on_error without it")
	}

	// SdkHost answers it as on_error does, and logs the page's English only
	// when a Solana flow takes the answer
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	handler := functionBody(host, "wallet_.on_solana_error = [this](std::string message, std::string pageText)")
	if handler == "" {
		t.Fatal("SdkHost.cpp: no on_solana_error handler")
	}
	if !regexp.MustCompile(`const bool solanaFlowWaiting =\s*bridge::RouteSignature\(static_cast<bool>\(walletSignDone_\),\s*` +
		`walletAuthDone_ && !ssoAttempt_\) != bridge::SignatureRoute::Drop;\s*` +
		`if \(solanaFlowWaiting && !pageText\.empty\(\)\) \{\s*` +
		`LogWarn\("[^"]*\{\}", solana::PageTextLogLine\(pageText\)\);\s*\}\s*` +
		`if \(wallet_\.on_error\) wallet_\.on_error\(std::move\(message\)\);\s*\}$`).MatchString(handler) {
		t.Errorf("SdkHost.cpp on_solana_error: the page's text is logged other than as one line when a Solana flow takes the answer, or the message is not answered as on_error does:\n%s", handler)
	}

	// every message is translated in every language, with the wallet name's {}
	// exactly where the message takes one and no other brace, so std::format
	// never throws on it; and FailureTextFor shows each with that placeholder
	root := repositoryRoot(t)
	entries, err := os.ReadDir(filepath.Join(root, "app", "src", "App", "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	presentationSource := readAppSource(t, "SolanaWalletPresentation.cpp")
	for name, takesWalletName := range solanaWalletFailureMessages {
		row := `{.key = "` + name + `", .takesWalletName = ` + strconv.FormatBool(takesWalletName) + `}`
		if strings.Count(presentationSource, row) != 1 {
			t.Errorf("SolanaWalletPresentation.cpp: FailureTextFor has no row %s", row)
		}
		englishValue := reswValue(t, root, "en", name)
		if englishValue == "" {
			t.Errorf("en/Resources.resw has no %s", name)
			continue
		}
		placeholders := 0
		if takesWalletName {
			placeholders = 1
		}
		for _, entry := range entries {
			if !entry.IsDir() {
				continue
			}
			value := reswValue(t, root, entry.Name(), name)
			if value == "" {
				t.Errorf("%s/Resources.resw has no %s", entry.Name(), name)
				continue
			}
			if entry.Name() != "en" && value == englishValue {
				t.Errorf("%s/Resources.resw %s is English", entry.Name(), name)
			}
			if strings.Count(value, "{}") != placeholders || strings.Count(value, "{") != placeholders || strings.Count(value, "}") != placeholders {
				t.Errorf("%s/Resources.resw %s = %q, want the wallet name placeholder {} %d times and no other brace", entry.Name(), name, value, placeholders)
			}
		}
	}
	if got := strings.Count(presentationSource, `{.key = "`); got != len(solanaWalletFailureMessages) {
		t.Errorf("SolanaWalletPresentation.cpp: FailureTextFor has %d rows, want the sdk's %d messages", got, len(solanaWalletFailureMessages))
	}
}

// The messages FailureTextFor shows are the sdk's own
// (urnet::SolanaWalletFailureMessage*), all seven of them, and the source and
// the failure WalletConnect.cpp names for what it finds itself are the sdk's.
// Skipped without an sdk header that has them (see sdkHeaderWith).
func TestSolanaWalletFailureMessagesMatchTheSdkHeader(t *testing.T) {
	headerSource := sdkHeaderWith(t, "SolanaWalletFailureMessage")
	for name, key := range map[string]string{
		"SolanaWalletFailureMessageUserRejected":      "bittensor_error_user_rejected",
		"SolanaWalletFailureMessageNoWallet":          "no_wallets_found_alert_content",
		"SolanaWalletFailureMessageWalletAppNotFound": "couldnt_open_wallet",
		"SolanaWalletFailureMessageExtensionNotFound": "bittensor_error_extension_not_found",
		"SolanaWalletFailureMessageNoAccount":         "bittensor_error_no_account",
		"SolanaWalletFailureMessageSessionNotFound":   "solana_wallet_error_session_not_found",
		"SolanaWalletFailureMessageWallet":            "bittensor_error_wallet_failed",
		"SolanaWalletFailureSourceBridge":             "bridge",
		"SolanaWalletFailureSourceApp":                "app",
		"SolanaWalletFailureWallet":                   "wallet_error",
	} {
		if !strings.Contains(headerSource, `inline constexpr const char* `+name+` = "`+key+`";`) {
			t.Errorf("urnetwork_sdk.hpp: urnet::%s is not %q", name, key)
		}
		if strings.HasPrefix(name, "SolanaWalletFailureMessage") {
			if _, ok := solanaWalletFailureMessages[key]; !ok {
				t.Errorf("solanaWalletFailureMessages does not list the sdk's %q", key)
			}
		}
	}
	if got := strings.Count(headerSource, "inline constexpr const char* SolanaWalletFailureMessage"); got != len(solanaWalletFailureMessages) {
		t.Errorf("urnetwork_sdk.hpp names %d Solana wallet failure messages, and FailureTextFor shows %d: give a new one its row", got, len(solanaWalletFailureMessages))
	}
	if !strings.Contains(headerSource, "inline std::optional<SolanaWalletFailure> solanaWalletFailureFor(const std::string& source, const std::string& code, const std::string& wallet_name) {") {
		t.Error("urnetwork_sdk.hpp: urnet::solanaWalletFailureFor is not the (source, code, wallet_name) WalletConnect.cpp calls")
	}
}
