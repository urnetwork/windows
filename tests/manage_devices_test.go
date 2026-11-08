// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The device list (App/ManageDevicesSheet.h, ManageDevicesPresentation.h; owner,
// 2026-11-08): the network's devices and hosted proxies, each removable but this
// one, from the client limit notice, Settings, and a sign-in the limit refused.
// The list, its order, the removal and the retry that adds this device again are
// the sdk's (ManageDevicesViewController, tested there); this pins how a snapshot
// reads, and the wiring the spec cannot run, read with every comment blanked.

// Compile and run the spec (app/tools/manage-devices-tests.cpp).
func TestManageDevices(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("manage devices tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "manage-devices-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "manage-devices-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build manage devices tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("manage devices: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
	project := readAppSource(t, "App.vcxproj")
	for _, item := range []string{`<ClCompile Include="ManageDevicesSheet.cpp" />`,
		`<ClInclude Include="ManageDevicesSheet.h" />`, `<ClInclude Include="ManageDevicesPresentation.h" />`} {
		if !strings.Contains(project, item) {
			t.Errorf("App.vcxproj does not list %s", item)
		}
	}
}

// The presentation spells the sdk's values itself; they must be the sdk's.
// Skipped with no urnetwork_sdk.hpp that has the device list around.
func TestManageDevicesSpellsTheSdksValues(t *testing.T) {
	header := sdkHeaderWith(t, "ManageDevicesViewController")
	presentation := readAppSource(t, "ManageDevicesPresentation.h")
	for _, pair := range [][2]string{
		{"ManagedDeviceKindProxy", "kKindProxy"},
		{"ManageDevicesErrorSignInRequired", "kErrorSignInRequired"},
		{"ManageDevicesErrorLoadFailed", "kErrorLoadFailed"},
		{"ManageDevicesErrorRemoveFailed", "kErrorRemoveFailed"},
	} {
		sdkValue := constantValue(t, "urnetwork_sdk.hpp", header, pair[0])
		appValue := constantValue(t, "ManageDevicesPresentation.h", presentation, pair[1])
		if sdkValue != appValue {
			t.Errorf("ManageDevicesPresentation.h: %s is %q, the sdk's %s is %q", pair[1], appValue, pair[0], sdkValue)
		}
	}
	for _, want := range []string{"ManageDevicesViewController openManageDevicesViewController() const;",
		"inline ManageDevicesViewController newSignInManageDevicesViewController(const Api& api, const std::string& network_by_jwt, ManageDevicesSignInRetry retry)",
		"void removeDevice(const std::string& client_id) const;", "bool getCanManage() const;"} {
		if !strings.Contains(header, want) {
			t.Errorf("urnetwork_sdk.hpp: missing %s", want)
		}
	}
}

// SdkHost: the list opens on the writer worker, on the session's device when
// there is one (so a removal ends its hold), else on the Api with the stored
// sign-in token and no retry, and for a refused sign-in on that sign-in's token
// with its registration as the retry. The listener drops an older open's
// snapshots and takes no lock; a session's teardown opens the list again on the
// Api before the device goes; the refused sign-in is kept by a client limit
// refusal and forgotten by a sign-in that goes through and by a sign-out.
func TestManageDevicesHostWiring(t *testing.T) {
	source := sdkHostSource(t)
	open := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::OpenManageDevices(")
	provideRequireOrder(t, "OpenManageDevices", open, "manageDevicesGeneration_.fetch_add(1) + 1;",
		`PostWrite("open the device list"`)
	provideRequire(t, "OpenManageDevices", open, "std::scoped_lock lock(mutex_);", "OpenManageDevicesLocked(generation);")
	locked := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::OpenManageDevicesLocked(")
	provideRequireOrder(t, "OpenManageDevicesLocked", locked,
		"if (generation != manageDevicesGeneration_.load()) return;", "CloseManageDevicesVcLocked();")
	provideRequireOrder(t, "OpenManageDevicesLocked", locked,
		"if (!manageDevicesForSignIn_ && device_) {", "manageDevicesVc_ = device_->openManageDevicesViewController();")
	provideRequire(t, "OpenManageDevicesLocked", locked,
		"opened = OpenManageDevicesOnApiLocked(manageDevicesForSignIn_);",
		"ManageDevicesUpdate{urnet::ManageDevicesSnapshot{}, false}",
		"StartManageDevicesLocked(generation);")
	onApi := definitionBody(t, "SdkHost.cpp", source, "bool SdkHost::OpenManageDevicesOnApiLocked(")
	provideRequire(t, "OpenManageDevicesOnApiLocked", onApi,
		"byJwt = refusedSignIn_->byJwt;",
		"retry = [this] { RetryRefusedSignIn(); };",
		"byJwt = localState_->getByJwt();",
		"urnet::newSignInManageDevicesViewController(*api_, byJwt, std::move(retry));")
	start := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::StartManageDevicesLocked(")
	listener := clientLimitBlock(t, "StartManageDevicesLocked", start, "addManageDevicesListener(", "manageDevicesVc_.start();")
	provideRequire(t, "the device list's listener", listener,
		"if (generation != manageDevicesGeneration_.load()) return;",
		"(*handler)(ManageDevicesUpdate{std::move(*snapshot), canManage});")
	requireNone(t, "the device list's listener", listener, "scoped_lock", "lock_guard", "unique_lock")
	provideRequireOrder(t, "StartManageDevicesLocked", start, "addManageDevicesListener(", "manageDevicesVc_.start();")
	closeVc := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::CloseManageDevicesVcLocked() {")
	provideRequire(t, "CloseManageDevicesVcLocked", closeVc,
		"manageDevicesSession_ == sessionGeneration_",
		"device_->closeManageDevicesViewController(manageDevicesVc_);",
		"manageDevicesVc_.close();")
	teardown := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::TeardownSessionLocked(")
	provideRequireOrder(t, "TeardownSessionLocked", teardown, "ReopenManageDevicesOffDeviceLocked();",
		"if (device_) { device_->close(); device_.reset(); }")
	closeAll := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::CloseManageDevices() {")
	provideRequireOrder(t, "CloseManageDevices", closeAll, "manageDevicesGeneration_.fetch_add(1);",
		"CloseManageDevicesVcLocked();")

	register := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::RegisterNetworkClient(")
	provideRequireOrder(t, "RegisterNetworkClient", register,
		"refusedSignIn_ = RefusedSignIn{byJwt, signOutEpoch};", "complete(AuthResult{false, false, *token});")
	provideRequireOrder(t, "RegisterNetworkClient", register, "refusedSignIn_.reset();",
		"AuthResult r{true, false, \"\"};")
	retry := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::RetryRefusedSignIn() {")
	provideRequire(t, "RetryRefusedSignIn", retry, "RegisterNetworkClient(", "refused->byJwt,", "refused->epoch);")
	logout := definitionBody(t, "SdkHost.cpp", source, "void SdkHost::Logout() {")
	provideRequireOrder(t, "Logout", logout, "refusedSignIn_.reset();", "loggedIn_.store(false, std::memory_order_release);")
}

// The window: the notice's Manage devices for a plan only a free slot lifts,
// the Settings row, the sheet in the single-sheet gate with its list closed
// when it closes, a refused sign-in's list (and, with it open, what a retry
// answered), and the sheet's calls into the host.
func TestManageDevicesWindowWiring(t *testing.T) {
	window := stripComments(readAppSource(t, "MainWindow.xaml.cpp"))
	provideRequire(t, "MainWindow's constructor", window,
		"clientLimitManage_.Content(LocBox(\"manage_devices\"));",
		"self->ShowManageDevicesSheet(false);",
		"actions.Children().Append(clientLimitManage_);")
	apply := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::ApplyClientLimitNotice() {")
	provideRequire(t, "ApplyClientLimitNotice", apply,
		"clientLimitManage_.Visibility(offersUpgrade ? Visibility::Collapsed : Visibility::Visible);")
	show := definitionBody(t, "MainWindow.xaml.cpp", window, "winrt::fire_and_forget MainWindow::ShowManageDevicesSheet(")
	provideRequireOrder(t, "ShowManageDevicesSheet", show, "if (sheetOpen_) co_return;", "self->sheetOpen_ = true;")
	provideRequireOrder(t, "ShowManageDevicesSheet", show, "urnw::ManageDevicesSheet::Create(", ".ShowAsync();")
	provideRequireOrder(t, "ShowManageDevicesSheet", show, ".ShowAsync();", "self->manageDevicesSheet_->Close();")
	provideRequireOrder(t, "ShowManageDevicesSheet", show, "self->manageDevicesSheet_->Close();", "self->sheetOpen_ = false;")
	offer := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::OfferSignInManageDevices(")
	provideRequire(t, "OfferSignInManageDevices", offer,
		"urnw::clientlimit::SignInRefusalMessageKey(error)",
		"manageDevicesSheet_->SetMessage(",
		"if (key && Sdk().HasRefusedSignIn()) ShowManageDevicesSheet(true, Loc(key));")
	auth := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::ApplyAuthState(")
	provideRequireOrder(t, "ApplyAuthState", auth, "login_->ShowErrorOnCurrentStep(error);", "OfferSignInManageDevices(error);")
	provideRequire(t, "ApplyAuthState", auth,
		"(manageDevicesSheet_->ForSignIn() ? loggedIn : state == urnw::AuthState::LoggedOut)",
		"manageDevicesSheet_->Dismiss();")

	settings := stripComments(readAppSource(t, "SettingsPage.cpp"))
	device := definitionBody(t, "SettingsPage.cpp", settings, "void SettingsPage::BuildDeviceSection(")
	provideRequire(t, "Settings' device section", device, "NavRow(card, Loc(\"manage_devices\"), unused);",
		"w_.ShowManageDevicesSheet(false);")

	sheet := stripComments(readAppSource(t, "ManageDevicesSheet.cpp"))
	provideRequire(t, "ManageDevicesSheet.cpp", sheet,
		"sdk_.OpenManageDevices(forSignIn_,",
		"self->sdk_.RefreshManageDevices();",
		"sdk_.RemoveManagedDevice(clientId);",
		"sdk_.CloseManageDevices();",
		"managedevices::StatusOf(snapshot_, canManage_)",
		"managedevices::TitleOf(device, text)",
		"managedevices::DetailOf(device, text)",
		"if (managedevices::Removable(device)) {",
		"Format(managedevices::kRemoveBodyKey, Widen(title))")
	confirm := definitionBody(t, "ManageDevicesSheet.cpp", sheet, "void ManageDevicesSheet::ConfirmRemove() {")
	provideRequireOrder(t, "ConfirmRemove", confirm, "CancelRemove();", "sdk_.RemoveManagedDevice(clientId);")
	// a removal is asked for first: the row's Remove only asks
	ask := clientLimitBlock(t, "Render", sheet, "remove.Click([weak = weak_from_this(), device]", "});")
	provideRequire(t, "the row's Remove", ask, "self->AskRemove(device);")
	requireNone(t, "the row's Remove", ask, "RemoveManagedDevice")
}
