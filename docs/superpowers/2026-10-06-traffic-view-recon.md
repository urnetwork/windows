# Traffic-view recon — what a Portmaster-style live traffic view can eat

Date: 2026-10-06. Scope: read-only recon of this repo (`urnetwork-windows`), plus the
sibling `sdk/` and `connect/` checkouts only where the vendored DLL's behavior needs
explaining. Question: what can a future advanced-mode "live traffic view" (Portmaster
class: per-connection / per-app live feed) consume from this codebase, and what does
not exist yet.

Bottom line up front:

- The Connect pane already runs a **1 Hz, 60 s-window throughput feed and a live
  per-routing-decision connection list** — but none of it comes over the app's
  control pipe. All live stats flow app-side over the **device RPC** (mTLS loopback
  into the service's DeviceLocal) via SDK view controllers that push, not poll.
- The control pipe (`\\.\pipe\urnetwork.control`) has **no stats verb at all**. Its
  only counter is `tunnel_local_up_millis`.
- The service keeps **per-packet counters only** (no bytes, no windowing) for its
  dead-tunnel failsafe, and it has a full **per-flow → owning-exe attribution engine
  (FlowOwner)** that today feeds only the SDK's app-pinning seam — nobody reads it
  for stats.
- The split-tunnel driver **exists in-repo but is control-only** (no stats IOCTL),
  is shipped only under `-IncludeDriver`, and its README calls it a hardening-gated
  skeleton.
- The vendored SDK exposes **nothing per-app and nothing per-flow** stats-wise. The
  finest grain it offers is per-host/IP routing decisions (with packet/byte counts)
  and per-provider (exit) rows.

---

## (a) What the Connect pane samples TODAY, and how

### The control pipe carries no stats

`app/src/Common/Protocol.h` — the complete verb set (`urnw::proto::msg`):

- app→service: `hello`, `start_tunnel`, `stop_tunnel`, `get_state`,
  `set_split_tunnel`, `set_kill_switch`, `logout` (Protocol.h:66-72)
- service→app: `reply`, `event` (Protocol.h:73-74); the only pushed event is
  `tunnel_state` (`ControlServer::PushState`, Service/ControlServer.cpp:112-132).

`TunnelStatus` fields (Protocol.h:216-303): state, rpc endpoint/identity, error,
service/protocol versions, **`tunnel_local_up_millis` — the only counter on the
pipe**, plus mode/routes/dns/wfp/egress indices/stop_reason/failsafe_armed. The
header says so itself: *"best-effort counters (authoritative stats come over the
device RPC)"* (Protocol.h:228). `ControlServer::Handle` confirms no stats branch
(Service/ControlServer.cpp:35-101).

### The real feed: SDK view controllers over device RPC, listener-push

The app's `SdkHost` opens SDK presentation controllers against its `DeviceRemote`
and subscribes to push listeners (`SdkHost::SubscribeStats`, App/SdkHost.cpp:2745-2783;
throughput at 3071-3072; block actions/stats at 3080-3083). The design note is
explicit: *"Pushed to the UI on SDK listener callbacks (macOS parity: listener-push,
not polling)"* (App/SdkHost.h:88-89).

**Sampling rate lives inside the SDK's ContractViewController** (sibling
`sdk/contract_view_controller.go:29-30`): `defaultThroughputSampleInterval = 1s`,
`defaultThroughputWindowDuration = 60s`. Its `run` loop polls
`device.GetPacketStats()` / `GetProviderPacketStats()` / `GetExtenderStats()` once
per second (contract_view_controller.go:309-316) — each is a synchronous
`DeviceLocalRpc.GetPacketStats` etc. call into the service (sdk/device_rpc.go:5155,
5319, 1401; server side 10567, 10873, 11823) — and appends one
`ThroughputPoint{Remote,Local,Block}` per route, zero-holding gaps. The throughput
listener fires **every 1 s tick while any retained point in the window is active**,
plus one idle snapshot when it goes quiet (contract_view_controller.go:344-382).

The per-1s-sample shape (`urnet::ThroughputSample`,
third_party/urnetwork-sdk/amd64/urnetwork_sdk.hpp:3107-3121): egress/ingress byte
counts, egress/ingress packet counts, egress/ingress bit rates, per route
(Remote/Local/Block). Window seconds come from
`contractVc_->getWindowDurationSeconds()` (App/SdkHost.cpp:3267-3268).

