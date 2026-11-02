// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// Compile and execute the insufficient-balance gate (Common/BalanceGate.h): the
// connect button keeps Disconnect out of balance, the balance reaction never
// disconnects, the notice is posted once per out-of-balance episode, and a
// connect gesture out of balance starts nothing while a live session is kept.
func TestBalanceGate(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("balance gate tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "balance-gate-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "Common"),
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "balance-gate-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build balance gate tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("balance gate: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Every connect entry point passes the start-connect gate (BalanceGate.h)
// before it records a session request, so out of balance the tray, a row or
// the connect button cannot start the tunnel. A reattach and Disconnect are
// never gated: a session that runs out of balance stays up.
func TestConnectEntryPointsAdmitStartConnect(t *testing.T) {
	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	entries := []string{
		"void SdkHost::ConnectBestAvailable()",
		"void SdkHost::Connect(const std::string& connectLocationJson)",
		"void SdkHost::Connect(const urnet::ConnectLocation& location)",
		"void SdkHost::ConnectFromRow(const urnet::ConnectLocation& location)",
		"void SdkHost::ConnectBestAvailableFromRow()",
	}
	connectKinds := 0
	for _, signature := range entries {
		body := definitionBody(t, "SdkHost.cpp", host, signature)
		admit := strings.Index(body, "AdmitStartConnect(")
		request := strings.Index(body, "RequestSession(")
		if admit < 0 || request < 0 || request < admit {
			t.Errorf("%s: records a session request without AdmitStartConnect first", signature)
		}
		connectKinds += strings.Count(body, "r.kind = ConnectKind::")
	}
	// no other place builds a connecting request
	if n := strings.Count(host, "r.kind = ConnectKind::BestAvailable") +
		strings.Count(host, "r.kind = ConnectKind::Location"); n != connectKinds {
		t.Errorf("SdkHost.cpp: %d connecting requests, %d of them behind the gate", n, connectKinds)
	}
	for _, signature := range []string{
		"void SdkHost::EnsureSession(const char* reason, bool automaticRecovery)",
		"void SdkHost::Disconnect()",
	} {
		if strings.Contains(definitionBody(t, "SdkHost.cpp", host, signature), "AdmitStartConnect(") {
			t.Errorf("%s: gated on balance; a live session must never be dropped or stranded", signature)
		}
	}

	app := stripLineComments(readAppSource(t, "AppController.cpp"))
	start := strings.Index(app, "cb.onConnectToggle = [this] {")
	end := strings.Index(app, "cb.connectAction = ")
	if start < 0 || end < start {
		t.Fatal("AppController.cpp: no tray connect toggle; update this contract")
	}
	if !strings.Contains(app[start:end], "urnw::balance::RouteConnectGesture(") {
		t.Errorf("AppController.cpp: the tray's Connect does not route through RouteConnectGesture")
	}
	if !strings.Contains(app, "sdk_.SetStartConnectGate(") {
		t.Errorf("AppController.cpp: the start-connect gate is never set")
	}

	page := stripLineComments(readAppSource(t, "ConnectPage.cpp"))
	toggle := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::OnConnectToggle(")
	admit := strings.Index(toggle, "AdmitStartConnect(")
	optimistic := strings.Index(toggle, "connectStatus_ = ConnectStatus::Connecting")
	if admit < 0 || optimistic < 0 || optimistic < admit {
		t.Errorf("ConnectPage::OnConnectToggle: flips to Connecting before the start-connect gate")
	}
}

// The start-connect gate reads the out-of-balance latch, not the raw contract
// status the SDK resets on the user's Disconnect, and the window's gate and
// banner read the same latched state.
func TestStartConnectGateReadsTheLatch(t *testing.T) {
	app := stripLineComments(readAppSource(t, "AppController.cpp"))
	onStats := definitionBody(t, "AppController.cpp", app, "void AppController::OnStats(const LiveStats& stats)")
	if strings.Contains(onStats, "insufficientBalance_ = stats.insufficientBalance") {
		t.Errorf("AppController::OnStats: the gate reads the raw contract status, which Disconnect resets")
	}
	if !strings.Contains(onStats, "ObserveBalanceLatch()") {
		t.Errorf("AppController::OnStats: does not feed the out-of-balance latch")
	}
	if !strings.Contains(onStats, "latched.insufficientBalance = insufficientBalance_") ||
		strings.Contains(onStats, "OnStatsChanged(stats)") {
		t.Errorf("AppController::OnStats: the window gets the raw contract status, not the latched one")
	}
	observe := definitionBody(t, "AppController.cpp", app, "void AppController::ObserveBalanceLatch()")
	for _, want := range []string{"balanceLatch_.Observe(", "insufficientBalance_ = balanceLatch_.InsufficientBalance()"} {
		if !strings.Contains(observe, want) {
			t.Errorf("AppController::ObserveBalanceLatch: missing %s", want)
		}
	}
	auth := definitionBody(t, "AppController.cpp", app, "void AppController::OnAuthState(")
	if !strings.Contains(auth, "balanceLatch_.Reset()") {
		t.Errorf("AppController::OnAuthState: a sign-in or sign-out does not reset the latch")
	}
	gate := definitionBody(t, "AppController.cpp", app, "bool AppController::OutOfBalance() const")
	if !strings.Contains(gate, "insufficientBalance_") {
		t.Errorf("AppController::OutOfBalance: does not read the latched state")
	}
}

// The start-connect gate reads the subscription balance with its read time,
// and on a stale one fetches it before deciding, so the first connect after a
// launch on an empty account is blocked while no contract status exists yet.
// Every connect entry point hands the gate the gesture to ask again once that
// fetch settles; the fetch settles on success, failure and its timeout.
func TestStartConnectGateReadsAFreshBalance(t *testing.T) {
	app := stripLineComments(readAppSource(t, "AppController.cpp"))
	facts := definitionBody(t, "AppController.cpp", app,
		"urnw::balance::StartConnectFacts AppController::CurrentStartConnectFacts() const")
	for _, want := range []string{
		"f.latched = insufficientBalance_",
		"f.balance.availableBytes = balance.availableByteCount",
		"f.balance.openTransferBytes = balance.pendingByteCount",
		"f.balance.fetchedAtMs = balance.fetchedAtMillis",
		"f.fetchSettledAtMs = balance_.FetchSettledAtMillis()",
	} {
		if !strings.Contains(facts, want) {
			t.Errorf("AppController::CurrentStartConnectFacts: missing %s", want)
		}
	}
	if !strings.Contains(app, "balance_.FetchThen(") {
		t.Errorf("AppController.cpp: the start-connect gate never fetches a stale balance")
	}

	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	for _, signature := range []string{
		"void SdkHost::ConnectBestAvailable()",
		"void SdkHost::Connect(const std::string& connectLocationJson)",
		"void SdkHost::Connect(const urnet::ConnectLocation& location)",
		"void SdkHost::ConnectFromRow(const urnet::ConnectLocation& location)",
		"void SdkHost::ConnectBestAvailableFromRow()",
	} {
		body := definitionBody(t, "SdkHost.cpp", host, signature)
		if !strings.Contains(body, "AdmitStartConnect(\"") || !strings.Contains(body, "[this") {
			t.Errorf("%s: does not hand the gate the gesture to ask again", signature)
		}
	}
	page := stripLineComments(readAppSource(t, "ConnectPage.cpp"))
	toggle := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::OnConnectToggle(")
	if !strings.Contains(toggle, "AdmitStartConnect(\"connect button\", [") {
		t.Errorf("ConnectPage::OnConnectToggle: does not hand the gate the press to ask again")
	}

	store := stripLineComments(readAppSource(t, "SubscriptionBalance.cpp"))
	fetch := definitionBody(t, "SubscriptionBalance.cpp", store, "void SubscriptionBalanceStore::Fetch()")
	if strings.Count(fetch, "Settle();") < 2 {
		t.Errorf("SubscriptionBalanceStore::Fetch: a failed or successful fetch does not settle the waiters")
	}
	if !strings.Contains(store, "kStartConnectFetchTimeoutMs") {
		t.Errorf("SubscriptionBalance.cpp: a start-connect fetch has no timeout")
	}
	apply := definitionBody(t, "SubscriptionBalance.cpp", store,
		"void SubscriptionBalanceStore::Apply(urnet::SubscriptionBalanceResult const& result)")
	if !strings.Contains(apply, "snapshot_.fetchedAtMillis = NowMillis()") {
		t.Errorf("SubscriptionBalanceStore::Apply: does not stamp when the balance was read")
	}
}

// A connect the balance blocked recovers by itself (BalanceGate.h,
// BalanceRecovery): the gate hands the refused gesture to the app, which
// waits on the balance with it; every stats and balance push feeds the
// recovery, which runs the gesture again (or rebuilds a held connection) only
// past the gate and says so; another connect, the user's Disconnect, a sign-in
// or sign-out and the banner's Cancel end the wait. The banner says whether
// the data is reserved or used up and that the app reconnects by itself.
func TestBalanceRecoveryWiring(t *testing.T) {
	root := repositoryRoot(t)
	host := stripLineComments(readAppSource(t, "SdkHost.cpp"))
	app := stripLineComments(readAppSource(t, "AppController.cpp"))
	window := stripLineComments(readAppSource(t, "MainWindow.xaml.cpp"))
	connect := stripLineComments(readAppSource(t, "ConnectPage.cpp"))
	sheets := stripLineComments(readAppSource(t, "BalanceSheets.cpp"))
	ticker := stripLineComments(readAppSource(t, "FreeRefreshTicker.cpp"))

	admit := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::AdmitStartConnect(const char* what, std::function<void()> again)")
	// the retry passes the gate; a start that waited on its balance is told
	// its outcome there too (start_connect_wait_test.go)
	for _, want := range []string{
		"if (!startConnectFacts_ || retryingRefusedConnect_) {",
		"return true;",
		"host.startConnectUpgrade_(again);",
		"if (connectAdmitted_) connectAdmitted_();",
	} {
		if !strings.Contains(admit, want) {
			t.Errorf("SdkHost::AdmitStartConnect: missing %s", want)
		}
	}
	if decided := strings.Index(admit, "AdmitStartConnect(startConnectFacts_(), sinks)"); decided < 0 ||
		!strings.Contains(admit[decided:], "if (admitted) {") {
		t.Errorf("SdkHost::AdmitStartConnect: the observer must hear only an admitted gesture")
	}
	retry := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::RetryRefusedConnect(const std::function<void()>& connect)")
	set := strings.Index(retry, "retryingRefusedConnect_ = true;")
	run := strings.Index(retry, "connect();")
	clear := strings.Index(retry, "retryingRefusedConnect_ = false;")
	if set < 0 || run < set || clear < run {
		t.Error("SdkHost::RetryRefusedConnect does not run the gesture past the gate and close the gate after it")
	}
	if !strings.Contains(definitionBody(t, "SdkHost.cpp", host, "void SdkHost::Disconnect()"), "if (onUserDisconnect_) onUserDisconnect_();") {
		t.Error("SdkHost::Disconnect does not end the wait on the balance")
	}

	// the gate's refused gesture and the tray's refused Connect wait on the balance
	gate := strings.Index(app, "sdk_.SetStartConnectGate(")
	if gate < 0 {
		t.Fatal("AppController.cpp: no start-connect gate; update this contract")
	}
	wait := strings.Index(app[gate:], "WaitOnBalance(std::move(refused));")
	show := strings.Index(app[gate:], "ShowUpgradeForBlockedConnect();")
	if wait < 0 || show < wait {
		t.Error("AppController.cpp: the gate's refused gesture does not wait on the balance before the upgrade path shows")
	}
	if !strings.Contains(app, "sdk_.SetConnectGestureObserver([this] { ClearBalanceRecovery(); },") {
		t.Error("AppController.cpp: an admitted connect or the user's Disconnect does not end the wait")
	}
	tray := app[strings.Index(app, "cb.onConnectToggle = [this] {"):strings.Index(app, "cb.connectAction = ")]
	if !strings.Contains(tray, "app.WaitOnBalance(") {
		t.Error("AppController.cpp: the tray's refused Connect does not wait on the balance")
	}
	for _, signature := range []string{
		"void AppController::OnStats(const LiveStats& stats)",
	} {
		body := definitionBody(t, "AppController.cpp", app, signature)
		latch := strings.Index(body, "ObserveBalanceLatch();")
		recovery := strings.Index(body, "ObserveBalanceRecovery();")
		if latch < 0 || recovery < latch || !strings.Contains(body, "connectRequested_ = stats.connected;") {
			t.Errorf("%s: does not feed the recovery after the latch", signature)
		}
	}
	balancePush := app[strings.Index(app, "balance_.SetChangeHandler("):]
	if latch, recovery := strings.Index(balancePush, "ObserveBalanceLatch();"), strings.Index(balancePush, "ObserveBalanceRecovery();"); latch < 0 || recovery < latch {
		t.Error("AppController.cpp: a balance push does not feed the recovery after the latch")
	}
	observe := definitionBody(t, "AppController.cpp", app, "void AppController::ObserveBalanceRecovery()")
	for _, want := range []string{
		"balanceRecovery_.Observe(OutOfBalance(), connectRequested_, facts.balance, facts.nowMs);",
		"sdk_.RetryRefusedConnect(step.target);",
		`Localized("insufficient_balance_reconnecting")`,
	} {
		if !strings.Contains(observe, want) {
			t.Errorf("AppController::ObserveBalanceRecovery: missing %s", want)
		}
	}
	// every retry goes past the gate, never through a gated entry point directly
	if strings.Count(observe, "sdk_.RetryRefusedConnect(") != 2 ||
		strings.Count(observe, "sdk_.Connect") != strings.Count(observe, "sdk_.Connect(*location);")+
			strings.Count(observe, "sdk_.ConnectBestAvailable();")+strings.Count(observe, "sdk_.ConnectSelected();") {
		t.Error("AppController::ObserveBalanceRecovery: a retry does not go through RetryRefusedConnect")
	}
	auth := definitionBody(t, "AppController.cpp", app, "void AppController::OnAuthState(AuthState state, const std::string& error)")
	if !strings.Contains(auth, "ClearBalanceRecovery();") {
		t.Error("AppController::OnAuthState: a sign-in or sign-out does not end the wait")
	}

	// the banner: open for a waiting start, Cancel, and the lines
	update := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::UpdateBalanceWarning()")
	if !strings.Contains(update, "urnw::balance::BannerOpen(outOfBalance() || dataLimit_, balanceRecovery_)") {
		t.Error("MainWindow::UpdateBalanceWarning: a waiting start does not keep the banner open")
	}
	if !strings.Contains(window, "urnw::App().ClearBalanceRecovery();") ||
		!strings.Contains(window, `L"acceptance.insufficient-balance.cancel-reconnect"`) {
		t.Error("MainWindow.xaml.cpp: the banner has no Cancel for a waiting start")
	}
	message := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyBalanceWarningMessage()")
	for _, want := range []string{
		"urnw::balance::RecoveryLinesFor(",
		"urnw::OutOfBalanceKindText(recovery.kind, w_.reservedByteCount())",
		`"insufficient_balance_will_reconnect"`,
	} {
		if !strings.Contains(message, want) {
			t.Errorf("ConnectPage::ApplyBalanceWarningMessage: missing %s", want)
		}
	}
	if !strings.Contains(definitionBody(t, "BalanceSheets.cpp", sheets, "void UpgradeSheet::Build("),
		"OutOfBalanceKindText(balance::OutOfBalanceKindFor(read), snapshot.pendingByteCount)") {
		t.Error("UpgradeSheet::Build: the blocked connect's sheet does not say whether the data is reserved or used up")
	}
	kind := definitionBody(t, "FreeRefreshTicker.cpp", ticker, "std::wstring OutOfBalanceKindText(balance::OutOfBalanceKind kind, int64_t reservedByteCount)")
	for _, want := range []string{`"insufficient_balance_reserved"`, `"insufficient_balance_exhausted"`} {
		if !strings.Contains(kind, want) {
			t.Errorf("OutOfBalanceKindText: missing %s", want)
		}
	}

	// every new string reaches the catalog, with its placeholder lowered
	for key, want := range map[string]string{
		"insufficient_balance_reserved":       "{} is reserved for your open connections. What they don't use is returned as they close.",
		"insufficient_balance_exhausted":      "You're out of data until the free refresh or an upgrade.",
		"insufficient_balance_will_reconnect": "You'll be reconnected when data is available again.",
		"insufficient_balance_reconnecting":   "Data is available again. Reconnecting…",
		"cancel":                              "Cancel",
	} {
		if got := reswValue(t, root, "en", key); got != want {
			t.Errorf("en resw %s = %q, want %q", key, got, want)
		}
	}
}

// Compile the spec against a copy of a header rewritten by old -> replacement, and
// require it to fail, naming want. `header` is its path under app/src.
func requireBalanceGateFailure(t *testing.T, header, old, replacement, want string) {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("balance gate tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	source, err := os.ReadFile(filepath.Join(root, "app", "src", filepath.FromSlash(header)))
	if err != nil {
		t.Fatal(err)
	}
	mutated := strings.ReplaceAll(string(source), old, replacement)
	if mutated == string(source) {
		t.Fatalf("negative control did not change the production %s: %q", header, old)
	}
	includeDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(includeDir, filepath.Base(header)), []byte(mutated), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "balance-gate-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-Wno-unused-parameter", "-I"+includeDir,
		"-I"+filepath.Join(root, "app", "src", "Common"),
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "balance-gate-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build balance gate tests: %v\n%s", err, output)
	}
	output, err := exec.Command(program).CombinedOutput()
	if err == nil {
		t.Fatalf("the spec passed with %q rewritten\n%s", old, output)
	}
	if !strings.Contains(string(output), want) {
		t.Fatalf("the spec did not name %q:\n%s", want, output)
	}
}

// The defect: every session reads as held, what the banner and the tray notice
// said before they asked the service.
func TestBalanceGateNegativeEverySessionReadsAsHeld(t *testing.T) {
	requireBalanceGateFailure(t, "Common/BalanceGate.h",
		"if (routesInstalled) return TrafficLine::Held;",
		"return TrafficLine::Held;",
		"never captured: the notice does not claim the traffic is held")
}

// The other way to get it wrong: a kill switch's blocked machine told it is exposed.
func TestBalanceGateNegativeBlockedReadsAsNotProtected(t *testing.T) {
	requireBalanceGateFailure(t, "Common/BalanceGate.h",
		"return firewallInForce ? TrafficLine::Blocked : TrafficLine::NotProtected;",
		"return TrafficLine::NotProtected;",
		"a session with no capture under a policy is blocked")
}

// The defect: a paying account refused hears nothing.
func TestBalanceGateNegativeNoDataLimit(t *testing.T) {
	requireBalanceGateFailure(t, "Common/BalanceGate.h",
		"return planKnown && pro && !refusedBeforePro_ && !confirming;",
		"return false;",
		"Pro refused reads as the limit")
}

// The trap: a purchase while held announces a data limit.
func TestBalanceGateNegativeUpgradeReadsAsADataLimit(t *testing.T) {
	requireBalanceGateFailure(t, "Common/BalanceGate.h",
		"if (planKnown && !pro) refusedBeforePro_ = true;",
		"",
		"the old refusal after the purchase does not read as a limit")
}

// The first balance of a launch taken for a purchase hides a real limit.
func TestBalanceGateNegativeFirstBalanceReadsAsAnUpgrade(t *testing.T) {
	requireBalanceGateFailure(t, "Common/BalanceGate.h",
		"if (planKnown && !pro) refusedBeforePro_ = true;",
		"if (!pro) refusedBeforePro_ = true;",
		"Pro on the first reading is no upgrade")
}

// The limit's words: not the balance's upgrade.
func TestBalanceGateNegativeDataLimitInTheBalancesWords(t *testing.T) {
	requireBalanceGateFailure(t, "App/DataInfo.h",
		`return dataLimit ? "data_limit_held_notice" : "insufficient_balance_held_notice";`,
		`return "insufficient_balance_held_notice";`,
		"at a data limit and held offers no upgrade")
}

// The line said for nobody leaves the page's stall unexplained.
func TestBalanceGateNegativeNeverSaysDisconnectFirst(t *testing.T) {
	requireBalanceGateFailure(t, "Common/BalanceGate.h",
		"return outOfBalance && (line == TrafficLine::Held || line == TrafficLine::Blocked);",
		"return false;",
		"disconnect first: out of balance, and the traffic cannot get out")
}

// The line said over a session that never captured sends the user to disconnect
// from nothing: the page goes out the normal way there.
func TestBalanceGateNegativeSaysDisconnectFirstWithNothingInTheWay(t *testing.T) {
	requireBalanceGateFailure(t, "Common/BalanceGate.h",
		"return outOfBalance && (line == TrafficLine::Held || line == TrafficLine::Blocked);",
		"return outOfBalance;",
		"disconnect first: no capture and no policy, the page goes out the normal way")
}

// A banner telling the user to disconnect with nothing on it to do so.
func TestBalanceGateNegativeBannerWithoutADisconnect(t *testing.T) {
	requireBalanceGateFailure(t, "Common/BalanceGate.h",
		"return refused && sessionUp;",
		"return false;",
		"banner disconnect: a refused session can be ended from the banner")
}

// Checkout runs in WebView2 or the browser, which the capture takes, so the banner
// tells an account out of balance to disconnect first, from the decision and the
// traffic line it already reads. A paying account at a limit is not told.
func TestOutOfBalanceBannerSaysDisconnectFirst(t *testing.T) {
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	message := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyBalanceWarningMessage()")
	provideRequireOrder(t, "ApplyBalanceWarningMessage", message,
		"urnw::balance::BannerSaysDisconnectFirst(outOfBalance, traffic)",
		`urnw::Localized("insufficient_balance_disconnect_first")`)
	provideRequireOrder(t, "ApplyBalanceWarningMessage", message,
		"urnw::datainfo::TrafficLineKey(dataLimit, traffic)",
		"urnw::balance::BannerSaysDisconnectFirst(outOfBalance, traffic)")
	// the checkout paths this rests on: the page in WebView2, then the browser, and
	// the only UI image the firewall permits is the app's, whose SDK sockets alone
	// are bound off the tun
	sheets := stripComments(readAppSource(t, "BalanceSheets.cpp"))
	provideRequire(t, "BalanceSheets.cpp", sheets, "core.Navigate(H(url));", "LaunchUriAsync(Uri(H(url)))")
	policy := stripComments(readServiceSource(t, "WfpPolicy.h"))
	provideRequire(t, "WfpPolicy.h", policy, "std::wstring app_image_path;")
	requireNone(t, "the service", stripComments(readServiceSource(t, "TunnelController.cpp")+policy),
		"msedgewebview2")
}

// The banner's Disconnect: on the banner, ending the session the way the connect
// button's press does, offered from the one decision, and touching nothing else,
// the provide mode least of all (a user who provides keeps providing).
func TestOutOfBalanceBannerHasADisconnect(t *testing.T) {
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	if count := strings.Count(window, `L"acceptance.insufficient-balance.disconnect"`); count != 1 {
		t.Errorf("MainWindow.xaml.cpp names the banner's Disconnect %d times, want once", count)
	}
	provideRequire(t, "MainWindow.xaml.cpp", window,
		`balanceDisconnect_.Content(LocBox("disconnect"));`,
		"links.Children().Append(balanceDisconnect_);",
		"BalanceWarning().Content(links);")
	at := strings.Index(window, "balanceDisconnect_.Click(")
	if at < 0 {
		t.Fatal("MainWindow.xaml.cpp: the banner's Disconnect has no click")
	}
	end := strings.Index(window[at:], "});")
	if end < 0 {
		t.Fatal("MainWindow.xaml.cpp: the banner's Disconnect click does not close")
	}
	click := window[at : at+end]
	provideRequire(t, "the banner's Disconnect", click, "Sdk().Disconnect();")
	requireNone(t, "the banner's Disconnect", click,
		"Provide", "ConnectSelected", "AdmitStartConnect", "OnConnectToggle")

	shown := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::ApplyBalanceDisconnect(bool offered)")
	provideRequire(t, "ApplyBalanceDisconnect", shown, "balanceDisconnect_.Visibility(")
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	message := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyBalanceWarningMessage()")
	provideRequire(t, "ApplyBalanceWarningMessage", message,
		"w_.ApplyBalanceDisconnect(urnw::balance::BannerOffersDisconnect(outOfBalance || dataLimit, sessionUp));")

	// what the press runs: the session worker's Disconnect, which writes no mode
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	requireNone(t, "SdkHost::Disconnect", definitionBody(t, "SdkHost.cpp", host, "void SdkHost::Disconnect()"),
		"setProvideControlMode", "SetProvideControlMode")
}

// Both surfaces read the one decision. The banner and the tray notice need Windows
// and the service, so they are read with their comments blanked: neither names the
// held string itself, each takes the routes and the firewall from the service's
// facts, and the tray reads them when it posts.
func TestRefusalAlertsAskTheServiceWhatTheTrafficIsDoing(t *testing.T) {
	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	message := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyBalanceWarningMessage()")
	provideRequire(t, "ApplyBalanceWarningMessage", message,
		"urnw::balance::TrafficLineFor(sessionUp, facts.routesInstalled,",
		"urnw::gesture::WfpInForce(facts)",
		"urnw::datainfo::AlertTitleKey(dataLimit)",
		"if (outOfBalance || dataLimit) {")
	requireNone(t, "ApplyBalanceWarningMessage", message,
		"insufficient_balance_held_notice", "data_limit_held_notice", "lines.held")

	app := stripComments(readAppSource(t, "AppController.cpp"))
	notice := definitionBody(t, "AppController.cpp", app, "void AppController::ShowRefusalNotice(bool dataLimit)")
	provideRequire(t, "ShowRefusalNotice", notice,
		"CurrentServiceFacts();",
		"urnw::balance::TrafficLineFor(connectRequested_, facts.routesInstalled,",
		"gesture::WfpInForce(facts)",
		"urnw::datainfo::AlertTitleKey(dataLimit)")
	react := definitionBody(t, "AppController.cpp", app, "void AppController::ReactToBalance()")
	requireNone(t, "ReactToBalance", react, "insufficient_balance_held_notice", "data_limit_held_notice", "ShowBalloon")
	// the acceptance driver counts this line, once per out-of-balance episode
	provideRequire(t, "ReactToBalance", react, `LogInfo("app: insufficient balance notice posted");`)
}

// A paying account refused gets the alert: the gate reads the raw status, the
// banner opens for it with nothing to buy on it, the hero shows the error, and a
// session's own Disconnect is untouched.
func TestDataLimitAlertWiring(t *testing.T) {
	app := stripComments(readAppSource(t, "AppController.cpp"))
	react := definitionBody(t, "AppController.cpp", app, "void AppController::ReactToBalance()")
	for _, pair := range [][2]string{
		{"dataLimit_ = dataLimitGate_.Observe(rawInsufficientBalance_, balance.loaded, balance.isPro,",
			"urnw::balance::ReactToDataLimitPush(dataLimitNotice_, rawInsufficientBalance_, dataLimit_,"},
		{"urnw::balance::ReactToDataLimitPush(dataLimitNotice_, rawInsufficientBalance_, dataLimit_,",
			"PublishDataLimit();"},
	} {
		provideRequireOrder(t, "ReactToBalance", react, pair[0], pair[1])
	}
	requireNone(t, "ReactToBalance", react, "dataLimitGate_.Observe(insufficientBalance_")
	provideRequire(t, "PublishDataLimit",
		definitionBody(t, "AppController.cpp", app, "void AppController::PublishDataLimit()"),
		"self->SetDataLimit(dataLimit_);")

	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	update := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::UpdateBalanceWarning()")
	provideRequire(t, "UpdateBalanceWarning", update,
		"urnw::balance::BannerOpen(outOfBalance() || dataLimit_, balanceRecovery_)",
		"urnw::balance::BannerOffersUpgrade(dataLimit_)",
		"balanceUpgrade_.Visibility(offered);",
		"balanceWhy_.Visibility(offered);")
	provideRequire(t, "MainWindow.xaml.cpp", window, "balanceUpgrade_ = getPro;", "balanceWhy_ = why;")
	set := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::SetDataLimit(bool dataLimit)")
	provideRequire(t, "SetDataLimit", set, "UpdateBalanceWarning();")

	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	status := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyConnectStatus()")
	provideRequire(t, "ApplyConnectStatus", status,
		"urnw::balance::BannerOpen(outOfBalance || w_.dataLimit(), w_.balanceRecovery())",
		"} else if (outOfBalance || w_.dataLimit()) {")
	// a paying account's button stays what it was: the limit blocks no gesture
	requireNone(t, "ApplyConnectStatus", status, "const bool blocked = processing || outOfBalance || w_.dataLimit();")
}
