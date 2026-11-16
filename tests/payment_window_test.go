// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// A window the payment pages open: one of Stripe's payment hosts (3D Secure)
// stays in the app, left unhandled for WebView2 with its opener; every other
// window goes to the system browser (PaymentWindow.h, compiled and run here).
// Both web views route their windows through it.
func TestPaymentPagesKeepStripesWindowsInTheApp(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("payment window tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	program := filepath.Join(t.TempDir(), "payment-window-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(root, "app", "src", "App"),
		filepath.Join(root, "app", "tools", "payment-window-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build payment window tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("payment window: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}

	if !strings.Contains(readAppSource(t, "App.vcxproj"), `<ClInclude Include="PaymentWindow.h" />`) {
		t.Error("App.vcxproj does not list PaymentWindow.h")
	}
	sheets := stripComments(readAppSource(t, "BalanceSheets.cpp"))
	route := cryptoPayCode(definitionBody(t, "BalanceSheets.cpp", sheets, "void RouteNewWindow("))
	requireCryptoPayOrder(t, "RouteNewWindow", route,
		"if(PaymentWindowRouteFor(urnw::Narrow(std::wstring_view{args.Uri()}))==PaymentWindowRoute::InApp){return;}",
		"args.Handled(true);",
		"LaunchUriAsync(Uri(args.Uri()));")
	// the pay page and the checkout bridge, and no window handled any other way
	if got := strings.Count(cryptoPayCode(sheets), "core.NewWindowRequested([](autoconst&,autoconst&args){RouteNewWindow(args);});"); got != 2 {
		t.Errorf("BalanceSheets.cpp routes %d web views' windows through RouteNewWindow, want 2 (the pay page and the checkout bridge)", got)
	}
	if got := strings.Count(sheets, "NewWindowRequested("); got != 2 {
		t.Errorf("BalanceSheets.cpp handles NewWindowRequested %d times, want 2", got)
	}
}
