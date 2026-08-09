# The Windows tunnel data path: routing, self-exclusion, and what needs a driver

**Status:** research / decision document. External research anchored to
`beta/custom-server` as of 2026-08-08. Nothing here was executed on this
machine — no adapter was created, no route or DNS entry written, `urnetworkd`
was never started. Every claim is either code we can read, a cited upstream
implementation, or explicitly marked unverified.

**Question:** what is the best way to get all of a Windows machine's traffic
into our tunnel and back out, without a routing loop, without breaking on
network changes, and without a signed kernel driver on day one?

**Short answer:** the data path we have already built is the right one. Its
route strategy is better suited to our product than what WireGuard, Tailscale
or Mullvad do on Windows, and its self-exclusion is in the same school as
Tailscale's — the only school that works for an unbounded peer set. The `/1 +
/1` "split default" the brief asks about turns out to be a *Linux* `wg-quick`
technique that none of the three Windows implementations uses; the real choice
was between a single `/0` and our complement set, and the complement set wins
for a reason specific to us (§2).

It has six real gaps, in descending order of how much they hurt:

1. **IPv6 is completely unhandled** — not tunnelled, not blocked, not
   blackholed. On any dual-stack network the majority of a user's browsing
   silently bypasses the tunnel. This is the worst bug in the data path and it
   is not a routing subtlety, it is a missing feature.
2. **The service's own hostname resolution is not self-excluded.** R1 covers
   sockets `connect` opens. It cannot cover `GetAddrInfoW`, which is what
   `net.DefaultResolver` uses on Windows, and which runs inside the `dnscache`
   service — a different process we do not bind.
3. **MTU is a hardcoded 1440** regardless of the underlying link. On a
   1400-byte path (PPPoE, many LTE hotspots, some corporate VPN-over-VPN) that
   is a large-transfer stall with a "works until you upload a photo" signature.
4. **No WFP layer at all**, so there is no leak guard, no fail-closed state,
   and no way to make a partial failure safe.
5. **We watch only one of the three network-change notifications.** A DHCP
   lease renewal changes the physical source address without changing the
   interface or the route table's shape, so the split-tunnel driver keeps
   rebinding excluded apps to an address the DHCP server has taken back.
6. **Tun routes use an on-link next hop**, which on Windows costs the last
   address of every prefix — 31 specific public IPs that break only while
   connected.

None of the six needs a kernel driver.

---

## 1. What we have already built

Read first, because every recommendation below is a delta against this.

### `TunnelController::StartLocked` — `app/src/Service/TunnelController.cpp`

The 8-step sequence, with a hard fence after step 5:

| Step | What | Where |
| --- | --- | --- |
| 1/8 | wintun adapter, pinned GUID + alias, 4 MiB ring | `TunnelController.cpp:143-185` |
| 2/8 | **R1** — bind SDK egress to the physical NIC | `TunnelController.cpp:187-222` |
| 3/8 | NetworkSpace | `:224` |
| 4/8 | DeviceLocal | `:234` |
| 5/8 | mTLS RPC listener | `:253` |
| — | **THE FENCE** — rpc-only returns here | `:261-280` |
| 6/8 | `netConfig_->Apply` — address, MTU, routes, DNS | `TunnelController.cpp:325-351` |
| 7/8 | split tunnel (driver optional) | `:353` |
| 8/8 | packet pump | `:363` |

Steps 6–8 live in `BringUpTunnelLocked` and **have never been run**. Step 2
runs before any SDK object exists so no socket is ever created unbound, and
before step 6 so `DiscoverEgress` still sees a clean route table. That ordering
is load-bearing and correct; do not weaken it.

### `NetworkConfig::Apply` — `app/src/Service/NetworkConfig.cpp:122-191`

- Tun address `169.254.2.1/24` (from `device_->tunnelLocalAddress()`, with that
  as fallback), `DadState = IpDadStatePreferred`.
- `NlMtu = 1440`, `UseAutomaticMetric = FALSE`, `Metric = 1`, **`AF_INET`
  only**.
- **31 complement prefixes** (`kIncludedV4Routes`, `:79-88`) covering
  `0.0.0.0/0` minus RFC1918, each `CreateIpForwardEntry2` on the tun LUID with
  on-link next hop `0.0.0.0` and route metric 0.
- DNS via `SetInterfaceDnsSettings` on the tun's GUID.
- Nothing on the physical adapter is touched; the physical default route is
  never deleted.

### R1 self-exclusion — `EgressMonitor.cpp` + `connect/egress_windows.go`

`DiscoverEgress` picks the lowest combined route+interface metric default route
that is not the tun LUID, preferring a connected interface;
`NotifyIpInterfaceChange` keeps it current; the index goes to
`urnet::setEgressInterfaceIndex` → `connect.SetEgressInterfaceIndex`, and
`connect/egress_windows.go:43 applyEgressInterface` sets `IP_UNICAST_IF`
(network byte order) / `IPV6_UNICAST_IF` (host byte order) on every socket via
`net.Dialer.Control`.

Two deliberate behaviours worth keeping: it **never pushes 0** while a valid
retained index exists (unbinding is worse than pinning to a downed NIC), and it
re-validates the retained index still names a live interface before retaining
it.

### Crash safety — `NetworkConfig.h:44-101`

The primary restore path is the OS's: the wintun adapter dies with the process
and the stack drops every route and address bound to it. `CrashRevert` (routes
only, no DNS RPC) and `SweepOrphanedTunnel` are defence in depth. This design
is sound and I would not change it — see §4, where the same "dies with the
process" property is the reason to pick a *dynamic* WFP session over anything
persistent, and the reason to reject Mullvad's fail-closed-past-reboot model.

---

## 2. Route installation strategy

### The three options

| Approach | Mechanism | Needs the physical gateway? |
| --- | --- | --- |
| Replace the default route | delete phys `0.0.0.0/0`, add tun `0.0.0.0/0` | to restore it, yes |
| Real `/0` on the tun, lower interface metric | tun `0.0.0.0/0` metric 0 beats phys `0.0.0.0/0` | no |
| Split-default / complement prefixes | longer prefixes sort above `/0` by LPM | no |

Windows route selection is **longest-prefix-match first, metric only as the
tiebreaker between equal prefix lengths**
([MS: route determination process](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-vista/cc766470(v=ws.10))).
That single fact is what makes options 2 and 3 both work and makes option 1
never necessary. **Never delete the physical default route.** It is the only
irreversible thing in the whole data path, and a process that dies between the
delete and the restore strands the machine with no automatic recovery. Our code
already refuses to do this and the comment block in `NetworkConfig.h:44-56` is
the reason to keep refusing.

### Nobody on Windows uses the `/1 + /1` trick

This is the most commonly mis-cited thing in this area, so, having read all
three: **WireGuard for Windows, Tailscale and Mullvad all install a real
`0.0.0.0/0` (and `::/0`) on the tun.** None of them splits into `/1` halves.
That trick is `wg-quick` on *Linux*, where it is paired with an `fwmark` and an
`ip rule ... suppress_prefixlength 0` — neither of which exists on Windows.
They all rely instead on route metric 0 plus a forced low **interface** metric:

| | tun route metric | tun interface metric |
| --- | --- | --- |
| WireGuard for Windows | 0 | `UseAutomaticMetric = false; Metric = 0` (only if a default was found for that family) |
| Tailscale | 0 | `UseAutomaticMetric = false; Metric = 0` |
| Mullvad | 0 | `UseAutomaticMetric = false; Metric = 1` |
| **us** | 0 | `UseAutomaticMetric = FALSE; Metric = 1` |

So the real alternative on the table is not `/1 + /1`; it is **a single
`0.0.0.0/0` at interface metric 0**. See the verdict below for why we should
still not take it.

On WireGuard specifically, `tunnel/addressconfig.go`
`configureInterface()` installs each AllowedIP verbatim via
`luid.SetRoutesForFamily()` → `winipcfg/luid.go AddRoute()`, so
`AllowedIPs = 0.0.0.0/0` becomes a genuine `0.0.0.0/0` route on the tun, route
metric 0, and the interface gets `UseAutomaticMetric = false; Metric = 0` (only
when a default route was found for that family). No split, no `/32` host route
to the peer anywhere in the codebase.

They can get away with a real `/0` because **their encrypted UDP never consults
a route table containing the tun**. In current WireGuardNT,
`wireguard-nt/driver/socket.c SocketResolvePeerEndpoint()` walks
`GetIpForwardTable2` itself with one load-bearing line:

```c
if (Table->Table[i].InterfaceLuid.Value == Peer->Device->InterfaceLuid.Value)
    continue;
