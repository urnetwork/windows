# URnetwork Windows — iOS parity on a native Windows shell

2026-08-06. Design approved by the owner. Supersedes the build order in
`docs/superpowers/reports/2026-08-05-ui-parity.md` §5, which sequenced against
Android and assumed the tunnel came first.

## What changed about the goal

Two owner decisions set the shape of this work:

1. **Reference platform is the iOS/macOS app, and parity means everything it
   has.** `claude_sandbox_ios/app/network` — ~28k LOC of Swift across 190
   files. The earlier audit's "deliberately not doing" list (introduction
   funnel, overlays, identicons, onboarding) is **withdrawn**: those are in.
2. **Connect logic is LAST.** The order is: talk to the APIs, log in, developer
   settings, general settings, everything else — then the tunnel. This inverts
   `NEXTSTEPS.md`, which made the service the blocking gate.

The consequence is that the plan must deliver a driveable, verifiable client
*before* a single packet moves.

## The product tiering (owner decision, 2026-08-08)

The owner's words: *"iOS and Android apps was built simple. Windows, linux and
other desktop apps we will make will be more advanced in nature. … For the
Normal windows app I wanna make it slightly more advanced in nature and feature
rich and also develop an Advanced Mode view that changes everything in view."*
Portmaster is the named reference: one switch that surfaces advanced logs and
configs EVERYWHERE in the UI, for people who know what they are doing.

So iOS parity is the FLOOR of this client, not its ceiling. Three tiers:

1. **Mobile parity** — everything iOS has. (The original goal; largely done.)
2. **Windows Normal** — parity plus desktop-grade depth by default: the
   developer/reliability destination, real tables, the status strip, richer
   diagnostics than mobile ships. This is where the client already is.
3. **Advanced Mode** — a persisted app-wide toggle (Settings, developer
   section). Not a separate page: it changes what EVERY surface shows.
   Inline diagnostics, raw values, IDs and event streams appear across the
   app; Normal hides them. Same pages, deeper view.

Design rule that follows: every surface built from here on has a Normal
reading and an Advanced reading, and pages built earlier gain their Advanced
reading when the mode lands. `LiveStats.rawConnectionStatus`/`rawConnected`
(built for "the one place that should see through the clamp") generalise:
Advanced Mode is that idea applied everywhere.

Known implementation anchors for the mode: standing state like the mode
notice (a bootstrap-time reason must survive until a view exists to show
it); persisted in LocalState alongside routeLocal; every page gets an
ApplyAdvancedMode() the way it has ApplyStrings(). The DeviceLocal-only
holes (dropExit, stallExit, shuffleExits, probe-suite getters) become
in-scope to bridge, since Advanced Mode is where they would surface.

**Scope settled (owner, 2026-08-08): Advanced Mode is read AND write.**
The owner's words: *"advanced mode can let users tune the SDK themselves
with all the settings and such we made. Along with log views and advanced
details. For normal mode we assume the VPN just works normally and is
optimized to work as is."* So:

- **Advanced** = the full reliability-settings surface (all 34, live),
  the fault-injection and probe actions, log views (app log + service
  glog + the `[rel]` event grammar), raw values and IDs everywhere.
- **Normal** = curated defaults, no tuning. The VPN presents as a thing
  that works, not a thing you operate.
- Consequence drawn here (overrule if wrong): the **Developer destination
  folds behind the Advanced Mode toggle**. The earlier "leave it visible"
  decision predates the toggle existing; a Normal user whose VPN "just
  works" has no 34-knob destination in the nav. Advanced Mode shows it
  and adds the inline surfaces everywhere else.
- The SDK bridge work (dropExit/stallExit/shuffleExits, the probe-suite
  getters, and the int32 action counts the bridge currently drops) is a
  prerequisite and proceeds in the sdk repo as its own phase.

## Ground truth (verified 2026-08-06, not inherited from docs)

- `app/tools/build-local.ps1` builds the solution here in **4s incremental**,
  0 warnings, 0 errors. CI remains the reference build (both arches, pinned
  v143 toolset); it is not the inner loop.
- **The app runs on this machine.** Launched, posted a synthetic tray click,
  captured the window. The login screen renders with brand fonts and the
  correct palette, offering email, Bittensor, Solana and auth-code sign-in.
- **The service has never brought up a tunnel.** `urnetworkd` is not installed;
  the single launch attempt on 2026-08-05 initialized the Go SDK and wrote a
  3-byte log.
- `SettingsView` contains three controls (`MainWindow.xaml:761-774`) against
  iOS's `SettingsForm-macOS.swift` (621 LOC) + `SettingsView.swift` (356) +
  `SettingsViewModel.swift` (365).
