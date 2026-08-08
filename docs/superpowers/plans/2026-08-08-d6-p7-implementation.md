# D6 + P7 — implementation plan

2026-08-08, written against `beta/custom-server` `256fd03`. Two independent
work packages. **D6 is safe and can run unattended. P7 rewrites the machine's
routing table and DNS and is the highest-severity work in this repo.**

Read first: `docs/superpowers/reports/2026-08-08-ui-state.md` (UI vocabulary and
harness traps) and `docs/superpowers/specs/2026-08-06-ios-parity-native-shell.md`
§ "The product tiering" (Advanced Mode scope).

---

# D6 — wire the fault-injection and probe actions

## Why this exists

Advanced Mode is **read and write** by owner decision: *"advanced mode can let
users tune the SDK themselves with all the settings and such we made."* The read
half shipped. The write half — fault injection and the probe suite — is not
wired, and **two comments in our own code say it cannot be**:

- `SdkHost.cpp:2460–2463`
- `DeveloperPage.h:23–26`

Both claim `dropExit` / `stallExit` / `shuffleExits` / the probe suite exist only
on `DeviceLocal` with no `DeviceRemote` equivalent. **That stopped being true
when S1 landed.** In the vendored SDK the app now builds against, all seven are
declared on `DeviceRemote` (`urnetwork_sdk.hpp:10114–10150`), implemented over
the C ABI, and exported (`urnetwork_sdk.def:334–370`). The bridging is pure
additive `SdkHost` work.

**Delete those comments as part of this change.** A stale "this is impossible"
comment is worse than no comment: it already cost one agent a scoping decision.

## Scope

1. **`SdkHost` methods** for the seven, following the existing
   `RunReliabilityAction` pattern: `DropExit(clientId)`, `StallExit(clientId,
   bool)`, `ShuffleExits()`, `StartProbeSuite(config)`, `StopProbeSuite()`,
   `ProbeSuiteRunning()`, `GetProbeResults()`.
   - Guard every `*List` unwrap with `ReadSdkList` (`Common/Sdk.h`).
     `GetProbeResults` returns a list — with a live session, seven of eleven list
     getters were observed throwing `type_error.302` because Go marshals a nil
     slice as the 4-byte document `null`.
   - These are **fault injection**: they must be immediate-or-nothing, never
     queued into sync state. The SDK side already guarantees this (S1 added
     `TestDeviceRemoteAdvancedModeActionsAreNeverQueued`); do not add client-side
     retry that reintroduces it.
2. **Return the counts.** `migrateExit` / `probeAllExits` return `int64_t` across
   the ABI; `RunReliabilityAction` discards them **by choice**, so the UI says
   "Requested:" where it could say "Migrated N flows". Surface them.
3. **Expose in Advanced Mode**, per-exit in the Developer surface's exits table
   and/or the Home inspector's exit rows: Drop / Stall / Unstall per exit,
   Shuffle for the window, and a probe-suite run with its results. Use the
   existing `adv_*` / `dev_*` fallback for labels — **no English literals**.
4. **Destructive-action discipline.** These degrade a live connection on purpose.
   Each needs a confirmation appropriate to Advanced Mode (the audience knows
   what they are doing — do not add a modal to every click), and the log must
   name what was done to which exit.

## Also bridgeable today — take if clean, else report

These would complete the inspector and are all already exported:

- `Device::getConnectedProviderLocations()` → per-exit country/region/city/lat-lon
  **and `ConnectedSinceMillis`**. Turns the inspector's "Session exit country"
  into a real per-connection country and gives a genuine connected-since.
  **`*List` → must use `ReadSdkList`.**
- `ContractViewController::getPacketStats()` → directional packets *and* bytes
  (Remote/Local/Block × Egress/Ingress). Struct-shaped, no guard needed. This is
  what turns the inspector's "(total)" labels into in/out.
- `DeviceRemote::getProbeResults()` → per-target DNS / connect / TTFB / total
  latency. **`*List` → `ReadSdkList`.**

**Not available, do not fake:** protocol, source/destination port, per-flow
process attribution (only via the `DeviceLocal`-only `setFlowOwnerLookup`, which
lives in the service process), ASN/org, per-connection duration, per-connection
RTT. The egress interface index for the status strip is **not** in
`proto::TunnelStatus` — the driver knows it; surfacing it is a service-side
change, not a client one.

## Verification

`--preview-ui=connect` + `URNETWORK_PREVIEW_SAMPLE=1` for layout. For the actions
themselves you need a live device: `URNETWORK_NETWORK_HOST=beta-test.net`,
`urnetworkd.exe console --rpc-only` (unelevated, structurally cannot touch
routes), seedphrase at `C:\Users\ryanm\Downloads\urnetwork-beta-test-account.txt`.
**Never** paste that phrase anywhere. Kill `URnetwork` only, never `urnetworkd`.

Acceptance: each action fires against a live `DeviceRemote` and its effect is
visible in the exits table or the log; counts render; no action is queued across
an RPC reconnect.

---

# P7 — the tunnel

## Status: NOT AUTHORISED TO RUN. Plan only.

The owner said *"Save step 7 for after my manual approval and review of
everything"* and has since said *"Next is D6 and P7"* in the context of asking
for **a plan to hand off**. This document is that plan. **An agent executing it
must obtain explicit confirmation before the first elevated run**, and must never
run the elevated path unattended.

