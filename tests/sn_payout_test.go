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

// The SN payout line and settlement v2 (App/SnPayoutPresentation.h): the C++
// spec, the page's wiring and the strings.

// Builds the C++ spec (tools/sn-payout-tests.cpp) with `extra` arguments.
func buildSnPayoutTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("sn payout tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	program := filepath.Join(t.TempDir(), "sn-payout-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + appDir}
	arguments = append(arguments, extra...)
	arguments = append(arguments,
		filepath.Join(root, "app", "tools", "sn-payout-tests.cpp"),
		filepath.Join(appDir, "SnPayoutPresentation.cpp"),
		"-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build sn payout tests: %v\n%s", err, output)
	}
	return program
}

func runSnPayoutTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("sn payout: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Compile and execute the SN payout line spec (App/SnPayoutPresentation.h):
// provider payouts moved to the UR subnet, and Earnings says how and when a
// provider is paid under the points figure, with the current epoch's times
// from the SDK's epoch schedule, Claim while something is claimable and the
// coldkey prompt while none is set. The same program is settlement v2's spec
// (APPS-SPEC.md, the UX rules): the section for every state of the read,
// configured or not, pending as pending and unavailable never as a zero, and
// the v2 claim dialog's progress (TestSettlementV2Wiring reads the page that
// draws it).
func TestSnPayoutLine(t *testing.T) {
	root := repositoryRoot(t)
	appDir := filepath.Join(root, "app", "src", "App")
	runSnPayoutTests(t, buildSnPayoutTests(t))

	// the page draws the decision, with the schedule the claims read carries,
	// and its actions are the existing claim dialog and coldkey flow
	read := func(path string) string {
		source, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		return string(source)
	}
	walletPage := read(filepath.Join(appDir, "WalletPage.cpp"))
	for _, want := range []string{
		"snpayout::PayoutLineFor(",
		"result->schedule->claim_open_millis",
		`"sn_payout_schedule"`,
		`"sn_payout_schedule_times"`,
		`"set_coldkey_to_get_paid"`,
		`"set_coldkey"`,
	} {
		if !strings.Contains(walletPage, want) {
			t.Errorf("WalletPage.cpp: missing %s", want)
		}
	}
	mainWindow := read(filepath.Join(appDir, "MainWindow.xaml"))
	for _, want := range []*regexp.Regexp{
		regexp.MustCompile(`x:Name="SetColdkeyButton"[^>]*Click="OnConnectWallet"`),
		regexp.MustCompile(`x:Name="SnPayoutClaimButton"[^>]*Click="OnClaimAlpha"`),
	} {
		if !want.MatchString(mainWindow) {
			t.Errorf("MainWindow.xaml: missing %s", want)
		}
	}
	if project := read(filepath.Join(appDir, "App.vcxproj")); !strings.Contains(project, `Include="SnPayoutPresentation.cpp"`) {
		t.Error("App.vcxproj does not compile SnPayoutPresentation.cpp")
	}

	// the en resources: the line's strings, and no payout string that fixes the
	// claim window
	resw := read(filepath.Join(appDir, "Strings", "en", "Resources.resw"))
	value := func(key string) string {
		match := regexp.MustCompile(`<data name="` + key + `" xml:space="preserve">\s*<value>([^<]*)</value>`).FindStringSubmatch(resw)
		if match == nil {
			t.Errorf("Resources.resw: no %s", key)
			return ""
		}
		return match[1]
	}
	if got, want := value("sn_payout_schedule_times"), "This epoch ends {0}. Claim your share from {1} until {2}."; got != want {
		t.Errorf("sn_payout_schedule_times = %q, want %q", got, want)
	}
	duration := regexp.MustCompile(`\d+ (hours?|days?|weeks?|epochs?)`)
	for _, key := range []string{"sn_payout_schedule", "set_coldkey_to_get_paid", "set_coldkey", "claims_open_after_finalization"} {
		if v := value(key); duration.MatchString(v) {
			t.Errorf("%s fixes a duration: %q", key, v)
		}
	}
	if strings.Contains(resw, `name="payouts_amount_threshold"`) {
		t.Error("Resources.resw still carries the retired payouts_amount_threshold")
	}
}

// The v2 boundary templates against the generated SDK header: WalletPage
// instantiates snpayout::LedgerAccountFrom with urnet::SnLedgerAccount,
// snpayout::UnavailablePoolsFrom with its unavailable pools,
// snpayout::LedgerClaimCallbackFor with urnet::SnLedgerClaimCallback,
// snpayout::CtfWinsReadFrom with urnet::SnCtfClientResult (its unclaimed wins) and
// snpayout::RelayAccountsFrom with urnet::SnClaimStatusResult, and the claim
// sheet snpayout::RelayAccountsFrom too, so the spec builds each with the
// header's own types and reads the json the device answers with. The header
// is git-ignored (fetch-deps unpacks it into app/third_party/urnetwork-sdk/<arch>;
// URNETWORK_SDK_INCLUDE names another directory) and needs nlohmann/json, so a
// host without them skips this, and so does a copy from before the sdk's
// unclaimed wins (sdk-ctf-v2).
func TestSnPayoutAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := sdkHeaderDirWith(t, root, "unclaimed_wins")
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with capture the flag's unclaimed wins (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_SN_PAYOUT_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runSnPayoutTests(t, buildSnPayoutTests(t, extra...))
}

