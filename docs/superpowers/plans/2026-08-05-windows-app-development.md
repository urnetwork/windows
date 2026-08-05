# URnetwork Windows — development plan

2026-08-05. The solution now compiles and links for x64 and ARM64 (CI run
31020657340, the first ever). This plan takes it from "binaries exist" to "a
usable client with Android's function set", executed by parallel agents.

## Ground truth (verified, not assumed)

- **~14k LOC of C++ that has never run.** It compiled with zero errors of its
  own — every failure so far was environment or SDK. Compiling is not running:
  nothing in this codebase has executed a single line.
- **Two processes.** `URnetwork.exe` = per-user WinUI 3 (C++/WinRT) tray app
  holding a `DeviceRemote`. `urnetworkd.exe` = LocalSystem service owning the
  wintun tunnel, `DeviceLocal`, packet pump, network config, egress monitor.
  They talk over the SDK's own mTLS WebSocket RPC on loopback; the named pipe
  carries only lifecycle/config. Same split as apple, so the **DeviceLocalRpc
  reliability bridge already applies here**.
- **It is a TRAY app.** `main.cpp` single-instances via `AppInstance`, then
  `Application::Start`; the window is opened from the tray (`OnLaunched`
  creates the tray + SDK host). "No UI on launch" is partly by design — but see
  WP1, because a silent exit looks identical.
- Sizes: `MainWindow.xaml` 696 lines, `MainWindow.xaml.cpp` 2143, service
  ~1500 across 8 files, `SdkHost.cpp` is the DeviceRemote surface.
- `WindowsPackageType=None` is set, so the App SDK bootstrapper auto-initializes
  — but an unpackaged app still needs the **Windows App Runtime installed on
  the machine**, and if it is missing the process exits silently.

## Dependencies and branches

Everything tracks **`beta/custom-server`** in every repo — windows, sdk,
connect. The sdk beta line now carries the merged reliability checkpoint, the
DeviceLocalRpc bridge, and the three cgo fixes this client needed as the first
C-ABI consumer. All work in this plan commits to `beta/custom-server`.

## Verification reality

- **CI is the only compiler for the solution** (`beta-build.yml`: push, PR,
  manual). ~7 min per round, both arches, msbuild log uploaded on failure.
- **A local MSVC exists** at `Microsoft Visual Studio\18\BuildTools` — usable
  to syntax-check individual translation units and the SDK header without CI.
  Use it before pushing; it is far cheaper than a round trip.
- **Nobody here can run the app.** No Windows box with admin rights is
  available to this session, and the service needs LocalSystem + a tun device.
  Runtime verification is the OWNER's, so every runtime-affecting change must
  ship with a way for the owner to see what happened (logs, visible errors) —
  a change the owner cannot observe is not done.

## The rule this project keeps relearning

A mechanism with no observable signal does not exist. This codebase has never
run, so *nothing* in it is known to work. Prefer failing loudly over failing
silently, and add the log line before the feature.

---

## WP1 — Make it start, and make failure visible (BLOCKING)

Everything else waits on this. Nobody knows whether the app runs, and the one
symptom we have ("opened the exe, no UI") is consistent with at least four
causes that look identical from outside:

1. It started fine and put an icon in the notification area (tray apps have no
   window on launch) — the owner may simply not have looked there.
2. The Windows App Runtime is not installed, so the auto-bootstrapper fails
   before `wWinMain` reaches XAML, and the process exits silently.
3. `AppInstance::FindOrRegisterForKey` threw (App SDK unavailable), same
   silent exit.
4. XAML/resources failed to load (`resources.pri` missing or unindexed), so
   the window never materializes.

