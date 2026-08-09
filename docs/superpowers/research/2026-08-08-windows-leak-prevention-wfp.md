# Leak prevention and the kill switch on Windows, via WFP

Research note, 2026-08-08. External research + read-only inspection of
`beta/custom-server`. **No source file was modified and no filter, route or DNS
setting was applied to the machine while producing this.** Nothing here has been
executed; every claim about our own behaviour comes from reading the code, and
every claim about Windows comes from Microsoft documentation or a reference
implementation's source, cited inline.

---

## 0. The short version

1. **`Device::setRouteLocal` is not a kill switch and cannot be made into one.**
   It is a userspace *fallback* switch inside `DeviceLocal.sendPacket`, and it
   only ever sees packets the OS already routed **into the tun**. Everything that
   is not routed into the tun — all IPv6, DNS from other adapters' resolvers,
   LLMNR/mDNS/NetBIOS, LAN traffic, split-tunnel-excluded apps, and *everything*
   in the window where the tun adapter does not exist (which is exactly the
   crash/disconnect case a kill switch exists for) — never reaches that branch.
   §2 has the proof from the SDK source.
2. **The kill switch has to be WFP, from user mode, and it needs no driver.**
   Mullvad and WireGuard both do the whole thing from user mode via
   `fwpuclnt.dll`; the split-tunnel callout driver is a separate, later concern
   (§9).
3. **DNS: WFP port-53 block + the per-interface resolvers we already set on the
   tun is the primary. NRPT is the fallback, not the primary** — because NRPT is
   machine-global registry state that *survives a force-kill and a reboot*, and
   a stale rule leaves the box with no DNS at all. And **do not use Windows DoH
   on either OS version** — the SDK's `UpgradeMux` already does the upgrade
   in-tunnel, which deletes the whole Win10-vs-Win11 branch from `PLAN.md`'s R6.
   §5.
4. **IPv6: WFP block at the two v6 ALE layers. Not `DisabledComponents`, not
   `Set-NetAdapterBinding`, and route blackholes only as belt-and-braces** — §6.
5. **The teardown guarantee is three independent mechanisms**, and the one that
   actually carries the load is the *provider `serviceName`* field, which
   Mullvad does not use and we should — §8.
6. Our tun routes deliberately exclude RFC1918 so LAN bypasses the tunnel. The
   WFP LAN permit **must match that route set exactly** or the kill switch and
   the route table will disagree. There is also a pre-existing quirk: our route
   set captures `169.254.0.0/16` and `224.0.0.0/3` into the tun, which Mullvad
   treats as LAN/multicast and exempts (§7.2).

---

## 1. What the tree does today (read-only findings)

| Area | Current state | File |
| --- | --- | --- |
| Tun address, MTU, metric | `169.254.2.1/24` (or SDK-supplied), MTU 1440, metric 1 | `app/src/Service/NetworkConfig.cpp:122-160` |
| IPv4 capture routes | 31 prefixes covering `0.0.0.0/0` **minus** `10/8`, `172.16/12`, `192.168/16` | `NetworkConfig.cpp:79-88` (`kIncludedV4Routes`) |
| IPv6 | **Nothing.** No v6 address, no v6 route, no v6 filter. | — |
| DNS | `SetInterfaceDnsSettings` on the tun interface only | `NetworkConfig.cpp:90-118` |
| DNS leak guard | **None.** The code says so: `"R6: also needs a leak guard against other adapters' resolvers"` | `NetworkConfig.cpp:177` |
| Kill switch | UI toggle → `LocalState::setRouteLocal` + `Device::setRouteLocal`. No OS enforcement anywhere. | `app/src/App/SdkHost.cpp:2328-2365` |
| Crash safety | `ArmCrashRevert`/`CrashRevert` (routes only), `SweepOrphanedTunnel`, `tunnel_active` marker, `urnetworkd revert` verb | `NetworkConfig.h:44-104`, `TunnelController.cpp:425-456`, `main.cpp` |
| Egress self-exclusion (R1) | SDK sockets bound to the physical ifindex; split-tunnel driver rebinds excluded apps | `TunnelController.cpp:187-222` |

So today, while "connected":

- All IPv6 leaves via the physical NIC, unencrypted, from the host's real GUA.
  **R7 is completely unmitigated.**
- Any resolver reachable on a non-tun adapter is still queried by `dnscache`,
  because Windows resolution is per-adapter. **R6 is unmitigated.**
- LAN traffic intentionally bypasses the tunnel (matching Android/iOS).
- On disconnect or crash, the adapter dies, the routes die with it, and
  **everything falls straight back to the physical NIC in the clear.** That is
  the correct *fail-open* design for a machine you do not want to brick — and it
  is precisely the behaviour a kill switch has to replace.

---

## 2. Why `setRouteLocal` cannot be the kill switch

From `urnetwork/sdk` `device_local.go` (fetched from upstream `main`), the send
path is:

```go
func (self *DeviceLocal) sendPacket(packet []byte) bool {
	route := self.sendRoute.Load()
	if route.upgradeMux != nil {           // in-tunnel: mux claims DNS/HTTP
		return route.upgradeMux.SendPacket(...)
	} else if route.remoteUserNatClient != nil {   // in-tunnel: provider
		return route.remoteUserNatClient.SendPacket(...)
	} else if route.routeLocal {           // <-- the flag
		localUserNat := route.provider.LocalUserNat()
		...
		return localUserNat.SendPacket(...) // out the PHYSICAL nic, in the clear
	} else {
		return false                        // dropped
	}
}
```

Three things follow, and all three are fatal to using it as a kill switch:

1. **It is downstream of the OS routing decision.** `sendPacket` is fed by
   `PacketPump` reading the wintun ring. A packet only arrives here if the
   kernel already chose the tun for it. IPv6 (no v6 route on the tun), LAN
   (deliberately excluded from `kIncludedV4Routes`), and anything an excluded
   app sends (rebound to the physical source address by the split-tunnel driver)
   never appear.
2. **It requires the tun to exist.** `routeLocal=false` drops packets *the tun
   received*. If `urnetworkd` dies, wintun tears the adapter down, the routes go
   with it, and the OS resumes using the physical default route. There is no
   code running to drop anything. This is the exact scenario the kill switch is
   for, and the flag is structurally incapable of covering it.
3. **It is not enforcement, it is a fallback preference.** `DefaultRouteLocal:
   true` in the SDK settings — the shipped default is *leak*, and the kill switch
   is the opt-out. `SdkHost::CurrentKillSwitch()` even returns `false` when no
   state exists, "claim the permissive default, not the strict one"
   (`SdkHost.cpp:2337`).

**Verdict: keep the toggle and keep persisting it, but re-point it.** The
persisted `!routeLocal` should become the *arming* input to a WFP policy owned by
`urnetworkd`, with `setRouteLocal` kept in sync as the in-datapath belt. The UI
contract does not change; what it drives does.

> **Unverified:** whether `DeviceRemote::setRouteLocal` round-trips over the RPC
> to the service today, or whether the service reads the persisted LocalState
> itself on start. Either works, but the service must be able to read the armed
> state **without an app session**, because "armed while the app is not running"
> is a required state (§4.3).

---

## 3. The WFP model, only the parts that decide our design

