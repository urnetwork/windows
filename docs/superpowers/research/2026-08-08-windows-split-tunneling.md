# Split tunneling on Windows — what it costs, what we can ship first

**Date:** 2026-08-08
**Status:** research / decision document. No code changed.
**Scope:** routing *some* traffic around the tunnel on the URnetwork Windows client.
**Repo state read:** `beta/custom-server`, read-only.

---

## 0. The straight answer

**App-based split tunneling — "Steam bypasses the VPN" — genuinely requires a kernel
driver on Windows. There is no supported user-mode path, and every shipping
implementation I could find uses one.** Do not let anyone talk you into a
workaround; the workarounds are worse than the driver, and I describe the least-bad
one in §4.4 precisely so it can be rejected on the record.

But that is not the whole question, because there are three *different* features
sold under the name "split tunneling", and only one of them needs the driver:

| Form | Needs a driver? | Status in our tree |
|---|---|---|
| **Destination-based** (exclude an IP/CIDR/host from the tunnel) | **No** | Mechanism already built and running — see §2.2, §5 |
| **App-based bypass** (exclude an *executable*) | **Yes** | Driver skeleton exists, never run; blocked on signing |
| **App-based blocking** ("this app gets no network at all") | **No** | Not built; ~1 week of user-mode WFP work — §4.3 |

So: we can ship a real, defensible, user-visible split-tunneling feature this
quarter without touching kernel code, as long as we call it what it is
(*destination* split tunneling) and do not imply the app picker exists yet.

The three headline recommendations:

1. **Ship destination-based split tunneling next.** The routing machinery is
   already in `NetworkConfig.cpp` and the SDK already emits the `local` verdict for
   it. This is a small, honest feature, not a stunt.
2. **Treat the driver as a next-*year* feature, not next-quarter**, and re-plan the
   signing story before spending engineering on it — `SIGNING.md`'s premise
   (attestation signing is the retail path) is now contradicted by Microsoft's own
   current documentation. See §7. This is the single most important finding here.
3. **Keep the `local` chip, but bind its meaning to destination rules today** and
   fix the harder problem it will create *after* the driver lands: driver-excluded
   apps become **invisible to the inspector entirely**, not shown as `local`. §6.

---

## 1. What is already in the tree (grounding)

I read this before searching anything, and it changes the shape of the answer.

### 1.1 The driver hook that has never run

`app/src/Service/TunnelController.cpp:353-361`, step 7/8:

```cpp
step = "7/8 split tunnel";
splitTunnel_.Open();
excludedPaths_ = config.excluded_app_paths;
allowlist_ = config.allowlist_mode;
PushExcludedToDriver(excludedPaths_, allowlist_);
```

`SplitTunnelClient::Open()` (`app/src/Service/SplitTunnelClient.h`) registers and
starts a kernel service on demand, opens its device, and pushes state over IOCTL.
Every operation is a graceful no-op when the driver is absent, and
`IsAvailable()` feeds the `split_tunnel=driver|none` field in the UP log line
(`TunnelController.cpp:287-289`). The failure mode today is silence, which is the
right default but means nothing anywhere tells a user their app rules are inert.

### 1.2 There is more driver here than "a hook"

`app/driver/` is not a placeholder. It contains `Driver.c` (DriverEntry, WDM
device, IOCTL dispatch, WFP engine/callout registration, classify redirect,
`PsSetCreateProcessNotifyRoutineEx` process-tree tracking), `Ioctl.h`,
`SplitTunnel.inf`, and a WDK vcxproj. `Driver.c:295-296` registers filters at
`FWPM_LAYER_ALE_BIND_REDIRECT_V4/V6`.

`app/driver/README.md` is a genuinely good behavioural spec — it already names the
right hard corners (DNS via `svchost` is not split; loopback needs a
connect-redirect fixup; already-open sockets keep their path; fail-open on every
error). `PROVENANCE.md` records a clean-room discipline so the driver can ship
MPL-2.0 rather than inheriting Mullvad's GPL.

**This materially changes the cost estimate.** The M4/M5 driver milestone is not
"write a WFP callout from scratch" — the design work and the licence-safe sourcing
discipline are done. What remains is hardening, signing, and distribution, and
those are the expensive parts (§7, §8).

### 1.3 Destination splitting is already implemented — for the LAN

`app/src/Service/NetworkConfig.cpp:67-87` does *not* install `0.0.0.0/0`, and does
not use the `0.0.0.0/1 + 128.0.0.0/1` trick either. It installs a hand-computed
**complement prefix set**: all of IPv4 *except* `10/8`, `172.16/12`, `192.168/16`,
as 27 prefixes, each on-link via the tun with `Metric = 0`, plus interface metric 1
(`:154-155`). Longest-prefix-match then sends everything to the tun while RFC1918
falls through to the physical NIC.

That is destination-based split tunneling. It is built, it runs, and its exclusion
set is currently a compile-time constant. **Generalising it to user-supplied CIDRs
is an extension to one table, not new machinery** (§5).

### 1.4 The SDK does its own local egress — this is the important one

