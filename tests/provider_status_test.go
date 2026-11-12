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

// The Earnings provider status (App/ProviderStatusPresentation.h): the C++
// spec, alone and against the generated sdk header, the page's wiring read
// from the app sources, and negative controls on the readings.

// Compile the provider status spec (app/tools/provider-status-tests.cpp,
// header-only). extra adds compiler arguments ahead of the source.
func buildProviderStatusTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("provider status tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "provider-status-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(root, "app", "src", "App")}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(root, "app", "tools", "provider-status-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build provider status tests: %v\n%s", err, output)
	}
	return program
}

// Run a built spec and log what it printed.
func runProviderStatusTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("provider status: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Compile and execute the provider status spec
// (App/ProviderStatusPresentation.h, support part P008 phase 2): the Demand
// histogram's 60 bars, total and empty state, the states table, the line under
// the provide mode row (local reason first, then the server's, then "no
// traffic yet", an unknown code's English), every Why? row, and the percent.
func TestProviderStatusPresentation(t *testing.T) {
	runProviderStatusTests(t, buildProviderStatusTests(t))
}

// The same spec built against the generated SDK header, so the templates are
// instantiated with urnet::ProviderStatus, ProviderRankingNumber and
// ProviderStatusCountry, a status parsed through the header's json goes
// through the page model, and the sdk's reason and number constants match the
// presentation's. The header is git-ignored (fetch-deps unpacks it into
// app/third_party/urnetwork-sdk/<arch>; URNETWORK_SDK_INCLUDE names another
// directory) and needs nlohmann/json, so a host without them skips this, and
// so does a copy from before the provider status API.
func TestProviderStatusPresentationAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := providerStatusSdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the provider status API (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_PROVIDER_STATUS_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runProviderStatusTests(t, buildProviderStatusTests(t, extra...))
}

// The directory of a urnetwork_sdk.hpp that has the provider status API, or "".
func providerStatusSdkHeaderDir(t *testing.T, root string) string {
	t.Helper()
	explicit := os.Getenv("URNETWORK_SDK_INCLUDE")
	candidates := []string{}
	if explicit != "" {
		candidates = append(candidates, explicit)
	}
	for _, arch := range []string{"amd64", "arm64"} {
		candidates = append(candidates, filepath.Join(root, "app", "third_party", "urnetwork-sdk", arch))
	}
	for _, dir := range candidates {
		header, err := os.ReadFile(filepath.Join(dir, "urnetwork_sdk.hpp"))
		if err != nil {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: %v", explicit, err)
			}
			continue
		}
		if !strings.Contains(string(header), "class ProviderStatusViewController final") {
			if dir == explicit {
				t.Fatalf("URNETWORK_SDK_INCLUDE=%s: urnetwork_sdk.hpp has no provider status API", explicit)
			}
			t.Logf("%s: urnetwork_sdk.hpp predates the provider status API", dir)
			continue
		}
		return dir
	}
	return ""
}