- The connect-button defects from the 2026-08-05 audit are **fixed**:
  `OnConnectToggle` reads `SelectedLocation()` (`MainWindow.xaml.cpp:1082`) and
  the status line is driven by `connectionStatus` (`:1717`).
- `getProviderGridPointList` has **no consumer anywhere in `app/src/App`**. The
  grid listener is subscribed at `SdkHost.cpp:839` and the points are dropped
  at `:876`.
- **The window never calls `AppWindow.Resize()`.** It opens at ~1920×1094 — a
  tray app filling the entire work area.

## The keystone: `--rpc-only`

`TunnelController::StartLocked` (`app/src/Service/TunnelController.cpp:110-247`)
is an eight-step sequence, and the destructive part is fenced:

| Step | Action | Touches the machine's network? |
|---|---|---|
| 1/8 | wintun adapter created | adds an interface, **no address, no route** |
| 2/8 | R1 egress bind to the physical NIC | no |
| 3/8 | NetworkSpace opened | no |
| 4/8 | `DeviceLocal` constructed | no |
| 5/8 | **mTLS RPC listener started** | no |
| 6/8 | `netConfig_->Apply` — address, MTU, routes, DNS | **YES — first destructive act** |
| 7/8 | split-tunnel driver | no |
| 8/8 | packet pump | moves packets |

The code says so itself at step 1: the adapter "carries no address or route yet
so it cannot attract traffic while we set the rest up."

A start mode that returns after step 5 therefore gives the app a **live
`DeviceRemote` with zero risk to the routing table**. This is what makes
"connect logic last" buildable rather than a promise — without it, the
developer screen and every connect control are UI nobody can exercise.

## Reachability classes

Every surface falls into exactly one, and this determines what can proceed in
parallel and what carries risk.

- **Class A — in-process `Api`.** Runs against the `NetworkSpace` the `SdkHost`
  already owns. No service, no RPC, no SDK work. Covers login, profile,
  settings, auth methods, wallet, payouts, points, reliability, leaderboard,
  blocked locations, referral, delete account, Stripe. **Zero risk, fully
  parallelizable.**
