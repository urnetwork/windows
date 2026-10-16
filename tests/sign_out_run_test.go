// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// A sign-out on a thread of its own (App/SignOutRun.h, owner report
// 2026-10-16) as a C++ spec with negative controls, and the SdkHost,
// AppController and SettingsPage lines that wire it, read with every comment
// blanked so prose cannot satisfy a contract.

func signOutRunProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("sign-out run tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "SignOutRun.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(source)
	if mutate != nil {
		content = mutate(content)
		if content == string(source) {
			t.Fatal("negative control did not change the production SignOutRun.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "SignOutRun.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "sign-out-run-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "sign-out-run-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build sign-out run tests: %v\n%s", err, output)
	}
	return program
}

// One sign-out at a time; a sign-in during the tail waits for it and runs
// after it; a registration from before the sign-out installs nothing; an exit
// waits for the tail, bounded.
func TestSignOutRun(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="SignOutRun.h" />`) {
		t.Error("App.vcxproj does not list SignOutRun.h")
	}
	if output, err := exec.Command(signOutRunProgram(t, nil)).CombinedOutput(); err != nil {
		t.Fatalf("sign-out run: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

func requireSignOutRunFailure(t *testing.T, old, replacement, want string) {
	t.Helper()
	program := signOutRunProgram(t, func(source string) string {
		if strings.Count(source, old) != 1 {
			t.Fatalf("SignOutRun.h no longer has exactly one %q; update this control", old)
		}
		return strings.Replace(source, old, replacement, 1)
	})
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// Two clicks, two tails: the second signed out whatever had signed in between.
func TestSignOutRunRejectsASecondSignOut(t *testing.T) {
	requireSignOutRunFailure(t, "    if (running_) return false;\n    running_ = true;",
		"    running_ = true;", "a second click joins the sign-out running")
}

// The defect a tail off the UI thread would bring without the wait: a sign-in
// installs its credential and the tail's logout then clears it.
func TestSignOutRunRejectsASignInThatDoesNotWait(t *testing.T) {
	requireSignOutRunFailure(t, "    if (!running_) return false;\n    replaced =",
		"    (void)signIn;\n    (void)replaced;\n    return false;\n    replaced =",
		"the account that signed in after the sign-out began is not signed out")
}

// A registration that began before the click finishes after it and signs the
// app back in.
func TestSignOutRunRejectsAnEpochThatDoesNotMove(t *testing.T) {
	requireSignOutRunFailure(t, "    running_ = true;\n    ++epoch_;", "    running_ = true;",
		"a registration that began before the sign-out does not install its credential")
}

// The sign-in that waited is never run.
func TestSignOutRunRejectsATailThatDropsTheWaitingSignIn(t *testing.T) {
	requireSignOutRunFailure(t, "next = std::exchange(deferred_, std::nullopt);",
		"deferred_.reset();", "the sign-in runs after the tail's logout and signed-out state")
}

// An older sign-in waiting is dropped without being told.
func TestSignOutRunRejectsAReplacedSignInThatIsNotTold(t *testing.T) {
	requireSignOutRunFailure(t, "replaced = std::exchange(deferred_, std::move(signIn));",
		"(void)replaced;\n    deferred_ = std::move(signIn);", "an older sign-in waiting is told it was replaced")
}

// The sign-out as it runs: Logout's click, then the tail and its locked
// steps, in that order, comments blanked. The contracts that pinned Logout's
// steps before the tail moved off the UI thread read this.
func signOutSource(t *testing.T, host string) string {
	t.Helper()
	var parts []string
	for _, signature := range []string{"void SdkHost::Logout(", "void SdkHost::SignOutTail(",
		"void SdkHost::SignOutTailLocked("} {
		body := functionBody(host, signature)
		if body == "" {
			t.Fatalf("SdkHost.cpp no longer has %q; update this contract", signature)
		}
		parts = append(parts, body)
	}
	return strings.Join(parts, "\n")
}

// The SdkHost lines the spec models, and the exits.
func TestSignOutRunWiring(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))

	logout := functionBody(host, "void SdkHost::Logout(")
	if logout == "" {
		t.Fatal("SdkHost::Logout is missing")
	}
	quitRequireSequence(t, "SdkHost::Logout", logout,
		`if \(!signOutRun_\.Begin\(\)\)`, `CancelPendingWalletFlows\(`, `CancelStartWait\(`,
		`pending_ = SessionRequest\{\};`, `loggedIn_\.store\(false`, `onSignOutProgress_\(true\)`,
		`signOutWorker_\.Request\(\)`)
	quitForbid(t, "SdkHost::Logout", logout,
		"the click runs on the UI thread, and the session lock and the pipe can each wait out an rpc timeout",
		"scoped_lock lock(mutex_", "signOut_.Begin(", "device_->close()", "TeardownSessionLocked(")

	tail := functionBody(host, "void SdkHost::SignOutTail(")
	quitRequireSequence(t, "SdkHost::SignOutTail", tail,
		`std::scoped_lock lock\(mutex_\)`, `SignOutTailLocked\(\)`, `signOutRun_\.Finish\(\)`,
		`onSignOutProgress_\(false\)`, `RegisterNetworkClient\(next->byJwt`)
	tailLocked := functionBody(host, "void SdkHost::SignOutTailLocked(")
	quitRequireSequence(t, "SdkHost::SignOutTailLocked", tailLocked,
		`asyncLocalState_->logout\(`, `api_->setByJwt\(""\)`, `signOut_\.Begin\(SignOutServiceLocked\(\)\)`,
		`device_->close\(\)`, `TeardownSessionLocked\(`, `SetAuthState\(AuthState::LoggedOut\)`)

	register := functionBody(host, "void SdkHost::RegisterNetworkClient(")
	quitRequireSequence(t, "SdkHost::RegisterNetworkClient", register,
		`signOutRun_\.Defer\(DeferredSignIn\{byJwt, done\}, replaced\)`, `return;`,
		`const std::uint64_t signOutEpoch = signOutRun_\.Epoch\(\);`, `api_->setByJwt\(byJwt\)`,
		`std::scoped_lock lock\(mutex_\);\s*signedOutMeanwhile = signOutRun_\.Epoch\(\) != signOutEpoch;`,
		`localState_->setByJwt\(byJwt\)`, `if \(signedOutMeanwhile\)`, `SetAuthState\(AuthState::LoggedIn\)`)

	network := functionBody(host, "bool SdkHost::ApplyNetworkServer(")
	quitRequireSequence(t, "SdkHost::ApplyNetworkServer", network,
		`CancelPendingWalletFlows\(`, `if \(signOutRun_\.Running\(\)\)`, `std::scoped_lock lock\(mutex_\)`)

	quit := functionBody(host, "void SdkHost::QuitInOrder(")
	quitRequireSequence(t, "SdkHost::QuitInOrder", quit,
		`signOutWorker_\.Stop\(\);`, `std::scoped_lock lock\(mutex_\);`)
	destructor := functionBody(host, "SdkHost::~SdkHost()")
	quitRequireSequence(t, "SdkHost's destructor", destructor,
		`signOutWorker_\.Stop\(\);`, `std::scoped_lock lock\(mutex_\);`)
	settled := functionBody(host, "bool SdkHost::ExitSettled(")
	quitRequireSequence(t, "SdkHost::ExitSettled", settled,
		`if \(signOutRun_\.Running\(\)\) workerIdle = false;`,
		`return exitdrain::HostDestroyableAtExit\(workerIdle, quitFinished\);`)
}

// The window shows the sign-out running, the tray offers no action meanwhile,
// and an exit waits for the tail.
func TestSignOutRunViewWiring(t *testing.T) {
	controller := stripComments(readAppSource(t, "AppController.cpp"))
	handler := handlerSource(t, "AppController.cpp", controller, "sdk_.SetSignOutProgressHandler(")
	quitRequireSequence(t, "the sign-out progress handler", handler,
		`OnUi\(`, `UpdateTray\(\)`, `SetSigningOut\(signingOut\)`)
	reconcile := functionBody(controller, "void AppController::ReconcileWindowPresentation(")
	quitRequireSequence(t, "ReconcileWindowPresentation", reconcile, `SetSigningOut\(sdk_\.SigningOut\(\)\)`)
	facts := functionBody(controller, "tray::Facts AppController::CurrentTrayFacts(")
	quitRequireSequence(t, "AppController::CurrentTrayFacts", facts,
		`facts\.signedIn = authState_ == AuthState::LoggedIn && sdk_\.IsLoggedIn\(\);`)
	shutdown := functionBody(controller, "void AppController::Shutdown(")
	quitRequireSequence(t, "AppController::Shutdown", shutdown,
		`else sdk_\.SettleCommandsForExit\(`, `if \(!plan\.stopService\) \{\s*sdk_\.SettleSignOutForExit\(`,
		`if \(!sdk_\.ExitSettled\(\)\) LeaveAppToProcessExit\(\);`)

	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	forward := functionBody(window, "void MainWindow::SetSigningOut(")
	quitRequireSequence(t, "MainWindow::SetSigningOut", forward, `settings_->SetSigningOut\(signingOut\)`)

	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	danger := functionBody(settings, "void SettingsPage::BuildDangerSection(")
	quitRequireSequence(t, "SettingsPage::BuildDangerSection", danger,
		`signOutRow_ = signOut;`, `signOutRow_\.IsEnabled\(!signingOut_\);`)
	set := functionBody(settings, "void SettingsPage::SetSigningOut(")
	quitRequireSequence(t, "SettingsPage::SetSigningOut", set,
		`signingOut_ = signingOut;`, `signOutRow_\.IsEnabled\(!signingOut\)`)
}
