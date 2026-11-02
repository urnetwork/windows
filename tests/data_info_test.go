// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The "About your data" sheet: its pure logic (App/DataInfo.h) as a C++ spec,
// and its entry points read from the app sources.

// Compile and execute the "About your data" sheet logic (App/DataInfo.h): the
// next 00:00 UTC free refresh and the countdown to it, the Used, Pending and
// Available split from a balance, the daily amount from the server's
// start_balance_byte_count, and when the banner's refresh line and the upgrade
// sheet's Wait for refresh show.
func TestDataInfo(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("data info tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "data-info-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		"-I"+filepath.Join(root, "app", "src", "Common"),
		filepath.Join(root, "app", "tools", "data-info-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build data info tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("data info: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// The entry points: the Account card's info button and the banner's Why? open
// the "About your data" sheet; the banner leads with when the free data
// refreshes whenever it is open; only a start connect refused for the balance
// opens the upgrade sheet with the refresh line and Wait for refresh.
func TestDataInfoEntryPoints(t *testing.T) {
	root := repositoryRoot(t)
	window := stripLineComments(readAppSource(t, "MainWindow.xaml.cpp"))
	app := stripLineComments(readAppSource(t, "AppController.cpp"))
	connect := stripLineComments(readAppSource(t, "ConnectPage.cpp"))
	sheets := stripLineComments(readAppSource(t, "BalanceSheets.cpp"))

	// the start-connect block marks the upgrade sheet it opens
	blocked := definitionBody(t, "AppController.cpp", app, "void AppController::ShowUpgradeForBlockedConnect()")
	if !strings.Contains(blocked, "self->OpenUpgradeForBlockedConnect();") {
		t.Error("the start-connect block no longer opens the upgrade sheet through OpenUpgradeForBlockedConnect")
	}
	open := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::OpenUpgradeForBlockedConnect()")
	for _, want := range []string{
		"upgradeForBlockedConnect_ = true;",
		"OnOpenUpgrade(nullptr, nullptr);",
		"upgradeForBlockedConnect_ = false;",
	} {
		if !strings.Contains(open, want) {
			t.Errorf("OpenUpgradeForBlockedConnect: missing %s", want)
		}
	}
	// nothing else sets the mark: Get Pro, the Account button and the
	// onboarding link open the sheet without the refresh line
	if count := strings.Count(window, "upgradeForBlockedConnect_ = true;"); count != 1 {
		t.Errorf("MainWindow.xaml.cpp sets the blocked-connect mark %d times, want once", count)
	}
	show := definitionBody(t, "MainWindow.xaml.cpp", window, "winrt::fire_and_forget MainWindow::ShowUpgradeSheet()")
	decide := strings.Index(show, "urnw::datainfo::UpgradeShowsFreeRefresh(upgradeForBlockedConnect_, balance_.isPro)")
	if decide < 0 {
		t.Fatal("ShowUpgradeSheet no longer decides the refresh line with UpgradeShowsFreeRefresh")
	}
	// read before the first suspension, while OpenUpgradeForBlockedConnect runs
	if shows := strings.Index(show, ".ShowAsync("); shows < 0 || shows < decide {
		t.Error("ShowUpgradeSheet reads the blocked-connect mark after the sheet shows")
	}
	upgrade := definitionBody(t, "BalanceSheets.cpp", sheets, "void UpgradeSheet::Build(")
	for _, want := range []string{
		"if (freeRefresh_)",
		`"insufficient_balance_refreshes_in"`,
		`"wait_for_refresh"`,
		"self->dialog_.Hide();",
	} {
		if !strings.Contains(upgrade, want) {
			t.Errorf("UpgradeSheet::Build: missing %s", want)
		}
	}

	// the banner's Why? and the Account info button open the sheet
	if count := strings.Count(window, `L"acceptance.insufficient-balance.why"`); count != 1 {
		t.Errorf("MainWindow.xaml.cpp names the banner's Why? %d times, want once", count)
	}
	if !strings.Contains(window, "links.Children().Append(why);") ||
		!strings.Contains(window, "BalanceWarning().Content(links);") {
		t.Error("the out-of-balance banner no longer carries the Why? link")
	}
	if !strings.Contains(definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::OnOpenDataInfo("),
		"ShowDataInfoSheet();") {
		t.Error("the Account info button no longer opens the data sheet")
	}
	ids := xamlAutomationIds(t, filepath.Join(root, "app", "src", "App", "MainWindow.xaml"))
	if got := ids["AccountDataInfoButton"]; got != "acceptance.account.data-info" {
		t.Errorf("AccountDataInfoButton: AutomationProperties.AutomationId = %q", got)
	}
	xaml, err := os.ReadFile(filepath.Join(root, "app", "src", "App", "MainWindow.xaml"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(xaml), `Click="OnOpenDataInfo"`) {
		t.Error("MainWindow.xaml: the Account info button has no OnOpenDataInfo click")
	}

	// the banner leads with when the free data refreshes whenever it is open
	message := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyBalanceWarningMessage()")
	for _, want := range []string{
		"urnw::datainfo::BannerLinesFor(",
		`"insufficient_balance_refreshes_in"`,
		"urnw::datainfo::TrafficLineKey(",
	} {
		if !strings.Contains(message, want) {
			t.Errorf("ApplyBalanceWarningMessage: missing %s", want)
		}
	}

	// the sheet: the three explanations, the server's daily balance and the
	// refresh line, which Pro does not get
	sheet := definitionBody(t, "BalanceSheets.cpp", sheets, "void DataInfoSheet::Build(")
	for _, want := range []string{
		`"data_info_used"`,
		`"data_info_pending"`,
		`"data_info_available"`,
		`"daily_data_balance"`,
		`"data_info_refresh_at"`,
		"balance.startBalanceByteCount",
		"datainfo::ShowsFreeRefresh(balance.isPro)",
	} {
		if !strings.Contains(sheet, want) {
			t.Errorf("DataInfoSheet::Build: missing %s", want)
		}
	}

	// every new string reaches the catalog, with its placeholder lowered
	for key, want := range map[string]string{
		"data_info_title":                   "About your data",
		"data_info_why":                     "Why?",
		"data_info_refresh_at":              "Free data refreshes daily at 00:00 UTC (in {}).",
		"insufficient_balance_refreshes_in": "Free data refreshes in {}.",
		"wait_for_refresh":                  "Wait for refresh",
	} {
		if got := reswValue(t, root, "en", key); got != want {
			t.Errorf("en resw %s = %q, want %q", key, got, want)
		}
	}

	// the pure header and the ticker are in the project
	project := readAppSource(t, "App.vcxproj")
	for _, want := range []string{
		`<ClInclude Include="DataInfo.h" />`,
		`<ClInclude Include="FreeRefreshTicker.h" />`,
		`<ClCompile Include="FreeRefreshTicker.cpp" />`,
	} {
		if !strings.Contains(project, want) {
			t.Errorf("App.vcxproj: missing %s", want)
		}
	}
}