`app/third_party/urnetwork-sdk/amd64/urnetwork_sdk.hpp`:

```cpp
struct RouteOverride { bool Local{}; bool Pin{}; };

struct BlockAction {
  ... std::optional<StringList> Hosts; std::optional<StringList> Ips;
  bool Block{};  bool Local{};
  std::optional<BlockOverride> BlockOverride;
  std::optional<RouteOverride> RouteOverride;
  int64_t PacketCount{}; int64_t ByteCount{};
};

struct PacketStats {
  int64_t RemoteEgressPacketCount{};  int64_t RemoteEgressByteCount{};
  int64_t RemoteIngressPacketCount{}; int64_t RemoteIngressByteCount{};
  int64_t LocalEgressPacketCount{};   int64_t LocalEgressByteCount{};
  int64_t LocalIngressPacketCount{};  int64_t LocalIngressByteCount{};
  int64_t BlockEgressPacketCount{};   int64_t BlockEgressByteCount{};
  ...
};
```

`PacketStats` has a **three-way** classification of every packet in the SDK's own
path: remote (to a provider), local (out the box directly), blocked. The Go side
of this is `connect.LocalUserNat` — in the SDK clone at
`C:\Users\ryanm\Downloads\sdk\device_local.go:1462-1497`, `sendPacket()` either
hands the packet to `remoteUserNatClient` or, on the local branch, to
`localUserNat.SendPacket(...)`. That is a **user-mode NAT**: the SDK takes the IP
packet off the tun, egresses it through ordinary sockets (pinned to the physical
NIC via `urnet::setEgressInterfaceIndex`, which `TunnelController` sets and clears
at `:405`), and writes replies back into the tun.

So the platform has two independent ways to send traffic around the tunnel:

- **Before the tun** — route-table exclusion (§1.3). The packet never enters
  wintun. Invisible to the SDK.
- **After the tun** — the SDK's `LocalUserNat`. The packet enters wintun, the SDK
  classifies it, and it leaves through a socket in `urnetworkd`. This is what
  produces `BlockAction.Local` and the `local` chip.

Those are not the same thing and they do not produce the same telemetry. Any UI
copy that treats them as one feature will be wrong about one of them.

### 1.5 How the SDK divides responsibility with the platform

`BlockActionOverride` carries **both** `Hosts` and `AppIds`:

```cpp
struct BlockActionOverride {
  std::optional<std::string> OverrideId;
  std::optional<StringList> Hosts;
  std::optional<StringList> AppIds;
  std::optional<BlockOverride> BlockOverride;
  std::optional<RouteOverride> RouteOverride;
};
struct OverrideLocalAppIds { std::optional<StringList> Included, Excluded; };
```

and `Device::getLocalOverrideAppIds()` hands the app rules back out as two lists.
The contract is explicit: **the SDK enforces host/IP rules itself; app rules are
handed to the platform to enforce.** On Android the platform is
`VpnService.addAllowedApplication` / `addDisallowedApplication`. On Windows the
platform is our driver, and `SdkHost::PushLocalOverrideAppsToDriver()`
(`SdkHost.cpp:2152`) plus `ComputeAppSplit()` (`:1306-1322`) is where the handoff
lives, including Android's "inclusions take precedence" allowlist flip.

The SDK cannot see a process id. It sees IP packets on a tun. That is *why* app
rules are the platform's job on every OS, and it is the structural reason the
Windows answer is a driver.

---

## 2. How app-based split tunneling is actually done on Windows

### 2.1 Windows has no `SO_BINDTODEVICE`, no fwmark, no cgroups

On Linux, per-app splitting is a policy-routing exercise entirely in user space:
mark packets with `fwmark` (or a net_cls cgroup), add an `ip rule`, done. macOS
13+ gives you `NETransparentProxyProvider`, which hands your *user-mode* extension
each flow together with the originating app's bundle id and lets you decline it.

Windows has neither. There is no supported way to bind a socket you do not own to
an interface, no packet mark, and no user-mode flow-interception provider. The one
place in the OS where "which process is opening this socket" and "which interface
does it get" are both available and mutable is a **WFP callout at the ALE layers,
which is kernel mode by definition** — `FwpsCalloutRegister`,
`FwpsAcquireWritableLayerDataPointer`, `FwpsApplyModifiedLayerData` are all
kernel-only. From user mode you can *add filters*, but the only actions available
to a user-mode filter are `FWP_ACTION_BLOCK` and `FWP_ACTION_PERMIT`; every
redirect/inspect action is a `FWP_ACTION_CALLOUT_*` and requires a registered
callout, i.e. a driver. (Microsoft's own user-mode app-filter sample —
[Permitting and Blocking Applications and Users][ms-permit] — is exactly
`FwpmGetAppIdFromFileName0` + `FWPM_CONDITION_ALE_APP_ID` + a block/permit action,
nothing more.)

### 2.2 What the field does

