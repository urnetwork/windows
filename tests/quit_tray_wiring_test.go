// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"strings"
	"testing"
)

// The call sites of the owner's 2026-10-05 decision (support inbox 1521):
// closing the window hides it to the tray, and Quit in the tray menu stops the
// tunnel session and the provider-only device in the service, and the app's own
// background work, before the app exits. What each ending stops is pure and
// runs in app_lifetime_test.go; the window, the tray, SdkHost and the service
// need Windows and WinRT, so these read their sources with every comment
// blanked, so prose cannot satisfy a contract.

// The text from the first `opener` through the first `terminator` after it.
func quitSourceFrom(t *testing.T, where, source, opener, terminator string) string {
	t.Helper()
	start := strings.Index(source, opener)
	if start < 0 {
		t.Fatalf("%s no longer has %q; update this contract", where, opener)
	}
	end := strings.Index(source[start:], terminator)
	if end < 0 {
		t.Fatalf("cannot find the end of %q in %s", opener, where)
	}
	return source[start : start+end+len(terminator)]
}

// The index of the first match of `pattern` in text, or -1.
func quitIndex(text, pattern string) int {
	if location := regexp.MustCompile(pattern).FindStringIndex(text); location != nil {
		return location[0]
	}
	return -1
}

// Each pattern occurs in text, in this order.
func quitRequireSequence(t *testing.T, where, text string, patterns ...string) {
	t.Helper()
	last, lastPattern := -1, ""
	for _, pattern := range patterns {
		at := quitIndex(text, pattern)
		if at < 0 {
			t.Errorf("%s is missing %s", where, pattern)
			continue
		}
		if at < last {
			t.Errorf("%s must have %s before %s", where, lastPattern, pattern)
		}
		last, lastPattern = at, pattern
	}
}

// Each needle must be absent from text; why says what running it would break.
func quitForbid(t *testing.T, where, text string, why string, needles ...string) {
	t.Helper()
	for _, needle := range needles {
		if strings.Contains(text, needle) {
			t.Errorf("%s must not run %q: %s", where, needle, why)
		}
	}
}

// The app's AppController.cpp, comments stripped.
func appControllerSource(t *testing.T) string {
	return stripComments(readAppSource(t, "AppController.cpp"))
}

// Close -> hide. The main window's own close (its X, Alt+F4, the taskbar's or
// the system menu's Close) is cancelled and hides the window to the tray;
// nothing on that path ends the app, stops the tunnel or stops the provider.
// Only Shutdown closes the window for real or exits the application.
func TestQuitTrayClosingTheWindowHidesIt(t *testing.T) {
	controller := appControllerSource(t)
	closing := handlerSource(t, "AppController.cpp", controller, "appWindow.Closing(")
	quitRequireSequence(t, "the window's Closing handler", closing,
		regexp.QuoteMeta("if (quitting_) return;"),
		regexp.QuoteMeta("args.Cancel(true);"),
		regexp.QuoteMeta("HideWindow();"))
	quitForbid(t, "the window's Closing handler", closing,
		"closing the window hides it to the tray",
		"Shutdown(", ".Exit(", "sdk_.")

	hide := definitionBody(t, "AppController.cpp", controller, "void AppController::HideWindow() {")
	provideRequire(t, "AppController::HideWindow", hide, ".AppWindow().Hide();")
	quitForbid(t, "AppController::HideWindow", hide,
		"a hidden window keeps the app, the tunnel and the provider running",
		"Shutdown(", ".Exit(", ".Close()", "sdk_.Quit(", "sdk_.Disconnect(",
		"sdk_.StopServiceTunnel(", "sdk_.Logout(")

	// The window is closed, and the application exited, by Shutdown alone (and
	// App::OnLaunched's launch failures, before there is a window or a tray).
	shutdown := definitionBody(t, "AppController.cpp", controller, "void AppController::Shutdown(")
	for _, needle := range []string{"window_.Close()", ".Exit()"} {
		if strings.Count(controller, needle) != 1 || !strings.Contains(shutdown, needle) {
			t.Errorf("AppController.cpp runs %q outside Shutdown (or not exactly once): "+
				"a close path that quits instead of hiding", needle)
		}
	}
	for name, source := range appSourceFiles(t, ".cpp") {
		if name == "AppController.cpp" || name == "App.xaml.cpp" {
			continue
		}
		if strings.Contains(stripComments(source), ".Exit()") {
			t.Errorf("%s exits the application: only AppController::Shutdown may end the app", name)
		}
	}
}

