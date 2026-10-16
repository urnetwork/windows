// SPDX-License-Identifier: MPL-2.0

package tests

import (
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
	if !strings.Contains(update, "urnw::balance::BannerOpen(outOfBalance(), balanceRecovery_)") {
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