// Settlement v2 on Earnings (APPS-SPEC.md, the UX rules): the page draws the
// spec's decisions from the SDK's ledger accounts call, claims through the
// SDK's v2 claim with the dialog the spec moves along, reads the accounts again
// after Done, offers Claim only toward a configured vault2, sits right after
// the v1 tile, which keeps its own Claim, repeats its Claim in the Earnings
// actions menu while pane A is folded, under a label that names settlement v2,
// and says every new line in every language under the sn_ledger_* store keys,
// which the Android and Linux apps look up too (all but that menu label).
func TestSettlementV2Wiring(t *testing.T) {
	root := repositoryRoot(t)
	requireAll := func(file string, source string, wants ...string) {
		t.Helper()
		for _, want := range wants {
			if !strings.Contains(source, want) {
				t.Errorf("%s: missing %s", file, want)
			}
		}
	}
	walletPage := stripComments(readAppSource(t, "WalletPage.cpp"))
	requireAll("WalletPage.cpp", walletPage,
		// the balances: the SDK's ledger accounts call, copied at the boundary,
		// drawn from the spec's decision
		"Sdk().device().snLedgerAccounts(",
		"snpayout::LedgerAccountFrom(account)",
		"snpayout::LedgerSectionFor(snWallet_.has_value(), Vault2Configured(),",
		// an answer without its list is unavailable, as an error is: never a zero
		"else if (!result || !result->accounts)",
		// read beside the v1 claims once the coldkey is known
		"self->wallet().LoadV2Accounts();",
		// Claim only toward a configured vault2
		"!settings->vault2_address.empty()",
		"const bool allowActions = CanClaim() && Vault2Configured();",
		// the claim: the SDK's v2 claim, through the v2 dialog
		"Sdk().device().snLedgerClaim(",
		"snpayout::LedgerClaimCallbackFor<urnet::SnLedgerClaimCallback>(events)",
		"urnw::ClaimLedgerSheet::Create(",
		// every line of the section: the heading, each account's title (Pool {}
		// as before capture the flag: EarningsSheets.cpp PoolTitleText), labels
		// and pending line, and the note the spec chose (snpayout::LedgerNoteKey)
		`Loc("sn_ledger_title")`, "PoolTitleText(account.titleKey, account.noId)",
		`Loc("sn_ledger_claimable")`, `Loc("sn_ledger_paid")`, `Loc("sn_ledger_earned")`,
		`"sn_ledger_pending"`, "snpayout::LedgerNoteKey(view.note)",
	)
	// the claimable figure takes the v1 tile's earnings accent while there is some
	requireAll("WalletPage::RebuildV2Section", functionBody(walletPage, "void WalletPage::RebuildV2Section("),
		"if (account.claimableRao > 0) figure.Foreground(colors::AccentBrush());",
	)
	// After Done (the claim in flight's own first: snpayout::WithClaimPaid,
	// which the spec pins) the page reads the accounts and the gas balance
	// again, whether or not a dialog is still up.
	if !regexp.MustCompile(`events = snpayout::WithClaimPaid\(std::move\(events\), \[queue, weak, session\]\(bool paid\) \{\s*` +
		`queue\.TryEnqueue\(\[weak, paid, session\] \{\s*if \(auto self = weak\.get\(\)\) \{\s*` +
		`self->wallet\(\)\.LoadV2Accounts\(\);\s*self->wallet\(\)\.LoadGas\(\);`).MatchString(walletPage) {
		t.Error("WalletPage.cpp: the v2 claim does not read the accounts and the gas balance again after Done")
	}

	// The claim in flight is the page's, the signed-in session's, never the
	// dialog's (owner, 2026-11-12): the page starts it on the dialog's progress
	// (snpayout::LedgerClaimInFlight, which the spec pins), each event moves
	// that progress on the UI thread whether or not a dialog is up, an open
	// dialog draws it again, and Done ends the claim in flight.
	startV2 := functionBody(walletPage, "bool WalletPage::StartV2Claim(")
	requireAll("WalletPage::StartV2Claim", startV2,
		"if (v1Claim_.InFlight()) return false;",
		"const std::optional<uint64_t> started = v2Claim_.Start(progress);",
		"if (!started) return false;",
		"const std::weak_ptr<snpayout::LedgerClaimProgress> claim = progress;",
		"rows.Sent(noId, index, txHash);",
		"rows.Confirmed(noId, index, txHash, amountRao);",
		"rows.Failed(noId, index, message);",
		"if (auto self = weak.get()) self->wallet().OnV2ClaimDone(claim, ticket);",
		"Sdk().device().snLedgerClaim(",
	)
	if strings.Count(startV2, "self->wallet().OnV2ClaimEvent(") != 3 {
		t.Error("WalletPage::StartV2Claim: Sent, Confirmed and Failed do not each go through OnV2ClaimEvent")
	}
	requireAll("WalletPage::OnV2ClaimEvent", functionBody(walletPage, "void WalletPage::OnV2ClaimEvent("),
		"const auto progress = claim.lock();",
		"if (!progress) return;",
		"step(*progress);",
		"if (v2ClaimSheet_) v2ClaimSheet_->Redraw();",
	)
	requireAll("WalletPage::OnV2ClaimDone", functionBody(walletPage, "void WalletPage::OnV2ClaimDone("),
		"if (const auto progress = claim.lock()) progress->Done();",
		"if (v2Claim_.End(ticket)) {",
		"RebuildV2Section();",
		"if (v2ClaimSheet_) v2ClaimSheet_->Redraw();",
		// a v1 dialog up waited for it
		"if (claimSheet_) claimSheet_->Redraw();",
	)
	requireAll("WalletPage::MakeV2Claimer", functionBody(walletPage, "LedgerClaimer WalletPage::MakeV2Claimer("),
		"return self && self->wallet().StartV2Claim(std::move(progress));",
	)

	// The v2 dialog's own functions (the v1 dialog beside it has its own).
	sheets := stripComments(readAppSource(t, "EarningsSheets.cpp"))
	sheet := func(signature string) string {
		t.Helper()
		body := functionBody(sheets, signature)
		if body == "" {
			t.Fatalf("EarningsSheets.cpp: no %s", signature)
		}
		return body
	}
	// the dialog's Claim asks the page for the claim on its progress, held
	// while one is out, and applies none of the claim's events itself: a
	// dismissed dialog would drop them
	startClaim := sheet("void ClaimLedgerSheet::StartClaim(")
	requireAll("ClaimLedgerSheet::StartClaim", startClaim,
		"if (progress_->sending() || !claimer_) return;",
		"if (!claimer_(progress_)) return;",
	)
	for _, own := range []string{"weak_from_this()", "progress_->Sent(", "progress_->Confirmed(", "progress_->Failed(",
		"progress_->Done(", "progress_->Start("} {
		if strings.Contains(startClaim, own) {
			t.Errorf("ClaimLedgerSheet::StartClaim: %s: the dialog moves the claim itself, and drops its events once dismissed", own)
		}
	}
	// needs_gas opens the v1 dialog's funding hint, asking for a v2 claim's gas,
	// and the dialog clamps to its window as the v1 dialog does (SheetFit.h)
	requireAll("ClaimLedgerSheet::Build", sheet("void ClaimLedgerSheet::Build("),
		`urnw::Format("send_tao_to_mirror", FormatTao(snpayout::kLedgerClaimGasTao),`,
		"content.MinWidth(sheetfit::Width(root, 460));",
		"scroll.MaxHeight(sheetfit::Height(root, 560));",
	)
	requireAll("ClaimLedgerSheet::ApplyState", sheet("void ClaimLedgerSheet::ApplyState("),
		"needsGasPanel_.Visibility(progress_->needsGas() ? Visibility::Visible : Visibility::Collapsed);",
		// and waits for a v1 claim in flight: the gas key sends one at a time
		"claimButton_.IsEnabled(progress_->CanClaim(allowActions_) && !Held());",
		"LedgerFailureText(progress_->failure())",
	)
	// Sent links the explorer; a failure says the store's sentence or the
	// message as it came
	requireAll("ClaimLedgerSheet::BuildRow", sheet("UIElement ClaimLedgerSheet::BuildRow("),
		"PoolTitleText(progress_->TitleKey(row), row.noId)",
		"snpayout::ExplorerTxLink(explorerTxUrl_, row.txHash)",
		`link.Content(LocBox("earnings_view_transaction"));`,
		"LedgerFailureText(row.message)",
	)
	requireAll("LedgerFailureText", sheet("hstring LedgerFailureText("),
		"snpayout::ClaimFailureKey(message)",
	)

	mainWindow := readAppSource(t, "MainWindow.xaml")
	if !regexp.MustCompile(`x:Name="SettlementV2ClaimButton"[^>]*Click="OnClaimV2"`).MatchString(mainWindow) {
		t.Error("MainWindow.xaml: SettlementV2ClaimButton does not open the v2 claim dialog")
	}
	if !regexp.MustCompile(`x:Name="ClaimButton"[^>]*Click="OnClaimAlpha"`).MatchString(mainWindow) {
		t.Error("MainWindow.xaml: the v1 tile lost its Claim")
	}
	unclaimed := strings.Index(mainWindow, `x:Name="UnclaimedPanel"`)
	v2 := strings.Index(mainWindow, `x:Name="SettlementV2Panel"`)
	top200 := strings.Index(mainWindow, `x:Name="Top200Panel"`)
	if unclaimed < 0 || v2 < unclaimed || top200 < v2 {
		t.Error("MainWindow.xaml: the settlement v2 section is not right after the v1 unclaimed tile")
	}
	requireAll("MainWindow.xaml.cpp", readAppSource(t, "MainWindow.xaml.cpp"), "wallet_->OnClaimV2(s, e);")

	// Below 900dip pane A folds and the ledger header's "Earnings actions" menu
	// is its door: an item there opens the v2 claim dialog, shown while the
	// section shows the button (hiding the section leaves the button as it was)
	// and enabled while the button is. The button says Claim under the section's
	// title; the item says which claim it is (sn_ledger_claim), since the v1
	// tile's Claim sits right above it.
	requireAll("WalletPage.cpp", walletPage, `w_.SettlementV2ClaimButton().Content(LocBox("claim"));`)
	menu := functionBody(walletPage, "void WalletPage::ShowEarningsActionsMenu(")
	if !regexp.MustCompile(`claimV2\.Click\(\[weak\]\(IInspectable const& s, RoutedEventArgs const& e\) \{\s*` +
		`if \(auto self = weak\.get\(\)\) self->wallet\(\)\.OnClaimV2\(s, e\);\s*\}\);\s*` +
		`flyout\.Items\(\)\.Append\(claimV2\);`).MatchString(menu) {
		t.Error("WalletPage.cpp: the Earnings actions menu has no item that opens the v2 claim dialog")
	}
	if !regexp.MustCompile(`const bool v2ClaimShown =\s*` +
		`w_\.SettlementV2Panel\(\)\.Visibility\(\) == Visibility::Visible &&\s*` +
		`w_\.SettlementV2ClaimButton\(\)\.Visibility\(\) == Visibility::Visible;\s*` +
		`claimV2\.Visibility\(v2ClaimShown \? Visibility::Visible : Visibility::Collapsed\);`).MatchString(menu) {
		t.Error("WalletPage.cpp: the menu's v2 Claim does not show exactly while the section shows the button")
	}
	requireAll("WalletPage::ShowEarningsActionsMenu", menu,
		"claimV2.IsEnabled(w_.SettlementV2ClaimButton().IsEnabled());",
		`claimV2.Text(Loc("sn_ledger_claim"));`,
	)

	// the en strings, the store's for every app, and every catalog says each
	// line, with its placeholder
	english := map[string]string{
		"sn_ledger_title":       "Settlement v2",
		"sn_ledger_claimable":   "Claimable",
		"sn_ledger_paid":        "Paid",
		"sn_ledger_earned":      "Earned",
		"sn_ledger_pending":     "Pending: +{}, awaiting finalization",
		"sn_ledger_pool":        "Pool {}",
		"sn_ledger_unavailable": "Unavailable",
		"sn_ledger_claim":       "Claim settlement v2",
	}
	catalogs, err := filepath.Glob(filepath.Join(root, "app", "src", "App", "Strings", "*", "Resources.resw"))
	if err != nil || len(catalogs) < 28 {
		t.Fatalf("catalogs: %v %v", catalogs, err)
	}
	for key, want := range english {
		if got := reswValue(t, root, "en", key); got != want {
			t.Errorf("%s = %q, want %q", key, got, want)
		}
		for _, catalog := range catalogs {
			locale := filepath.Base(filepath.Dir(catalog))
			if got := reswValue(t, root, locale, key); got == "" ||
				strings.Count(got, "{}") != strings.Count(want, "{}") {
				t.Errorf("%s: %s = %q", locale, key, got)
			}
		}
	}
}

