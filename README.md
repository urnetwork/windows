# URnetwork for Windows

Native Windows 10 21H2+ / Windows 11 client for x64 and ARM64. A WinUI 3 tray
app drives a privileged Windows service that owns the VPN tunnel; both embed
the URnetwork SDK (the cgo C ABI + C++ wrapper from `sdk/cgo`).

## Download

Release MSIs (`URnetwork-<version>-x64.msi`, `URnetwork-<version>-arm64.msi`)
are attached to the releases the build pipeline publishes. The newest release
is at https://github.com/urnetwork/build/releases/latest. Now and then one
carries no MSIs: take the newest that does. The pipeline publishes on its own,
so the newest release can be minutes old. When you install by hand, prefer the
newest one that has been out for a day, which is what the updater offers: the
releases page dates each release, and the developer screen's update line names
the one offered.

The in-app updater polls the same releases
(`app/src/Common/ReleaseSelection.h`, `kOfficialFeed`). It offers the newest
release that has been out, unchanged, for a day, judged at the start of each
UTC day. So the offer is a day or two behind that page and moves on when the
UTC day does, and a build that a later one overtakes before its day is up is
never offered. A release that is deleted, marked a pre-release or edited stops
being offered at once. "Later" on the banner hides a release until the app is
next started.

A release built before the updater polled these releases does not look for
updates here, and never offers one. The first release that does is
v2026.10.9-1067985620, published on 2026-10-10. If yours was installed from an
older release, install the newest release by hand once: it upgrades the
installed copy in place, and updates itself from then on.