## Why it is the highest-severity work here

`urnetworkd` runs as **LocalSystem**, creates a wintun adapter, rewrites the
routing table and DNS. The worst failure this product can have is leaving the
machine with no network. Nothing in steps 6–8 has ever run.

## What already exists (verified)

`TunnelController::StartLocked` is an eight-step sequence
(`app/src/Service/TunnelController.cpp`):

| step | line | action | touches the network? |
|---|---|---|---|
| 1/8 | 147 | wintun adapter created | adds an interface, **no address, no route** |
| 2/8 | 195 | R1 egress bind to the physical NIC | no |
| 3/8 | 225 | NetworkSpace opened | no |
| 4/8 | 235 | `DeviceLocal` constructed | no |
| 5/8 | 254 | **mTLS RPC listener** | no |
| 6/8 | 326 | `netConfig_->Apply` — address, MTU, routes, DNS | **YES — first destructive act** |
| 7/8 | 354 | split-tunnel driver | no |
| 8/8 | 364 | packet pump | moves packets |

Steps 1–5 are proven: `--rpc-only` runs them unelevated every day in this
project. **Only 6–8 are unproven.**

Recovery surface already built and unit-verified (`NetworkConfig.h`):
`Apply` / `Revert` / `DeleteTunnelRoutes` / `ClearTunnelDns` / `ArmCrashRevert` /
`DisarmCrashRevert` / `CrashRevert` / `SweepOrphanedTunnel`, plus the active
marker (`SetActiveMarker` / `TakeActiveMarker` / `PeekActiveMarker`) that lets a
later start tell an orderly shutdown from a crash. `urnetworkd revert` exists as
an out-of-band escape hatch — **but it refuses while `ControlPipeInUse()`**, so
the console process must be stopped first. That advice is already in the banner.

## Preconditions — all of these before any elevated run

1. **A second network path off the machine** (phone hotspot, second NIC) so a
   wedged routing table is recoverable without this app.
2. A **System Restore point** or equivalent, and the current
   `Get-NetRoute -AddressFamily IPv4` and `Get-DnsClientServerAddress` captured
   to a file for byte comparison afterwards.
3. `urnetworkd revert` verified to run **from a second elevated shell** while the
   service is stopped.
4. The owner physically present. This step is theirs.

## Execution order (each gate must pass before the next)

**Gate A — adapter, unelevated-fail.** Confirm `console` (no `--rpc-only`) fails
cleanly at step 1 without elevation, naming elevation as the cause. Already
observed; re-confirm on the current build.

**Gate B — elevated, adapter only.** Elevated `console`, allow step 1, abort
before 6. Confirm: adapter appears with **no address and no route**; routing
table byte-identical to baseline; Ctrl+C removes the adapter; no marker left.

**Gate C — R1 socket self-exclusion (the top risk).** With the adapter up and
before routes are installed, confirm the `egress:` log line names the **physical**
adapter and its source address, and that `Get-NetTCPConnection -OwningProcess
<urnetworkd pid>` shows no socket sourced from the tun's `169.254.2.1`. If the
service's own platform/provider sockets route into the tun, the client deadlocks
against itself. **This is the gate that matters most.**

**Gate D — routes, then immediate revert.** Allow step 6/8. Immediately capture
routes and DNS; then Ctrl+C and confirm `Revert()` restores both to the
byte-identical baseline. Do this **before** ever running the pump.

**Gate E — crash revert.** Repeat D, then `Stop-Process -Force` instead of
Ctrl+C. Confirm `CrashRevert` / the marker path restores the network on the next
start. **This is the test that matters**, because it is the real failure mode.

**Gate F — traffic.** Only now allow 7–8. Browse through a provider. Then:

- **R6 DNS leak**: Windows resolves per-adapter, so setting the tun's DNS is not
  enough while other adapters keep resolvers. Validate with a DNS-leak test; pick
  and implement NRPT or a WFP port-53 block scoped to non-tun interfaces. Win10
  has no per-adapter DoH — plain DNS to the in-tunnel resolver is the fallback.
- **R7 IPv6 leak**: confirm whether the tunnel is v4-only. If so, v6 must be
  blackholed or blocked while connected (`::/1` + `8000::/1` to the tun, or a WFP
  v6 block) or v6 traffic bypasses the VPN entirely.
- **Kill switch**: `Device::setRouteLocal` is already wired in the UI
  (kill switch = `!routeLocal`) and verified to persist across a restart. Confirm
  it actually blocks when the tunnel drops.

**Gate G — the service proper.** Only after F: install as a real Windows service,
auto-start, SCM recovery policy, and confirm the tunnel survives a reboot and
that an abnormal service exit still reverts.

## Rules for the executing agent

- **Stop and ask before the first elevated run.** Never run elevated unattended.
- Never leave the machine without confirming the routing table is restored — take
  a baseline capture at the start of every session and diff it at the end.
- Report each gate as pass/fail with the actual command output. A gate that was
  not run is not a pass.
- Do not attempt gates D–G on a machine that is the owner's only network path.
- If anything goes wrong: stop the service, run `urnetworkd revert` from a
  separate elevated shell, and report — do not iterate on a broken routing table.

## Out of scope for P7

The split-tunnel WFP driver (needs the WDK and attestation signing —
`app/SIGNING.md`), the WiX MSI, and Store submission. Those are M4/M5.