// Alt+F4 on the tray's hidden window, which holds the foreground after its
// menu closes, is the closing gesture aimed at a window nobody can see: it is
// swallowed before DefWindowProc turns it into a WM_CLOSE that would end the
// app, and every other system command still reaches DefWindowProc.
func TestQuitTrayAltF4OnTheTrayWindowDoesNotQuit(t *testing.T) {
	source := stripComments(readAppSource(t, "TrayIcon.cpp"))
	wndProc := definitionBody(t, "TrayIcon.cpp", source, "LRESULT CALLBACK TrayIcon::WndProc(")
	command := quitSourceFrom(t, "TrayIcon::WndProc", wndProc, "case WM_SYSCOMMAND:", "case WM_CLOSE:")
	quitRequireSequence(t, "TrayIcon::WndProc's WM_SYSCOMMAND case", command,
		regexp.QuoteMeta("if ((wParam & 0xFFF0) == SC_CLOSE) {"),
		regexp.QuoteMeta("return 0;"),
		regexp.QuoteMeta("return ::DefWindowProcW(hwnd, msg, wParam, lParam);"))
	quitForbid(t, "TrayIcon::WndProc's WM_SYSCOMMAND case", command,
		"Alt+F4 is a close, and closing never quits", "cb_.onQuit", "cb_.onCloseRequest")
}

