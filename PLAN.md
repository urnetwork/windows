# URnetwork Windows App — Port Plan

Port of the macOS menu-bar app (`../apple/app`) to a native Windows 10 21H2+ / Windows 11 app
(amd64 + arm64) at `windows/app`, embedding the URnetwork SDK via the c abi + c++ wrapper from
`../sdk/cgo` (`URnetworkSdk.dll`, `urnetwork_sdk.h`, `urnetwork_sdk.hpp`).

> **Implementation status (2026-07-09):** the full solution is written under `windows/app/`
> (Common lib, `urnetworkd` service, WinUI 3 tray app, clean-room split-tunnel driver, WiX MSI)
> — see `windows/README.md` for build steps and per-component status. The SDK-side
> prerequisites are done and verified on macOS: R1 socket self-exclusion (`connect/egress*.go`
> + `sdk.SetEgressInterfaceIndex` + cgo `urnet_set_egress_interface_index`) and a JSON-friendly
> `NetworkSpaceManager.UpdateNetworkSpaceValues` (the callback form can't cross the ABI). All
> SDK changes are uncommitted and build for darwin + windows/amd64 + windows/arm64; the C++
> wrapper smoke test passes. The C++ app/service/driver are written against the verified SDK
> signatures but compile on the Windows toolchain, not the macOS authoring host.
>
> **v1 scope (revised 2026-07-10): FULL macOS parity — sign in, sign up, connect (all
> controls), and provide.** The service/tunnel spine is complete; the UI written so far is a
> *subset* (sign-in + connect-best + basic account/wallet/leaderboard/support). Full v1 still
> adds: sign up (create network + verify), Google + Apple browser SSO, the location/provider
> picker, the connect detail sheets (contracts/split/DNS/throughput), and the full provide
> surface. See the M2/M3 milestones and §7 decisions.

Stack decisions below marked **[verified]** come from a cited deep-research pass (2026-07-09,
primary sources: Microsoft Learn, wintun.net, Mullvad/Tailscale repos); items marked
**[judgment]** are engineering recommendations where the research produced no surviving
verified claims (service/IPC pattern, packaging, solution layout) — revisit as needed.

## 1. Architecture (mirrors macOS)

```
+--------------------------------------+      +-------------------------------------------+
| URnetwork.exe (tray app, user, HKCU) |      | urnetworkd.exe (Windows service, SYSTEM)  |
|  WinUI 3 flyout + main window        |      |  URnetworkSdk.dll:                        |
|  URnetworkSdk.dll:                   |      |   NetworkSpaceManager(import json)        |
|   NetworkSpaceManager(storage)       |      |   DeviceLocal (NewDeviceLocalWith         |
|   NetworkSpace -> Api, LocalState    |      |     KeyMaterial) + SetRpcServer(listen)   |
|   DeviceRemote + SetRpcServer(dial)  |      |  wintun.dll adapter + packet pump         |
|   ConnectViewController etc.         |      |  routes/DNS/MTU via iphlpapi              |
+-----+--------------------+-----------+      +-----+-------------------------+-----------+
      |                    |                        |                         |
      |                    +--- device rpc: mTLS websocket 127.0.0.1:12000-12100
      |                         (SDK-internal; pinned per-session self-signed certs,
      |                          exactly the macOS app<->extension channel)
      |
      +--- service control: named pipe \\.\pipe\urnetwork  [judgment]
           start/stop tunnel, config injection (by_jwt, network_space json,
           instance_id, rpc PEMs, listen port), logout — replaces
           NETunnelProviderManager + providerConfiguration + handleAppMessage
```

The trust story transfers unchanged: the app generates `GenerateDeviceRpcKeyMaterial()` per
session, hands `{serverPem, clientCertPem}` to the service over the control channel, keeps
`{clientPem, serverCertPem}`; the service `DeviceLocal.SetRpcServer(serverPem, clientCertPem,
host:port)` (mTLS listener), the app `DeviceRemote.SetRpcServer(clientPem, serverCertPem,
host:port)` (dialer, pins server cert). mTLS with per-session pinned self-signed certs is the
localhost anti-spoofing answer; the named pipe additionally gets a restrictive SDDL and only
carries lifecycle/config. Last-good rpc session persists like `RpcSessionStore` (registry or
`%LOCALAPPDATA%\URnetwork\rpc_session.json`).

## 2. Stack decisions

- **UI: WinUI 3 (Windows App SDK), C++/WinRT, unpackaged.** [verified] Microsoft's
  recommendation for new Windows-only native apps; unpackaged C++ works on Win10 1809+ and
  Win11, x64 + arm64. Requirements: set `<WindowsPackageType>None</WindowsPackageType>` in the
  .vcxproj (auto-initializes the bootstrapper) and chain `WindowsAppRuntimeInstall.exe --quiet`
  in the installer (or self-contained mode, ~200 MB). Caveats: Mica is Win11-only; the
  WinUI-vs-Qt ergonomics comparison for a small flyout was NOT verifiable — if C++/WinRT XAML
  friction proves too high in M2, the fallback is Qt 6.12 LTS (supports the full matrix under
  MSVC 2022; but it is the LAST Qt with Win10 support and later LTS patches are
  commercial-only — a pinning risk that keeps WinUI 3 the default). [verified]
- **Tray: classic Win32 regardless of framework** (Windows App SDK has no tray API). [verified]
  `Shell_NotifyIcon` with GUID identity (`NIF_GUID`, `__declspec(uuid)`) + `NIM_SETVERSION`
  `NOTIFYICON_VERSION_4`; `NIN_SELECT` (left-click) toggles the flyout, `WM_CONTEXTMENU`
  (right-click) shows the menu (Connect/Disconnect/Show/Quit, same as macOS); anchor the
  flyout with `Shell_NotifyIconGetRect` + `CalculatePopupWindowPosition(TPM_VERTICAL |
  TPM_VCENTERALIGN | TPM_CENTERALIGN | TPM_WORKAREA)` (per-monitor-DPI physical pixels).
  Known constraints [verified]: new icons land in the hidden overflow by default and ONLY the
  user can pin them — first-run UX must guide pinning; the GUID registration is bound to the
  exe path unless old and new binaries are Authenticode-signed by the same company — sign
  consistently from day one. The 4 connect×provide icon states port directly; light/dark
  taskbar icon switching had no verified pattern — prototype against
  `HKCU...\Themes\Personalize\SystemUsesLightTheme` + `WM_SETTINGCHANGE` in M2 [judgment].
- **Data plane: wintun.dll** (upstream-signed, redistributed unmodified). [verified] L3 TUN
  "akin to /dev/net/tun"; ships as a signed-DLL zip (amd64/x86/arm64 + `wintun.h`); the driver
  is embedded in the DLL and installs on first `WintunCreateAdapter` (admin/SYSTEM — i.e. the
  service); the signed DLLs are the only supported distribution, so no driver signing burden.
  Download pinned like Mullvad: SHA256 of `wintun-0.14.1.zip` + upstream Authenticode signer
  thumbprint. Industry confirmation: Tailscale, Mullvad (killed TAP/OpenVPN Jan 2026),
  NordLynx, Mozilla VPN all ship wintun. WireGuardNT is not applicable (kernel WireGuard, not
  a generic TUN for our Go engine).
- **Service: `urnetworkd` Windows service (SCM, auto-start, LocalSystem).** [judgment]
  Hosts DeviceLocal + wintun + network config. Applies what
  `NEPacketTunnelNetworkSettings` did on macOS by hand via iphlpapi/dnsapi:
  adapter IP `DeviceLocal.TunnelLocalAddress()` (default 169.254.2.1/24), MTU 1440,
  DNS from `TunnelDnsSetting()` (`SetInterfaceDnsSettings`; DoH only where the OS supports
  it — plain DNS-to-resolver fallback on Win10), default route via the tun with
  0.0.0.0/1 + 128.0.0.0/1 (or metric) — see risk R1 on loop avoidance.
- **Packaging: WiX MSI, distributed through the Microsoft Store as a Win32 (EXE/MSI) listing.**
  [decided 2026-07-09; certification details unverified — see R9] The Store's Win32 app type
  runs our installer, so the MSI can install the LocalSystem service, wintun, and (later) the
  split-tunnel driver — an MSIX-packaged service/driver would need restricted capabilities and
  cannot carry a driver cleanly. Updates flow through the Store (no in-app updater; MSI must
  support silent install/upgrade). The MSI bundles: app + service + `URnetworkSdk.dll` +
  `wintun.dll` + chained `WindowsAppRuntimeInstall.exe`, all Authenticode-signed.
  Verify early via a certification spike: Win32-listing rules for VPN apps, service+driver
  installers, and third-party commerce (Stripe) under current Store policies.
- **VS solution (MSBuild, native Visual Studio):** [judgment]
  ```
  windows/app/URnetwork.sln
    src/App/         URnetwork.exe    WinUI 3 C++/WinRT, unpackaged, tray + flyout + main window
    src/Service/     urnetworkd.exe   C++ Win32 service; sdk host + wintun pump + net config
    src/Common/      static lib       control-pipe protocol (json), settings, paths, logging glue
    third_party/urnetwork-sdk/{amd64,arm64}/   from sdk cgo URnetworkSdkWindows.zip
                     (dll + .h + .hpp + .def; import .lib via `lib /def:urnetwork_sdk.def`)
    third_party/wintun/                        pinned download, per-arch dll + wintun.h
    installer/       WiX project
    vcpkg.json       nlohmann-json (+ wil)      manifest mode
  ```

## 3. Component mapping (from the macOS inventory)

| macOS | Windows |
|---|---|
| `MenuBarExtra` + 4 state icons | Shell_NotifyIcon GUID icon + WinUI flyout; same 4 assets ×(light/dark) |
| Main window `NavigationSplitView` (Connect/Account/Leaderboard/Support) | WinUI 3 `NavigationView`, same 4 sections |
| `DeviceManager` (NetworkSpaceManager→NetworkSpace(`ur.network`/`main`)→Api/LocalState; DeviceRemote; ~10 persisted listeners) | `SdkHost` in src/App over `urnet::` classes, 1:1 |
| `ConnectViewModel` over `SdkConnectViewController` (+Contract/BlockAction VCs) | same VCs via `urnet::DeviceRemote::openConnectViewController()` etc. |
| `PacketTunnelProvider` (DeviceLocal, `readPackets`→`sendPacket` / `ReceivePacket`→`writePackets`, key material persist, logout msg) | service tunnel core: `WintunReceivePacket`→`urnet::DeviceLocal::sendPacket` / `addReceivePacket`→`WintunSendPacket`; key material via `getKeyMaterial()`/`newDeviceLocalWithKeyMaterial` |
| `VPNManager` + `NETunnelProviderManager` + providerConfiguration | service control named pipe + SCM start/stop |
| Auth: Google SDK, Apple, email+pw+verify, guest, auth-code | email/guest/auth-code native; Google & Apple via system-browser OAuth + `urnetwork://` protocol handler or loopback redirect (decide M2; no native SDKs) |
| StoreKit 2 subscriptions | **open** — no StoreKit equivalent; needs product decision (Stripe checkout in browser / balance codes only at launch) |
| `SMAppService` launch-at-login + login helper | HKCU Run key (tray app), service auto-start covers the daemon |
| `UNUserNotificationCenter` | WinRT toast notifications (works unpackaged with AUMID registration) |
| `urnetwork://` deep links (Solflare/Phantom wallet callbacks, SSO) | `HKCR urnetwork` protocol registration → single-instance forward to tray app |
| Storage: per-process documents dir; logs `Caches/Logs`; `SetMemoryLimit` 48–64 MB in extension | `%LOCALAPPDATA%\URnetwork\app` (user) and `%ProgramData%\URnetwork\service` (service); `urnet::setLogDir` each; same memory limits in service |
| Localizable.xcstrings (~22 languages), custom fonts | port catalog to resw or app-owned json; bundle PP Neue Montreal/Bit, ABC Gravity (license check) |
| App Intents (Connect/Disconnect) | later: URI verbs / jump-list tasks |

## 4. Milestones

- **M0 — skeleton + toolchain proof.** Solution builds on a Windows box; loads
  `URnetworkSdk.dll`; `urnet::version()`; storage paths; logging to
  `%LOCALAPPDATA%`. Deliverable: console app calling Api login against main env.
- **M1 — headless tunnel service.** urnetworkd installs/starts; control pipe; wintun adapter;
  DeviceLocal + packet pump; routes/DNS/MTU; device rpc listener up; a CLI test client
  (DeviceRemote) connects, routes traffic end-to-end, disconnects cleanly. Exit criteria:
  browse the web through a provider; no route loops (R1); clean service stop restores network.
- **M2 — tray shell + full auth.** Tray icon (4 states, theme), flyout with connect toggle +
  status, service session bootstrap (key material exchange), first-run pin-the-icon UX, and
  the FULL auth surface: sign in (email/pw+verify, guest, auth-code), **sign up (create
  network + verify)**, reset password, and **Google + Apple via system-browser OAuth**. Exit:
  sign up or sign in by any method → connect → disconnect from the tray.
- **M3 — full feature parity (connect + provide + account).** ALL connect controls: the
  location/provider picker (REST `findLocations`/provider lists + the `ConnectGrid`),
  connect/disconnect/best-available, selected-location, and the detail sheets (client
  contracts, in-tunnel split/block rules, DNS, throughput chart). **PROVIDE**: provide toggle
  + control mode (never/always/auto/manual) + network mode via `ProvideViewController` (the
  service's DeviceLocal provides; the app's DeviceRemote drives it over the RPC).
  Account/profile/settings, wallet (connect wallet, payout, balance codes, reliability,
  points) + Stripe upgrade, leaderboard, feedback, blocked locations. This is the bulk of the
  UI work — full macOS parity, not a connect-only subset.
- **M3.5 — per-app split tunneling (committed for v1, clean-room, MPL-2.0).** WFP callout
  kernel driver that redirects excluded processes' socket binds to the physical interface —
  the Mullvad/NordVPN-class mechanism — implemented from first principles so the entire
  distribution stays GPL-free and the driver ships under MPL-2.0 like the rest of urnetwork.
  Provenance discipline: implement ONLY from Microsoft documentation (WFP redirect surface:
  `FWPM_LAYER_ALE_BIND_REDIRECT_V4/V6`, `FWPM_LAYER_ALE_CONNECT_REDIRECT_V4/V6`,
  `FwpsRedirect*`, `PsSetCreateProcessNotifyRoutineEx`) and Microsoft's MIT-licensed
  Windows-driver-samples (WFP inspection/proxy samples demonstrate the callout + redirect
  mechanics); published vendor *design write-ups* may inform the behavioral spec; no one who
  writes driver code reads GPL driver source (win-split-tunnel or any other); keep a
  provenance log. Deliverables: behavioral spec first (exclusion semantics: image-path match
  + child-process inheritance; sockets already open at exclusion time; physical-address
  updates on network change; dual-stack; loopback exemption; explicit dnscache/DNS behavior
  for excluded apps), then driver + IOCTL device interface (exclusion list, physical v4/v6
  addresses, enable/disable) + urnetworkd client + excluded-apps settings UI + Driver
  Verifier/stress hardening. Attestation signing (EV cert + Partner Center) starts at M0. See
  R10.
- **M4 — packaging + signing + Store submission.** WiX MSI per arch (silent
  install/upgrade for Store-managed updates), chained WinAppSDK runtime, service + driver
  install/upgrade/uninstall (network restore on uninstall), Authenticode everywhere, arm64
  build via sdk arm64 dll (needs llvm-mingw on the mac build server), Store certification
  pass (R9).
- **M5 — polish.** Notifications, deep links/SSO, launch-at-login, localization, fonts,
  kill-switch (`vpnInterfaceWhileOffline` via user-mode WFP block).

## 5. Risks & investigations

- **R1 — route loop avoidance (top technical risk).** On macOS the NE provider's own sockets
  bypass the tunnel automatically, and on Android the app excludes itself with
  `VpnService.Builder.addDisallowedApplication(packageName)` (`MainService.kt`). Windows has
  NO per-application tunnel exclusion for classic desktop apps — the only true per-app
  redirect is a WFP callout kernel driver rewriting socket binds (how Mullvad implements
  split tunneling), which would reintroduce the driver-signing burden wintun avoids.
  **Primary mitigation — self-exclusion at the socket layer**: the SDK binds its own outbound
  sockets to the physical interface via `IP_UNICASTIF`/`IPV6_UNICASTIF` (the
  wireguard-windows pattern; a socket with a forced egress interface ignores the tun default
  route). Socket creation in `connect` is centralized (`ConnectSettings.NetDialer()` in
  connect/net.go, the QUIC dial in net_extender.go, `ListenUDP` in transport.go + webrtc), so
  a package-level socket-control hook covers everything; expose
  `urnet_set_egress_interface_index(idx)` through cgo and have the service update it from
  `NotifyIpInterfaceChange` (track the best non-tun default route, like wireguard-windows).
  Loopback rpc is unaffected; the tray app needs no exclusion (its api traffic rides the
  tunnel, as on macOS). Fallbacks if needed: endpoint host routes via the physical gateway,
  or a WFP permit+redirect approach. Prototype in M1 before anything else.
- **R2 — WinUI 3 C++ friction** (unverified ergonomics): timebox the M2 flyout; Qt 6.12 LTS is
  the researched fallback with its Win10-pinning tradeoff.
- **R3 — payments**: no StoreKit on Windows; product decision needed before M3 wallet/upgrade
  parity.
- **R4 — wintun maintenance** (0.14.1 since 2021; still the shipped standard across major
  VPNs): low risk now; keep WireGuardNT/DCO trends on the radar.
- **R5 — tray icon overflow default**: mitigated by first-run UX only; no API. Sign
  consistently so the GUID identity survives updates.
- **R6 — DNS leaks.** Windows resolves per-adapter (smart multi-homed resolution), so setting
  the tun's DNS is not enough while other adapters keep their resolvers. Options: NRPT rule
  (registry), interface metrics, or a WFP port-53 block scoped to non-tun interfaces while
  connected. Pick and validate in M1 with a leak test; Win10 has no per-adapter DoH, so DoH
  `tunnelDnsSetting` modes fall back to plain DNS to the in-tunnel resolver there.
- **R7 — IPv6 leaks.** Confirm what the macOS tunnel does for v6; if the tunnel is v4-only,
  Windows must blackhole/block v6 while connected (route ::/1+8000::/1 to tun or WFP block)
  or v6 traffic bypasses the VPN entirely.
- **R8 — multi-user Windows sessions.** One service/tunnel, potentially multiple logged-in
  users. v1 policy: single tunnel owned by the session that authenticated; the mTLS rpc keys
  already gate control (only the owning session holds clientPem); pipe SDDL allows
  authenticated users but config/PEM injection only flows app→service per connection. Fast
  user switching keeps the tunnel as-is.
- **R9 — Store certification (researched 2026-07-09, see `app/STORE.md`).** Confirmed with
  citations: Win32 EXE/MSI listings are accepted (ExpressVPN precedent); service + kernel
  driver installers are allowed (driver has its own attestation gate); no VPN-specific
  prohibition, but policy 10.5.1 makes a privacy policy mandatory; third-party commerce
  (Stripe) is allowed for non-game PC apps (10.8.x); installer needs Authenticode (OV
  enough, MS does not re-sign). **Key correction:** the Store does NOT auto-update EXE/MSI
  apps — we need our own updater (see auto-update above). Remaining unknown is only the live
  submission itself (needs a Partner Center account); STORE.md has the spike checklist.
  Fallback if blocked: direct MSI + winget.
- **R10 — split-tunnel driver ownership (clean-room).** Attestation signing (EV cert +
  Partner Center .cab) is new release infrastructure with lead times — start in M0. The
  clean-room decision (no GPL in the distribution; driver under MPL-2.0) means the hardening
  burden is entirely ours: no battle-tested upstream to inherit years of edge-case fixes
  from, kernel bugs are BSODs, and every race (process launch vs exclusion update, network
  change vs open flows) must be found by our own spec + Driver Verifier + stress testing.
  Expect M3.5 to be the largest engineering item after the UI. Excluded apps' DNS through the
  shared dnscache service is the known hard corner — the spec must define our behavior
  explicitly rather than inherit one. Driver updates must survive Store-managed MSI upgrades
  (stop/replace/restart ordering).

## 7. Open questions

Product — DECIDED 2026-07-09:
- **Payments**: Stripe (checkout via system browser; api already has
  `CreateStripePaymentIntent`) + balance codes.
- **v1 = FULL macOS functionality** (revised 2026-07-10): sign in, sign up, connect (all
  controls), and provide are ALL in v1. Not a trimmed connect-only/sign-in-only slice — the
  desktop is a full-featured node at parity with the macOS app.
- **Auth (full parity — v1)**: sign in (email/password + verify, guest, auth-code) AND sign
  up (create network + verify) AND reset password; Google **and** Apple via system-browser
  OAuth. Apple = web "Sign in with Apple" (a Services ID + `urnetwork://`/loopback return),
  replacing macOS's native `ASAuthorization` — so Apple users get one-tap SSO too, not only
  the auth-code fallback. No native SSO SDKs; everything routes through the system browser +
  the `urnetwork://` (or loopback) callback.
- **Provide (full — v1)**: the desktop is a full node — provide/earn is in v1. Provide toggle
  + control mode (never/always/auto/manual) + network mode, via the SDK `ProvideViewController`
  and provide-mode setters/listeners. The DeviceLocal already provides (it's the same object
  that runs the client tunnel); on Windows the app's DeviceRemote drives the service's
  DeviceLocal provide state over the RPC (in-process on Linux). Ethernet counts as unmetered/
  provide-eligible (NetworkCostType).
- **Connect (full — v1)**: every connect control functional — the location/provider picker
  (REST `findLocations` + provider lists + the `ConnectGrid`/`ConnectViewController` grid),
  best-available, and the detail sheets (client contracts via `ContractViewController`,
  in-tunnel split/block rules via `BlockActionViewController`, DNS settings, throughput
  chart). Not just "connect to best available."
- **Distribution**: Microsoft Store first, as a Win32 EXE/MSI listing (see R9;
  confirmed accepted — ExpressVPN precedent; Stripe third-party commerce allowed;
  mandatory privacy policy per policy 10.5.1). See `app/STORE.md`.
- **Auto-update**: ~~via the Store~~ **CORRECTED 2026-07-09** — the Store does NOT
  push updates for EXE/MSI listings (only MSIX auto-updates). We ship our own
  **service-assisted updater** (`urnetworkd` downloads + swaps binaries, no UAC
  prompt) and upload new installers to the Store per release. See STORE.md.
- **Tunnel persistence after tray quit**: tunnel keeps running (service-owned).
- **Provide defaults**: ethernet maps as unmetered/provide-eligible via NetworkCostType.
- **Per-app split tunneling: IN SCOPE for v1** (M3.5) via a clean-room, MPL-2.0,
  attestation-signed WFP callout driver implemented from first principles (Microsoft docs +
  MIT-licensed official samples only; no GPL source contact — keeps the distribution
  GPL-free); in-tunnel block/split rules port as-is alongside. Note: Windows has no
  user-mode per-app exclusion; the only OS-native per-app VPN (UWP VpnChannel platform) is
  incompatible with this architecture.

Technical (owner: port work, listed by milestone):
- M0/M4: windows/arm64 DLL proof (llvm-mingw on the mac build server; brew formula check).
- M1: R1 socket-binding hook in `connect` + `urnet_set_egress_interface_index`; R6 DNS leak
  mechanism; R7 IPv6 policy; packet-pump throughput measurement (decide whether a batched
  `urnet_device_local_send_packets` cgo export is warranted).
- M2: WinUI 3 C++ ergonomics spike (Qt 6.12 LTS fallback, with its Win10-pinning tradeoff);
  tray light/dark icon switching against `SystemUsesLightTheme` (unverified pattern);
  first-run pin-the-icon UX.
- M4: WER LocalDumps for the service + SCM recovery policy (restart on crash; DeviceRemote
  already queues state across reconnects); localization pipeline (convert xcstrings to an
  app-owned json catalog rather than resw); font licenses for Windows distribution
  (PP Neue Montreal/Bit, ABC Gravity).

## 6. sdk/cgo follow-ups (make usage simpler/faster as we go)

- Endpoint-IP enumeration for R1 exclusion routes if needed (new small export).
- Everything else the port needs is already exported and verified: both `SetRpcServer` sides,
  `GenerateDeviceRpcKeyMaterial` + key-material buffer-outs, packet path
  (`sendPacket`/`addReceivePacket`), `NetworkSpace.toJson`/`importNetworkSpaceFromJson`,
  all 12 view-controller openers on DeviceRemote, `uploadLogs`, `setMemoryLimit`/`freeMemory`.
- The c++ wrapper (`urnetwork_sdk.hpp`) is the app-facing api; raw C header stays the ABI.