### What shows live throughput right now

- **Three TransferCharts** (Remote / Blocked / Local), built in
  `ConnectPage::BuildCharts` (App/ConnectPage.cpp:1296-1305), fed by
  `SdkHost::PublishThroughput` → `SetThroughputHandler` →
  `remoteChart_->SetPoints(points, windowSeconds)` etc. (ConnectPage.cpp:1411-1421;
  publisher at SdkHost.cpp:3260-3331). `TransferChart` redraws on `Tick()` at ~10 fps
  and labels rolling 5-bucket averages + sliding peaks (App/TransferChart.h:1-10).
- **A "↓ x ↑ y" rate line** on the activity pane from `LiveStats.downBitsPerSecond /
  upBitsPerSecond` (ConnectPage.cpp:1081-1087), which `ReadStats` takes from the most
  recent Remote throughput point (SdkHost.cpp:2840-2856).
- **The window status strip** shows the same pair
  (App/MainWindow.xaml.cpp:1010-1011, fed at 2462-2463).
- **TransportBar**: the window's remote bytes partitioned by transport
  (`TransportDistributionSnapshot`, App/SdkHost.h:328-377; built ConnectPage.cpp:1311-1322),
  published only on change (SdkHost.cpp:3269-3277, 3327-3329).
- **IpFamilyStatusRow** (dualstack/v4/v6 counts) and **ExtenderPanel**, both under
  the charts (ConnectPage.cpp:1323-1333).
- **The Connections list** (activity pane): every SDK block/routing decision,
  newest first, verdict-colored (blocked/allowed/local-bypass), with per-decision
  `timeMillis / byteCount / packetCount`, group-by-host fold, verdict filter and
  search — incremental reconcile because *"the feed pushes several times a second"*
  (ConnectPage.cpp:1966-2104, esp. the header comment at 2074-2087; row render
  2031-2072). This is already the closest thing to Portmaster's connection list.
- **Advanced-mode inspector**: per selected connection, joined against exit routing
  tables refreshed every ~5 s (`RefreshExitRouting`, ConnectPage.cpp:3250-3299; tick
  gate at 3583-3586; reads `SdkHost::ReadReliability` → `getExits` /
  `getDestinationExits` / `getReliabilityMetrics`, SdkHost.cpp:4348-4363).
- **Client contracts sheet**: per-peer contract circles with per-contract used/total
  bytes and live bit rate (`ContractPeerRow`/`ContractEntry`, App/SdkHost.h:195-248;
  sheet described in App/StatsSheets.h:29-40).
- **Earnings page**: provider/extender throughput series on the same 1 s tick
  (`ProviderThroughputSnapshot`, App/SdkHost.h:379-402).

---

## (b) What the service tracks

`app/src/Service/` is thin on stats — by design the SDK owns counters and the app
reads them over device RPC.

- **Packet counters, packets only**: `PacketCounters{outbound,inbound}` —
  `std::atomic<uint64_t>` incremented per packet in the pump
  (Service/PacketPump.h:44-47; increments in PacketPump.cpp's outbound loop and
  inbound callback). Read at 1 Hz by `TunnelWatchdog` for the dead-tunnel failsafe
  (PacketPump.h:24-47 documents that consumer; wired at
  TunnelController.cpp:1064). **No byte counts, no windowing, no per-destination.**
- **Uptime only**: `upSinceMillis_` → `tunnel_local_up_millis`
  (TunnelController.cpp:882, 1788-1789, 1848).
- **FlowOwner — the sleeper asset** (Service/FlowOwner.h/.cpp): a full per-flow
  app-attribution engine. A background worker enumerates
  `GetExtendedTcpTable`/`GetExtendedUdpTable` (v4+v6) every ~1 s
  (`kRefreshInterval{1000}`, FlowOwner.h:225) into bounded caches
  (5-tuple→pid, 8192 entries; pid→exe image path, 4096), and exposes a cache-only,
  never-blocking `Lookup`. It is installed into the SDK via
  `device_->setFlowOwnerLookup(...)` at tunnel start
  (TunnelController.cpp:783-806; SDK seam declared at urnetwork_sdk.hpp:15640,
  16297). **Today the SDK uses it only for per-app pin routing decisions**
  (connect/ip_remote_multi_client.go:1906-1933 — the lookup resolves the pinned app
  for a flow; routing_classifier_light.go:32 consumes it). **Nothing counts bytes
  or flows per app anywhere** — FlowOwner answers "which exe owns this 5-tuple",
  and that answer currently goes to routing, not to any stats store.