- **Class B — `DeviceRemote` over RPC.** Needs the service in `--rpc-only`
  mode. Covers the developer/reliability surface, connect controls, locations,
  provide, DNS, split rules. `DeviceRemote` already exposes the complete
  `open*ViewController` set plus the reliability bridge ported for iOS
  (sdk#135) — **no SDK work is required for any of it**.

  **How to actually run it** (landed in P1):

  ```powershell
  .\urnetworkd.exe console --rpc-only    # unelevated, no UAC prompt
  .\URnetwork.exe                         # no environment variable needed
  ```

  One switch is enough. The clamped service serves every `start_tunnel` as
  rpc-only and the app **adopts** that session rather than refusing it, so the
  app comes up driveable. `URNETWORK_RPC_ONLY=1` exists to request rpc-only from
  an *unclamped* service and is not required for the workflow above. (It is
  parsed as an explicit allow-list — `1`/`true`/`yes`/`on`; anything else is
  off and logs a warning.)

  Two properties hold for every rpc-only session, and a Class-B screen must not
  assume it can ignore either: the connect surface is **clamped at the source**
  — `SdkHost::ReadStats` forces `connectionStatus` to the unrecognised
  `"RPC_ONLY"` and zeroes `connected`, provider count and throughput, so nothing
  downstream can render "Connected" — and a **persistent, non-dismissible
  notice** is pushed through `SdkHost::SetModeNoticeHandler` saying no traffic
  is carried. Any screen that renders connection state binds that handler and
  keeps the notice visible for the life of the session. **P2 owns the view for
  it; P1 shipped the signal only** (see P1's report).

  P1 also makes the mode impossible to enter by accident: the service is clamped
  for the life of the process, and the app refuses to ask a service older than
  control-protocol v2 for rpc-only, because such a service silently ignores the
  field and builds a real tunnel.
- **Class C — the tunnel.** Routes, DNS, packet pump. The owner's to run.

Known Class-B holes, flagged so nobody assumes them: `dropExit`, `stallExit`,
`shuffleExits` and the probe-suite getters are `DeviceLocal`-only and have no
`DeviceRemote` equivalent. A developer screen must not offer them without an
SDK/service change; treat that as separate work.

## Design decisions

### The component kit comes before the screens

"Native shell, brand content" is a systematic property or it is nothing. iOS
ships `UrButton`, `UrCard`, `UrTextField`, `UrSwitchToggle`, `UrSnackbar`,
`UrLabel`. Windows has only `URButton` (added in `af12548`). Building the rest
first — native WinUI metrics, brand skin — means each of the ~20 remaining
screens is assembled from correct parts instead of making the native/brand
tradeoff twenty separate times.

### `MainWindow.xaml.cpp` must be split before the feature phases

It is 2128 lines and every new screen currently lands in it. It is the one file
every parallel agent would touch. Split into per-page units — `ConnectPage`,
`AccountPage`, `SettingsPage`, `WalletPage`, `DeveloperPage` — each owning its
own file, with `MainWindow` keeping only navigation wiring and the shared
`sheetOpen_` guard. This is a prerequisite for parallelism, not a cleanup.

### Native shell, brand content — the concrete list

Native: window sizing (~480×760 default) and position persistence, Mica
backdrop with a solid `#101010` fallback on Win10, `ExtendsContentIntoTitleBar`
with the brand wordmark and real caption buttons, standard WinUI control
metrics and hover/focus states, `NavigationView` idiom, native context menus.

Brand, unchanged: the palette (already byte-correct against Android across
11/11 tokens), ABC Gravity / PP Neue Montreal / PP Neue Bit, the connect canvas
hero, the brand colour dots.

## Phases

| | Phase | Class | Depends on |
|---|---|---|---|
| P0 | Native shell + component kit + page split | — | — |
| P1 | Service `--rpc-only` + app session bootstrap | — | — |
| P2 | Developer / reliability screen | B | P0, P1 |
| P3 | Settings + account long tail | A | P0 |
| P4 | Wallet / payouts / points | A | P0 |
| P5 | Auth completion | A | P0 |
| P6 | Connect hero canvas | B | P1, P2 |
| P7 | Connect logic / tunnel | C | all |

P0 and P1 ran in parallel but were **not** disjoint, as originally assumed. P1
needed the app to request and report the session mode, so it added ~420 lines to
`src/App/SdkHost.{h,cpp}` on top of its `src/Service/` work. It touched no view
file — `MainWindow.*`, `AppController.*`, `App.xaml*`, `TrayIcon.*`, none of
P0's new page units — so the two still merge cleanly: the overlap is `SdkHost`
only, and P1's connect-status clamp survives P0's page split because it lives at
the source in `SdkHost::ReadStats`, not in the view.

P3, P4 and P5 are Class A and run in parallel once P0 lands.

**Running several phases at once on one machine.** Every worktree builds the
same executable names and, by default, shares one per-user storage root — one
SDK LocalState (JWT, instance id), one `rpc_session.json`, one log. Two
concurrent agents therefore corrupt each other's state and read each other's
evidence. P1 added the escape hatch; use it:

```powershell
$env:URNETWORK_APP_ROOT = 'C:\Users\...\wt-pN\.localstate'
```

`app/tools/build-local.ps1` also used to kill every `URnetwork`/`urnetworkd` on
the machine by name, so one agent's build terminated another's running app; it
is now scoped to its own worktree's output by path. And a screenshot harness
must select the app **by executable path** (`MainModule.FileName`), never by
process name, window title or class — all three are identical across worktrees,
so a harness picking the first match reports on somebody else's binary.

### Per-phase surface mapping (iOS file → Windows target)

**P2** — `Developer/DeveloperView.swift` (742), `ReliabilityStore.swift` (543).

> **P2 will hit this first — a live `device_` crashes the app on a null list.**
> `parseJson<ThroughputPointList>` (`urnetwork_sdk.hpp:16272`) guards a NULL
> `char*` at `:16269` but not the four-byte string `"null"`, so it throws
> `json.exception.type_error.302 "type must be array, but is null"` out of an
> ordinary getter — unhandled, the app dies at startup. Go-side cause:
> `GetThroughputPoints` returns a non-nil list whose `values` slice is nil,
> `MarshalJSON` renders that as `null`, and `exports_gen.go` short-circuits only
> on a nil pointer. The same unguarded shape is at `SdkHost.cpp:1280`, `:1311`,
> `:1338`, `:1392` and `:1773`.
>
> This is pre-existing and P1 did not fix it, but **P1 changed its
> reachability**: auto-adopt is the first configuration that reliably produces a
> live `device_` without a real tunnel, so P2 is where it starts firing. Fix it
> at the SDK wrapper (treat `"null"` as an empty list) rather than at each call
> site.
>
> **Bind the session notice.** P1 ships the signal only —
> `SdkHost::SetModeNoticeHandler`, delivering `ModeNotice{active, kind,
> requestedTunnel, message}`. There is **no consumer on any branch**, so an
> adopted rpc-only session currently shows no banner and a failed session
> resume shows nothing at all. `Kind::RpcOnly` means a live session carrying no
> traffic; `Kind::SessionFailed` means there is no session and the user is
> **still signed in** — it must not route anyone to the sign-in screen. Render
> `message` verbatim (it is a complete sentence; adding a title duplicates the
> words), keep it visible, and give it no dismiss control. The handler may be
> invoked with `SdkHost`'s lock held: marshal to the UI thread and return.

**P3** — `SettingsForm-macOS.swift` (621), `SettingsView.swift` (356),
`SettingsViewModel.swift` (365), `AddAuthSheet.swift` (529), `AuthMethods.swift`,
`AuthCodeCreate`, `UpdateReferralNetworkSheet`, `ProfileView`/`ProfileViewModel`,
`BlockedLocationsView` (204) + `AddBlockedLocationSheet`,
`PostQuantumIdentityPanel` + `ShareSheet` + `ProviderIdentitiesView`,
`ExportLogsButton`/`LogExportService`, `DePinHubSettingsLinkRow`,
`AccountPreferencesViewModel`.

**P4** — `WalletsView` (394), `PopulatedWallets`, `EmptyWallets`, `WalletIcon`,
`PayoutWalletTag`/`PayoutWalletViewModel`, `PayoutItemView`, `PaymentsList`,
`AccountPaymentsViewModel`, `AccountPointsBreakdown`/`AccountPointsStore`,
`NetworkReliabilityView` (203)/`NetworkReliabilityStore`, `LeaderboardView`
completion.

**P5** — `LoginSeedphrase` + `SeedphraseDisplayView`, `CreateNetworkInstant`,
`UrGoogleSignInButton` (system-browser OAuth), `NetworkServerSheet`,
`Introduction/*` (4 files), `AccountMenu`, `ReferralShareLink`, `ReferBar`,
`LoginCarousel`.

**P6** — `ConnectButton/` canvas states (Connected, Connecting, Disconnected,
Error, ProcessingSubscription) + `StatusIndicator`.

## Verification protocol

The rule this project keeps relearning: **a mechanism with no observable signal
does not exist**, and this codebase's history is that reading it does not find
the bugs — running it does. Three crashes were found in one 40-second loop that
no amount of review had found.

Every phase, before it is called done:

1. `app/tools/build-local.ps1` green locally (4s). CI is not the inner loop.
2. **Launch and drive the app on this box.** Loop: kill, launch, post
   `WM_APP+1` with `NIN_SELECT` (`0x0400`) in `LOWORD(lParam)` to the hidden
   `URnetworkTrayWindow`, screenshot, read
   `%LOCALAPPDATA%\URnetwork\app\logs\urnetwork-app.log`. ~40 seconds.
3. A reviewer subagent before any work is accepted, per the protocol that has
   already caught a build break, a settings-zeroing trap and a permanent
   refresh wedge on this project.

Two harness traps, both paid for:

- **`FindWindow` will not locate the tray window from a PowerShell P/Invoke
  harness** unless the `DllImport` sets `CharSet=CharSet.Unicode` — the default
  marshals ANSI into the `W` entry point and silently returns 0. Enumerate with
  `EnumWindows` + `GetClassNameW` instead; it is immune. (`PLAN.md`'s note that
  the window can simply be found by name is incomplete.)
- **The DPI trap**: the app is per-monitor-DPI-aware and works in physical
  pixels. A harness that is not DPI-aware reads 1536×875 where the app logs
  1920×1094. Call `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` first.

P7 is the only phase that genuinely requires the owner at the keyboard.

## Constraints and risks

- **No new strings.** The localization store carries 916 English keys and the
  code uses 248. Payouts, points, blocked locations, kill switch, device name,
  auth codes, multipliers, delete account and the developer surface are all
  already in `app/src/App/Strings/*/Resources.resw`. Add keys to
  `urnetwork/localizations` only if a surface genuinely has none.
- **Everything tracks `beta/custom-server`** in windows, sdk and connect. No
  agent points CI at another branch.
- **The git index on this machine disappears mid-session** (antivirus). Verify
  `git ls-files` count before every commit — baseline **154 files at
  `dcdec98`** — and confirm `git diff --cached --name-status` lists only
  intended files. A partial index silently commits a truncated tree.
- **CRLF checkouts break generator regexes here.** Check generated diffs with
  `--ignore-all-space` before committing.
- Reliability settings are a read-modify-write of the **whole** struct from a
  **fresh** read; a nil read means "nothing in force", not "everything off".
  Getting this wrong writes an all-zero override that disables the stack and
  latches on sync re-apply.
- Nothing in the service is runtime-verified. Every "works" claim about
  `urnetworkd` means "compiles" until P1 proves otherwise.