// Capture the flag in the claim flow (sn CAPTURETHEFLAG.md §10): the ledger
// lists the coldkey's capture the flag account beside its provider pool
// account, and the SDK claims both with one SnLedgerClaim, in one batch
// transaction. Earnings titles each account by its pool, shows a pool the
// ledger could not read as unavailable beside the others, says Claim both
// while the one claim claims exactly both, marks any account under the
// relay's transfer floor, lone or not, adds the claim sheet's row per account
// and the no-wallet tile's rows, with no total when nothing was read, and
// says a line for each slot won and not claimed yet from GET /sn/ctf/client,
// read again at a claim's Done and, after a claim that paid an account, 30 s,
// 60 s and 120 s after it until a sign-out, all in the store's sn_ctf_* and
// sn_ledger_* words. The decisions are the spec's (TestSnPayoutLine); this
// reads the page and the sheets that draw them.
func TestCaptureTheFlagClaimFlow(t *testing.T) {
	root := repositoryRoot(t)
	requireAll := func(file string, source string, wants ...string) {
		t.Helper()
		for _, want := range wants {
			if !strings.Contains(source, want) {
				t.Errorf("%s: missing %s", file, want)
			}
		}
	}
	body := func(file, source, signature string) string {
		t.Helper()
		found := functionBody(source, signature)
		if found == "" {
			t.Fatalf("%s: no %s", file, signature)
		}
		return found
	}

	page := stripComments(readAppSource(t, "WalletPage.cpp"))
	// the read: the pools the ledger could not read come with the accounts
	requireAll("WalletPage::LoadV2Accounts", body("WalletPage.cpp", page, "void WalletPage::LoadV2Accounts("),
		"unavailable = snpayout::UnavailablePoolsFrom(result->unavailable_pools);",
		"ok ? snpayout::LedgerFetch::Ready : snpayout::LedgerFetch::Failed, accounts, unavailable);")
	requireAll("WalletPage::ApplyV2Accounts", body("WalletPage.cpp", page, "void WalletPage::ApplyV2Accounts("),
		"v2Unavailable_ = unavailable;")
	// the tile: each block's title, an unavailable pool's block, the floor's
	// status, the won slots with capture the flag's account, and Claim both
	section := body("WalletPage.cpp", page, "void WalletPage::RebuildV2Section(")
	requireAll("WalletPage::RebuildV2Section", section,
		// a claim in flight keeps Claim, which opens its dialog; between
		// claims it waits for a read that started after the last v2 Done
		"CanClaim(), v2Unavailable_, TransferFloorRao(), v2Claim_.InFlight(),",
		"v2Reads_.Fresh());",
		"if (!account.titleKey.empty()) {",
		"MakeText(PoolTitleText(account.titleKey, account.noId), 13, colors::TextBrush())",
		"if (account.unavailable) {",
		`MakeText(Loc("sn_ledger_pool_unavailable"), 13, colors::MutedBrush(), true)`,
		"if (account.underFloor) {",
		`MakeText(Loc("sn_ledger_pool_under_floor"), 13, colors::MutedBrush(), true)`,
		// a line for every win not claimed yet, under capture the flag's
		// block, else after the blocks
		"for (auto const& win : ctfWins_.lines()) {",
		"host.Children().Append(MakeText(CtfWinText(win), 13, colors::TextBrush(), true));",
		"if (!winShown && account.kind == snpayout::PoolKind::Ctf) {",
		"appendWins(block);",
		"if (!winShown) appendWins(panel);",
		`w_.SettlementV2ClaimButton().Content(LocBox(view.claimBoth ? "sn_ledger_claim_both" : "claim"));`,
		// a network with no wallet here: the relay's sum of the primary
		// coldkey's accounts with no figure when none was read, Below the claim
		// minimum under a lone account's, its rows without the Pending pill,
		// and the won slots
		"const snpayout::RelayTileView tile = snpayout::RelayTileFor(",
		"snpayout::RelayAccountsFrom(*claimStatus_), claimStatus_->coldkey_ss58.value_or(std::string()),",
		"snpayout::UnavailablePoolsFrom(claimStatus_->unavailable_pools), claimStatus_->transfer_floor_rao);",
		"if (tile.showTotal) {",
		"if (tile.underFloor) {",
		"btkeyviews::MakeRelayRows(tile.rows, std::string(),",
		"appendWins(relayPanel);")
	// the relay tile's figure only with a total, and the floor's line after it
	requireOrder := func(where, source string, steps ...string) {
		t.Helper()
		at := 0
		for _, step := range steps {
			found := strings.Index(source[at:], step)
			if found < 0 {
				t.Errorf("%s: missing %s, or out of order", where, step)
				return
			}
			at += found + len(step)
		}
	}
	requireOrder("WalletPage::RebuildV2Section", section,
		"if (tile.showTotal) {", "MakeValue(hstring{FormatAlphaRao(claimStatus_->claimable_rao)}, 32);",
		"if (tile.underFloor) {", `Loc("sn_ledger_pool_under_floor")`)
	// an unavailable pool's block draws no figure: the figures are the else
	// branch of the unavailable line
	if at := strings.Index(section, "if (account.unavailable) {"); at >= 0 {
		rest := section[at:]
		elseBranch := strings.Index(rest, "} else {")
		figure := strings.Index(rest, `MakeValue(hstring{FormatAlphaRao(account.claimableRao)}, 32)`)
		if elseBranch < 0 || figure < elseBranch {
			t.Error("WalletPage::RebuildV2Section: an unavailable pool's block is not drawn without its figures")
		}
	}
	requireAll("WalletPage::TransferFloorRao", body("WalletPage.cpp", page, "int64_t WalletPage::TransferFloorRao("),
		"return claimStatus_ && !claimStatus_->error ? claimStatus_->transfer_floor_rao : 0;")
	// the claim dialog: the claim in flight's progress while there is one, else
	// one over the read with its unavailable pools and the floor; its one Claim
	// is the SDK's SnLedgerClaim, which batches
	openV2 := body("WalletPage.cpp", page, "winrt::fire_and_forget WalletPage::OnClaimV2(")
	requireAll("WalletPage::OnClaimV2", openV2,
		"std::shared_ptr<snpayout::LedgerClaimProgress> progress = v2Claim_.Progress();",
		"if (!progress) {",
		"progress = std::make_shared<snpayout::LedgerClaimProgress>(v2Accounts_, gas.tao, v2Unavailable_,",
		"TransferFloorRao());",
		"urnw::ClaimLedgerSheet::Create(self->Content().XamlRoot(), std::move(progress), gas,",
		"ExplorerTxUrl(), allowActions, MakeV2Claimer(), v1InFlight);",
		"return window && window->wallet().v1Claim_.InFlight();")
	requireOrder("WalletPage::OnClaimV2", openV2,
		"= v2Claim_.Progress();", "if (!progress) {", "std::make_shared<snpayout::LedgerClaimProgress>(",
		"urnw::ClaimLedgerSheet::Create(")
	claimer := body("WalletPage.cpp", page, "bool WalletPage::StartV2Claim(")
	requireAll("WalletPage::StartV2Claim", claimer,
		"Sdk().device().snLedgerClaim(",
		// a win the claim paid loses its line: the wins are read again at Done,
		// and after a claim that paid an account on the apps' schedule (the
		// server reads a claim only once the finalized head covers it)
		// the session the claim starts in: a Done after a sign-out starts no
		// reads for the next account
		"const uint32_t session = ctfWinsAfterClaim_.Session();",
		"events = snpayout::WithClaimPaid(std::move(events), [queue, weak, session](bool paid) {",
		"queue.TryEnqueue([weak, paid, session] {",
		"self->wallet().LoadCtfClient();",
		"self->wallet().ReadCtfWinsAfterClaim(paid, session);")
	requireOrder("WalletPage::StartV2Claim", claimer,
		"if (v1Claim_.InFlight()) return false;", "v2Claim_.Start(", "ctfWinsAfterClaim_.Session();", "snpayout::WithClaimPaid(", "self->wallet().LoadCtfClient();",
		"self->wallet().ReadCtfWinsAfterClaim(paid, session);",
		"snpayout::LedgerClaimCallbackFor<urnet::SnLedgerClaimCallback>(events)")
	requireAll("WalletPage::ReadCtfWinsAfterClaim", body("WalletPage.cpp", page, "void WalletPage::ReadCtfWinsAfterClaim("),
		"if (const auto wait = ctfWinsAfterClaim_.Done(paid, session)) ArmCtfWinsTimer(*wait);")
	// a timer for each wait, whose tick hands the wait's token back: a tick
	// already on its way from a timer since replaced reads nothing
	arm := body("WalletPage.cpp", page, "void WalletPage::ArmCtfWinsTimer(")
	requireAll("WalletPage::ArmCtfWinsTimer", arm,
		"if (ctfWinsTimer_) ctfWinsTimer_.Stop();",
		"ctfWinsTimer_ = w_.DispatcherQueue().CreateTimer();", "ctfWinsTimer_.IsRepeating(false);",
		"ctfWinsTimer_.Interval(wait.wait);",
		"ctfWinsTimer_.Tick([weak = w_.get_weak(), alive = alive_, token = wait.token](auto const&, auto const&) {",
		"if (!*alive) return;", "if (auto self = weak.get()) self->wallet().OnCtfWinsTimer(token);",
		"ctfWinsTimer_.Start();")
	requireOrder("WalletPage::ArmCtfWinsTimer", arm,
		"ctfWinsTimer_.Stop();", "CreateTimer();", "ctfWinsTimer_.Tick(", "ctfWinsTimer_.Start();")
	requireAll("WalletPage::OnCtfWinsTimer", body("WalletPage.cpp", page, "void WalletPage::OnCtfWinsTimer("),
		"const snpayout::CtfWinsReadsAfterClaim::Step step = ctfWinsAfterClaim_.Tick(token);",
		"if (!step.read) return;", "LoadCtfClient();", "if (step.next) ArmCtfWinsTimer(*step.next);")
	requireAll("WalletPage::~WalletPage", body("WalletPage.cpp", page, "WalletPage::~WalletPage() {"),
		"if (ctfWinsTimer_) ctfWinsTimer_.Stop();")
	// the won slots: every unclaimed win, read with the Earnings reads, dropped
	// at sign-out, with the reads after that account's claim; a read that failed
	// keeps the last good read's lines, the lines show the newest read's good
	// answer whatever order the answers land in, and a client answer never
	// replaces a network answer's lines (snpayout::CtfWinsLines)
	loadWins := body("WalletPage.cpp", page, "void WalletPage::LoadCtfClient(")
	requireAll("WalletPage::LoadCtfClient", loadWins,
		"const uint32_t read = ctfWins_.Start();",
		"Sdk().device().snCtfClient(", "std::optional<std::vector<snpayout::CtfWin>> wins;",
		"wins = snpayout::CtfWinsReadFrom(*result);",
		"answer = snpayout::CtfAnswerOf(*result);",
		"self->wallet().ApplyCtfClient(read, wins, answer);")
	if strings.Contains(loadWins, "CtfWinsFrom(") {
		t.Error("WalletPage::LoadCtfClient: reads CtfWinsFrom, which makes a failed read no lines")
	}
	requireAll("WalletPage::ApplyCtfClient", body("WalletPage.cpp", page, "void WalletPage::ApplyCtfClient("),
		"if (!ctfWins_.Apply(read, std::move(wins), answer)) return;", "RebuildV2Section();")
	requireAll("WalletPage::LoadWallet", body("WalletPage.cpp", page, "void WalletPage::LoadWallet("), "LoadCtfClient();")
	// a sign-out also drops the claim in flight, so the next account's Claim is
	// its own
	requireAll("WalletPage::ResetForSignOut", body("WalletPage.cpp", page, "void WalletPage::ResetForSignOut("),
		"ctfWins_.Reset();", "ctfWinsAfterClaim_.Stop();", "if (ctfWinsTimer_) ctfWinsTimer_.Stop();",
		"v2Claim_.Drop();")
	header := stripComments(readAppSource(t, "WalletPage.h"))
	requireAll("WalletPage.h", header,
		"void ReadCtfWinsAfterClaim(bool paid, uint32_t session);",
		"void ArmCtfWinsTimer(snpayout::CtfWinsReadsAfterClaim::Wait wait);",
		"void OnCtfWinsTimer(uint32_t token);", "snpayout::CtfWinsReadsAfterClaim ctfWinsAfterClaim_;",
		"winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer ctfWinsTimer_{nullptr};",
		"snpayout::CtfWinsLines ctfWins_;",
		"snpayout::LedgerClaimInFlight v2Claim_;",
		"bool StartV2Claim(std::shared_ptr<snpayout::LedgerClaimProgress> progress);")
	// the lines are the newest read's good answer: no generation drops an older
	// read's good answer when a newer read fails
	if strings.Contains(page, "ctfClientGeneration_") || strings.Contains(header, "ctfClientGeneration_") {
		t.Error("WalletPage: the wins still have a read generation, which drops an older good answer")
	}
	// the page draws the unclaimed wins, never the latest win alone
	for _, gone := range []string{"latest_win", "CtfWinFrom(", "ctfWin_"} {
		if strings.Contains(page, gone) {
			t.Errorf("WalletPage.cpp: still reads %s", gone)
		}
	}

	sheets := stripComments(readAppSource(t, "EarningsSheets.cpp"))
	// the titles and the won line in the store's words
	requireAll("PoolTitleText", body("EarningsSheets.cpp", sheets, "hstring PoolTitleText("),
		`if (titleKey == "sn_ledger_pool") return hstring{urnw::Format("sn_ledger_pool", noId)};`,
		"return Loc(titleKey);")
	requireAll("CtfWinText", body("EarningsSheets.cpp", sheets, "hstring CtfWinText("),
		"RegionDisplayName(win.country)", "urnw::Format(snpayout::CtfWinKey(win), country)")
	// the claim dialog: the progress it is given (the claim in flight's, or one
	// over the read's unavailable pools and the floor: WalletPage::OnClaimV2),
	// each row's title, its unavailable and under-the-floor words, and Claim both
	requireAll("ClaimLedgerSheet::ClaimLedgerSheet", sheets, ": progress_(std::move(progress)),")
	requireAll("ClaimLedgerSheet::BuildRow", body("EarningsSheets.cpp", sheets, "UIElement ClaimLedgerSheet::BuildRow("),
		"PoolTitleText(progress_->TitleKey(row), row.noId)",
		// no figure for an unavailable pool or an amount not known yet: never a zero
		"const bool known = row.amountRao > 0 && !row.unavailable;",
		"known ? hstring{FormatAlphaRao(row.amountRao)} : hstring{}",
		`word = Loc("sn_ledger_pool_unavailable");`, `word = Loc("sn_ledger_pool_under_floor");`)
	requireAll("ClaimLedgerSheet::ApplyState", body("EarningsSheets.cpp", sheets, "void ClaimLedgerSheet::ApplyState("),
		// the figure: during a claim what the claim moves, between claims what
		// Claim moves (snpayout::LedgerClaimProgress::FigureRao)
		"const int64_t total = progress_->FigureRao();",
		// with only pools it could not read, no total
		"totalValue_.Text(progress_->TotalKnown() ? hstring{FormatAlphaRao(total)} : hstring{});",
		// nothing Claim moves and no claim out: only Close, never a Claim of 0
		"claimButton_.Visibility(0 < total || progress_->sending() ? Visibility::Visible : Visibility::Collapsed);",
		"progress_->ClaimsBoth()", `? Loc("sn_ledger_claim_both")`,
		// and with nothing it moves no amount at all: never a Claim of 0
		`: 0 < total ? hstring{urnw::Format("claim_amount_button", FormatAlphaRao(total))} : Loc("claim")`)

	// the claim sheet: a row per account under its figure, which shows only
	// with a total
	relaySheet := stripComments(readAppSource(t, "BittensorKeySheets.cpp"))
	relayBuild := body("BittensorKeySheets.cpp", relaySheet, "void ClaimRelaySheet::Build(")
	requireAll("ClaimRelaySheet::Build", relayBuild,
		"if (auto rows = btkeyviews::MakeRelayPoolRows(*status_)) content.Children().Append(rows);",
		"if (snpayout::RelayTotalKnown(snpayout::RelayAccountsFrom(*status_), coldkey,",
		"snpayout::UnavailablePoolsFrom(status_->unavailable_pools))) {")
	requireOrder("ClaimRelaySheet::Build", relayBuild,
		"if (snpayout::RelayTotalKnown(", `figure.Children().Append(MakeText(Loc("sn_ledger_claimable"), 12, colors::MutedBrush()));`,
		"figure.Children().Append(MakeText(hstring{FormatAlphaRao(status_->claimable_rao)}, 28, colors::TextBrush()));",
		"content.Children().Append(figure);", "}",
		`urnw::Format("sn_claim_relay_lands_on", ShortAddress(coldkey))`)
	views := stripComments(readAppSource(t, "BittensorKeyViews.cpp"))
	requireAll("btkeyviews::MakeRelayPoolRows", body("BittensorKeyViews.cpp", views, "FrameworkElement MakeRelayPoolRows("),
		"snpayout::RelayRowsFor(snpayout::RelayAccountsFrom(status), snpayout::UnavailablePoolsFrom(status.unavailable_pools),",
		"status.transfer_floor_rao);",
		"return MakeRelayRows(rows, bannerUrl,")
	rowsBody := body("BittensorKeyViews.cpp", views, "FrameworkElement MakeRelayRows(")
	requireAll("btkeyviews::MakeRelayRows", rowsBody,
		"if (rows.empty()) return nullptr;",
		"PoolTitleText(row.titleKey, row.noId)",
		`word = Loc("pending_data_key");`, `word = Loc("sn_ledger_pool_under_floor");`,
		`word = Loc("claim_confirmed");`, `word = Loc("sn_ledger_pool_unavailable");`,
		// the tile leaves Pending to the relay's reason; every other status has its pill
		"if (pendingPill || row.status != snpayout::RelayRowStatus::Pending) {",
		"row.txUrl != bannerUrl")

	// the store's words, in every catalog with their placeholders
	english := map[string]string{
		"sn_ctf_pool":                "Capture the flag",
		"sn_ledger_provider_pool":    "Provider pool",
		"sn_ledger_pool_under_floor": "Below the claim minimum",
		"sn_ledger_pool_unavailable": "Unavailable right now",
		"sn_ledger_claim_both":       "Claim both",
		"sn_ctf_won_provider_slot":   "You won one of the {} provider slots",
		"sn_ctf_won_extender_slot":   "You won one of the {} extender slots",
	}
	catalogs, err := filepath.Glob(filepath.Join(root, "app", "src", "App", "Strings", "*", "Resources.resw"))
	if err != nil || len(catalogs) < 28 {
		t.Fatalf("catalogs: %v %v", catalogs, err)
	}
	for key, want := range english {
		if got := reswValue(t, root, "en", key); got != want {
			t.Errorf("%s = %q, want %q", key, got, want)
		}
		for _, catalog := range catalogs {
			locale := filepath.Base(filepath.Dir(catalog))
			if got := reswValue(t, root, locale, key); got == "" ||
				strings.Count(got, "{}") != strings.Count(want, "{}") {
				t.Errorf("%s: %s = %q", locale, key, got)
			}
		}
	}
}