A sign-in through the browser (the login sheet's "Continue in
browser", built off until the approval page is live) registers the
`urnetwork://` link for the current Windows user only (`HKCU`); a portable copy
leaves it behind when it is deleted, and `reg delete HKCU\Software\Classes\urnetwork /f`
removes it (an MSI uninstall does not).

The Bittensor key Earnings can create for payouts ("Set up payouts") is kept
for the current Windows user only, in `%LOCALAPPDATA%\URnetwork\app\bittensor_key`,
one file per network with its recovery phrase protected by DPAPI. A sign-out
and an MSI uninstall leave it, so it comes back when that network signs in
again; deleting the account deletes it. Back up its recovery phrase from the
Key screen: a reinstalled or replaced Windows does not bring the key back.

## Architecture

```
URnetwork.exe (tray, per-user)          urnetworkd.exe (service, LocalSystem)
  WinUI 3 window + tray flyout            DeviceLocal + wintun packet pump
  SdkHost: DeviceRemote --------------->  DeviceLocal.SetRpcServer (mTLS ws)
  ServiceClient (named pipe) ----------->  ControlServer -> TunnelController
                                           NetworkConfig (routes/DNS/MTU)
                                           WfpPolicy (leak guards, kill switch)
                                           EgressMonitor -> SDK egress bind
                                           SplitTunnelClient -> SplitTunnel.sys
```

The split exists for one reason: the tunnel needs LocalSystem and the UI must
not have it. The app's `DeviceRemote` controls the service's `DeviceLocal` over
the SDK's own mTLS WebSocket RPC on loopback; the named pipe carries only
lifecycle and config. This mirrors the macOS app↔extension split.

One consequence is worth stating, because it is easy to get wrong when adding
an API: every UI action crosses a process boundary, so anything the UI needs to
*report* — not merely trigger — must return its value across the RPC. That is
why `MigrateExit` and `ProbeAllExits` return counts rather than void.

## Layout

| Path | What |
|---|---|
| `app/src/Common/` | protocol, named-pipe transport, paths, logging, SDK bootstrap (static lib) |
| `app/src/Service/` | `urnetworkd` — SCM service, wintun, packet pump, network config, WFP policy, egress, control server |
| `app/src/App/` | `URnetwork` — WinUI 3 tray app, SdkHost (DeviceRemote), service client, UI |
| `app/driver/` | `SplitTunnel.sys` — clean-room WFP split-tunnel driver (MPL-2.0) + spec |
| `app/installer/` | WiX v5 MSI |
| `app/third_party/` | vendored SDK + wintun (fetched, not committed) |
| `app/tools/fetch-deps.ps1` | fetches wintun (pinned) + the SDK zip, builds import libs |
| `app/tools/build-local.ps1` | one-command local build of the whole solution |

## Prerequisites

- Visual Studio 2022 (v143), "Desktop development with C++" + Windows 11 SDK
  (10.0.22621). The WDK is needed only for the driver.
- vcpkg (manifest mode; `app/vcpkg.json` pulls nlohmann-json + wil).
- WiX Toolset v5 (`dotnet tool install --global wix`) for the installer.
- The SDK Windows DLLs — build them (`build-sdk.ps1` writes
  `URnetworkSdkWindows.zip`).

### A note on the SDK bindings

The cgo bindings under `sdk/cgo` are **committed artifacts**: `exports_gen.go`
and `include/urnetwork_sdk.hpp` are in the tree, and `make build_windows` does
not regenerate them. You only need to regenerate after changing an exported SDK
signature:

```sh
make generate     # = go run ./gen -- pure Go, runs on any host
```

Two traps, both of which fail quietly rather than loudly:

- **Run the generator with `GOOS=linux` in the environment.** On a Windows host
  it drops every `!windows`-tagged declaration (`IoLoop` among them) and emits
  bindings that are wrong with no warning.
- `GOOS=linux go run ./gen` cross-*builds* and then cannot execute the result.
  Build first, then run: `go build -o gen_tool ./gen && GOOS=linux ./gen_tool`.

`make build_windows` needs the cross-toolchains (llvm-mingw / zig), and that is
the only host-sensitive part of the SDK build. Generation is not.

## Build

```powershell
cd app

# 1. fetch wintun + SDK, generate import libs (Developer PowerShell)
tools\fetch-deps.ps1 -SdkZip <path>\URnetworkSdkWindows.zip

# 2. build the app + service (+ driver, with the WDK)
msbuild URnetwork.sln /p:Configuration=Release /p:Platform=x64

# 3. build the MSI (stage binaries into build\x64\Release first)
dotnet build installer\Installer.wixproj -c Release -p:Platform=x64
```

`tools\build-local.ps1` wraps steps 1–2 for a normal edit/build loop (~60s).

Add the app icons under `app/src/App/Assets/` first (see that folder's README);
they are generated from the macOS art by the Go command in
`app/tools/make-icons`.

The app log is at `%LOCALAPPDATA%\URnetwork\app\logs\urnetwork-app.log`.

## What is implemented

The client connects, tunnels, and handles the failure modes a VPN client has to
handle:

- **Tunnel core** — wintun packet pump, `DeviceLocal`, routes/DNS/MTU, and
  socket self-exclusion (`SetEgressInterfaceIndex`), which is what keeps the
  service's own traffic off its own tunnel.
- **Leak prevention** — WFP filters for DNS and IPv6 plus a real kill switch,
  so a dead tunnel fails closed instead of quietly reverting to the clear.
- **Failsafes** — network-change reaction, dead-tunnel detection, and teardown
  paths that clear their own latches. A tunnel that dies must not leave the
  machine unable to reach the internet, and must not refuse to restart
  afterwards; both of those were real bugs and both are covered now. An account
  out of balance is the exception: its traffic stays in the tunnel, the app
  tells the user, and Disconnect is theirs to press, because turning the tunnel
  off would let the traffic out in the clear. A session that never captured (it
  started out of balance, so no provider was proven) holds nothing, and the app
  says what its traffic is doing instead. A paying account refused at a data
  limit gets the same alert in words for the limit.
- **UI** — connect flow, account, wallet/payouts, leaderboard, settings, split
  tunnel, and a developer/reliability screen behind an app-wide Advanced Mode
  toggle.
- **Updater** — `UpdateChecker`, because the Store does not push EXE/MSI
  updates (see `app/STORE.md`). It installs the official MSI from
  urnetwork/build's releases, checked against GitHub's SHA-256 for it, once
  the release has been out, unchanged, for a day.

## Docs

- `PLAN.md` — architecture, decisions, milestones, risks.
- `app/STORE.md` — Microsoft Store submission findings + certification checklist.
- `app/SIGNING.md` — the two signing pipelines (Authenticode installer + attestation driver).
- `app/driver/README.md`, `app/driver/PROVENANCE.md` — split-tunnel driver spec + clean-room record.

## Status and known gaps

The client builds for both architectures and has been run and exercised on real
hardware. Still open, and worth knowing before relying on this:

- Store submission itself (needs Partner Center) and attestation signing for
  the driver — `app/SIGNING.md`, `app/STORE.md`.
- The driver's loopback fixup and Driver Verifier hardening.
- Localization.
- The split-tunnel driver is the least-exercised component here; the
  process-based bind-redirect path has had considerably more real use than the
  rest of it.
