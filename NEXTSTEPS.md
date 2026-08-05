# URnetwork Windows — Next Steps

Concrete, ordered pickup list. Context: `PLAN.md` (architecture/risks), `README.md`
(build), `app/STORE.md` (Store), `app/SIGNING.md` (signing). Everything under
`windows/app/` is written and uncommitted; the SDK-side R1 work is committed-ready
in `connect/`+`sdk/`+`cgo/`.

## Where it stands

- **Verified (on macOS):** R1 socket self-exclusion (`connect/egress*.go` +
  `sdk.SetEgressInterfaceIndex` + cgo `urnet_set_egress_interface_index`) builds
  for darwin + windows/amd64 + windows/arm64; cgo C ABI + `urnetwork_sdk.hpp`
  wrapper regenerate and pass the C++ smoke test; windows/amd64 DLL cross-builds
  via mingw-w64.
- **Written, not yet compiled on Windows:** the whole `windows/app` solution —
  Common lib, `urnetworkd` service (wintun + DeviceLocal + packet pump + net
  config + R1 egress monitor + control pipe), WinUI 3 tray app (SdkHost +
  ServiceClient + tray + Account/Wallet/Leaderboard/Support UI), clean-room
  split-tunnel driver, WiX MSI. Real brand icons generated + committed.

## 0. Running the app: the Windows App Runtime, the log, and `--diagnose`

**URnetwork.exe is a tray app.** A successful launch opens no window — it puts an
icon in the notification area (bottom-right; Windows 11 hides new icons behind
the `^` chevron by default, so look there and drag it onto the taskbar). Left
click opens the window, right click gives Open / Connect / Quit.

Everything on the startup path is logged, from the first instruction of
`wWinMain`:

```
%LOCALAPPDATA%\URnetwork\app\logs\urnetwork-app.log
```

and any failure before the tray icon exists is also shown in a message box
naming the cause and that path — a tray app has no other channel.

### One command to see what happened

```powershell
.\URnetwork.exe --diagnose
```

It prints the same facts the app logs at startup — build/arch, Windows version,
the resolved **Windows App Runtime** path (which carries its version),
`resources.pri`, `URnetworkSdk.dll`, the log file, whether another instance is
running, whether `urnetworkd`'s control pipe is listening — and exits without
starting the UI. Run from a terminal it prints there; double-clicked it shows the
same text in a message box. Paste that output into any bug report.

> PowerShell does not wait on a GUI-subsystem process, so it returns the prompt
> before the output lands under it. To get it in order, pipe it —
> `.\URnetwork.exe --diagnose | Out-String` — or redirect it to a file.

### An unpackaged WinUI 3 app needs the Windows App Runtime installed

`WindowsPackageType=None` (App.vcxproj) makes the Windows App SDK bootstrapper
auto-initialize, but it can only bootstrap a runtime that is **already on the
machine**. If it is missing, the bootstrapper terminates the process from a CRT
initializer — *before* `wWinMain`, so nothing in this app can log or report it.

> **The tell:** you ran the exe and `urnetwork-app.log` does not exist, or has no
> new `startup: wWinMain` line for that launch. That means the process died
> before its own entry point; the runtime is the first thing to check.
> (Windows Error Reporting also logs it: Event Viewer → Windows Logs →
> Application.)

Install it, matching **the same major.minor the app was built against**
(`Microsoft.WindowsAppSDK` in `app/src/App/App.vcxproj` — currently **2.2**) and
**the same architecture as the exe** (x64 or ARM64; an ARM64 build will not run
on an x64 runtime):

1. Download "Windows App SDK runtime — downloads" from Microsoft
   (`https://learn.microsoft.com/windows/apps/windows-app-sdk/downloads`), take
   the **redistributable installer** (`WindowsAppRuntimeInstall-<arch>.exe`) for
   that version, and run it. No admin rights are needed for the per-user install.
2. Or via winget, if the id for that version exists on the box:
   `winget install Microsoft.WindowsAppRuntime.<major>.<minor>`.

Verify:

```powershell
Get-AppxPackage -Name Microsoft.WindowsAppRuntime.* | Select-Object Name, Version
.\URnetwork.exe --diagnose      # "app runtime : present: <path with the version>"
```

Also required next to `URnetwork.exe`: `URnetworkSdk.dll`, `resources.pri`, and
`Microsoft.WindowsAppRuntime.Bootstrap.dll` (the build copies all three into
`build\<arch>\Release`; copy the whole folder, not just the exe). `--diagnose`
reports each one.

Once the WiX MSI is building in CI (plan WP5) it deploys the runtime itself, and
this section becomes the fallback for loose builds.

## 1. Get it building on a Windows box (the gate — nothing else moves until this)

1. Install: VS 2022 (v143) + "Desktop development with C++" + Windows 11 SDK
   (10.0.22621) + the WDK (for the driver); WiX v5 (`dotnet tool install --global wix`);
   vcpkg (manifest mode).