// The v1 claim is the signed-in session's, not its dialog's, as the v2 claim
// is (owner, 2026-11-12): the page holds it (snpayout::ClaimInFlight of
// ClaimAlphaProgress, whose holder the spec pins), each event moves its
// progress on the UI thread whether or not the dialog is up, the dialog opened
// during the claim draws it with Claim held, the tile's Claim stays enabled to
// open it, the page reads the claims again after its Done, and a sign-out
// drops it.
func TestV1ClaimIsTheSessions(t *testing.T) {
	requireAll := func(file string, source string, wants ...string) {
		t.Helper()
		for _, want := range wants {
			if !strings.Contains(source, want) {
				t.Errorf("%s: missing %s", file, want)
			}
		}
	}
	body := func(file, source, signature string) string {
		t.Helper()
		found := functionBody(source, signature)
		if found == "" {
			t.Fatalf("%s: no %s", file, signature)
		}
		return found
	}
	header := stripComments(readAppSource(t, "WalletPage.h"))
	requireAll("WalletPage.h", header,
		"snpayout::ClaimInFlight<ClaimAlphaProgress> v1Claim_;",
		"bool StartV1Claim(std::shared_ptr<ClaimAlphaProgress> progress);")
	page := stripComments(readAppSource(t, "WalletPage.cpp"))
	requireAll("WalletPage::MakeClaimer", body("WalletPage.cpp", page, "Claimer WalletPage::MakeClaimer("),
		"return self && self->wallet().StartV1Claim(std::move(progress));")
	start := body("WalletPage.cpp", page, "bool WalletPage::StartV1Claim(")
	requireAll("WalletPage::StartV1Claim", start,
		// one claim at a time: none while a v2 claim is in flight
		"if (v2Claim_.InFlight()) return false;",
		"const std::optional<uint64_t> started = v1Claim_.Start(progress);",
		"if (!started) return false;",
		"const std::weak_ptr<ClaimAlphaProgress> claim = progress;",
		"rows.Sent(epoch, txHash);", "rows.Confirmed(epoch, txHash, amountRao);", "rows.Failed(epoch, message);",
		"if (auto self = weak.get()) self->wallet().OnV1ClaimDone(claim, ticket);",
		"Sdk().device().snClaim(epochs, callback);")
	if strings.Count(start, "self->wallet().OnV1ClaimEvent(") != 3 {
		t.Error("WalletPage::StartV1Claim: Sent, Confirmed and Failed do not each go through OnV1ClaimEvent")
	}
	requireAll("WalletPage::OnV1ClaimEvent", body("WalletPage.cpp", page, "void WalletPage::OnV1ClaimEvent("),
		"const auto progress = claim.lock();", "if (!progress) return;", "step(*progress);",
		"if (claimSheet_) claimSheet_->Redraw();")
	requireAll("WalletPage::OnV1ClaimDone", body("WalletPage.cpp", page, "void WalletPage::OnV1ClaimDone("),
		"if (const auto progress = claim.lock()) progress->Done();",
		"if (v1Claim_.End(ticket)) {", "LoadClaims();", "LoadGas();",
		// a v2 dialog up waited for it
		"if (v2ClaimSheet_) v2ClaimSheet_->Redraw();")
	open := body("WalletPage.cpp", page, "winrt::fire_and_forget WalletPage::OnClaimAlpha(")
	requireAll("WalletPage::OnClaimAlpha", open,
		"std::shared_ptr<ClaimAlphaProgress> progress = v1Claim_.Progress();",
		"progress = std::make_shared<ClaimAlphaProgress>(claims_, gas.tao);",
		"urnw::ClaimAlphaSheet::Create(self->Content().XamlRoot(), std::move(progress), gas,",
		"ExplorerTxUrl(), allowActions, MakeClaimer(), v2InFlight);",
		"return window && window->wallet().v2Claim_.InFlight();")
	// the tile's Claim: a claim in flight keeps it, to open the claim
	// (snpayout::V1ClaimEnabled, which the spec pins), drawn from both of the
	// claims read's answers
	claims := body("WalletPage.cpp", page, "void WalletPage::ApplyClaims(")
	if strings.Count(claims, "RebuildClaimButton();") != 2 {
		t.Error("WalletPage::ApplyClaims: the failed and the good answer do not each draw the tile's Claim")
	}
	requireAll("WalletPage::RebuildClaimButton", body("WalletPage.cpp", page, "void WalletPage::RebuildClaimButton("),
		"snpayout::V1ClaimEnabled(v1Reads_.Fresh(), totalClaimableRao_, CanClaim(), v1Claim_.InFlight()) ||",
		"previewSample);")
	requireAll("WalletPage::ResetForSignOut", body("WalletPage.cpp", page, "void WalletPage::ResetForSignOut("),
		"v1Claim_.Drop();")

	sheets := stripComments(readAppSource(t, "EarningsSheets.cpp"))
	startClaim := body("EarningsSheets.cpp", sheets, "void ClaimAlphaSheet::StartClaim(")
	requireAll("ClaimAlphaSheet::StartClaim", startClaim,
		"if (progress_->sending() || !claimer_) return;", "if (!claimer_(progress_)) return;")
	for _, own := range []string{"weak_from_this()", "TryEnqueue(", "progress_->Sent(", "progress_->Confirmed(",
		"progress_->Failed(", "progress_->Done(", "progress_->Start("} {
		if strings.Contains(startClaim, own) {
			t.Errorf("ClaimAlphaSheet::StartClaim: %s: the dialog moves the claim itself, and drops its events once dismissed", own)
		}
	}
	requireAll("ClaimAlphaSheet::ApplyState", body("EarningsSheets.cpp", sheets, "void ClaimAlphaSheet::ApplyState("),
		"for (auto const& row : progress_->rows()) rowsPanel_.Children().Append(BuildRow(row));",
		// the figure: during a claim what it moves, fixed at the press
		"const int64_t total = progress_->FigureRao();",
		"totalValue_.Text(hstring{FormatAlphaRao(total)});",
		// held by its own claim and by a v2 claim in flight
		"claimButton_.IsEnabled(allowActions_ && total > 0 && !progress_->needsGas() && !progress_->sending() &&",
		"!Held());")
	// the progress is the pure layer's, which the spec builds and runs ("v1
	// claim: the dialog's figure and count are the press's until Done")
	presentation := stripComments(readAppSource(t, "SnPayoutPresentation.cpp"))
	requireAll("ClaimAlphaProgress::FigureRao", body("SnPayoutPresentation.cpp", presentation, "int64_t ClaimAlphaProgress::FigureRao() const"),
		"return sending_ ? pressRao_ : TotalClaimableRao();")
	// the events' rows, moved from the dialog's handlers unchanged
	requireAll("ClaimAlphaProgress::Failed", body("SnPayoutPresentation.cpp", presentation, "void ClaimAlphaProgress::Failed("),
		`if (MentionsExpiry(message)) row->claim.status = "expired";`, "if (MentionsGas(message)) needsGas_ = true;")
	requireAll("ClaimAlphaProgress::Confirmed", body("SnPayoutPresentation.cpp", presentation, "void ClaimAlphaProgress::Confirmed("),
		`row->claim.status = "claimed";`, "if (amountRao > 0) row->claim.amountRao = amountRao;")
	requireAll("ClaimAlphaProgress::Done", body("SnPayoutPresentation.cpp", presentation, "void ClaimAlphaProgress::Done("),
		"sending_ = false;", "if (row.state == Row::State::Sending) row.state = Row::State::Idle;")
	// the page and the dialog name the pure layer's progress
	sheetsHeader := stripComments(readAppSource(t, "EarningsSheets.h"))
	requireAll("EarningsSheets.h", sheetsHeader,
		"using EpochClaim = snpayout::EpochClaim;", "using ClaimAlphaProgress = snpayout::ClaimAlphaProgress;")
}

