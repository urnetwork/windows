# D6 + P7 — implementation plan

2026-08-08, written against `beta/custom-server` `256fd03`. Two independent
work packages. **D6 is safe and can run unattended. P7 rewrites the machine's
routing table and DNS and is the highest-severity work in this repo.**

Read first: `docs/superpowers/reports/2026-08-08-ui-state.md` (UI vocabulary and
harness traps) and `docs/superpowers/specs/2026-08-06-ios-parity-native-shell.md`
§ "The product tiering" (Advanced Mode scope).

---

# D6 — wire the fault-injection and probe actions

## Status: DONE 2026-08-08 (uncommitted at time of writing)

All seven bridged and verified against a live `DeviceRemote` over
`urnetworkd console --rpc-only`, unelevated. Both stale comments deleted and
replaced with notes citing the header/`.def` lines, so the claim is checkable
rather than re-litigable. Changed: `SdkHost.h`, `SdkHost.cpp`,
`DeveloperPage.cpp`, `DeveloperPage.h`. 33 new `dev_*` ids for upstream.

**Grep trap for the next person:** the exports are snake_case C ABI names
(`urnet_device_remote_drop_exit`), not camelCase — a camelCase grep finds nothing
and looks like confirmation that the methods don't exist. That is exactly how the
stale comments survived.

**Bug that only running found:** `migrateExit` returns **-1** for an exit not in
the window — a not-found sentinel, not a count — and the first implementation
rendered "affected **-1**". `< 0` now declines; `0` is deliberately *not* treated
as a sentinel, because "migrated 0 flows" is a real answer. The `int64_t`
signature alone would never have surfaced this.

**Honest gaps — do not read the above as full coverage:**
- The beta-test account has insufficient balance, so it never forms a provider
  contract and the exits table is always empty. **No Drop/Stall/Migrate against a
  real exit was possible.** The bridge and the decline path are proven; a
  successful drop is not.
- Requeue is a **weak pass**. Nothing replayed across three service restarts, but
  the app does not re-bootstrap its session in-process, so the replay opportunity
  was never created. Structurally there is no queue in the added path;
  empirically it is unproven.
- `getConnectedProviderLocations()` skipped: clean to bridge, but renders empty
  with zero connected providers and `ConnectPage` lacks the serial bridge
  `DeveloperPage` has — shipping it would mean shipping unverified UI.
- `getPacketStats()` skipped — see the correction under "Also bridgeable" below.

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

  > **CORRECTION (2026-08-08): this instruction was wrong; it was correctly
  > refused during D6.** There is exactly one device-level `contractVc_`
  > (`SdkHost.cpp:1786`), so its packet stats are **device-wide**, while the
  > inspector's `packetCount`/`byteCount` are **per-block-action**. Rendering
  > device-wide in/out under per-connection rows is the right shape around the
  > wrong number. The existing comment at `ConnectPage.cpp:1352–1355` already
  > says this and is correct — leave it. Do not bridge this field for the
  > inspector.

- `DeviceRemote::getProbeResults()` → per-target DNS / connect / TTFB / total
  latency. **`*List` → `ReadSdkList`.**

**Not available, do not fake:** ~~protocol, source/destination port,~~ per-flow
process attribution, ASN/org, per-connection duration, per-connection RTT. The
egress interface index for the status strip is **not** in `proto::TunnelStatus` —
the driver knows it; surfacing it is a service-side change, not a client one.

