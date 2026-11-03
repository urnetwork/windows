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

// The one-tap Bittensor key and its claims on Windows (apple/BTEASY.md, the
// approved mocks' flows 1 to 4): the C++ spec of how the screens word what the
// sdk decides, and of the store's bytes (App/BittensorKeyPresentation.h,
// App/BittensorKeyStoreFormat.h, tools/bittensor-key-tests.cpp), with a
// negative control for each decision it pins; the spec against the generated
// SDK header when one is at hand; and the WinRT and Win32 half read on its
// source, since it cannot be built off Windows: the store stays on this device
// and this user, the decisions stay in the sdk, the words reach nothing but the
// screen and an explicit Copy, the funds disclaimer is the sdk's on every
// screen, and Sign out and Delete account warn of a key that isn't backed up.

// The headers the spec builds against, copied, with `mutation` applied to the
// one named `mutated` when it is set.
func buildBittensorKeyTests(t *testing.T, mutated string, mutation func(string) string, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("bittensor key tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, name := range []string{"BittensorKeyPresentation.h", "BittensorKeyStoreFormat.h"} {
		header, err := os.ReadFile(filepath.Join(root, "app", "src", "App", name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(header)
		if mutation != nil && name == mutated {
			content = mutation(content)
			if content == string(header) {
				t.Fatalf("negative control did not change %s", name)
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "bittensor-key-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(root, "app", "tools", "bittensor-key-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build bittensor key tests: %v\n%s", err, output)
	}
	return program
}

// Compile and execute the spec: the disclaimer as the owner's two store keys
// and nothing else, each failure in the store's words, the payout card's body
// for every state of the sdk's card and of the consent read, the Key screen's
// steps and texts, the reminder's step, the claim relay's reasons and notes,
// the amounts, and the store's file names, items and notes.
func TestBittensorKeyPresentation(t *testing.T) {
	program := buildBittensorKeyTests(t, "", nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("bittensor key: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Each decision the spec pins, put back wrong in a copy of its header, fails
// the spec in the case that names it.
func TestBittensorKeyPresentationCatchesEachRegression(t *testing.T) {
	const presentation = "BittensorKeyPresentation.h"
	const format = "BittensorKeyStoreFormat.h"
	for _, control := range []struct {
		name     string
		header   string
		old, new string
		want     string
	}{
		{"a disclaimer id the sdk never names shows as a key", presentation,
			"if (id == kDisclaimerFull || id == kDisclaimerFundsOnly) return std::string(id);",
			"if (!id.empty()) return std::string(id);",
			"an id the sdk never names"},
		{"a store that can't answer reads as one that refused", presentation,
			`text.key = "bittensor_key_failure_keychain";`, `text.key = "bittensor_key_failure_store";`,
			"the key for store_unavailable"},
		{"the server's own words are dropped", presentation,
			"    text.message = std::string(message);\n", "    text.key = \"bittensor_key_failure_consent\";\n",
			"the message for consent_failed"},
		{"the key path shows to a session the sdk doesn't show it to", presentation,
			"      if (!showsKeyPath) return layout;\n", "      (void)showsKeyPath;\n",
			"a guest or a session that isn't the owner's"},
		{"a linked key draws the wallet card", presentation,
			"        layout.body = CardBody::Linked;", "        layout.body = CardBody::WalletCard;",
			"1c, 1i"},
		{"a key under another consent loses its lines", presentation,
			"        layout.otherConsentLines = true;", "        layout.otherConsentLines = false;",
			"a key here under another consent"},
		{"payouts read as on before they start", presentation,
			`return payoutsStartNextEpoch ? "bittensor_key_payouts_start" : "bittensor_key_payouts_on";`,
			`(void)payoutsStartNextEpoch; return "bittensor_key_payouts_on";`,
			"next epoch"},
		{"what deleting means stays over the words", presentation,
			"  if (confirming != kConfirmNone) return KeyStep::Confirm;\n  if (deleteExplained) return KeyStep::DeleteExplained;",
			"  if (deleteExplained) return KeyStep::DeleteExplained;\n  if (confirming != kConfirmNone) return KeyStep::Confirm;",
			"the words win once the sdk asks"},
		{"Retry shows while the key signs", presentation,
			"  if (busy) return KeyBanner::Signing;\n  if (offersRetry) return KeyBanner::Retry;",
			"  if (offersRetry) return KeyBanner::Retry;\n  if (busy) return KeyBanner::Signing;",
			"busy"},
		{"Save now's address bullet names no address", presentation,
			`{{"bittensor_key_save_now_bullet_address", true},`, `{{"bittensor_key_save_now_bullet_address", false},`,
			"it names the address"},
		{"Keep is drawn as destructive", presentation,
			`"bittensor_key_keep_prompt", "bittensor_key_keep_confirm_2", false};`,
			`"bittensor_key_keep_prompt", "bittensor_key_keep_confirm_2", true};`,
			"Keep is not destructive"},
		{"the countdown rounds down", presentation,
			"const int64_t seconds = (left + 999) / 1000;", "const int64_t seconds = left / 1000;",
			"rounded up"},
		{"a schedule is reported as if the system would deliver it", presentation,
			"if (action == kReminderSchedule) return ReminderStep::Arm;",
			"if (action == kReminderSchedule) return ReminderStep::PostNow;",
			"a schedule is waited for, not reported"},
		{"a paused relay reads as claiming", presentation,
			"return reason != kReasonRelayNotRunning && reason != kReasonRelayPaused;",
			"return reason != kReasonRelayNotRunning;",
			"paused"},
		{"the threshold names SN25α with no price", presentation,
			"text.detail = 0 < thresholdRao ? ReasonDetail::Threshold : ReasonDetail::ThresholdNoPrice;",
			"(void)thresholdRao; text.detail = ReasonDetail::Threshold;",
			"the detail for under_threshold"},
		{"the sheet says the tile's note", presentation,
			"return inSheet ? ClaimNowNote::PaidEach : ClaimNowNote::Paid;", "(void)inSheet; return ClaimNowNote::Paid;",
			"the sheet's, per coldkey"},
		{"the claimed state never says when Claim now is back", presentation,
			"return 0 < nextAtMillis ? ClaimNowNote::AgainOn : ClaimNowNote::None;",
			"(void)nextAtMillis; return ClaimNowNote::None;",
			"the claimed state's limit"},
		{"an estimate keeps no decimal under ten", presentation,
			"  if (10 <= value) {", "  if (1 <= value) {",
			"one to ten"},
		{"a dot in a network id reaches the folder", format,
			"if (!letter && !digit && c != '-') return std::nullopt;",
			"if (!letter && !digit && c != '-' && c != '.') return std::nullopt;",
			"refused: .."},
		{"every network's item unprotects with any other's", format,
			"  entropy.append(networkId);\n", "  (void)networkId;\n",
			"two networks, two entropies"},
		{"bytes without the magic read as an item", format,
			"if (kMaxFileBytes < bytes.size() || !bytes.starts_with(kItemMagic)) return std::nullopt;\n  bytes.remove_prefix(kItemMagic.size());",
			"if (kMaxFileBytes < bytes.size()) return std::nullopt;\n  if (bytes.starts_with(kItemMagic)) bytes.remove_prefix(kItemMagic.size());",
			"no magic"},
		{"a note takes a line break", format,
			"if (c == '\\n' || c == '\\r' || c == '\\t' || c == '\\0') return false;",
			"if (c == '\\t' || c == '\\0') return false;",
			"a line break in the value"},
		{"an empty note stays", format,
			"    notes.erase(std::string(name));", "    notes[std::string(name)] = std::string(value);",
			"removes the note"},
	} {
		t.Run(control.name, func(t *testing.T) {
			program := buildBittensorKeyTests(t, control.header, func(source string) string {
				if strings.Count(source, control.old) != 1 {
					t.Fatalf("%s no longer has exactly one %q; update this control", control.header, control.old)
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

// Every id the header mirrors against the generated SDK header's own, and the
// card, the screen and the claim status the views read as the header's types.
// The header is git-ignored (fetch-deps unpacks it into
// app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE names another
// directory) and needs nlohmann/json, so a host without them skips this, and so
// does a copy from before the claim status's TAO figures.
func TestBittensorKeyAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := sdkHeaderDirWith(t, root, "pays_key_on_this_device")
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the one-tap key's flow and the claim relay (set URNETWORK_SDK_INCLUDE)")
	}
	header, err := os.ReadFile(filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	if err != nil {
		t.Fatal(err)
	}
	// the calls the app makes, as the header declares them
	for _, want := range []string{
		"uint64_t urnet_new_bittensor_local_key_flow(uint64_t api, const char* network_id, urnet_bittensor_local_key_store_add_key_cb store_add_key, urnet_bittensor_local_key_store_delete_key_cb store_delete_key, urnet_bittensor_local_key_store_key_address_cb store_key_address, urnet_bittensor_local_key_store_key_phrase_cb store_key_phrase, urnet_bittensor_local_key_store_read_note_cb store_read_note, urnet_bittensor_local_key_store_write_note_cb store_write_note, void* store_user_data);",
		"uint64_t urnet_bittensor_local_key_flow_add_bittensor_local_key_flow_listener(uint64_t self, urnet_bittensor_local_key_flow_cb listener_bittensor_local_key_flow_changed, void* listener_user_data);",
		"char* urnet_copy_string(const char* s);",
	} {
		if !strings.Contains(string(header), want) && !strings.Contains(sdkCHeader(t, sdkDir), want) {
			t.Errorf("urnetwork_sdk.h: missing %s", want)
		}
	}
	for _, want := range []string{
		"std::optional<BittensorLocalKeyCard> getCard() const;",
		"std::optional<BittensorLocalKeyScreen> getScreen() const;",
		"std::optional<BittensorLocalKeyWordCheck> checkWords(const std::string& first_word, const std::string& second_word) const;",
		"void submitWords(const std::string& first_word, const std::string& second_word, bool authenticated) const;",
		"void reveal(bool authenticated) const;",
		"std::optional<StringList> revealedWords() const;",
		"std::string phraseToCopy() const;",
		"std::string fundsDisclaimerAt(const std::string& place) const;",
		"std::optional<BittensorLocalKeyReminder> reminder(int64_t current_epoch, int64_t claim_open_millis, int64_t epoch_blocks, bool claimable) const;",
		"void snClaimStatus(SnClaimStatusCallback callback) const;",
		"void snClaimNow(const std::string& coldkey_ss58, SnClaimNowCallback callback) const;",
		// what the sdk decides for the page: the card pays a key here, the tile
		// shows, the claim is drawn as claimed with its transaction
		"bool pays_key_on_this_device{};",
		"bool shows_tile{};",
		"bool shows_claimed{};",
		"std::optional<std::string> claim_tx_url;",
		"std::optional<int64_t> claim_amount_rao;",
	} {
		if !strings.Contains(string(header), want) {
			t.Errorf("urnetwork_sdk.hpp: missing %s", want)
		}
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_BITTENSOR_KEY_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	program := buildBittensorKeyTests(t, "", nil, extra...)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("bittensor key against the sdk header: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func readBittensorKeyFile(t *testing.T, path string) string {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

// The C header beside the wrapper, or "".
func sdkCHeader(t *testing.T, dir string) string {
	t.Helper()
	data, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.h"))
	if err != nil {
		return ""
	}
	return string(data)
}

// The app's sources of the key, by name, without comments.
func bittensorKeySources(t *testing.T) map[string]string {
	t.Helper()
	sources := map[string]string{}
	for _, name := range []string{
		"BittensorKeyStore.h", "BittensorKeyStore.cpp", "BittensorKeyStoreFormat.h",
		"BittensorKeyPlatform.h", "BittensorKeyPlatform.cpp", "BittensorKeyPresentation.h",
		"BittensorKeyViews.h", "BittensorKeyViews.cpp", "BittensorKeySheets.h", "BittensorKeySheets.cpp",
	} {
		sources[name] = stripComments(readAppSource(t, name))
	}
	return sources
}

// Each of `wants` is in `source`, a comment-stripped source: a want's own
// inline comments are blanked the same way, so it reads as the code does.
func requireBittensorKey(t *testing.T, where, source string, wants ...string) {
	t.Helper()
	for _, want := range wants {
		if !strings.Contains(source, stripComments(want)) {
			t.Errorf("%s: missing %s", where, want)
		}
	}
}

// The first `before` comes before the first `after` in `source`, and both are there.
func requireBittensorKeyOrder(t *testing.T, where, source string, steps ...string) {
	t.Helper()
	at := 0
	for _, step := range steps {
		step = stripComments(step)
		found := strings.Index(source[at:], step)
		if found < 0 {
			t.Errorf("%s: missing %s, or out of order", where, step)
			return
		}
		at += found + len(step)
	}
}

func bittensorKeyBody(t *testing.T, file, source, signature string) string {
	t.Helper()
	body := functionBody(source, signature)
	if body == "" {
		t.Fatalf("%s: no %s", file, signature)
	}
	return body
}

// The store keeps the item on this device, for this user, never roaming: DPAPI
// for the current user with its network's entropy, under the app's local root
// beside the sdk's storage (which a sign-out clears) rather than in it; an add
// never replaces an item; and nothing it logs carries a phrase, an address or a
// note.
func TestBittensorKeyStoreStaysOnThisDevice(t *testing.T) {
	store := bittensorKeySources(t)["BittensorKeyStore.cpp"]
	requireBittensorKey(t, "BittensorKeyStore.cpp", store,
		`return StorageRoot(/*isService=*/false) / L"bittensor_key";`,
		"::CryptProtectData(&input, btkeystore::kProtectDescription, &entropy, nullptr, nullptr,\n                          CRYPTPROTECT_UI_FORBIDDEN, &output)",
		"::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,",
		"Protect(mnemonic, btkeystore::Entropy(networkId))",
		"Unprotect(item->protectedPhrase, btkeystore::Entropy(networkId))",
		"if (::GetFileAttributesW(path->c_str()) != INVALID_FILE_ATTRIBUTES) return false;",
		"WriteBytes(*path, *bytes, /*replace=*/false)",
		"const DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);",
		"static BittensorKeyStore* store = new BittensorKeyStore();",
		"::SecureZeroMemory(output.pbData, output.cbData);",
	)
	for _, forbidden := range []string{
		"CRYPTPROTECT_LOCAL_MACHINE", "PasswordVault", "CredWrite", "FOLDERID_RoamingAppData", "APPDATA",
		"SdkStorageDir(", "LoadAppPrefs", "SaveAppPref",
	} {
		if strings.Contains(store, forbidden) {
			t.Errorf("BittensorKeyStore.cpp uses %s", forbidden)
		}
	}
	// an add never replaces, a delete of nothing is no refusal, and a read that
	// fails says the store can't answer, which is never "no key"
	add := bittensorKeyBody(t, "BittensorKeyStore.cpp", store, "bool BittensorKeyStore::AddKey(")
	if strings.Contains(add, "replace=*/true") {
		t.Error("AddKey can replace an item")
	}
	phrase := bittensorKeyBody(t, "BittensorKeyStore.cpp", store, "std::string BittensorKeyStore::KeyPhrase(")
	requireBittensorKey(t, "BittensorKeyStore::KeyPhrase", phrase,
		"case Read::Missing:\n      return std::string();", "case Read::Failed:\n      return Unavailable();",
		"if (!item) return Unavailable();", "if (!phrase || phrase->empty()) return Unavailable();")
	// the local root is the user's own, not a roaming profile
	paths := stripComments(readBittensorKeyFile(t, filepath.Join(repositoryRoot(t), "app", "src", "Common", "Paths.cpp")))
	if !strings.Contains(paths, ": KnownFolder(FOLDERID_LocalAppData);") {
		t.Error("Paths.cpp: the app's root is no longer %LOCALAPPDATA%")
	}
	// a sign-out leaves the key: nothing in the sign-out or the sdk host reaches it
	for _, name := range []string{"SdkHost.cpp", "SdkHost.h"} {
		source := stripComments(readAppSource(t, name))
		for _, forbidden := range []string{"bittensor_key", "BittensorKeyStore", "ForgetBittensorKey"} {
			if strings.Contains(source, forbidden) {
				t.Errorf("%s reaches the key's store (%s): a sign-out must leave the key", name, forbidden)
			}
		}
	}
	// no log line names anything the store holds
	logCall := regexp.MustCompile(`Log(?:Info|Warn|Error|Debug)\(([^;]*)\);`)
	for name, source := range bittensorKeySources(t) {
		for _, match := range logCall.FindAllStringSubmatch(source, -1) {
			for _, secret := range []string{"phrase", "mnemonic", "address", "networkId", "words", "first", "second", "name", "value"} {
				if regexp.MustCompile(`,\s*[^,]*\b` + secret + `\b`).MatchString(match[1]) {
					t.Errorf("%s logs %s: %s", name, secret, match[0])
				}
			}
		}
	}
}

// The sdk decides. The app makes no key, signs nothing and derives no address
// itself, and it makes the sdk's flow in one place, over the store, so every
// platform keeps the same rules (apple's TestBittensorKeyDecisionsStayInTheSdk).
func TestBittensorKeyDecisionsStayInTheSdk(t *testing.T) {
	files := appSourceFiles(t, ".cpp", ".h")
	makers := 0
	for _, name := range sortedNames(files) {
		code := stripComments(files[name])
		for _, forbidden := range []string{
			"bittensorLocalKeyGenerate(", "bittensorLocalKeySign(", "bittensorLocalKeyAddress(",
			"urnet_bittensor_local_key_generate", "urnet_bittensor_local_key_sign", "urnet_bittensor_local_key_address",
			"newBittensorLocalKeyFlow(",
		} {
			if strings.Contains(code, forbidden) {
				t.Errorf("%s calls %s: only the sdk's flow makes a key or signs with one, and the app makes it once", name, forbidden)
			}
		}
		makers += strings.Count(code, "urnet_new_bittensor_local_key_flow(")
		if strings.Contains(code, "urnet_new_bittensor_local_key_flow(") && name != "BittensorKeyStore.cpp" {
			t.Errorf("%s makes the sdk's flow; BittensorKeyStore.cpp's NewBittensorKeyFlow is the one place", name)
		}
	}
	if makers != 1 {
		t.Errorf("the sdk's flow is made %d times, want once (NewBittensorKeyFlow)", makers)
	}
	store := bittensorKeySources(t)["BittensorKeyStore.cpp"]
	requireBittensorKey(t, "NewBittensorKeyFlow", bittensorKeyBody(t, "BittensorKeyStore.cpp", store, "urnet::BittensorLocalKeyFlow NewBittensorKeyFlow("),
		"&AddKeyThunk, &DeleteKeyThunk, &KeyAddressThunk,",
		"&KeyPhraseThunk, &ReadNoteThunk, &WriteNoteThunk, nullptr)")
	// the wallet's own consent path stays the wallets'
	page := stripComments(readAppSource(t, "WalletPage.cpp"))
	for _, signature := range []string{
		"void WalletPage::StartConsent(", "void WalletPage::ApplyConsentChallenge(",
		"void WalletPage::ApplyConsentSigned(", "void WalletPage::ApplyConsentSubmitted(",
	} {
		if strings.Contains(bittensorKeyBody(t, "WalletPage.cpp", page, signature), "keyFlow_") {
			t.Errorf("%s reaches the key's flow", signature)
		}
	}
	// the page runs the flow's calls; it never decides a state of its own
	requireBittensorKey(t, "WalletPage::OnKeyAction", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::OnKeyAction("),
		"if (keyCard_->replaces_payout_wallet) {", "ConfirmKeyReplacement(action);", "RunKeyAction(action, /*replaceConfirmed=*/false);")
	requireBittensorKey(t, "WalletPage::RunKeyAction", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::RunKeyAction("),
		"keyFlow_->setUpPayouts(replaceConfirmed);", "keyFlow_->finishSetup(replaceConfirmed);",
		"keyFlow_->saveFirst(replaceConfirmed);")
	requireBittensorKey(t, "WalletPage::FeedKeyConsent", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::FeedKeyConsent("),
		"keyFlow_->setConsent(true, consent_->coldkeySs58, consent_->fromEpoch);",
		"keyFlow_->setConsent(false, std::string(), 0);", "keyFlow_->setConsentUnavailable();",
		"keyFlow_->setCurrentEpoch(", "keyFlow_->setGuest(", "keyFlow_->setTop200Registered(keyTop200Registered_);")
}

// The words reach nothing but the screen and an explicit Copy: they are in the
// visual tree only while they show, as text nothing selects; Copy keeps them out
// of the clipboard history and the cloud clipboard and takes them off after the
// sdk's 60 s; the window stays out of screen capture while the Key screen is up;
// and they hide when the window loses focus or hides.
func TestBittensorKeyWordsReachOnlyTheScreen(t *testing.T) {
	sources := bittensorKeySources(t)
	for name, source := range sources {
		if strings.Contains(source, "IsTextSelectionEnabled(true)") {
			t.Errorf("%s makes text selectable", name)
		}
		for _, call := range []string{"phraseToCopy()", "revealedWords()", "CopyPhrase("} {
			if strings.Contains(source, call) && name != "BittensorKeySheets.cpp" && !(call == "CopyPhrase(" && strings.HasPrefix(name, "BittensorKeyPlatform")) {
				t.Errorf("%s calls %s: only the Key screen shows or copies the words", name, call)
			}
		}
	}
	for name, source := range appSourceFiles(t, ".cpp", ".h") {
		if strings.HasPrefix(name, "BittensorKey") {
			continue
		}
		code := stripComments(source)
		for _, call := range []string{"phraseToCopy(", "revealedWords(", "keyPhrase(", "KeyPhrase("} {
			if strings.Contains(code, call) {
				t.Errorf("%s calls %s: the words leave the flow only on the Key screen", name, call)
			}
		}
	}
	sheets := sources["BittensorKeySheets.cpp"]
	words := bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "UIElement BittensorKeySheet::BuildWords(")
	requireBittensorKeyOrder(t, "BittensorKeySheet::BuildWords", words,
		"if (!screen_.revealed) {", "return cover;", "flow_->revealedWords()", "::SecureZeroMemory(word.data(), word.size());")
	requireBittensorKeyOrder(t, "BittensorKeySheet::Copy", bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "void BittensorKeySheet::Copy("),
		"phrase = flow_->phraseToCopy();", "if (phrase.empty()) return;", "btkeyplatform::CopyPhrase(phrase, queue_);",
		"::SecureZeroMemory(phrase.data(), phrase.size());")
	platform := sources["BittensorKeyPlatform.cpp"]
	requireBittensorKey(t, "btkeyplatform::CopyPhrase", bittensorKeyBody(t, "BittensorKeyPlatform.cpp", platform, "bool CopyPhrase("),
		"options.IsAllowedInHistory(false);", "options.IsRoamable(false);",
		"if (!dt::Clipboard::SetContentWithOptions(package, options)) {", "ClearLater(queue, ::GetClipboardSequenceNumber());")
	if strings.Contains(platform, "Clipboard::SetContent(") {
		t.Error("BittensorKeyPlatform.cpp copies with SetContent, which opts into the history and the cloud clipboard")
	}
	requireBittensorKey(t, "btkeyplatform::ClearLater", platform,
		"co_await winrt::resume_after(std::chrono::seconds(urnet::BittensorLocalKeyCopySeconds));",
		"if (::GetClipboardSequenceNumber() != sequence) return;", "dt::Clipboard::Clear();")
	requireBittensorKey(t, "btkeyplatform::CaptureExclusion", platform,
		"::SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE)", "::SetWindowDisplayAffinity(window_, WDA_MONITOR)",
		"if (applied_) ::SetWindowDisplayAffinity(window_, previous_);")
	build := bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "void BittensorKeySheet::Build(")
	requireBittensorKey(t, "BittensorKeySheet::Build", build,
		"capture_ = std::make_unique<btkeyplatform::CaptureExclusion>(window_);", "self->capture_.reset();")
	// hidden on deactivate, while the window doesn't present, and closed when it hides
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	if !regexp.MustCompile(`WindowActivationState::Deactivated\) \{\s*if \(auto self = weak\.get\(\)\) self->wallet_->OnWindowDeactivated\(\);\s*return;`).MatchString(window) {
		t.Error("MainWindow.xaml.cpp: the words don't hide when the window loses focus")
	}
	requireBittensorKey(t, "MainWindow::SetPresentationActive", bittensorKeyBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::SetPresentationActive("),
		"if (!active) wallet_->OnWindowDeactivated();")
	requireBittensorKey(t, "MainWindow::CloseSheetsForHide", bittensorKeyBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::CloseSheetsForHide("),
		"wallet_->CloseKeyScreenForHide();")
	page := stripComments(readAppSource(t, "WalletPage.cpp"))
	requireBittensorKey(t, "WalletPage::OnWindowDeactivated", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::OnWindowDeactivated("),
		"keySheet_->HideWords();", "keyFlow_->hide();")
	requireBittensorKey(t, "WalletPage::CloseKeyScreenForHide", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::CloseKeyScreenForHide("),
		"keySheet_->CloseNow();")
	// leaving the screen, however it closes, is the sdk's CloseScreen
	requireBittensorKeyOrder(t, "WalletPage::ShowKeySheet", bittensorKeyBody(t, "WalletPage.cpp", page, "winrt::fire_and_forget WalletPage::ShowKeySheet("),
		"if (!screen || !screen->open) co_return;", "co_await keySheet_->Dialog().ShowAsync();", "flow->closeScreen();")
}

// Windows Hello where the sdk asks for device authentication, and nowhere else:
// at Tap to reveal for a stored key, and at Delete's button after the two words
// for a stored key, stopping on anything but a verified answer; Keep and Save
// now ask nothing. The words hide before what deleting means shows.
func TestBittensorKeyAuthenticationWhereTheSdkAsks(t *testing.T) {
	sheets := bittensorKeySources(t)["BittensorKeySheets.cpp"]
	requireBittensorKeyOrder(t, "BittensorKeySheet::TapToReveal", bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "winrt::fire_and_forget BittensorKeySheet::TapToReveal("),
		"bool authenticated = false;", "if (screen_.needs_authentication) {",
		`authenticated = co_await btkeyplatform::Authenticate(window_, Loc("bittensor_key_auth_reason"));`,
		"flow_->reveal(authenticated);")
	requireBittensorKeyOrder(t, "BittensorKeySheet::Submit", bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "winrt::fire_and_forget BittensorKeySheet::Submit("),
		"if (screen_.confirm_needs_authentication) {",
		`authenticated = co_await btkeyplatform::Authenticate(window_, Loc("bittensor_key_auth_reason"));`,
		"if (!authenticated) {", "co_return;", "flow_->submitWords(first, second, authenticated);")
	requireBittensorKeyOrder(t, "BittensorKeySheet::ExplainDelete", bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "void BittensorKeySheet::ExplainDelete("),
		"flow_->hide();", "deleteExplained_ = true;")
	main := bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "void BittensorKeySheet::BuildMain(")
	requireBittensorKeyOrder(t, "BittensorKeySheet::BuildMain", main, "if (screen_.offers_keep) {", "self->flow_->confirmKeep();")
	if strings.Count(sheets, "btkeyplatform::Authenticate(") != 2 {
		t.Error("BittensorKeySheets.cpp asks Windows Hello outside Tap to reveal and Delete's button")
	}
	platform := bittensorKeySources(t)["BittensorKeyPlatform.cpp"]
	requireBittensorKey(t, "btkeyplatform::Authenticate", platform,
		"interop->RequestVerificationForWindowAsync(",
		"verified = result == credui::UserConsentVerificationResult::Verified;",
		"case credui::UserConsentVerifierAvailability::NotConfiguredForUser:")
}

// The two-word fields learn nothing and suggest nothing, and the confirmation
// uses only them; each keystroke asks the sdk's check, which changes nothing.
func TestBittensorKeyWordFieldsLearnNothing(t *testing.T) {
	sheets := bittensorKeySources(t)["BittensorKeySheets.cpp"]
	requireBittensorKey(t, "QuietWordField", bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "TextBox QuietWordField("),
		"box.IsSpellCheckEnabled(false);", "box.IsTextPredictionEnabled(false);",
		"password.NameValue(winrt::Microsoft::UI::Xaml::Input::InputScopeNameValue::Password);",
		"box.InputScope(scope);")
	confirm := bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "void BittensorKeySheet::BuildConfirm(")
	if !strings.Contains(confirm, "box = QuietWordField(name);") || strings.Contains(confirm, "TextBox()") || strings.Contains(confirm, "PasswordBox") {
		t.Error("the two-word check doesn't use the quiet field")
	}
	requireBittensorKey(t, "BittensorKeySheet::OnWordsTyped", bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "void BittensorKeySheet::OnWordsTyped("),
		"check = flow_->checkWords(", "check->first == btkey::kWordWrong", "wordsReady_ = check && check->ready;")
}

// The owner's disclaimer on every screen of the easy flows, once a screen, always
// as the sdk names it: the payout card, each step of the Key screen, the claim
// tile, the claim sheet and Change wallet's key offer. Its words are the
// store's, word for word, and no source spells them.
func TestBittensorKeyDisclaimerFromTheSdk(t *testing.T) {
	sources := bittensorKeySources(t)
	page := stripComments(readAppSource(t, "WalletPage.cpp"))
	requireBittensorKey(t, "WalletPage::RebuildKeyCard", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::RebuildKeyCard("),
		"btkeyviews::ShowFundsDisclaimer(w_.PayoutKeyDisclaimerHost(), card.funds_disclaimer, false);")
	claim := bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::ApplyClaimDisclaimers(")
	requireBittensorKey(t, "WalletPage::ApplyClaimDisclaimers", claim,
		"keyCard_->claim_funds_disclaimer",
		"btkeyviews::ShowFundsDisclaimer(w_.UnclaimedDisclaimerHost(), v1Shown ? disclaimer : std::string(), false);",
		"!v1Shown && v2Shown ? disclaimer : std::string(), false);")
	requireBittensorKey(t, "WalletPage::OpenClaimRelaySheet", bittensorKeyBody(t, "WalletPage.cpp", page, "winrt::fire_and_forget WalletPage::OpenClaimRelaySheet("),
		"KeyDisclaimerAt(btkey::kPlaceClaimSheet)")
	requireBittensorKey(t, "WalletPage::KeyOfferForConsent", bittensorKeyBody(t, "WalletPage.cpp", page, "pages::WalletKeyOffer WalletPage::KeyOfferForConsent("),
		"if (!offerKey || !keyFlow_ || !keyCard_ || !keyCard_->offers_set_up) return offer;",
		"offer.disclaimer = KeyDisclaimerAt(btkey::kPlaceChangeWallet);")
	dialogs := stripComments(readAppSource(t, "BittensorWalletDialogs.cpp"))
	requireBittensorKey(t, "ChooseBittensorWallet", dialogs, "btkeyviews::MakeFundsDisclaimer(keyOffer.disclaimer, /*boxed=*/true)")
	sheets := sources["BittensorKeySheets.cpp"]
	for _, signature := range []string{
		"void BittensorKeySheet::BuildMain(", "void BittensorKeySheet::BuildDeleteExplained(", "void BittensorKeySheet::BuildConfirm(",
	} {
		body := bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, signature)
		if strings.Count(body, stripComments("btkeyviews::MakeFundsDisclaimer(screen_.funds_disclaimer, /*boxed=*/true)")) != 1 {
			t.Errorf("%s doesn't show the screen's disclaimer exactly once", signature)
		}
	}
	requireBittensorKey(t, "ClaimRelaySheet::Build", bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "void ClaimRelaySheet::Build("),
		"btkeyviews::MakeFundsDisclaimer(disclaimer_, /*boxed=*/true)")
	views := sources["BittensorKeyViews.cpp"]
	requireBittensorKey(t, "btkeyviews::MakeFundsDisclaimer", bittensorKeyBody(t, "BittensorKeyViews.cpp", views, "FrameworkElement MakeFundsDisclaimer("),
		"const std::string key = btkey::DisclaimerKey(disclaimer);", "if (key.empty()) return nullptr;",
		"auto words = MakeText(Loc(key), 12, colors::TextBrush());")
	// the owner's words, from the store, as the owner wrote them
	resw := readBittensorKeyFile(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "Strings", "en", "Resources.resw"))
	for key, want := range map[string]string{
		"bittensor_funds_disclaimer":            "This key is stored locally in the app secure storage. It is never shared with URnetwork. URnetwork does not touch and is not responsible for any user funds.",
		"bittensor_funds_disclaimer_funds_only": "URnetwork does not touch and is not responsible for any user funds.",
	} {
		match := regexp.MustCompile(`<data name="` + key + `" xml:space="preserve">\s*<value>([^<]*)</value>`).FindStringSubmatch(resw)
		if match == nil || match[1] != want {
			t.Errorf("Resources.resw: %s is not the owner's words", key)
		}
	}
	for name, source := range appSourceFiles(t, ".cpp", ".h") {
		if strings.Contains(stripComments(source), "responsible for any user funds") {
			t.Errorf("%s spells the disclaimer instead of looking it up", name)
		}
	}
}

// The card leads Earnings while the key path needs action, under the backup
// banner, and keeps its place after settlement v2 otherwise; Back up first from
// Sign out and Delete account, and the reminder's click, open the Key screen;
// a sign-out keeps the key and an account deletion forgets it first.
func TestBittensorKeyCardLeadsAndBackUpFirst(t *testing.T) {
	xaml := readAppSource(t, "MainWindow.xaml")
	stack := strings.Index(xaml, `<StackPanel x:Name="WalletPaneAStack">`)
	banner := strings.Index(xaml, `<StackPanel x:Name="BittensorKeyBannerHost" Visibility="Collapsed" />`)
	figure := strings.Index(xaml, `x:Name="PointsHeadlineValue"`)
	if stack < 0 || banner < stack || figure < banner {
		t.Error("MainWindow.xaml: the backup banner doesn't lead pane A")
	}
	for _, host := range []string{"PayoutKeyHost", "PayoutKeyDisclaimerHost", "UnclaimedDisclaimerHost", "SettlementV2RelayHost", "SettlementV2DisclaimerHost"} {
		if !strings.Contains(xaml, `x:Name="`+host+`"`) {
			t.Errorf("MainWindow.xaml: no %s", host)
		}
	}
	page := stripComments(readAppSource(t, "WalletPage.cpp"))
	requireBittensorKey(t, "WalletPage::PlaceConsentPanel", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::PlaceConsentPanel("),
		"const bool lead = keyCard_ && keyCard_->shows_key_path;",
		"UIElement anchor = lead ? UIElement(w_.BittensorKeyBannerHost()) : UIElement(w_.SettlementV2Panel());",
		"stack.InsertAt(anchorIndex + 1, panel);")
	requireBittensorKey(t, "WalletPage::ApplyKeyBanner", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::ApplyKeyBanner("),
		"if (!keyCard_ || !keyCard_->backup_reminder) {", "btkeyviews::MakeBackupBanner(")
	requireBittensorKey(t, "WalletPage::RebuildConsentPanel", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::RebuildConsentPanel("),
		"PlaceConsentPanel();", "RebuildKeyCard(view);")
	// a consent the flow got accepted reads the consent and the earnings again
	requireBittensorKeyOrder(t, "WalletPage::ReadKeyFlow", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::ReadKeyFlow("),
		"if (keyCard_ && keyCard_->link_generation != keyLinkGeneration_) {", "LoadWallet();")

	sheets := bittensorKeySources(t)["BittensorKeySheets.cpp"]
	signOut := bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "winrt::fire_and_forget RequestSignOut(")
	requireBittensorKeyOrder(t, "btkeysheets::RequestSignOut", signOut,
		"if (!BittensorKeyNeedsBackup(networkId)) {", "if (signOut) signOut();", "co_return;",
		`dialog.Title(winrt::box_value(Loc("bittensor_key_sign_out_title_2")));`,
		`message.Text(Loc("bittensor_key_sign_out_message_2"));`,
		`dialog.PrimaryButtonText(Loc("bittensor_key_back_up_first"));`,
		`dialog.SecondaryButtonText(Loc("sign_out"));`,
		"dialog.DefaultButton(ContentDialogButton::Close);",
		"window->OpenEarningsKeyScreen();", "if (signOut) signOut();")
	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	requireBittensorKey(t, "SettingsPage::OnSignOut", bittensorKeyBody(t, "SettingsPage.cpp", settings, "void SettingsPage::OnSignOut("),
		"urnw::btkeysheets::RequestSignOut(w_.get_strong(), [] { Sdk().Logout(); });")
	requireBittensorKey(t, "SettingsPage::ShowDeleteAccountSheet", bittensorKeyBody(t, "SettingsPage.cpp", settings, "winrt::fire_and_forget SettingsPage::ShowDeleteAccountSheet("),
		"deleteSheet_->BackUpFirstChosen()", "if (backUpFirst) self->OpenEarningsKeyScreen();")
	login := stripComments(readAppSource(t, "LoginPage.cpp"))
	requireBittensorKey(t, "LoginPage::OnAccountMenu", bittensorKeyBody(t, "LoginPage.cpp", login, "void LoginPage::OnAccountMenu("),
		"urnw::btkeysheets::RequestSignOut(self, [] { Sdk().Logout(); });")
	if strings.Contains(bittensorKeyBody(t, "LoginPage.cpp", login, "void LoginPage::OnAccountMenu("), "actions.onSignOut = [] { Sdk().Logout(); };") {
		t.Error("LoginPage.cpp: the account menu signs out past the key's warning")
	}
	deleteSheet := stripComments(readAppSource(t, "SettingsSheets.cpp"))
	requireBittensorKey(t, "DeleteAccountSheet::Build", bittensorKeyBody(t, "SettingsSheets.cpp", deleteSheet, "void DeleteAccountSheet::Build("),
		"if (BittensorKeyNeedsBackup(networkId_)) {", `keyWarning.Text(Loc("bittensor_key_delete_account_message_2"));`,
		`dialog_.SecondaryButtonText(Loc("bittensor_key_back_up_first"));`, "self->backUpFirstChosen_ = true;")
	requireBittensorKeyOrder(t, "DeleteAccountSheet::Submit", bittensorKeyBody(t, "SettingsSheets.cpp", deleteSheet, "void DeleteAccountSheet::Submit("),
		"if (ok) {", "ForgetBittensorKey(networkId);", "sdk->Logout();")
	sessions := stripComments(readAppSource(t, "SessionsPage.cpp"))
	requireBittensorKeyOrder(t, "SessionsPage::ConfirmSignOut", bittensorKeyBody(t, "SessionsPage.cpp", sessions, "winrt::fire_and_forget SessionsPage::ConfirmSignOut("),
		"current = row.current;", "if (current) {", "if (BittensorKeyNeedsBackup(networkId)) {", `Loc("bittensor_key_sign_out_message")`)

	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	requireBittensorKey(t, "MainWindow::OpenEarningsKeyScreen", bittensorKeyBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::OpenEarningsKeyScreen("),
		"HomeNav().SelectedItem(WalletNavItem());", "wallet_->OpenKeyScreenWhenReady();")
	// the one reminder: the tray's notification, its click the Key screen, and
	// only one that went out is reported; a schedule is never reported, since the
	// app, not the system, waits for its moment
	app := stripComments(readAppSource(t, "AppController.cpp"))
	requireBittensorKey(t, "AppController", app,
		"if (tag == kBalloonBittensorKey) OpenEarningsKeyScreen();",
		`tray_.ShowBalloon(Localized("bittensor_key_reminder_title_2"),`,
		`Localized("bittensor_key_reminder_body_2"), kBalloonBittensorKey);`)
	tray := stripComments(readAppSource(t, "TrayIcon.cpp"))
	requireBittensorKey(t, "TrayIcon::WndProc", tray,
		"} else if (event == NIN_BALLOONUSERCLICK) {", "self->cb_.onBalloonClick(self->balloonTag_);")
	requireBittensorKeyOrder(t, "WalletPage::PostKeyReminder", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::PostKeyReminder("),
		"if (!PostBittensorKeyReminder()) return;", "keyFlow_->reminderDelivered();")
	if strings.Contains(page, "reminderScheduled(") {
		t.Error("WalletPage.cpp reports a schedule the system won't deliver")
	}
	requireBittensorKey(t, "WalletPage::ApplyKeyReminder", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::ApplyKeyReminder("),
		"reminder = keyFlow_->reminder(", "switch (btkey::ReminderStepFor(reminder->action)) {")
}

// Claims through URnetwork's relay: the tile and the claim sheet word what the
// sdk's claim status decides, Claim now goes through the sdk, and the device's
// own claim stays as the fallback. After the one tap, the wallet card shows the
// key on this device, and the points card doesn't ask for a coldkey the key
// provides.
func TestBittensorKeyClaimsThroughTheRelay(t *testing.T) {
	page := stripComments(readAppSource(t, "WalletPage.cpp"))
	requireBittensorKey(t, "WalletPage::LoadClaimStatus", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::LoadClaimStatus("),
		"Sdk().device().snClaimStatus(")
	requireBittensorKey(t, "WalletPage::OpenClaimRelaySheet", bittensorKeyBody(t, "WalletPage.cpp", page, "winrt::fire_and_forget WalletPage::OpenClaimRelaySheet("),
		"Sdk().device().snClaimNow(coldkey,", "if (answered) page.LoadClaimStatus();", "if (selfClaim) page.OnClaimV2(nullptr, nullptr);")
	relay := bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::ApplyRelayToV2Section(")
	requireBittensorKey(t, "WalletPage::ApplyRelayToV2Section", relay,
		"btkeyviews::DrawRelaySection(", "self->wallet().OpenClaimRelaySheet();", "self->wallet().OnClaimV2(nullptr, nullptr);")
	views := bittensorKeySources(t)["BittensorKeyViews.cpp"]
	section := bittensorKeyBody(t, "BittensorKeyViews.cpp", views, "void DrawRelaySection(")
	requireBittensorKey(t, "btkeyviews::DrawRelaySection", section,
		"const bool relayClaims = btkey::RelayClaims(status.reason);", `Loc("sn_claim_relay_for_you")`,
		`Loc("pending_data_key")`, "btkey::ReasonTextFor(", "btkey::ClaimNowNoteFor(",
		"status.claim_now_available, std::move(claimNow)", `Loc("sn_claim_relay_waits_for_relay")`,
		"if (status.self_claim_available)")
	sheets := bittensorKeySources(t)["BittensorKeySheets.cpp"]
	requireBittensorKey(t, "ClaimRelaySheet::StartClaimNow", bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "void ClaimRelaySheet::StartClaimNow("),
		"if (claiming_ || !status_ || !status_->claim_now_available || !claimNow_) return;",
		"self->outcome_ = Outcome::Requested;", "self->outcome_ = Outcome::Refused;")
	requireBittensorKey(t, "ClaimRelaySheet::ApplyState", bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "void ClaimRelaySheet::ApplyState("),
		`Loc("sn_claim_relay_requested")`, `Loc("sn_claim_relay_queued_button")`, "if (status.self_claim_available) {")
	// the sdk decides when a network with no wallet here shows the tile, and
	// when the claim is drawn as claimed, with its transaction
	requireBittensorKey(t, "WalletPage::RelayTileShows", bittensorKeyBody(t, "WalletPage.cpp", page, "bool WalletPage::RelayTileShows("),
		"return claimStatus_ && !claimStatus_->error && claimStatus_->shows_tile;")
	requireBittensorKey(t, "WalletPage::ApplyRelayToV2Section", relay, "claimStatus_->shows_claimed")
	requireBittensorKey(t, "btkeyviews::DrawRelaySection", section,
		"if (status.shows_claimed) add(MakeClaimedBanner(status));", "btkey::ClaimedNoteFor(nextAtMillis)")
	// the claimed state (4d): what URnetwork's claim paid, the coldkey it landed
	// on, its transaction, and when Claim now is available again
	requireBittensorKey(t, "btkeyviews::MakeClaimedBanner", bittensorKeyBody(t, "BittensorKeyViews.cpp", views, "FrameworkElement MakeClaimedBanner("),
		`Loc("claim_confirmed")`, "status.claim_amount_rao.value_or(0)", "status.coldkey_ss58.value_or(std::string())",
		`urnw::Format("sn_claim_relay_lands_on", ShortAddress(coldkey))`,
		"status.claim_tx_url.value_or(std::string())", `Loc("earnings_view_transaction")`)
	requireBittensorKey(t, "btkeyviews::OpenTransaction", views, `if (url.rfind("https://", 0) != 0) return;`)
	requireBittensorKeyOrder(t, "ClaimRelaySheet::ApplyState", bittensorKeyBody(t, "BittensorKeySheets.cpp", sheets, "void ClaimRelaySheet::ApplyState("),
		"if (status.shows_claimed) {", "btkey::ClaimedNoteFor(nextAtMillis)", "} else if (btkey::RelayClaims(status.reason)) {")
	// the wallet card and the points card after the one tap
	requireBittensorKey(t, "WalletPage::ApplyKeyWalletCard", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::ApplyKeyWalletCard("),
		"const bool key = keyCard_ && !keyCard_->stored_address.empty();",
		`Loc(linked ? "wallet_connected_to_protocol" : "bittensor_key_card_not_linked")`,
		"w_.ChangeWalletButton().Visibility(Visibility::Collapsed);")
	requireBittensorKey(t, "WalletPage::RebuildPayoutLine", bittensorKeyBody(t, "WalletPage.cpp", page, "void WalletPage::RebuildPayoutLine("),
		"const bool hasColdkey = snWallet_.has_value() || KeyPaysThisDevice();")
	requireBittensorKey(t, "WalletPage::KeyPaysThisDevice", bittensorKeyBody(t, "WalletPage.cpp", page, "bool WalletPage::KeyPaysThisDevice("),
		"return keyCard_ && keyCard_->pays_key_on_this_device;")
}
