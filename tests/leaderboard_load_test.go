// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"fmt"
	"regexp"
	"strings"
	"testing"
)

// The Earnings leaderboard loads once per session, the first time it is
// showing while signed in. It is showing while its tab is the selected one,
// and that becomes true two ways: the tab is selected, or a session starts
// with the tab already selected - a sign-out does not change the selection.
// Only the first ever loaded the board, so after a re-sign-in with the tab
// still selected it stayed on "Loading..." until the user switched tabs. Both
// ways now ask one gate, WalletPage::LoadLeaderboardIfShowing. The page needs
// WinRT, so these read its source and MainWindow's with comments stripped.

// What is wrong with the board's load, given WalletPage.cpp and
// MainWindow.xaml.cpp sources, if anything.
func leaderboardLoadViolations(wallet, window string) []string {
	wallet = stripComments(lf(wallet))
	window = stripComments(lf(window))
	var violations []string
	body := func(source, signature string) (string, bool) {
		start := strings.Index(source, signature)
		if start < 0 {
			return "", false
		}
		end := strings.Index(source[start:], "\n}\n")
		if end < 0 {
			return "", false
		}
		return source[start : start+end+2], true
	}

	// a session starting on Earnings runs the destination's load: the wallet
	// branch of MainWindow::LoadCurrentDestination
	loadCurrent, ok := body(window, "void MainWindow::LoadCurrentDestination() {")
	if !ok {
		return []string{"MainWindow.xaml.cpp no longer defines MainWindow::LoadCurrentDestination"}
	}
	walletBranch := loadCurrent
	if at := strings.Index(walletBranch, `tag == L"wallet"`); at >= 0 {
		walletBranch = walletBranch[at:]
		if end := strings.Index(walletBranch, "} else if"); end >= 0 {
			walletBranch = walletBranch[:end]
		}
	} else {
		violations = append(violations, "MainWindow::LoadCurrentDestination has no wallet branch")
	}
	sessionLoads := regexp.MustCompile(`wallet_->([A-Za-z_][A-Za-z0-9_]*)\(`).FindAllStringSubmatch(walletBranch, -1)
	if len(sessionLoads) == 0 {
		violations = append(violations, "the wallet branch of MainWindow::LoadCurrentDestination calls nothing on the page")
	}
	reached := false
	var called []string
	for _, match := range sessionLoads {
		called = append(called, match[1])
		if load, ok := body(wallet, "void WalletPage::"+match[1]+"("); ok && strings.Contains(load, "LoadLeaderboardIfShowing();") {
			reached = true
		}
	}
	if !reached {
		violations = append(violations, fmt.Sprintf(
			"a session that starts on Earnings (MainWindow::LoadCurrentDestination -> wallet_->%s) never asks for the board's load: with the Leaderboard tab still selected from before a sign-out, it stays on Loading",
			strings.Join(called, ", ")))
	}

	// selecting the tab asks the same gate, after the hosts show the tab
	tab, ok := body(wallet, "void WalletPage::OnEarningsTableChanged(")
	if !ok {
		violations = append(violations, "WalletPage.cpp no longer defines WalletPage::OnEarningsTableChanged")
	} else {
		shown := strings.Index(tab, "w_.LeaderboardHost().Visibility(")
		asked := strings.Index(tab, "LoadLeaderboardIfShowing();")
		if asked < 0 {
			violations = append(violations, "selecting the Leaderboard tab (WalletPage::OnEarningsTableChanged) does not ask LoadLeaderboardIfShowing")
		} else if shown < 0 || asked < shown {
			violations = append(violations, "WalletPage::OnEarningsTableChanged asks for the board before it shows the tab's host")
		}
	}

	// the gate: once per session, only while signed in, only while showing
	gate, ok := body(wallet, "void WalletPage::LoadLeaderboardIfShowing() {")
	if !ok {
		return append(violations, "WalletPage.cpp does not define WalletPage::LoadLeaderboardIfShowing, the board's one load gate")
	}
	for _, guard := range []string{"leaderboardLoaded_", "Sdk().IsLoggedIn()", "w_.LeaderboardHost().Visibility() != Visibility::Visible"} {
		if at := strings.Index(gate, guard); at < 0 || at > strings.Index(gate, "LoadLeaderboard();") {
			violations = append(violations, fmt.Sprintf("WalletPage::LoadLeaderboardIfShowing loads without checking %s first", guard))
		}
	}
	if strings.Count(wallet, "leaderboardLoaded_ = true;") != 1 || !strings.Contains(gate, "leaderboardLoaded_ = true;") {
		violations = append(violations, "the board's load is marked done outside WalletPage::LoadLeaderboardIfShowing")
	}
	// ...and a sign-out re-arms it
	if reset, ok := body(wallet, "void WalletPage::ResetForSignOut() {"); !ok || !strings.Contains(reset, "leaderboardLoaded_ = false;") {
		violations = append(violations, "WalletPage::ResetForSignOut no longer re-arms the board's load")
	}
	return violations
}

func TestLeaderboardLoadsWheneverItIsShowing(t *testing.T) {
	for _, violation := range leaderboardLoadViolations(readAppSource(t, "WalletPage.cpp"), readAppSource(t, "MainWindow.xaml.cpp")) {
		t.Error(violation)
	}
}

// The contract fires on each way back to the defect.
func TestLeaderboardLoadContractFires(t *testing.T) {
	wallet := lf(readAppSource(t, "WalletPage.cpp"))
	window := lf(readAppSource(t, "MainWindow.xaml.cpp"))
	mutations := []struct{ name, file, old, new, want string }{
		{"the session load does not ask", "wallet", "  protocol_->LoadMe();\n  LoadLeaderboardIfShowing();\n", "  protocol_->LoadMe();\n", "never asks for the board's load"},
		{"the tab does not ask", "wallet", "  LoadLeaderboardIfShowing();\n  UpdatePointsIndicator();\n", "  UpdatePointsIndicator();\n", "does not ask LoadLeaderboardIfShowing"},
		{"the gate skips the sign-in check", "wallet", "  if (leaderboardLoaded_ || !Sdk().IsLoggedIn()) return;\n", "  if (leaderboardLoaded_) return;\n", "without checking Sdk().IsLoggedIn()"},
		{"a second load marker", "wallet", "void WalletPage::LoadLeaderboard() {\n", "void WalletPage::LoadLeaderboard() {\n  leaderboardLoaded_ = true;\n", "marked done outside"},
		{"the session load moved off the page", "window", "    wallet_->LoadWallet();\n", "    wallet_->ResyncProviderStats();\n", "never asks for the board's load"},
	}
	for _, mutation := range mutations {
		source := map[string]string{"wallet": wallet, "window": window}
		if count := strings.Count(source[mutation.file], mutation.old); count != 1 {
			t.Errorf("%s: want the text to mutate once, found it %d times", mutation.name, count)
			continue
		}
		source[mutation.file] = strings.Replace(source[mutation.file], mutation.old, mutation.new, 1)
		violations := leaderboardLoadViolations(source["wallet"], source["window"])
		joined := strings.Join(violations, "; ")
		if !strings.Contains(joined, mutation.want) {
			t.Errorf("%s: want a violation with %q, got %q", mutation.name, mutation.want, violations)
			continue
		}
		t.Logf("fires (%s): %s", mutation.name, joined)
	}
}