> **CORRECTION (2026-08-08): protocol and both ports ARE available.**
> `DeviceLocal::setFlowOwnerLookup` (`urnetwork_sdk.hpp:9568`, `:10066`) invokes a
> callback carrying `version, protocol, source_ip, source_port, destination_ip,
> destination_port` and asks us who owns the flow. Those are handed to us as
> arguments — and the same callback is the race-free moment to do a socket→PID
> lookup, because the socket is live by construction. That makes **per-flow
> process attribution reachable too**, without WFP, a driver, or elevated
> privileges.
>
> Two constraints on using it: it is `DeviceLocal`-only, so it lives in the
> **service** process and any design must cross our mTLS RPC boundary; and its
> calling cadence and thread context are **unverified** — spike that before
> building on it (task #23). Separately, `BlockAction` has no ports or owner, so
> joining a flow table to it can only key on IP: two processes hitting the same
> IP collapse into one row. Disclose that (Portmaster's `" or "` pattern) rather
> than picking a winner.
>
> Full analysis, including why the presumed user-mode WFP route is a dead end
> (`FWPM_NET_EVENT_HEADER3` has no PID field; the event log is a ~128 KB circular
> buffer holding ~100–150 events; it logs drops, not allows):
> `docs/superpowers/research/2026-08-08-windows-traffic-logging.md`.

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

> **UPDATE 2026-08-08 — prerequisites landed in front of the gates.** Four research
> passes and a non-destructive prep pass have run since this was written. Nothing
> elevated was executed. What changed:
>
> **The data-path architecture is validated — keep what we built.** wintun + the
> 31-prefix complement route set + `IP_UNICAST_IF` self-exclusion. The prefix table
> was verified arithmetically (31 aligned, non-overlapping, union = `0/0` minus the
> three RFC1918 ranges). The `0.0.0.0/1`+`128.0.0.0/1` idea is a *Linux* `wg-quick`
> technique requiring `fwmark` + `suppress_prefixlength` — **do not adopt it**.
>
> **R6 and R7 are confirmed live on the test machine, not risks.** The tunnel is
> v4-only and the box has a global routable IPv6 address and a v6 default route, so
> Windows prefers v6 per RFC 6724 and most real browsing bypasses the VPN entirely.
> Ethernet's resolver sits inside the deliberately-excluded `192.168/16`, so DNS
> leaks by construction. **Gate F as written would fail, and Gates D/E would be
> testing a tunnel that leaks by design.**
>
> **Recommended reordering:** build the user-mode WFP leak layer (task #20) and fix
> the recovery-path defects (task #21) *before* the first elevated run, rather than
> running the gates against a known-leaky configuration and re-running them after.
> This reorders the plan; it does not expand it.
>
> **Defects found by reading, not yet fixed** — all in task #21:
> - `main.cpp:211` sweeps the route table (`SweepOrphanedTunnel(remove=true)`)
>   *before* the control-pipe conflict is detected at `:216`. `RevertNetwork` and
>   `RunConsole` both guard with `ControlPipeInUse()`; the SCM path does not. So
>   `sc start urnetworkd` against a live console tunnel deletes its 31 routes and
>   clears its DNS while the console still reports Up — the exact scenario
>   `main.cpp:320-328` calls unacceptable, in the one path missing the guard.
> - `NetworkConfig.cpp:178-185` — a **DNS failure inside `Apply` is not rolled
>   back**. `applied_ = true`, `Apply` returns true, the tunnel reports
>   `state=up`/`routes_installed=true`, and the only signal is a `LogWarn`. The UI
>   says Connected while every query goes out in the clear. Needs `dns_applied` on
>   `TunnelStatus`.
>
> **Crash recovery works, but not for the reason the code comment gives.**
> `TerminateProcess` runs no user code — not the exception filter (`main.cpp:159`),
> the terminate handler (`:172`) or the console handler (`:178`). **`CrashRevert`
> does not run.** Routes come back because wintun never calls `SwDeviceSetLifetime`,
> so process death is a PnP *surprise removal*; `DIF_REMOVE` never runs, so a
> phantom devnode and registry residue survive. "The network came back" and "nothing
> was left behind" are different claims and only the first is guaranteed. Requires
> wintun **≥ 0.14** (confirmed: we ship 0.14.1).
>
> **Gate E is safe to attempt**, for a reason not previously stated: the tun route
> set deliberately excludes the private ranges (`NetworkConfig.cpp:66-88`), so even
> a lingering adapter leaves `10/8`, `172.16/12` and `192.168/16` on the physical
> path — router, LAN and the local resolver all stay reachable. Worst realistic
> outcome is "no internet, LAN and local elevated shell intact". The hotspot is a
> nice-to-have, not the primary recovery.
>
> **Environment blockers for Gate F:** Tailscale is running (a second wintun 0.14.1
> consumer with 2 NRPT rules and its own v6 address) and must be stopped or the
> leak results are uninterpretable. `C:\ProgramData\URnetwork\service` is machine-
> wide and deliberately not redirectable (`Paths.cpp:44-50`) — one marker, one log,
> one adapter GUID — so **only one build may exist on the box during P7**.
>
> **Standing hazard:** `WintunDeleteDriver` detaches *every* wintun adapter on the
> machine including other vendors' (the mechanism behind three tracked outages, and
> Tailscale is installed here). We resolve the export but never call it. That is
> correct — do not "tidy up" by calling it.
>
> Reports: `docs/superpowers/reports/p7-baseline/` (capture script, stability-proven
> baseline, `p7-gates.ps1`) and `docs/superpowers/research/2026-08-08-windows-{tunnel-datapath,leak-prevention-wfp,traffic-logging,split-tunneling}.md`.

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

**Gate A — adapter, unelevated-fail. PASSED 2026-08-08 — but the wording below was
wrong, and a gate that proves nothing is worse than no gate.**

~~Confirm `console` (no `--rpc-only`) fails cleanly at step 1 without elevation,
naming elevation as the cause.~~

**The console does not fail.** `RunConsole` logs the elevation error at
`main.cpp:385-391` and **continues, serving the pipe**. Step 1 only executes when
a client sends `start_tunnel`. As originally written this gate executes no tunnel
code at all and looks like a pass. To actually run it you must drive it: send
`start_tunnel{mode:tunnel}` over the named pipe. That is safe — an unelevated
token cannot create a wintun adapter, and steps 6–8 are double-guarded
(`TunnelController.cpp:312` mode check, `:320` null-adapter check).

Observed result, build SHA256 `CD0814BF…`:

```
ERR: wintun: CreateAdapter failed: 5
ERR: tunnel: start FAILED at step 1/8 wintun (mode=tunnel): failed to create the
     wintun adapter (needs LocalSystem/admin and a loadable wintun driver)
INF: tunnel: stopped, no network state to restore (nothing was applied)
```

`routes_installed:false`, `state:error`, error 5 = `ERROR_ACCESS_DENIED`, baseline
byte-identical afterwards.

**Record the corollary: the elevation guard is the OS, not our code.** Nothing in
the C++ refuses to proceed unelevated — `WintunCreateAdapter` returning
`ERROR_ACCESS_DENIED` is the only thing that stops it. Adequate, but not what
this plan implied.

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
