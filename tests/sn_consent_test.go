// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"fmt"
	"maps"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// Earnings' payout consent (v2/CONSENT.md): the C++ spec of its decisions
// (App/SnConsentPresentation.h, tools/sn-consent-tests.cpp) with negative
// controls that rewrite a copy of the header, the spec against the generated
// SDK header when one is at hand, the WinRT wiring (WalletPage, the coldkey
// dialog, SdkHost, the window) with a negative control for each assertion, and
// the strings.

// Builds the consent spec against a copy of SnConsentPresentation.h, rewritten
// by `mutation` when it is set, with `extra` compiler arguments.
func buildSnConsentTests(t *testing.T, mutation func(string) string, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("sn consent tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	header, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "SnConsentPresentation.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(header)
	if mutation != nil {
		content = mutation(content)
		if content == string(header) {
			t.Fatal("negative control did not change SnConsentPresentation.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "SnConsentPresentation.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "sn-consent-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(root, "app", "tools", "sn-consent-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build sn consent tests: %v\n%s", err, output)
	}
	return program
}

// Compile and execute the payout consent spec (CONSENT.md §4 and §8): the
// panel for every state (hidden until the consent is read, loading, no consent
// with Sign with wallet, a consent with Change wallet and the next-epoch note,
// the consent a signing just made, active and starting next epoch, waiting for
// the wallet, a failed read, no session), the typed coldkey checked by the ss58
// rule before any request, the sentence of each refusal (expired, signature
// mismatch, not the network owner, and none for any other), and that the
// submission is the server's own message for the coldkey it names and nothing
// else.
func TestSnConsentPanel(t *testing.T) {
	program := buildSnConsentTests(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("sn consent: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Each decision the spec pins, put back wrong in a copy of the header, fails
// the spec in the case that names it.
func TestSnConsentPanelCatchesEachRegression(t *testing.T) {
	for _, control := range []struct {
		name     string
		old, new string
		want     string
	}{
		{"shown before the consent is read",
			"  if (fetch == Fetch::Idle) return view;\n", "",
			"hidden until the consent is read"},
		{"a failed read shows the consent before it",
			"const std::optional<Status> known = view.failed ? std::nullopt : status;",
			"const std::optional<Status> known = status;",
			"a failed read shows no consent and no button"},
		{"a consent that ended reads as active",
			"const bool active = known && known->active;", "const bool active = known.has_value();",
			"no consent, then Sign with wallet"},
		{"no next-epoch note under a consent",
			`: active ? "sn_consent_change_note"`, `: active ? "sn_consent_body"`,
			"right after a signing the consent is active, starting next epoch"},
		{"Change wallet missing under a consent",
			`view.buttonKey = active ? "sn_consent_change" : "sn_consent_sign";`,
			`view.buttonKey = "sn_consent_sign";`,
			"a consent says whose coldkey it pays, then Change wallet"},
		{"no wait while signing",
			`view.noteKey = signing  ? "sn_consent_signing"`, `view.noteKey = false  ? "sn_consent_signing"`,
			"Waiting for the wallet while a signing is out"},
		{"the button signs twice",
			"view.buttonEnabled = canSign && !signing;", "view.buttonEnabled = canSign;",
			"Waiting for the wallet while a signing is out"},
		{"the button signs without a session",
			"view.buttonEnabled = canSign && !signing;", "view.buttonEnabled = !signing && (canSign || true);",
			"without a session to sign with, the button waits"},
		{"the typed coldkey is not checked",
			"  if (!valid(coldkey)) return std::nullopt;\n", "  (void)valid;\n",
			"nothing, or an address the ss58 rule refuses, asks for nothing"},
		{"the typed coldkey is not trimmed",
			"const std::string coldkey(typed.substr(first, typed.find_last_not_of(kSpace) - first + 1));",
			"const std::string coldkey(typed);",
			"what the user typed, trimmed, checked before any request"},
		{"an expired challenge is not named",
			`  if (is(kCodeChallengeExpired)) return "sn_consent_expired";` + "\n", "",
			"an expired challenge says so"},
		{"a signature mismatch is not named",
			`  if (is(kCodeSignatureMismatch)) return "sn_consent_failed";` + "\n", "",
			"a signature that does not match says so"},
		{"a session not the owner's is not named",
			`  if (is(kCodeNetworkOwnerOnly)) return "sn_consent_owner_only";` + "\n", "",
			"a session that is not the network owner's says so"},
		{"a code anywhere in an error is taken for the refusal",
			"return codeOrError.starts_with(code) &&\n           (codeOrError.size() == code.size() || codeOrError[code.size()] == ':');",
			"return codeOrError.find(code) != std::string_view::npos;",
			"any other goes to the page's existing error surface"},
		{"another message is submitted",
			"if (signedMessage != challenge.message || signedAddress != challenge.coldkeySs58) {",
			"if (signedMessage.data() == nullptr || signedAddress != challenge.coldkeySs58) {",
			"never another message"},
		{"another account is submitted",
			"if (signedMessage != challenge.message || signedAddress != challenge.coldkeySs58) {",
			"if (signedMessage != challenge.message || signedAddress.data() == nullptr) {",
			"never another account"},
		{"a submission without a signature",
			"challenge.coldkeySs58.empty() || signatureHex.empty()) {", "challenge.coldkeySs58.empty()) {",
			"nothing without a signature or a challenge"},
		{"the status loses whether it is active",
			"  out.active = status.active;\n", "",
			"the status and the challenge as plain views"},
		{"the challenge loses its message",
			"  out.message = challenge.message;\n", "",
			"the status and the challenge as plain views"},
	} {
		t.Run(control.name, func(t *testing.T) {
			program := buildSnConsentTests(t, func(source string) string {
				if strings.Count(source, control.old) != 1 {
					t.Fatalf("SnConsentPresentation.h no longer has exactly one %q; update this control", control.old)
				}
				return strings.Replace(source, control.old, control.new, 1)
			})
			output, err := exec.Command(program).CombinedOutput()
			if err == nil || !strings.Contains(string(output), control.want) {
				t.Fatalf("negative control was not detected (want %q): %v\n%s", control.want, err, output)
			}
		})
	}
}

// The boundary templates against the generated SDK header: WalletPage and
// SdkHost instantiate snconsent::StatusFrom with urnet::SnNetworkConsentStatus
// and snconsent::ChallengeFrom with urnet::SnNetworkConsentChallengeResult, so
// the spec builds both with the header's own types, reads each from the json
// shapes of CONSENT.md §9 through the header's conversions, and compares the
// refusal codes with the header's. The calls the app makes are the header's,
// as the app makes them, and its consent purpose is the SDK's. The header is
// git-ignored (fetch-deps unpacks it into app/third_party/urnetwork-sdk/<arch>;
// URNETWORK_SDK_INCLUDE names another directory) and needs nlohmann/json, so a
// host without them skips this, and so does a copy from before the consent.
func TestSnConsentAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := sdkHeaderDirWith(t, root, "SnNetworkConsentStatus")
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the network consent (set URNETWORK_SDK_INCLUDE)")
	}
	header, err := os.ReadFile(filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		"void snNetworkConsentChallenge(const std::string& coldkey_ss58, SnNetworkConsentChallengeCallback callback) const;",
		"void snNetworkConsentSubmit(const std::string& coldkey_ss58, const std::string& message, const std::string& signature_hex, SnSetWalletCallback callback) const;",
		"void snNetworkConsentStatus(SnNetworkConsentStatusCallback callback) const;",
		"void setNetworkConsentChallenge(const std::optional<SnNetworkConsentChallengeResult>& result, int64_t now_millis) const;",
		"using SnNetworkConsentChallengeCallback = std::function<void(std::optional<SnNetworkConsentChallengeResult> result, std::optional<std::string> err_param)>;",
		"using SnNetworkConsentStatusCallback = std::function<void(std::optional<SnNetworkConsentStatus> result, std::optional<std::string> err_param)>;",
		`inline constexpr const char* BittensorWalletPurposeConsent = "consent";`,
		"inline bool validateSs58(const std::string& address) {",
	} {
		if !strings.Contains(string(header), want) {
			t.Errorf("urnetwork_sdk.hpp: missing %s", want)
		}
	}
	if !strings.Contains(readAppSource(t, "BittensorWalletFlow.h"), `inline constexpr std::string_view kPurposeConsent = "consent";`) {
		t.Error("BittensorWalletFlow.h: the consent purpose is not the sdk's")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_SN_CONSENT_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	program := buildSnConsentTests(t, nil, extra...)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("sn consent against the sdk header: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The sources the payout consent's wiring lives in, by file name.
type snConsentSources map[string]string

func readSnConsentSources(t *testing.T) snConsentSources {
	t.Helper()
	sources := snConsentSources{}
	for _, name := range []string{
		"WalletPage.cpp", "BittensorWalletDialogs.cpp", "BittensorWalletDialogs.h", "SdkHost.cpp",
		"MainWindow.xaml", "MainWindow.xaml.cpp", "MainWindow.xaml.h", "App.vcxproj",
	} {
		sources[name] = readAppSource(t, name)
	}
	return sources
}

// Every way `sources` miss the payout consent's wiring, one line each. The
// WinRT files do not build here, so these read their source.
func snConsentWiringMisses(sources snConsentSources) []string {
	misses := []string{}
	page := stripComments(sources["WalletPage.cpp"])
	dialogs := stripComments(sources["BittensorWalletDialogs.cpp"])
	host := stripComments(sources["SdkHost.cpp"])
	body := func(file, source, signature string) string {
		found := functionBody(source, signature)
		if found == "" {
			misses = append(misses, fmt.Sprintf("%s: no %s", file, signature))
		}
		return found
	}
	require := func(where, source string, wants ...string) {
		for _, want := range wants {
			if !strings.Contains(source, want) {
				misses = append(misses, fmt.Sprintf("%s: missing %s", where, want))
			}
		}
	}
	requirePattern := func(where, source, pattern, what string) {
		if !regexp.MustCompile(pattern).MatchString(source) {
			misses = append(misses, fmt.Sprintf("%s: %s", where, what))
		}
	}
	forbid := func(where, source string, unwanted ...string) {
		for _, word := range unwanted {
			if strings.Contains(source, word) {
				misses = append(misses, fmt.Sprintf("%s: calls %s", where, word))
			}
		}
	}

	// the read: every Earnings load reads the network's consent through the
	// SDK, copied at the boundary, and a failed read is a failed read
	require("WalletPage::LoadWallet", body("WalletPage.cpp", page, "void WalletPage::LoadWallet("),
		"LoadConsentStatus();")
	require("WalletPage::LoadConsentStatus", body("WalletPage.cpp", page, "void WalletPage::LoadConsentStatus("),
		"Sdk().api().snNetworkConsentStatus(",
		"if (ok) status = snconsent::StatusFrom(*result);",
		"ok ? snconsent::Fetch::Ready : snconsent::Fetch::Failed, status);")

	// every state: the panel draws snconsent::PanelFor's decision from the
	// session, the read and the signing, with each of its lines
	panel := body("WalletPage.cpp", page, "void WalletPage::RebuildConsentPanel(")
	require("WalletPage::RebuildConsentPanel", panel,
		"snconsent::PanelFor(CanCallApi() && !connectingWallet_, consentFetch_,",
		"consent_, consentSigning_);",
		`line = Loc("sn_consent_none");`,
		`line = hstring{urnw::Format("sn_consent_active", ShortAddress(view.coldkeySs58))};`,
		"Loc(view.noteKey)",
		"w_.PayoutConsentButton().Content(LocBox(view.buttonKey));",
		"w_.PayoutConsentButton().IsEnabled(view.buttonEnabled);")
	requirePattern("WalletPage::RebuildConsentPanel", panel,
		`view\.loading\s+\?\s+Loc\("loading"\)\s+:\s+view\.failed\s+\?\s+Loc\("something_went_wrong"\)`,
		"the header does not say Loading, or that the read failed")
	require("WalletPage::ApplyStrings", body("WalletPage.cpp", page, "void WalletPage::ApplyStrings("),
		`w_.PayoutConsentHeading().Text(Loc("sn_consent_title"));`,
		"RebuildConsentPanel();")
	// a connect out holds the button: the two share the bridge
	require("WalletPage::SetConnectingWallet", body("WalletPage.cpp", page, "void WalletPage::SetConnectingWallet("),
		"RebuildConsentPanel();")

	// Sign with wallet and Change wallet alike: the wallet, then the coldkey the
	// consent names, then the challenge for it
	sign := body("WalletPage.cpp", page, "void WalletPage::OnSignConsent(")
	require("WalletPage::OnSignConsent", sign,
		"if (!CanCallApi()) {",
		"if (!page || coldkeySs58.empty()) return;",
		"page->wallet().StartConsent(coldkeySs58, walletId);")
	requirePattern("WalletPage::OnSignConsent", sign,
		`ChooseBittensorWallet\(w_\.get_strong\(\), \[weak\]\(std::string walletId\) \{\s*`+
			`auto self = weak\.get\(\);\s*if \(!self \|\| walletId\.empty\(\)\) return;\s*`+
			`EnterConsentColdkey\(self, \[weak, walletId\]\(std::string coldkeySs58\) \{`,
		"the wallet is not chosen before the coldkey is asked for")
	// the coldkey: typed, never prefilled, and checked by the SDK's ss58 rule
	// before any request; an invalid one keeps the form open and says why
	coldkey := body("BittensorWalletDialogs.cpp", dialogs, "winrt::fire_and_forget EnterConsentColdkey(")
	require("EnterConsentColdkey", coldkey,
		"snconsent::ColdkeyFrom(\n        Narrow(addressBox.Text().c_str()),",
		"[](std::string const& address) { return urnet::validateSs58(address); });",
		"args.Cancel(true);",
		`errorText.Text(Loc("invalid_ss58_address"));`,
		"*coldkey = *valid;",
		"if (next) next(*coldkey);",
		`dialog.Title(winrt::box_value(Loc("sn_consent_title")));`)
	if strings.Count(coldkey, "addressBox.Text(") != strings.Count(coldkey, "addressBox.Text()") {
		misses = append(misses, "EnterConsentColdkey: the coldkey is prefilled")
	}
	require("BittensorWalletDialogs.h", sources["BittensorWalletDialogs.h"],
		"winrt::fire_and_forget EnterConsentColdkey(")

	// signing: the challenge, with the wait shown until the submission answers
	start := body("WalletPage.cpp", page, "void WalletPage::StartConsent(")
	require("WalletPage::StartConsent", start,
		"Sdk().api().snNetworkConsentChallenge(",
		"BeginFlow(consentFlow_, kApiTimeoutMs,")
	requirePattern("WalletPage::StartConsent", start, `consentSigning_ = true;\s*RebuildConsentPanel\(\);`,
		"the panel does not say the wallet is awaited")
	requirePattern("WalletPage::EndConsent", body("WalletPage.cpp", page, "void WalletPage::EndConsent("),
		`consentSigning_ = false;\s*RebuildConsentPanel\(\);`,
		"the wait does not end")

	// the app signs only the server's message: the SDK's challenge goes to the
	// wallet whole, the session takes it verbatim under the consent purpose,
	// nothing else is fetched or signed on the way, and the submission is
	// SubmissionFor's, or sn_consent_failed
	challenge := body("WalletPage.cpp", page, "void WalletPage::ApplyConsentChallenge(")
	require("WalletPage::ApplyConsentChallenge", challenge,
		"Sdk().SignNetworkConsent(\n      walletId, *result,",
		"const snconsent::Challenge challenge = snconsent::ChallengeFrom(*result);")
	signed := body("WalletPage.cpp", page, "void WalletPage::ApplyConsentSigned(")
	require("WalletPage::ApplyConsentSigned", signed,
		"snconsent::SubmissionFor(challenge, address, message, signature);",
		"Sdk().api().snNetworkConsentSubmit(\n      submission->coldkeySs58, submission->message, submission->signatureHex,")
	requirePattern("WalletPage::ApplyConsentSigned", signed,
		`if \(!submission\) \{[^}]*Notify\(Loc\("sn_consent_failed"\), InfoBarSeverity::Error\);\s*return;`,
		"an answer for another message or account is not refused with sn_consent_failed")
	submitted := body("WalletPage.cpp", page, "void WalletPage::ApplyConsentSubmitted(")
	for name, source := range map[string]string{
		"WalletPage::OnSignConsent": sign, "WalletPage::StartConsent": start,
		"WalletPage::ApplyConsentChallenge": challenge, "WalletPage::ApplyConsentSigned": signed,
		"WalletPage::ApplyConsentSubmitted": submitted, "EnterConsentColdkey": coldkey,
	} {
		forbid(name, source, "SignWithBittensorWallet(", "SignWithSolanaWallet(", "RequestWalletChallenge(",
			"authWalletChallenge(", "snNetworkWalletMappingChallenge")
	}
	consentHost := body("SdkHost.cpp", host, "void SdkHost::SignNetworkConsent(")
	require("SdkHost::SignNetworkConsent", consentHost,
		`CancelPendingWalletFlows("superseded by a payout consent signature");`,
		"const std::string purpose(bittensor::kPurposeConsent);",
		"BeginBittensorSession(\n      walletId, purpose,",
		"session->setNetworkConsentChallenge(challenge, BittensorNowMillis());",
		"OpenBittensorWallet(serial, session);")
	forbid("SdkHost::SignNetworkConsent", consentHost,
		"authWalletChallenge(", "challengeArgs(", "setChallenge(", "RequestWalletChallenge(", "bridgeUrl(")
	// the wallet hop is the coldkey connect's own, shared, not a second one
	require("SdkHost::BeginBittensorProof", body("SdkHost.cpp", host, "void SdkHost::BeginBittensorProof("),
		"BeginBittensorSession(walletId, purpose, std::move(done), serial);",
		"OpenBittensorWallet(serial, session);")
	require("SdkHost::OpenBittensorWallet", body("SdkHost.cpp", host, "void SdkHost::OpenBittensorWallet("),
		"url = session->bridgeUrl();", "wallet_.OpenBittensorBridge(url);")
	if strings.Count(host, "wallet_.OpenBittensorBridge(") != 1 {
		misses = append(misses, "SdkHost.cpp: the Bittensor bridge opens from more than one place")
	}

	// each error string: the session's refusal code reaches the page, which
	// names an expired challenge, a mismatch and a session not the owner's
	// (snconsent::RefusalKey), and shows any other as the page does today
	requirePattern("SdkHost::HandleBittensorReturn", body("SdkHost.cpp", host, "void SdkHost::HandleBittensorReturn("),
		`session->walletId\(\)\),\s*result->ErrorCode\}\);`,
		"the bridge's refusal code does not reach the flow")
	require("SdkHost::SignNetworkConsent", consentHost,
		`{false, {}, Narrow(Localized("wallet_connect_failed")), e.what()});`)
	require("WalletPage::FailConsent", body("WalletPage.cpp", page, "void WalletPage::FailConsent("),
		"const std::string key = snconsent::RefusalKey(codeOrError);",
		"Notify(key.empty() ? fallback : Loc(key), InfoBarSeverity::Error);")
	require("WalletPage::ApplyConsentChallenge", challenge,
		"FailConsent(error, SnErrorText(TransportError(error)));")
	requirePattern("WalletPage::ApplyConsentChallenge", challenge,
		`BeginFlow\(consentFlow_, kBridgeTimeoutMs,[^;]*;\s*self->wallet\(\)\.Notify\(Loc\("sn_consent_expired"\), InfoBarSeverity::Error\);`,
		"a wallet that never answers before the challenge expires is not an expired request")
	require("WalletPage::ApplyConsentSigned", signed,
		"if (bridge::IsSuperseded(error)) {",
		`FailConsent(code, error.empty() ? Loc("wallet_connect_failed") : H(error));`)
	require("WalletPage::ApplyConsentSubmitted", submitted,
		"FailConsent(error, SnErrorText(TransportError(error)));",
		"FailConsent(result->error->code.value_or(std::string()),\n                SnErrorText(SetWalletError(*result->error)));")
	// right after a signing the status is read again: active, starting next epoch
	requirePattern("WalletPage::ApplyConsentSubmitted", submitted, `EndConsent\(\);\s*LoadConsentStatus\(\);\s*\}$`,
		"the status is not read again after the consent is stored")

	// a sign-out hides the consent that left and drops anything still out for it
	require("WalletPage::ResetForSignOut", body("WalletPage.cpp", page, "void WalletPage::ResetForSignOut("),
		"consent_.reset();", "consentFetch_ = snconsent::Fetch::Idle;", "++consentFlow_.generation;",
		"consentSigning_ = false;")

	// the panel sits right after settlement v2's section, its button is the
	// window's handler, and the folded pane's Earnings actions menu is its door
	xaml := sources["MainWindow.xaml"]
	v2 := strings.Index(xaml, `x:Name="SettlementV2Panel"`)
	consentAt := strings.Index(xaml, `x:Name="PayoutConsentPanel"`)
	top200 := strings.Index(xaml, `x:Name="Top200Panel"`)
	if v2 < 0 || consentAt < v2 || top200 < consentAt {
		misses = append(misses, "MainWindow.xaml: the payout consent is not right after settlement v2's section")
	}
	if consentAt >= 0 && top200 > consentAt {
		panelXaml := xaml[consentAt:top200]
		for _, name := range []string{"PayoutConsentHeading", "PayoutConsentStatusText", "PayoutConsentText", "PayoutConsentNote"} {
			require("MainWindow.xaml", panelXaml, `x:Name="`+name+`"`)
		}
		requirePattern("MainWindow.xaml", panelXaml, `x:Name="PayoutConsentButton"[^>]*Click="OnSignConsent"`,
			"PayoutConsentButton does not sign the consent")
	}
	require("MainWindow.xaml.cpp", sources["MainWindow.xaml.cpp"], "wallet_->OnSignConsent(s, e);")
	require("MainWindow.xaml.h", sources["MainWindow.xaml.h"], "void OnSignConsent(")
	require("App.vcxproj", sources["App.vcxproj"], `<ClInclude Include="SnConsentPresentation.h" />`)
	menu := body("WalletPage.cpp", page, "void WalletPage::ShowEarningsActionsMenu(")
	requirePattern("WalletPage::ShowEarningsActionsMenu", menu,
		`consent\.Click\(\[weak\]\(IInspectable const& s, RoutedEventArgs const& e\) \{\s*`+
			`if \(auto self = weak\.get\(\)\) self->wallet\(\)\.OnSignConsent\(s, e\);\s*\}\);\s*`+
			`flyout\.Items\(\)\.Append\(consent\);`,
		"the Earnings actions menu has no item that signs the consent")
	requirePattern("WalletPage::ShowEarningsActionsMenu", menu,
		`const bool consentShown = w_\.PayoutConsentPanel\(\)\.Visibility\(\) == Visibility::Visible &&\s*`+
			`w_\.PayoutConsentButton\(\)\.Visibility\(\) == Visibility::Visible;\s*`+
			`consent\.Visibility\(consentShown \? Visibility::Visible : Visibility::Collapsed\);\s*`+
			`consent\.IsEnabled\(w_\.PayoutConsentButton\(\)\.IsEnabled\(\)\);`,
		"the menu's consent item does not follow the panel's button")
	require("WalletPage::ShowEarningsActionsMenu", menu, `consent.Text(Loc("sn_consent_title"));`)
	return misses
}

// The payout consent's wiring: the read and every state of the panel, the
// steps of the flow (the wallet, the typed coldkey checked before any request,
// the challenge, the signature under the consent purpose through the coldkey
// connect's connector, the submission only of the server's message), each error
// string, the window's panel and handler and the menu's door.
func TestSnConsentWiring(t *testing.T) {
	for _, miss := range snConsentWiringMisses(readSnConsentSources(t)) {
		t.Error(miss)
	}
}

// Each wiring assertion, with the change it pins taken out of a copy of the
// sources, reports exactly that.
func TestSnConsentWiringCatchesEachRegression(t *testing.T) {
	sources := readSnConsentSources(t)
	for _, control := range []struct {
		name, file, old, new, want string
	}{
		{"no read with the wallet's", "WalletPage.cpp", "  LoadLeaderboardIfShowing();\n  LoadConsentStatus();\n", "  LoadLeaderboardIfShowing();\n",
			"WalletPage::LoadWallet: missing LoadConsentStatus();"},
		{"the read is not the sdk's", "WalletPage.cpp", "Sdk().api().snNetworkConsentStatus(", "Sdk().api().snGetWallet(",
			"missing Sdk().api().snNetworkConsentStatus("},
		{"a failed read reads as no consent", "WalletPage.cpp", "ok ? snconsent::Fetch::Ready : snconsent::Fetch::Failed, status);",
			"snconsent::Fetch::Ready, status);", "ok ? snconsent::Fetch::Ready : snconsent::Fetch::Failed"},
		{"the panel ignores the signing", "WalletPage.cpp", "consent_, consentSigning_);", "consent_, false);",
			"missing consent_, consentSigning_);"},
		{"the panel ignores the connect out", "WalletPage.cpp", "snconsent::PanelFor(CanCallApi() && !connectingWallet_, consentFetch_,",
			"snconsent::PanelFor(CanCallApi(), consentFetch_,", "missing snconsent::PanelFor(CanCallApi() && !connectingWallet_, consentFetch_,"},
		{"no line without a consent", "WalletPage.cpp", `line = Loc("sn_consent_none");`, `line = hstring{};`,
			`missing line = Loc("sn_consent_none");`},
		{"the coldkey is not shortened", "WalletPage.cpp", `ShortAddress(view.coldkeySs58)`, `Widen(view.coldkeySs58)`,
			"sn_consent_active"},
		{"the button keeps its words", "WalletPage.cpp", "  if (!view.buttonKey.empty()) w_.PayoutConsentButton().Content(LocBox(view.buttonKey));\n", "",
			"LocBox(view.buttonKey)"},
		{"the button never waits", "WalletPage.cpp", "w_.PayoutConsentButton().IsEnabled(view.buttonEnabled);", "w_.PayoutConsentButton().IsEnabled(true);",
			"IsEnabled(view.buttonEnabled)"},
		{"a failed read says nothing", "WalletPage.cpp", `view.failed ? Loc("something_went_wrong")`, `view.failed ? hstring{}`,
			"the header does not say Loading, or that the read failed"},
		{"no title", "WalletPage.cpp", "  w_.PayoutConsentHeading().Text(Loc(\"sn_consent_title\"));\n", "",
			`w_.PayoutConsentHeading().Text(Loc("sn_consent_title"));`},
		{"the button ignores a connect out", "WalletPage.cpp", "                         connecting ? Loc(\"opening_wallet_in_browser\") : hstring{});\n  RebuildConsentPanel();",
			"                         connecting ? Loc(\"opening_wallet_in_browser\") : hstring{});", "WalletPage::SetConnectingWallet: missing RebuildConsentPanel();"},
		{"a consent action without a session", "WalletPage.cpp", "  if (consentSigning_ || connectingWallet_ || w_.sheetOpen()) return;\n  // before the chooser and the browser: every step of this ends in a server call\n  if (!CanCallApi()) {",
			"  if (consentSigning_ || connectingWallet_ || w_.sheetOpen()) return;\n  if (false) {", "WalletPage::OnSignConsent: missing if (!CanCallApi()) {"},
		{"no wallet chosen first", "WalletPage.cpp", "  ChooseBittensorWallet(w_.get_strong(), [weak](std::string walletId) {\n    auto self = weak.get();\n    if (!self || walletId.empty()) return;\n    EnterConsentColdkey(",
			"  SkipChooser(w_.get_strong(), [weak](std::string walletId) {\n    auto self = weak.get();\n    if (!self || walletId.empty()) return;\n    EnterConsentColdkey(",
			"the wallet is not chosen before the coldkey is asked for"},
		{"a challenge without a coldkey", "WalletPage.cpp", "      if (!page || coldkeySs58.empty()) return;\n", "      if (!page) return;\n",
			"missing if (!page || coldkeySs58.empty()) return;"},
		{"the coldkey is prefilled", "BittensorWalletDialogs.cpp", "  // empty on purpose: the coldkey earnings are paid to is the user's to choose\n  TextBox addressBox;\n",
			"  TextBox addressBox;\n  addressBox.Text(H(window->wallet().AttachedColdkey()));\n",
			"EnterConsentColdkey: the coldkey is prefilled"},
		{"the coldkey is not checked", "BittensorWalletDialogs.cpp", "[](std::string const& address) { return urnet::validateSs58(address); });",
			"[](std::string const&) { return true; });", "missing [](std::string const& address) { return urnet::validateSs58(address); });"},
		{"an invalid coldkey closes the form", "BittensorWalletDialogs.cpp", "      args.Cancel(true);  // keep the form, say what is wrong\n      errorText.Text(Loc(\"invalid_ss58_address\"));",
			"      errorText.Text(Loc(\"invalid_ss58_address\"));", "EnterConsentColdkey: missing args.Cancel(true);"},
		{"an invalid coldkey says nothing", "BittensorWalletDialogs.cpp", "      errorText.Text(Loc(\"invalid_ss58_address\"));\n", "",
			`missing errorText.Text(Loc("invalid_ss58_address"));`},
		{"no wait while signing", "WalletPage.cpp", "  consentSigning_ = true;\n  RebuildConsentPanel();\n", "  RebuildConsentPanel();\n",
			"the panel does not say the wallet is awaited"},
		{"the wait never ends", "WalletPage.cpp", "  consentSigning_ = false;\n  RebuildConsentPanel();\n}\n\nvoid WalletPage::OnClaimTop200", "  RebuildConsentPanel();\n}\n\nvoid WalletPage::OnClaimTop200",
			"the wait does not end"},
		{"the challenge is not the sdk's", "WalletPage.cpp", "Sdk().api().snNetworkConsentChallenge(", "Sdk().api().snNetworkWalletMappingChallenge(",
			"missing Sdk().api().snNetworkConsentChallenge("},
		{"the wallet signs a message the app built", "WalletPage.cpp", "      walletId, *result,\n", "      walletId, MakeMessage(*result),\n",
			"Sdk().SignNetworkConsent(\n      walletId, *result,"},
		{"the page also signs a login challenge", "WalletPage.cpp", "  const snconsent::Challenge challenge = snconsent::ChallengeFrom(*result);\n",
			"  const snconsent::Challenge challenge = snconsent::ChallengeFrom(*result);\n  Sdk().SignWithBittensorWallet(walletId, std::string(), \"login\", nullptr);\n",
			"WalletPage::ApplyConsentChallenge: calls SignWithBittensorWallet("},
		{"the wallet's answer is submitted as it came", "WalletPage.cpp", "      submission->coldkeySs58, submission->message, submission->signatureHex,", "      address, message, signature,",
			"submission->coldkeySs58, submission->message, submission->signatureHex,"},
		{"a refused answer says nothing", "WalletPage.cpp", "    Notify(Loc(\"sn_consent_failed\"), InfoBarSeverity::Error);\n", "",
			"an answer for another message or account is not refused with sn_consent_failed"},
		{"the session fetches a challenge of its own", "SdkHost.cpp", "    session->setNetworkConsentChallenge(challenge, BittensorNowMillis());",
			"    session->setChallenge(std::nullopt, BittensorNowMillis());",
			"SdkHost::SignNetworkConsent: calls setChallenge("},
		{"the consent signs under another purpose", "SdkHost.cpp", "const std::string purpose(bittensor::kPurposeConsent);", "const std::string purpose(bittensor::kPurposeConnect);",
			"const std::string purpose(bittensor::kPurposeConsent);"},
		{"the consent opens a bridge of its own", "SdkHost.cpp", "  OpenBittensorWallet(serial, session);\n}\n\nvoid SdkHost::HandleDeepLink(",
			"  wallet_.OpenBittensorBridge(session->bridgeUrl());\n}\n\nvoid SdkHost::HandleDeepLink(", "the Bittensor bridge opens from more than one place"},
		{"the consent does not take over the bridge", "SdkHost.cpp", "  CancelPendingWalletFlows(\"superseded by a payout consent signature\");\n", "",
			`CancelPendingWalletFlows("superseded by a payout consent signature");`},
		{"the bridge's refusal code is lost", "SdkHost.cpp", "                                                             session->walletId()),\n                                result->ErrorCode});",
			"                                                             session->walletId())});", "the bridge's refusal code does not reach the flow"},
		{"the session's expiry is lost", "SdkHost.cpp", `Narrow(Localized("wallet_connect_failed")), e.what()});`, `Narrow(Localized("wallet_connect_failed"))});`,
			"e.what()});"},
		{"refusals are not named", "WalletPage.cpp", "  const std::string key = snconsent::RefusalKey(codeOrError);\n", "  const std::string key;\n",
			"const std::string key = snconsent::RefusalKey(codeOrError);"},
		{"the challenge's refusal is not named", "WalletPage.cpp", "    urnw::LogError(\"earnings: snNetworkConsentChallenge failed: {}\", error);\n    FailConsent(error, SnErrorText(TransportError(error)));",
			"    urnw::LogError(\"earnings: snNetworkConsentChallenge failed: {}\", error);\n    EndConsent();", "WalletPage::ApplyConsentChallenge: missing FailConsent(error, SnErrorText(TransportError(error)));"},
		{"a wallet that never answers is a failed connect", "WalletPage.cpp", "      self->wallet().Notify(Loc(\"sn_consent_expired\"), InfoBarSeverity::Error);",
			"      self->wallet().Notify(Loc(\"wallet_connect_failed\"), InfoBarSeverity::Error);", "a wallet that never answers before the challenge expires is not an expired request"},
		{"a superseded signing is an error", "WalletPage.cpp", "    if (bridge::IsSuperseded(error)) {\n      // another wallet flow started", "    if (false) {\n      // another wallet flow started",
			"WalletPage::ApplyConsentSigned: missing if (bridge::IsSuperseded(error)) {"},
		{"the signing's refusal code is dropped", "WalletPage.cpp", `FailConsent(code, error.empty() ? Loc("wallet_connect_failed") : H(error));`, `FailConsent(std::string(), error.empty() ? Loc("wallet_connect_failed") : H(error));`,
			`FailConsent(code, error.empty()`},
		{"a mismatch is not named", "WalletPage.cpp", "    FailConsent(result->error->code.value_or(std::string()),\n", "    FailConsent(result->error->message,\n",
			"FailConsent(result->error->code.value_or(std::string()),"},
		{"no read after the consent is stored", "WalletPage.cpp", "  EndConsent();\n  LoadConsentStatus();\n}", "  EndConsent();\n}",
			"the status is not read again after the consent is stored"},
		{"a sign-out keeps the consent", "WalletPage.cpp", "  consent_.reset();\n", "",
			"WalletPage::ResetForSignOut: missing consent_.reset();"},
		{"a sign-out keeps the panel", "WalletPage.cpp", "  consentFetch_ = snconsent::Fetch::Idle;\n", "",
			"WalletPage::ResetForSignOut: missing consentFetch_ = snconsent::Fetch::Idle;"},
		{"the panel is not beside settlement v2", "MainWindow.xaml", `<StackPanel x:Name="PayoutConsentPanel" Visibility="Collapsed">`, `<StackPanel x:Name="PayoutConsentPanelMoved" Visibility="Collapsed">`,
			"the payout consent is not right after settlement v2's section"},
		{"the button signs nothing", "MainWindow.xaml", `Click="OnSignConsent" />`, `Click="OnClaimV2" />`,
			"PayoutConsentButton does not sign the consent"},
		{"the window does not forward the button", "MainWindow.xaml.cpp", "wallet_->OnSignConsent(s, e);", "wallet_->OnClaimV2(s, e);",
			"MainWindow.xaml.cpp: missing wallet_->OnSignConsent(s, e);"},
		{"the header is not in the project", "App.vcxproj", `<ClInclude Include="SnConsentPresentation.h" />`, "",
			`<ClInclude Include="SnConsentPresentation.h" />`},
		{"no menu door", "WalletPage.cpp", "  flyout.Items().Append(consent);\n", "",
			"the Earnings actions menu has no item that signs the consent"},
		{"the menu's door ignores the button", "WalletPage.cpp", "consent.IsEnabled(w_.PayoutConsentButton().IsEnabled());", "consent.IsEnabled(true);",
			"the menu's consent item does not follow the panel's button"},
	} {
		t.Run(control.name, func(t *testing.T) {
			mutated := maps.Clone(sources)
			if strings.Count(mutated[control.file], control.old) != 1 {
				t.Fatalf("%s no longer has exactly one %q; update this control", control.file, control.old)
			}
			mutated[control.file] = strings.Replace(mutated[control.file], control.old, control.new, 1)
			misses := snConsentWiringMisses(mutated)
			for _, miss := range misses {
				if strings.Contains(miss, control.want) {
					return
				}
			}
			t.Fatalf("negative control was not detected (want %q): %q", control.want, misses)
		})
	}
}

// The panel's words, the store's for every app (CONSENT.md §5) with the
// coldkey as Windows' {}, in every catalog with its placeholder, tagged
// windows, and every one of them named by the panel's sources.
var snConsentEnglish = map[string]string{
	"sn_consent_title":       "Payout consent",
	"sn_consent_none":        "No payout wallet yet. Earnings aren't paid until you sign.",
	"sn_consent_body":        "Sign once with your Bittensor wallet so this network's earnings are paid to it.",
	"sn_consent_sign":        "Sign with wallet",
	"sn_consent_signing":     "Waiting for your wallet's signature…",
	"sn_consent_active":      "Earnings are paid to {}",
	"sn_consent_change":      "Change wallet",
	"sn_consent_change_note": "A new wallet takes effect from the next epoch.",
	"sn_consent_expired":     "The request expired. Try again.",
	"sn_consent_failed":      "The signature didn't match this wallet. Try again.",
	"sn_consent_owner_only":  "Only the network owner can choose the payout wallet.",
}

// The value of resource `name` in a catalog's text, or "".
func reswTextValue(catalog string, name string) string {
	match := regexp.MustCompile(`(?s)<data name="` + regexp.QuoteMeta(name) + `" xml:space="preserve">\s*<value>(.*?)</value>`).
		FindStringSubmatch(catalog)
	if match == nil {
		return ""
	}
	return match[1]
}

// Every way the catalogs (by locale), the key list and the panel's sources
// miss the consent's strings.
func snConsentStringMisses(catalogs map[string]string, keyList string, sources string) []string {
	misses := []string{}
	tagged := readCatalogTaggedKeys(keyList)
	for _, key := range sortedNames(snConsentEnglish) {
		want := snConsentEnglish[key]
		if got := reswTextValue(catalogs["en"], key); got != want {
			misses = append(misses, fmt.Sprintf("en: %s = %q, want %q", key, got, want))
		}
		for _, locale := range sortedNames(catalogs) {
			if got := reswTextValue(catalogs[locale], key); got == "" ||
				strings.Count(got, "{}") != strings.Count(want, "{}") {
				misses = append(misses, fmt.Sprintf("%s: %s = %q", locale, key, got))
			}
		}
		if !tagged[key] {
			misses = append(misses, fmt.Sprintf("windows-keys.txt: %s is not tagged windows", key))
		}
		if !strings.Contains(sources, `"`+key+`"`) {
			misses = append(misses, fmt.Sprintf("the panel never names %s", key))
		}
	}
	return misses
}

func readSnConsentStrings(t *testing.T) (map[string]string, string, string) {
	t.Helper()
	root := repositoryRoot(t)
	paths, err := filepath.Glob(filepath.Join(root, "app", "src", "App", "Strings", "*", "Resources.resw"))
	if err != nil || len(paths) < 28 {
		t.Fatalf("catalogs: %v %v", paths, err)
	}
	catalogs := map[string]string{}
	for _, path := range paths {
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		catalogs[filepath.Base(filepath.Dir(path))] = string(data)
	}
	keyList, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "Strings", "windows-keys.txt"))
	if err != nil {
		t.Fatal(err)
	}
	sources := stripComments(readAppSource(t, "WalletPage.cpp")) +
		stripComments(readAppSource(t, "BittensorWalletDialogs.cpp")) +
		stripComments(readAppSource(t, "SnConsentPresentation.h"))
	return catalogs, string(keyList), sources
}

func TestSnConsentStrings(t *testing.T) {
	catalogs, keyList, sources := readSnConsentStrings(t)
	for _, miss := range snConsentStringMisses(catalogs, keyList, sources) {
		t.Error(miss)
	}
}

// Each string assertion, with its string taken out, reports exactly that.
func TestSnConsentStringsCatchEachRegression(t *testing.T) {
	catalogs, keyList, sources := readSnConsentStrings(t)
	drop := func(catalog, name string) string {
		return regexp.MustCompile(`(?s)\s*<data name="`+regexp.QuoteMeta(name)+`" xml:space="preserve">.*?</data>`).
			ReplaceAllString(catalog, "")
	}
	for _, control := range []struct {
		name     string
		mutate   func(catalogs map[string]string) (string, string)
		wantMiss string
	}{
		{"the English is another sentence", func(c map[string]string) (string, string) {
			c["en"] = strings.Replace(c["en"], "The request expired. Try again.", "Expired.", 1)
			return keyList, sources
		}, `en: sn_consent_expired = "Expired."`},
		{"a language lacks a key", func(c map[string]string) (string, string) {
			c["de"] = drop(c["de"], "sn_consent_owner_only")
			return keyList, sources
		}, `de: sn_consent_owner_only = ""`},
		{"a language drops the coldkey", func(c map[string]string) (string, string) {
			c["ja"] = strings.Replace(c["ja"], reswTextValue(c["ja"], "sn_consent_active"), "収益", 1)
			return keyList, sources
		}, `ja: sn_consent_active = "収益"`},
		{"a key is not tagged windows", func(map[string]string) (string, string) {
			return strings.Replace(keyList, "sn_consent_signing\n", "", 1), sources
		}, "windows-keys.txt: sn_consent_signing is not tagged windows"},
		{"the panel never shows a key", func(map[string]string) (string, string) {
			return keyList, strings.ReplaceAll(sources, `"sn_consent_change_note"`, `"sn_consent_body"`)
		}, "the panel never names sn_consent_change_note"},
	} {
		t.Run(control.name, func(t *testing.T) {
			mutated := maps.Clone(catalogs)
			mutatedKeys, mutatedSources := control.mutate(mutated)
			for _, miss := range snConsentStringMisses(mutated, mutatedKeys, mutatedSources) {
				if strings.Contains(miss, control.wantMiss) {
					return
				}
			}
			t.Fatalf("negative control was not detected (want %q)", control.wantMiss)
		})
	}
}

// Every way WalletPage.cpp still applies a consent read's stale answer (the
// review's W-1, CONSENT.md §10), one line each: the read carries its number,
// each read takes the next one and a sign-out moves it on, and
// ApplyConsentStatus refuses an answer that is not the newest read's before it
// stores anything.
func snConsentStaleStatusMisses(walletPage string) []string {
	misses := []string{}
	page := stripComments(walletPage)
	load := functionBody(page, "void WalletPage::LoadConsentStatus(")
	apply := functionBody(page, "void WalletPage::ApplyConsentStatus(")
	reset := functionBody(page, "void WalletPage::ResetForSignOut(")
	if load == "" || apply == "" || reset == "" {
		return append(misses, "WalletPage.cpp: no LoadConsentStatus, ApplyConsentStatus or ResetForSignOut")
	}
	// the status callback carries more than the queue and the window: this read's number
	if capture := regexp.MustCompile(`snNetworkConsentStatus\(\s*\[([^\]]*)\]`).FindStringSubmatch(load); capture == nil {
		misses = append(misses, "WalletPage::LoadConsentStatus: no snNetworkConsentStatus callback")
	} else {
		extra := []string{}
		for _, name := range strings.Split(capture[1], ",") {
			if name = strings.TrimSpace(name); name != "" && name != "queue" && name != "weak" {
				extra = append(extra, name)
			}
		}
		if len(extra) == 0 {
			misses = append(misses, "WalletPage::LoadConsentStatus: the status callback captures only ["+capture[1]+
				"]: an earlier read's answer cannot be told from the newest's")
		}
	}
	for _, want := range []string{
		"const uint32_t read = ++consentReadGeneration_;",
		"[queue, weak, read](std::optional<urnet::SnNetworkConsentStatus> result,",
		"queue.TryEnqueue([weak, read, status, ok] {",
		"read, ok ? snconsent::Fetch::Ready : snconsent::Fetch::Failed, status);",
	} {
		if !strings.Contains(load, want) {
			misses = append(misses, "WalletPage::LoadConsentStatus: missing "+want)
		}
	}
	// ApplyConsentStatus refuses before it stores
	if store := strings.Index(apply, "consent_ = status;"); store < 0 {
		misses = append(misses, "WalletPage::ApplyConsentStatus: no consent_ = status;")
	} else if !strings.Contains(apply[:store], "return") {
		misses = append(misses, "WalletPage::ApplyConsentStatus: stores every answer unconditionally")
	}
	if !regexp.MustCompile(`^\{\s*if \(read != consentReadGeneration_\) return;\s*consentFetch_ = fetch;\s*consent_ = status;`).MatchString(apply) {
		misses = append(misses, "WalletPage::ApplyConsentStatus: an answer that is not the newest read's is not dropped first")
	}
	if !strings.Contains(reset, "\n  ++consentReadGeneration_;\n") {
		misses = append(misses, "WalletPage::ResetForSignOut: a read from before the sign-out is still the newest")
	}
	return misses
}

// The review's W-1: a consent read's answer from before a sign-out (the
// network that left: B's owner would be told A's coldkey is paid) or from
// before a newer read (the read before a submission, answering after the one
// the submission started: "No payout wallet yet" right after a consent was
// stored) is dropped.
func TestSnConsentStatusDropsStaleAnswers(t *testing.T) {
	for _, miss := range snConsentStaleStatusMisses(readAppSource(t, "WalletPage.cpp")) {
		t.Error(miss)
	}
}

// Each stale-answer assertion, with the change it pins taken out of a copy of
// WalletPage.cpp, reports exactly that.
func TestSnConsentStatusDropsStaleAnswersCatchesEachRegression(t *testing.T) {
	page := readAppSource(t, "WalletPage.cpp")
	for _, control := range []struct {
		name, old, new, want string
	}{
		{"the read carries no number", "[queue, weak, read](std::optional<urnet::SnNetworkConsentStatus> result,",
			"[queue, weak](std::optional<urnet::SnNetworkConsentStatus> result,", "the status callback captures only"},
		{"the answer loses its number", "queue.TryEnqueue([weak, read, status, ok] {", "queue.TryEnqueue([weak, status, ok] {",
			"missing queue.TryEnqueue([weak, read, status, ok] {"},
		{"a read takes no new number", "const uint32_t read = ++consentReadGeneration_;", "const uint32_t read = consentReadGeneration_;",
			"missing const uint32_t read = ++consentReadGeneration_;"},
		{"every answer is stored", "  if (read != consentReadGeneration_) return;\n", "",
			"stores every answer unconditionally"},
		{"a stale answer is dropped after it is stored", "  if (read != consentReadGeneration_) return;\n  consentFetch_ = fetch;\n  consent_ = status;\n",
			"  consentFetch_ = fetch;\n  consent_ = status;\n  if (read != consentReadGeneration_) return;\n", "is not dropped first"},
		{"a sign-out keeps the read", "\n  ++consentReadGeneration_;\n", "\n",
			"a read from before the sign-out is still the newest"},
	} {
		t.Run(control.name, func(t *testing.T) {
			if strings.Count(page, control.old) != 1 {
				t.Fatalf("WalletPage.cpp no longer has exactly one %q; update this control", control.old)
			}
			for _, miss := range snConsentStaleStatusMisses(strings.Replace(page, control.old, control.new, 1)) {
				if strings.Contains(miss, control.want) {
					return
				}
			}
			t.Fatalf("negative control was not detected (want %q)", control.want)
		})
	}
}
