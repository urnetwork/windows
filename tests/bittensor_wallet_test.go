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

// UPGRADE.md 4.6: a Bittensor wallet proof is the SDK's session helper
// (urnet::BittensorWalletSession), for three wallets, all through the ur.io
// bridge in the browser: Talisman, TAO.com and WalletConnect. The app keeps only
// its own decisions (App/BittensorWalletFlow.h, compiled and run here) and must
// not parse the hand-back or build the bridge url itself.
func TestBittensorWalletSessionHelper(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("bittensor wallet tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	program := filepath.Join(t.TempDir(), "bittensor-wallet-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+appDir, filepath.Join(root, "app", "tools", "bittensor-wallet-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build bittensor wallet tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("bittensor wallet: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}

	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	for _, want := range []string{
		"urnet::newBittensorWalletSession(",
		"std::string(bittensor::kRedirectLink)",
		"session->challengeArgs(expectedAddress)",
		"session->setChallenge(result,",
		"session->bridgeUrl()",
		"session->handleBridgeReturn(url,",
		"bittensor::IsForeignReturn(",
		"bittensor::NeedsWalletConnectProjectId(walletId)",
		"session->setWalletConnectProjectId(urnw::config::kWalletConnectProjectId)",
	} {
		if !strings.Contains(host, want) {
			t.Errorf("SdkHost.cpp: the Bittensor proof does not go through the session helper (missing %s)", want)
		}
	}

	bridge := stripLineComments(readAppSource(t, "WalletConnect.cpp"))
	for _, unwanted := range []string{"wc_project_id", "SignMessageBittensor", `params["signature"]`} {
		if strings.Contains(bridge, unwanted) {
			t.Errorf("WalletConnect.cpp: still builds or parses the Bittensor bridge itself (%s)", unwanted)
		}
	}
	if !strings.Contains(bridge, "on_bittensor_return(url)") {
		t.Error("WalletConnect.cpp: the Bittensor hand-back is not handed to the session")
	}

	for _, name := range []string{"LoginPage.cpp", "WalletPage.cpp"} {
		if !strings.Contains(stripLineComments(readAppSource(t, name)), "ChooseBittensorWallet(") {
			t.Errorf("%s: the Bittensor flow does not start at the wallet chooser", name)
		}
	}
	dialogs := stripLineComments(readAppSource(t, "BittensorWalletDialogs.cpp"))
	for _, want := range []string{
		"urnet::bittensorWalletDisplayName(",
		"bittensor::kChooserWallets[i]",
		"bittensor::ChooserHintKey(walletId)",
		"bittensor::WalletForChoice(*choice)",
		`Loc("bittensor_choose_wallet")`,
	} {
		if !strings.Contains(dialogs, want) {
			t.Errorf("BittensorWalletDialogs.cpp: missing %s", want)
		}
	}
	if !strings.Contains(stripLineComments(readAppSource(t, "WalletPage.cpp")), "bittensor::BrowserHintKey(walletId)") {
		t.Error("WalletPage.cpp: the browser hint does not follow the chosen wallet")
	}
	if strings.Contains(dialogs, `"Talisman"`) || strings.Contains(dialogs, `"TAO.com"`) || strings.Contains(dialogs, `"WalletConnect"`) {
		t.Error("BittensorWalletDialogs.cpp: wallet names come from the sdk, not literals")
	}
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClCompile Include="BittensorWalletDialogs.cpp" />`) {
		t.Error("App.vcxproj: BittensorWalletDialogs.cpp is not compiled")
	}
}

// The ur.io bridge page hands a failure back with its own code (sdk
// BittensorWalletResult.BridgeErrorCode) next to its English text. A code the
// app knows reads in the user's language (App/BittensorWalletFlow.h
// BridgeErrorTextFor, run by the harness above), with the wallet's name where
// the string has a {}; any other code shows the page's text, as before.
func TestBittensorBridgeErrorCodes(t *testing.T) {
	hostSource := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	for _, want := range []string{
		"bittensor::BridgeErrorTextFor(bridgeCode)",
		"urnw::Format(bridge.key, Widen(urnet::bittensorWalletDisplayName(walletId)))",
		"result->BridgeErrorCode",
		"session->walletId()",
	} {
		if !strings.Contains(hostSource, want) {
			t.Errorf("SdkHost.cpp: the bridge page's code does not reach the error text (missing %s)", want)
		}
	}

	flowSource := readAppSource(t, "BittensorWalletFlow.h")
	start := strings.Index(flowSource, "inline BridgeErrorText BridgeErrorTextFor(")
	if start < 0 {
		t.Fatal("BittensorWalletFlow.h: no BridgeErrorTextFor")
	}
	end := strings.Index(flowSource[start:], "\n}\n")
	if end < 0 {
		t.Fatal("BittensorWalletFlow.h: BridgeErrorTextFor has no end")
	}
	keyMatches := regexp.MustCompile(`return \{"([a-z0-9_]+)", (true|false)\};`).FindAllStringSubmatch(flowSource[start:start+end], -1)
	if len(keyMatches) != 8 {
		t.Fatalf("BridgeErrorTextFor: %d keys, want 8", len(keyMatches))
	}
	root := repositoryRoot(t)
	localeEntries, err := os.ReadDir(filepath.Join(root, "app", "src", "App", "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	// every key is in the English catalog, and a translation has the wallet
	// name's {} exactly when the key takes it (MRT falls back to English for
	// a language without the key)
	for _, keyMatch := range keyMatches {
		name, takesWalletName := keyMatch[1], keyMatch[2] == "true"
		if reswValue(t, root, "en", name) == "" {
			t.Errorf("en/Resources.resw has no %s", name)
			continue
		}
		for _, entry := range localeEntries {
			if !entry.IsDir() {
				continue
			}
			value := reswValue(t, root, entry.Name(), name)
			if value != "" && strings.Contains(value, "{}") != takesWalletName {
				t.Errorf("%s/Resources.resw %s = %q, want the wallet name placeholder: %v", entry.Name(), name, value, takesWalletName)
			}
		}
	}
	// the strings this change added or completed are translated in every
	// language (earnings_wallet_mismatch is an older string, translated in a few)
	for _, name := range []string{
		"bittensor_error_address_not_in_wallet",
		"bittensor_error_extension_not_found",
		"bittensor_error_no_account",
		"bittensor_error_user_rejected",
		"bittensor_error_walletconnect_expired",
		"bittensor_error_walletconnect_unavailable",
	} {
		englishValue := reswValue(t, root, "en", name)
		for _, entry := range localeEntries {
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

// The bridge codes BridgeErrorTextFor knows are the sdk's own
// (urnet::BittensorWalletBridgeError*), and the sdk result carries the
// page's code. The header is git-ignored (fetch-deps unpacks it into
// app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE names another
// directory), so a host without one, or with one from before the codes,
// skips this.
func TestBittensorBridgeCodesMatchTheSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	headerDirs := []string{}
	if explicit := os.Getenv("URNETWORK_SDK_INCLUDE"); explicit != "" {
		headerDirs = append(headerDirs, explicit)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		headerDirs = append(headerDirs, filepath.Join(root, "app", "third_party", "urnetwork-sdk", arch))
	}
	headerSource := ""
	for _, dir := range headerDirs {
		data, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
		if err == nil && strings.Contains(string(data), "BittensorWalletBridgeError") {
			headerSource = string(data)
			t.Logf("against %s", filepath.Join(dir, "urnetwork_sdk.hpp"))
			break
		}
	}
	if headerSource == "" {
		t.Skip("no urnetwork_sdk.hpp with the Bittensor bridge codes (set URNETWORK_SDK_INCLUDE)")
	}
	if !strings.Contains(headerSource, "std::string BridgeErrorCode{};") {
		t.Error("urnetwork_sdk.hpp: BittensorWalletResult has no BridgeErrorCode")
	}
	flowSource := readAppSource(t, "BittensorWalletFlow.h")
	for name, code := range map[string]string{
		"BittensorWalletBridgeErrorAddressNotInWallet":       "address_not_in_wallet",
		"BittensorWalletBridgeErrorAddressMismatch":          "address_mismatch",
		"BittensorWalletBridgeErrorExtensionNotFound":        "extension_not_found",
		"BittensorWalletBridgeErrorNoAccount":                "no_account",
		"BittensorWalletBridgeErrorUserRejected":             "user_rejected",
		"BittensorWalletBridgeErrorWalletConnectExpired":     "walletconnect_expired",
		"BittensorWalletBridgeErrorWalletConnectUnavailable": "walletconnect_unavailable",
	} {
		if !strings.Contains(headerSource, `inline constexpr const char* `+name+` = "`+code+`";`) {
			t.Errorf("urnetwork_sdk.hpp: urnet::%s is not %q", name, code)
		}
		if !strings.Contains(flowSource, `if (bridgeCode == "`+code+`")`) {
			t.Errorf("BittensorWalletFlow.h: BridgeErrorTextFor does not know the sdk's %q", code)
		}
	}
}

// Signing by hand is on the bridge page ("Enter address manually"), which hands
// its proof back like any other: every chooser wallet is browser_bridge on
// Windows (sdk BittensorWalletTransportFor), so the app has no form of its own
// and takes no signature except through the session's handleBridgeReturn. A
// session of any other transport has no step: the proof is answered with a
// failure at once (NextStepFor, run by the harness above).
func TestBittensorProofHasNoFormInTheApp(t *testing.T) {
	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	if strings.Contains(host, "handleSignature(") {
		t.Error("SdkHost.cpp: a signature reaches the session other than through a bridge hand-back")
	}
	open := functionBody(host, "void SdkHost::OpenBittensorWallet(")
	for _, want := range []string{
		"bittensor::NextStepFor(session->transport())",
		"case bittensor::NextStep::OpenBrowser:",
		"case bittensor::NextStep::Unsupported:",
		`FinishBittensorProof(serial, {false, {}, Narrow(Localized("wallet_connect_failed"))});`,
	} {
		if !strings.Contains(open, want) {
			t.Errorf("SdkHost::OpenBittensorWallet: missing %s", want)
		}
	}
	for _, name := range []string{"BittensorWalletDialogs.cpp", "SettingsSheets.cpp", "MainWindow.xaml.cpp"} {
		source := stripLineComments(readAppSource(t, name))
		for _, unwanted := range []string{`"bittensor_message_to_sign"`, `"bittensor_signature_label"`, `"bittensor_manual_sign_instructions"`} {
			if strings.Contains(source, unwanted) {
				t.Errorf("%s: asks for a pasted signature (%s)", name, unwanted)
			}
		}
	}
}

// A refused proof reads in the server's words. Sign-in and network create ask
// for result_errors, so a signature from another account than the address comes
// back as that sentence rather than a 401, and the add sheet shows addAuth's. The
// bridge page's manual form checks a paste before it hands it back, so this is
// what is left when it could not.
func TestBittensorRefusedProofReadsInTheServersWords(t *testing.T) {
	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	for _, signature := range []string{"void SdkHost::AuthLoginWithWallet(", "void SdkHost::SubmitCreateNetwork("} {
		body := definitionBody(t, "SdkHost.cpp", host, signature)
		for _, want := range []string{
			"args.result_errors = true;",
			"AuthResult r{false, false, result->error->message};",
		} {
			if !strings.Contains(body, want) {
				t.Errorf("SdkHost.cpp: %s is missing %s", signature, want)
			}
		}
	}
	if !strings.Contains(stripLineComments(readAppSource(t, "SettingsSheets.cpp")), "error = H(add_->Error());") {
		t.Error("SettingsSheets.cpp: the add sheet does not show addAuth's refusal")
	}
}

// The coldkey connect's api fallback keeps the refusal's code, as the device path
// does, so the page words a coded refusal (SnErrorText: wallet_blocked, an
// invalid address) the same on both.
func TestWalletConnectFallbackKeepsTheRefusalsCode(t *testing.T) {
	page := stripLineComments(readAppSource(t, "WalletPage.cpp"))
	setWalletError := definitionBody(t, "WalletPage.cpp", page, "urnet::SnError SetWalletError(")
	if !strings.Contains(setWalletError, "error.code = source.code;") {
		t.Error("WalletPage.cpp: the api fallback drops the refusal's code")
	}
}