Sources: [Object Management](https://learn.microsoft.com/en-us/windows/win32/fwp/object-management),
[Filter Arbitration](https://learn.microsoft.com/en-us/windows/win32/fwp/filter-arbitration),
[FWPM_FILTER0](https://learn.microsoft.com/en-us/windows/win32/api/fwpmtypes/ns-fwpmtypes-fwpm_filter0),
[FWPM_PROVIDER0](https://learn.microsoft.com/en-us/windows/win32/api/fwpmtypes/ns-fwpmtypes-fwpm_provider0).

### 3.1 Arbitration — the four rules that matter

1. Within a sublayer, filters are evaluated **highest weight first**; the first
   permit-or-block wins and the rest are skipped.
2. Across sublayers in the same layer, **all** sublayers are evaluated in
   sublayer-weight order, and **block beats permit**.
3. **A filter's block is a *hard* block by default; a filter's permit is a
   *soft* permit.** (`FWPM_FILTER_FLAG_CLEAR_ACTION_RIGHT` inverts this per
   filter.) A hard block cannot be overridden by anyone's permit in any
   sublayer.
4. Therefore: **our block filters automatically beat Windows Firewall's
   permits and every other VPN's permits — we do not need to win a weight
   race for security.** Sublayer weight only decides whose *permit* survives.
   For a fail-closed kill switch that is the right way round: we can only ever
   be *more* restrictive than the rest of the system, never less.

Practical consequence, and the one that bites people: **a hard block of ours in
sublayer A will also beat a permit of ours in sublayer B.** Put the whole policy
in one sublayer (WireGuard), or split it deliberately with a lifting permit
(Mullvad — see §5.3). Do not split it by accident.

Filter weight as `FWP_UINT8` in 0..15 is a *weight class*: BFE assigns the real
64-bit weight inside that band. libwfp names them `Min = 0`, `Medium = 7`,
`Max = 15` ([`filterbuilder.h`](https://github.com/mullvad/libwfp/blob/master/src/libwfp/filterbuilder.h)).
Use classes, not raw `UINT64` — raw weights collide unpredictably with other
providers.

### 3.2 ALE reauthorization — why this works on *existing* connections

Adding or removing a filter at an ALE layer is a policy change, and WFP
re-submits already-established flows for classification on their next packet
(flagged `FWP_CONDITION_FLAG_IS_REAUTHORIZE`) —
[ALE Reauthorization](https://learn.microsoft.com/en-us/windows/win32/fwp/ale-re-authorization).
So a browser that had TCP connections open before we armed gets them killed on
the next packet, rather than continuing to leak. This is the single strongest
argument for enforcing at ALE rather than at the packet layers. It is also
something to **test explicitly** (§10, test 6): it is triggered by traffic, not
immediately, so an idle flow stays up until it moves.

### 3.3 Object lifetime — the four kinds, and which we use

| Lifetime | How | Dies when | Our use |
| --- | --- | --- | --- |
| **Dynamic** | `FWPM_SESSION_FLAG_DYNAMIC` on `FwpmEngineOpen0` | session handle closes **or the process dies, including a crash** | **The connected-state policy.** This is our crash safety. |
| Static | default | deleted, BFE stops, or shutdown | avoid — survives a crash but not a reboot; worst of both |
| **Persistent** | `FWPM_*_FLAG_PERSISTENT` | explicitly deleted | **Armed-across-reboot only**, and only behind the service-name gate (§8.3) |
| **Boot-time** | `FWPM_FILTER_FLAG_BOOTTIME` | when BFE finishes init | pairs with persistent to close the boot window |

`PERSISTENT` and `BOOTTIME` are **mutually exclusive on one filter** — you add
both sets, as Mullvad does. tcpip/netio applies the boot-time set when the stack
starts; BFE swaps in the persistent set when it initialises, and the handover is
atomic, so there is no window with neither in force.

A persistent object may only reference persistent objects owned by the same
provider, so the persistent path needs its **own provider and its own sublayer**,
separate from the dynamic-session ones.

### 3.4 Everything above is user mode

`FwpmEngineOpen0` / `FwpmFilterAdd0` / `FwpmTransaction*` live in
`fwpuclnt.dll`. The ALE layers are kernel-mode *layers* but they are managed
from user mode. Both reference implementations prove the whole design out of
user mode:

- WireGuard's entire firewall is Go calling `fwpm*` —
  [`tunnel/firewall/blocker.go`](https://github.com/WireGuard/wireguard-windows/blob/master/tunnel/firewall/blocker.go).
- Mullvad's boot-time + persistent block-all is added from the **user-mode
  daemon**, in Rust —
  [`talpid-core/src/firewall/windows/objects/persistent.rs`](https://github.com/mullvad/mullvadvpn-app/blob/main/talpid-core/src/firewall/windows/objects/persistent.rs).
  (A web summary claimed boot-time filters are kernel-only; that is
  contradicted by this shipping user-mode implementation. Treat the summary as
  wrong, but verify on our own box before relying on it — §10, test 8.)

We need `FWPM_ACTRL_ADD`/`FWPM_ACTRL_ADD_LINK` on the engine, which
`urnetworkd` running as LocalSystem already has. **No driver, no signing
dependency, no `app/SIGNING.md` blocker.** See §9 for what *is* gated on the
driver.

---

## 4. The concrete filter set

Three policy states. All filters are `FWP_ACTION_BLOCK`/`FWP_ACTION_PERMIT`
only — no callouts anywhere, so nothing here needs a driver.

Objects:

| Object | Key | Weight | Lifetime |
| --- | --- | --- | --- |
| Provider `URnetwork` | fixed GUID, `serviceName = L"urnetworkd"` | — | dynamic session |
| Provider `URnetwork persistent` | fixed GUID, `serviceName = L"urnetworkd"`, `FWPM_PROVIDER_FLAG_PERSISTENT` | — | persistent |
| Sublayer `URnetwork baseline` | fixed GUID | `MAXUINT16` | dynamic session |
| Sublayer `URnetwork DNS` | fixed GUID | `MAXUINT16 - 1` | dynamic session |
| Sublayer `URnetwork persistent` | fixed GUID | `MAXUINT16` | persistent |

`L` = layer, abbreviated: `C4`/`C6` = `FWPM_LAYER_ALE_AUTH_CONNECT_V{4,6}`,
`A4`/`A6` = `FWPM_LAYER_ALE_AUTH_RECV_ACCEPT_V{4,6}`.
Weight column is the libwfp class (0..15).

### 4.1 State CONNECTED — tunnel up, nothing escapes it

| # | Filter | Layers | Sublayer | Action | Wt | Why |
| --- | --- | --- | --- | --- | --- | --- |
| C1 | Permit `ALE_APP_ID == urnetworkd.exe` (optionally `+ ALE_USER_ID == LocalSystem`) | C4 C6 A4 A6 | baseline | permit | 15 | Our own transport must reach providers. This *is* R1 expressed in WFP. Mirrors WireGuard `permitWireGuardService`. |
| C2 | Permit `FWP_CONDITION_FLAG_IS_LOOPBACK` | C4 C6 A4 A6 | baseline | permit | 13 | Match on the **flag**, not on `127/8`/`::1` — local-to-local between two of the host's own addresses also carries it. |
| C3 | Permit `IP_LOCAL_INTERFACE == tunLuid` | C4 C6 A4 A6 | baseline | permit | 12 | Everything on the tun is by definition tunnelled. `FWP_UINT64`, `NET_LUID.Value`. |
| C4 | Permit LAN: remote ∈ {`10/8`,`172.16/12`,`192.168/16`,`169.254/16`} | C4 A4 | baseline | permit | 12 | **Must match `kIncludedV4Routes`' complement.** See §7.2 — today it does not. |
| C5 | Permit LAN v6: remote ∈ {`fe80::/10`, `fc00::/7`} | C6 A6 | baseline | permit | 12 | Only if v6 LAN is wanted; see §6. |
| C6 | Permit multicast/broadcast: remote ∈ {`255.255.255.255/32`, `224.0.0.0/24`, `239.0.0.0/8`} | C4 A4 | baseline | permit | 12 | Mullvad's `g_ipv4MulticastNets`. Gate on the same "allow LAN" setting. |
| C7 | Permit DHCPv4 out: UDP, local port 68, remote `255.255.255.255`, remote port 67 | C4 | baseline | permit | 12 | Without it the lease never renews and the machine loses its address mid-session. |
| C8 | Permit DHCPv4 in: UDP, local port 68, remote port 67 | A4 | baseline | permit | 12 | |
| C9 | Permit DHCPv6 out: UDP, local `fe80::/10`:546 → {`ff02::1:2`, `ff05::1:3`}:547 | C6 | baseline | permit | 12 | Both multicast groups; omitting `ff05::1:3` breaks relayed enterprise DHCPv6. |
| C10 | Permit DHCPv6 in: UDP, local `fe80::/10`:546, remote `fe80::/10`:547 | A6 | baseline | permit | 12 | |
| C11 | Permit NDP: ICMPv6 code 0, types 133→`ff02::2`, 134←`fe80::/10`, 135↔`fe80::/10`+solicited-node, 136↔`fe80::/10` | C6 A6 | baseline | permit | 12 | 7 filters. Copy Mullvad `permitndp.cpp` verbatim. Type 137 (redirect) optional — I would **drop** it; we are blocking v6 anyway. |
| C12 | **Permit remote port 53 (lifting rule)** | C4 C6 | baseline | permit | 7 | Lifts DNS out of the baseline block so the DNS sublayer decides it. Mullvad `baseline/permitdns.cpp`. **Without this, C4 (LAN permit) would let a query to the router's resolver escape.** |
| C13 | **Block all** (no conditions) | C4 C6 A4 A6 | baseline | block | 0 | The floor. Hard block, so nobody's permit overrides it. |
| D1 | Permit port 53 where `IP_LOCAL_INTERFACE == tunLuid` **and** remote ∈ tunnel resolvers | C4 | dns | permit | 7 | Only our resolver, only on the tun. |
| D2 | Permit port 53 where loopback flag set | C4 C6 | dns | permit | 7 | Local stub resolvers (`127.0.0.1:53`), dnscrypt-proxy, Docker, WSL. |
| D3 | **Block remote port 53, UDP+TCP** | C4 C6 | dns | block | 0 | Kills every other resolver on every other adapter. This is the R6 fix. |

Total: ~40 filters. Mullvad's connected policy is the same order of magnitude.

**Why two sublayers.** DNS has to be decided independently of "is this LAN".
C4 permits `192.168.1.0/24`, which includes the router's resolver. If DNS lived
in the same sublayer, C4 would win by weight and the query would leave. Lifting
port 53 to a *soft* permit in baseline (C12) and hard-blocking it in a second
sublayer (D3) gets both: LAN works, LAN DNS does not. This is exactly why
Mullvad has a `SublayerDns`, and the comment at the top of their `fwcontext.cpp`
says so.

### 4.2 State CONNECTING / RECONNECTING

Identical to CONNECTED **minus C3** (no tun yet) and minus D1. C1 is what lets
us reconnect. This is the state the UI already has a string for
(`"Reconnecting while the kill switch blocks traffic"`,
`docs/superpowers/specs/2026-08-08-windows-redesign-spec.md:304`).

### 4.3 State ARMED — kill switch on, tunnel down

Same as CONNECTING. There is deliberately **no separate ruleset**: the whole
point is that "disconnected but armed" and "trying to connect" allow exactly the
same thing, so there is no transition window where the policy is briefly weaker.

Applied when: the user disconnects with the kill switch on; the tunnel drops;
the service starts and reads armed-state from persisted LocalState; the app
exits while armed.

### 4.4 State ARMED-ACROSS-REBOOT (optional, opt-in, "advanced kill switch")

Persistent + boot-time, own provider and sublayer, **no permits at all** —
4 boot-time block filters and 4 persistent block filters at `C4 C6 A4 A6`,
weight `Max`, exactly Mullvad's `persistent.rs`.

| # | Filter | Layers | Sublayer | Action | Wt | Lifetime |
| --- | --- | --- | --- | --- | --- | --- |
| P1-4 | Block all | C4 C6 A4 A6 | persistent | block | 15 | `FWPM_FILTER_FLAG_BOOTTIME` |
| P5-8 | Block all | C4 C6 A4 A6 | persistent | block | 15 | `FWPM_FILTER_FLAG_PERSISTENT` |

Note there is **no loopback permit** in Mullvad's persistent set either, so
local-only software is also blocked between boot and the daemon starting. That
is a deliberate "seconds of maximum lockdown" trade. I would **add a loopback
permit at weight 15** to our persistent set: the exposure window is tiny and
blocking loopback at boot breaks local databases, dev servers and IPC in ways
that look like our bug.

The moment `urnetworkd` starts and installs the dynamic policy, it deletes the
whole persistent set inside the same transaction (§8.2).

**This state should be off by default and behind an explicit, separately-worded
toggle.** It is the mode that can leave a machine with no network and no obvious
cause, and it is what Proton calls "Advanced kill switch"
([docs](https://protonvpn.com/support/advanced-kill-switch)).

---

## 5. DNS (R6) — recommendation

**Primary: WFP hard-block of remote port 53 (UDP + TCP) with a narrow permit for
the tunnel resolver, plus the per-interface resolver we already set on the tun.
Fallback: an NRPT catch-all `"."` rule — and only with a mandatory startup
purge.**

The framing that makes this decision easy: **on Windows, no registry setting is a
security boundary.** SMHNR policy, NRPT and per-adapter resolvers are *steering*
— they change where the DNS client prefers to send a query. Only a WFP filter is
*enforcement*, and only a WFP filter disappears automatically when our process
dies. So WFP is the floor, and the steering mechanisms exist to stop the floor
from turning a leak into an outage.

### 5.1 Why the OS-level DNS problem is smaller for us than for other VPNs

Two properties of our architecture collapse most of the version matrix:

1. **We never use OS-level encrypted DNS, on any Windows version.** The tun's
   resolvers are always plain `:53` pointed at an address the SDK's `UpgradeMux`
   claims, and the mux performs the plaintext→DoH upgrade *in-tunnel*
   (`TunnelController.cpp:333-342`). The plaintext hop exists only between
   `dnscache` and the tun, inside the machine and then inside the tunnel.
2. Therefore **Windows 10's lack of per-adapter DoH is a non-issue for us**, and
   `PLAN.md:232-236`'s worry ("Win10 has no per-adapter DoH, so DoH
   `tunnelDnsSetting` modes fall back to plain DNS to the in-tunnel resolver
   there") resolves to: *that is the design on both, and it is fine.* **Do not
   call `Add-DnsClientDohServerAddress` and do not touch the DoH GPO.** It buys
   nothing, only works on 11, and is another persistent registry mutation to
   clean up.

This is the same shape as Mullvad's local resolver and Tailscale's
`100.100.100.100` — one invariant ("the only legal DNS destination is the tunnel
resolver") instead of six version-dependent behaviours. We get it for free
because the mux already is that resolver.

### 5.2 The three candidates, judged

| | NRPT `"."` catch-all | **WFP port-53 block (recommended)** | Force per-adapter resolvers everywhere |
| --- | --- | --- | --- |
| Kind | steering | **enforcement** | steering |
| Beats SMHNR? | **Yes** — the DNS client consults NRPT before choosing an interface | Yes (the query is simply dropped) | Only if every adapter has the same servers |
| Catches apps with their own resolver (Go's pure-Go resolver, Electron, malware)? | **No** — never touches `dnscache` | **Yes** | No |
| Survives our process being force-killed? | **NO — machine-global registry, persists across reboot** | **Yes — dynamic session, BFE deletes on process death** | **No, and worst of the three**: `NameServer` converts the adapter from DHCP-DNS to static, so a crash strands the user's laptop pointing at a dead tunnel resolver on their home Wi-Fi |
| New adapter arrives mid-session | covered (rule is global) | **covered** (we match on remote port, not on an enumerated interface list) | **race** — dock/hotspot/Hyper-V vSwitch comes up with DHCP resolvers and is usable before we notice |
| Failure mode | queries go to the old resolver | **fails closed** | open (race), then stuck-closed after a crash |
| Multi-instance conflict | **two `"."` rules from two VPNs = *neither applies*** (Microsoft: conflicting rules mean no rule is applied) — a silent total bypass | block-beats-permit, so the other VPN's block still wins; fails closed | — |

**Force-per-adapter-resolvers is rejected outright.** Beyond the table: it fights
`DhcpNameServer` vs `NameServer` precedence, it collides with the Win10
`ProfileNameServer` per-WLAN-profile key, it breaks corporate split-DNS, AD join
and captive portals, and "what was the previous value" becomes data we must
durably persist *before* mutating. The one legitimate use is what we already do —
**set resolvers on the tun adapter only**, which is safe because the config dies
with the adapter.

### 5.3 The recommended filter shape (already filters D1-D3 in §4.1)

Enumerate what to **allow**, never what to block:

- **Deny broadly**: `IP_REMOTE_PORT == 53` × `IP_PROTOCOL ∈ {UDP(17), TCP(6)}`,
  no interface condition, weight `Min`, in the DNS sublayer.
- **Permit narrowly** at a higher weight: same port/protocol +
  `IP_REMOTE_ADDRESS == <tunnel resolver>` + `IP_LOCAL_INTERFACE == tunLuid`.
- **Permit loopback DNS** (`FWP_CONDITION_FLAG_IS_LOOPBACK` + port 53) so a local
  stub resolver, dnscrypt-proxy, Docker's embedded DNS and WSL keep working.
- **Lift port 53 to a soft permit in the baseline sublayer** (filter C12) so the
  LAN permit does not let a query to the router's resolver escape.

Do **not** scope the deny as `IP_LOCAL_INTERFACE != tunLuid`. That requires
negation/enumeration and reintroduces the adapter-arrival race the whole approach
exists to avoid. WireGuard's code encodes the invariant as an assertion — *"The
allow weight must be greater than the deny weight"*
([`rules.go` `blockDNS`](https://github.com/WireGuard/wireguard-windows/blob/master/tunnel/firewall/rules.go)).

### 5.4 If we ever add NRPT anyway

We probably will not need it: WireGuard for Windows ships with **no NRPT at
all** — per-interface resolvers + WFP is the whole design, and it is the design
closest to ours. But if a customer hits split-horizon breakage, the rule is:

- Write to `HKLM\SYSTEM\CurrentControlSet\Services\Dnscache\Parameters\DnsPolicyConfig`
  (local), **unless** non-ours rules already exist in
  `HKLM\SOFTWARE\Policies\Microsoft\Windows NT\DNSClient\DnsPolicyConfig`, in
  which case write there too and call `NotifyMachinePolicyChange()`. This is
  Tailscale's rule, and the reason is that a domain GPO populating NRPT will
  otherwise beat us.
- **Purge every rule we own on service start, unconditionally**, before anything
  else — same slot as `SweepOrphanedTunnel` in `main.cpp:208`. Persist our rule
  GUIDs so we know which are ours. Tailscale does exactly this and says why:
  *"NRPT rules survive the unclean termination of the Tailscale process."*
  Tailscale issue #18828 is a real user whose Windows 11 box had **no DNS at
  all** until they hand-deleted registry keys, because a stale rule pointed at a
  dead `10.2.0.1`.
- Purge in the MSI uninstall custom action too. Different code path, routinely
  forgotten.
- Max ~50 domains per rule; suffix namespaces need a leading dot; validate by
  reading back with `Get-DnsClientNrptPolicy` and treat a read failure as "my
  policy is broken, fail closed" (OpenVPN #747: one malformed exclusion rule made
  Windows silently ignore the *entire* NRPT policy and fall back to the Wi-Fi
  resolvers).

### 5.5 SMHNR — set it, never rely on it

```
HKLM\SOFTWARE\Policies\Microsoft\Windows NT\DNSClient
    DisableSmartNameResolution  REG_DWORD 1      ; note: SOFTWARE\Policies, NOT Dnscache
HKLM\SYSTEM\CurrentControlSet\Services\Dnscache\Parameters
    DisableParallelAandAAAA     REG_DWORD 1      ; different feature: A/AAAA parallelism
```

Smart multi-homed name resolution (Windows 8+) sends a query out *every* active
adapter's resolvers and takes the first usable answer — originally across DNS,
LLMNR and NetBIOS at once. Its behaviour changed repeatedly across Windows 10
builds (parallel → sequential around 1703/1709; the policy reportedly ignored in
some 1803+ builds), and **the version matrix is not reliably documented — I could
not obtain a primary source and I am marking the per-build detail unverified.**
There is no Microsoft statement that the disable was ever removed, and the ADMX
policy still ships on Windows 11.

The conclusion is the same either way: it is two registry writes, so do it, but
**it is not the mitigation.** The SANS whitepaper on this reaches the same place
— NRPT worked irrespective of whether SMHNR was on or off. Note these are
persistent HKLM writes, so they join the startup-purge/uninstall cleanup list.

### 5.6 The other name-resolution channels

Port 53 is not the only way a name becomes an address. **Handle all of these with
WFP port filters, not registry values** — the registry forms are per-interface
and inherit the adapter-arrival race.

| Channel | Ports | Add to the DNS sublayer block | Registry (bonus only) |
| --- | --- | --- | --- |
| **LLMNR** | UDP/TCP **5355** (`224.0.0.252`, `ff02::1:3`) | yes | `...DNSClient\EnableMulticast = 0` |
| **mDNS** | UDP **5353** (`224.0.0.251`, `ff02::fb`) | yes | `Dnscache\Parameters\EnableMDNS = 0` — **breaks Miracast, Chromecast, AirPrint-style discovery. Prefer the WFP filter; if we ever set the registry value it must be a user-visible toggle.** |
| **NetBIOS-NS/DGM/SSN** | UDP **137**, **138**, TCP **139** | yes | per-interface `NetBT\Parameters\Interfaces\Tcpip_{GUID}\NetbiosOptions = 2` |

Worth noting Mullvad's published security doc does not mention LLMNR/mDNS/NetBIOS
at all. That is a gap not to replicate — LLMNR in particular fires on *any* DNS
failure, which is exactly the state a kill switch creates.

### 5.7 Browser-internal DoH — a limitation to document, not to fix

Chrome and Firefox resolve names inside the browser process over 443 to a
resolver of their choosing. NRPT, SMHNR settings, adapter resolvers and a
port-53 block are all irrelevant to them.

- Firefox's canary domain (`use-application-dns.net` → return NXDOMAIN to
  disable DoH) **does not apply to users who enabled DoH manually** — i.e.
  exactly our user base. Nearly free to honour if we ever run a stub resolver;
  do not count on it.
- Deterministic control means policy: Chrome/Edge `DnsOverHttpsMode`, Firefox
  `network.trr.mode`. Prefer `secure` + `DnsOverHttpsTemplates` pointed at our
  resolver over `off` — it keeps the user's encryption while removing the leak.
- **Blocking 443 to known public DoH endpoint IPs is an unwinnable blocklist
  game.** Do not.

**Position: this is unenforceable by any VPN client. Say so in the docs, expose a
toggle, default to signalling rather than overriding a deliberate user choice.**
It is also a smaller hole than it looks: the browser's DoH still egresses through
the tunnel while connected (our block-all permits nothing else), so the leak is
"which resolver sees the name", not "the query left in the clear".

### 5.8 Implementation gotchas that will cost a day each

- **`FWPM_CONDITION_IP_REMOTE_PORT` is `FWP_UINT16` in HOST byte order.** Passing
  `htons(53)` yields a filter matching port 13568 and a leak that tests clean on
  a big-endian-blind reading of the code.
- **Use `FWPM_CONDITION_IP_LOCAL_INTERFACE` (`FWP_UINT64`, `NET_LUID.Value`), not
  `FWPM_CONDITION_INTERFACE_INDEX` (`FWP_UINT32`).** Interface *indices are
  recycled*. If our tun is torn down and another adapter later takes that index,
  an index-based permit now permits DNS out someone else's NIC. Silent leak.
  `NetworkConfig` already works in LUIDs — keep it that way through the WFP
  layer and do not let `EgressInterfaces`' ifIndex fields leak into filter
  conditions.
- **`FWPM_CONDITION_ALE_APP_ID` cannot single out the DNS client.** `dnscache`
  runs inside `svchost.exe`; since 1703's svchost refactoring it gets its own
  *process* on machines with >3.5 GB RAM, but the image path is still
  `svchost.exe`. "Permit 53 only from svchost" permits dozens of unrelated
  services; "deny 53 except svchost" is precisely the bypass an app with its own
  resolver uses. **Scope DNS on port + protocol + remote address + local
  interface. Ignore app identity for DNS entirely.** (App-id is still the right
  tool for filter C1, our own service.)
- **Consecutive conditions with the same `fieldKey` are OR'd; different fieldKeys
  are AND'd.** That is how WireGuard expresses "UDP or TCP" in one filter and how
  Mullvad expresses a list of LAN networks. Documented for Windows 7/2008 R2 in
  `FWPM_FILTER0` and relied on by both implementations on modern Windows.
- **Every policy change goes in one `FwpmTransactionBegin`/`Commit`.** Adding a
  permit and a deny non-atomically means a window where either everything is
  blocked or everything leaks.

---

## 6. IPv6 (R7) — recommendation

**Primary: WFP block at `ALE_AUTH_CONNECT_V6` + `ALE_AUTH_RECV_ACCEPT_V6`
(filters C13/D3 above), with the NDP/DHCPv6/loopback permits above them. Nothing
else, by default.**

This is what Mullvad
([`baseline/blockall.cpp`](https://github.com/mullvad/mullvadvpn-app/blob/main/windows/winfw/src/winfw/rules/baseline/blockall.cpp))
and WireGuard
([`rules.go` `blockAll`](https://github.com/WireGuard/wireguard-windows/blob/master/tunnel/firewall/rules.go))
independently converged on: four unconditional ALE block filters, v4 and v6,
nothing at the packet layers.

Why not the alternatives:

| Approach | Verdict | Reason |
| --- | --- | --- |
| **WFP v6 ALE block** | **ship this** | Catches TCP connect, first UDP packet per tuple, raw sockets. Triggers ALE reauth on existing flows. Fails *fast*, so Happy Eyeballs falls back to IPv4 in ~200ms instead of timing out. Removed automatically with the dynamic session. |
| WFP `OUTBOUND_IPPACKET_V6` | skip for v1 | Catches stack-generated NDP/MLD/ICMPv6-errors too — which means you must re-add every exemption there or you break the link. ALE deliberately never classifies ICMPv6 *errors*, so an ALE-only block leaves Path MTU Discovery (type 2, Packet Too Big) working for free. That is a real argument, not a shortcut. |
| WFP `IPFORWARD_V6` | add **if** we ever support ICS / Mobile Hotspot / Hyper-V external switch | A routed guest VM sails past ALE entirely. Not a v1 concern; note it. |
| Route blackhole `::/1` + `8000::/1` | belt-and-braces only, `store=active` | Longest-prefix beats the default route, so it stops internet-bound v6. It does **not** stop link-local, the on-link `/64` from the RA's PIO (i.e. every neighbour can still reach your real GUA), or multicast. Cheap and clears on reboot; never the control. |
| `DisabledComponents = 0x20` (prefer IPv4) | **do not ship** | Changes a *preference*, not reachability — an IPv6 literal still leaks. Needs a reboot. Persistent machine-wide registry change a crash leaves behind. Microsoft warns values other than 0 or 32 break RRAS. |
| `Set-NetAdapterBinding -ComponentID ms_tcpip6 -Enabled $false` | **do not ship** | Microsoft calls unbinding *"an unsupported Windows configuration"*. Per-adapter, so a dock/hotspot/vSwitch arriving mid-session leaks anyway. Persistent, survives our process dying, needs a reboot to undo cleanly. Known to break LDAP-over-UDP on DCs, Exchange, Failover Clustering, DirectAccess. |

**What breaks when v6 is blocked.** Link-local service discovery (mDNS `ff02::fb`,
LLMNR `ff02::1:3`, WSD/SSDP `ff02::c`) — printers and cast devices stop being
found. DirectAccess stops entirely. Hyper-V/WSL2 v6 stops. And the one that
matters:

**IPv6-only networks.** On an IPv6-only access network with NAT64/DNS64, blocking
v6 does not degrade the user, it disconnects them — *including from our own
providers*, so the client cannot recover. Windows 11 ships a CLAT (464XLAT) for
**cellular interfaces**; extending it to Wi-Fi/Ethernet was announced as a
preview and I **could not confirm GA status as of 2026-08**. Blocking v6 kills
the CLAT and with it every IPv4 socket it was synthesising.

**Therefore: detect and refuse, don't blindly block.** Before arming, check for a
usable IPv4 default route + global IPv4 address (we already compute
`EgressInterfaces.index4`, and `TunnelController` already logs when it is 0 —
`TunnelController.cpp:211-222`). If there is no v4 path, surface *"this network
is IPv6-only; the VPN can't protect you here"* rather than silently bricking it.

Phone hotspots are gentler than the folklore: iOS Personal Hotspot always hands
tethered clients `172.20.10.0/28` + NAT, and Android tethering on an IPv6-only
carrier generally runs the CLAT on the phone. So the §10 hotspot test will
usually have working IPv4. *(The Android detail is unverified.)*

**The real fix is to stop being IPv4-only** — give the tun a v6 address and route
`::/0` into it. Both reference implementations treat blocking as the fallback for
when the server can't do v6, behind a user toggle. Same trajectory for us.

---

## 7. Exemptions, ranked by how badly their absence hurts

### 7.1 The ranking

Ranked by blast radius if omitted. 1-3 are non-negotiable; 4-6 are "the support
queue fills up"; 7-9 are judgement calls.

| Rank | Exemption | If missing | Filters |
| --- | --- | --- | --- |
| **1** | **Our own service (`ALE_APP_ID`)** | **Unrecoverable.** Armed, blocked, and unable to reconnect — the machine has no network and no way back except an elevated `urnetworkd revert`. This is the one that turns a bug into a support incident. | C1 |
| **2** | **Loopback (via `FWP_CONDITION_FLAG_IS_LOOPBACK`)** | Breaks local databases, dev servers, RPC-over-loopback, and **our own mTLS control channel** (`ControlServer` listens on localhost). Matching `127.0.0.0/8` instead of the flag misses local-to-local between two real host addresses. | C2, D2 |
| **3** | **DHCPv4 + DHCPv6** | Works for hours, then the lease expires and the machine loses its address. Failure is delayed, which makes it hard to attribute. | C7-C10 |
| **4** | **NDP (ICMPv6 133-136)** | The v6 link never comes up cleanly; DAD fails; on some drivers this stalls interface init and slows the *whole* stack, v4 included. Cheap to include even when blocking all v6. | C11 |
| **5** | **DNS lifting rule** | Not a breakage — a *silent leak*. LAN permit lets queries to the router's resolver out. This is the subtle one. | C12 |
| **6** | **The tun interface itself** | Nothing works while connected. Loud and obvious, so it fixes itself in five minutes of testing. | C3 |
| **7** | **LAN / private ranges** | Printers, NAS, casting, RDP to another box on the LAN all die. **Should be user-configurable**: Mullvad ships it as "Local network sharing", default *off*; WireGuard has no LAN permit at all and gets complaints for it. Our route table already excludes RFC1918 from the tunnel, so for us the default should be **on** — anything else makes WFP and the route table disagree. | C4, C5, C6 |
| **8** | **Captive-portal detection (NCSI)** | Windows shows "No internet" and may pop the captive-portal flyout while armed. **Do not exempt it.** Permitting `msftconnecttest.com` / `dns.msftncsi.com` while armed is a real (if small) leak of "this machine is online, here is its IP" to Microsoft, and it teaches users the network works when it doesn't. Mullvad blocks it; the correct fix is our own UI saying *"kill switch active"* — which the spec already calls for (`redesign-spec.md:505`). |
| **9** | **ICMPv6 redirect (type 137)** | Nothing. Mullvad and WireGuard both permit it; RFC 4890 calls it a policy decision. Since we are blocking v6 anyway there is nothing to redirect. **Drop it** and reduce surface. |

### 7.2 The LAN exemption must match `kIncludedV4Routes` — today it would not

`NetworkConfig.cpp:79-88` captures `0.0.0.0/0` minus `10/8`, `172.16/12`,
`192.168/16`. Two mismatches against the Mullvad-style LAN/multicast exemption:

- **`169.254.0.0/16`** falls inside `{0xA8000000, /6}` (168.0.0.0/6 = 168–171),
  so APIPA/link-local **is routed into the tun**. Mullvad lists it in
  `g_ipv4LanNets`
  ([generated `lannetworks.h`](https://github.com/mullvad/mullvadvpn-app/blob/main/talpid-types/src/bin/snapshots/generate_cpp_lannetworks__tests__cpp_definition.snap)).
  Note our own tun address is `169.254.2.1/24`, so this is entangled — decide
  deliberately rather than inheriting it.
- **`224.0.0.0/3`** (`{0xE0000000, /3}` = 224.0.0.0–255.255.255.255) captures all
  multicast **and the `255.255.255.255` broadcast address** into the tun.
  Mullvad exempts `224.0.0.0/24`, `239.0.0.0/8` and `255.255.255.255/32` as
  LAN multicast.

Neither is a *leak*; both are a **consistency bug waiting to happen** the moment
a WFP permit set is written from a different list than the route set. Derive both
from one table in one place.

---

## 8. The teardown guarantee

> A persistent block filter left behind by a crashed VPN is the worst bug in this
> category. Everything below exists to make that impossible.

Four independent mechanisms, in order of how much load they carry.

### 8.1 Mechanism 1 (primary): the dynamic session

Open the engine with `FWPM_SESSION_FLAG_DYNAMIC`. Every object added in that
session — provider, sublayers, all ~40 filters — is destroyed by BFE when the
session handle closes **or the process dies for any reason**, including
`TerminateProcess`, a Go panic in the embedded SDK runtime, or a bugcheck.

This is structurally the same guarantee `NetworkConfig.h:44-62` already relies on
for routes ("the wintun adapter it created goes away with it… the machine heals
without us running a line of code"), and it is the reason WireGuard's
`DisableFirewall()` is literally just `fwpmEngineClose0`.

**For the CONNECTED / CONNECTING / ARMED states, use dynamic and only dynamic.**
The correct behaviour on a crash of `urnetworkd` is *fail open*: the user gets
their network back and the app tells them the kill switch is no longer in force.
A kill switch that survives its own enforcement process is not more secure, it is
just harder to undo.

### 8.2 Mechanism 2: startup purge, before anything else

`main.cpp:208` already sweeps orphaned tunnel interfaces before anything can
fail. Add a WFP purge to the same place, mirroring Mullvad's
[`objectpurger.cpp`](https://github.com/mullvad/mullvadvpn-app/blob/main/windows/winfw/src/winfw/objectpurger.cpp):

1. Open a **standard** (non-dynamic) session.
2. Begin a transaction.
3. Enumerate filters; collect every key whose `providerKey` is *either* of our
   two provider GUIDs. Enumerate sublayers, same.
4. Delete filters first, then sublayers, then both providers. Treat
   `FWP_E_*_NOT_FOUND` as success.
5. Commit.

Ordering matters: a sublayer holding filters cannot be deleted, and objects
cannot be deleted while the enumeration that produced them is in progress —
collect keys, drop the enumerator, then delete.

This must run in `urnetworkd revert` too (`main.cpp:329` `RevertNetwork`), which
already refuses while a service is running and requires elevation. Extend its
log line to report WFP objects found alongside orphaned interfaces.

**Same `remove=false` discipline as `SweepOrphanedTunnel`.** rpc-only mode must
*observe and report* leftover WFP objects, not delete them — a mode whose entire
promise is "this will not touch your network" must not open by rewriting the
filter engine.

### 8.3 Mechanism 3 (the one that actually saves us): provider `serviceName`

This is the finding I would build the persistent design around, and **Mullvad
does not use it** — their `persistent.rs` sets `.name().description().guid()
.persistent()` and no service name.

From [FWPM_PROVIDER0](https://learn.microsoft.com/en-us/windows/win32/api/fwpmtypes/ns-fwpmtypes-fwpm_provider0)
and [Object Management](https://learn.microsoft.com/en-us/windows/win32/fwp/object-management):

> At service start, BFE only adds persistent objects to the system if they are
> not associated with a provider, or the associated provider has no Windows
> service name, or the associated Windows service is set to auto-start.

and `FWPM_PROVIDER_FLAG_DISABLED` / `FWPM_FILTER_FLAG_DISABLED` are set *by BFE*
when the named service is absent or not auto-start.

So: **set `serviceName = L"urnetworkd"` on the persistent provider.** Then:

- Service disabled by the user → persistent filters are not enabled at boot.
- Service uninstalled (MSI removal, `urnetworkd uninstall`) → same.
- Someone's registry cleaner eats the service → same.

The machine cannot end up permanently blocked by a service that no longer
exists. This turns "the worst bug in this category" into a self-healing case,
and it costs one field.

> **Unverified:** whether the *boot-time* set honours the same service-name gate.
> Boot-time filters are applied by netio/tcpip from
> `HKLM\SYSTEM\CurrentControlSet\Services\BFE\Parameters\Policy\BootTime` before
> BFE (and therefore before the SCM state is consulted), so it may not. Test 8
> in §10 checks this. If it does not, the boot-time window is bounded by BFE
> init — seconds — and the persistent set behind it *is* gated, so the failure
> mode is "a few seconds of no network at boot", not "permanently bricked".

### 8.4 Mechanism 4: the active marker, extended

`TunnelController::SetActiveMarker` writes `tunnel_active` containing the pid
*before* installing routes, and removes it after reverting. It does not restore
anything — it exists so the next start can *say* the last one ended badly.

Extend it, don't replace it: write a small JSON blob instead of a bare pid —
`{pid, wfp_policy: "connected"|"armed"|"persistent", armed: bool}`. Then on
start:

- marker present + `persistent` → say loudly that a persistent lockdown was
  installed and is now being purged.
- marker present + armed → re-arm rather than coming up open.
- marker absent + our WFP objects found → a crash between engine-open and marker
  write. Purge and report.

`ArmCrashRevert`/`CrashRevert` should **not** grow a WFP path.
`CrashRevert` runs from the unhandled-exception filter and the console control
handler under a "no allocation, no lock, no blocking" contract
(`NetworkConfig.h:80-87`), and `FwpmEngineClose0` is an RPC to BFE — exactly the
class of call `ClearTunnelDns` is already excluded for
(`NetworkConfig.h:69-73`). The dynamic session makes it unnecessary: the crash
path does not need to do anything, because BFE does it when the process dies.
**Write that reasoning into the header comment next to the existing note** so the
next person does not "fix" the omission.

### 8.5 Failure-mode table

| Failure | Connected/armed (dynamic) | Persistent lockdown |
| --- | --- | --- |
| Orderly stop | filters removed in `StopLocked` | swap to persistent, or purge if disarming |
| `std::terminate` / SEH | **BFE removes on process death** | persistent set stays (intended) |
| Go panic in the SDK | **BFE removes on process death** | stays |
| `TerminateProcess` / task kill | **BFE removes on process death** | stays |
| Bugcheck / power loss | **removed — static+dynamic objects die when BFE stops** | boot-time set applies at next boot, then persistent |
| Service disabled | n/a | **not enabled at boot** (serviceName gate) |
| MSI uninstall | n/a | **not enabled at boot**, plus explicit purge in the uninstall custom action |
| BFE service stopped by an admin | filters gone, network open | persistent set gone until BFE restarts |

The last row is worth stating in the UI honestly: **WFP cannot protect against
an administrator who stops the Base Filtering Engine.** No Windows VPN can.

---

## 9. User mode vs kernel — what is and is not gated on the driver

| Capability | User mode via `fwpuclnt.dll`? | Notes |
| --- | --- | --- |
| Block/permit at ALE v4/v6 layers | **Yes** | Whole kill switch. WireGuard + Mullvad both. |
| Block/permit at `OUTBOUND_IPPACKET_*`, `IPFORWARD_*` | **Yes** | Same API; still no callout. |
| Interface/app-id/user-id/port/ICMP-type conditions | **Yes** | All in `FWPM_CONDITION_*`. |
| Persistent + boot-time filters | **Yes** | Mullvad does it from the user-mode daemon (`persistent.rs`). Verify locally — §10 test 8. |
| Transactions, purge, enumeration | **Yes** | |
| **Per-app socket redirection (split tunnel)** | **No** | Needs a callout at `ALE_BIND_REDIRECT_*` / `ALE_CONNECT_REDIRECT_*` → kernel driver. **Already our `SplitTunnel.sys`, and already gated on attestation signing (`app/SIGNING.md` §2) — LATER MILESTONE.** |
| Per-packet inspection/modification | No | Callout. Not needed. |

**None of §4-§8 is blocked by `app/SIGNING.md`.** The unfinished attestation
signing gates split tunnelling, not leak prevention. This is worth stating
explicitly in the plan, because R6/R7 currently read as if they might be.

One consequence worth naming: while the split-tunnel driver is unsigned/absent,
an app on the exclusion list is *not* excluded, and with the kill switch armed
its traffic is **blocked** rather than sent out the physical NIC. That is the
correct fail-closed behaviour, but it will look like a bug. Log it once per
session at the point where `splitTunnel_.IsAvailable()` is false and a rule set
is non-empty.

---

## 10. Safe manual test procedure

**Preconditions.** Two independent paths: the box's normal Wi-Fi/Ethernet, and a
**phone hotspot** as the out-of-band path. Do the WFP work on the box, observe
from the phone or from a second machine. Have an elevated PowerShell already open
before arming anything, with the recovery command typed but not run:

```powershell
# RECOVERY — type this before you arm anything, run it if you lose the network.
Stop-Service urnetworkd; & 'C:\Program Files\URnetwork\urnetworkd.exe' revert
# Nuclear option if a persistent set is stuck (an admin CAN always do this):
#   net stop bfe   /  net start bfe
```

Also: **do the first persistent/boot-time test in a VM with a checkpoint.** Not
on the daily driver. That is the one test that can cost a reinstall.

### The tests

**0. Baseline capture (before touching anything).**
```powershell
Get-NetRoute -AddressFamily IPv4 | Sort-Object -Property ifIndex | Format-Table
Get-NetRoute -AddressFamily IPv6
Get-DnsClientServerAddress
Get-DnsClientNrptPolicy; Get-DnsClientNrptRule
netsh wfp show filters file=C:\temp\wfp-before.xml
```
Keep `wfp-before.xml`. Every later diff is against it.

**1. Enumerate our objects (read-only, unelevated is fine for `show state`).**
```powershell
netsh wfp show filters file=C:\temp\wfp-armed.xml
Select-String -Path C:\temp\wfp-armed.xml -Pattern 'URnetwork' -Context 2,8
```
Confirm: two providers, three sublayers, expected filter count, expected weights,
and that our provider GUIDs are the only ones we added.

**2. Connected-state leak test — verify the leak is CLOSED, not assume it.**

The mistake to avoid is testing with a browser and concluding from a website.
Test at the wire.

- On the box: `pktmon start --capture --comp nics -f C:\temp\cap.etl` (or
  Wireshark on the **physical** adapter, not the tun).
- Generate traffic: browse, `nslookup example.com`, `ping -6 ipv6.google.com`,
  `curl -6 https://ipv6.google.com`.
- Stop, `pktmon etl2txt C:\temp\cap.etl`.
- **Pass = on the physical adapter you see only (a) our provider transport, (b)
  DHCP, (c) ARP/NDP, (d) LAN traffic. Zero port-53 packets to anything but our
  resolver. Zero IPv6 to a global address.**

Website-level checks (`test-ipv6.com`, `dnsleaktest.com`, `browserleaks.com/dns`)
are a *secondary* confirmation — they only see what your browser did, and a
browser doing its own DoH will show a clean result while the OS leaks.

**3. DNS leak test the OS way, not the browser way.**
```powershell
Resolve-DnsName leak-probe.example.com -Server 8.8.8.8    # must FAIL/time out
Resolve-DnsName leak-probe.example.com                     # must succeed via tun
nslookup leak-probe.example.com 192.168.1.1                # router: must FAIL
Get-DnsClientServerAddress | Format-Table -AutoSize        # inspect ALL adapters
```
Then flush and retest with the cache cold: `Clear-DnsClientCache`.
Watch `pktmon` for UDP/TCP 53 on the physical adapter throughout.

**4. The multi-homed test — this is the one that actually exercises R6.**
This is why the hotspot matters. With the tunnel up on Wi-Fi, **plug in the
phone hotspot over USB tethering** so a second adapter appears with its own DHCP
resolver. Repeat test 3. Windows' per-adapter resolution is exactly what will
try that second resolver.
Then reverse it: connect on the hotspot, plug in Ethernet.

**5. Kill-switch drop test — the real one.**
Do **not** click Disconnect (that is the orderly path). Instead:
```powershell
Stop-Process -Name urnetworkd -Force     # simulates a crash
```
- Expected: WFP objects **gone** (dynamic session), routes gone, network **open**,
  app shows the kill switch is no longer enforced. That is fail-open-by-design
  (§8.1) — confirm it is what you see, and that the app says so.
- Then the *armed* variant: with the kill switch on, pull the network cable /
  turn off Wi-Fi while `urnetworkd` stays alive. Expected: filters stay, and when
  the link returns, **nothing** flows until the tunnel is back. Verify with
  `pktmon` on the physical adapter, not with a ping.

**6. ALE reauthorization (existing flows).**
- Before connecting, open a long-lived connection: `ssh` somewhere, or
  `Test-NetConnection -ComputerName x -Port 443 -InformationLevel Detailed`, or
  just leave a streaming tab playing.
- Connect / arm.
- The flow must **die**, not continue. Remember reauth is triggered by the *next
  packet*, so an idle flow survives until it moves — poke it.

**7. Exemption regression sweep (armed state, one pass).**
`ping 127.0.0.1` · `ping <router>` · `ping <NAS>` · print to a network printer ·
`ipconfig /renew` · `ipconfig /renew6` · RDP to another LAN box ·
`Resolve-DnsName` via the tunnel · confirm the app can still reconnect.
Anything that fails here is a missing exemption from §7, and the rank tells you
how urgent.

**8. Persistent / boot-time (VM ONLY, checkpoint first).**
- Enable the advanced lockdown, stop the service, reboot.
- Expected: no network at all until `urnetworkd` starts and purges.
- **Then the important half:** with the persistent set installed, `sc config
  urnetworkd start= disabled`, reboot, and check whether the persistent filters
  came back. §8.3 predicts they do **not** (serviceName gate). Also check the
  boot-time set — that is the part I could not verify.
- Then: with the persistent set installed, uninstall the MSI, reboot, confirm
  the machine has network.
- `netsh wfp show filters` after each reboot; diff against `wfp-before.xml`.

**9. The three DNS invariants (§5) as explicit tests.**
- **App with its own resolver.** A Go binary using the pure-Go resolver, or
  `Resolve-DnsName -Server 8.8.8.8`, must be **blocked**. This is the test NRPT
  alone fails and WFP passes — it is the whole argument for the firewall floor.
- **Second adapter.** Covered by test 4; restate as a pass/fail invariant: *no
  port-53 packet may egress a newly-arrived adapter.*
- **Force-kill leaves nothing behind.** `taskkill /f /im urnetworkd.exe`, then:
  ```powershell
  Get-DnsClientNrptRule; Get-DnsClientNrptPolicy      # must be empty / ours gone
  Get-DnsClientServerAddress                          # no adapter left pointing at a dead resolver
  netsh wfp show filters file=C:\temp\wfp-after-kill.xml
  Select-String C:\temp\wfp-after-kill.xml -Pattern 'URnetwork'   # must find nothing
  Resolve-DnsName microsoft.com                       # must succeed
  ```
  Then start the service and confirm the startup purge reports zero leftovers.

**10. Attribution — prove a drop is OURS.**
```powershell
netsh wfp show netevents file=C:\temp\netevents.xml
```
Match the `filterId` in the drop events against the ids in
`netsh wfp show filters`. If a user reports "the VPN broke my printer", this is
how you tell our filter from Windows Firewall. Worth wiring into a
`urnetworkd diag` verb.

---

## 11. Reference implementations, side by side

| | Mullvad (`winfw` + `talpid-core`) | WireGuard for Windows | OpenVPN | Proton |
| --- | --- | --- | --- | --- |
| Session | **standard** (survives process) + explicit purge on start | **dynamic** (`cFWPM_SESSION_FLAG_DYNAMIC`) | own sublayer, shared across instances | not public |
| Sublayers | 2: baseline `MAXUINT16`, dns `MAXUINT16-1` | 1, weight `0xFFFF` | 1 shared | — |
| Layers | 4 ALE (C4/C6/A4/A6) only | 4 ALE + 2 `MAC_FRAME_NATIVE` (pre-IP DHCPv4) | ALE, DNS only | — |
| Weights | `Min=0` block-all, `Medium=7` permits, `Max=15` tunnel-endpoint rules | 15 service, 14 block-DNS, 13 loopback, 12 tun/DHCP/NDP, 0 block-all | — | — |
| DNS | lift port 53 in baseline, hard-block in dns sublayer, permit tunnel resolvers; own resolver on :53 | block 53 except configured servers; **no NRPT at all** | `--block-outside-dns` = WFP block on 53 except the tun (fixed shared sublayer GUID across `openvpn.exe`/`openvpnserv.exe` so concurrent tunnels don't fight); separately, 2.6's `--dns` uses an NRPT `"."` rule | — |
| LAN | opt-in "Local network sharing", default off; `10/8,172.16/12,192.168/16,169.254/16` + `fe80::/10,fc00::/7` + multicast | **none** — LAN is blocked with `/0` AllowedIPs | — | — |
| Self-exemption | `PermitEndpoint` (relay IP+port+proto, optionally + app id) | `permitWireGuardService` (`ALE_APP_ID` + `ALE_USER_ID`) | own process | — |
| Kill switch trigger | explicit policy states (`Blocked`/`Connecting`/`Connected`) | implicit: only when a single peer has a `/0` AllowedIP | — | standard = on drop; advanced = persistent |
| Across reboot | `apply_persistent_blocking()`: purge ephemeral + install boot-time **and** persistent block-all, one transaction | no | no | "Advanced kill switch" |
| Provider `serviceName` | **not used** | not used | — | — |

The disagreement worth noticing: **Mullvad uses a standard session and purges on
start; WireGuard uses a dynamic session and lets BFE clean up.** WireGuard's is
the safer default and Mullvad's is the more secure one. We can have both by
splitting them the way §4 does — dynamic for the normal states, persistent only
for an explicit opt-in mode with the serviceName gate underneath it.

Our closest analogue is WireGuard: same wintun adapter, same LocalSystem service,
same "one process owns the tunnel" shape. **Start from `tunnel/firewall/`, add
Mullvad's DNS sublayer, LAN network list and persistent design on top.**

---

## 12. Open questions / could not verify

1. Whether a **boot-time** filter honours the provider `serviceName` gate the way
   persistent objects do (§8.3). Bounded risk either way; test 8 answers it.
2. Whether `DeviceRemote::setRouteLocal` reaches the service, or whether the
   service must read persisted LocalState itself to know it is armed (§2).
3. GA status of Windows 11 CLAT (464XLAT) for Wi-Fi/Ethernet as of 2026-08.
4. Whether Android hotspot tethering universally provides IPv4 on IPv6-only
   carriers.
5. Proton's actual enforcement mechanism (WFP vs adapter binding) — not public.
6. `netsh interface ipv6 set prefixpolicy` + `store=active` non-persistence.
7. Whether NLA classifies a v6-blocked network as "no internet" and how loudly.
8. **Exact SMHNR behaviour per Windows 10 build** (parallel→sequential at
   1703/1709; the policy reportedly ignored in some 1803+ builds). Only secondary
   sources, and they disagree. The one rigorous source (Upchurch, SANS, 2021) was
   not retrievable. **Get that PDF before writing any version-conditional DNS
   code** — though §5.5's conclusion ("set it, never rely on it") makes the
   answer non-load-bearing.
9. Whether Microsoft ever removed the ability to disable SMHNR. No statement
   found either way; the ADMX policy still ships on Windows 11.
10. Whether `dnscache` honours `DnsPolicyConfig` subkeys created
    `REG_OPTION_VOLATILE`. If it does, that is free reboot-scoped NRPT cleanup.
    Untested, and it would not help against a force-kill without reboot anyway.
11. Whether ALE reauthorization reliably re-evaluates already-established
    **UDP** flows (long-lived socket bound before our filters exist). The
    condition and the mechanism are documented; an explicit Microsoft guarantee
    for this case was not found. §10 test 6 covers TCP; extend it to UDP.
12. Windows 11 `Add-DnsClientNrptRule -DohTemplate`: the Server 2025 cmdlet
    reference does not list the parameter. Moot for us (§5.1) but note it if the
    fallback is ever used.

---

## 13. Suggested next steps

1. **Correct `PLAN.md` R6/R7 and `NEXTSTEPS.md`**: the kill switch is not
   `vpnInterfaceWhileOffline`/`setRouteLocal`; it is a WFP policy in
   `urnetworkd`, and it is **not** blocked on driver signing (§9). Today's line
   `PLAN.md:203` reads as if it were an M5 polish item; it is the R6/R7
   mitigation and belongs with them.
2. **One table, one place** for the tunnel-capture prefixes and the LAN/multicast
   exemptions, so `kIncludedV4Routes` and the WFP LAN permit cannot drift (§7.2).
   Decide `169.254/16` and `224.0.0.0/3` deliberately while doing it.
3. Add a `Firewall` class next to `NetworkConfig` with the same shape:
   `Apply(state)` / `Revert()` / a startup `SweepOrphanedFilters(remove)` that
   honours the rpc-only observe-only contract.
4. Extend `tunnel_active` to record the WFP policy state (§8.4), and add the
   "why `CrashRevert` deliberately has no WFP path" note to `NetworkConfig.h`
   next to the existing crash-safety comment block.
5. Run §10 tests 0-5 and 9 on the owner's box before writing filter code — the
   baseline `wfp-before.xml` is only capturable while the machine is clean.
   Tests 8 (persistent/boot-time) in a VM with a checkpoint, never on the daily
   driver.
