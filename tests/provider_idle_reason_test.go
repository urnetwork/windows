// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// Why providing is enabled but idle (App/ProviderIdleReason.h): the C++ spec,
// and the Earnings line's wiring read from the app sources.

// Compile and execute the idle reason spec (App/ProviderIdleReason.h, support
// part P008): why providing is enabled but idle, in its rule order (Never and
// unknown modes, Network, Auto while disconnected, paused for Wi-Fi, paused,
// no traffic yet), the precedences, the control mode strings and the keys.
func TestProviderIdleReason(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("provider idle reason tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "provider-idle-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "provider-idle-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build provider idle reason tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("provider idle reason: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The line under the Earnings provide mode row, which cannot be built off
// Windows: WalletPage paints it from ProviderIdleReasonFor on every live update
// (ahead of the gate, which acts only when it changes) and on every provider
// distribution, with the desktop's any-network mode, merged with the server's
// reason (providerstatus::LineFor); the row sits right under the provide mode
// row with Change opening the Connect page as the row does; and the strings it
// shows are in the catalog.
func TestProviderIdleReasonWiring(t *testing.T) {
	root := repositoryRoot(t)
	wallet := stripLineComments(readAppSource(t, "WalletPage.cpp"))

	apply := definitionBody(t, "WalletPage.cpp", wallet, "void WalletPage::ApplyProvideState(urnw::LiveStats const& stats) {")
	read := strings.Index(apply, "Sdk().StoredProvideControlMode()")
	reason := strings.Index(apply, "ApplyProvideReason();")
	gate := regexp.MustCompile(`if \(enabled [!=]= providingEnabled_\)`).FindStringIndex(apply)
	switch {
	case read < 0 || reason < 0 || gate == nil:
		t.Error("WalletPage::ApplyProvideState no longer reads the control mode, paints the idle reason and gates the plots")
	case reason > gate[0]:
		t.Error("WalletPage::ApplyProvideState paints the idle reason behind the gate, so it would stop following the live stats")
	}
	if count := strings.Count(apply, "Sdk().StoredProvideControlMode()"); count != 1 {
		t.Errorf("WalletPage::ApplyProvideState reads the control mode %d times; it runs on every stats push, read the stored mode once", count)
	}
	for _, want := range []string{
		"provideControlMode_ = provideridle::ProvideControlModeFrom(controlMode);",
		"liveProvideMode_ = stats.provideMode;",
		"providePaused_ = stats.providePaused;",
	} {
		if !strings.Contains(apply[:reason+1], want) {
			t.Errorf("WalletPage::ApplyProvideState: %s must come before the idle reason is painted", want)
		}
	}

	paint := definitionBody(t, "WalletPage.cpp", wallet, "void WalletPage::ApplyProvideReason() {")
	for _, want := range []string{
		"provideridle::ProviderIdleReasonFor(",
		"provideControlMode_, liveProvideMode_, providePaused_",
		"provideridle::ProvideNetworkMode::All, providerWindowBytes_",
		"providerstatus::LineFor(",
		"w_.WalletProvideReasonText().Text(",
		"w_.WalletProvideReasonRow().Visibility(",
	} {
		if !strings.Contains(paint, want) {
			t.Errorf("WalletPage::ApplyProvideReason: missing %s", want)
		}
	}

	throughput := definitionBody(t, "WalletPage.cpp", wallet, "void WalletPage::ApplyProviderThroughput(urnw::ProviderThroughputSnapshot const& snapshot) {")
	bytes := strings.Index(throughput, "providerWindowBytes_ = snapshot.providerDistribution->byteCount;")
	repaint := strings.Index(throughput, "ApplyProvideReason();")
	if bytes < 0 || repaint < bytes {
		t.Error("WalletPage::ApplyProviderThroughput does not repaint the idle reason from the provider bytes in the window")
	}
	if !strings.Contains(definitionBody(t, "WalletPage.cpp", wallet, "void WalletPage::ApplyStrings() {"),
		`w_.WalletProvideReasonChange().Content(LocBox("change"));`) {
		t.Error("WalletPage::ApplyStrings does not label the idle reason's Change")
	}

	// Change does what the provide mode row does: it opens the Connect page,
	// whose provide group changes the mode
	window := stripLineComments(readAppSource(t, "MainWindow.xaml.cpp"))
	if !strings.Contains(definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::OnWalletProvideMode("),
		"HomeNav().SelectedItem(ConnectNavItem());") {
		t.Error("MainWindow::OnWalletProvideMode no longer opens the Connect page")
	}

	// the row: right under the provide mode row, ahead of the extender row
	markup := readAppSource(t, "MainWindow.xaml")
	row := regexp.MustCompile(`(?s)<Border x:Name="WalletProvideReasonRow"[^>]*Style="\{StaticResource UrPaneRowStyle\}"[^>]*>(.*?)</Border>`).FindStringSubmatch(markup)
	if row == nil {
		t.Fatal("MainWindow.xaml: no WalletProvideReasonRow pane row")
	}
	for _, want := range []*regexp.Regexp{
		regexp.MustCompile(`<TextBlock x:Name="WalletProvideReasonText"[^>]*Style="\{StaticResource UrRowNoteStyle\}"`),
		regexp.MustCompile(`<HyperlinkButton x:Name="WalletProvideReasonChange"[^>]*Click="OnWalletProvideMode"`),
	} {
		if !want.MatchString(row[1]) {
			t.Errorf("MainWindow.xaml: WalletProvideReasonRow is missing %s", want)
		}
	}
	modeRow := strings.Index(markup, `x:Name="WalletProvideModeButton"`)
	reasonRow := strings.Index(markup, `<Border x:Name="WalletProvideReasonRow"`)
	extenderRow := strings.Index(markup, `<Border x:Name="WalletExtenderRow"`)
	if modeRow < 0 || extenderRow < 0 || reasonRow < modeRow || extenderRow < reasonRow {
		t.Error("MainWindow.xaml: the idle reason row must sit between the provide mode row and the extender row")
	} else if between := markup[modeRow:reasonRow]; strings.Count(between, "<Border") != 0 {
		t.Error("MainWindow.xaml: the idle reason row must directly follow the provide mode row")
	}

	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="ProviderIdleReason.h" />`) {
		t.Error("App.vcxproj does not list ProviderIdleReason.h")
	}

	// the strings the line shows, as the spec words them
	for key, want := range map[string]string{
		"provider_idle_auto_not_connected": "Auto shares with everyone only while you're connected. Choose Always to earn while idle.",
		"provider_idle_network_only":       "Shared only with your own devices. Choose Always to share with everyone.",
		"provider_idle_paused_wifi_only":   "Paused: providing is set to Wi-Fi only, and this device isn't on Wi-Fi.",
		"provider_idle_paused_no_network":  "Paused: this device can't provide on its current network.",
		"provider_idle_no_traffic_yet":     "New providers need several hours of steady uptime and a speed test before clients are sent to them. Traffic also depends on demand in your region.",
		"change":                           "Change",
	} {
		if got := reswValue(t, root, "en", key); got != want {
			t.Errorf("en resw %s = %q, want %q", key, got, want)
		}
	}
}
