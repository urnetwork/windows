// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// A start connect waiting on its balance (App/StartConnectWait.h, owner report
// 2026-10-16) as a C++ spec, with negative controls that rewrite a copy of the
// header, and the SdkHost, AppController and ConnectPage lines that wire it,
// read with every comment blanked so prose cannot satisfy a contract.

// Compile the spec (app/tools/start-connect-wait-tests.cpp) against a copy of
// App/StartConnectWait.h. `mutate`, when set, rewrites the copy for a negative
// control.
func startConnectWaitProgram(t *testing.T, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("start connect wait tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "StartConnectWait.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := string(source)
	if mutate != nil {
		content = mutate(content)
		if content == string(source) {
			t.Fatal("negative control did not change the production StartConnectWait.h")
		}
	}
	if err := os.WriteFile(filepath.Join(includeDir, "StartConnectWait.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "start-connect-wait-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(root, "app", "tools", "start-connect-wait-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build start connect wait tests: %v\n%s", err, output)
	}
	return program
}

// The spec: a tap on a stale balance shows connecting at once, a repeated tap
// connects once, a disconnect or a sign-out during the wait ends it and nothing
// connects after, a refused start goes back before the upgrade path, and an
// admitted one keeps connecting into its connect.
func TestStartConnectWait(t *testing.T) {
	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="StartConnectWait.h" />`) {
		t.Error("App.vcxproj does not list StartConnectWait.h")
	}
	program := startConnectWaitProgram(t, nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("start connect wait: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run the spec against a rewritten copy of the header and require that it
// fails, naming want.
func requireStartConnectWaitFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	program := startConnectWaitProgram(t, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

func replaceStartConnectWaitOnce(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("StartConnectWait.h no longer has exactly one %q; update this control", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// The defect before this header: the store calls every settle it was handed,
// and each one asked the gate again, so a settle that ignores which wait it
// belongs to connects after the user's disconnect and after a sign-out.
func TestStartConnectWaitRejectsASettleThatIgnoresItsWait(t *testing.T) {
	requireStartConnectWaitFailure(t, func(source string) string {
		return replaceStartConnectWaitOnce(t, source,
			"if (!waiting_ || token != generation_) return false;",
			"(void)token;")
	}, "the start never connects after the user's disconnect")
}

// The same defect from the other side: three taps during one fetch connected
// three times.
func TestStartConnectWaitRejectsStackedTaps(t *testing.T) {
	requireStartConnectWaitFailure(t, func(source string) string {
		return replaceStartConnectWaitOnce(t, source,
			"if (!waiting_ || token != generation_) return false;",
			"if (token == 0) return false;")
	}, "three taps during one wait connect once")
}

// A cancel that leaves the wait standing lets a sign-out's own settle connect
// the signed-out account.
func TestStartConnectWaitRejectsACancelThatKeepsTheWait(t *testing.T) {
	requireStartConnectWaitFailure(t, func(source string) string {
		source = replaceStartConnectWaitOnce(t, source,
			"    waiting_ = false;\n    ++generation_;\n    return was;",
			"    return was;")
		return source
	}, "the start never connects after the sign-out")
}

// Before this header nothing showed while the start waited; a refused start
// that is never told Ended would keep connecting over the upgrade path.
func TestStartConnectWaitRejectsARefusalThatIsNotTold(t *testing.T) {
	requireStartConnectWaitFailure(t, func(source string) string {
		return replaceStartConnectWaitOnce(t, source,
			"change = admitted ? StartWaitChange::Admitted : StartWaitChange::Ended;\n    return true;",
			"change = StartWaitChange::Admitted;\n    return admitted;")
	}, "refused: back to the reported status before the upgrade path opens")
}

// A retry's own disconnect is part of the start asking again: ending it there
// steps the view back before the connect.
func TestStartConnectWaitRejectsARetryThatEndsItsOwnStart(t *testing.T) {
	requireStartConnectWaitFailure(t, func(source string) string {
		return replaceStartConnectWaitOnce(t, source,
			"    const bool was = waiting_;\n    waiting_ = false;",
			"    const bool was = waiting_ || settling_;\n    waiting_ = false;\n    settling_ = false;")
	}, "a retry's own disconnect does not end the start it is part of")
}

// The SdkHost lines the spec models.
func TestStartConnectWaitWiring(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))

	admit := functionBody(host, "bool SdkHost::AdmitStartConnect(")
	if admit == "" {
		t.Fatal("SdkHost::AdmitStartConnect is missing")
	}
	quitRequireSequence(t, "AdmitStartConnect's upgrade sink", admit,
		`TellStartWaitOutcome\(\s*false\)`, `startConnectUpgrade_\(again\)`)
	quitRequireSequence(t, "AdmitStartConnect's fetch sink", admit, `WaitForBalance\(std::move\(again\)\)`)
	quitForbid(t, "AdmitStartConnect", admit,
		"the store calls every settle it is handed, so the gesture must go through the wait's token",
		"startConnectFetchBalance_(std::move(again))")
	decided := admit[strings.Index(admit, "AdmitStartConnect(startConnectFacts_(), sinks)"):]
	quitRequireSequence(t, "AdmitStartConnect's admission", decided,
		`if \(admitted\) \{`, `TellStartWaitOutcome\(\s*true\)`, `connectAdmitted_\(\)`)

	wait := functionBody(host, "void SdkHost::WaitForBalance(")
	if wait == "" {
		t.Fatal("SdkHost::WaitForBalance is missing")
	}
	quitRequireSequence(t, "WaitForBalance", wait,
		`startWait_\.Begin\(\)`, `StartWaitChange::Waiting`, `startWait_\.Settle\(token\)`,
		`again\(\)`, `startWait_\.TakeUnanswered\(change\)`, `startConnectFetchBalance_\(std::move\(settled\)\)`)

	disconnect := functionBody(host, "void SdkHost::Disconnect(")
	quitRequireSequence(t, "SdkHost::Disconnect", disconnect,
		`CancelStartWait\("disconnect"\)`, `RequestSession\(`)

	logout := signOutSource(t, host)
	quitRequireSequence(t, "SdkHost::Logout", logout,
		`CancelStartWait\("signing out"\)`, `loggedIn_\.store\(false`, `SetAuthState\(AuthState::LoggedOut\)`)

	cancel := functionBody(host, "void SdkHost::CancelStartWait(")
	quitRequireSequence(t, "SdkHost::CancelStartWait", cancel,
		`if \(!startWait_\.Cancel\(\)\) return;`, `StartWaitChange::Ended`)
}

// The views: the window hears every change through the UI queue, and a window
// that shows later is told the wait that stands; the connect page overlays the
// wait on every push and goes back to the reported status when it ends.
func TestStartConnectWaitViewWiring(t *testing.T) {
	controller := stripComments(readAppSource(t, "AppController.cpp"))
	handler := handlerSource(t, "AppController.cpp", controller, "sdk_.SetStartWaitHandler(")
	quitRequireSequence(t, "the start wait handler", handler, `OnUi\(`, `OnStartWaitChanged\(change\)`)
	reconcile := functionBody(controller, "void AppController::ReconcileWindowPresentation(")
	quitRequireSequence(t, "ReconcileWindowPresentation", reconcile,
		`OnStartWaitChanged\(sdk_\.StartConnectWaiting\(\)`)

	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	apply := functionBody(page, "void ConnectPage::ApplyStats(")
	quitRequireSequence(t, "ConnectPage::ApplyStats", apply,
		`reportedConnectStatus_ = ParseConnectStatus\(stats\.connectionStatus\)`,
		`connectStatus_ = startWaiting_ \? ConnectStatus::Connecting : reportedConnectStatus_`,
		`reportedHealth_ = stats\.health`,
		`health_ = startWaiting_ \? urnw::health::State::Connecting : reportedHealth_`)
	changed := functionBody(page, "void ConnectPage::OnStartWaitChanged(")
	quitRequireSequence(t, "ConnectPage::OnStartWaitChanged", changed,
		`case urnw::StartWaitChange::Waiting:`, `startWaiting_ = true;`,
		`case urnw::StartWaitChange::Ended:`, `connectStatus_ = reportedConnectStatus_;`,
		`health_ = reportedHealth_;`, `ApplyConnectStatus\(\);`)

	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	forward := functionBody(window, "void MainWindow::OnStartWaitChanged(")
	quitRequireSequence(t, "MainWindow::OnStartWaitChanged", forward, `connect_->OnStartWaitChanged\(change\)`)
}
