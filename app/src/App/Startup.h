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
// file size cannot see it. MRT Core is a WinRT API, so this must be called AFTER
// init_apartment. Returns a ready-formatted diagnostics line.
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