2. Produce the SDK Windows zip **in the build VM**: `windows/build-sdk.ps1`
   (Go + llvm-mingw, provisioned by `all/windows/packer/scripts/provision.ps1`) →
   `sdk/cgo/build/URnetworkSdkWindows.zip`. (No mac cross-toolchain needed.)
3. On Windows: `windows/app/tools/fetch-deps.ps1 -SdkZip <path to the zip>`
   (fetches pinned wintun, unzips the SDK, builds the import libs).
4. `msbuild URnetwork.sln /p:Configuration=Release /p:Platform=x64`.
   **Expect iteration on the WinUI 3 app project (R2)** — the flagged surface:
   - Verify NuGet versions in `src/App/packages.config` against the installed
     Windows App SDK; adjust the import paths in `App.vcxproj` to match.
   - Fix C++/WinRT idioms that were written blind: `AppController.cpp` uses
     `window_.try_as<implementation::MainWindow>()` to call impl methods — switch
     to `winrt::get_self<implementation::MainWindow>(window_)`. The
     `IWindowNative`/`IInitializeWithWindow` interop includes may need `<microsoft.ui.xaml.window.h>`.
   - The tray + SdkHost + ServiceClient are plain Win32/C++ and independent of
     the XAML toolchain — they should compile straight away.

## 2. Prove M1 (the service tunnel) end-to-end on Windows

1. `urnetworkd.exe console` (dev mode), then the tray app: log in → connect.
2. **Confirm R1 (top risk):** with the tunnel up, the service's own platform +
   provider sockets must NOT loop into the tun. Watch `EgressMonitor` set the
   egress interface; verify browsing works through a provider and there's no
   route loop. This is the payoff of the socket self-exclusion.
3. Clean service stop restores the network (routes/DNS reverted).

## 3. arm64

- The arm64 DLL builds in the VM via `windows/build-sdk.ps1` (llvm-mingw targets
  both arches). No mac llvm-mingw needed; the mac only builds the Linux SDK (zig).
- Build the solution for ARM64; smoke-test on an arm64 Windows box/VM.

## 4. Close the leak guards (R6/R7) — implement + validate in M1

- **R6 DNS leaks:** Windows resolves per-adapter, so setting the tun DNS isn't
  enough. Add an NRPT rule (or a WFP port-53 block scoped to non-tun interfaces)
  while connected; validate with a DNS-leak test. Win10 has no per-adapter DoH —
  fall back to plain DNS to the in-tunnel resolver.
- **R7 IPv6 leaks:** confirm whether the tunnel is v4-only; if so, blackhole/block
  v6 while connected (route `::/1`+`8000::/1` to the tun, or a WFP v6 block).

## 5. Split-tunnel driver hardening + signing (R10)

