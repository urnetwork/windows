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

// AppInstance single-instance key. Shared here so every site that registers it
// agrees on it (wWinMain registers it on launch); a private copy is how two
// sites drift into a keyless app and a second full instance.
inline constexpr wchar_t kSingleInstanceKey[] = L"URnetwork.Desktop";

// An instance's exiting signal is this, with its process id appended: a
// manual-reset event that the instance raises when it begins to exit
// (App/SingleInstance.cpp). The instance creates it and a launch that finds
// the instance holding kSingleInstanceKey opens it, so both read this name.
inline constexpr wchar_t kExitingSignalPrefix[] = L"Local\\URnetwork.Desktop.Exiting.";

// "Launch URnetwork on system startup" (Common/StartupRegistration.h): the
// user's Run value of this name, and Task Manager's StartupApproved record of
// it. The installer's uninstall deletes both by this name (Package.wxs).
inline constexpr wchar_t kStartupRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kStartupApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
inline constexpr wchar_t kStartupRunValue[] = L"URnetwork";

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
// The operator is bringyour.com: the planned move of the operator to
// *.ur.network was cancelled, so the official space is keyed by the host it
// actually talks to and carries NO migration host name (the SDK's ServiceUrl
// prefers a migration host over the key's host, so a stale one would silently
// redirect every api/connect url). ur.io stays the link/site domain.
inline constexpr char kNetworkSpaceHostName[] = "bringyour.com";
// The key earlier builds bundled under. Each launch re-keys that space to
// kNetworkSpaceHostName before anything binds to it (NetworkSpaceStartup.h),
// so the stored credentials and preferences follow the operator host.
inline constexpr char kLegacyNetworkSpaceHostName[] = "ur.network";
inline constexpr char kNetworkSpaceEnvName[] = "main";

// The SSO callback origin for the Google / Apple browser flows is ALWAYS the
// operator's api, never the pointed-at space's: bringyour manages the provider
// registrations and the exchange secret centrally, and the identity token the
// callback hands back is verified by signature + audience at /auth/login on
// whatever space the client is pointed at. The mobile shape: play services
// hands the token to the active api directly, and no beta or self-hosted space
// ever needs provider config of its own. (Upstream keys the callback off the
// active space's api instead - BrowserSso.swift takes deviceManager's
// activeApiUrl - which strands every non-default space at the provider's
// registration page.)
inline constexpr char kOperatorApiUrl[] = "https://api.bringyour.com";

}  // namespace urnw::ids
