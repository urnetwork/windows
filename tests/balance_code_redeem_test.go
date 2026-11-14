// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile and execute the redeem sheet's answer spec (App/BalanceCodeRedeem.h),
// and check the sheet classifies through the SDK with the network's
// redeemed-code list instead of deciding from the raw answer.
func TestBalanceCodeRedeem(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("balance code redeem tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "balance-code-redeem-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "balance-code-redeem-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build balance code redeem tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("balance code redeem: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}

	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "BalanceSheets.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	sheet := string(source)
	// first without the list, then again with it
	if n := strings.Count(sheet, "urnet::classifyBalanceCodeRedeem("); n != 2 {
		t.Errorf("BalanceSheets.cpp: want 2 urnet::classifyBalanceCodeRedeem calls, got %d", n)
	}
	for _, want := range []string{
		"BalanceCodeRedeemNeedsCodeList(",
		"getNetworkRedeemedBalanceCodes(",
		`Loc("balance_code_already_redeemed_message")`,
		"urnet::isBalanceCodeFormatValid(",
		"urnet::BalanceCodeLength",
	} {
		if !strings.Contains(sheet, want) {
			t.Errorf("BalanceSheets.cpp: missing %s", want)
		}
	}
	// the sheet no longer decides from the raw answer or a hardcoded length
	for _, unwanted := range []string{"kBalanceCodeLength", "result->transfer_balance.has_value()"} {
		if strings.Contains(sheet, unwanted) {
			t.Errorf("BalanceSheets.cpp: still has %s", unwanted)
		}
	}
}

// A redeem is committed when the server answers: a data code's data is on the
// balance, and a gift code's year of Pro is granted before the answer. The
// redeem owner must read the balance once, which shows either (the plan flips
// to Pro on that read), and never start the Pro confirmation poll, which waits
// for a payment there is none of (the plan ring spins and the
// insufficient-balance notice is held back for 2 minutes, then the poll records
// a timed-out purchase).
func TestBalanceCodeRedeemReadsTheBalanceOnce(t *testing.T) {
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "MainWindow.xaml.cpp"))
	if err != nil {
		t.Fatal(err)
	}
	window := string(source)
	start := strings.Index(window, "winrt::fire_and_forget MainWindow::ShowRedeemSheet()")
	if start < 0 {
		t.Fatal("MainWindow.xaml.cpp: no ShowRedeemSheet")
	}
	end := strings.Index(window[start:], "\n}\n")
	if end < 0 {
		t.Fatal("MainWindow.xaml.cpp: ShowRedeemSheet has no end")
	}
	showRedeemSheet := window[start : start+end]
	if strings.Contains(showRedeemSheet, "StartConfirmationPolling(") {
		t.Errorf("ShowRedeemSheet: a redeemed data code starts the Pro confirmation poll")
	}
	if !strings.Contains(showRedeemSheet, "balance().Refresh()") {
		t.Errorf("ShowRedeemSheet: a redeemed code does not read the balance")
	}
}

// A gift code grants a year of Pro and a 0-byte transfer balance. The redeem
// sheet confirms it as "A year of Pro is yours." or, for a year queued after a
// paid Pro period, the day it starts in the user's locale. A gift this network
// already holds, found in its redeemed-code list after a lost or refused
// answer (searched for as the server reads a typed code), confirms with the
// plain line; a code the server calls redeemed that the list lacks reads
// "already redeemed". The field leaves room for a code in groups of four, as a
// card prints it. The redeemed-code list shows a gift as "A year of Pro", never
// "+0 B".
func TestBalanceCodeRedeemShowsAGiftsYearOfPro(t *testing.T) {
	sheets := stripComments(readAppSource(t, "BalanceSheets.cpp"))
	inOrder := func(name, code string, wants ...string) {
		t.Helper()
		at := 0
		for _, want := range wants {
			next := strings.Index(code[at:], want)
			if next < 0 {
				t.Errorf("%s: missing, or out of order: %s", name, want)
				return
			}
			at += next + len(want)
		}
	}
	inOrder("GiftRedeemedText", definitionBody(t, "BalanceSheets.cpp", sheets, "hstring GiftRedeemedText("),
		"const bool queued = result && result->pro && result->pro->queued;",
		"LocalDateText(*result->pro->start_time)",
		"ShowsQueuedGiftLine(queued, !date.empty())",
		`Format("gift_pro_redeemed_queued", date)`,
		`Loc("gift_pro_redeemed")`)
	inOrder("LocalDateText", definitionBody(t, "BalanceSheets.cpp", sheets, "std::wstring LocalDateText("),
		"cryptopay::ParseRfc3339(rfc3339)", `DateTimeFormatter formatter(L"shortdate")`)
	inOrder("RedeemCodeSheet::Classify", definitionBody(t, "BalanceSheets.cpp", sheets, "void RedeemCodeSheet::Classify("),
		"ApplyResult(BalanceCodeRedeemNoticeFor(outcome), result);",
		"getNetworkRedeemedBalanceCodes(",
		"codes ? RedeemedCodeKind(secret, *codes) : std::nullopt;",
		"result && result->error && RefusalSaysAlreadyRedeemed(result->error->message);",
		"BalanceCodeRedeemNoticeAfterList(urnet::classifyBalanceCodeRedeem(result, codes, secret),")
	inOrder("RedeemCodeSheet::ApplyResult", definitionBody(t, "BalanceSheets.cpp", sheets, "void RedeemCodeSheet::ApplyResult("),
		"notice == BalanceCodeRedeemNotice::GiftRedeemed) {",
		"if (notice == BalanceCodeRedeemNotice::GiftRedeemed || (result && result->pro)) {",
		"GiftRedeemedText(notice == BalanceCodeRedeemNotice::Redeemed ? result : std::nullopt)",
		`successAmountText_.Text(H("+" + FormatByteCountCompact(bytes)));`,
		"if (onRedeemed_) onRedeemed_(")
	for _, want := range []string{
		"static_assert(std::string_view(urnet::BalanceCodeKindProGift) == kBalanceCodeKindProGift);",
		"codeBox_.MaxLength(static_cast<int32_t>(2 * urnet::BalanceCodeLength));",
	} {
		if !strings.Contains(sheets, want) {
			t.Errorf("BalanceSheets.cpp: missing %s", want)
		}
	}

	account := stripComments(readAppSource(t, "AccountPage.cpp"))
	render := definitionBody(t, "AccountPage.cpp", account, "void AccountPage::RenderBalanceCodes(")
	for _, want := range []string{"IsProGiftCode(code.kind)", `? Loc("balance_code_pro_year")`,
		`: H("+" + urnw::FormatByteCountCompact(code.balance_byte_count))`} {
		if !strings.Contains(render, want) {
			t.Errorf("AccountPage::RenderBalanceCodes: missing %s", want)
		}
	}
}
