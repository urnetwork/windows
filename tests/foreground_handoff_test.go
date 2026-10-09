// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"strings"
	"testing"
)

// lf makes a source file's line endings LF: a Windows checkout has CRLF, and
// definitionBody finds a definition's end by its closing brace in column 0.
func lf(source string) string {
	return strings.ReplaceAll(source, "\r\n", "\n")
}

// A browser handoff must surface the window. Measured live: after a real
// "Open URnetwork?" the login page had laid its sign-in error out on screen
// inside a window that stayed BEHIND the browser for as long as anyone watched -
// which reads as "the app did nothing". Nothing asked Windows to raise it:
// Window::Activate() is ShowWindow + UpdateWindow + SetActiveWindow and never
// requests the foreground. macOS gets the raise from NSApp.activate(
// ignoringOtherApps:) and Android from the OS. On Windows the contracts below
// pin the pieces so a tidy-up cannot silently remove one. The window-level
// behavior itself was verified on the live desktop (z-order and real screen
// pixels, not PrintWindow, which draws a covered window just as happily as an
// uncovered one), on both branches: a launch that held the foreground right,
// and one that held none (a Task Scheduler launch: the z-order fallback).

// The second launch is the process the browser just launched, so it may hold
// the foreground right the running instance needs. The Windows App SDK's
// RedirectActivationToAsync already passes it on (AppInstance::QueueRequest ->
// AllowSetForegroundWindow); the explicit grant before the redirect is
// belt-and-braces against that changing, and its log line is the only record of
// whether the launch held a right at all (a launcher with none is the case only
// the z-order fallback can serve). Pinned so it is not removed as "redundant"
// without that being decided.
func TestSecondLaunchHandsTheForegroundRightToTheRunningInstance(t *testing.T) {
	source := stripLineComments(lf(readAppSource(t, "main.cpp")))
	redirect := definitionBody(t, "main.cpp", source, "urnw::instance::RedirectAttempt Launcher::Redirect() {")
	allowAt := strings.Index(redirect, "::AllowSetForegroundWindow(")
	redirectAt := strings.Index(redirect, "RedirectActivationToAsync(")
	if allowAt < 0 {
		t.Fatal("Launcher::Redirect no longer hands the foreground right to the running instance explicitly: the SDK's redirect still does, but the 'no foreground right' log line that distinguishes the launcher with none is gone")
	}
	if redirectAt < 0 || allowAt > redirectAt {
		t.Fatal("Launcher::Redirect must call AllowSetForegroundWindow BEFORE it redirects the activation: the second launch exits right after")
	}
	if !strings.Contains(redirect, "::AllowSetForegroundWindow(holder_.ProcessId())") {
		t.Fatal("Launcher::Redirect must hand the right to the HOLDER of the key (AppInstance::ProcessId), not to ASFW_ANY and not to itself")
	}
}

// ...and the running instance must USE it: nothing else asks. Window::Activate()
// never requests the foreground, so without shell::RaiseToFront the window stays
// under the browser whatever rights the process holds.
func TestShowWindowRaisesTheWindowAfterActivate(t *testing.T) {
	source := stripLineComments(lf(readAppSource(t, "AppController.cpp")))
	show := definitionBody(t, "AppController.cpp", source, "void AppController::ShowWindowImpl(")
	activateAt := strings.Index(show, "window_.Activate()")
	raiseAt := strings.Index(show, "shell::RaiseToFront(")
	if activateAt < 0 || raiseAt < 0 || raiseAt < activateAt {
		t.Fatal("ShowWindowImpl must call shell::RaiseToFront after window_.Activate(): Activate never asks for the foreground, so alone it leaves the window behind the browser")
	}
}

