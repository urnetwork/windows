// Startup diagnostics for the tray app — the code that runs before anything
// else, so that a launch which produces no window still produces evidence.
//
// "I opened URnetwork.exe and nothing happened" has at least four causes that
// look identical from outside (plan WP1): the tray icon is there and was not
// noticed; the Windows App Runtime is missing so the App SDK bootstrapper kills
// the process before wWinMain; AppInstance threw; XAML/resources.pri failed to
// load. Until the tray icon exists the app has no UI at all, so a failure on
// that path has exactly two channels — the log file and a message box. Every
// failure here writes to both, and --diagnose prints the same facts to a
// console for pasting.
//
// The strings in this file are deliberately NOT localized: the resource stack
// (resources.pri + MRT) is one of the things that may have failed, and
// Localized() falls back to the raw key id when it has, which would turn a
// diagnostic into a puzzle.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace urnw {

// Open the app log (Common LogInit) and write the first lines. Called as the
// first statement of wWinMain — before init_apartment, before any App SDK call
// — so that the ABSENCE of a "startup: wWinMain" line means the process died
// before reaching its own first instruction. That is the signature of a missing
// Windows App Runtime (see NEXTSTEPS.md), because the App SDK's auto-
// initializer runs from a CRT initializer, ahead of every line we own.
void StartupLogInit();

// One line per environment fact the look-alike startup failures are told apart
// by: App Runtime presence + path (the path carries the version), resources.pri,
// the SDK dll, the log file, the service pipe. Logged at startup and printed by
// --diagnose. Pure Win32 — safe to call before COM/WinRT is up.
std::vector<std::wstring> CollectDiagnostics();

// Log every line at info, under the "startup:" prefix the rest of the path uses.
void LogDiagnostics(const std::vector<std::wstring>& lines);

// Whether the app's localized resources actually RESOLVE — not merely whether
// resources.pri exists, which is all a file check can tell. Localized() falls
// back to the key id, so a present-but-unindexed pri renders every string in the
// UI as "app_name" / "connect": that is cause 4 of the four look-alikes, and a
// file size cannot see it.
//
// CALL ORDER MATTERS. Localization.cpp caches its ResourceLoader in a
// function-local static on the FIRST call, keeping the failure too — so probing
// before the app is up would move that first call earlier than the UI's, and a
// probe that failed for a reason of its own (no apartment yet, MRT not ready)
// would then make every string in the UI render as its key id for the rest of
// the process. A diagnostic that causes the fault it looks for is worse than no
// diagnostic. So: --diagnose calls this (that process prints and exits, there is
// no UI to poison), and the normal path calls it from OnLaunched AFTER the tray
// icon has already resolved a string, where it only reads what is cached.
std::wstring ResourceProbe();

// --diagnose: print the lines to the console this process was launched from (a
// /SUBSYSTEM:WINDOWS process has none of its own, so it attaches to the
// parent's), and to a message box when there is no console at all — a
// double-clicked diagnostic that shows nothing would be the exact bug this work
// package exists to remove. Returns the process exit code.
int WriteDiagnosticsToConsole(const std::vector<std::wstring>& lines);

// True when this process was launched to run diagnostics rather than the app:
// --diagnose, -diagnose, /diagnose or a bare diagnose.
bool WantsDiagnose();

// ---- signed-out preview of the signed-in UI --------------------------------
//
// READ THIS IF YOU ARE BUILDING A SCREEN THAT LIVES BEHIND SIGN-IN.
//
// Almost every surface in this app — the connect drawer, account, wallet,
// payouts, points, leaderboard, settings, the developer screen — only renders
// after a successful login, and this codebase's history is that "reads correct"
// is not evidence: the defects here have been found by running, never by
// review. Without a way in, a screen can be written, reviewed, merged and
// shipped without a single pixel of it ever having been drawn.
//
// So: launch with --preview-ui and the app shows the signed-in shell with no
// session at all. Nothing logs in, no token is read or written, the SDK is not
// asked for anything; only the LoginRoot/HomeNav swap is forced, and the panels
// render against the (empty) local snapshots.
//
//   URnetwork.exe --preview-ui              the connect drawer
//   URnetwork.exe --preview-ui=account      account
//   URnetwork.exe --preview-ui=wallet       wallet   (also raises its snackbar)
//   URnetwork.exe --preview-ui=leaderboard  leaderboard
//   URnetwork.exe --preview-ui=support      support  (also raises its snackbar)
//   URnetwork.exe --preview-ui=settings     settings
//
// or set URNETWORK_PREVIEW_UI to the same tag. The tags are the NavigationView
// item tags in MainWindow.xaml; an unknown one falls back to connect and says so
// in the log.
//
// The window still only appears on a tray click, exactly as in a normal run, so
// drive it the way the verification protocol describes: post WM_APP+1 with
// NIN_SELECT in LOWORD(lParam) to the hidden URnetworkTrayWindow, then capture.
//
// Returns an empty string when the switch is absent, which is every real run.
// This grants nothing: there is no account to read.
std::string PreviewUiDestination();

// Set by App::OnLaunched, read by wWinMain after Application::Start returns. A
// message loop that ends without OnLaunched ever having run means XAML gave up
// without throwing — the fourth look-alike, and the one nothing else can see.
void MarkLaunched();
bool WasLaunched();

// A startup failure the user can see: logged as an error, then a message box
// naming the cause, the mechanical detail (hresult / GetLastError / path) and
// the log file. Blocks until dismissed; callers exit afterwards.
void FailVisible(std::wstring_view cause, std::wstring_view detail);

// True once FailVisible has shown anything. wWinMain returns non-zero then, even
// when the message loop went on to exit normally: a process that told the user
// it failed must not also tell the shell (and any script wrapping it) that it
// succeeded.
bool HadVisibleFailure();

}  // namespace urnw
