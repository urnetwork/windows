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

// How the app publishes and applies live stats snapshots: read on a worker of
// their own, never on the caller's thread (App/CoalescedWorker.h), and applied
// in the order their reads began (App/StatsReadOrder.h), as a C++ spec with
// negative controls that rewrite a copy of a header; and the wiring in
// SdkHost, AppController and ConnectPage, which need Windows and WinRT, read
// with every comment blanked so prose cannot satisfy a contract.

// The headers the spec compiles against.
var statsPublishHeaders = []string{"StatsReadOrder.h", "CoalescedWorker.h"}

// Compile the spec (app/tools/stats-publish-tests.cpp) against copies of its
// headers. `mutate`, when set, rewrites the copy of the header `name`.
func statsPublishTestProgram(t *testing.T, name string, mutate func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("stats publish tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for _, header := range statsPublishHeaders {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", header))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if mutate != nil && header == name {
			content = mutate(content)
			if content == string(source) {
				t.Fatalf("negative control did not change the production %s", header)
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, header), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "stats-publish-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
		"-I"+includeDir, filepath.Join(root, "app", "tools", "stats-publish-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build stats publish tests: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: a stats read runs on the publish worker, requests during
// one fold into one more, nothing runs after Stop, and snapshots apply in the
// order their reads began.
func TestStatsPublish(t *testing.T) {
	vcxproj := readAppSource(t, "App.vcxproj")
	for _, header := range statsPublishHeaders {
		if !strings.Contains(vcxproj, `<ClInclude Include="`+header+`" />`) {
			t.Errorf("App.vcxproj does not list %s", header)
		}
	}
	program := statsPublishTestProgram(t, "", nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("stats publish: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run the spec against a rewritten copy of `name` and require that it fails,
// naming want.
func requireStatsPublishFailure(t *testing.T, name string, mutate func(string) string, want string) {
	t.Helper()
	program := statsPublishTestProgram(t, name, mutate)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The source with the one occurrence of old rewritten; the control fails when
// the header no longer has exactly one.
func replaceStatsPublishOnce(t *testing.T, source, old, replacement string) string {
	t.Helper()
	if strings.Count(source, old) != 1 {
		t.Fatalf("the header no longer has exactly one %q; update this control", old)
	}
	return strings.Replace(source, old, replacement, 1)
}

// The defect: the read ran on the caller's thread, the UI thread on a window
// show and on the health re-check, and waited there on the device's lock.
func TestCoalescedWorkerRejectsRunningOnTheCaller(t *testing.T) {
	requireStatsPublishFailure(t, "CoalescedWorker.h", func(source string) string {
		return replaceStatsPublishOnce(t, source, "    thread_ = std::thread([this] { Loop(); });\n",
			"    thread_ = std::thread([] {});\n    running_ = false;\n    dirty_ = false;\n    job_();\n")
	}, "the job ran on the caller's thread")
}

// A request while a read waits on a getter must still be read after it, or the
// window keeps the state from before the request.
func TestCoalescedWorkerRejectsLosingARequestDuringARun(t *testing.T) {
	requireStatsPublishFailure(t, "CoalescedWorker.h", func(source string) string {
		return replaceStatsPublishOnce(t, source, "    dirty_ = true;\n    if (running_) return;\n",
			"    if (running_) return;\n    dirty_ = true;\n")
	}, "a request during a run was lost")
}

// After Stop the owner tears down what the job reads. The control is a Stop
// that only joins: it neither refuses the next request nor drops a pending one.
func TestCoalescedWorkerRejectsRunningAfterStop(t *testing.T) {
	requireStatsPublishFailure(t, "CoalescedWorker.h", func(source string) string {
		source = replaceStatsPublishOnce(t, source, "    if (stopped_) return;\n", "")
		source = replaceStatsPublishOnce(t, source, "      stopped_ = true;\n      dirty_ = false;\n",
			"      stopped_ = true;\n")
		return replaceStatsPublishOnce(t, source, "if (stopped_ || !dirty_) {", "if (!dirty_) {")
	}, "a request after Stop ran the job against a stopped owner")
}

// A getter that throws must not take the worker, or the process, down.
func TestCoalescedWorkerRejectsDyingOnAThrow(t *testing.T) {
	requireStatsPublishFailure(t, "CoalescedWorker.h", func(source string) string {
		source = replaceStatsPublishOnce(t, source, "      try {\n        job_();\n      } catch (...) {\n      }\n",
			"      job_();\n")
		return source
	}, "a getter failed")
}

// No UI-thread path reads a stats snapshot: the window's show resync and the
// health re-check ask SdkHost's publish worker, the inspector reads the last
// applied push, and the publish worker stops above mutex_ in Quit and in the
// destructor, before what it reads is torn down.
func TestStatsReadsRunOffTheUiThread(t *testing.T) {
	controller := stripComments(readAppSource(t, "AppController.cpp"))
	reconcile := definitionBody(t, "AppController.cpp", controller,
		"void AppController::ReconcileWindowPresentation()")
	if !strings.Contains(reconcile, "sdk_.RepublishStats();") {
		t.Error("ReconcileWindowPresentation does not ask for the show's stats snapshot off the UI thread")
	}
	for _, name := range []string{"AppController.cpp", "ConnectPage.cpp", "MainWindow.xaml.cpp",
		"LocationSheets.cpp", "TrayIcon.cpp", "SettingsPage.cpp", "AccountPage.cpp", "WalletPage.cpp"} {
		source := stripComments(readAppSource(t, name))
		for _, read := range []string{"CurrentStats(", "ReadStats(", "PublishStats();"} {
			if strings.Contains(source, read) {
				t.Errorf("%s reads a stats snapshot on the UI thread (%s)", name, read)
			}
		}
	}

	header := stripComments(readAppSource(t, "SdkHost.h"))
	if !strings.Contains(header, "void RepublishStats() { statsPublisher_.Request(); }") {
		t.Error("RepublishStats does not hand the read to the publish worker")
	}
	if strings.Contains(header, "CurrentStats(") {
		t.Error("SdkHost still offers a synchronous stats read (CurrentStats)")
	}
	if !strings.Contains(header, "CoalescedWorker statsPublisher_{[this] { PublishStatsOnWorker(); }};") {
		t.Error("the publish worker does not run PublishStatsOnWorker")
	}

	sdkHost := stripComments(readAppSource(t, "SdkHost.cpp"))
	onWorker := definitionBody(t, "SdkHost.cpp", sdkHost, "void SdkHost::PublishStatsOnWorker()")
	quitRequireSequence(t, "PublishStatsOnWorker", onWorker, `try \{`, `PublishStats\(\);`, `catch`)
	destructor := definitionBody(t, "SdkHost.cpp", sdkHost, "SdkHost::~SdkHost()")
	quitRequireSequence(t, "SdkHost's destructor", destructor,
		`statsPublisher_\.Stop\(\);`, `std::scoped_lock lock\(mutex_\);`)
	// Quit's steps run on its own thread (ExitDrain.h): the worker stops with
	// the other joins, inside the quit's bound, before the locked teardown
	quit := definitionBody(t, "SdkHost.cpp", sdkHost, "void SdkHost::QuitInOrder(")
	quitRequireSequence(t, "SdkHost::QuitInOrder", quit,
		`StopProviderOnlyStats\(\);`, `statsPublisher_\.Stop\(\);`, `std::scoped_lock lock\(mutex_\);`)
	connectJson := definitionBody(t, "SdkHost.cpp", sdkHost,
		"void SdkHost::Connect(const std::string& connectLocationJson)")
	if strings.Contains(connectJson, "PublishStats();") {
		t.Error("Connect's parse failure reads a stats snapshot on the caller's thread")
	}
}

// The defect: every snapshot was applied in the order it arrived, so a read
// that waited on a getter rolled the window back when it finished.
func TestStatsReadOrderRejectsApplyingWhateverArrives(t *testing.T) {
	requireStatsPublishFailure(t, "StatsReadOrder.h", func(source string) string {
		return replaceStatsPublishOnce(t, source, "if (readSequence <= applied_) return false;", "")
	}, "the older read rolled the snapshot back")
}

// A repeated snapshot is not newer than itself.
func TestStatsReadOrderRejectsApplyingASnapshotTwice(t *testing.T) {
	requireStatsPublishFailure(t, "StatsReadOrder.h", func(source string) string {
		return replaceStatsPublishOnce(t, source, "readSequence <= applied_", "readSequence < applied_")
	}, "the same snapshot was applied twice")
}

// An order that forgets what it applied cannot tell an older read.
func TestStatsReadOrderRejectsForgettingTheAppliedRead(t *testing.T) {
	requireStatsPublishFailure(t, "StatsReadOrder.h", func(source string) string {
		return replaceStatsPublishOnce(t, source, "    applied_ = readSequence;\n", "")
	}, "the older read rolled the snapshot back")
}

// The text of `pattern`'s first match in text, or -1.
func statsIndex(text, pattern string) int {
	if location := regexp.MustCompile(pattern).FindStringIndex(text); location != nil {
		return location[0]
	}
	return -1
}

// Each read takes its sequence before it reads anything, and the UI applies a
// snapshot only when it is newer than every one it applied, before anything
// else in OnStats acts on it.
func TestStatsSnapshotsApplyInReadOrder(t *testing.T) {
	sdkHost := stripComments(readAppSource(t, "SdkHost.cpp"))
	read := definitionBody(t, "SdkHost.cpp", sdkHost, "LiveStats SdkHost::ReadStats()")
	sequence := statsIndex(read, `s\.readSequence = statsReadSequence_\.fetch_add\(`)
	if sequence < 0 {
		t.Fatal("ReadStats does not stamp its snapshot with a read sequence")
	}
	for _, firstRead := range []string{`service_\.IsConnected\(\)`, `connectVc_->`, `device_->`} {
		if at := statsIndex(read, firstRead); at >= 0 && at < sequence {
			t.Errorf("ReadStats reads %s before it takes its read sequence", firstRead)
		}
	}

	controller := stripComments(readAppSource(t, "AppController.cpp"))
	onStats := definitionBody(t, "AppController.cpp", controller, "void AppController::OnStats(const LiveStats& stats)")
	admit := statsIndex(onStats, `if \(!statsReadOrder_\.Admit\(stats\.readSequence\)\) return;`)
	if admit < 0 {
		t.Fatal("AppController::OnStats applies a snapshot whatever its read order")
	}
	for _, state := range []string{`trayHealth_`, `rawInsufficientBalance_`, `connectRequested_`, `OnStatsChanged`} {
		if at := statsIndex(onStats, state); at < 0 || at < admit {
			t.Errorf("AppController::OnStats acts on %s before it drops an older snapshot", state)
		}
	}
}

// Compile the peer location spec (app/tools/peer-location-tests.cpp) against a
// copy of PeerLocation.h rewritten by mutate, run it on the app directory, and
// require that it fails, naming want.
func requirePeerLocationFailure(t *testing.T, mutate func(string) string, want string) {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("peer location tests require a C++20 compiler: ", err)
	}
	appDir := filepath.Join(repositoryRoot(t), "app")
	source, err := os.ReadFile(filepath.Join(appDir, "src", "App", "PeerLocation.h"))
	if err != nil {
		t.Fatal(err)
	}
	content := mutate(string(source))
	if content == string(source) {
		t.Fatal("negative control did not change the production PeerLocation.h")
	}
	includeDir := t.TempDir()
	if err := os.WriteFile(filepath.Join(includeDir, "PeerLocation.h"), []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	program := filepath.Join(includeDir, "peer-location-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+includeDir,
		filepath.Join(appDir, "tools", "peer-location-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build peer location tests: %v\n%s", err, output)
	}
	output, err := exec.Command(program, appDir).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The label names only a peer the peer list would name: connected and
// providing, as PeerViewController.getPeers filters them.
func TestSelectedLocationNameRejectsAPeerThatDoesNotProvide(t *testing.T) {
	requirePeerLocationFailure(t, func(source string) string {
		return replaceStatsPublishOnce(t, source,
			"if (peer.ProvideEnabled && peer.ClientId && *peer.ClientId == *clientId) {",
			"if (peer.ClientId && *peer.ClientId == *clientId) {")
	}, "a peer that does not provide is not one the peer list names")
}

// A selected peer is named after its device, not the name it was connected
// under.
func TestSelectedLocationNameRejectsKeepingTheConnectedName(t *testing.T) {
	requirePeerLocationFailure(t, func(source string) string {
		return replaceStatsPublishOnce(t, source, "      return PeerDisplayName(peer);\n", "      return name;\n")
	}, "the drawer does not name the selected peer after its device")
}

// A stats push makes no SdkHost read on the UI thread: ConnectPage::ApplyStats
// renders the snapshot's fields, the selected peer's name and the peers line's
// two readings among them, which ReadStats takes off the UI thread. Before,
// ApplyStats took mutex_ three or four times per push (SelectedLocation,
// ConnectedProvidePeers, and through ApplyPeerCount RemoteConnected and
// ConnectedPeerCount), and mutex_ is held by the session worker through a
// whole connect.
func TestStatsPushReadsNothingOnTheUiThread(t *testing.T) {
	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	apply := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::ApplyStats(urnw::LiveStats const& stats)")
	if strings.Contains(apply, "Sdk()") {
		t.Error("ConnectPage::ApplyStats reads SdkHost on the UI thread")
	}
	if statsIndex(apply, `ApplyPeerCount\(std::nullopt,\s*PeerLineReading\{\.remoteConnected = stats\.remoteConnected,\s*\.connectedPeerCount = stats\.connectedPeerCount\}\);`) < 0 {
		t.Error("ConnectPage::ApplyStats does not hand the peers line the snapshot's readings")
	}
	// the line keeps the snapshot's reading for the pushes that carry none,
	// and reads no getter of its own (drawer_push_reads_test.go)
	peerCount := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::ApplyPeerCount(")
	if !strings.Contains(peerCount, "if (reading) peerLine_ = *reading;") {
		t.Error("ApplyPeerCount does not keep a stats push's reading")
	}
	if strings.Contains(peerCount, "Sdk()") {
		t.Error("ApplyPeerCount reads SdkHost on the UI thread")
	}

	sdkHost := stripComments(readAppSource(t, "SdkHost.cpp"))
	read := definitionBody(t, "SdkHost.cpp", sdkHost, "LiveStats SdkHost::ReadStats()")
	for _, pattern := range []string{
		`s\.locationName = SelectedLocationName\(\s*std::move\(s\.locationName\), connectVc_ \? connectVc_->getSelectedLocation\(\) : loc,\s*connectedPeers\);`,
		`s\.remoteConnected = device_->getRemoteConnected\(\);`,
		`s\.connectedPeerCount = `,
	} {
		if statsIndex(read, pattern) < 0 {
			t.Errorf("ReadStats does not read the drawer's %s", pattern)
		}
	}
}