- **EgressMonitor / NetworkConfig**: physical egress interface discovery/pinning
  and Wi-Fi signal level only (Service/EgressMonitor.h:1-60). No traffic counters.
- **WfpPolicy**: kill-switch firewall policy state (surfaced as `wfp_state` string),
  no counters.
- **WindowTrace**: opt-in (`URNETWORK_SDK_TRACE`) high-frequency log sampler of the
  SDK's exit window — diagnostics to the service log, not a stats feed
  (Service/WindowTrace.h).

### Split-tunnel driver status

- **It exists in-repo**: `app/driver/Driver.c` (505 lines), `Ioctl.h` (84),
  `SplitTunnel.inf`, `SplitTunnel.vcxproj`, `README.md`, `PROVENANCE.md`. Clean-room
  WFP callout that rebinds excluded processes' sockets at `ALE_BIND_REDIRECT`.
- **Ship gating**: the MSI includes `SplitTunnel.sys` only when built with
  `-p:IncludeDriver=true` (`app/installer/Installer.wixproj:17-20`;
  `Package.wxs:141-145` component, `243-248` feature). The service registers/starts
  the driver on demand and *"if the driver isn't shipped or won't load, all
  operations are graceful no-ops"* (Service/SplitTunnelClient.h:1-6).
- **Maturity**: README "Status" — *"Skeleton + spec … must go through Driver
  Verifier + stress + leak testing before ship (plan R10)"* (driver/README.md:66-72).
- **Attribution potential**: the IOCTL surface is entirely control-input —
  `SET_ENABLED`, `SET_PHYSICAL_ADDRS`, `SET_EXCLUDED_PATHS`, `CLEAR`, `SET_MODE`
  (driver/Ioctl.h:31-56). **There is no query IOCTL: no per-app counters, no flow
  table, no events.** At classify time the driver *has* the owning process id
  (README.md:19-22), so per-app verdict/byte attribution is technically reachable
  from that seam, but it would be new kernel code plus a new query/event IOCTL —
  the biggest-ticket option, and gated on the driver shipping at all.
- The app drives the path set live: `SdkHost::PushLocalOverrideAppsToDriver` maps
  SDK local-override app ids → `service_.SetSplitTunnel(paths, allowlist)`
  (App/SdkHost.cpp:3607-3628; re-pushed on every override change at 3084-3087).

---

## (c) What the vendored SDK exposes, stats-wise

Headers: `app/third_party/urnetwork-sdk/{amd64,arm64}/urnetwork_sdk.h` (4667 lines),
`urnetwork_sdk.hpp` (29500), `urnetwork_sdk.def`, plus the DLL/.lib. A second copy
staged under `.local-deps/chk/windows/amd64` and `.local-deps-new/unz/windows/...`.

Available today (all consumed over device RPC or in-process VCs):

- **`PacketStats`** — cumulative remote/local/block egress+ingress packet and byte
  counts, plus `TransportStats` per-transport breakdown
  (urnetwork_sdk.hpp:2391-2405, 3157-3162). This is the raw counter the 1 Hz sampler
  deltas. Provider variant: `getProviderPacketStats()`; extender: `getExtenderStats()`
  (used at sdk/contract_view_controller.go:311-313).
- **Throughput time series** — `ThroughputPoint/Sample` (hpp:3107-3121) via
  `ContractViewController::getThroughputPoints / getProviderThroughputPoints /
  getExtenderThroughputPoints` (hpp:16194-16199); 1 s × 60 s, per route.
- **`TransportDistribution`** — windowed remote bytes by transport with shares/
  boundaries/percents computed SDK-side (hpp:3146-3150, 3169-3182).
- **`BlockAction`** — one routing decision: hosts, ips, matched hosts/ips,
  block/local verdicts, override ids, **`PacketCount` and `ByteCount`**
  (hpp:1206-1220); window via `BlockActionViewController::getBlockActions`
  (hpp:16113). Aggregate `BlockStats{AllowedCount,BlockedCount}` (hpp:1234-1237).
  This is the per-connection feed — keyed by host/IP cluster, **not by app**.
