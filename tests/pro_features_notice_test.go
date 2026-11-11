// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The network's Pro features in use, as the app shows them
// (App/ProFeaturesNotice.h; owner, 2026-11-11): SOCKS and WireGuard hosted
// proxies are part of Pro (pro.yml), and a network without Pro whose proxies
// hold them is told so above the device list and in a notice on the main
// window, before the server's enforce_features switch turns on and after it.
// The decisions (the prompt, when the main window's notice shows, the
// dismissal) are the sdk's ProFeaturesViewController's and are tested there;
// this pins what the app does with them: the spec of the header, and the wiring
// the spec cannot run, read with every comment blanked.

// Compile and run the spec (app/tools/pro-features-notice-tests.cpp): the words
// for each prompt and for none, the device list's notice and its Get Pro, and
// the purchase that reads the proxies again.
func TestProFeaturesNotice(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("pro features notice tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "pro-features-notice-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "pro-features-notice-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build pro features notice tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("pro features notice: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
	project := readAppSource(t, "App.vcxproj")
	if !strings.Contains(project, `<ClInclude Include="ProFeaturesNotice.h" />`) {
		t.Error("App.vcxproj does not list ProFeaturesNotice.h")
	}
}

// The header spells the sdk's prompts itself so it needs no sdk header; they
// must be the sdk's (cgo urnetwork_sdk.hpp, which carries the controller from
// the Pro features sdk on), and the wrapper must have the calls the app makes.
// Skipped with no such header around.
func TestProFeaturesNoticeSpellsTheSdksValues(t *testing.T) {
	header := sdkHeaderWith(t, "ProFeaturesViewController")
	notice := readAppSource(t, "ProFeaturesNotice.h")
	for _, pair := range [][2]string{
		{"ProFeaturesPromptNone", "kPromptNone"},
		{"ProFeaturesPromptUpgrade", "kPromptUpgrade"},
		{"ProFeaturesPromptStopped", "kPromptStopped"},
	} {
		sdkValue := constantValue(t, "urnetwork_sdk.hpp", header, pair[0])
		appValue := constantValue(t, "ProFeaturesNotice.h", notice, pair[1])
		if sdkValue != appValue {
			t.Errorf("ProFeaturesNotice.h: %s is %q, the sdk's %s is %q", pair[1], appValue, pair[0], sdkValue)
		}
	}
	for _, want := range []string{
		"ProFeaturesViewController openProFeaturesViewController() const;",
		"Sub addProFeaturesListener(ProFeaturesListener listener) const;",
		"void dismissMainNotice() const;",
		"using ProFeaturesListener = std::function<void(std::optional<ProFeaturesSnapshot> snapshot)>;",
		"std::string MainPrompt{};",
		"std::optional<ProFeaturesStatus> ProFeatures;",
	} {
		if !strings.Contains(header, want) {
			t.Errorf("urnetwork_sdk.hpp: missing %s", want)
		}
	}
}

// The words, each looked up through the header and nowhere else: the main
// window and the device list both word a prompt with WordsFor.
func TestProFeaturesNoticeWordsAreTheHeaders(t *testing.T) {
	notice := stripComments(readAppSource(t, "ProFeaturesNotice.h"))
	for _, want := range []string{
		`inline constexpr const char* kUpgradeTitleKey = "pro_features_in_use";`,
		`inline constexpr const char* kUpgradeMessageKey = "pro_features_upgrade_message";`,
		`inline constexpr const char* kStoppedTitleKey = "pro_features_stopped";`,
		`inline constexpr const char* kStoppedMessageKey = "pro_features_stopped_message";`,
	} {
		if !strings.Contains(notice, want) {
			t.Errorf("ProFeaturesNotice.h: missing %s", want)
		}
	}
	for name, source := range appSourceFiles(t, ".cpp", ".h") {
		if name == "ProFeaturesNotice.h" {
			continue
		}
		code := stripComments(source)
		for _, key := range []string{`"pro_features_in_use"`, `"pro_features_upgrade_message"`,
			`"pro_features_stopped"`, `"pro_features_stopped_message"`} {
			if strings.Contains(code, key) {
				t.Errorf("%s spells %s: the words are ProFeaturesNotice.h's (WordsFor)", name, key)
			}
		}
	}
}

// AppController: one controller per sign-in, opened on the Api at each sign-in
// push (a repeated one replaces it) and closed at sign-out and at quit, its
// notice with it; its listener only hops to the UI thread, where a closed
// controller's snapshot is dropped; the window's notice follows MainPrompt and
// is replayed on show; Close is the sdk's dismissal; the proxies are read again
// when the window comes to the front and when the plan turns Pro; and nothing
// here posts a tray notification, disconnects, stops the tunnel or changes the
// provide mode.
func TestProFeaturesNoticeAppWiring(t *testing.T) {
	source := appControllerSource(t)

	// one controller, opened in one place, on the Api
	if n := strings.Count(source, "openProFeaturesViewController()"); n != 1 {
		t.Errorf("AppController.cpp opens the Pro features controller in %d places, want 1", n)
	}
	for name, file := range appSourceFiles(t, ".cpp", ".h") {
		if name != "AppController.cpp" && strings.Contains(stripComments(file), "openProFeaturesViewController(") {
			t.Errorf("%s opens a Pro features controller: AppController owns the sign-in's one", name)
		}
	}
	open := definitionBody(t, "AppController.cpp", source, "void AppController::OpenProFeatures() {")
	provideRequireOrder(t, "OpenProFeatures", open, "CloseProFeatures();",
		"proFeatures_.emplace(sdk_.api().openProFeaturesViewController());")
	provideRequireOrder(t, "OpenProFeatures", open, "if (!sdk_.apiReady() || !sdk_.IsLoggedIn()) return;",
		"proFeatures_.emplace(sdk_.api().openProFeaturesViewController());")
	provideRequireOrder(t, "OpenProFeatures", open, "const std::uint64_t generation = proFeaturesGeneration_;",
		"addProFeaturesListener(")
	provideRequireOrder(t, "OpenProFeatures", open, "addProFeaturesListener(", "proFeatures_->start();")
	listener := clientLimitBlock(t, "OpenProFeatures", open, "addProFeaturesListener(", "proFeatures_->start();")
	provideRequire(t, "the Pro features listener", listener,
		"OnUi([this, generation, snapshot = std::move(*snapshot)] {",
		"OnProFeatures(generation, snapshot);")
	requireNone(t, "the Pro features listener", listener, "SetProFeaturesPrompt(", "proFeaturesPrompt_", "window_")

	// open at every sign-in push and nowhere else; closed at sign-out
	if n := strings.Count(source, "OpenProFeatures();"); n != 1 {
		t.Errorf("AppController.cpp opens the Pro features in %d places, want 1 (the sign-in)", n)
	}
	auth := cryptoPayCode(definitionBody(t, "AppController.cpp", source,
		"void AppController::OnAuthState(AuthState state, const std::string& error) {"))
	provideRequire(t, "OnAuthState", auth,
		"if(state==AuthState::LoggedIn){OpenProFeatures();}elseif(state==AuthState::LoggedOut&&wasLoggedIn){CloseProFeatures();}")

	// the close: no snapshot after it, the listener before the controller, and
	// the notice gone with it
	closing := definitionBody(t, "AppController.cpp", source, "void AppController::CloseProFeatures() {")
	order := []string{"++proFeaturesGeneration_;", "proFeaturesSub_.reset();", "proFeatures_->close();",
		"proFeatures_.reset();", "SetProFeaturesPrompt(std::string());"}
	for i := 0; i+1 < len(order); i++ {
		provideRequireOrder(t, "CloseProFeatures", closing, order[i], order[i+1])
	}
	shutdown := definitionBody(t, "AppController.cpp", source, "void AppController::Shutdown(lifetime::Ending ending) {")
	provideRequireOrder(t, "Shutdown", shutdown, "cryptoInvoice_.Stop(", "CloseProFeatures();")
	provideRequireOrder(t, "Shutdown", shutdown, "CloseProFeatures();", "tray_.Destroy();")
	if n := strings.Count(source, "CloseProFeatures();"); n != 3 {
		t.Errorf("AppController.cpp closes the Pro features in %d places, want 3 (a new sign-in, the sign-out, the quit)", n)
	}

	// a snapshot: dropped for a closed controller, else its MainPrompt
	snapshot := definitionBody(t, "AppController.cpp", source, "void AppController::OnProFeatures(")
	provideRequireOrder(t, "OnProFeatures", snapshot, "if (generation != proFeaturesGeneration_) return;",
		"SetProFeaturesPrompt(snapshot.MainPrompt);")
	prompt := definitionBody(t, "AppController.cpp", source, "void AppController::SetProFeaturesPrompt(std::string prompt) {")
	provideRequire(t, "SetProFeaturesPrompt", prompt, "if (prompt == proFeaturesPrompt_) return;",
		"if (!windowVisible_ || !window_) return;", "self->SetProFeaturesNotice(proFeaturesPrompt_);")
	present := definitionBody(t, "AppController.cpp", source, "void AppController::ReconcileWindowPresentation() {")
	provideRequire(t, "ReconcileWindowPresentation", present, "self->SetProFeaturesNotice(proFeaturesPrompt_);")

	// Close is the sdk's dismissal; a refresh is the sdk's read
	dismiss := definitionBody(t, "AppController.cpp", source, "void AppController::DismissProFeaturesNotice() {")
	provideRequireOrder(t, "DismissProFeaturesNotice", dismiss, "if (!proFeatures_) return;",
		"proFeatures_->dismissMainNotice();")
	refresh := definitionBody(t, "AppController.cpp", source, "void AppController::RefreshProFeatures() {")
	provideRequireOrder(t, "RefreshProFeatures", refresh, "if (!proFeatures_) return;", "proFeatures_->refresh();")
	if n := strings.Count(source, "proFeatures_->dismissMainNotice();"); n != 1 {
		t.Errorf("AppController.cpp dismisses the Pro features notice in %d places, want 1", n)
	}

	// read again when the window comes to the front, and when the plan turns Pro
	activated := handlerSource(t, "AppController.cpp", source, "window_.Activated(")
	provideRequireOrder(t, "the window's Activated handler", activated,
		"windowFocused_ = args.WindowActivationState() != WindowActivationState::Deactivated;",
		"if (windowFocused_) RefreshProFeatures();")
	start := definitionBody(t, "AppController.cpp", source, "void AppController::Start() {")
	purchase := clientLimitBlock(t, "Start", start, "profeatures::PlanTurnedPro(", "clientLimitBalancePro_ = snapshot.isPro;")
	provideRequire(t, "the purchase's read", purchase,
		"profeatures::PlanTurnedPro(clientLimitBalanceLoaded_, clientLimitBalancePro_,",
		"snapshot.loaded, snapshot.isPro)", "RefreshProFeatures();")

	// the notice is the window's alone: no tray notification, and nothing that
	// disconnects, stops the tunnel or changes the provide mode
	for _, signature := range []string{"void AppController::OpenProFeatures() {",
		"void AppController::CloseProFeatures() {", "void AppController::OnProFeatures(",
		"void AppController::SetProFeaturesPrompt(std::string prompt) {",
		"void AppController::RefreshProFeatures() {", "void AppController::DismissProFeaturesNotice() {"} {
		body := definitionBody(t, "AppController.cpp", source, signature)
		requireNone(t, signature, body, "ShowBalloon(", "Disconnect(", "StopServiceTunnel(",
			"SetProvideControlMode(", "Logout(")
	}
}

// The window's notice: one writer for its words, Get Pro through the client
// limit notice's OpenNoticeUpgrade, Manage devices through the device list,
// Close through the controller's dismissal, the InfoBar under the client limit
// notice with no close button of its own; and the device list's close reads
// the proxies again, its Get Pro running OpenNoticeUpgrade once the sheet is
// down.
func TestProFeaturesNoticeWindowWiring(t *testing.T) {
	source := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	apply := definitionBody(t, "MainWindow.xaml.cpp", source, "void MainWindow::ApplyProFeaturesNotice() {")
	provideRequire(t, "ApplyProFeaturesNotice", apply,
		"urnw::profeatures::WordsFor(proFeaturesPrompt_);",
		"bar.Title(Loc(words->titleKey));",
		"bar.Message(Loc(words->messageKey));",
		"bar.IsOpen(true);", "bar.IsOpen(false);",
		"bar.Visibility(Visibility::Visible);", "bar.Visibility(Visibility::Collapsed);")
	set := definitionBody(t, "MainWindow.xaml.cpp", source, "void MainWindow::SetProFeaturesNotice(std::string const& prompt) {")
	provideRequireOrder(t, "SetProFeaturesNotice", set, "if (prompt == proFeaturesPrompt_) return;",
		"ApplyProFeaturesNotice();")
	if n := strings.Count(source, "ApplyProFeaturesNotice();"); n != 1 {
		t.Errorf("MainWindow.xaml.cpp writes the Pro features notice from %d places, want 1 (SetProFeaturesNotice)", n)
	}

	block := clientLimitBlock(t, "MainWindow's constructor", source, "solidBannerAction(upgrade);",
		"ProFeaturesNotice().Content(actions);")
	provideRequire(t, "the Pro features notice's buttons", block,
		`upgrade.Content(LocBox("get_pro"));`,
		`L"acceptance.pro-features.upgrade"`,
		"self->OpenNoticeUpgrade();",
		"ProFeaturesNotice().ActionButton(upgrade);",
		`manage.Content(LocBox("manage_devices"));`,
		"self->ShowManageDevicesSheet(false);",
		`dismiss.Content(LocBox("close"));`,
		`L"acceptance.pro-features.close"`,
		"urnw::App().DismissProFeaturesNotice();")
	requireNone(t, "the Pro features notice's buttons", block, "Disconnect(", "SetProvideControlMode(")
	// the client limit notice's Get Pro and this one are the same function
	limit := source[strings.Index(source, "clientLimitUpgrade_.Click("):]
	limit = limit[:strings.Index(limit, "});")]
	provideRequire(t, "the client limit notice's Get Pro", limit, "self->OpenNoticeUpgrade();")

	ids := xamlAutomationIds(t, filepath.Join(repositoryRoot(t), "app", "src", "App", "MainWindow.xaml"))
	if got := ids["ProFeaturesNotice"]; got != "acceptance.pro-features.notice" {
		t.Errorf("MainWindow.xaml: ProFeaturesNotice's automation id is %q", got)
	}
	xaml := readAppSource(t, "MainWindow.xaml")
	limitBar := strings.Index(xaml, `<InfoBar x:Name="ClientLimitNotice"`)
	bar := strings.Index(xaml, `<InfoBar x:Name="ProFeaturesNotice" IsOpen="False" IsClosable="False"`)
	setup := strings.Index(xaml, `<InfoBar x:Name="ServiceSetupBar"`)
	if limitBar < 0 || bar < 0 || setup < 0 || !(limitBar < bar && bar < setup) {
		t.Error("MainWindow.xaml: the Pro features notice is not an unclosable InfoBar right under the client limit notice")
	}

	// the device list: it reads the proxies again when it closes, and its
	// notice's Get Pro opens the upgrade once no sheet is open
	show := definitionBody(t, "MainWindow.xaml.cpp", source, "winrt::fire_and_forget MainWindow::ShowManageDevicesSheet(")
	provideRequireOrder(t, "ShowManageDevicesSheet", show, ".ShowAsync();",
		"upgrade = self->manageDevicesSheet_->UpgradeRequested();")
	provideRequireOrder(t, "ShowManageDevicesSheet", show, "self->sheetOpen_ = false;",
		"urnw::App().RefreshProFeatures();")
	provideRequireOrder(t, "ShowManageDevicesSheet", show, "self->sheetOpen_ = false;",
		"if (upgrade) self->OpenNoticeUpgrade();")
}

// The device list: its snapshot's ProFeatures.Prompt, worded by the header,
// above the rows and only beside them, with Get Pro unless the list is a
// refused sign-in's; Get Pro closes the sheet for the window's upgrade flow;
// there is no close control.
func TestProFeaturesDeviceListWiring(t *testing.T) {
	sheet := stripComments(readAppSource(t, "ManageDevicesSheet.cpp"))
	plain := definitionBody(t, "ManageDevicesSheet.cpp", sheet, "managedevices::Snapshot PlainOf(")
	provideRequire(t, "PlainOf", plain,
		"plain.proFeaturesPrompt = snapshot.ProFeatures ? snapshot.ProFeatures->Prompt : std::string();")
	build := definitionBody(t, "ManageDevicesSheet.cpp", sheet, "void ManageDevicesSheet::Build(")
	provideRequire(t, "ManageDevicesSheet::Build", build,
		"proFeaturesNotice_.IsClosable(false);",
		`proFeaturesUpgrade_.Content(winrt::box_value(Loc("get_pro")));`,
		"self->RequestUpgrade();",
		"proFeaturesNotice_.ActionButton(proFeaturesUpgrade_);")
	provideRequireOrder(t, "ManageDevicesSheet::Build", build, "listPanel_.Children().Append(proFeaturesNotice_);",
		"listPanel_.Children().Append(MakeLine(Loc(managedevices::kHelpKey)")
	render := definitionBody(t, "ManageDevicesSheet.cpp", sheet, "void ManageDevicesSheet::Render() {")
	provideRequire(t, "ManageDevicesSheet::Render", render,
		"status.showsRows ? profeatures::ListNoticeFor(snapshot_.proFeaturesPrompt, forSignIn_)",
		"proFeaturesNotice_.Title(Loc(notice->words.titleKey));",
		"proFeaturesNotice_.Message(Loc(notice->words.messageKey));",
		"proFeaturesUpgrade_.Visibility(notice->offersUpgrade ? Visibility::Visible",
		"proFeaturesNotice_.IsOpen(notice.has_value());",
		"proFeaturesNotice_.Visibility(notice ? Visibility::Visible : Visibility::Collapsed);")
	request := definitionBody(t, "ManageDevicesSheet.cpp", sheet, "void ManageDevicesSheet::RequestUpgrade() {")
	provideRequireOrder(t, "ManageDevicesSheet::RequestUpgrade", request, "upgradeRequested_ = true;", "dialog_.Hide();")
	requireNone(t, "ManageDevicesSheet.cpp", sheet, "DismissProFeaturesNotice", "dismissMainNotice",
		"proFeaturesNotice_.IsClosable(true)", `Loc("close")`, "ShowUpgradeSheet")
	header := stripComments(readAppSource(t, "ManageDevicesSheet.h"))
	provideRequire(t, "ManageDevicesSheet.h", header, "bool UpgradeRequested() const { return upgradeRequested_; }")
	presentation := stripComments(readAppSource(t, "ManageDevicesPresentation.h"))
	provideRequire(t, "ManageDevicesPresentation.h", presentation, "std::string proFeaturesPrompt;")
}
