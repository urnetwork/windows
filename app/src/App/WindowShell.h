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
// Below this the drawer's 600-wide card column has nothing left to give.
inline constexpr int kMinWidthDips = 400;
inline constexpr int kMinHeightDips = 480;

// Apply the shell to a just-created window. Call once, after the window exists
// and before it is first activated.
//
// - System backdrop: Mica where the OS supports it (Windows 11), which also
//   means clearing the root element's opaque background, or the Mica is drawn
//   and then painted over — a mechanism with no signal. On Windows 10 the
//   solid #101010 stays and nothing else changes.
// - Title bar: the caption buttons are drawn by the system over the extended
//   content, so their colours are set to match the brand surface. The drag
//   region itself is the window's own AppTitleBar element (MainWindow sets it).
// - Placement: restores the last saved size and position, else the compact
//   default, clamped onto a monitor that actually exists.
void ApplyNativeShell(winrt::Microsoft::UI::Xaml::Window const& window, HWND hwnd);

// Record the window's current size and position. Called on the two ways the
// window goes away — hidden to the tray, and quit — rather than on every frame
// of a drag.
void SaveWindowPlacement(HWND hwnd);

}  // namespace urnw::shell