- **`Exit`** — per-provider window row: ClientId, Warning/Quarantined/Proven,
  **FlowCount**, DialFailureCount, Tier/EffectiveTier, ProbeAgeSeconds, and
  provider-reported block packet/byte counters (hpp:1598-1620); `getExits`,
  `getDestinationExits` (hpp:16236-16237). **`ReliabilityMetrics`** — 30+ aggregate
  flow/recovery/dial counters (hpp:2616-2650).
- **`ContractStats`** and the ContractDetails VC's per-peer rows — per-contract
  used/total bytes + bit rate (hpp:1411-1418; App-side mapping SdkHost.h:195-248).
- **`DeviceStats`** — cumulative session counters: connect count, durations,
  **net remote receive/send byte counts** (hpp:16468-16481).
- **`WindowStatus`** — window formation: target size, provider state buckets,
  dualstack counts, StallReason, Failed (hpp:3353-3368).
- **`TransferStatsResult`** — paid/unpaid bytes provided (wallet/earnings,
  hpp:3141-3144, `Api::getTransferStats` hpp:16029).

Not present (searched the hpp and the sibling `connect/` Go tree): **no per-app
stats, no per-flow stats, no active-flow table**. `FlowOwnerLookup` is an *input*
seam (hpp:15640) — the SDK asks the platform who owns a flow; it never reports
that attribution back out as statistics.

---

## (d) The split: consumable by a UI-only wave vs needs new service/SDK code

**UI-only (no protocol or service changes)** — everything below is already pushed
into `ConnectPage`/sheets today:

- Live throughput strip/chart upgrades: richer rendering of the existing
  1 Hz `ThroughputPoint` series (three routes), per-transport shares, peak/avg
  labels. Pure UI work on `TransferChart`/`TransportBar` consumers.
- A Portmaster-style **live connections table keyed by host/IP**: the BlockAction
  feed already delivers per-decision verdicts + packet/byte counts several times a
  second; the current list (ConnectPage.cpp:2074+) is one presentation of it. A
  denser advanced-mode table (sparklines per host, rate columns derived by diffing
  successive pushes) is UI-only. Note the honesty limit: these are *routing
  decisions* (newest-first event log), not an open-connection table — the SDK
  aggregates by host cluster and does not expose sockets.
- **Per-provider (exit) live view**: `Exit` rows (FlowCount, tier, proven,
  warnings) + `ReliabilityMetrics`, already refreshed at 5 s in advanced mode
  (ConnectPage.cpp:3583-3586). A per-exit traffic panel is presentable from this
  plus the provider throughput series — no new service code.
- **Per-contract circles/rows**: per-peer contract bit rates already flow to the
  contracts sheet.
- Synthetic-data development: the preview-UI sample generator
  (`PreviewSampleCharts`, ConnectPage.cpp:3660, seeded rates at 3798-3811) already
  fabricates points/decisions/exits in-process — a new view can be built and
  exercised with zero session.

**Needs NEW code** (roughly, smallest first):

1. **Windowed byte counters in the service** (if the view should keep drawing when
   the app's own RPC subscription is the bottleneck, or to show service-authoritative
   bytes): PacketPump already touches every packet's byte length
   (PacketPump.cpp:60-67 computes `packetByteCount` per packet) — adding
   `outboundBytes/inboundBytes` atomics beside the packet counters is a few lines.
   Surfacing them means a **new pipe verb** (e.g. `get_stats` reply, or extending
   `TunnelStatus`) plus, if push is wanted, a new event type next to `tunnel_state`
   (ControlServer.cpp:112-132 is the one existing push site to mirror). Protocol
   versioning precedent is documented in Protocol.h:27-61 (additive fields have not
   needed bumps when absence degrades safely).
2. **Per-app live attribution**: all the pieces exist except the counting. FlowOwner
   already maps 5-tuple→exe at 1 Hz in the service; the SDK does not report
   per-app stats. Options, cheapest first: (i) count bytes per flow in PacketPump's
   two hot paths, join flow→exe via FlowOwner's cache, aggregate per exe, push a
   windowed `app_stats` event over the pipe — entirely service-side, no SDK change,
   but PacketPump sees IP packets so flow keying means parsing headers on the hot
   path; (ii) extend the SDK to expose its internal flow→app pin cache as stats —
   requires an SDK rebuild (cgo, sibling repos); (iii) driver-level per-app counters
   via a new query IOCTL — blocked on the driver shipping (R10 hardening) and is new
   kernel code.