- The driver is process-based `BIND_REDIRECT` with the real source-address
  rewrite (service supplies it). Remaining:
  - Implement the loopback fixup (the complementary connect-redirect that reverts
    the source for local destinations — spec'd in `driver/README.md`, deferred).
  - Driver Verifier + stress + leak testing (the hardening burden is ours since
    this is clean-room, not upstream).
  - Stand up attestation signing (`app/SIGNING.md`): EV cert + Partner Center
    Hardware Dev Center; per-arch `.cab` submission. **Start the cert/account
    enrollment now — lead times.**
  - Test load on clean Win10 + Win11 without test-signing mode.

## 6. Service-assisted updater (Store won't auto-update EXE/MSI — STORE.md)

- The Store does NOT push updates for EXE/MSI listings. Build a
  service-assisted updater: `urnetworkd` downloads + swaps binaries (no UAC),
  the app checks for updates. Keep the Authenticode signer identity stable
  across updates (the tray-icon GUID registration is bound to it — R5).

## 7. Microsoft Store submission (R9 — needs Partner Center)

- Follow the `app/STORE.md` certification-spike checklist: enroll Partner Center,
  register the OV cert, submit a minimal service-installing MSI first, then add
  the driver feature. Confirmed admissible: Win32 EXE/MSI + service + driver +
  Stripe commerce; mandatory privacy policy (10.5.1).

## 8. Full-parity UI (M2/M3 — the bulk of v1; revised 2026-07-10)

v1 = full macOS functionality, so the UI written so far (sign-in + connect-best +
basic panels) is only a subset. Build out, wired to the SDK view controllers /
`Api` already exported in the cgo surface:
- **Full auth:** sign up (create network + verify), reset password, guest, and
  **Google + Apple via system-browser OAuth** (`urnetwork://`/loopback callback;
  Apple = web Sign-in-with-Apple, replacing the native SDK).
- **Location/provider picker:** REST `findLocations`/provider lists + the
  `ConnectGrid` — connect to a chosen country/region/city, not only best-available.
- **Connect detail sheets:** client contracts (`ContractViewController`),
  in-tunnel split/block rules (`BlockActionViewController`), DNS, throughput.
- **Provide:** toggle + control mode (never/always/auto/manual) + network mode
  via `ProvideViewController`; the app's DeviceRemote drives the service's
  DeviceLocal provide over the RPC.
- **Wallet depth:** connect wallet, payout selection, balance-code history,
  reliability, points; **Account/Leaderboard/Support** to full parity.

### iOS-parity additions from `apple/DESKTOP2.md` (verified SDK surfaces)

The macOS app was made a superset of iOS; bring the same to Windows. All use the
cross-platform SDK already in the cgo surface (via the app's `SdkHost` /
DeviceRemote → service):

- [ ] **Onboarding/Introduction flow** (DESKTOP2 §1) — welcome → plan/paywall →
  participate-to-earn → refer, gated by `isPro`/introduction-complete; "get more
  data" opens it. Gating via `Api::subscriptionBalance`.
- [x] **Guest mode** (DESKTOP2 §2) — **wired end to end, not yet compiled**:
  "Try Guest Mode" on the initial step opens `AuthSheets` GuestModeSheet (terms
  consent) → `SdkHost::LoginAsGuest` (`Api::networkCreate{guest_mode,terms}` →
  register client, mirroring linux); the plan cards' "Create an account" routes a
  guest into the create step's guest-upgrade mode → `SdkHost::UpgradeGuest`
  (`Api::upgradeGuest`, verify supported) → re-register under the new jwt.
  (`upgradeGuestExisting` — linking a guest to an existing login — remains open.)
- [ ] **Account menu** (DESKTOP2 §3) — logout, referral share link
  (`Api::getNetworkReferralCode`), create-account. (Windows already has an Account
  panel — add the menu actions.)
- [ ] **Copy client ID** (DESKTOP2 §4) — `DeviceRemote::getClientId()` → clipboard,
  in the contract-details view.
- [x] **Wallet sign in + connect wallet (Solana and Bittensor)** (DESKTOP2 /
  `apple/BITTENSOR.md`) — **wired end to end, not yet compiled**:
  `src/App/WalletConnect.{h,cpp}` drives the `ur.io/wallet-connect` bridge for both
  providers (Solana keeps the NaCl envelope; Bittensor is a single `signMessage`
  returning plain `?address=<ss58>&signature=<0xhex>`), `SdkHost::SignInWith{Solana,
  Bittensor}` → `authLogin{wallet_auth{blockchain=SOL|TAO}}`, sign-in buttons on the
  login panel (Bittensor before Solana), and connect-wallet by address on the Wallet
  page (`walletValidateAddress` per chain → `createAccountWallet`).
  **`urnetwork://` protocol activation is now handled**: the MSI already registered
  the scheme; `main.cpp` single-instances on `AppInstance::FindOrRegisterForKey` and
  redirects a second launch (the browser returning the wallet callback) to the
  running app, which routes it on `AppInstance::Activated` →
  `AppController::HandleDeepLink` → `SdkHost::HandleDeepLink`. Remaining: set
  `UrnWalletConnectProjectId` (see `src/App/Config.h`), deploy ur.io/wallet-connect,
  and do a real-wallet test.
  > **No create-network-with-wallet path.** A wallet with no network still only gets
  > an error ("this wallet isn't linked to a network yet") because the app has no
  > sign-up UI at all. Both wallets are equally affected; fix it with the sign up
  > work above (`NetworkCreate{wallet_auth}` is already in the cgo surface).

> **Wrapper-signature reconciliation (correctness, do first on-Windows):** the cgo
> wrapper now uses `(std::optional<Result>, std::optional<std::string> err)`
> callbacks. `SdkHost.cpp`'s auth callbacks were reconciled to this, but the rest of
> the app (MainWindow.xaml.cpp view models, etc.) may still use the old
> single-arg `(Result)` form and must be swept to match, or it won't compile.
- [ ] **Country search** on the add-blocked-location surface (DESKTOP2 remaining).
- n/a **In-app review prompt** — the Store has `StoreContext.RequestRateAndReviewAppAsync`
  if wanted, but it's optional; not required for parity.

> Cross-platform note: guest mode is already implemented on Linux
> (`linux/app/src/SdkHost.cpp::LoginAsGuest`) — reuse that flow as the reference.

## 9. Polish (M5)

- ~~Localization~~ **done**: the app reads the shared store
  (`urnetwork/localizations`, 28 locales). `Strings/<locale>/Resources.resw` is
  generated (`npm run gen`) and indexed into `resources.pri` by MakePri; the UI
  goes through `Localization.h` (`Localized` / `Format` / `Plural`). No string
  lives in the app: add or change one in `localizations/keys/*.yaml`.
- Toast notifications, launch-at-login (HKCU Run), kill-switch
  (`vpnInterfaceWhileOffline` via WFP).
- Not localized yet, and the store has no keys for them: `StatsFormat.cpp` (byte
  and bit-rate units, `"unknown"`, and the compact relative times `now` / `5s
  ago` / `3m ago` / `2h ago`) and the leaderboard row (`"%d.  %s  —  %.1f MiB"`).
  They are unit abbreviations and numeric formatting, and they match macOS today.

## Open decisions

- Store fallback: if certification stalls, ship direct MSI + winget first
  (documented in R9).
- Provide-on-desktop default (ethernet = unmetered via NetworkCostType) — confirm
  the UX once the service runs.