```

then derives the source with `GetBestRoute2`. In the legacy userspace path
(`tunnel/defaultroutemonitor.go` at `v0.3.16`), `bindSocketRoute()` did the
same walk in Go and then called `IP_UNICAST_IF` — which is exactly our design.

**We cannot copy the NT approach.** It resolves *one* peer endpoint per send. A
URnetwork client talks to the platform, to DoH resolvers, to ICE/STUN, and to
an unbounded, runtime-discovered set of provider IPs. There is no single
endpoint to route around. That kills the `/32`-host-route family of solutions
for us outright, and it is the central reason our design is what it is.

### The table is arithmetically correct — verified

Before recommending anything about `kIncludedV4Routes`, I checked it. All 31
entries are **prefix-aligned**, **non-overlapping**, and their union is exactly
`0.0.0.0/0` minus exactly three ranges:

```
10.0.0.0     - 10.255.255.255    (10/8)
172.16.0.0   - 172.31.255.255    (172.16/12)
192.168.0.0  - 192.168.255.255   (192.168/16)
```

No gaps, no overlaps, no misalignment. The table is right; the §"refinements"
below are about what is *in* the exclusion list, not about the arithmetic.

### Verdict: keep the 31 complement prefixes

I expected to recommend collapsing `kIncludedV4Routes` to something simpler.
Having read it and the three references, **don't** — for a reason that is easy
to miss and that the references do not face because they made a different
product decision.

Our requirement is **all of RFC1918 bypasses the tunnel**, matching Android
(`MainService excludeRoute`) and iOS (`NEIPv4Settings.excludedRoutes`). A single
`0.0.0.0/0` on the tun does **not** give you that. It gives you only the
*directly connected* subnet, because the physical NIC's on-link `/24` is a
longer prefix. A machine on `192.168.1.0/24` reaching a `10.0.0.0/8` host on
the other side of its router would go into the tunnel.

To get full RFC1918 bypass with a `/0` you would need *exclusion* routes for
`10/8`, `172.16/12` and `192.168/16` pointing at the **physical gateway** — which
is exactly what Mullvad has to maintain for its relay `/32`s, and exactly why
`RouteManagerInternal::default_route_change()` has to delete and re-add them on
every default-route flap. That is real, ongoing roaming fragility.

**The complement set needs nothing but the tun LUID.** No gateway, no
re-writing on roam, no monitor. It is a worse-looking table that buys a
strictly simpler lifecycle, and on Windows the lifecycle is where VPN clients
actually break.

Cost: 31 `CreateIpForwardEntry2` calls instead of one, so 31 route-change
notifications at bring-up. Today that is inert for us because `EgressMonitor`
only watches *interface* changes — but change #5 below adds
`NotifyRouteChange2`, at which point our own step 6 becomes a 31-event burst
into our own handler. **That is the concrete reason change #8 (debounce) must
land in the same commit as change #5**, not as a later polish item. Accept the
31 routes; do not accept them without the debounce.

### The on-link next hop is a real hazard — change it

`AddTunRoute` (`NetworkConfig.cpp:29-47`) sets `NextHop` to `0.0.0.0`, i.e.
**on-link**, with the comment "`0.0.0.0 next hop => on-link`". That is the
obvious thing to do and it is what I would have written. Both Tailscale and
Mullvad deliberately do **not** do it:

- Tailscale (`wgengine/router/osrouter/ifconfig_windows.go`) uses a
  pseudo-gateway — `tsaddr.TailscaleServiceIP()` (100.100.100.100) for v4 —
  as the next hop for range routes, and reserves an on-link next hop for
  single-host routes only. The documented reason is that **on-link subnet
  routes on Windows break the last IP in the range**, which then fails with
  `WSAEADDRNOTAVAIL` (10049) because the stack treats it as the subnet
  broadcast address.
- Mullvad (`talpid-routing/src/windows/route_manager.rs`) always sets a real
  `NextHop` from the resolved node's gateway.

For a `/24` that costs you one address. For our prefixes it costs the **last
address of each of 31 ranges**, several of which are ordinary routable space —
`159.255.255.255` (from `128.0.0.0/3`), `207.255.255.255` (from
`200.0.0.0/5`), and so on. Rare, but it is precisely the class of bug that
presents as "one specific site is broken on the VPN and nobody can reproduce
it".

**Recommendation:** give the tun routes a pseudo next hop inside the tun's own
`/24` — e.g. `169.254.2.2` when the tun holds `169.254.2.1/24`. It is
reachable via the tun's on-link subnet route, so the routing works out the same,
and it takes the broadcast-address special case off the table. *I have not
reproduced the 10049 behaviour on this machine (nothing was run) — treat the
mechanism as reported-by-two-independent-implementations rather than
first-hand.*

### Four refinements to the prefix set

The set is a hardcoded table computed for RFC1918 only. It relies on
longest-prefix-match against *system* routes for several important ranges,
which works but is implicit. Make the dependencies explicit:

1. **Derive the set at runtime** from a declared exclusion list rather than
   hardcoding 31 constants. The complement computation is ~20 lines and it
   makes the next four items one-line changes instead of a hand-recomputed
   table.
2. **`127.0.0.0/8` is currently captured by `64.0.0.0/2`.** It works today only
   because the Loopback Pseudo-Interface's on-link `127.0.0.0/8` is a longer
   prefix. Our own RPC listener and the tray app's dial ride on that. Put
   loopback in the exclusion list explicitly.
3. **`169.254.0.0/16` is captured by `168.0.0.0/6`.** Our own tun address is
   `169.254.2.1`, so the tun's on-link `/24` wins for us, but the general
   link-local range should not be in the tunnel. Exclude it.
4. **`224.0.0.0/3` captures all multicast and broadcast.** mDNS/SSDP/LLMNR are
   normally sent per-interface via `IP_MULTICAST_IF` rather than the unicast
   route table, so this is probably inert — *unverified*. Excluding `224/4` and
   `255.255.255.255/32` costs nothing and removes the question.

Consider also excluding `100.64.0.0/10`: it is currently captured by
`64.0.0.0/2`, and a machine also running Tailscale is only saved by
Tailscale's own `/10` being longer. That works, but it is luck rather than
design.

### Interface metric

We set `UseAutomaticMetric = FALSE; Metric = 1`. WireGuard uses `Metric = 0`.
With LPM deciding, neither matters for route selection. It *does* matter for
one thing: **Windows orders DNS servers by interface metric**, so metric 1 vs 0
affects which resolver `dnscache` prefers. Since we *want* the tun's resolver
preferred, keeping the tun's metric at or below the physical adapter's is
correct. Leave it at 1 or move it to 0; do not leave it automatic.

Two things WireGuard sets that we do not, both worth copying from
`tunnel/addressconfig.go`:

- `DadTransmits = 0` — duplicate address detection is meaningless on a
  point-to-point tun and costs startup latency.
- `RouterDiscoveryBehavior = RouterDiscoveryDisabled`.

### Network change, sleep, resume, adapter reorder

Our routes are keyed on the **tun LUID**, not on an interface index and not on
a gateway address. That is the property that makes them survive everything in
this list: a LUID is stable for the life of the adapter, and the adapter's life
is our process's life. Physical adapter reorder, DHCP renew, Wi-Fi→Ethernet,
and resume all change the *physical* side, which we never wrote to.

What actually has to follow a network change is only the **egress binding**,
and `EgressMonitor` already does it via `NotifyIpInterfaceChange`.

Three gaps, and the first is the one I would fix first:

- **We watch only one of the three notifications that matter.**
  `EgressMonitor::Start` registers `NotifyIpInterfaceChange` alone. Mullvad's
  `DefaultRouteMonitor::register_callbacks()` registers **all three**:
  `NotifyRouteChange2`, `NotifyIpInterfaceChange`, **and
  `NotifyUnicastIpAddressChange`**. That third one is the gap with teeth: a
  DHCP lease renewal that hands the machine a *new address on the same
  interface* changes nothing about the interface's state or the route table's
  shape. Our egress *index* is still correct — so R1 survives — but
  `PushPhysicalAddressesLocked` has pushed a now-stale **source address** to
  the split-tunnel driver, and every excluded app stays bound to an address the
  DHCP server has reclaimed. `TunnelController.cpp:470-472` already names this
  exact failure in a comment; the notification that would catch it is not
  registered. Add `NotifyUnicastIpAddressChange`, and `NotifyRouteChange2` too
  while you are there.
- **No debounce.** WireGuard coalesces bursts with a 150 ms timer
  (`monitorDefaultRoutes`); Mullvad uses a `BurstGuard` with
  `BURST_BUFFER_PERIOD = 200 ms` and `BURST_LONGEST_BUFFER_PERIOD = 2 s`. A
  Wi-Fi→Ethernet switch produces a burst; each of ours does a full
  `GetIpForwardTable2` walk plus a `setEgressInterfaceIndex` store on a system
  worker thread, and adding two more notification sources makes the burst
  bigger. Debounce ~200 ms with a 2 s ceiling — take Mullvad's numbers, they
  are watching the same three sources we would be.
- **No power-event handling.** We do not request
  `SERVICE_ACCEPT_POWEREVENT` or handle `PBT_APMRESUMEAUTOMATIC`. Note that
  WireGuard for Windows appears not to either — *I could not find any power
  handling in their tree, and their resume path seems to be implicit via
  interface-change notifications* (unverified as a deliberate choice rather
  than an omission). Since our SDK holds long-lived QUIC/WebSocket sessions
  that a suspend will have silently killed, an explicit resume signal is worth
  more to us than to them.

### Force-kill

Best-case of the three strategies, and we already have it: the routes are on an
interface that ceases to exist when the process dies. Nothing to restore, no
gateway to remember, no persistent state. This is the strongest argument for
the current design and the reason not to move any of it into persistent
configuration.

---

## 3. Self-exclusion — the single highest risk

### The techniques, and which ones actually work on Windows

Windows has **no `SO_BINDTODEVICE`**. There is no single substitute; there are
four partial ones, and only one of them is a general answer.

| Technique | What it really does | Verdict for us |
| --- | --- | --- |
| **`IP_UNICAST_IF` / `IPV6_UNICAST_IF`** | forces the outgoing interface for unicast; affects outbound only, not receive | **the primary mechanism.** The only one that scales to an unbounded, runtime-discovered peer set |
| `/32` host route to the peer via the physical gateway | a route-table entry that beats the tun by prefix length | **rejected for us** — works for Mullvad (one relay), impossible for an unbounded runtime-discovered peer set; needs the gateway and must be rewritten on every roam |
| Source-address bind (`bind()` to the NIC's address) | pins the source, and Windows' strong host model then constrains the send | **breaks silently on roaming** — the address changes on DHCP renew and the socket keeps a now-invalid source |
| WFP permit/block filters | **cannot move a packet from the tun to the physical NIC.** Permit/block only | a leak guard, *not* self-exclusion |
| WFP `ALE_BIND_REDIRECT` / `ALE_CONNECT_REDIRECT` callout | genuinely reroutes a bind | needs a signed kernel driver (this is our `SplitTunnel.sys`) |
| Marking / fwmark | no Windows equivalent | n/a |

The WFP row is the one people get wrong. `FwpmFilterAdd0` from user mode
(`fwpuclnt.lib`) can only `FWP_ACTION_PERMIT` or `FWP_ACTION_BLOCK`. Redirection
requires a kernel-mode callout. **So WFP is how you stop a leak, never how you
avoid a loop.**

This is worth labouring because all three references have an app-ID WFP permit
for their own binary, and it is routinely mis-cited as their self-exclusion:

- WireGuard: `tunnel/firewall/rules.go permitWireGuardService` —
  `FWPM_CONDITION_ALE_APP_ID` (from `FwpmGetAppIdFromFileName0`) **AND**
  `FWPM_CONDITION_ALE_USER_ID`, so a same-named binary under another user does
  not inherit the permit.
- Tailscale: `wf/firewall.go permitTailscaleService` —
  `FieldALEAppID == wf.AppID(os.Executable())`.
- Mullvad: `rules::multi::PermitEndpoint` — `ConditionApplication` wrapping
  `FwpmGetAppIdFromFileName0`, narrowed to the relay IP/port/protocol.

**None of those is self-exclusion.** Every one of them exempts the process from
its own **killswitch** (`blockAll` at weight 0). The actual loop avoidance is
elsewhere in each case: WireGuard's LUID skip in `SocketResolvePeerEndpoint`,
Tailscale's `IP_UNICAST_IF`, Mullvad's relay `/32`.

### The two live schools, and which one we are in

The references split cleanly, and it is worth knowing we are not alone:

| | **Tailscale** — bind the socket | **Mullvad** — route around |
| --- | --- | --- |
| Mechanism | `IP_UNICAST_IF`/`IPV6_UNICAST_IF` (`sockoptBoundInterface = 31`) in `net/netns/netns_windows.go` `bindSocket4`/`bindSocket6` | `/32` (`/128`) host route per relay endpoint via `NetNode::DefaultNode`, resolved by `get_best_default_route()` |
| Loop guard | `isTailscaleInterface()` rejects its own WinTun by adapter description | `is_route_on_physical_interface()` rejects loopback, `IF_TYPE_TUNNEL`, and descriptions containing `WireGuard`/`Wintun`/`Tunnel` |
| On roam | **re-create the sockets** — `magicsock.Conn.Rebind()` when netmon reports `RebindLikelyRequired` | **re-write the routes** — delete and re-add every `DefaultNode` route against the new LUID/gateway |
| Cost | every socket must be re-made on link change | a monitor that must never miss a default-route flap |
| Peer set | unbounded (DERP + every peer) | small and known (the relay) |

**We are firmly in Tailscale's school, and correctly so** — our peer set is
unbounded and discovered at runtime, which is the same constraint that pushed
Tailscale there. Mullvad's approach is only viable because a Mullvad client
talks to exactly one relay.

Two details worth stealing from Tailscale. First, their loop guard is
*defensive*: `interfaceIndexFor` → `GetBestInterfaceEx` explicitly **rejects the
answer if it is the Tailscale adapter** and falls back. Ours excludes the tun by
LUID in `DiscoverEgress`, which is stronger than matching on a description
string — Mullvad's own source carries a `TODO(Jon)` saying matching the known
tunnel LUID would be more correct than their description match. **We already do
the thing both of them wish they did.** Keep it.

Second, and this is a difference not a gap: Tailscale does not have to worry
about a stale binding because it *re-creates* sockets on link change. We keep
long-lived sockets and re-push an index instead. That is a deliberate trade —
ours keeps sessions alive across a roam — but it means our retained-index
revalidation (`EgressMonitor.cpp:100-119`) is doing work Tailscale gets for
free, and it is the reason that code deserves to stay as carefully written as it
is.

Our design — `IP_UNICAST_IF` set in `net.Dialer.Control` before connect, from a
monitored physical interface index — is the correct primary mechanism and
matches both the legacy WireGuard-for-Windows path
(`tunnel/defaultroutemonitor.go bindSocketRoute` at `v0.3.16`) and Tailscale.

The byte-order asymmetry is real and we get it right:
`connect/egress_windows.go:43` swaps for v4 and not for v6, matching
`wireguard-go/conn/boundif_windows.go bindSocketToInterface4`. The MS docs
confirm: `IP_UNICAST_IF` takes the index **in network byte order** (it is typed
as an IPv4 address in `0.x.x.x`); `IPV6_UNICAST_IF` takes host order.

### The hole: the OS resolver

**This is the part of R1 that `IP_UNICAST_IF` architecturally cannot close, and
I believe it is currently unmitigated.**

`ConnectSettings.NetDialer()` (`connect/net.go:129-138`) builds an
`egressDialer` and passes `Resolver: self.Resolver`. Grepping `connect` for
assignments to `ConnectSettings.Resolver` finds **none** outside tests — it is
always nil. A nil `net.Dialer.Resolver` means `net.DefaultResolver`, and on
Windows Go's default resolver is the **system** resolver, not the pure-Go one.
This is not a heuristic; it is unconditional in `net/conf.go lookupOrder`:

```go
// On systems that don't use /etc/resolv.conf or /etc/nsswitch.conf, we are done.
switch c.goos {
case "windows", "plan9", "android", "ios":
    return fallbackOrder, nil
}
```

and with neither `PreferGo` nor `netdns=go` set, `fallbackOrder` is
`hostLookupCgo` — `GetAddrInfoW`. I grepped both `connect` and this repo for
`netdns` / `GODEBUG` / `netgo`: **nothing sets them.** `GetAddrInfoW` is an RPC
into the `dnscache` service inside `svchost.exe`. That is a different process.
Its sockets are not ours to bind, and no socket option we set has any effect on
them.

So every hostname dial the SDK makes — the platform connection, DoH bootstrap,
anything not already an IP literal — resolves through a path that follows the
route table, which after step 6 points at our own tun.

Why this probably has not shown up as a hard deadlock: Windows **Smart
Multi-Homed Name Resolution** issues queries across interfaces and takes the
first answer, so the physical adapter's resolver (which still works — we never
touch the physical adapter) will usually answer while the tun query hangs. The
exact behaviour has changed across Windows builds and I could not pin down the
current semantics — **treat "SMHNR saves us" as unverified and do not design
around it.** It is a race, and it also *is* the R6 DNS leak from the other
direction.

**Fix, concretely:** give the SDK a resolver it owns.

- Service side: `EgressMonitor` already knows the physical interface. Have it
  also read that interface's DNS servers (`GetAdaptersAddresses` →
  `FirstDnsServerAddress`) and push them down alongside the index — a new
  `urnet::setEgressDnsServers(...)` mirroring `setEgressInterfaceIndex`.
- `connect` side: set `ConnectSettings.Resolver` to
  `&net.Resolver{PreferGo: true, Dial: <dial the pushed physical resolvers through egressDialer>}`.
  `net_http_doh.go:502-534` already builds exactly this shape twice
  (`remoteResolver`/`localResolver`), so the pattern is established in-tree.

That makes the SDK's own name resolution use *our* sockets, which are pinned,
and closes the last uncovered path. It also removes the dependency on SMHNR
behaviour we cannot verify.

### The second hole: everything that is not a `net.Dialer`

`applyEgress` covers the platform QUIC `UDPConn` (`transport.go:1434`) and pion
ICE (`egress_net.go`). Anything that opens a socket by another route is
unprotected. Worth an audit each time a transport is added — and worth the
readback in the next section, which turns "we think it's pinned" into "the log
says it's pinned".

### Failure semantics we already got right

`applyEgressInterface` returns an error when **every** attempted option fails
(`egress_windows.go:73-82`), so a stale index fails the dial loudly instead of
silently following the route table. Per-option failure is still ignored, which
is correct — setting the wrong family's option on a single-family socket fails
harmlessly. Keep both behaviours.

### Verifying it worked — the exact operator procedure

The literal phrasing "prove no socket is sourced from the tun address" needs
one correction before it is checkable: *the tun address is the source for all
the host's tunnelled traffic* — that is the point. The claim to prove is
narrower: **no socket owned by `urnetworkd` is sourced from the tun address.**

With the tunnel up:

```powershell
$svc = (Get-Process urnetworkd).Id
$tun = (Get-NetIPAddress -InterfaceAlias URnetwork -AddressFamily IPv4).IPAddress

