// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The Connect page's sharing limits (App/ProvideSharePresentation.h; sdk
// provide_share.go, owner decision 2026-11-28, feature 10): the C++ spec,
// alone and against the generated sdk header, and the wiring of SdkHost, the
// page and the markup, read from the app sources with comments stripped.

// Compile the sharing limits spec (app/tools/provide-share-tests.cpp,
// header-only). extra adds compiler arguments ahead of the source.
func buildProvideShareTests(t *testing.T, extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("provide share tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "provide-share-tests")
	arguments := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I" + filepath.Join(root, "app", "src", "App")}
	arguments = append(arguments, extra...)
	arguments = append(arguments, filepath.Join(root, "app", "tools", "provide-share-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, arguments...).CombinedOutput(); err != nil {
		t.Fatalf("build provide share tests: %v\n%s", err, output)
	}
	return program
}

func runProvideShareTests(t *testing.T, program string) {
	t.Helper()
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("provide share: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The two pickers' values, No limit first and a limit the cli set in its
// place, the selection, the whole Mbps of a speed, and the line under them.
func TestProvideSharePresentation(t *testing.T) {
	runProvideShareTests(t, buildProvideShareTests(t))
}

// The same spec on the generated header's urnet::ProvideShareStatus, parsed
// through its json. The header is git-ignored (URNETWORK_SDK_INCLUDE names a
// directory) and needs nlohmann/json, so a host without them skips this, and
// so does a copy from before the sharing limits.
func TestProvideSharePresentationAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := sdkHeaderDirWith(t, root, "struct ProvideShareStatus {")
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the sharing limits (set URNETWORK_SDK_INCLUDE)")
	}
	jsonDir, found := jsonIncludeDir(root)
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := []string{"-DURNW_PROVIDE_SHARE_TESTS_SDK", "-isystem", sdkDir}
	if jsonDir != "" {
		extra = append(extra, "-isystem", jsonDir)
	}
	t.Logf("against %s", filepath.Join(sdkDir, "urnetwork_sdk.hpp"))
	runProvideShareTests(t, buildProvideShareTests(t, extra...))
}

// SdkHost and the page cannot be built off Windows: the device's status
// reaches the page through the drawer's subscriptions, seeded once and
// cleared when the session ends; a picker writes both limits on the write
// worker, the other one read from the device there; the group is hidden,
// never disabled, while there is no status, right under the router port
// mapping row; and every string it shows is the store's.
func TestProvideShareWiring(t *testing.T) {
	root := repositoryRoot(t)

	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	subscribe := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::SubscribeDrawer() {")
	provideRequire(t, "SdkHost::SubscribeDrawer", subscribe,
		"presentationSubs_.push_back(device_->addProvideShareStatusChangeListener(",
		"if (onProvideShareStatus_) onProvideShareStatus_(std::move(status));",
		`ReadSdkList(loggedProvideShareStatus, "getProvideShareStatus",`)
	provideRequireOrder(t, "SdkHost::SubscribeDrawer", subscribe,
		"device_->addProvideShareStatusChangeListener(", "device_->getProvideShareStatus()")
	clear := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::ClearDrawer(bool sessionEnding) {")
	provideRequire(t, "SdkHost::ClearDrawer", clear,
		"if (sessionEnding && onProvideShareStatus_) onProvideShareStatus_(std::nullopt);")
	set := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::SetProvideShareLimits(")
	quitRequireSequence(t, "SdkHost::SetProvideShareLimits", set,
		regexp.QuoteMeta(`PostWrite("set provide share limits",`),
		regexp.QuoteMeta("std::scoped_lock lock(mutex_);"),
		regexp.QuoteMeta("if (!device_) return;"),
		regexp.QuoteMeta("device_->getProvideShareLimits();"),
		regexp.QuoteMeta("if (maxBytesPerSecond) limits->max_bytes_per_second = *maxBytesPerSecond;"),
		regexp.QuoteMeta("if (monthlyByteCount) limits->monthly_byte_count = *monthlyByteCount;"),
		regexp.QuoteMeta("device_->setProvideShareLimits(limits);"))
	header := stripComments(readAppSource(t, "SdkHost.h"))
	provideRequire(t, "SdkHost.h", header,
		"void SetProvideShareStatusHandler(ProvideShareStatusHandler h) {",
		"ProvideShareStatusHandler onProvideShareStatus_;",
		"void SetProvideShareLimits(std::optional<int64_t> maxBytesPerSecond,")

	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	feeds := functionBody(page, "void ConnectPage::WireDrawerFeeds(")
	// the argument comments are blanked with the rest
	quitRequireSequence(t, "the sharing limits' push",
		handlerSource(t, "ConnectPage.cpp", feeds, "sdk.SetProvideShareStatusHandler("),
		regexp.QuoteMeta("queue.TryEnqueue("), regexp.QuoteMeta("page.provideShareStatus_ = status;"),
		`page\.ApplyProvideShareRows\(\s*false\);`)
	rows := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::ApplyProvideShareRows(bool restring) {")
	provideRequire(t, "ConnectPage::ApplyProvideShareRows", rows,
		"w_.ProvideShareGroup().Visibility(shown ? Visibility::Visible : Visibility::Collapsed);",
		"provideshare::SpeedChoices(status.max_bytes_per_second)",
		"provideshare::MonthlyChoices(status.monthly_byte_count)",
		"updatingControls_ = true;",
		"provideshare::StatusLineFor(status)",
		`urnw::Format("provide_share_limit_reached",`,
		"urnw::FormatLocalDate(status.period_end_millis)",
		`urnw::Format("provide_share_used_of_limit", shared,`,
		"urnw::FormatByteCountCompact(status.monthly_byte_count)",
		`urnw::Format("provide_share_used", shared);`,
		"urnw::FormatByteCountCompact(status.byte_count)")
	requireNone(t, "ConnectPage::ApplyProvideShareRows", rows, "IsEnabled(", "SetProvideShareLimits(")
	provideRequire(t, "the pickers' labels", page,
		`if (bytesPerSecond <= 0) return Loc("provide_share_no_limit");`,
		`urnw::Format("provide_share_speed_mbps", provideshare::SpeedMbps(bytesPerSecond))`,
		`if (byteCount <= 0) return Loc("provide_share_no_limit");`,
		"return H(urnw::FormatByteCountCompact(byteCount));")
	for signature, write := range map[string]string{
		"void ConnectPage::OnProvideShareSpeedChanged() {":   "Sdk().SetProvideShareLimits(maxBytesPerSecond, std::nullopt);",
		"void ConnectPage::OnProvideShareMonthlyChanged() {": "Sdk().SetProvideShareLimits(std::nullopt, monthlyByteCount);",
	} {
		changed := definitionBody(t, "ConnectPage.cpp", page, signature)
		provideRequireOrder(t, signature, changed, "if (updatingControls_) return;", "if (!provideShareStatus_) return;")
		provideRequireOrder(t, signature, changed, "if (!provideShareStatus_) return;", write)
		// nothing rebuilt inside the picker's own selection change
		requireNone(t, signature, changed, "ApplyProvideShareRows(", "Items()")
	}
	initialize := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::Initialize() {")
	provideRequire(t, "ConnectPage::Initialize", initialize,
		"w_.ProvideShareSpeedPicker().SelectionChanged(", "self->connect().OnProvideShareSpeedChanged();",
		"w_.ProvideShareMonthlyPicker().SelectionChanged(", "self->connect().OnProvideShareMonthlyChanged();")
	applyStrings := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::ApplyStrings() {")
	quitRequireSequence(t, "ConnectPage::ApplyStrings", applyStrings,
		regexp.QuoteMeta(`w_.ProvideShareLabel().Text(Loc("provide_share_limits"));`),
		regexp.QuoteMeta(`w_.ProvideShareSpeedLabel().Text(Loc("provide_share_max_speed"));`),
		regexp.QuoteMeta(`w_.ProvideShareMonthlyLabel().Text(Loc("provide_share_monthly_limit"));`),
		regexp.QuoteMeta(`w_.ProvideShareHelpText().Text(Loc("provide_share_limits_help"));`),
		`ApplyProvideShareRows\(\s*true\);`)

	// The markup: the group right under the router port mapping row, ahead of
	// the connect options, hidden until a status shows it, each picker named
	// by its title.
	xaml := readAppSource(t, "MainWindow.xaml")
	provideRequire(t, "MainWindow.xaml", xaml,
		`<StackPanel x:Name="ProvideShareGroup" Visibility="Collapsed">`,
		`AutomationProperties.LabeledBy="{Binding ElementName=ProvideShareSpeedLabel}"`,
		`AutomationProperties.LabeledBy="{Binding ElementName=ProvideShareMonthlyLabel}"`)
	provideRequireOrder(t, "MainWindow.xaml", xaml, `x:Name="PortMappingRow"`, `x:Name="ProvideShareGroup"`)
	provideRequireOrder(t, "MainWindow.xaml", xaml, `x:Name="ProvideShareGroup"`, `x:Name="ConnectOptionsLabel"`)
	group := sourceBetween(t, "MainWindow.xaml", xaml, `x:Name="ProvideShareGroup"`, `x:Name="ConnectOptionsLabel"`)
	for _, pattern := range []string{
		`<TextBlock x:Name="ProvideShareLabel"[^>]*Style="\{StaticResource UrGroupHeaderTextStyle\}"`,
		`<ComboBox x:Name="ProvideShareSpeedPicker"`,
		`<ComboBox x:Name="ProvideShareMonthlyPicker"`,
		`<TextBlock x:Name="ProvideShareStatusText"[^>]*Style="\{StaticResource UrRowNoteStyle\}"`,
		`<TextBlock x:Name="ProvideShareHelpText"[^>]*Style="\{StaticResource UrRowNoteStyle\}"`,
	} {
		if !regexp.MustCompile(pattern).MatchString(group) {
			t.Errorf("MainWindow.xaml: the sharing limits group is missing %s", pattern)
		}
	}
	requireNone(t, "the sharing limits group", group, "IsEnabled=", "SelectedIndex=", "SelectionChanged=")

	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="ProvideSharePresentation.h" />`) {
		t.Error("App.vcxproj does not list ProvideSharePresentation.h")
	}

	// the strings, as the spec words them
	for key, want := range map[string]string{
		"provide_share_limits":        "Sharing limits",
		"provide_share_max_speed":     "Max sharing speed",
		"provide_share_monthly_limit": "Monthly data limit",
		"provide_share_no_limit":      "No limit",
		"provide_share_speed_mbps":    "{} Mbps",
		"provide_share_used":          "{} shared this month",
		"provide_share_used_of_limit": "{0} of {1} shared this month",
		"provide_share_limit_reached": "Monthly limit reached. Sharing with others resumes {}.",
		"provide_share_limits_help":   "A limit lowers how much this device shares, which can lower its reliability ranking and its earnings.",
	} {
		if got := reswValue(t, root, "en", key); got != want {
			t.Errorf("en resw %s = %q, want %q", key, got, want)
		}
	}
}