// A refused SetForegroundWindow is not the end of it: z-order is NOT locked, so
// the topmost toggle still leaves the window on top (unfocused). That fallback
// is what makes the result visible when the launch held no foreground right to
// pass on (measured with a Task Scheduler launch: error 5, the lock refuses, the
// toggle puts the window in front in 184 ms while the browser keeps the focus).
func TestRaiseToFrontFallsBackToTheZOrderWhenTheForegroundIsRefused(t *testing.T) {
	source := stripLineComments(lf(readAppSource(t, "WindowShell.cpp")))
	raise := definitionBody(t, "WindowShell.cpp", source, "bool RaiseToFront(")
	foregroundAt := strings.Index(raise, "::SetForegroundWindow(")
	topmostAt := strings.Index(raise, "HWND_TOPMOST")
	notTopmostAt := strings.Index(raise, "HWND_NOTOPMOST")
	if foregroundAt < 0 || topmostAt < 0 || notTopmostAt < 0 ||
		!(foregroundAt < topmostAt && topmostAt < notTopmostAt) {
		t.Fatal("RaiseToFront must try SetForegroundWindow first, then lift the window with a HWND_TOPMOST -> HWND_NOTOPMOST toggle: a refused raise must still end with the window on top")
	}
	if !strings.Contains(raise, "SWP_NOACTIVATE") {
		t.Fatal("the topmost toggle must not activate the window: it exists for the case where taking focus was refused")
	}
	if !strings.Contains(raise, "WS_EX_TOPMOST") {
		t.Fatal("RaiseToFront must leave a window the user already pinned topmost alone instead of un-pinning it")
	}
}

// A fresh sign-in click supersedes the attempt before it, and the SDK answers
// the superseded attempt with a "superseded by ..." reason. That is bookkeeping:
// applied to the page it would un-grey the affordances the fresh click just
// disabled and print the reason as a login error. Every callback that applies a
// flow's answer to the page must drop it first (bridge::IsSuperseded, the seam
// WalletPage uses - a click counter would also drop genuine late results).
func TestLoginSignInCallbacksDropSupersededAnswers(t *testing.T) {
	source := stripLineComments(lf(readAppSource(t, "LoginPage.cpp")))
	applied := strings.Count(source, "ApplyWalletSignInResult(r)")
	dropped := strings.Count(source, "SupersededAnswer(r)")
	if applied < 3 {
		t.Fatalf("LoginPage.cpp applies %d flow answers; the Google/Apple, Solana and Bittensor callbacks should be three - update this contract", applied)
	}
	if dropped < applied {
		t.Fatalf("LoginPage.cpp applies %d sign-in answers to the page but drops superseded ones in only %d callbacks: a superseded answer would show 'superseded by ...' as a login error", applied, dropped)
	}
	if !strings.Contains(source, "bridge::IsSuperseded(") {
		t.Fatal("SupersededAnswer must be bridge::IsSuperseded: the one tested definition of what a supersede reason looks like")
	}
}

// The window hears of an auth push only while it is presented, and replays the
// standing state when it next is. The standing ERROR must be replayed only
// until a presented window has shown it: replayed forever, a sign-in failure
// resurfaces on every later show, on whichever step the user is on by then, and
// force-scrolls the page to it.
func TestStandingAuthErrorIsReplayedOnlyUntilDelivered(t *testing.T) {
	source := stripLineComments(lf(readAppSource(t, "AppController.cpp")))
	reconcile := definitionBody(t, "AppController.cpp", source, "void AppController::ReconcileWindowPresentation(")
	if !strings.Contains(reconcile, "authErrorUndelivered_ ? authError_") {
		t.Fatal("ReconcileWindowPresentation must replay the standing auth error only while it is undelivered (authErrorUndelivered_)")
	}
	push := definitionBody(t, "AppController.cpp", source, "void AppController::OnAuthState(")
	if !strings.Contains(push, "authErrorUndelivered_ = !error.empty()") ||
		!strings.Contains(push, "authErrorUndelivered_ = false") {
		t.Fatal("OnAuthState must mark a new error undelivered and clear the mark when a presented window has taken it")
	}
}
