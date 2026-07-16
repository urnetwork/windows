// Stable identifiers shared across the URnetwork Windows components.
// Keep these constant across releases: the tray icon GUID and the service/app
// identities are bound to installed state, and changing them orphans it.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <guiddef.h>

namespace urnw::ids {

// Named pipe for the app<->service control channel. The service creates it with
// a restrictive SDDL (see PipeServer) so only Administrators and authenticated
// interactive users can open it.
inline constexpr wchar_t kControlPipeName[] = L"\\\\.\\pipe\\urnetwork.control";

// Windows service name (SCM) and display name.
inline constexpr wchar_t kServiceName[] = L"urnetworkd";
inline constexpr wchar_t kServiceDisplayName[] = L"URnetwork Service";

// Tray icon identity. Shell_NotifyIcon with NIF_GUID uses this; it must be
// stable and the binary must keep a consistent Authenticode signer across
// updates or the registration breaks (see plan R5).
// {B7E9C2A1-4F3D-4C8E-9A1B-2D6E8F0A1C34}
inline constexpr GUID kTrayIconGuid = {
    0xb7e9c2a1, 0x4f3d, 0x4c8e, {0x9a, 0x1b, 0x2d, 0x6e, 0x8f, 0x0a, 0x1c, 0x34}};

// App user model id — required for toast notifications from an unpackaged app,
// and for correct taskbar/tray grouping.
inline constexpr wchar_t kAppUserModelId[] = L"URnetwork.Desktop";

// Deep-link / OAuth callback scheme (matches macOS `urnetwork://`).
inline constexpr wchar_t kUriScheme[] = L"urnetwork";

// Split-tunnel driver device interface (see driver/Ioctl.h for the codes).
inline constexpr wchar_t kSplitTunnelDevicePath[] = L"\\\\.\\URnetworkSplitTunnel";

// Split-tunnel kernel driver: SCM service name + on-disk image (installed next to
// urnetworkd.exe). The service registers + starts it on demand and stops +
// deletes it on teardown (SplitTunnelClient) — Windows Installer can't host a
// kernel-driver service, so the MSI only lays down the .sys.
inline constexpr wchar_t kSplitTunnelServiceName[] = L"URnetworkSplitTunnel";
inline constexpr wchar_t kSplitTunnelSysFileName[] = L"SplitTunnel.sys";

// Stable GUID for the wintun tunnel adapter, so it keeps the same NLA identity
// and interface index across restarts.
// {C4E5F6A7-8B9C-4D0E-A1F2-3B4C5D6E7F80}
inline constexpr GUID kTunAdapterGuid = {
    0xc4e5f6a7, 0x8b9c, 0x4d0e, {0xa1, 0xf2, 0x3b, 0x4c, 0x5d, 0x6e, 0x7f, 0x80}};
inline constexpr wchar_t kTunAdapterName[] = L"URnetwork";

// Network space identity (matches macOS DeviceManager.initializeNetworkSpace).
inline constexpr char kNetworkSpaceHostName[] = "ur.network";
inline constexpr char kNetworkSpaceEnvName[] = "main";

}  // namespace urnw::ids
