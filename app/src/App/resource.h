// Resource identifiers for the tray app. The .rc file maps these to the icon
// assets under Assets/ (light/dark variants of the four connect x provide
// states, matching the macOS menu-bar assets).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Light-taskbar icons: base + TrayState (0..3)
#define IDI_TRAY_LIGHT_BASE 100
#define IDI_TRAY_LIGHT_NOPROVIDE_NOCONNECT 100
#define IDI_TRAY_LIGHT_NOPROVIDE_CONNECT 101
#define IDI_TRAY_LIGHT_PROVIDE_NOCONNECT 102
#define IDI_TRAY_LIGHT_PROVIDE_CONNECT 103

// Dark-taskbar icons: base + TrayState (0..3)
#define IDI_TRAY_DARK_BASE 110
#define IDI_TRAY_DARK_NOPROVIDE_NOCONNECT 110
#define IDI_TRAY_DARK_NOPROVIDE_CONNECT 111
#define IDI_TRAY_DARK_PROVIDE_NOCONNECT 112
#define IDI_TRAY_DARK_PROVIDE_CONNECT 113

// App icon
#define IDI_APP 120

// Data resources (RCDATA). world-110m.json is the quantized TopoJSON the
// provider-locations globe draws its land from (Natural Earth 110m, public
// domain, ~100 KB). Embedded rather than deployed beside the exe because the
// app is unpackaged, so a file would carry an install-path question.
#define IDR_WORLD_TOPOLOGY 200