// The reads of what Claim offers, in order (every app, 2026-11-12): each read
// of the v1 claims, the v2 accounts and the gas balance is numbered as it
// starts and carries its number to its answer, which applies only if no read
// of its kind that started after it has (snpayout::ClaimReads, whose rule the
// spec runs, the race included). A claim's Done makes the reads of its kind
// from before it stale: the page marks them before the read at Done starts,
// and every Claim of that kind waits for a read that started after it (the v1
// tile's and the payout line's, the v2 section's, and the dialogs Claim opens,
// which keep the rows they open with). A sign-out, or the coldkey gone, drops
// every read out.
func TestClaimReadOrder(t *testing.T) {
	requireAll := func(where string, source string, wants ...string) {
		t.Helper()
		for _, want := range wants {
			if !strings.Contains(source, want) {
				t.Errorf("%s: missing %s", where, want)
			}
		}
	}
	requireOrder := func(where, source string, steps ...string) {
		t.Helper()
		at := 0
		for _, step := range steps {
			found := strings.Index(source[at:], step)
			if found < 0 {
				t.Errorf("%s: missing %s, or out of order", where, step)
				return
			}
			at += found + len(step)
		}
	}
	body := func(source, signature string) string {
		t.Helper()
		found := functionBody(source, signature)
		if found == "" {
			t.Fatalf("WalletPage.cpp: no %s", signature)
		}
		return found
	}
	header := stripComments(readAppSource(t, "WalletPage.h"))
	requireAll("WalletPage.h", header,
		"snpayout::ClaimReads v1Reads_;", "snpayout::ClaimReads v2Reads_;", "snpayout::ClaimReads gasReads_;",
		"void ApplyClaims(uint32_t read, std::vector<EpochClaim> const& claims,",
		"void ApplyGas(uint32_t read, std::optional<GasKeyInfo> gas);",
		"void ApplyV2Accounts(uint32_t read, snpayout::LedgerFetch fetch,")
	page := stripComments(readAppSource(t, "WalletPage.cpp"))

	// each read numbered as it starts, its number carried to its answer, which
	// applies only if no newer read's has
	requireOrder("WalletPage::LoadClaims", body(page, "void WalletPage::LoadClaims("),
		"ApplyClaims(v1Reads_.Start(), {}, 0, Fetch::Failed, std::nullopt);",
		"const uint32_t read = v1Reads_.Start();",
		"Sdk().device().snClaims([queue, weak, read](",
		"queue.TryEnqueue([weak, read, claims = std::move(claims), total, ok, error, schedule] {",
		"self->wallet().ApplyClaims(read, claims, total,")
	requireOrder("WalletPage::ApplyClaims", body(page, "void WalletPage::ApplyClaims("),
		"if (!v1Reads_.Apply(read)) return;", "claims_ = claims;")
	requireOrder("WalletPage::LoadV2Accounts", body(page, "void WalletPage::LoadV2Accounts("),
		"ApplyV2Accounts(v2Reads_.Start(), snpayout::LedgerFetch::NoDevice, {}, {});",
		"const uint32_t read = v2Reads_.Start();",
		"Sdk().device().snLedgerAccounts([queue, weak, read](",
		"queue.TryEnqueue([weak, read, accounts = std::move(accounts), unavailable = std::move(unavailable), ok] {",
		"read, ok ? snpayout::LedgerFetch::Ready : snpayout::LedgerFetch::Failed, accounts, unavailable);")
	requireOrder("WalletPage::ApplyV2Accounts", body(page, "void WalletPage::ApplyV2Accounts("),
		"if (!v2Reads_.Apply(read)) return;", "v2Fetch_ = fetch;")
	requireOrder("WalletPage::LoadGas", body(page, "void WalletPage::LoadGas("),
		"ApplyGas(gasReads_.Start(), std::nullopt);",
		"ApplyGas(gasReads_.Start(), std::nullopt);",
		"ApplyGas(gasReads_.Start(), info);",
		"const uint32_t read = gasReads_.Start();",
		"Sdk().device().snGasBalance([queue, weak, read, info](",
		"queue.TryEnqueue([weak, read, info] {",
		"self->wallet().ApplyGas(read, info);")
	requireOrder("WalletPage::ApplyGas", body(page, "void WalletPage::ApplyGas("),
		"if (!gasReads_.Apply(read)) return;", "gas_ = std::move(gas);")
	// every answer carries its read's number: none applies around the order
	for name, first := range map[string]*regexp.Regexp{
		"ApplyClaims":     regexp.MustCompile(`ApplyClaims\(\s*(read|v1Reads_\.Start\(\)|uint32_t read),`),
		"ApplyV2Accounts": regexp.MustCompile(`ApplyV2Accounts\(\s*(read|v2Reads_\.Start\(\)|uint32_t read),`),
		"ApplyGas":        regexp.MustCompile(`ApplyGas\(\s*(read|gasReads_\.Start\(\)|uint32_t read),`),
	} {
		calls := strings.Count(page, name+"(")
		if numbered := len(first.FindAllString(page, -1)); calls == 0 || numbered != calls {
			t.Errorf("WalletPage.cpp: %d of %d %s calls carry a read's number", numbered, calls, name)
		}
	}

	// a claim's Done marks its kind's reads before the read at Done starts
	requireOrder("WalletPage::OnV1ClaimDone", body(page, "void WalletPage::OnV1ClaimDone("),
		"if (v1Claim_.End(ticket)) {", "v1Reads_.Done();", "RebuildClaimButton();", "RebuildPayoutLine();",
		"LoadClaims();", "LoadGas();", "}",
		// the dialogs draw once no claim is in flight: a v2 dialog that waited
		// for this claim offers its Claim again
		"if (claimSheet_) claimSheet_->Redraw();", "if (v2ClaimSheet_) v2ClaimSheet_->Redraw();")
	requireOrder("WalletPage::OnV2ClaimDone", body(page, "void WalletPage::OnV2ClaimDone("),
		"if (v2Claim_.End(ticket)) {", "v2Reads_.Done();", "RebuildV2Section();")
	if strings.Contains(body(page, "void WalletPage::OnV2ClaimDone("), "v1Reads_") ||
		strings.Contains(body(page, "void WalletPage::OnV1ClaimDone("), "v2Reads_") {
		t.Error("WalletPage.cpp: a claim's Done marks the other kind's reads")
	}
	// the read after a v2 claim's Done is WithClaimPaid's, queued after the
	// Done (the spec pins that the Done is handed on first)
	requireOrder("WalletPage::StartV2Claim", body(page, "bool WalletPage::StartV2Claim("),
		"if (auto self = weak.get()) self->wallet().OnV2ClaimDone(claim, ticket);",
		"events = snpayout::WithClaimPaid(std::move(events), [queue, weak, session](bool paid) {",
		"self->wallet().LoadV2Accounts();")

	// every Claim waits for a read since its kind's last Done
	requireAll("WalletPage::RebuildClaimButton", body(page, "void WalletPage::RebuildClaimButton("),
		"snpayout::V1ClaimEnabled(v1Reads_.Fresh(), totalClaimableRao_, CanClaim(), v1Claim_.InFlight())")
	requireAll("WalletPage::RebuildPayoutLine", body(page, "void WalletPage::RebuildPayoutLine("),
		"w_.SnPayoutClaimButton().IsEnabled(",
		"view.showClaim &&",
		"snpayout::V1ClaimEnabled(v1Reads_.Fresh(), totalClaimableRao_, CanClaim(), v1Claim_.InFlight()));")
	requireAll("WalletPage::RebuildV2Section", body(page, "void WalletPage::RebuildV2Section("),
		"v2Claim_.InFlight(),", "v2Reads_.Fresh());")
	requireOrder("WalletPage::OnClaimAlpha", body(page, "winrt::fire_and_forget WalletPage::OnClaimAlpha("),
		"std::shared_ptr<ClaimAlphaProgress> progress = v1Claim_.Progress();", "if (!progress) {",
		"if (!v1Reads_.Fresh()) {", `Notify(Loc("loading"), InfoBarSeverity::Informational);`, "co_return;", "}",
		"progress = std::make_shared<ClaimAlphaProgress>(claims_, gas.tao);", "urnw::ClaimAlphaSheet::Create(")
	requireOrder("WalletPage::OnClaimV2", body(page, "winrt::fire_and_forget WalletPage::OnClaimV2("),
		"std::shared_ptr<snpayout::LedgerClaimProgress> progress = v2Claim_.Progress();", "if (!progress) {",
		"if (!v2Reads_.Fresh()) {", `Notify(Loc("loading"), InfoBarSeverity::Informational);`, "co_return;", "}",
		"progress = std::make_shared<snpayout::LedgerClaimProgress>(", "urnw::ClaimLedgerSheet::Create(")

	// a sign-out, and the coldkey gone, drop every read out
	requireOrder("WalletPage::ResetForSignOut", body(page, "void WalletPage::ResetForSignOut("),
		"v1Claim_.Drop();", "v2Claim_.Drop();", "v1Reads_.Reset();", "v2Reads_.Reset();", "gasReads_.Reset();",
		"RebuildClaimButton();", "RebuildPayoutLine();", "RebuildV2Section();")
	wallet := body(page, "void WalletPage::ApplySnWallet(")
	if at := strings.Index(wallet, "if (!connected) {"); at < 0 {
		t.Error("WalletPage::ApplySnWallet: no branch for a coldkey gone")
	} else {
		requireOrder("WalletPage::ApplySnWallet", wallet[at:],
			"claims_.clear();", "v2Accounts_.clear();", "v1Reads_.Reset();", "v2Reads_.Reset();", "gasReads_.Reset();", "}")
	}

	// the order is the pure layer's, which the spec builds and runs
	presentation := stripComments(readAppSource(t, "SnPayoutPresentation.cpp"))
	requireAll("ClaimReads::Apply", functionBody(presentation, "bool ClaimReads::Apply("),
		"if (read <= shown_) return false;", "shown_ = read;")
	requireAll("V1ClaimEnabled", functionBody(presentation, "bool V1ClaimEnabled("),
		"return (fresh && totalClaimableRao > 0 && canClaim) || claimInFlight;")
	requireAll("LedgerSectionFor", functionBody(presentation, "LedgerSectionView LedgerSectionFor("),
		"view.claimEnabled = vault2Configured && ((canClaim && fresh && !movable.empty()) || claimInFlight);")
}