// Quit -> stop_tunnel and stop_provider -> exit. The tray menu's Quit reaches
// Shutdown(Quit); Shutdown takes the window and the tray down first, then has
// SdkHost stop the service, then exits; SdkHost::Quit closes every way of
// starting anything again before it sends stop_tunnel and then stop_provider
// under the session lock; and the service's stop_tunnel ends the session,
// lifts the firewall policy (the kill switch's armed floor too, as Disconnect
// does) and retires the provider-only device.
func TestQuitTrayQuitStopsTheTunnelAndTheProviderThenExits(t *testing.T) {
	tray := stripComments(readAppSource(t, "TrayIcon.cpp"))
	menu := definitionBody(t, "TrayIcon.cpp", tray, "void TrayIcon::ShowContextMenu(POINT pt) {")
	provideRequire(t, "TrayIcon::ShowContextMenu", menu,
		`::AppendMenuW(menu, MF_STRING, kMenuQuit, Localized("quit_urnetwork").c_str());`,
		"case kMenuQuit: if (cb_.onQuit) cb_.onQuit(); break;")

	controller := appControllerSource(t)
	onQuit := quitSourceFrom(t, "AppController::Start", controller, "cb.onQuit = [this] {", "};")
	provideRequire(t, "the tray's Quit callback", onQuit, "Shutdown(lifetime::Ending::Quit);")

	shutdown := definitionBody(t, "AppController.cpp", controller,
		"void AppController::Shutdown(lifetime::Ending ending) {")
	quitRequireSequence(t, "AppController::Shutdown", shutdown,
		regexp.QuoteMeta("if (quitting_.exchange(true, std::memory_order_acq_rel)) return;"),
		regexp.QuoteMeta("const lifetime::Plan plan = lifetime::PlanFor(ending);"),
		regexp.QuoteMeta("updates_.Stop();"),
		regexp.QuoteMeta("balance_.Stop();"),
		regexp.QuoteMeta("tray_.Destroy();"),
		regexp.QuoteMeta("if (window_) window_.Close();"),
		regexp.QuoteMeta("if (plan.stopService) sdk_.Quit();"),
		regexp.QuoteMeta("if (auto app = Application::Current()) app.Exit();"))
	if strings.Count(controller, "sdk_.Quit()") != 1 {
		t.Error("AppController.cpp must stop the service in Shutdown's one guarded call, and nowhere else")
	}

	host := sdkHostSource(t)
	quit := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::Quit() {")
	quitRequireSequence(t, "SdkHost::Quit", quit,
		regexp.QuoteMeta("std::scoped_lock lock(pendingMutex_);"),
		regexp.QuoteMeta("quitting_.store(true);"),
		regexp.QuoteMeta("QuitInOrder(start);"))
	// its steps, on the quit's own thread: the user's Disconnect still queued
	// runs first, and the waits are bounded (exit_drain_test.go)
	inOrder := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::QuitInOrder(std::chrono::steady_clock::time_point start) {")
	quitRequireSequence(t, "SdkHost::QuitInOrder", inOrder,
		regexp.QuoteMeta("SettleCommandsForExit(start + exitdrain::kCommandsCap)"),
		regexp.QuoteMeta("StopServiceWatchdog();"),
		regexp.QuoteMeta("StopPresentationWorker();"),
		regexp.QuoteMeta("StopSyncWatchdog();"),
		regexp.QuoteMeta("StopProviderOnlyStats();"),
		regexp.QuoteMeta("std::scoped_lock lock(mutex_);"),
		regexp.QuoteMeta("if (!service_.IsConnected()) service_.Connect();"),
		regexp.QuoteMeta("service_.StopTunnel();"),
		regexp.QuoteMeta("service_.StopProvider(&after, &error)"),
		`TeardownSessionLocked\(\s*false\);`)
	unlocked := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::StopServiceWithoutLock() {")
	for name, body := range map[string]string{
		"SdkHost::Quit": quit, "SdkHost::QuitInOrder": inOrder, "SdkHost::StopServiceWithoutLock": unlocked,
	} {
		quitForbid(t, name, body,
			"quitting is not signing out, and nothing may start what it stops",
			"service_.Logout(", "asyncLocalState_->logout(", "setRouteLocal(",
			"setProvideControlMode(", "service_.StartProvider(", "service_.StartTunnel(",
			"RequestSession(", "RequestProviderReconcile(", "EnsureSession(", "SetKillSwitch(")
	}

	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	stopTunnel := quitSourceFrom(t, "ControlServer::Handle", server,
		"type == proto::msg::kStopTunnel", "} else if")
	provideRequire(t, "ControlServer's stop_tunnel", stopTunnel, "tunnel_.Stop();")
	stopProvider := quitSourceFrom(t, "ControlServer::Handle", server,
		"type == proto::msg::kStopProvider", "} else if")
	provideRequire(t, "ControlServer's stop_provider", stopProvider, "reply.ok = tunnel_.StopProvider();")

	controllerSource := tunnelControllerSource(t)
	stop := definitionBody(t, "TunnelController.cpp", controllerSource, "void TunnelController::Stop() {")
	if quitIndex(stop, `StopLocked\(\s*true\);`) < 0 {
		t.Error("TunnelController::Stop must tear down with finalDisarm=true: a deliberate stop lifts the firewall policy")
	}
	stopLocked := definitionBody(t, "TunnelController.cpp", controllerSource,
		"void TunnelController::StopLocked(bool finalDisarm)")
	quitRequireSequence(t, "TunnelController::StopLocked", stopLocked,
		regexp.QuoteMeta("RevertMachineStateLocked(finalDisarm, hadRoutes);"),
		regexp.QuoteMeta("RetireProviderDeviceLocked();"),
		regexp.QuoteMeta("TearDownSessionLocked();"))
	revert := definitionBody(t, "TunnelController.cpp", controllerSource,
		"void TunnelController::RevertMachineStateLocked(bool finalDisarm, bool hadRoutes)")
	disarm := quitSourceFrom(t, "TunnelController::RevertMachineStateLocked", revert,
		"if (finalDisarm) {", "} else if")
	provideRequire(t, "RevertMachineStateLocked's finalDisarm branch", disarm,
		"ApplyWfpLocked(WfpState::Off);")
	// the branch may name the kill switch in its log line, never gate on it
	if quitIndex(disarm, `if\s*\([^)]*killSwitch_`) >= 0 {
		t.Error("RevertMachineStateLocked's finalDisarm branch gates the lift on the kill switch: " +
			"a deliberate stop (Disconnect, Quit) must lift the policy whatever the setting")
	}
	stopProviderBody := definitionBody(t, "TunnelController.cpp", controllerSource,
		"bool TunnelController::StopProvider() {")
	provideRequire(t, "TunnelController::StopProvider", stopProviderBody, "RetireProviderDeviceLocked();")
}

// No auto-restart after Quit. Every start in this app goes through the session
// worker, which only serves what RequestSession recorded: the latch Quit sets
// stops RequestSession recording anything, a pass already running cannot start
// the provider (ReconcileProviderLocked), the threads that ask by themselves
// are stopped for good, and the latch is never cleared in this process. The
// service starts nothing without a request. The next launch reconciles afresh.
func TestQuitTrayNothingStartsAgainAfterQuit(t *testing.T) {
	host := sdkHostSource(t)

	request := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::RequestSession(SessionRequest request) {")
	quitRequireSequence(t, "SdkHost::RequestSession", request,
		regexp.QuoteMeta("std::scoped_lock lock(pendingMutex_);"),
		regexp.QuoteMeta("if (quitting_.load()) {"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("pending_ = std::move(request);"),
		regexp.QuoteMeta("std::thread([this] { SessionWorkerLoop(); }).detach();"))

	reconcile := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::ReconcileProviderLocked(const char* reason) {")
	latch := strings.Index(reconcile, "if (quitting_.load()) return;")
	if latch < 0 {
		t.Fatal("SdkHost::ReconcileProviderLocked does not stop at the quit latch")
	}
	for _, call := range []string{"CurrentServiceStatusLocked(", "service_.StopProvider(", "service_.StartProvider("} {
		if at := strings.Index(reconcile, call); at < 0 || at < latch {
			t.Errorf("SdkHost::ReconcileProviderLocked reaches %q before the quit latch", call)
		}
	}

	// the latch is set by Quit alone and never cleared
	if strings.Count(host, "quitting_.store(") != 1 ||
		!strings.Contains(definitionBody(t, "SdkHost.cpp", host, "void SdkHost::Quit() {"), "quitting_.store(true);") {
		t.Error("SdkHost.cpp must set the quit latch in Quit, once, and never clear it")
	}
	provideRequire(t, "SdkHost.h", stripComments(readAppSource(t, "SdkHost.h")),
		"std::atomic<bool> quitting_{false};")

	// every start is the worker's: start_provider in the reconcile, start_tunnel
	// in the bootstrap, and both called from the worker loop alone
	worker := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::SessionWorkerLoop() {")
	bootstrap := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::BootstrapSession(const char* reason, bool attachOnly) {")
	for call, owner := range map[string]string{
		"service_.StartProvider(": reconcile,
		"service_.StartTunnel(":   bootstrap,
	} {
		if strings.Count(host, call) != 1 || !strings.Contains(owner, call) {
			t.Errorf("SdkHost.cpp sends %q outside the session worker's passes", call)
		}
	}
	for call, sites := range map[string]int{"ReconcileProviderLocked(": 2, "BootstrapSession(": 1} {
		// the definition is the one occurrence outside the worker
		if got := strings.Count(worker, call); got != sites || strings.Count(host, call) != sites+1 {
			t.Errorf("SdkHost.cpp calls %s outside the session worker (worker %d, file %d)",
				call, got, strings.Count(host, call))
		}
	}

	// the threads that act by themselves stay stopped once Quit stopped them
	provideRequire(t, "SdkHost::ScheduleServiceRetry",
		definitionBody(t, "SdkHost.cpp", host, "void SdkHost::ScheduleServiceRetry() {"),
		"if (watchdogStop_) return;")
	provideRequire(t, "SdkHost::StopServiceWatchdog",
		definitionBody(t, "SdkHost.cpp", host, "void SdkHost::StopServiceWatchdog() {"),
		"watchdogStop_ = true;")
	provideRequire(t, "SdkHost::ArmSyncWatchdogLocked",
		definitionBody(t, "SdkHost.cpp", host, "void SdkHost::ArmSyncWatchdogLocked("),
		"if (syncStop_) return;")
	provideRequire(t, "SdkHost::SetPresentationActive",
		definitionBody(t, "SdkHost.cpp", host, "void SdkHost::SetPresentationActive(bool active) {"),
		"if (presentationStop_) return;")
	if strings.Count(host, "providerOnlyThread_ = std::thread(") != 1 ||
		!strings.Contains(definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::Initialize() {"),
			"providerOnlyThread_ = std::thread(") {
		t.Error("the provider-only statistics loop must be started by Initialize alone, so Quit's stop holds")
	}

	// The service starts a session or a provider only when asked to: each start
	// is its own request's, and nothing in the controller or the service's
	// entry point starts either by itself (a restarted service runs nothing).
	server := stripComments(readServiceSource(t, "ControlServer.cpp"))
	for call, verb := range map[string]string{
		"tunnel_.Start(":         "type == proto::msg::kStartTunnel",
		"tunnel_.StartProvider(": "type == proto::msg::kStartProvider",
	} {
		branch := quitSourceFrom(t, "ControlServer::Handle", server, verb, "} else if")
		if strings.Count(server, call) != 1 || !strings.Contains(branch, call) {
			t.Errorf("ControlServer.cpp runs %q outside its own request", call)
		}
	}
	tunnel := tunnelControllerSource(t)
	start := definitionBody(t, "TunnelController.cpp", tunnel,
		"proto::TunnelStatus TunnelController::Start(const proto::StartTunnel& config) {")
	if strings.Count(tunnel, "StartLocked(") != 2 || !strings.Contains(start, "StartLocked(config)") {
		t.Error("TunnelController.cpp starts a session outside the start_tunnel request's Start")
	}
	if strings.Count(tunnel, "StartProvider(") != 1 {
		t.Error("TunnelController.cpp starts the provider-only device outside the start_provider request")
	}
	service := stripComments(readServiceSource(t, "main.cpp"))
	quitForbid(t, "the service's main.cpp", service, "the service starts nothing by itself",
		"StartProvider(", "StartLocked(", ".Start(cfg", ".Start(config")

	// The next launch starts fresh: the resume reattaches only (D8), and the
	// pass that finds nothing ends in the provider reconcile from the stored
	// mode; signed out, the reconcile stops whatever an earlier run left.
	initialize := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::Initialize() {")
	provideRequire(t, "SdkHost::Initialize", initialize,
		`EnsureSession("resume");`, `RequestProviderReconcile("launch, signed out");`)
	provideRequire(t, "SdkHost::SessionWorkerLoop", worker, "if (!device_) ReconcileProviderLocked(req.reason);")
}

// Not a Quit. A WM_CLOSE from outside the app, the in-app update's installer
// closing it among them, exits the app and leaves the service as it is; the
// app does not quit for the update by itself. Signing out keeps the app
// running in the tray (Logout stops the tunnel and the provider as Quit does,
// then logs the service out: sign_out_wiring_test.go).
func TestQuitTrayOtherEndingsLeaveTheService(t *testing.T) {
	controller := appControllerSource(t)
	provideRequire(t, "AppController::Start", controller,
		"cb.onCloseRequest = [this] { Shutdown(lifetime::Ending::CloseRequest); };")
	// the Quit, the session end (session_end_wiring_test.go) and this one
	if strings.Count(controller, "Shutdown(lifetime::Ending::") != 3 {
		t.Error("AppController.cpp ends the app from a place this contract does not know")
	}
	auth := definitionBody(t, "AppController.cpp", controller,
		"void AppController::OnAuthState(AuthState state, const std::string& error) {")
	quitForbid(t, "AppController::OnAuthState", auth, "signing out is not quitting", "Shutdown(", "sdk_.Quit(")

	logout := definitionBody(t, "SdkHost.cpp", sdkHostSource(t), "void SdkHost::Logout() {")
	quitForbid(t, "SdkHost::Logout", logout, "signing out is not quitting",
		"Quit(", "quitting_", "StopServiceWatchdog(", "StopProviderOnlyStats(")
}