| Vendor | Windows mechanism | Kernel driver? |
|---|---|---|
| **Mullvad** | `mullvad-split-tunnel` / [`win-split-tunnel`][mullvad-repo]: non-PnP KMDF driver; maintains a full process tree via process-notify callbacks, propagates exclusion to children, redirects socket binds, and uses explicit WFP permit/block actions to punch through its own firewall rules | **Yes** |
| **Windscribe** | [`WindscribeSplitTunnel.sys`][ws-wiki] + user-mode `CalloutFilter`; hooks `FWPM_LAYER_ALE_AUTH_CONNECT_V4/V6` and `FWPM_LAYER_ALE_RESOURCE_ASSIGNMENT_V4/V6`, identifies the process by AppId, and rewrites `FWPS_CONNECT_REQUEST` to name a local interface index that bypasses the VPN adapter | **Yes** |
| **WireSock / TunnlTo** | TunnlTo is a UI over WireSock; its README states plainly: *"TunnlTo has always relied on Wiresock, a closed-source network driver."* | **Yes** |
| **Tailscale** | App-based split tunneling on Windows **does not exist**; the open FR ([tailscale#15489][ts-issue]) points at Mullvad's driver as the way to do it | n/a — they didn't ship it |

Windscribe's own cross-platform contrast is the cleanest statement of the problem:
Windows → kernel callout driver; macOS → user-mode `NETransparentProxy`; Linux →
`nftables` + `fwmark`. Windows is the outlier that needs the kernel.

Note what our `Driver.c` chose versus Windscribe: we redirect at
**`ALE_BIND_REDIRECT`** (decide at `bind()`, rewrite the local address), Windscribe
redirects at **`ALE_AUTH_CONNECT` + `ALE_RESOURCE_ASSIGNMENT`** (decide at
`connect()`, rewrite the interface index). Ours is simpler and is why our README
has to document the loopback fixup as a separate connect-redirect pass — at bind
time there is no destination to test. Both are legitimate; ours has the smaller
kernel surface, which for a team without a driver group is the right trade.

**Conclusion for §2: yes, app-based rerouting is a genuine kernel-driver problem.**

---

## 3. Route/IP-based splitting (no driver)

### 3.1 What is achievable

Everything a user means by "don't send my bank / my NAS / my game server through
the VPN, by address":

- exclude a CIDR or a host `/32`
- exclude a hostname, by resolving it and excluding the resulting addresses
- exclude the local network (**we already do this**)

Two implementations are available to us, and they are not equivalent:

**(a) Route-table exclusion (before the tun).** Extend the complement-prefix set in
`NetworkConfig.cpp` so user-excluded ranges are holes in the tun's coverage.
Traffic to them never enters wintun; the OS sends it out the physical NIC with the
real source address. This is a *true* bypass — zero overhead, correct source IP,
inbound works, and it survives `urnetworkd` being busy.

**(b) SDK local egress (after the tun).** A `BlockActionOverride` with
`Hosts`/`Ips` and `RouteOverride{Local:true}`; the packet enters the tun, the SDK
NATs it out through its own socket. Slightly more overhead, and the excluded
traffic depends on `urnetworkd` staying alive — but it works on *hostnames the SDK
observed in DNS*, and it is the only one of the two that produces telemetry.

We should implement (a) and let (b) keep doing what it already does. They compose:
(a) for user-declared address ranges, (b) for host-pattern rules.

### 3.2 The sharp edges when the tunnel is the default route

1. **Complement arithmetic, not "add an exclude route".** Because we deliberately
   do *not* own `0.0.0.0/0`, you cannot exclude a range by installing a more
   specific route to it — there is nothing more specific to beat. You must **remove
   it from the tun's prefix set**, which means recomputing the complement of
   (RFC1918 ∪ user exclusions) over `0.0.0.0/0`. That is fiddly, exactly-once code
   that deserves a property test (`complement(S)` must partition, must be disjoint,
   must not contain any address in `S`). Getting it wrong leaks or blackholes.
2. **The all-or-nothing revert invariant.** `Apply()` already reverts a partial
   route set on any failure (`:165-173`). User-supplied ranges multiply the ways a
   single `CreateIpForwardEntry2` can fail (duplicate route from another VPN,
   overlapping on-link prefix). Keep that invariant; do not add a "best effort"
   path.
3. **Hostname rules are a lie told once.** Excluding `netflix.com` means excluding
   the addresses it resolved to *at that moment*. Cloudflare's WARP docs are
   refreshingly blunt about the same limitation: domain-based split rules are
   materialised into IP routes when the tunnel comes up and refreshed only on
   reconnect. If we offer hostname exclusions via routes, we must say "resolved at
   connect time" in the UI, or route them through mechanism (b) instead, where the
   SDK re-evaluates per flow.
4. **DNS does not follow the exclusion.** Our DNS is `settings.dns_servers` on the
   tun, and the mux does unencrypted→DoH in-tunnel
   (`TunnelController.cpp:333-342`). An excluded destination is still *resolved*
   through the tunnel. That is usually what you want (no DNS leak), but it means
   "excluded" ≠ "the VPN sees nothing about this destination". Say so.
5. **IPv6.** The tun is IPv4-only today (`:335`). Any exclusion UI must not imply
   IPv6 coverage.
6. **Excluded ≠ protected.** This is a security-relevant statement, not a caveat
   in small print. See §6.

### 3.3 Inverse split tunneling (only listed things go through the VPN)

**For destinations: trivially easier, not harder.** "Only route `10.0.0.0/8` and
`203.0.113.0/24` through the tunnel" is *just install those two routes* — it is
strictly less code than the exclude direction, because there is no complement to
compute. It is also the natural fit for our current design, since we already avoid
owning the default route.

**For apps: materially harder, and the risk is asymmetric.** Our `Ioctl.h` already
defines the semantics (`IOCTL_URST_SET_MODE`: denylist = redirect the set;
allowlist = redirect *everything except* the set), and mirrors Android's
"inclusions take precedence". The problems are operational, not conceptual:

- In denylist mode a driver bug affects the handful of apps in the list. In
  allowlist mode it affects **every process on the machine**, because every process
  is being redirected.
- The fail-open rule (README: "any driver error … results in pass-through, never a
  blocked flow") is a *safe* default in denylist mode and a **privacy failure** in
  allowlist mode: pass-through there means "the tunnel-only app tunnels", which is
  fine — but the inverse error, failing to redirect a non-listed app, silently puts
  traffic in the tunnel that the user asked to keep out. That is the benign
  direction. The dangerous one is a listed app failing to *stay* on the tunnel.
  Whichever way, the blast radius is the whole system.
- The service must be exempt in both modes or the VPN transport rebinds itself into
  its own tunnel. `Ioctl.h` says this is handled ("the controlling service … is
  ALWAYS exempt"); it needs a test that actually proves it, not a comment.

Recommendation: ship denylist (exclude apps) first even after the driver lands;
gate allowlist behind Advanced Mode and a second release.

---

## 4. What user mode *can* buy us, ranked

### 4.1 Destination exclusion via routes — **do this**
§3. Small, safe, already 80% built.

### 4.2 SDK host-rule local egress — **already works, expose it properly**
Mechanism (b) exists and the UI already reads it
(`SdkHost::PublishSplitRules`, `SdkHost.cpp:2127-2150`, and the split-rules sheet in
`StatsSheets.cpp`). *Unverified:* in the SDK revision on disk, the local branch is
reachable only when `provider != nil` (`device_local.go:1465-1467`), i.e. local
egress rode on the provide-mode NAT. If that is still true in the shipped SDK, host
rules with `RouteOverride.Local` **silently do nothing** for a user who is not
providing. This is the single most important thing to confirm with the SDK team
before we put a destination-splitting UI in front of users — it decides whether §4.1
is a nice-to-have or the *only* working destination mechanism.

### 4.3 App-based *blocking* via user-mode WFP — **cheap, real, and we have none**
This is the distinction the brief asked about, and it lands well.

`FwpmEngineOpen0` + `FwpmFilterAdd0` from user mode (we already link
`fwpuclnt.lib` in `SplitTunnelClient.cpp`), with
`FWPM_CONDITION_ALE_APP_ID` from `FwpmGetAppIdFromFileName0`, at
`FWPM_LAYER_ALE_AUTH_CONNECT_V4/V6`, action `FWP_ACTION_BLOCK`. `urnetworkd` runs
as SYSTEM, so it can open the engine. No driver, no signing, no reboot.

What that buys:

- **Per-app blocking** — "this app gets no network while the VPN is up". A real
  Portmaster-shaped feature, and it is the *only* app-granular thing we can honestly
  offer before the driver.
- **A real kill switch.** We have **no user-mode WFP filters at all today** (grep:
  the only `FWPM_LAYER` references in the tree are in `Driver.c`). Our kill switch
  is `!routeLocal` inside the SDK — it drops packets that reach the SDK. Anything
  that never reaches the SDK is not covered. A WFP block-all-except-`urnetworkd`-on-
  the-physical-NIC sublayer is the standard implementation, and it shares all of its
  plumbing with per-app blocking.
- A place to stand for later. The sublayer, provider GUID, filter bookkeeping and
  revert-on-crash logic are the same objects the driver will need to coexist with —
  Mullvad's driver exists partly to *punch permit holes through its own app's WFP
  filters*.

What it does **not** buy: rerouting. Blocking an app on the tun interface does not
make its traffic go out the physical NIC; it makes `connect()` fail. Do not ship
"block on tun" as if it were a bypass.

Estimated cost: **1–2 engineer-weeks** including revert-on-crash and tests. High
value per week, and it is not wasted work if the driver slips or dies.

### 4.4 The user-mode "app split" that almost works — **on the record, and rejected**

There is one way to do app-granular *routing* without a driver, and I want it
written down so nobody rediscovers it in six months and thinks it's clever.

The tun sees a packet. Its source is the tun address and an ephemeral port.
`GetExtendedTcpTable`/`GetExtendedUdpTable` (`TCP_TABLE_OWNER_PID_ALL`) map
(local address, local port) → PID → image path. So the pump *can* attribute a flow
to a process in user mode. Feed that into the SDK's local/remote decision and you
have app-based splitting with no kernel code.

Why it is not good enough:

- **It is not a bypass, it is a proxy.** The excluded app's packets still traverse
  wintun, the pump, and the SDK's NAT. Every excluded byte is copied through
  `urnetworkd`. Latency-sensitive apps — exactly the ones people exclude — get the
  worst of it.
- **Fate-sharing.** With bind-redirect, an excluded app keeps working if
  `urnetworkd` hangs. With user-mode NAT it dies with the service, and it dies
  *while the routes are still installed*, which is the worst failure shape we have.
- **No inbound, no listeners, no raw.** Anything an excluded app wants to receive
  unsolicited stops working. Peer-to-peer clients — a top-3 reason users want
  exclusions — break.
- **Attribution races and cost.** The table is a snapshot; the first packet of a
  flow can precede the entry, and UDP flows from short-lived sockets can be gone
  before you look. `GetTcpTable` is documented to fail under rapid table churn
  ([dotnet/runtime#29560][dotnet-issue] observes `STATUS_UNSUCCESSFUL` on a busy
  machine) and the sibling APIs behave the same way. You need a cache with
  invalidation, and you still get an unattributable tail.
- **`svchost` again.** DNS is `dnscache`'s traffic, not the app's — the identical
  limitation the driver has, so this buys nothing there.
- **It needs an SDK change anyway.** The SDK decides local vs remote from
  host/IP rules; there is no hook to say "this packet is local because of who sent
  it". Adding one is an upstream API change of real size.

So the cheap path costs an SDK API change and delivers a worse product with more
failure modes. **Reject it.** If we are going to spend that effort, spend it on the
driver.

*One variant deserves a footnote: shipping somebody else's already-signed driver
(WinDivert, WinpkFilter/WireSock). It removes our signing problem and replaces it
with a licensing problem (WinDivert is LGPL-3.0, WireSock is closed and
commercially licensed), a support problem (these drivers are widely flagged by AV
and are attractive blocklist candidates because malware uses them), and a
provenance problem — `PROVENANCE.md` exists specifically so our distribution stays
MPL-2.0-clean. Not recommended, but it is the only genuine shortcut and someone
will ask.*

---

## 5. Concretely, what "destination split tunneling" ships as

1. **Model.** A rule is `{cidr_or_host, direction}` where direction ∈
   {`bypass`, `tunnel-only`}. Persist next to the existing app rules; the SDK
   already has the shape for host rules (`BlockActionOverride.Hosts` +
   `RouteOverride`), so use it and do not invent a parallel store.
2. **Service.** Extend `NetworkConfig`'s prefix computation from the hard-coded
   RFC1918 complement to `complement(RFC1918 ∪ user_bypass_cidrs)`; in tunnel-only
   mode install exactly the listed prefixes instead of the complement. Keep the
   all-or-nothing revert.
3. **Hostnames.** Route them to the SDK's host-rule mechanism (b), *not* to the
   route table, so they are re-evaluated per flow — subject to the §4.2 caveat.
4. **UI.** The existing split-rules sheet already edits host rules with a
   Local/Remote chip (`StatsSheets.cpp:1756-1768`, `ConnectPage.cpp:1574-1582`).
   Add CIDR entry to it. The app picker in `SettingsPage` must be gated on
   `SplitTunnelClient::IsAvailable()` — see §6.
5. **Test.** A property test for the complement, and a live check that a `/32`
   exclusion actually egresses the physical NIC (`tracert` to the excluded host
   should not show the tun's first hop).

Estimated cost: **2–3 engineer-weeks** including UI and tests. No signing, no
reboot, no elevation beyond what step 6/8 already takes.

---

## 6. The `local` verdict — how we honour it, and the trap after the driver lands

### 6.1 Where it comes from

`BlockAction.Local` is set **by the SDK, inside the SDK's own packet path**, when a
flow matched a host/IP rule carrying `RouteOverride{Local:true}`. It is surfaced
verbatim: `SdkHost.cpp:2091` (`item.local = it->Local`), rendered at
`ConnectPage.cpp:1023-1031` and `StatsSheets.cpp:1052-1054`, and documented
honestly in the inspector's header comment at `ConnectPage.cpp:1134-1147`.

So the answer to "how do we know at logging time that a packet went around the
tunnel" is: **we don't infer it — the component that made the decision reports it.**
That is the right architecture and it should not change. Do not add packet-path
heuristics on the Windows side to reconstruct a verdict the SDK already knows.
Anything else (this inspector's `local`, `PacketStats.LocalEgress*`,
`ThroughputRoute::Local` on the chart) is the same source of truth.

### 6.2 The trap

**Route-table exclusions (§3.1a) and driver-excluded apps produce no `BlockAction`
at all.** Their packets never enter wintun, so the SDK never sees them, so they are
neither `blocked` nor `tunnelled` nor `local` — they are *absent*.

This is not a rendering bug, it is a semantic collision. Today the inspector's
three verdicts partition everything the user's machine does, because everything goes
through the tun. The moment we ship either route exclusions or the driver, the
activity list silently stops being a complete picture, and a user reading it will
conclude that an excluded app is not talking to the network.

That is the worst kind of wrong: a privacy tool that appears to say "nothing is
happening" when the truth is "something is happening where I can't see it".

**Recommendation.** Whichever of the two lands first, the same release must add a
persistent, non-dismissible line to the activity pane:

> *N bypass rules active — traffic matching them is not shown here and is not
> protected.*

with the count linking to the rules sheet. And the `local` chip's tooltip should
say what `local` actually means: *sent around the tunnel by a rule — allowed, and
not protected.* The chip is currently `kUrGreen` in `StatsSheets.cpp:1053` while
`ConnectPage.cpp:1023` uses `kUrAmber` for the same state and comments that amber is
the "not protected" colour. **Make it amber in both places.** Green for "your
traffic left unencrypted" is the wrong signal, and the inconsistency means one of
the two was written by someone who had thought about it and one wasn't.

### 6.3 So: honour it or hide it?

**Honour it — it is already honoured, for host rules.** The `local` verdict is not
speculative UI written against a feature that doesn't exist; it is a faithful
rendering of a decision the SDK makes today. Keep it.

What must *not* ship before the driver is the **app** side: any picker that lets a
user select an executable and mark it bypass. `SettingsPage` and
`ComputeAppSplit`/`PushExcludedToDriver` will accept those rules and persist them,
`SplitTunnelClient` will no-op, and the user gets a settings screen that lies. Gate
the app picker on `IsAvailable()`, and when it is false show *"App-based split
tunneling requires the URnetwork network driver, which isn't installed"* rather
than a disabled control with no explanation.

---

## 7. Driver signing, honestly — and a correction to `SIGNING.md`

### 7.1 What `SIGNING.md` currently assumes

> "`SplitTunnel.sys` is a kernel driver: Windows 10 1607+ only loads kernel drivers
> signed by Microsoft. We use **attestation signing** (no HLK tests, Windows 10/11
> client, our target) via the Partner Center Hardware Dev Center dashboard."

That was a correct reading of Microsoft's documentation when it was written. It no
longer matches the live documentation.

### 7.2 What Microsoft's docs say today

[Driver Signing Options][ms-offerings] (page `ms.date: 2026-03-23`, fetched
2026-08-08) titles the relevant section:

> **"Attestation signed drivers for testing scenarios"**
>
> "**For testing purposes only**, you can submit your drivers for attestation
> signing, which doesn't require HLK testing."

and lists, among the restrictions:

> "Attestation signed drivers can't be published to Windows Update for retail
> audiences… Publishing attestation signed drivers to Windows Update for testing
> purposes is supported by selecting **CoDev** or **Test Registry Key / Surface
> SSRK** options."
> "When a driver receives attestation signing, it's not Windows Certified."
> "Windows Server 2016 and greater doesn't accept attested device and filter driver
> signing submissions."

Separately, Microsoft has announced ([Advancing Windows driver security:
Removing trust for the cross-signed driver program][ms-blog], and the
consumer-facing [Windows Driver Policy][ms-policy] page) that from the **April 2026**
servicing update, on Windows 11 24H2 / 25H2 / 26H1 and Server 2025, **only drivers
signed through WHCP or on Microsoft's explicit allow list load by default**, with
trust removed for the legacy cross-signed root program.

### 7.3 My reading, with the uncertainty marked

- **Verified:** attestation signing still exists, still requires an EV certificate
  and Hardware Dev Center enrolment, still requires no HLK testing, and still works
  only on Windows 10 Desktop and later — never on Windows Server.
- **Verified:** the April 2026 change is written against **cross-signed** drivers.
  Attestation-signed drivers are signed by Microsoft's own attestation CA, and
  nothing in the primary sources says they stop loading. Today, an
  attestation-signed driver loads on a patched Windows 11 client.
- **Unverified / directional, and the reason to be nervous:** Microsoft renamed the
  attestation section to *"for testing scenarios"* and inserted *"For testing
  purposes only"* in a March 2026 revision, in the same window as the WHCP-only
  kernel-trust announcement. Secondary coverage of the April change is split on
  whether attestation survives as a production path; I found **no primary source
  either promising or withdrawing it**. Treat "attestation is the retail path for a
  VPN split-tunnel driver in 2027" as an assumption with a real chance of being
  false, not as a plan.

If attestation is withdrawn for retail, the fallback is WHCP: HLK Studio, a test
lab (multiple machines/architectures), full test passes per architecture per
release, and a submission process measured in months rather than days. That is a
different order of project.

### 7.4 The bill, itemised

| Item | Cost | Notes |
|---|---|---|
| EV code-signing certificate | ~$400–700/yr *(unverified, vendor list price)* | Hardware token or cloud HSM. Secondary sources report code-signing certificate lifetimes capped at **1 year from 2026-02-15** *(unverified — confirm with the CA)*, so this is recurring, not one-off |
| Partner Center / Hardware Dev Center enrolment | Days–weeks *(unverified)* | Needs a D-U-N-S number and the EV cert to sign the enrolment. Legal/finance dependency, not an engineering one — start it before the code is ready |
| Per-submission turnaround | Hours–days, per architecture | amd64 and arm64 are separate submissions. Every driver change is a new round trip |
| Driver hardening to ship quality | **8–14 engineer-weeks** | Driver Verifier (all flags), HVCI/KMCI compatibility, IPv6, the loopback connect-redirect fixup, process-tree races, stress + leak, teardown-under-load, crash-safety with routes installed. `README.md` already names most of these as plan R10 and correctly notes the burden is ours because the code is clean-room rather than battle-tested |
| Installer + lifecycle | 1–2 weeks | WiX driver feature, INF install/uninstall, upgrade across a signed-driver replacement, and a clean uninstall that cannot leave a redirecting driver behind |
| Support tail | Ongoing, unbounded | AV false positives, HVCI-enabled machines, third-party VPN/security-software conflicts, bugcheck reports. Budget real on-call for this |
| **Strategic risk** | — | Attestation-as-testing-only (§7.3). If it turns, add WHCP: lab hardware, HLK runs, months |

**Verdict: the driver is a next-year milestone, not a next-quarter one**, and the
signing question should be re-answered *before* any further driver engineering. The
right next action on the driver is not code — it is one person spending a week
getting a definitive answer from Microsoft (Hardware Dev Center support ticket or a
partner contact) to: *"can a third-party VPN split-tunnel WFP callout ship to retail
consumers with attestation signing in 2026-2027, or is WHCP required?"* Everything
downstream of that question changes with the answer, and it costs one week to
de-risk a three-month milestone.

---

## 8. Windows-native alternatives — researched, and why none of them rescue us

### 8.1 `Add-VpnConnectionRoute` / `Set-VpnConnection -SplitTunneling`
These configure the **built-in RAS VPN client** (IKEv2/L2TP/SSTP/PPTP) profiles.
They do not apply to a third-party wintun adapter, and they are destination-based
anyway — the same thing we can do ourselves with `CreateIpForwardEntry2` and
already do. **No value.**

### 8.2 VPNv2 CSP `TrafficFilterList` — real per-app VPN, wrong platform
This is the most interesting near-miss. [VPNv2 CSP][ms-vpnv2] `TrafficFilterList`
accepts `App/Id` as a **PackageFamilyName, a desktop `FilePath`, or `SYSTEM`**, with
a per-rule `RoutingPolicyType` of `SplitTunnel` or `ForceTunnel`. That is genuine,
OS-enforced, app-granular VPN routing with no driver — Windows has had this since
1511.

Why it does not help us:
- It configures the **Windows VPN platform's** profiles. Our data path is our own
  wintun adapter driven by `PacketPump`; the platform is not in it, and there is no
  API to attach traffic filters to a foreign adapter.
- It is an **MDM/Intune** surface (SyncML atomic blocks against
  `./Device/Vendor/MSFT/VPNv2/...`). It is not a consumer-app configuration path.
- Its default is brutal: *"Once a TrafficFilterList is added, all traffic is blocked
  other than the ones matching the rules."* Fine for a managed fleet, wrong for a
  consumer VPN.

The only route to it is §8.3.

### 8.3 The UWP VPN plugin model (`IVpnPlugIn` / `VpnPlugInProfile`)
Microsoft's pitch is exactly our problem statement — third-party VPN protocols as
app-containerised WinRT plug-ins, *"eliminating the complexity and problems often
associated with writing to system-level drivers"*. Plugin profiles do get
`TrafficFilterList`, so per-app routing would come free and platform-enforced.

Why I am not recommending it:
- It **replaces our entire data path**. `VpnChannel` hands you packet buffers; you
  give up wintun, `PacketPump`, and the current `TunnelController` bring-up
  sequence. This is not a feature, it is a second product.
- The plugin must be a packaged app with the **`networkingVpnProvider` restricted
  capability** — Store approval, and a hard dependency on Microsoft's review for
  every release.
- The plugin runs in an app container. Hosting the Go SDK (`URnetworkSdk.dll`) and
  its provider transports inside that sandbox is a substantial unknown, and our
  service architecture (a SYSTEM service owning routes and the driver) does not fit
  the model at all.
- The ecosystem is enterprise-client-shaped (Pulse, F5, FortiClient, GlobalProtect,
  Check Point) — none of it is consumer VPN, which tells you where the platform
  investment goes.
- It would not save the driver anyway for the case we care most about, because a
  plugin profile's traffic filters are still an MDM-configured surface.

Worth revisiting only if the driver path dies outright at §7.3.

### 8.4 `FWPM_CONDITION_ALE_APP_ID` on user-mode filters
Yes, and it matters — but for **blocking, not rerouting**. Covered in §4.3; this is
the one native mechanism I'd actually spend money on this quarter.

---

## 9. Phased proposal

**Phase 0 — this sprint, ~0 engineering.** Gate the app-rule UI on
`SplitTunnelClient::IsAvailable()` and explain the gate. Make the `local` chip amber
everywhere and fix its tooltip to say "allowed, not protected". Nothing else in the
product currently tells the truth about the driver being absent.

**Phase 1 — this quarter, ~3–5 engineer-weeks.**
- Destination split tunneling via the route table (§5): exclude-CIDR and
  tunnel-only-CIDR, built on the existing complement-prefix code.
- The "N bypass rules active — not shown here, not protected" banner (§6.2).
- Confirm the §4.2 SDK question (does host-rule local egress work without provide
  mode) before shipping any hostname-rule UI.
- Ship it as **"Split tunneling (by destination)"**. Not "split tunneling".

**Phase 2 — next quarter, ~2 weeks + 1 week of research.**
- User-mode WFP sublayer: per-app **blocking**, plus the WFP kill switch we don't
  currently have (§4.3). Portmaster-shaped, driver-free, and it builds the plumbing
  the driver will need to coexist with.
- In parallel and independently: **get the signing answer** (§7.4). One person, one
  week, before any more driver code.

**Phase 3 — next year, conditional on Phase 2's signing answer.**
- Driver hardening, attestation/WHCP submission, installer feature, staged rollout
  behind Advanced Mode. Denylist mode only in the first release; allowlist (§3.3) a
  release later. Budget 10–16 engineer-weeks plus a support tail, and do not start
  it until the signing route is confirmed in writing.

**What each phase unlocks, in user words:**
- Phase 1: *"Keep my NAS and my bank off the VPN."*
- Phase 2: *"Stop this app from talking to the internet at all"*, and a kill switch
  that actually holds.
- Phase 3: *"Keep Steam off the VPN."* — and only this needs the kernel.

---

## 10. Open questions

1. **(Blocking Phase 1)** Does the shipped SDK's local-egress branch require provide
   mode? `device_local.go:1465-1467` in the clone at `C:\Users\ryanm\Downloads\sdk`
   suggests yes at that revision. If yes today, `RouteOverride{Local:true}` on host
   rules is inert for most users and the route-table path in §5 is not an
   enhancement, it is the whole feature.
2. **(Blocking Phase 3)** Is attestation signing accepted for a retail third-party
   VPN callout driver in 2026-2027, or is WHCP now required? §7.3. No primary source
   answers this.
3. Does `BlockAction.Local` ever get set for *app* rules, or only host/IP rules? My
   reading of the architecture (§1.5) says host/IP only — the SDK cannot see a PID —
   but it should be confirmed rather than assumed, because §6.2's banner text depends
   on it.
4. Do we want route-table exclusions to also appear as rules in the SDK's
   `BlockActionOverride` list (for cross-device sync), even though the SDK will never
   act on them? Consistency versus a store that contains rules its owner ignores.

---

## Sources

- [mullvad/win-split-tunnel — README][mullvad-repo]
- [Split Tunneling — Windscribe/Desktop-App (DeepWiki)][ws-wiki]
- [FR: App-based split tunneling on the Windows client — tailscale/tailscale#15489][ts-issue]
- [TunnlTo/desktop-app][tunnlto]
- [Permitting and Blocking Applications and Users — Microsoft Learn][ms-permit]
- [Driver Signing Options — Microsoft Learn][ms-offerings]
- [Attestation Sign Windows Drivers — Microsoft Learn][ms-attest]
- [Advancing Windows driver security: Removing trust for the cross-signed driver program — Microsoft Tech Community][ms-blog]
- [The Windows Driver Policy — Microsoft Support][ms-policy]
- [VPNv2 CSP — Microsoft Learn][ms-vpnv2]
- [VpnPlugInProfile / IVpnPlugIn — Microsoft Learn][ms-vpnplugin]
- [Split Tunnels — Cloudflare One docs][cf-split]
- [IPGlobalProperties throws on high load — dotnet/runtime#29560][dotnet-issue]

[mullvad-repo]: https://github.com/mullvad/win-split-tunnel/blob/master/README.md
[ws-wiki]: https://deepwiki.com/Windscribe/Desktop-App/3.6-split-tunneling
[ts-issue]: https://github.com/tailscale/tailscale/issues/15489
[tunnlto]: https://github.com/TunnlTo/desktop-app
[ms-permit]: https://learn.microsoft.com/en-us/windows/win32/fwp/permitting-and-blocking-applications-and-users
[ms-offerings]: https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings
[ms-attest]: https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation
[ms-blog]: https://techcommunity.microsoft.com/blog/windows-itpro-blog/advancing-windows-driver-security-removing-trust-for-the-cross-signed-driver-pro/4504818
[ms-policy]: https://support.microsoft.com/en-us/windows/hardware/drivers/the-windows-driver-policy
[ms-vpnv2]: https://learn.microsoft.com/en-us/windows/client-management/mdm/vpnv2-csp
[ms-vpnplugin]: https://learn.microsoft.com/en-us/uwp/api/windows.networking.vpn.vpnpluginprofile
[cf-split]: https://developers.cloudflare.com/cloudflare-one/connections/connect-devices/warp/configure-warp/route-traffic/split-tunnels/
[dotnet-issue]: https://github.com/dotnet/runtime/issues/29560