// Every way settlement v2's Claim misses the look of the earnings claim action,
// one line each (owner decision, 2026-10-13: its colour is consistent with the
// other platforms). On every other platform the v2 tile's Claim is the very
// button its v1 tile's Claim is: the primary button in the theme's accent on
// apple (UrButton .primary, accentColor), the primary button on android
// (URButton PRIMARY, BlueMedium), the earnings gold button on linux
// (ur-earn-gold-button), the tile action on ur.io (actionBtn). Here that button
// is the earnings action, UrPaneActionEarningsStyle, the primary action filled
// with the UrAccentBrush resource: the v1 tile's Claim wears it, so the v2
// tile's does, by the style and never by a colour of its own.
func settlementV2ClaimStyleMisses(mainWindow, appXaml string) []string {
	misses := []string{}
	button := func(name string) string {
		return regexp.MustCompile(`<Button x:Name="` + name + `"[^>]*>`).FindString(mainWindow)
	}
	style := func(element string) string {
		match := regexp.MustCompile(`Style="\{StaticResource (\w+)\}"`).FindStringSubmatch(element)
		if match == nil {
			return ""
		}
		return match[1]
	}
	v1, v2 := button("ClaimButton"), button("SettlementV2ClaimButton")
	if v1 == "" || v2 == "" {
		return append(misses, "MainWindow.xaml: no ClaimButton or SettlementV2ClaimButton")
	}
	if style(v2) == "" || style(v2) != style(v1) {
		misses = append(misses, `MainWindow.xaml: settlement v2's Claim wears "`+style(v2)+`" and the v1 tile's Claim "`+
			style(v1)+`": the two are one button on every platform`)
	}
	if style(v2) != "UrPaneActionEarningsStyle" {
		misses = append(misses, "MainWindow.xaml: settlement v2's Claim is not the earnings action (UrPaneActionEarningsStyle)")
	}
	// the colour is the style's, a brush of the app's resources
	for _, own := range []string{"Background", "Foreground", "BorderBrush"} {
		if strings.Contains(v2, own+"=") {
			misses = append(misses, "MainWindow.xaml: settlement v2's Claim sets a "+own+" of its own")
		}
	}
	if !regexp.MustCompile(`<Style x:Key="UrPaneActionEarningsStyle" TargetType="Button" BasedOn="\{StaticResource UrPaneActionPrimaryStyle\}">\s*` +
		`<Setter Property="Background" Value="\{StaticResource UrAccentBrush\}" />\s*</Style>`).MatchString(appXaml) {
		misses = append(misses, "App.xaml: the earnings action is no longer the primary action filled with the UrAccentBrush resource")
	}
	return misses
}

