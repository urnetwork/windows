// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The Solana wallet bridge that signing in with Phantom or Solflare uses: the
// C++ spec, and the words for the bridge page's failures. And the Earnings
// Solana payout wallet, which stays gone.

// Compile and execute the Solana wallet bridge spec
// (tools/solana-wallet-tests.cpp): the words for the bridge page's failure
// codes (App/SolanaWalletPresentation.h), and where the bridge's returns go and
// when a late challenge may still open it (App/WalletBridgeRoute.h).
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

// The ur.io wallet bridge hands a failure back with its own code (the sdk's
// SolanaWalletBridgeError*) next to its English text, on the connect and the
// sign step. WalletConnect.cpp says a code the app knows in the app's words
// (App/SolanaWalletPresentation.h BridgeErrorTextFor, run by
// TestSolanaWalletPresentation), with Phantom or Solflare where the string
// takes the wallet's name; any other code still shows the page's text.
func TestSolanaBridgeErrorCodes(t *testing.T) {
	bridge := stripLineComments(readAppSource(t, "WalletConnect.cpp"))
	localized := functionBody(bridge, "std::string LocalizedBridgeError(")
	for _, want := range []string{
		"solana::BridgeErrorTextFor(code)",
		"if (text.key.empty()) return pageText;",
		`Localized(p == WalletConnect::Provider::Solflare ? "solflare" : "phantom")`,
		"Format(text.key, walletName)",
	} {
		if !strings.Contains(localized, want) {
			t.Errorf("WalletConnect.cpp LocalizedBridgeError: missing %s", want)
		}
	}
	for _, handler := range []string{"void WalletConnect::HandleConnect(", "void WalletConnect::HandleSignMessage("} {
		if !strings.Contains(functionBody(bridge, handler), `on_error(LocalizedBridgeError(p, params["errorCode"], pageText));`) {
			t.Errorf("WalletConnect.cpp %s: the bridge page's code does not reach the error text", handler)
		}
	}

	// every key BridgeErrorTextFor returns is translated in every language, with
	// the wallet name's {} exactly where the key takes it
	root := repositoryRoot(t)
	entries, err := os.ReadDir(filepath.Join(root, "app", "src", "App", "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	for name, takesWalletName := range map[string]bool{
		"bittensor_error_extension_not_found":   true,
		"bittensor_error_no_account":            true,
		"bittensor_error_user_rejected":         false,
		"solana_wallet_error_session_not_found": false,
	} {
		englishValue := reswValue(t, root, "en", name)
		if englishValue == "" {
			t.Errorf("en/Resources.resw has no %s", name)
			continue
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
			if strings.Contains(value, "{}") != takesWalletName {
				t.Errorf("%s/Resources.resw %s = %q, want the wallet name placeholder: %v", entry.Name(), name, value, takesWalletName)
			}
		}
	}
	for _, name := range []string{"phantom", "solflare"} {
		if reswValue(t, root, "en", name) == "" {
			t.Errorf("en/Resources.resw has no %s, the wallet name the strings take", name)
		}
	}
}

// The codes BridgeErrorTextFor knows are the sdk's own
// (urnet::SolanaWalletBridgeError*). Skipped without an sdk header that has
// them (see sdkHeaderWith).
func TestSolanaBridgeCodesMatchTheSdkHeader(t *testing.T) {
	headerSource := sdkHeaderWith(t, "SolanaWalletBridgeError")
	presentationSource := readAppSource(t, "SolanaWalletPresentation.cpp")
	for name, code := range map[string]string{
		"SolanaWalletBridgeErrorExtensionNotFound": "extension_not_found",
		"SolanaWalletBridgeErrorNoAccount":         "no_account",
		"SolanaWalletBridgeErrorSessionNotFound":   "session_not_found",
		"SolanaWalletBridgeErrorUserRejected":      "user_rejected",
	} {
		if !strings.Contains(headerSource, `inline constexpr const char* `+name+` = "`+code+`";`) {
			t.Errorf("urnetwork_sdk.hpp: urnet::%s is not %q", name, code)
		}
		if !strings.Contains(presentationSource, `if (code == "`+code+`")`) {
			t.Errorf("SolanaWalletPresentation.cpp: BridgeErrorTextFor does not know the sdk's %q", code)
		}
	}
}