// The controller and the page, which cannot be built off Windows: the Earnings
// page opens the SDK's ProviderStatusViewController on the device once the
// destination shows with providing enabled, reads it on the UI thread, polls
// only while the destination shows and the window presents, and closes it with
// the typed close after dropping its listener; Demand and Why? sit right after
// the Blocked plot under the plots' own gate; and every string they show is in
// the catalog.
func TestProviderStatusWiring(t *testing.T) {
	root := repositoryRoot(t)
	wallet := stripLineComments(readAppSource(t, "WalletPage.cpp"))
	body := func(signature string) string {
		return definitionBody(t, "WalletPage.cpp", wallet, signature)
	}
	requireAll := func(name, source string, wants ...string) {
		t.Helper()
		for _, want := range wants {
			if !strings.Contains(source, want) {
				t.Errorf("%s: missing %s", name, want)
			}
		}
	}
	requireOrder := func(name, source, first, second string) {
		t.Helper()
		a, b := strings.Index(source, first), strings.Index(source, second)
		if a < 0 || b < 0 || b < a {
			t.Errorf("%s: %s must come before %s", name, first, second)
		}
	}

	// opened on the device, its listener marshalled to the UI thread
	open := body("void WalletPage::OpenProviderStatus(uint64_t device) {")
	requireAll("WalletPage::OpenProviderStatus", open,
		"Sdk().device().openProviderStatusViewController()",
		"providerStatusVc_->addProviderStatusListener(",
		"queue.TryEnqueue(",
		"if (!*alive) return;",
		"self->wallet().ReadProviderStatus();")

	// closed with the typed close, the listener dropped first, on the device
	// that opened it; never the generic close
	closeBody := body("void WalletPage::CloseProviderStatus(bool deviceAlive) {")
	requireAll("WalletPage::CloseProviderStatus", closeBody,
		"providerStatusSub_.reset();",
		"Sdk().device().handle() == providerStatusVcDevice_",
		"Sdk().device().closeProviderStatusViewController(*providerStatusVc_);",
		"providerStatusVc_.reset();")
	requireOrder("WalletPage::CloseProviderStatus", closeBody,
		"providerStatusSub_.reset();", "closeProviderStatusViewController(")
	for _, generic := range []string{"providerStatusVc_->close()", "closeViewController("} {
		if strings.Contains(closeBody, generic) {
			t.Errorf("WalletPage::CloseProviderStatus uses %s; the generic close cannot release the controller from the device", generic)
		}
	}
	if strings.Count(wallet, "closeProviderStatusViewController(") != 1 {
		t.Error("WalletPage.cpp closes the provider status controller outside CloseProviderStatus")
	}
	if strings.Contains(closeBody, "w_.") {
		t.Error("WalletPage::CloseProviderStatus touches XAML; the destructor calls it")
	}
	requireAll("WalletPage::~WalletPage", body("WalletPage::~WalletPage() {"), "CloseProviderStatus(/*deviceAlive=*/true);")

	// opened while providing is enabled and the destination shows, closed
	// when providing turns off or the device changes, polling while it shows
	reconcile := body("void WalletPage::ReconcileProviderStatus() {")
	requireAll("WalletPage::ReconcileProviderStatus", reconcile,
		"!providingEnabled_ || handle != providerStatusVcDevice_",
		"CloseProviderStatus(/*deviceAlive=*/true);",
		"provideStateKnown_ && providingEnabled_ && selected_ &&",
		"OpenProviderStatus(handle);",
		"selected_ && presentationActive_",
		"providerStatusVc_->start();",
		"providerStatusVc_->stop();")
	requireAll("WalletPage::SetSelected", body("void WalletPage::SetSelected(bool selected) {"), "ReconcileProviderStatus();")
	requireAll("WalletPage::SetPresentationActive", body("void WalletPage::SetPresentationActive(bool active) {"),
		"presentationActive_ = active;", "ReconcileProviderStatus();")
	apply := body("void WalletPage::ApplyProvideState(urnw::LiveStats const& stats) {")
	requireAll("WalletPage::ApplyProvideState", apply, "provideStateKnown_ = true;")
	gate := regexp.MustCompile(`if \(enabled [!=]= providingEnabled_\)`).FindStringIndex(apply)
	reconciled := strings.LastIndex(apply, "ReconcileProviderStatus();")
	if gate == nil || reconciled < gate[1] {
		t.Error("WalletPage::ApplyProvideState does not reconcile the provider status controller after the gate")
	}
	window := stripLineComments(readAppSource(t, "MainWindow.xaml.cpp"))
	requireAll("MainWindow::OnNavSelectionChanged",
		definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::OnNavSelectionChanged("),
		`wallet_->SetSelected(tag == L"wallet");`)

	// the controller's readings, then the page model
	requireAll("WalletPage::ReadProviderStatus", body("void WalletPage::ReadProviderStatus() {"),
		"providerStatusVc_->getIsLoaded()",
		"providerStatusVc_->getLastFetchError()",
		"providerStatusVc_->getProviderStatus()",
		"ApplyProviderStatus();")
	requireAll("WalletPage::ProviderStatusView", body("providerstatus::View WalletPage::ProviderStatusView() const {"),
		"providerstatus::ViewFor(providerStatusLoaded_",
		"(!providerStatusVc_ && !providerOnlySource_) ||",
		"!providerStatusError_.empty()")
	render := body("void WalletPage::ApplyProviderStatus() {")
	requireAll("WalletPage::ApplyProviderStatus", render,
		"statsSections_ && statsSections_->providerVisible",
		"w_.WalletProviderDemandRow().Visibility(shown(plots && sections.area != DemandArea::Hidden));",
		"w_.WalletProviderWhyRow().Visibility(shown(plots && sections.why));",
		`Loc("loading")`,
		`Loc("provider_status_unavailable")`,
		`Loc("provider_status_histogram_empty")`,
		`urnw::Plural("provider_status_histogram_total", view.histogram.total)`,
		"view.histogram.fractions[i] * kDemandBarsHeight",
		"ApplyProvideReason();")
	// the plots' gate is the one that shows Demand, so a gate change repaints it
	stats := body("void WalletPage::ApplyStatsSections(bool force) {")
	requireOrder("WalletPage::ApplyStatsSections", stats,
		"w_.WalletProviderBlockedChartRow().Visibility(shown(sections.providerVisible));", "ApplyProviderStatus();")
	requireAll("WalletPage::ApplyProvideReason", body("void WalletPage::ApplyProvideReason() {"),
		"providerstatus::LineFor(",
		"provideControlMode_ != provideridle::ProvideControlMode::Never")
	requireAll("WalletPage::RebuildProviderWhy", body("void WalletPage::RebuildProviderWhy() {"),
		"providerstatus::WhyRowsFor(*providerStatus_, ProviderStatusText())",
		"colors::kUrAmber",
		"Loc(row.helpKey)",
		"for (auto const& reason : row.reasons) {")
	// the reason lines' plurals go through the store's plural lookup
	requireAll("ProviderStatusText", body("providerstatus::ValueText<std::wstring> ProviderStatusText() {"),
		"text.plural = [](std::string_view key, int64_t count) { return urnw::Plural(key, count); };")
	requireAll("WalletPage::BuildCharts", body("void WalletPage::BuildCharts() {"),
		"w_.WalletProviderDemandBars()",
		"providerstatus::kBarCount",
		"colors::kUrGreen")
	requireAll("WalletPage::Initialize", body("void WalletPage::Initialize() {"),
		"w_.WalletProviderWhyButton().Click(", "ToggleProviderWhy();")

	// Demand and Why?: right after the Blocked plot, Demand at its height
	markup := readAppSource(t, "MainWindow.xaml")
	blocked := strings.Index(markup, `<Border x:Name="WalletProviderBlockedChartRow"`)
	demand := strings.Index(markup, `<Border x:Name="WalletProviderDemandRow"`)
	why := strings.Index(markup, `<Border x:Name="WalletProviderWhyRow"`)
	if blocked < 0 || demand < 0 || why < 0 || demand < blocked || why < demand {
		t.Fatal("MainWindow.xaml: the Demand and Why? rows must follow the Blocked plot, in that order")
	}
	if between := markup[blocked+1 : demand]; strings.Count(between, "<Border") != 0 {
		t.Error("MainWindow.xaml: the Demand row must directly follow the Blocked plot")
	}
	if end := strings.Index(markup[why:], "</ScrollViewer>"); end < 0 ||
		strings.Contains(markup[why+1:why+end], `<Border x:Name="Wallet`) {
		t.Error("MainWindow.xaml: the Demand and Why? rows must be the provider plots' last rows")
	}
	for _, pattern := range []string{
		`<Border x:Name="WalletProviderDemandRow" Visibility="Collapsed"[^>]*Style="\{StaticResource UrPaneRowStyle\}"`,
		`<TextBlock x:Name="WalletProviderDemandTitle"[^>]*FontSize="11" FontWeight="Medium"`,
		`<TextBlock x:Name="WalletProviderDemandTotal"[^>]*Style="\{StaticResource UrPaneMetaStyle\}"`,
		`<TextBlock x:Name="WalletProviderDemandCaption"[^>]*Style="\{StaticResource UrRowNoteStyle\}"`,
		`<TextBlock x:Name="WalletProviderDemandStatus"`,
		`<Grid x:Name="WalletProviderDemandChart"[^>]*Height="66"`,
		`<Grid x:Name="WalletProviderDemandBars"`,
		`<TextBlock x:Name="WalletProviderDemandEmpty"`,
		`<TextBlock x:Name="WalletProviderDemandStart"`,
		`<TextBlock x:Name="WalletProviderDemandEnd"[^>]*HorizontalAlignment="Right"`,
		`<Border x:Name="WalletProviderWhyRow" Visibility="Collapsed"`,
		`<Button x:Name="WalletProviderWhyButton"[^>]*Style="\{StaticResource UrPaneRowButtonStyle\}"`,
		`<TextBlock x:Name="WalletProviderWhyLabel"`,
		`<FontIcon x:Name="WalletProviderWhyChevron"[^>]*Glyph="&#xE70D;"`,
		`<StackPanel x:Name="WalletProviderWhyPanel" Visibility="Collapsed"`,
	} {
		if !regexp.MustCompile(pattern).MatchString(markup) {
			t.Errorf("MainWindow.xaml: missing %s", pattern)
		}
	}

	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="ProviderStatusPresentation.h" />`) {
		t.Error("App.vcxproj does not list ProviderStatusPresentation.h")
	}

	// every key the presentation and the page read is in the catalog
	presentation := readAppSource(t, "ProviderStatusPresentation.h")
	pageKeys := strings.Join([]string{open, render, body("void WalletPage::ApplyStrings() {"),
		body("void WalletPage::RebuildProviderWhy() {")}, "\n")
	keyPattern := regexp.MustCompile(`"((?:provider_status|provider_idle)_[a-z0-9_]+|reliability|country|loading)"`)
	keys := map[string]bool{}
	for _, source := range []string{presentation, pageKeys} {
		for _, match := range keyPattern.FindAllStringSubmatch(source, -1) {
			keys[match[1]] = true
		}
	}
	if len(keys) < 45 {
		t.Errorf("found only %d store keys in the provider status sources; update this contract", len(keys))
	}
	// the plural keys, which the catalog holds per category
	plurals := map[string]bool{
		"provider_status_histogram_total":  true,
		"provider_status_reason_line_tcp":  true,
		"provider_status_reason_line_ipv4": true,
	}
	for key := range keys {
		lookup := key
		if plurals[key] {
			lookup = key + ".other"
		}
		if reswValue(t, root, "en", lookup) == "" {
			t.Errorf("the provider status reads %q, which en/Resources.resw does not define", key)
		}
	}
	for key, want := range map[string]string{
		"provider_status_demand":                 "Demand",
		"provider_status_histogram_title":        "Times clients were offered this device, per minute, last hour",
		"provider_status_histogram_start":        "60 min ago",
		"provider_status_histogram_end":          "Now",
		"provider_status_histogram_empty":        "Not offered to clients in the last hour",
		"provider_status_unavailable":            "Provider status isn't available right now.",
		"provider_status_why":                    "Why?",
		"provider_status_histogram_total.other":  "{} times in the last hour",
		"provider_status_value_with_minimum":     "{0} (needs {1})",
		"provider_status_value_count_of_total":   "{0} of {1} loaded",
		"provider_status_reason_line_tcp.other":  "{} of these checks failed only over HTTP/3 (UDP), so this device is offered for TCP traffic only. Some app builds and some networks do not route UDP.",
		"provider_status_reason_line_ipv4.other": "{} of these checks failed only over IPv6, so this device is offered for IPv4 traffic only.",
	} {
		if got := reswValue(t, root, "en", key); got != want {
			t.Errorf("en resw %s = %q, want %q", key, got, want)
		}
	}
}

// The provider-only device's status read on the api applies an answer as the
// controller applies a poll: a failed poll that dropped the snapshot would
// blank Demand on every 404 or timeout, and a reading that took the network's
// first provider row would show another device's demand as this one's.
func TestProviderStatusReadingsRejectBrokenApply(t *testing.T) {
	// Run the spec against a rewritten copy of ProviderStatusPresentation.h and
	// require that it fails, naming want. The rest of the header's includes
	// still come from the app.
	requireProviderStatusFailure := func(mutate func(string) string, want string) {
		t.Helper()
		root := repositoryRoot(t)
		source, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "ProviderStatusPresentation.h"))
		if err != nil {
			t.Fatal(err)
		}
		changed := mutate(string(source))
		if changed == string(source) {
			t.Fatal("negative control did not change the production ProviderStatusPresentation.h")
		}
		dir := t.TempDir()
		if err := os.WriteFile(filepath.Join(dir, "ProviderStatusPresentation.h"), []byte(changed), 0600); err != nil {
			t.Fatal(err)
		}
		// -iquote: searched for the harness's quoted include ahead of the app's -I
		program := buildProviderStatusTests(t, "-iquote", dir)
		output, err := exec.Command(program).CombinedOutput()
		if err == nil || !strings.Contains(string(output), want) {
			t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
		}
	}

	requireProviderStatusFailure(func(source string) string {
		return strings.Replace(source,
			"    error = message.empty() ? std::string(\"no provider status\") : std::string(message);\n",
			"    error = message.empty() ? std::string(\"no provider status\") : std::string(message);\n    status.reset();\n", 1)
	}, "api: a failed poll keeps the last snapshot")
	requireProviderStatusFailure(func(source string) string {
		return strings.Replace(source, "if (candidate.client_id && *candidate.client_id == clientId) {",
			"if (candidate.client_id) {", 1)
	}, "api: the first row whose client id is this device's")
}