# 1. TCP — definitive. Expect zero rows.
Get-NetTCPConnection -OwningProcess $svc |
  Where-Object { $_.LocalAddress -eq $tun }

# 2. TCP — positive control. Every established remote should be sourced
#    from the PHYSICAL adapter's address (loopback rows are the tray RPC).
Get-NetTCPConnection -OwningProcess $svc -State Established |
  Format-Table LocalAddress, LocalPort, RemoteAddress, RemotePort

# 3. Route sanity — the physical default must still be there.
Get-NetRoute -AddressFamily IPv4 -DestinationPrefix 0.0.0.0/0 |
  Format-Table InterfaceAlias, NextHop, RouteMetric, ifIndex
```

Check 1 is the assertion; check 2 is the evidence it is meaningful (an empty
check 1 with an empty check 2 proves nothing).

**UDP/QUIC needs a different proof.** The platform UDP socket is
wildcard-bound, so `Get-NetUDPEndpoint` reports `0.0.0.0` no matter what —
`IP_UNICAST_IF` changes interface selection, not the bound address. Use
`pktmon` (in-box since Windows 10 1809) to assert that *nothing addressed to the
platform ever appears on the tun*:

```powershell
pktmon list                       # note the URnetwork adapter's component id
pktmon filter remove
pktmon filter add Loop -i <platform-endpoint-ip>
pktmon start --capture --comp <tun-component-id> --pkt-size 64 `
             --file-name C:\ProgramData\URnetwork\loopcheck.etl
Start-Sleep 60
pktmon stop
pktmon etl2txt C:\ProgramData\URnetwork\loopcheck.etl
```

