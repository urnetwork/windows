// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"strings"
	"testing"
)

// These pin the page-level half of "a browser sign-in return must be SEEN": the
// regressions that, one at a time, left an answered error invisible or wrong.
// The window-level half (the window must come in front) is in
// foreground_handoff_test.go.

// The error line is the last row of a vertically scrolling page: at the shell
// minimum it sits below the fold. It is made visible THIS tick, so its bounds do
// not exist until a layout pass runs - a bare StartBringIntoView on it (or one
// queued ahead of that pass) has nothing to measure and does nothing. The tick
// holds the window weakly and swallows an XAML exception: a late completion on a
// draining queue must find nothing, and an exception that reaches XAML ends the
// process.
func TestInitialLoginErrorIsScrolledIntoViewAfterALayout(t *testing.T) {
	source := stripLineComments(lf(readAppSource(t, "LoginPage.cpp")))
	set := definitionBody(t, "LoginPage.cpp", source, "void LoginPage::SetInitialLoginError(")
	layoutAt := strings.Index(set, "UpdateLayout()")
	bringAt := strings.Index(set, "StartBringIntoView(")
	if layoutAt < 0 || bringAt < 0 {
		t.Fatal("SetInitialLoginError no longer scrolls the error line into view: at the 500x600 minimum window it sits below the fold and nothing shows")
	}
	if layoutAt > bringAt {
		t.Fatal("SetInitialLoginError must force a layout (UpdateLayout) BEFORE StartBringIntoView: the line was made visible this tick and has no bounds to bring into view until the pending layout runs")
	}
	if !strings.Contains(set, "get_weak()") {
		t.Fatal("the scroll tick must capture the window weakly (like every sibling tick in LoginPage.cpp): a strong capture outlives a window that is shutting down")
	}
	if !strings.Contains(set, "catch (winrt::hresult_error") {
		t.Fatal("the scroll tick must catch winrt::hresult_error: an exception that reaches XAML ends the process, and a failed scroll must never become one")
	}
}

// The same bridge error reaches the page through TWO channels - the flow's own
// answer (ApplyWalletSignInResult) and the auth-state relay (ApplyAuthState ->
// ShowErrorOnCurrentStep) - and both must go through one mapping, or the friendly
// copy for a machine token flashes on the failure and a later delivery replaces it
// with the raw token. The relay therefore passes the RAW string.
func TestBothErrorChannelsShareOneMapping(t *testing.T) {
	page := stripLineComments(lf(readAppSource(t, "LoginPage.cpp")))
	for _, signature := range []string{
		"void LoginPage::ShowErrorOnCurrentStep(",
		"void LoginPage::ApplyWalletSignInResult(",
	} {
		if !strings.Contains(definitionBody(t, "LoginPage.cpp", page, signature), "MapAuthErrorForDisplay(") {
			t.Fatalf("%s must show the error through MapAuthErrorForDisplay: the two channels would disagree about the copy", signature)
		}
	}
	window := stripLineComments(lf(readAppSource(t, "MainWindow.xaml.cpp")))
	apply := definitionBody(t, "MainWindow.xaml.cpp", window, "void MainWindow::ApplyAuthState(")
	if !strings.Contains(apply, "login_->ShowErrorOnCurrentStep(error)") {
		t.Fatal("ApplyAuthState must hand the RAW error to ShowErrorOnCurrentStep (the page maps it): converting it first would put the unmapped token on screen")
	}
}

// The deep link raises and shows the window BEFORE the SDK processes it: the live
// auth push (SdkHost::SetAuthState -> AppController::OnAuthState) reaches the
// window only while it is presented, and the user has just clicked "Open
// URnetwork?" - there is no reason to keep the window away for a link that turns
// out not to match.
func TestDeepLinkShowsTheWindowBeforeTheSdkHandlesIt(t *testing.T) {
	source := stripLineComments(lf(readAppSource(t, "AppController.cpp")))
	handle := definitionBody(t, "AppController.cpp", source, "void AppController::HandleDeepLink(")
	showAt := strings.Index(handle, "ShowWindow(nullptr)")
	sdkAt := strings.Index(handle, "sdk_.HandleDeepLink(")
	if showAt < 0 || sdkAt < 0 || showAt > sdkAt {
		t.Fatal("HandleDeepLink must call ShowWindow(nullptr) before sdk_.HandleDeepLink(...): the error push only reaches a presented window")
	}
}