3. **A true open-flow table** (Portmaster's per-connection rows with state,
   duration, both endpoints): nothing at any layer exposes this today. BlockActions
   are the nearest proxy (decision log), `ReliabilityMetrics` only counts flows in
   aggregate. This is SDK territory (it owns the flow table) — new SDK export +
   rebuild.

**Watch out**: `ReadStats`' device-RPC getters are synchronous and have hung the UI
against a dying service (documented at SdkHost.cpp:2785-2799, an observed AppHangB1).
Any new sampling loop should hang off view-controller reads (in-process) or run
off-UI-thread like `RefreshExitRouting` (ConnectPage.cpp:3262), not add synchronous
RPC getters to the UI-thread tick.

---

## (e) Existing ticks/timers to hang new sampling off

- **`ConnectPage::chartTimer_`** — `DispatcherQueue` timer, **100 ms interval**
  (ConnectPage.cpp:191-195), started/stopped with window presentation
  (`SetPresentationActive`, 198-209) and skipped while hidden (3525). Inside
  `OnChartTick` (3523-3590): charts tick every 100 ms; **`chartTickCount_ % 10` gives
  a ~1 s cadence** (3572) used for relative-time refresh; **advanced mode already
  runs a 5 s RPC refresh** off `exitRefreshTick_` (3583-3586) — the exact pattern
  (gate on `advancedMode_` + visible + coalesce in-flight) a new sampler should
  copy. Health-reeval nudges ride the same tick (3533-3542).
- **`DeveloperPage::pollTimer_`** — 5 s, gated on advanced mode + page selected +
  presenting, with coalescing (`kPollInterval{5000}`, DeveloperPage.cpp:37,
  563-600). Another ready-made advanced-mode polling template.
- **SDK throughput listener** — the 1 Hz push itself (`addThroughputListener`,
  SdkHost.cpp:3072); free per-second wake-ups whenever traffic is active, delivered
  off the UI thread.
- Service-side precedents if a service sampler is added: FlowOwner's 1 Hz worker
  (FlowOwner.h:225, `WorkerLoop`) and the watchdog's 1 Hz counter read — both show
  the "cheap lock-free read on a timer" shape this codebase prefers.

---

## Evidence index (file:line)

| Claim | Where |
|---|---|
| Pipe verbs, no stats verb | app/src/Common/Protocol.h:65-75; app/src/Service/ControlServer.cpp:35-101 |
| Only pipe counter | Protocol.h:228-229; Service/TunnelController.cpp:1788-1789 |
| Only pushed pipe event | Service/ControlServer.cpp:112-132 |
| LiveStats shape + push model | app/src/App/SdkHost.h:88-166 |
| 1 Hz / 60 s sampling | ../sdk/contract_view_controller.go:29-30, 264-305 |
| Packet stats RPC | ../sdk/device_rpc.go:5155-5164, 10567; hpp:2391-2405 |
| Charts + feeds | app/src/App/ConnectPage.cpp:1296-1334, 1411-1421; SdkHost.cpp:3260-3331 |
| Rate lines | ConnectPage.cpp:1081-1087; MainWindow.xaml.cpp:1010-1011 |
| Connections list (decision feed) | ConnectPage.cpp:1966-2104; hpp BlockAction:1206-1220 |
| Advanced-mode 5 s exit refresh | ConnectPage.cpp:3250-3299, 3583-3586; SdkHost.cpp:4348-4363 |
| Service packet counters | Service/PacketPump.h:44-47; TunnelController.cpp:1064 |
| FlowOwner attribution engine | Service/FlowOwner.h (whole), :225; TunnelController.cpp:783-806; ../connect/ip_remote_multi_client.go:1906-1933 |
| Driver: exists, control-only, gated | app/driver/README.md:66-72; app/driver/Ioctl.h:31-56; app/installer/Package.wxs:141-145, 243-248; Service/SplitTunnelClient.h:1-6 |
| SDK stats structs | urnetwork_sdk.hpp:1206-1237, 1411-1418, 1598-1620, 2391-2405, 2616-2650, 3107-3121, 3146-3162, 3353-3368, 16468-16481 |
| Ticks | ConnectPage.cpp:191-195, 3523-3590; DeveloperPage.cpp:37, 563-600; SdkHost.cpp:3072 |
