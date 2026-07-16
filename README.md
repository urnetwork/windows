# URnetwork for Windows

Native Windows 10 21H2+ / Windows 11 (x64 + ARM64) client. A WinUI 3 tray app
controls a privileged Windows service that owns the VPN tunnel, embedding the
URnetwork SDK (the cgo C ABI + C++ wrapper from `../sdk/cgo`). See
`PLAN.md` for the full architecture and decisions.

## Architecture

```
URnetwork.exe (tray, per-user)          urnetworkd.exe (service, LocalSystem)
  WinUI 3 flyout + window                 DeviceLocal + wintun packet pump
  SdkHost: DeviceRemote --------------->  DeviceLocal.SetRpcServer (mTLS ws)
  ServiceClient (named pipe) ----------->  ControlServer -> TunnelController
                                           NetworkConfig (routes/DNS/MTU)
                                           EgressMonitor -> SDK egress bind (R1)
                                           SplitTunnelClient -> SplitTunnel.sys
```

The app and service each embed the SDK. The app's `DeviceRemote` controls the
service's `DeviceLocal` over the SDK's own mTLS WebSocket RPC on loopback; the
named pipe only carries lifecycle/config (mirrors macOS app↔extension).

## Layout

| Path | What |
|---|---|
| `app/src/Common/` | protocol, named-pipe transport, paths, logging, SDK bootstrap (static lib) |
| `app/src/Service/` | `urnetworkd` — SCM service, wintun, packet pump, network config, egress, control server |
| `app/src/App/` | `URnetwork` — WinUI 3 tray app, SdkHost (DeviceRemote), service client, UI |
| `app/driver/` | `SplitTunnel.sys` — clean-room WFP split-tunnel driver (MPL-2.0) + spec |
| `app/installer/` | WiX v5 MSI |
| `app/third_party/` | vendored SDK + wintun (fetched, not committed) |
| `app/tools/fetch-deps.ps1` | fetches wintun (pinned) + the SDK zip, builds import libs |

## Prerequisites (Windows build box)

- Visual Studio 2022 (v143), "Desktop development with C++" + Windows 11 SDK
  (10.0.22621) + the WDK (for the driver).
- vcpkg (manifest mode; `app/vcpkg.json` pulls nlohmann-json + wil).
- WiX Toolset v5 (`dotnet tool install --global wix`) for the installer.
- The SDK Windows zip: built by `../build-sdk.ps1` (Go + llvm-mingw; provisioned
  into the build VM by `all/windows/packer/scripts/provision.ps1`) →
  `../sdk/cgo/build/URnetworkSdkWindows.zip`.

## Build

```powershell
cd app

# 1. fetch wintun + SDK, generate import libs (Developer PowerShell)
tools\fetch-deps.ps1 -SdkZip ..\..\sdk\cgo\build\URnetworkSdkWindows.zip

# 2. build the app + service (+ driver, with the WDK)
msbuild URnetwork.sln /p:Configuration=Release /p:Platform=x64

# 3. build the MSI (stage binaries into build\x64\Release first)
dotnet build installer\Installer.wixproj -c Release -p:Platform=x64
```

Add the app icons under `app/src/App/Assets/` first (see that folder's README).

## Component status

Built to spec against the real SDK API and verified where verifiable on the
authoring host (macOS):

- **R1 socket self-exclusion** (`../connect/egress*.go`, `sdk.SetEgressInterfaceIndex`,
  cgo `urnet_set_egress_interface_index`) — implemented and **compiled+tested**
  for darwin and cross-built for windows/amd64. This is the load-bearing piece
  that keeps the service's own traffic off the tunnel.
- **Common, Service, App, driver, installer** — complete source, written against
  the verified SDK wrapper signatures. These build on the Windows toolchain;
  they are **not** compiled on the authoring host.

> This code was authored on macOS. The IDE/language-server errors you may see on
> a non-Windows host (`windows.h not found`, `nlohmann/json.hpp not found`, WinRT
> namespaces missing) are expected — there is no Windows SDK, WDK, or vcpkg there.
> The code targets MSVC v143 / C++20 and the Windows App SDK.

The **WinUI 3 App project** (`app/src/App/App.vcxproj`, XAML) is the most
toolchain-dependent piece: verify the NuGet versions in `app/src/App/packages.config`
against the installed Windows App SDK, and expect one iteration pass on a real
Windows box (per plan R2). The tray, SdkHost, and ServiceClient are plain
Win32/C++ and independent of the XAML toolchain.

## Docs

- `PLAN.md` — architecture, decisions, milestones, risks.
- `app/STORE.md` — Microsoft Store submission findings + certification-spike checklist.
- `app/SIGNING.md` — the two signing pipelines (Authenticode installer + attestation driver).
- `app/driver/README.md`, `app/driver/PROVENANCE.md` — split-tunnel driver spec + clean-room record.

## Milestones

Tracks `PLAN.md`. Implemented here: M0 skeleton, M1 service tunnel core
(wintun + DeviceLocal + R1 + control pipe), M2 tray + auth + connect UI, M3
Account/Wallet/Leaderboard/Support/Settings UI wired to the Api + Stripe upgrade
+ redeem-code + split-tunnel, M3.5 driver (process-based bind-redirect, real
source rewrite), M4 MSI. Real brand icons are generated from the macOS art by
`app/tools/make-icons.py`. Remaining before ship: Store submission itself (needs
Partner Center), attestation signing (app/SIGNING.md), the service-assisted updater
(the Store does not push EXE/MSI updates — see app/STORE.md), DNS/IPv6 leak guards
(R6/R7), the driver loopback-fixup + Verifier hardening (R10), and localization.