// Settlement v2's Claim wears the earnings claim action, as the v1 tile's Claim
// above it does.
func TestSettlementV2ClaimWearsTheEarningsAction(t *testing.T) {
	for _, miss := range settlementV2ClaimStyleMisses(readAppSource(t, "MainWindow.xaml"), readAppSource(t, "App.xaml")) {
		t.Error(miss)
	}
}

// Each of those assertions, with the change it pins taken out of a copy of the
// markup, reports exactly that.
func TestSettlementV2ClaimWearsTheEarningsActionCatchesEachRegression(t *testing.T) {
	sources := map[string]string{
		"MainWindow.xaml": readAppSource(t, "MainWindow.xaml"),
		"App.xaml":        readAppSource(t, "App.xaml"),
	}
	const v2Button = "<Button x:Name=\"SettlementV2ClaimButton\"\n" +
		"                                                    Style=\"{StaticResource UrPaneActionEarningsStyle}\""
	const v1Button = "<Button x:Name=\"ClaimButton\"\n" +
		"                                                    Style=\"{StaticResource UrPaneActionEarningsStyle}\""
	for _, control := range []struct {
		name, file, old, new, want string
	}{
		{"the v2 Claim is the plain action blue", "MainWindow.xaml", v2Button,
			strings.Replace(v2Button, "UrPaneActionEarningsStyle", "UrPaneActionPrimaryStyle", 1),
			"the two are one button on every platform"},
		{"the v1 Claim moves on alone", "MainWindow.xaml", v1Button,
			strings.Replace(v1Button, "UrPaneActionEarningsStyle", "UrPaneActionSecondaryStyle", 1),
			"the two are one button on every platform"},
		{"both Claims leave the earnings action", "MainWindow.xaml", "UrPaneActionEarningsStyle}\"\n                                                    HorizontalAlignment=\"Stretch\"\n                                                    IsEnabled=\"False\"",
			"UrPaneActionPrimaryStyle}\"\n                                                    HorizontalAlignment=\"Stretch\"\n                                                    IsEnabled=\"False\"",
			"settlement v2's Claim is not the earnings action"},
		{"a colour of its own", "MainWindow.xaml", v2Button, v2Button + "\n                                                    Background=\"#EFF7BB\"",
			"settlement v2's Claim sets a Background of its own"},
		{"the earnings action's fill is written out", "App.xaml",
			"BasedOn=\"{StaticResource UrPaneActionPrimaryStyle}\">\n                <Setter Property=\"Background\" Value=\"{StaticResource UrAccentBrush}\" />",
			"BasedOn=\"{StaticResource UrPaneActionPrimaryStyle}\">\n                <Setter Property=\"Background\" Value=\"#EFF7BB\" />",
			"the earnings action is no longer the primary action filled with the UrAccentBrush resource"},
	} {
		t.Run(control.name, func(t *testing.T) {
			mutated := map[string]string{"MainWindow.xaml": sources["MainWindow.xaml"], "App.xaml": sources["App.xaml"]}
			count := strings.Count(mutated[control.file], control.old)
			if control.name == "both Claims leave the earnings action" {
				// the two Claims share these lines: both change
				if count != 2 {
					t.Fatalf("%s no longer has exactly two %q; update this control", control.file, control.old)
				}
				mutated[control.file] = strings.ReplaceAll(mutated[control.file], control.old, control.new)
			} else {
				if count != 1 {
					t.Fatalf("%s no longer has exactly one %q; update this control", control.file, control.old)
				}
				mutated[control.file] = strings.Replace(mutated[control.file], control.old, control.new, 1)
			}
			for _, miss := range settlementV2ClaimStyleMisses(mutated["MainWindow.xaml"], mutated["App.xaml"]) {
				if strings.Contains(miss, control.want) {
					return
				}
			}
			t.Fatalf("negative control was not detected (want %q)", control.want)
		})
	}
}