Work:
- Add **startup logging from the first instruction of `wWinMain`**, to a file
  under `%LOCALAPPDATA%\URnetwork\logs\` (follow `Common/` logging if it
  exists; if not, the smallest thing that writes and flushes). Log: process
  start, App SDK runtime presence/version, single-instance decision, XAML app
  start, `OnLaunched` entry, tray icon creation result, window creation.
- Convert every silent failure path on that route into a **visible** one: a
  message box (the app has no UI yet, so a box is the only channel) naming the
  cause and the log path. Specifically wrap the App SDK calls in `main.cpp`.
- Make the tray icon's creation failure detectable (`Shell_NotifyIcon` returns
  a BOOL that is currently unchecked in most codebases — verify).
- Add a `--diagnose` (or `console`) argv path that prints the same diagnostics
  to stdout and exits, so the owner can run one command and paste the output.
- Document in `NEXTSTEPS.md` how to install the Windows App Runtime, since an
  unpackaged WinUI 3 app cannot run without it and the MSI is not built yet.

**Acceptance:** the owner runs the exe (or `--diagnose`) and gets either a
tray icon or a specific named reason. No silent exit remains on the startup
path.

## WP2 — Service bring-up and the R1 deadlock risk

`urnetworkd` has never run. `NEXTSTEPS.md` names R1 as the top risk: with the
tunnel up, the service's own platform and provider sockets must NOT loop into
the tun, or the client deadlocks against itself.

Work:
- Same treatment as WP1 for the service: log every lifecycle step
  (`console` mode already exists per NEXTSTEPS — verify it), wintun adapter
  creation, `DeviceLocal` construction, RPC server start, egress interface
  selection, route/DNS application, and clean revert on stop.
- Audit `EgressMonitor` + `SetEgressInterfaceIndex` wiring against connect's
  `egress*.go` — confirm the index the service picks is the physical adapter
  and is applied BEFORE the tunnel routes are installed.
- Audit `NetworkConfig` revert: every route/DNS change must be undone on stop
  AND on crash (the service dying with routes installed leaves the machine
  without network — the worst failure this app can have). Verify there is a
  restore path that survives an abnormal exit; if not, add one.
- Do NOT attempt to make the tunnel work end to end here — that needs the
  owner's machine. Deliver: it starts, it logs what it did, it reverts.

**Acceptance:** `urnetworkd console` runs and logs a complete startup and a
clean shutdown; a written checklist the owner can follow to confirm R1 on a
real box.

## WP3 — UI parity audit against Android (analysis, then build)

The Windows UI already has files for the main surfaces (`MainWindow.xaml` 696
lines, plus Auth/Balance/Location/Stats sheets, `TrayIcon`, `UsageBar`,
`TransferChart`, `WalletConnect`). Nobody knows how complete or correct any of
it is, because it has never rendered.

Android reference (`claude_sandbox_android/app/app/src/main/java/com/bringyour/network/ui/`):
`connect`, `account`, `settings`, `stats`, `wallet`, `payout`, `profile`,
`leaderboard`, `balance_codes`, `blocked_regions`, `feedback`, `login`,
`introduction`, `components`, `theme`.

Work — **audit first, build second**:
1. Produce a parity matrix: every Android screen/feature → the Windows file
   that implements it (or MISSING), with a one-line state assessment read from
   the code. Commit it as `docs/superpowers/reports/2026-08-05-ui-parity.md`.
2. From the matrix, identify the **connect screen** critical path (the app's
   reason to exist: connect/disconnect, provider selection, status) and bring
   exactly that to a state the owner can exercise once WP1 lands.
3. Theme: extract the Android theme values (`ui/theme`) and check them against
   `UrColors.h`. Desktop can be richer, but it must not be a *different* brand.

**Acceptance:** the parity matrix exists and is honest (MISSING where missing);
the connect path is coherent in code and reviewed.

## WP4 — Reliability surface (the desktop advantage)

Windows has the same `DeviceRemote` split as apple, so the reliability bridge
is already available: settings, metrics, exits, migrate, probe-all,
simulate-network-change, destination exits.

Work:
- Wire a developer/diagnostics view equivalent to the android developer screen
  and the apple `DeveloperView`. Read those two as references — the apple one
  is the closer architecture (RPC, polling, nil-tolerance).
- Respect the traps already paid for on other platforms: settings edits are a
  read-modify-write of the WHOLE struct from a FRESH read (never a cached
  snapshot); nil settings mean "nothing in force", not "everything off"; poll
  only while visible; never block the UI thread on an RPC.
- Desktop can go further than mobile: a real window means the exits table,
  metrics, and `[rel]`-style event stream can be shown properly rather than in
  a phone-sized list. Design for that, but do not gold-plate before WP1/WP2
  prove the app runs.

**Acceptance:** compiles both arches; the view degrades correctly with the
service down (which is the normal state until WP2 lands).

## WP5 — Packaging and delivery

- The WiX MSI (`app/installer/`) and the split-tunnel driver are excluded from
  the solution's build configurations. Get the MSI building in CI (WiX v5 via
  `dotnet tool install --global wix`) and produce an installer artifact, so the
  owner installs rather than copying loose binaries — this also solves the
  Windows App Runtime dependency properly.
- Leave the driver alone for now (needs the WDK and a signing story;
  `app/SIGNING.md` and `STORE.md` describe the intended path).

**Acceptance:** CI produces an MSI artifact that installs the app + service.

---

## Execution protocol

- **WP1 and WP2 run first and in parallel** (different processes, disjoint
  files: `src/App/main.cpp` + startup path vs `src/Service/*`). WP3's audit
  runs in parallel with both (read-only until the matrix exists). WP4 and WP5
  start after WP1 is green in CI.
- **Every agent gets a reviewer** before its work is accepted, per the protocol
  that has caught a build break, a settings-zeroing trap, and a permanent
  refresh wedge on this project already. Reviewer charter: correctness against
  the reference platform, silent-failure paths, lifetime/threading (this is
  C++ with a UI thread and a service — use-after-free and cross-thread UI
  access are the risks), and Windows-specific resource handling (HANDLE/COM
  lifetime, `Shell_NotifyIcon` teardown).
- **Syntax-check locally with MSVC before pushing.** CI rounds are 7 minutes;
  a local `cl /c` on the changed TU is seconds.
- **Manager (top-level session) cross-checks**: no agent may point CI at a
  branch other than `beta/custom-server`; no agent commits generated artifacts
  from a CRLF checkout without checking the diff is content and not line
  endings; every runtime claim is labelled as unverified until the owner runs it.
- **Final passes**: one integration reviewer over the full diff, one auditor
  over the process, as on the ios port.

## Owner decisions (2026-08-05)

- **Brand fonts: supplied, licensed, in hand.** ABC Gravity (extended +
  extra-condensed), PP Neue Bit Bold and PP Neue Montreal Regular are tracked
  in the android repo at `app/app/src/main/res/font/`. Copy them into this
  repo and wire them (Phase A4) — no substitute families, no licensing
  question outstanding.
- **UI scope: the audit's exclusions stand, EXCEPT seedphrase login, which is
  IN.** Dropped as mobile-shaped: onboarding carousel, introduction funnel,
  the overlay set, identicons, and the android-only settings (cellular
  provide, notification permissions, battery optimisation). Seedphrase auth is
  a real login path and desktop users need it, so it is in scope for the
  auth work.

## Known risks

- **Nothing is runtime-verified.** Treat every "works" claim as "compiles".
- The owner is the only one who can run the app or the service; plan the work
  so each delivery gives the owner something specific to look at.
- The service is LocalSystem and rewrites routes and DNS. A bad revert path
  can leave a machine with no network. WP2 treats that as the highest-severity
  item in this plan.
- CRLF checkouts silently break generator regexes here (already cost one
  round); check generated diffs with `--ignore-all-space` before committing.
