// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The client limit hold, as the app shows it (App/ClientLimitNotice.h; owner,
// 2026-11-08). Before it, nothing in the app read the session device's
// ClientLimitStatus: a device the platform held off for its network's concurrent
// client limit sat connecting with no explanation until the service's failsafe
// stopped it, and a sign-in the limit refused showed the server's raw message, or
// with production's x402 on, "402 Payment Required" and the payment terms. The
// decisions (the prompt, the notice policy, the 402 reading) are the sdk's and are
// tested there; this pins what the app does with them: the spec of the header,
// and the wiring the spec cannot run, read with every comment blanked.

// Compile and run the spec (app/tools/client-limit-notice-tests.cpp).
func TestClientLimitNotice(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("client limit notice tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "client-limit-notice-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "client-limit-notice-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build client limit notice tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("client limit notice: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
	project := readAppSource(t, "App.vcxproj")
	if !strings.Contains(project, `<ClInclude Include="ClientLimitNotice.h" />`) {
		t.Error("App.vcxproj does not list ClientLimitNotice.h")
	}
}

// The header spells the sdk's values itself so it needs no sdk header; they must
// be the sdk's (cgo urnetwork_sdk.hpp, which carries the prompt and the notice
// tracker from the client limit sdk on). Skipped with no such header around.
func TestClientLimitNoticeSpellsTheSdksValues(t *testing.T) {
	header := sdkHeaderWith(t, "ClientLimitNoticeTracker")
	notice := readAppSource(t, "ClientLimitNotice.h")
	for _, pair := range [][2]string{
		{"ClientLimitStatusExceeded", "kStatusExceeded"},
		{"ClientLimitPromptUpgrade", "kPromptUpgrade"},
		{"ClientLimitPromptRemoveClient", "kPromptRemoveClient"},
		{"ClientLimitNoticePost", "kNoticePost"},
		{"ClientLimitNoticeUpdate", "kNoticeUpdate"},
		{"ClientLimitNoticeRemove", "kNoticeRemove"},
	} {
		sdkValue := constantValue(t, "urnetwork_sdk.hpp", header, pair[0])
		appValue := constantValue(t, "ClientLimitNotice.h", notice, pair[1])
		if sdkValue != appValue {
			t.Errorf("ClientLimitNotice.h: %s is %q, the sdk's %s is %q", pair[1], appValue, pair[0], sdkValue)
		}
	}
	for _, want := range []string{"std::string Prompt{};", "std::optional<std::string> client_limit_prompt;",
		"bool retryClientLimit() const;"} {
		if !strings.Contains(header, want) {
			t.Errorf("urnetwork_sdk.hpp: missing %s", want)
		}
	}
}

// The value of `inline constexpr const char* name = "...";` in source.
func constantValue(t *testing.T, where, source, name string) string {
	t.Helper()
	match := regexp.MustCompile(`inline constexpr const char\* ` + regexp.QuoteMeta(name) +
		` = "([^"]*)";`).FindStringSubmatch(source)
	if match == nil {
		t.Fatalf("%s: no %s", where, name)
	}
	return match[1]
}

// SdkHost's feed: the session's device, from the bootstrap to the teardown, the
// slow-internet feed's shape. Begun with the session, the listener registered and
// one read asked for, the remote's presence noted, published only over a
// connected remote (a DeviceRemote that lost the service answers with its last
// readout), ended at the teardown, and its workers stopped before the device
// goes. The retry asks the device by its handle, off the caller's thread.
func TestClientLimitFeedWiring(t *testing.T) {
	source := sdkHostSource(t)
	bootstrap := definitionBody(t, "SdkHost.cpp", source, "bool SdkHost::BootstrapSession(")
	order := []string{
		"BeginTransportDelay(generation, device_->handle());",
		"BeginClientLimit(generation, device_->handle());",
		"NoteClientLimitRemote(generation, remoteConnected);",
		"device_->addClientLimitStatusChangeListener(",
		"NoteClientLimit(generation, ClientLimitReadingOf(*status));",
		"clientLimitReader_.Request();",
	}
	for i := 0; i+1 < len(order); i++ {
		provideRequireOrder(t, "BootstrapSession", bootstrap, order[i], order[i+1])
	}
	publish := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::PublishClientLimitLocked() {")
	provideRequire(t, "PublishClientLimitLocked", publish,
		"if (clientLimitRemote_) value = clientLimitReading_;",
		"if (value == clientLimitPublished_) return;",
		"onClientLimit_(std::move(value));")
	read := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::ReadClientLimitOnWorker() {")
	provideRequire(t, "ReadClientLimitOnWorker", read, "urnet_device_get_client_limit_status(device)",
		"urnet_free_string(json);")
	retry := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::RetryClientLimitOnWorker() {")
	provideRequire(t, "RetryClientLimitOnWorker", retry, "urnet_device_retry_client_limit(device)",
		"if (ended) clientLimitReader_.Request();")
	// the retry after a change (a purchase): the sdk starts its floor over
	changeRetry := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::RetryClientLimitAfterChangeOnWorker() {")
	provideRequire(t, "RetryClientLimitAfterChangeOnWorker", changeRetry,
		"urnet_device_retry_client_limit_after_change(device)",
		"if (ended) clientLimitReader_.Request();")
	requireNone(t, "the client limit feed", publish+read+retry+changeRetry, "device_->", "mutex_.lock", "std::scoped_lock lock(mutex_)")

	teardown := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::TeardownSessionLocked(")
	provideRequireOrder(t, "TeardownSessionLocked", teardown, "EndClientLimit();",
		"if (device_) { device_->close(); device_.reset(); }")
	if n := strings.Count(source, "clientLimitReader_.Stop();"); n != 2 {
		t.Errorf("SdkHost.cpp stops the client limit reader in %d places, want 2 (the destructor and the quit)", n)
	}
	if n := strings.Count(source, "clientLimitRetrier_.Stop();"); n != 2 {
		t.Errorf("SdkHost.cpp stops the client limit retry in %d places, want 2", n)
	}
	if n := strings.Count(source, "clientLimitChangeRetrier_.Stop();"); n != 2 {
		t.Errorf("SdkHost.cpp stops the client limit change retry in %d places, want 2", n)
	}
	header := stripComments(readAppSource(t, "SdkHost.h"))
	provideRequire(t, "SdkHost.h", header, "void RetryClientLimit() { clientLimitRetrier_.Request(); }",
		"void RetryClientLimitAfterChange() { clientLimitChangeRetrier_.Request(); }")
}

// AppController: the window's notice and the tray's, from the feed and the
// connection request. The tray's notice is the sdk's tracker's, the
// background rule decides whether it shows, a sign-in or sign-out ends it, the
// plan turning Pro in this run ends a hold, and nothing here disconnects.
func TestClientLimitNoticeAppWiring(t *testing.T) {
	source := stripComments(readAppSource(t, "AppController.cpp"))
	start := definitionBody(t, "AppController.cpp", source, "void AppController::Start() {")
	provideRequireOrder(t, "Start", start, "clientLimitTracker_ = urnet::newClientLimitNoticeTracker();",
		"sdk_.SetClientLimitHandler(")
	provideRequire(t, "Start", start, "OnUi([this, reading] { OnClientLimit(reading); });",
		"if (tag == kBalloonClientLimit) ShowWindow(nullptr);",
		"clientlimit::UpgradeEndsHold(clientLimitBalanceLoaded_, clientLimitBalancePro_,",
		"clientlimit::Held(clientLimit_)")
	show := definitionBody(t, "AppController.cpp", source, "void AppController::ShowWindowImpl(")
	provideRequire(t, "ShowWindowImpl", show,
		"windowFocused_ = args.WindowActivationState() != WindowActivationState::Deactivated;")
	upgrade := start[strings.Index(start, "clientlimit::UpgradeEndsHold("):]
	// a purchase is a change the server confirmed: the change retry, past the
	// backoff of retries refused before it (delta review D-K1)
	provideRequireOrder(t, "the upgrade's retry", upgrade, "sdk_.RetryClientLimitAfterChange();", "clientLimitBalancePro_ = snapshot.isPro;")
	requireNone(t, "the upgrade's retry", upgrade[:strings.Index(upgrade, "clientLimitBalancePro_ = snapshot.isPro;")], "sdk_.RetryClientLimit();")

	stats := definitionBody(t, "AppController.cpp", source, "void AppController::OnStats(")
	provideRequireOrder(t, "OnStats", stats, "connectRequested_ = stats.connected;", "ObserveClientLimit();")

	observe := definitionBody(t, "AppController.cpp", source, "void AppController::ObserveClientLimit() {")
	provideRequire(t, "ObserveClientLimit", observe,
		"clientlimit::NoticeFor(clientLimit_, connectRequested_)",
		"PublishClientLimit();",
		"ApplyClientLimitTray(clientLimitTracker_.update(status, connectRequested_));",
		"s.Prompt = clientLimit_->prompt;")
	tray := definitionBody(t, "AppController.cpp", source, "void AppController::ApplyClientLimitTray(")
	provideRequire(t, "ApplyClientLimitTray", tray,
		"clientlimit::TrayStepFor(action, InBackground())",
		"tray_.HideBalloon(kBalloonClientLimit);",
		"Localized(clientlimit::kTitleKey)",
		"Localized(clientlimit::MessageKey(offersUpgrade)), kBalloonClientLimit,",
		`LogInfo("app: client limit notice posted{}"`)
	background := definitionBody(t, "AppController.cpp", source, "bool AppController::InBackground() const")
	provideRequire(t, "InBackground", background, "return !windowVisible_ || !windowFocused_;")
	requireNone(t, "the client limit reaction", observe+tray, "Disconnect", "StopServiceTunnel")

	auth := definitionBody(t, "AppController.cpp", source, "void AppController::OnAuthState(")
	provideRequire(t, "OnAuthState", auth, "clientLimit_.reset();",
		"ApplyClientLimitTray(clientLimitTracker_.reset());", "clientLimitNotice_.reset();")

	present := definitionBody(t, "AppController.cpp", source, "void AppController::ReconcileWindowPresentation() {")
	provideRequire(t, "ReconcileWindowPresentation", present, "self->SetClientLimitNotice(clientLimitNotice_);")
}

// The window's notice: one writer for its words and its Get Pro, Retry through
// the controller, Get Pro through the balance banner's upgrade path (the
// notices' one OpenNoticeUpgrade, which the Pro features notice's Get Pro runs
// too), and the InfoBar itself beside the balance banner.
func TestClientLimitNoticeWindowWiring(t *testing.T) {
	source := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	apply := definitionBody(t, "MainWindow.xaml.cpp", source, "void MainWindow::ApplyClientLimitNotice() {")
	provideRequire(t, "ApplyClientLimitNotice", apply,
		"bar.Title(Loc(urnw::clientlimit::kTitleKey));",
		"bar.Message(Loc(urnw::clientlimit::MessageKey(offersUpgrade)));",
		"clientLimitUpgrade_.Visibility(offersUpgrade ? Visibility::Visible : Visibility::Collapsed);",
		"bar.IsOpen(true);", "bar.IsOpen(false);")
	provideRequire(t, "MainWindow's constructor", source,
		"ClientLimitNotice().ActionButton(clientLimitUpgrade_);",
		"clientLimitUpgrade_.Content(LocBox(\"get_pro\"));",
		"clientLimitRetry_.Content(LocBox(\"retry\"));",
		"clientLimitRetry_.Click([](auto const&, auto const&) { urnw::App().RetryClientLimit(); });",
		"ClientLimitNotice().Content(actions);")
	upgrade := source[strings.Index(source, "clientLimitUpgrade_.Click("):]
	upgrade = upgrade[:strings.Index(upgrade, "});")]
	provideRequire(t, "the notice's Get Pro", upgrade, "self->OpenNoticeUpgrade();")
	// a guest converts first and then gets the sheet; anyone else gets it at once
	flow := definitionBody(t, "MainWindow.xaml.cpp", source, "void MainWindow::OpenNoticeUpgrade() {")
	provideRequireOrder(t, "OpenNoticeUpgrade", flow, "if (balance_.guest) {", "DivertGuestToConversion(")
	provideRequireOrder(t, "OpenNoticeUpgrade", flow, "DivertGuestToConversion(", "self->ShowUpgradeSheet();")
	provideRequireOrder(t, "OpenNoticeUpgrade", flow, "return;", "ShowUpgradeSheet();\n}")

	ids := xamlAutomationIds(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "MainWindow.xaml"))
	if got := ids["ClientLimitNotice"]; got != "acceptance.client-limit.notice" {
		t.Errorf("MainWindow.xaml: ClientLimitNotice's automation id is %q", got)
	}
}

// A sign-in the limit refused: the registration hands the page the sdk's
// decision as a token ahead of the server's message, and the page shows the
// words for it.
func TestClientLimitSignInRefusalWiring(t *testing.T) {
	source := sdkHostSource(t)
	register := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::RegisterNetworkClient(")
	provideRequireOrder(t, "RegisterNetworkClient", register,
		"clientlimit::SignInRefusalToken(result->error->client_limit_prompt)",
		"AuthResult r{false, false, result->error->message};")
	provideRequire(t, "RegisterNetworkClient", register, "complete(AuthResult{false, false, *token});")
	login := stripComments(readAppSource(t, "LoginPage.cpp"))
	mapped := definitionBody(t, "LoginPage.cpp", login, "hstring LoginPage::MapAuthErrorForDisplay(")
	provideRequireOrder(t, "MapAuthErrorForDisplay", mapped,
		"urnw::clientlimit::SignInRefusalMessageKey(error)", "return H(error);")
}