Zero packets in the output is the proof. A cheap standing approximation, good
enough for a support script: with the tunnel up and **no** user traffic (close
every browser), sample `Get-NetAdapterStatistics -Name URnetwork` every 5 s for
a minute. Flat `SentBytes` while the platform connection stays `Connected`
means the service's own keepalives are not in the tun. Steadily climbing
`SentBytes` with no user traffic is the loop.

**Make the service prove it itself.** The strongest version of this check does
not need an operator at all: after the platform connection establishes, call
`getsockname()` on it and log the local address, with an explicit assertion
against the tun address. One log line — `egress: platform socket sourced from
192.168.1.42 (physical, OK)` versus a `LogError` naming the tun — turns the
whole of check 1 into something the log answers on its own. Given how much of
`TunnelController` is already written to make failures self-diagnosing, this
fits the house style and I would rank it above the operator procedure.

---

## 4. IPv6 — recommendation, not a survey

**Today: the tunnel is IPv4-only, and IPv6 is not handled at all.** No v6
address on the tun, no v6 routes, no v6 block. `NetworkConfig::Apply` sets
`ifRow.Family = AF_INET` only, and `kIncludedV4Routes` is v4 by construction.

On any dual-stack network — which is most home broadband and essentially all
mobile — the physical adapter keeps a working `::/0`, Happy Eyeballs prefers
IPv6, and the large majority of traffic to the major CDNs goes **around** the
tunnel in the clear. For a product whose entire proposition is that traffic goes
through the network, this is worse than any routing subtlety in §2.

### Recommendation: block IPv6 at WFP. Do not blackhole it with routes.

Sources conflict on this and I am picking a side.

The case for routes (`::/1` + `8000::/1` on the tun) is that a routing failure
gives a fast, clean `WSAENETUNREACH` while a firewall drop makes connections
hang. That argument is sound **only when the tun has no usable IPv6 source
address**. Ours will: wintun interfaces get an autoconfigured link-local
address, so source selection succeeds, the packet is handed to our pump, and
our pump — which has no v6 path — drops it. That is a *silent* blackhole and
the caller eats a full TCP connect timeout. The route approach gives us the
slow failure, not the fast one, which is the opposite of the usual advice.

A WFP block at `FWPM_LAYER_ALE_AUTH_CONNECT_V6` is classified at `connect()`
time and fails the call synchronously, so Happy Eyeballs falls back to IPv4
immediately. *(Fast-fail is the expected ALE semantics; I did not verify the
exact `WSAE*` code or the Happy Eyeballs timing empirically — mark as
unverified.)*

Three further reasons WFP wins here:

1. **It does not depend on the route table.** A roam, a resume, or a third
   party rewriting routes cannot re-open the leak. A blackhole route can be
   displaced by a longer prefix from other software; a block filter cannot.
2. **It reverts itself on process death.** Open the engine with
   `FWPM_SESSION_FLAG_DYNAMIC` and, per MS,
   *"any objects added during the session are automatically deleted when the
   session ends"* — including when the process crashes. That is the identical
   property to "the wintun adapter dies with the process", which
   `NetworkConfig.h:44-56` already identifies as our primary restore path. The
   two failure stories stay the same story. WireGuard for Windows uses exactly
   this (`tunnel/firewall/blocker.go`, session flagged dynamic, sublayer at
   `weight: ^uint16(0)`).
3. **It costs no routes to unwind**, so `CrashRevert` — which must not block,
   must not allocate, and must not make an RPC — has nothing new to do.

Do **not** disable IPv6 on the adapters. It is persistent, it is user-visible,
and it is exactly the kind of change that outlives a crashed process.

### Concretely

New `WfpGuard` alongside `NetworkConfig`, opened in step 6 and closed in
`StopLocked`:

- `FwpmEngineOpen0` with `FWPM_SESSION_FLAG_DYNAMIC`.
- Own provider + sublayer at max weight, mirroring `blocker.go`.
- Block at `FWPM_LAYER_ALE_AUTH_CONNECT_V6` and
  `FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V6` at weight 0.
- Permit above it, mirroring `rules.go`: loopback
  (`FWPM_CONDITION_FLAGS` / `FWP_CONDITION_FLAG_IS_LOOPBACK`), NDP
  (ICMPv6 types 133–137), and DHCPv6 — so link-local discovery and Hyper-V
  keep working and we do not break the machine's local networking to close a
  leak on the public side.

Three warnings, each learned from a reference:

1. **Use the WFP API, not `netsh advfirewall`.** Tailscale's
   `router_windows.go` carries the comment that changing the Windows firewall
   *"can be REALLY SLOW for reasons not understood. Like 4 minutes slow"*
   (tailscale#785), which is why their rule changes run in a detached goroutine
   with backoff. `FwpmFilterAdd0` is a different subsystem and is fast, but do
   not let anything in step 6 shell out to `netsh` — a four-minute stall inside
   `StartLocked` while holding `mutex_` would be indistinguishable from a hang.
2. **Fail open on exit, and do it deliberately.** Mullvad does the opposite:
   `impl Drop for Firewall` calls `winfw::deinit(BlockingUntilReboot)` and then
   `apply_persistent_blocking()`, explicitly so *"there is no window during
   which traffic is permitted"* — filters that survive process death **and
   reboot**. That is right for a privacy product where a leak is the worst
   outcome. It is wrong for us: our worst outcome is a consumer who cannot
   reach the internet to ask for help, which is the whole argument of
   `NetworkConfig.h:44-56`. A dynamic session gives us the opposite guarantee
   and we should take it knowingly, not by accident.
3. **Do not fail the tunnel if WFP setup fails.** A v6 block that could not be
   installed is a leak to log loudly, not a reason to deny the user a working
   v4 tunnel — the same call `Apply` already makes for DNS
   (`NetworkConfig.cpp:178-185`).

Ship v4-only with v6 blocked. Tunnelling v6 is a separate project on the SDK
side (`tunnelLocalAddress` and `tunnelDnsAddressesIpv4` are both v4-shaped) and
should not gate the first release.

---

## 5. wintun specifics

### Ring buffer

We pass `kRingCapacity = 0x400000` (4 MiB) to `WintunStartSession`
(`TunnelController.cpp:19`). `api/wintun.h` documents the capacity as *"Must be
between `WINTUN_MIN_RING_CAPACITY` and `WINTUN_MAX_RING_CAPACITY` (incl.) Must
be a power of two"*, with `WINTUN_MIN_RING_CAPACITY = 0x20000` (128 KiB) and
`WINTUN_MAX_RING_CAPACITY = 0x4000000` (64 MiB). 4 MiB is comfortably inside
that.

**Correction worth knowing: capacity is per *ring*, so budget 2× plus change.**
`api/session.c` does a single `VirtualAlloc` of `TUN_RING_SIZE(Capacity) * 2`
— send ring and receive ring, contiguous — where
`TUN_RING_SIZE(C) = sizeof(TUN_RING) + C + (TUN_MAX_PACKET_SIZE - TUN_ALIGNMENT)`,
i.e. a 12-byte header plus a one-max-packet wrap guard on *each* ring. Our
4 MiB capacity therefore commits **~8 MiB**, not 4. wireguard-go uses
`StartSession(0x800000)` (8 MiB → ~16 MiB total). Ours is the more conservative
choice and I would keep it: this runs on every machine, all the time.

**There is no published sizing-vs-throughput guidance** — not in the header,
the README, wintun.net, or the mailing list. The 8 MiB figure everyone quotes
is a convention propagated from wireguard-go, not a derived number. Treat any
sizing rule, including this one, as unverified.

`WintunAdapter::Send` (`Wintun.cpp`) drops when `AllocateSendPacket` returns
null (`ERROR_BUFFER_OVERFLOW`, ring full) and **discards the return value at the
call site** (`PacketPump.cpp:37`). Dropping is right, and it is what upstream
does — wireguard-go's entire handling is `case windows.ERROR_BUFFER_OVERFLOW:
continue // Dropping when ring is full.` The ring is the host's receive queue;
a full queue is congestion, and blocking or retrying there inverts flow control
and builds bufferbloat.

What is wrong is that the drop is **invisible**. Nobody upstream counts them
either, which is exactly why throughput regressions in this layer are so hard
to attribute. Add a counter and log it periodically: "the tunnel is slow" and
"the inbound ring is overflowing" are the same symptom, and only a counter
tells them apart.

### MTU — the concrete bug

`kTunnelMtu = 1440`, hardcoded, applied only to the `AF_INET` interface row.

Wintun itself does not set the interface MTU; the caller does it through
`SetIpInterfaceEntry`'s `NlMtu`, which is what we do. The value is the problem.
1440 assumes a 1500-byte path. Subtract our own encapsulation (outer IPv4 20 +
UDP 8 + QUIC long/short header and AEAD tag, call it 40–60) from 1500 and the
honest ceiling is around 1410–1430 on a *good* path. On a 1492-byte PPPoE link
or a 1400-byte mobile hotspot, 1440 is simply wrong, and the symptom is the
classic one: interactive traffic works, then a large upload or a TLS handshake
with a big certificate chain stalls forever, because the ICMP "fragmentation
needed" that PMTUD depends on is dropped somewhere.

Note that 1440 is not an arbitrary number — it is Donenfeld's *IPv4-exclusive*
figure. From the WireGuard list (2017-12-11): the worst case is IPv6 outer,
`1500-(40+8+4+4+8+16) = 1420`, *"however, if you know ahead of time that you're
going to be using IPv4 exclusively, then you could get away with N=1440."*
Someone picked 1440 correctly for a v4-only WireGuard tunnel on a 1500-byte
link. **Neither condition is ours** — our encapsulation is QUIC/WebSocket, not
plain UDP+WireGuard, and we do not know the link is 1500.

**Recommendation:** derive it, the way `tunnel/mtumonitor.go` does. Their exact
algorithm is worth copying because it is already tuned for this problem: take
the **lowest-metric non-tunnel default route's** interface MTU, subtract a
fixed overhead (80 for them), clamp to the family floor, and **re-run on every
route change**. `EgressMonitor` already picks exactly that interface and
already fires on change, so this is a small addition to code we have. Clamp to
`[1280, 1440]`: 1280 is the IPv6 architectural minimum, and
`Set-NetIPInterface` documents floors of 576 (IPv4) / 1280 (IPv6).

Three Windows-specific traps in doing this, all confirmed:

1. **Set `NlMtu` per address family — and the failure is silent.**
   `MIB_IPINTERFACE_ROW` is keyed on `{Family, InterfaceLuid, InterfaceIndex}`.
   tailscale#5914 is this bug shipped: IPv4 correctly set to 1280, the IPv6 row
   left at 1420, producing *intermittent* browsing failures through exit nodes —
   intermittent precisely because IPv4 keeps working. Fixed in tailscale#5915.
2. **Never reuse a row fetched from an `AF_UNSPEC` table.** A 2021 list thread
   ("Windows: wintun MTU is not set") had `Set()` fail with "The parameter is
   incorrect" for AF_INET while AF_INET6 succeeded; the fix was to fetch
   per-family. We already call `GetIpInterfaceEntry` with an explicit
   `Family` — keep it that way.
3. **`NlMtu = 0` means "leave unchanged", not "reset".** A zero-initialised
   `MIB_IPINTERFACE_ROW` silently skips the MTU. We read-modify-write, so we
   are fine; do not "simplify" that into a fresh struct.

**On MSS clamping I was initially too dismissive — the corrected position:**
the primary fix is `NlMtu`, and for locally-originated TCP that genuinely is
the whole fix, because Windows derives the advertised MSS from the tun's MTU.
Windows offers no useful clamp of its own: `netsh interface tcp set global` has
no MSS parameter, and `Set-NetIPInterface -ClampMss` is scoped to **forwarded**
packets, has no `MIB_IPINTERFACE_ROW` member (so it is unreachable from
`SetIpInterfaceEntry`), and is boolean rather than a value.

But **we do not need Windows to do it.** We own every packet crossing the
wintun rings, so OpenVPN-style `--mssfix` is ~30 lines in `PacketPump`: on
IPv4/IPv6 + TCP with SYN set, walk the TCP options for kind 2 (MSS, length 4),
lower it if over budget, and fix the checksum incrementally. **No driver
required.** Do it in *both* directions if you do it at all — clamping only
outbound SYNs leaves the peer→us direction oversized, which is the direction
that breaks downloads. Treat it as belt-and-braces behind change #3, valuable
when the path *inside* the tunnel is smaller than our tunnel MTU, which is the
case `NlMtu` alone cannot see.

For context on why this matters more than it sounds: Windows' own PMTU
black-hole detection (`EnablePMTUBHDetect`, on by default since Vista) "fixes"
the problem by noticing repeated unacknowledged retransmits and then *clearing
the Don't Fragment bit* — i.e. seconds of stall per connection, resolved by
accepting fragmentation. That is the fallback we are trying not to rely on.

### Adapter GUID, lifecycle, orphans

`api/wintun.h` describes `RequestedGUID` as *"The GUID of the created network
adapter, which then influences NLA generation deterministically. If it is set
to NULL, the GUID is chosen by the system at random, and hence a new NLA entry
is created for each new adapter."* So pinning it (`ids::kTunAdapterGuid`) is
right: without it, Windows creates a fresh "Network 2/3/4…" profile and can
re-prompt for network category on every start.

The README is then unusually frank: it is called *"requested"* GUID because
**"the API it uses is completely undocumented, and so there could be minor
interesting complications with its usage."** Our code's assumption that it is a
request rather than a guarantee is therefore well founded.

**And the complication is real, with teeth.** A June 2021 WireGuard list report:
killing the service PID produced, on restart,
`[Wintun] CreateAdapter: Requested GUID {…} has leftover residue`. Usually
recoverable — but **twice it hard-failed** ("The system cannot find the file
specified", then "Cannot create a file when that file already exists") and
required a **reboot**; reinstalling did not help. The residue lives in
`HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{GUID}`,
which can survive devnode removal and block reuse of that GUID.

This is a direct consequence of pinning a GUID, and it is a trade we should
make knowingly: a stable NLA identity in exchange for a rare, reboot-requiring
failure after a hard kill. It also means our fallback path — wintun assigning a
*different* GUID — is not hypothetical, and is exactly why we log the GUID
actually assigned versus the one requested
(`TunnelController.cpp:174-184`), which is the right instinct, and
`SweepOrphanedTunnel` correctly matches on GUID *or* alias-plus-wintun-shaped-
device. The asymmetric bias in that sweep (prefer a false negative over
deleting routes from a real adapter, `NetworkConfig.cpp:294-300`) is the right
call and should not be relaxed.

### What actually happens when the process dies — and our claim holds

`NetworkConfig.h:44-56` asserts that the adapter dies with the process and
takes the routes with it. **That is correct on wintun 0.14+, but not for the
reason the comment implies, and the precise mechanism is worth writing down
because it changes what "recovered" means.**

- Removal is done by the **DLL, not the driver**. `WintunCloseAdapter` calls
  `SwDeviceClose` then `AdapterRemoveInstance` (pure SetupAPI: `DIF_REMOVE`,
  `DI_REMOVEDEVICE_GLOBAL`). There is **no kernel-side removal on last-handle-
  close** — so `WintunCloseAdapter`'s *"if adapter was created with
  WintunCreateAdapter, removes adapter"* is about which code path runs, not a
  refcount.
- On `TerminateProcess`, none of that DLL code runs. What saves us is that
  wintun never calls `SwDeviceSetLifetime`, so the device keeps the default
  `SwDeviceLifetimeHandle`: the kernel closes the handle on process death, PnP
  issues a **surprise removal**, and the adapter stops working and disappears
  from `ncpa.cpl`. **The routes and address go with it.** Our primary restore
  path is sound.
- But `DIF_REMOVE` never ran, so the **devnode and registry residue survive**
  as a non-present phantom carrying a problem code. "The network came back" and
  "nothing was left behind" are therefore different claims, and only the first
  is guaranteed.
- wintun ships its own reaper for the phantom:
  `AdapterCleanupOrphanedDevices()` enumerates `GUID_DEVCLASS_NET` filtered by
  the wintun enumerator and removes only devices where
  `CM_Get_DevNode_Status` reports `DN_HAS_PROBLEM` — live adapters owned by
  running processes are skipped. It runs from `WintunCloseAdapter`,
  `WintunDeleteDriver`, **and DLL init**. So merely loading `wintun.dll` in our
  step 1 already reaps our own phantoms, before `SweepOrphanedTunnel` even
  looks.

**Version correction:** the "pool" concept was removed in **0.14** (commit
`544fdaaf`, "api: rewrite based on SwDevice", 2021-10-12), not 0.11. This
matters for reading old bug reports: **pre-0.14 adapters were persistent by
design** — real installed devices surviving process exit *and reboot*, removed
only by an explicit `WintunDeleteAdapter`, with `WintunOpenAdapter(Pool, Name)`
existing precisely to re-attach to yesterday's adapter. From 0.14 they are
ephemeral. A pre-0.14 report of a "stuck adapter" is describing designed
behaviour, not a bug. **Confirm which wintun version we ship** — our entire
crash-safety story assumes 0.14+.

Our escape hatch (`urnetworkd revert`, elevated, refusing while a `urnetworkd`
holds the control pipe) is the correct shape. Test B2 in
`docs/superpowers/reports/2026-08-05-service-bringup.md` already targets this;
it is the single most important thing to actually run.

**The failure mode is real and well documented in the wild.** Tailscale's
tracker is the best public evidence, and three issues are directly instructive:

- **tailscale#1290** — uninstall leaves the Wintun interface behind, and the
  leftover cannot be deleted from the adapter context menu. Our
  `urnetworkd revert` escape hatch is the answer to exactly this, and it is why
  it must be documented somewhere the user can find *without* a working network
  connection.
- **tailscale#1128** — `WintunOpenAdapter: adapter with the same name exists in
  another pool`, and creation then fails with "Cannot create a file when that
  file already exists." This is what a stranded adapter does to the *next*
  start, and it is why `SweepOrphanedTunnel` running before step 1 matters.
- **tailscale#7937** — *"Tailscale removes all wintun adapters on service
  restart, not just its own."* This is the cautionary tale for our sweep, and
  it validates the asymmetric bias already in `NetworkConfig.cpp:294-300`: a
  sweep that over-matches destroys a **competitor's** working tunnel. Do not
  relax the "alias match AND wintun-shaped device" requirement, and never sweep
  on device description alone.

Two adjacent hazards worth knowing about before support tickets arrive:

- **Coexistence, and the single most dangerous call in the wintun API.**
  `WintunDeleteDriver` reaps orphans and then calls `SetupUninstallOEMInfW()`
  on **every** matching wintun INF — detaching every wintun adapter on the
  machine, other vendors' included, and without cleanly removing the live
  devices first, so their adapters end up *broken* rather than gone. That is
  the mechanism behind tailscale#7937 (*"all other wintun adapters are disabled
  & removed (even if in use by OpenVPN)"*), amnezia-client#1874 and
  OpenConnect#699. **Never call `WintunDeleteDriver` as startup hygiene or in
  teardown — reserve it for uninstall.** We resolve the export in `Wintun.cpp`
  but do not call it, which is correct; keep it that way, and if anyone adds a
  "clean up wintun" path, this is the review comment. Note also tailscale#799:
  WireGuard for Windows repairing the shared driver on every connection broke
  Tailscale each time. Anything shipping wintun shares one global driver — be
  ready to say which version we ship.
- **Externally deleted adapter.** tailscale#11222: `tailscaled` becomes
  inoperative if its adapter is removed out from under it. Our packet pump
  would sit on `ReadWaitEvent` forever in that case. Worth a health check that
  turns "silently carrying nothing" into a visible error.

### The boot race — copy WireGuard's retry, and know which error to catch

`SetIpInterfaceEntry` returns **`ERROR_NOT_FOUND`** when the interface exists
but the row for that address family does not *yet* — as distinct from
`ERROR_FILE_NOT_FOUND`, which means no such interface at all. That distinction
is the whole boot race: a freshly created wintun adapter has no AF_INET row for
a moment, and step 6 runs immediately after step 1.

WireGuard defends with a **15-attempt, 1-second retry loop on `ERROR_NOT_FOUND`**,
gated on `services.StartedAtBoot()`, and only fires configuration after a
`MibAddInstance` notification for that LUID **and family**
(`tunnel/interfacewatcher.go`). A service with `SERVICE_AUTO_START` racing the
network stack is exactly our situation, and today `Apply` logs
`netcfg: set MTU/metric failed` and carries on — a tunnel that comes up with a
1500-byte MTU and reports success.

Also copy `tunnel/addressconfig.go`'s address handling: it retries
`SetIPAddressesForFamily` on `ERROR_OBJECT_ALREADY_EXISTS` by calling
`cleanupAddressesOnDisconnectedInterfaces()`, deleting the same address from
any interface that is not `IfOperStatusUp`. We treat
`ERROR_OBJECT_ALREADY_EXISTS` as success (`NetworkConfig.cpp:143`), which is
fine for our own leftover but does not handle a *disconnected other* interface
squatting on `169.254.2.1`.

---

## 6. Recommended data path, as a change list

Ordered by value. None of it needs a kernel driver.

| # | Change | Where |
| --- | --- | --- |
| 1 | **Block IPv6 at WFP** (dynamic session, block v6 ALE connect/recv, permit loopback + NDP + DHCPv6) | new `WfpGuard`, opened in step 6, closed in `StopLocked` |
| 2 | **Give the SDK its own resolver** pinned to the physical NIC's DNS servers | `EgressMonitor` pushes servers; `ConnectSettings.Resolver` set to a `PreferGo` resolver over `egressDialer` |
| 3 | **Derive MTU from the physical link**, clamp `[1280, 1440]`, re-apply on egress change; set both families | `EgressMonitor` + `NetworkConfig::Apply` |
| 4 | **Log the platform socket's `getsockname()`** and assert it is not the tun address | `connect`, surfaced through the existing `[net]dial` logging |
| 5 | **Register `NotifyUnicastIpAddressChange` + `NotifyRouteChange2`**, not just `NotifyIpInterfaceChange` — a DHCP renew currently leaves a stale source address in the split-tunnel driver. **Must ship with #8**: adding `NotifyRouteChange2` turns our own step 6 into a 31-event burst into our own handler | `EgressMonitor::Start` |
| 6 | **Replace the on-link next hop** with a pseudo-gateway inside the tun's `/24` (avoids the last-address-in-range `WSAEADDRNOTAVAIL` case) | `NetworkConfig.cpp:29-47 AddTunRoute` |
| 7 | **Compute `kIncludedV4Routes` at runtime**; add loopback, link-local, multicast/broadcast (and consider `100.64/10`) to the exclusion list | `NetworkConfig.cpp:79-88` |
| 8 | **Debounce egress-change notifications** ~200 ms, 2 s ceiling (Mullvad's `BurstGuard` numbers) | `EgressMonitor::OnChange` |
| 9 | `DadTransmits = 0`, `RouterDiscoveryDisabled` on the tun; boot-time retry around interface configuration | `NetworkConfig::Apply` |
| 10 | **Count and log wintun send-ring drops** (`ERROR_BUFFER_OVERFLOW`) | `Wintun.cpp` / `PacketPump.cpp:37` |
| 10b | **Confirm we ship wintun ≥ 0.14** — the whole crash-safety story assumes SwDevice-lifetime (ephemeral) adapters; pre-0.14 adapters are persistent *by design* | build / `PROVENANCE.md` |
| 11 | **Handle `SERVICE_ACCEPT_POWEREVENT`** and tell the SDK about resume | `main.cpp` service control handler |

Things that are already right and should not be changed: the fence before step
6; step 2 running before any SDK object exists; never deleting the physical
default route; never pushing egress index 0 while a valid index is retained;
routes keyed on the tun LUID; the asymmetric bias in `SweepOrphanedTunnel`;
routes-only `CrashRevert` with no DNS RPC.

---

## 7. Failure modes, ranked by how badly they hurt

| # | Failure | Blast radius | Mitigation |
| --- | --- | --- | --- |
| 1 | **Machine loses all connectivity and does not recover** — routes installed, process gone, adapter orphaned | total; user cannot reach the internet to get help | Primary: adapter dies with the process. Then `CrashRevert` (routes only), then `SweepOrphanedTunnel` at next start, then `urnetworkd revert` elevated. Already built — **run test B2** |
| 2 | **R1 loop** — the service's own sockets route into its own tun | tunnel never establishes or stalls minutes in and never recovers; looks like a server problem | `IP_UNICAST_IF` set pre-connect from a monitored index; never push 0; fail the dial when every option fails. **Gap: the OS resolver — change #2** |
| 3 | **IPv6 leak** — most traffic bypasses the tunnel in the clear | silent, total privacy failure; the user believes they are protected | WFP v6 block (change #1) |
| 4 | **DNS leak (R6)** — SMHNR queries the physical resolver in parallel | queries visible to the local network / ISP | Tun DNS at a lower metric is not sufficient. Needs a WFP `blockDNS`-style filter permitting only the tun's resolvers — same `WfpGuard` as #1 |
| 5 | **MTU black hole** — 1440 on a smaller path | "the internet works but big pages/uploads hang"; very hard to attribute | Derive MTU from the physical link (change #3) |
| 6 | **DHCP renew changes the physical source address**, split-tunnel driver keeps rebinding excluded apps to a reclaimed address | every excluded app loses connectivity; the tunnel looks fine, so blame lands elsewhere | Register `NotifyUnicastIpAddressChange` (change #5) |
| 7 | **Roam leaves the egress pinned to a dead interface** | tunnel dies until the next change notification | Retain-and-revalidate (built); add debounce (change #8) |
| 8 | **On-link next hop breaks the last address of each tun prefix** | a handful of specific public IPs unreachable only while connected; effectively unreproducible in support | Pseudo next hop inside the tun `/24` (change #6) |
| 9 | **Suspend/resume kills SDK sessions with no signal** | tunnel appears up, carries nothing | Power event handling (change #11) |
| 10 | **Boot race** — service starts before the network stack is ready | tunnel fails to start once per boot; usually self-heals on retry | Retry loop around interface configuration (change #9) |
| 11 | **Inbound ring overflow** | packet loss presenting as slowness | Counter + log (change #10) |
| 11b | **Our adapter is deleted out from under us** (another wintun client's sweep, Device Manager, a driver repair) | pump blocks on `ReadWaitEvent` forever; tunnel reports Up and carries nothing | Real — tailscale#11222, and tailscale#7937 shows another vendor doing the deleting. Health check on the adapter; fail visibly |
| 11c | **wintun driver coexistence** with WireGuard/Tailscale on the same machine | either product breaks the other on connect | tailscale#799. Never call `WintunDeleteDriver` in teardown (we don't — keep it); know which wintun version we ship |
| 12 | **Weak host send / IP forwarding enabled on another interface** routes tunnel-destined traffic back into the tun | loop the LUID skip cannot catch | WireGuard detects and warns (`tunnel/pitfalls.go pitfallWeakHostSend`). Detect and warn; do not silently "fix" a machine-wide setting |
| 13 | **WSL2 / Hyper-V guest traffic bypasses the tunnel** | a developer's whole Linux environment leaks while the tray says connected | Real: Mullvad published *"Linux under WSL2 can be leaking"* and ships `firewall/windows/hyperv.rs` WMI rules for it, because guest traffic reaches WFP only as L2 frames. Note that they exempt the **connected** state *"since the routing table will ensure that traffic is tunneled"* — so this may be a non-issue for a route-based capture like ours. **Unverified for us; test it before claiming either way** |

---

## 8. Driver or no driver

### Ships without a kernel driver — everything in the data path

- wintun (a driver, but Microsoft-attested and shipped by the WireGuard
  project; we load it, we do not sign it)
- full-tunnel capture via the complement-prefix route set
- self-exclusion via `IP_UNICAST_IF` / `IPV6_UNICAST_IF`
- DNS on the tun interface via `SetInterfaceDnsSettings`
- IPv6 block, DNS leak guard, and a killswitch — **all** of it: user-mode
  `FwpmFilterAdd0` supports `FWP_ACTION_PERMIT` and `FWP_ACTION_BLOCK`, and
  `FWPM_CONDITION_ALE_APP_ID` (via `FwpmGetAppIdFromFileName0`) plus
  `FWPM_CONDITION_ALE_USER_ID` is enough to exempt ourselves, exactly as
  `tunnel/firewall/rules.go permitWireGuardService` does
- correct MTU, and therefore correct locally-originated TCP MSS
- crash recovery and orphan sweeping

### Genuinely requires a signed kernel driver

- **Per-application split tunnelling** — our `app/driver/SplitTunnel.sys`.
  Rerouting a specific process's sockets needs `FWPM_LAYER_ALE_BIND_REDIRECT` /
  `ALE_CONNECT_REDIRECT`, which are callout layers. There is no user-mode
  redirect. Mullvad reaches the same conclusion and ships
  `mullvad-split-tunnel.sys` behind `\\.\MULLVADSPLITTUNNEL` with an IOCTL
  surface (`RegisterProcesses`, `RegisterIpAddresses`, `SetConfiguration`) that
  is a near-match for our `Ioctl.h` — including redirecting sockets bound to
  `0.0.0.0`, `::` **or the tunnel IP**, which is a case our `Ioctl.h` contract
  does not currently mention and is worth checking in `Driver.c`'s classify
  path. `SplitTunnelClient` degrading to a no-op when the driver is absent
  (`SplitTunnelClient.h`) is the right structure: ship without it, add it when
  the signing story is ready.
- Kernel-side crypto/forwarding for throughput (the OpenVPN DCO-Win /
  WireGuardNT class of optimisation). A performance question, not a correctness
  one — and the numbers say it is a smaller question than it sounds. OpenVPN's
  own iperf3 figures (AES-256-GCM):

  | Data path | Down | Up |
  | --- | --- | --- |
  | `ovpn-dco-win` (kernel) | 1.74 Gbit/s | 1.51 Gbit/s |
  | **wintun** | **1.20** | **1.20** |
  | tap-windows6 | 0.50 | 0.60 |

  The large jump is tap-windows6 → wintun (~2.4×). **DCO's incremental win over
  wintun is only ~25–45%**, bought with a signed kernel driver, AEAD-only
  ciphers, L3-only, no compression, and client/p2p-only on Windows. Not a day-
  one trade, and arguably not a year-one trade.

  Two corrections to the framing while we are here. **WireGuard for Windows no
  longer uses wintun at all** — commit `1288280` switched it to WireGuardNT and
  v0.5 removed the userspace path. So when this document cites
  `tunnel/addressconfig.go`, `mtumonitor.go` or `interfacewatcher.go`, those
  remain current (route/address/MTU configuration is shared), but their *data
  path* is a kernel driver and is not a model for ours. **Cloudflare WARP went
  the other way**: per Cloudflare's own docs it *"replaced **WinDivert** with
  **WinTun** architecture"* — off a WFP-based capture driver, onto a plain L3
  TUN adapter, making exactly three OS changes (create the interface, edit the
  routing table, add a firewall rule) with no Cloudflare-authored kernel driver
  anywhere. A company with every resource to write one chose not to, and bought
  protocol agility with it: swapping WireGuard for HTTP/3 MASQUE became a pure
  userspace change. That is the strongest external evidence that our
  architecture is the right one.
- Packet rewriting in the stack (MSS clamping for *forwarded* traffic). We do
  not forward, so we do not need it.

**Conclusion: nothing in the recommended change list needs a driver.** The one
feature that does — per-app split tunnelling — is already isolated behind a
component that no-ops cleanly when absent.

---

## 9. Open questions / unverified

- Whether Windows still issues SMHNR queries across all interfaces in parallel
  on current builds, and how the tun's metric-1 interface affects the order.
  **Do not design around it either way** — change #2 removes the dependency.
- The exact `WSAE*` code and timing from a WFP v6 `ALE_AUTH_CONNECT_V6` block,
  and the resulting Happy Eyeballs fallback latency. Measure before shipping.
- Whether `224.0.0.0/3` in `kIncludedV4Routes` actually affects
  mDNS/SSDP/LLMNR, given those normally use `IP_MULTICAST_IF`.
- What Android and iOS do about IPv6 in the URnetwork clients. §4 recommends
  blocking; if the mobile clients leak v6 today, that is the same bug on three
  platforms and worth raising as one issue rather than three.
- The real end-to-end encapsulation overhead for the QUIC/WebSocket transports,
  needed to pick the constant in change #3. Measure; do not guess — 1440 came
  from a *WireGuard, IPv4-only, 1500-byte-link* calculation that is not ours.
- **Which wintun version we ship.** The crash-safety argument in
  `NetworkConfig.h:44-56` is valid on 0.14+ (SwDevice-lifetime, ephemeral) and
  invalid pre-0.14 (adapters persistent by design). This should be pinned and
  recorded, not inferred.
- Whether `Set-NetIPInterface -ClampMss` affects locally-originated traffic
  despite being documented as forwarded-only. A five-minute test with a capture
  would settle it; it does not change the recommendation (userspace `mssfix` in
  the pump is available regardless) but it would close the question.
- Whether a second process holding a `WintunOpenAdapter` handle can make
  `DIF_REMOVE` fail — wintun logs "Failed to remove adapter when closing" and
  continues, so it is possible in principle, but no public report pins an
  orphan on it.
- Whether WireGuard for Windows' lack of power-event handling is deliberate.
  Our long-lived SDK sessions make our situation different regardless.
- Whether WSL2 / Hyper-V guest traffic is captured by our route set. Mullvad's
  own code implies a route-based full-tunnel capture is sufficient in the
  connected state, but this is exactly the kind of thing that should be tested
  rather than reasoned about.
- **Reproduce the on-link next hop / `WSAEADDRNOTAVAIL` case** before doing
  change #6. Two independent implementations avoid on-link next hops for range
  routes and Tailscale documents why, but I did not reproduce it, and the fix
  should not go in on hearsay alone.
- Whether the last-address hazard interacts with the pseudo-next-hop choice —
  i.e. confirm that a next hop inside the tun's own `/24` resolves through the
  tun's on-link subnet route as intended and does not itself become
  unreachable.

---

## Sources

- [wireguard-windows `tunnel/addressconfig.go`](https://raw.githubusercontent.com/WireGuard/wireguard-windows/master/tunnel/addressconfig.go)
- [wireguard-windows `tunnel/winipcfg/luid.go`](https://raw.githubusercontent.com/WireGuard/wireguard-windows/master/tunnel/winipcfg/luid.go)
- [wireguard-windows `tunnel/firewall/rules.go`](https://raw.githubusercontent.com/WireGuard/wireguard-windows/master/tunnel/firewall/rules.go)
- [wireguard-windows `tunnel/firewall/blocker.go`](https://git.zx2c4.com/wireguard-windows/tree/tunnel/firewall/blocker.go)
- [wireguard-windows `tunnel/interfacewatcher.go`, `tunnel/pitfalls.go`](https://raw.githubusercontent.com/WireGuard/wireguard-windows/master/tunnel/interfacewatcher.go)
- [wireguard-windows `tunnel/defaultroutemonitor.go` @ v0.3.16](https://raw.githubusercontent.com/WireGuard/wireguard-windows/v0.3.16/tunnel/defaultroutemonitor.go)
- [wireguard-go `conn/bind_windows.go`](https://raw.githubusercontent.com/WireGuard/wireguard-go/master/conn/bind_windows.go)
- [wireguard-nt `driver/socket.c`](https://git.zx2c4.com/wireguard-nt/tree/driver/socket.c)
- [tailscale `net/netns/netns_windows.go`](https://raw.githubusercontent.com/tailscale/tailscale/main/net/netns/netns_windows.go)
- [tailscale `wgengine/router/osrouter/ifconfig_windows.go`, `router_windows.go`](https://github.com/tailscale/tailscale/tree/main/wgengine/router/osrouter)
- [tailscale `wf/firewall.go`](https://github.com/tailscale/wf/blob/main/firewall.go)
- [tailscale `net/netmon/interfaces_windows.go` — `GetWindowsDefault`](https://github.com/tailscale/tailscale/tree/main/net/netmon)
- [mullvadvpn-app `talpid-routing/src/windows/route_manager.rs`, `get_best_default_route.rs`, `default_route_monitor.rs`](https://github.com/mullvad/mullvadvpn-app/tree/main/talpid-routing/src/windows)
- [mullvadvpn-app `windows/winfw/src/winfw/fwcontext.cpp`, `rules/multi/permitendpoint.cpp`](https://github.com/mullvad/mullvadvpn-app/tree/main/windows/winfw)
- [mullvadvpn-app `talpid-core/src/split_tunnel/windows/`](https://github.com/mullvad/mullvadvpn-app/tree/main/talpid-core/src/split_tunnel/windows)
- [mullvadvpn-app `docs/security.md`](https://github.com/mullvad/mullvadvpn-app/blob/main/docs/security.md)
- [MS: IPPROTO_IP socket options (`IP_UNICAST_IF`)](https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-ip-socket-options)
- [MS: `FWPM_SESSION0` / `FWPM_SESSION_FLAG_DYNAMIC`](https://learn.microsoft.com/en-us/windows/win32/api/fwpmtypes/ns-fwpmtypes-fwpm_session0)
- [MS: Permitting and blocking applications and users (WFP)](https://learn.microsoft.com/en-us/windows/win32/fwp/permitting-and-blocking-applications-and-users)
- [MS: The route determination process](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-vista/cc766470(v=ws.10))
- [wintun `api/wintun.h`](https://git.zx2c4.com/wintun/tree/api/wintun.h)
- [tailscale#1290 — uninstall does not remove the Wintun interface](https://github.com/tailscale/tailscale/issues/1290)
- [tailscale#1128 — adapter with the same name exists in another pool](https://github.com/tailscale/tailscale/issues/1128)
- [tailscale#7937 — removes *all* wintun adapters on service restart](https://github.com/tailscale/tailscale/issues/7937)
- [tailscale#11222 — inoperative if the adapter is externally deleted](https://github.com/tailscale/tailscale/issues/11222)
- [tailscale#799 — WireGuard for Windows repairing the shared wintun driver](https://github.com/tailscale/tailscale/issues/799)
- [tailscale#5914 / #5915 — IPv6 `NlMtu` row left unset](https://github.com/tailscale/tailscale/issues/5914)
- [wintun `api/session.c`, `api/adapter.c`](https://git.zx2c4.com/wintun/plain/api/session.c)
- [wintun commit `544fdaaf` — "api: rewrite based on SwDevice" (pool removal, 0.14)](https://git.zx2c4.com/wintun/commit/?id=544fdaaf8fb970d9657a59c1fc4c4569de4f2d3e)
- [wireguard-go `tun/tun_windows.go`](https://github.com/WireGuard/wireguard-go/blob/master/tun/tun_windows.go)
- [wireguard-windows `tunnel/mtumonitor.go`](https://git.zx2c4.com/wireguard-windows/plain/tunnel/mtumonitor.go)
- [WireGuard ML 2017-12-11 — MTU sizes (1420 vs 1440)](https://lists.zx2c4.com/pipermail/wireguard/2017-December/002201.html)
- [WireGuard ML 2021-03 — "wintun MTU is not set" (AF_UNSPEC row trap)](https://lists.zx2c4.com/pipermail/wireguard/2021-March/006482.html)
- [WireGuard ML 2021-06 — "Requested GUID has leftover residue"](https://lists.zx2c4.com/pipermail/wireguard/2021-June/006825.html)
- [WireGuardNT announcement — v0.5 drops wintun](https://lists.zx2c4.com/pipermail/wireguard/2021-October/007224.html)
- [MS: `SetIpInterfaceEntry`](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-setipinterfaceentry) · [`MIB_IPINTERFACE_ROW`](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/ns-netioapi-mib_ipinterface_row) · [`Set-NetIPInterface`](https://learn.microsoft.com/en-us/powershell/module/nettcpip/set-netipinterface)
- [MS: Advances in Windows Vista TCP/IP — PMTU black holes](https://learn.microsoft.com/en-us/archive/blogs/wndp/advances-in-windows-vista-tcpip)
- [OpenVPN DCO-Win benchmark (openvpn-devel)](https://www.mail-archive.com/openvpn-devel@lists.sourceforge.net/msg21799.html)
- [Cloudflare One devices FAQ — WinDivert → WinTun](https://developers.cloudflare.com/cloudflare-one/faq/devices-faq/)
- [amnezia-client#1874](https://github.com/amnezia-vpn/amnezia-client/issues/1874) · [OpenConnect#699](https://gitlab.com/openconnect/openconnect/-/issues/699) · [sing-box#3806](https://github.com/SagerNet/sing-box/issues/3806)
- [Go `net/conf.go` — `lookupOrder`](https://go.dev/src/net/conf.go)
- [Tailscale: Programming the Windows firewall with Go](https://tailscale.com/blog/windows-firewall)
- [SANS: Preventing Windows 10 SMHNR DNS leakage](https://www.sans.org/white-papers/40165)
