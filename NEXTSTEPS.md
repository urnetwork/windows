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

## 1. Get it building on a Windows box (the gate — nothing else moves until this)

1. Install: VS 2022 (v143) + "Desktop development with C++" + Windows 11 SDK
   (10.0.22621) + the WDK (for the driver); WiX v5 (`dotnet tool install --global wix`);
   vcpkg (manifest mode).
2. On the **macOS build server**, produce the SDK Windows zip:
   `make -C sdk/cgo build_windows` → `sdk/cgo/build/URnetworkSdkWindows.zip`.
   (arm64 needs `make -C sdk/cgo init` first for llvm-mingw — see step 4.)
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

- On the mac build server: `make -C sdk/cgo init` (installs llvm-mingw + zig),
  then `make -C sdk/cgo build_windows` produces the arm64 DLL too.
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
- [ ] **Guest mode** (DESKTOP2 §2) — `Api::networkCreate{guest_mode=true,terms=true}`
  → register client (mirror the Linux `SdkHost::LoginAsGuest`); "Try Guest Mode"
  entry + guest→full-account **upgrade** (`Api::upgradeGuest`/`upgradeGuestExisting`).
- [ ] **Account menu** (DESKTOP2 §3) — logout, referral share link
  (`Api::getNetworkReferralCode`), create-account. (Windows already has an Account
  panel — add the menu actions.)
- [ ] **Copy client ID** (DESKTOP2 §4) — `DeviceRemote::getClientId()` → clipboard,
  in the contract-details view.
- [~] **Solana wallet connect / Sign-in-with-Solana** (DESKTOP2) — core **DONE**:
  `src/App/WalletConnect.{h,cpp}` (Win32 glue: ShellExecute, CryptBinaryToString,
  same SDK crypto/envelope as Linux/macOS) + `SdkHost::SignInWithSolana`/
  `HandleDeepLink`/`AuthLoginWithWallet` + vcxproj entries. **Two integration
  points remain (WinUI, unverifiable here):** (1) a "Sign in with Solana" button in
  `MainWindow.xaml` calling `SignInWithSolana`; (2) register `urnetwork://` (MSI
  registry / package manifest) + handle protocol activation
  (`AppInstance`/`ProtocolActivatedEventArgs` in `App.xaml.cpp`) → route the URI to
  `SdkHost::HandleDeepLink`. Then deploy ur.io/wallet-connect + real-wallet test.

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

- Localization (convert `apple/.../Localizable.xcstrings` ~22 langs to an
  app-owned JSON catalog), toast notifications, launch-at-login (HKCU Run),
  kill-switch (`vpnInterfaceWhileOffline` via WFP).

## Open decisions

- Store fallback: if certification stalls, ship direct MSI + winget first
  (documented in R9).
- Provide-on-desktop default (ethernet = unmetered via NetworkCostType) — confirm
  the UX once the service runs.
