// The native Windows shell around the main window: backdrop, title-bar chrome,
// a compact default size, and placement that survives a restart.
//
// None of this is brand work. The palette, the faces, the connect canvas and
// the colour dots are untouched by this file; what it does is make the window
// behave like a Windows window rather than like a page that happens to have a
// frame around it. Before it, the app never called Resize at all and opened at
// ~1920x1094 — a tray app filling the entire work area.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <windows.h>

#include <winrt/Microsoft.UI.Xaml.h>

namespace urnw::shell {

// The compact default, in DIPs; scaled by the window's own DPI before use.
// A tray flyout, not a workspace.
inline constexpr int kDefaultWidthDips = 480;
inline constexpr int kDefaultHeightDips = 760;
// Below this the drawer's card column has nothing left to give.
inline constexpr int kMinWidthDips = 400;
inline constexpr int kMinHeightDips = 480;

// Apply the shell to a just-created window. Call once, after the window exists
// and before it is first activated.
//
// - System backdrop: Mica where the OS supports it (Windows 11), which also
//   means clearing the root element's opaque background, or the Mica is drawn
//   and then painted over — a mechanism with no signal. On Windows 10 the
//   solid #101010 stays and nothing else changes. Mica also carries the brand
//   background as its FallbackColor, for the several ways it can degrade at
//   runtime while still being "supported".
// - Title bar: the caption buttons are drawn by the system over the extended
//   content, so their colours are set to match the brand surface. The drag
//   region itself is the window's own AppTitleBar element (MainWindow sets it).
// - Placement: restores the last saved size and position, else the compact
//   default centred on the current monitor; either way clamped onto a monitor
//   that actually exists.
//
// RETURNS true when a SAVED placement was restored, which the caller must
// honour: the tray-anchor flyout move would otherwise overwrite the position
// the user chose, one statement after this function applied it. The anchor is
// a default, not an override — see AppController::ShowWindowImpl.
bool ApplyNativeShell(winrt::Microsoft::UI::Xaml::Window const& window, HWND hwnd);

// Record the window's current size and position. Called on the two ways the
// window goes away — hidden to the tray, and quit — rather than on every frame
// of a drag. Returns whether a complete placement was written.
bool SaveWindowPlacement(HWND hwnd);

// Put the window in front of whatever covers it, for a show the USER caused
// somewhere else: the browser's "Open URnetwork?" after a sign-in, an email
// link. Window::Activate() does NOT do this: it is ShowWindow + UpdateWindow +
// SetActiveWindow and never asks for the foreground, so it cannot lift a visible
// window another process covers, whatever rights this process holds. The
// window, and the sign-in result or error it had just painted, stayed behind the
// browser (measured live: a matched error callback laid its message out inside
// a window the browser covered for 8 s, which reads as "the app did nothing").
// macOS gets this from NSApp.activate(ignoringOtherApps:) and Android from the
// OS; on Windows it is ours to ask for.
//
// Order: SetForegroundWindow first. The Windows App SDK's redirect already hands
// the running instance the foreground right (AppInstance::QueueRequest ->
// AllowSetForegroundWindow), so this succeeds after a browser click or any
// launch that held a right. When Windows refuses it (a launch with none to pass
// on: a scheduled task, a service), z-order is NOT locked, so a topmost toggle
// lifts the window above every normal window WITHOUT stealing focus. Returns true
// only when the window became the foreground window; false still leaves it on
// top. "In front" is not "unobscured": another process's always-on-top window or
// a different virtual desktop can still cover it, and nothing raises it again.
bool RaiseToFront(HWND hwnd);

}  // namespace urnw::shell
