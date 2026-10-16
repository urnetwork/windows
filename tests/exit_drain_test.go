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

// What an ending of the tray app does with the session worker's queued
// commands (Common/ExitDrain.h) as a C++ spec, with negative controls that
// rewrite a copy of the header, and its call sites in SdkHost and
// AppController, read with every comment blanked so prose cannot satisfy a
// contract. The defect (Apple parity, apple 57a39432 and 2bffa3c8): a
// Disconnect tapped while a pass was in flight was dropped by every ending,
// so after a close request the next launch reattached to a connection the
// user had ended; and Quit waited out that pass with no bound, on the UI
// thread, inside WM_ENDSESSION for a sign-out of Windows or a shutdown.

// Compile the exit-drain spec (app/tools/exit-drain-tests.cpp) against
// Common/ExitDrain.h. `mutate`, when set, rewrites a copy of the header for a
// negative control.
func exitDrainTestProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("exit drain tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	fixtureDir := t.TempDir()
	includeDir := filepath.Join(root, "app", "src", "Common")
	if mutate != nil {
		source, err := os.ReadFile(filepath.Join(includeDir, "ExitDrain.h"))
		if err != nil {
			t.Fatal(err)
		}
		changed := mutate(string(source))
		if changed == string(source) {
			t.Fatal("negative control did not change the production ExitDrain.h")
		}
		includeDir = fixtureDir
		if err := os.WriteFile(filepath.Join(includeDir, "ExitDrain.h"), []byte(changed), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(fixtureDir, "exit-drain-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
		"-I"+includeDir, filepath.Join(root, "app", "tools", "exit-drain-tests.cpp"),
		"-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build exit drain tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: the user's Disconnect queued behind a pass in flight runs
// before the exit, a connect queued for an exiting app is dropped, the wait
// ends at its cap with a pass still held, and the stop route and the host's
// destruction follow.
func TestExitDrain(t *testing.T) {
	if !strings.Contains(readCommonSource(t, "Common.vcxproj"), `<ClInclude Include="ExitDrain.h" />`) {
		t.Error("Common.vcxproj does not list ExitDrain.h")
	}
	program := exitDrainTestProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("exit drain: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming want.
func requireExitDrainFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := exitDrainTestProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The source with the one occurrence of old rewritten; the control fails when
// the header no longer has exactly one.
func replaceExitDrainOnce(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("ExitDrain.h no longer has exactly one %q; update this control", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// The defect itself: the ending empties the slot, the Disconnect with it.
func TestExitDrainRejectsDroppingTheDisconnect(t *testing.T) {
	requireExitDrainFailure(t, func(source string) string {
		return replaceExitDrainOnce(t, source,
			"constexpr bool KeepsQueued(Queued queued) { return queued == Queued::Disconnect; }",
			"constexpr bool KeepsQueued(Queued) { return false; }")
	}, "disconnect: the user's Disconnect runs after the pass in flight, before the exit")
}

// Nothing but the Disconnect may run for an exiting app.
func TestExitDrainRejectsKeepingAConnect(t *testing.T) {
	requireExitDrainFailure(t, func(source string) string {
		return replaceExitDrainOnce(t, source,
			"constexpr bool KeepsQueued(Queued queued) { return queued == Queued::Disconnect; }",
			"constexpr bool KeepsQueued(Queued) { return true; }")
	}, "connect: a connect queued for an exiting app never runs")
}

// The other half of the defect: the wait for a pass inside an rpc had no
// bound.
func TestExitDrainRejectsAnUnboundedWait(t *testing.T) {
	requireExitDrainFailure(t, func(source string) string {
		return replaceExitDrainOnce(t, source,
			"  return changed.wait_until(lock, deadline, [&slot] { return !slot.WorkerAlive(); });",
			"  (void)deadline;\n  changed.wait(lock, [&slot] { return !slot.WorkerAlive(); });\n  return true;")
	}, "cap: the wait returns at its deadline with the pass still held")
}

// A host destroyed under a pass still inside an rpc frees what the pass uses.
func TestExitDrainRejectsDestroyingABusyHost(t *testing.T) {
	requireExitDrainFailure(t, func(source string) string {
		return replaceExitDrainOnce(t, source,
			"  return commandsSettled && quitFinished;",
			"  return quitFinished || commandsSettled;")
	}, "host: a pass still inside an rpc keeps the host alive to the process exit")
}

// Quit keeps the user's Disconnect. It closes the slot to new requests, then
// on its own thread waits for what is queued, bounded, through the header's
// SettleQueued; it no longer empties the slot; and the worker tells the wait
// when it goes idle.
func TestExitDrainQuitKeepsTheUsersDisconnect(t *testing.T) {
	host := sdkHostSource(t)
	quit := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::Quit() {")
	quitForbid(t, "SdkHost::Quit", quit, "the user's Disconnect still queued runs before the exit",
		"pending_ = SessionRequest{};", "pendingRequested_ = false;")
	inOrder := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::QuitInOrder(std::chrono::steady_clock::time_point start) {")
	quitForbid(t, "SdkHost::QuitInOrder", inOrder, "the user's Disconnect still queued runs before the exit",
		"pending_ = SessionRequest{};", "pendingRequested_ = false;")
	quitRequireSequence(t, "SdkHost::Quit", quit,
		regexp.QuoteMeta("std::scoped_lock lock(pendingMutex_);"),
		regexp.QuoteMeta("quitting_.store(true);"),
		regexp.QuoteMeta("QuitInOrder(start);"))
	quitRequireSequence(t, "SdkHost::QuitInOrder", inOrder,
		regexp.QuoteMeta("const bool settled = SettleCommandsForExit(start + exitdrain::kCommandsCap);"),
		regexp.QuoteMeta("StopServiceWatchdog();"),
		regexp.QuoteMeta("std::scoped_lock lock(mutex_);"),
		regexp.QuoteMeta("service_.StopTunnel();"))

	settle := definitionBody(t, "SdkHost.cpp", host,
		"bool SdkHost::SettleCommandsForExit(std::chrono::steady_clock::time_point deadline) {")
	quitRequireSequence(t, "SdkHost::SettleCommandsForExit", settle,
		regexp.QuoteMeta("exitdrain::Queued Kind() { return QueuedOf(host.pending_.kind); }"),
		regexp.QuoteMeta("bool WorkerAlive() { return host.sessionWorkerAlive_; }"),
		regexp.QuoteMeta("std::unique_lock<std::mutex> lock(pendingMutex_);"),
		regexp.QuoteMeta("exitdrain::SettleQueued(slot, lock, pendingCv_, deadline);"))
	quitForbid(t, "SdkHost::SettleCommandsForExit", settle,
		"the wait is on the slot's lock alone; the session lock is what a held pass keeps",
		"mutex_)", "scoped_lock lock(mutex_")

	// the header maps the user's Disconnect as the one request kept
	header := stripComments(readAppSource(t, "SdkHost.h"))
	provideRequire(t, "SdkHost::QueuedOf", header,
		"case ConnectKind::Disconnect:    return exitdrain::Queued::Disconnect;",
		"case ConnectKind::BestAvailable: return exitdrain::Queued::Connect;",
		"case ConnectKind::Location:      return exitdrain::Queued::Connect;",
		"case ConnectKind::Provider:      return exitdrain::Queued::Provider;")

	worker := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::SessionWorkerLoop() {")
	quitRequireSequence(t, "SdkHost::SessionWorkerLoop's exit", worker,
		regexp.QuoteMeta("sessionWorkerAlive_ = false;"),
		regexp.QuoteMeta("pendingCv_.notify_all();"),
		regexp.QuoteMeta("return;"),
		regexp.QuoteMeta("req = std::move(pending_);"))
}

// Quit is bounded. The caller waits for its steps at most kQuitCap and never
// takes the session lock or joins a thread itself; a pass still holding that
// lock at the commands' cap does not hold the service stop, which goes
// without the lock before the joins.
func TestExitDrainQuitIsBounded(t *testing.T) {
	host := sdkHostSource(t)
	quit := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::Quit() {")
	quitRequireSequence(t, "SdkHost::Quit", quit,
		regexp.QuoteMeta("std::thread([this, completion, start] {"),
		regexp.QuoteMeta("QuitInOrder(start);"),
		regexp.QuoteMeta("completion->Finish();"),
		regexp.QuoteMeta("}).detach();"),
		regexp.QuoteMeta("completion->WaitUntil(start + exitdrain::kQuitCap)"))
	quitForbid(t, "SdkHost::Quit", quit,
		"the caller waits at most the quit's cap: a pass inside an rpc holds the lock and the joins",
		"scoped_lock lock(mutex_", "StopServiceWatchdog(", "StopPresentationWorker(",
		"StopSyncWatchdog(", "StopProviderOnlyStats(", "service_.")

	inOrder := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::QuitInOrder(std::chrono::steady_clock::time_point start) {")
	quitRequireSequence(t, "SdkHost::QuitInOrder", inOrder,
		regexp.QuoteMeta("SettleCommandsForExit(start + exitdrain::kCommandsCap)"),
		regexp.QuoteMeta("if (exitdrain::StopRouteFor(settled) == exitdrain::StopRoute::Unlocked) {"),
		regexp.QuoteMeta("StopServiceWithoutLock();"),
		regexp.QuoteMeta("StopServiceWatchdog();"),
		regexp.QuoteMeta("StopPresentationWorker();"),
		regexp.QuoteMeta("StopSyncWatchdog();"),
		regexp.QuoteMeta("StopProviderOnlyStats();"),
		regexp.QuoteMeta("std::scoped_lock lock(mutex_);"))

	unlocked := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::StopServiceWithoutLock() {")
	quitRequireSequence(t, "SdkHost::StopServiceWithoutLock", unlocked,
		regexp.QuoteMeta("if (!service_.IsConnected()) {"),
		regexp.QuoteMeta("service_.StopTunnel();"),
		regexp.QuoteMeta("AdoptServiceFacts(stopped);"),
		regexp.QuoteMeta("service_.StopProvider(&after, &error)"))
	quitForbid(t, "SdkHost::StopServiceWithoutLock", unlocked,
		"a pass holds the session lock; the pipe serializes the calls itself",
		"mutex_", "device_", "TeardownSessionLocked(", "service_.Connect(")
	pipe := stripComments(readCommonSource(t, "PipeClient.cpp"))
	call := definitionBody(t, "PipeClient.cpp", pipe,
		"nlohmann::json PipeClient::Call(const nlohmann::json& request, int timeoutMs) {")
	provideRequire(t, "PipeClient::Call", call, "std::scoped_lock callLock(callMutex_);")
}

// Every ending settles the queued commands. Quit and the end of the Windows
// session through Quit; a close request (taskkill, an installer, the Restart
// Manager) through the wait alone, the service left as it is; and a host that
// still has a pass or the quit running is left to the process exit, never
// destroyed under it.
func TestExitDrainEveryEndingSettles(t *testing.T) {
	controller := appControllerSource(t)
	shutdown := definitionBody(t, "AppController.cpp", controller,
		"void AppController::Shutdown(lifetime::Ending ending) {")
	quitRequireSequence(t, "AppController::Shutdown", shutdown,
		regexp.QuoteMeta("tray_.Destroy();"),
		regexp.QuoteMeta("if (window_) window_.Close();"),
		regexp.QuoteMeta("if (plan.stopService) sdk_.Quit();"),
		regexp.QuoteMeta("else sdk_.SettleCommandsForExit(std::chrono::steady_clock::now() + exitdrain::kCommandsCap);"),
		regexp.QuoteMeta("if (!sdk_.ExitSettled()) LeaveAppToProcessExit();"),
		regexp.QuoteMeta("if (auto app = Application::Current()) app.Exit();"))
	leave := definitionBody(t, "AppController.cpp", controller, "void LeaveAppToProcessExit() {")
	provideRequire(t, "LeaveAppToProcessExit", leave, "(void)g_app.release();")
	if strings.Count(controller, "LeaveAppToProcessExit()") != 2 {
		t.Error("AppController.cpp leaves the app to the process exit outside Shutdown's one check")
	}

	host := sdkHostSource(t)
	settled := definitionBody(t, "SdkHost.cpp", host, "bool SdkHost::ExitSettled() {")
	quitRequireSequence(t, "SdkHost::ExitSettled", settled,
		regexp.QuoteMeta("workerIdle = !sessionWorkerAlive_;"),
		regexp.QuoteMeta("const bool quitFinished = !quitCompletion_ || quitCompletion_->Finished();"),
		regexp.QuoteMeta("return exitdrain::HostDestroyableAtExit(workerIdle, quitFinished);"))
}
