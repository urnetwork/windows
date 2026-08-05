# urnetworkd bring-up: the owner's checklist, and what the audit found

2026-08-05. WP2 deliverable. `urnetworkd.exe` has never executed a single
instruction. **Every runtime statement below is unverified** — nobody in the
session that wrote this had a Windows box with admin rights, and the service
needs LocalSystem plus a tun device. What is verified is that the code
compiles, and what the code *says* it does, read line by line.

The checklist in §3 and §4 is the whole point of this document: it is the
sequence the owner runs on a real machine to turn "compiles" into "works", and
to confirm the two risks that matter — R1 (the service deadlocking against its
own tunnel) and the revert path (the service leaving the machine with no
network).

---

## 1. Read this first: can this thing strand my machine?

Short answer: **it should heal by itself, and there are now three
independent backstops if it does not.** Long answer, because this is the
highest-severity item in the plan:

**What the service writes.** `NetworkConfig::Apply` only ever writes state that
hangs off the tun interface it just created:

- the tun's unicast address (`169.254.2.1/24` by default),
- the tun's MTU and interface metric,
- 31 IPv4 routes **whose `InterfaceLuid` is the tun** (the complement of the
  RFC1918 ranges, so LAN traffic bypasses the tunnel),
- the tun interface's DNS servers.

It does **not** delete or modify the physical default route, does not touch any
other adapter's DNS, and installs no NRPT rule and no WFP filter (R6/R7 are
still open — see §6). The tunnel captures traffic by adding prefixes that sort
*above* the physical default route, never by removing it.

**Why that matters.** Everything the service writes is scoped to one interface.
Windows deletes an interface's routes, addresses and DNS when the interface goes
away. A wintun adapter created with `WintunCreateAdapter` is a *software device
owned by the creating process* — when `urnetworkd.exe` dies for any reason
(crash, `taskkill /F`, power loss), the handle closes, the device is removed, and
the route table returns to what it was. **No code of ours has to run.** This is
the same property wireguard-windows relies on, and it is the reason a crash here
is survivable at all.

> This claim is from the wintun/`SwDeviceCreate` contract, not from observation.
> **§4 test B is the test that proves it.** If it turns out to be false on some
> Windows build, everything else in this section is what saves the machine.

**The backstops, in the order they fire:**

1. `NetworkConfig::CrashRevert()` — deletes the 31 routes and clears the tun DNS
   from an unhandled-exception filter, a `std::terminate` handler, or the console
   control handler (window close / logoff / shutdown). Allocation-free and
   lock-free on purpose: it runs when the process is already broken, and it runs
   *before* it logs, because formatting allocates.
2. `NetworkConfig::SweepOrphanedTunnel()` at every service start — finds a tun
   interface that outlived its process, by the pinned adapter GUID *and* by
   adapter alias (wintun treats the GUID as a request and may fall back to
   another), and takes its routes back. The installed service restarts itself
   after a crash (`SC_ACTION_RESTART`, 5s), so this normally fires within seconds
   without anyone doing anything.
3. `urnetworkd revert` — a command that takes back leftover routes and DNS
   without starting anything. This is the manual escape hatch. Run it elevated.

**And if all of that fails: reboot.** Routes created with
`CreateIpForwardEntry2` are not persistent — they live in the stack, not in the
registry, so they do not survive a restart. (Also per the docs, not observed.)

**The one case that is genuinely not covered:** the split-tunnel kernel driver.
If it is ever shipped and loaded, an abnormal exit leaves the driver service
registered and its WFP filters in place; only `SplitTunnelClient::Close()` on an
orderly path unloads it. The driver is excluded from the solution's build
configurations today, so it cannot load, and this is a note for whoever turns it
on — see §6.

---

## 2. Before you start

On the machine under test:

- An **elevated** PowerShell (`Run as administrator`). Without it, adapter
  creation and every route write fail; the service says so on the first line now,
  but it is a wasted round trip.
- `urnetworkd.exe`, `URnetworkSdk.dll` and `wintun.dll` in the same directory.
  The build copies the last two next to the exe; if `wintun.dll` is missing the
  log says `[1/8] loading wintun from ...` and then fails with a named error.
- The tray app (`URnetwork.exe`) for anything past "the service starts" — the
  service does nothing on its own until the app sends `start_tunnel` over
  `\\.\pipe\urnetwork.control`.
- Baseline the network **before** connecting, so you can compare afterwards:

```powershell
Get-NetRoute -AddressFamily IPv4 -DestinationPrefix 0.0.0.0/0 |
  Select-Object ifIndex, InterfaceAlias, NextHop, RouteMetric, ifMetric
Get-NetIPConfiguration | Select-Object InterfaceIndex, InterfaceAlias, IPv4Address, IPv4DefaultGateway
```

Write down the **ifIndex of your physical adapter** (Wi-Fi or Ethernet). Call it
`$PHYS`. Half of the R1 check is "does the service pick exactly this number".

Log file: `C:\ProgramData\URnetwork\service\logs\urnetworkd.log`.
`urnetworkd help` prints the path it is actually using.

---

## 3. Does it start? (`console` mode)

```powershell
.\urnetworkd.exe console
```

`console` now echoes the log to stdout, so this terminal is the transcript.
Expected, in order:

```
urnetworkd starting: pid=… cmd="console" identity=elevated administrator sdk=… log=C:\ProgramData\…
console: starting as elevated administrator
service: no leftover tunnel state from a previous run
sdk initialized: version=… logDir=… memLimit=64MB
console: running on \\.\pipe\urnetwork.control; press Ctrl+C to stop
```

(The leftover-state sweep runs *before* the SDK comes up on purpose: if a
previous run left the machine pointed at a tun that is gone, handing the routes
back matters more than getting the SDK started.)

Things that are now loud rather than silent, and what they mean:

| Line | Meaning |
| --- | --- |
| `NOT elevated` | Re-run elevated; a `start_tunnel` would die at step 1. |
| `is already served — another urnetworkd … is running` | `sc stop urnetworkd` first; the control pipe is single-instance. |
| `the previous run exited with tunnel routes installed` | A crash. §1's machinery already ran. Note it and tell us. |
| `ORPHANED tun interface … removed N stale routes` | The adapter outlived a previous process. **This is the case §1 says should not happen — please report it with the log.** |
| `cannot write …; logging to the debugger only` | The log file could not be opened; everything else still runs. |

**Then press Ctrl+C.** Expect:

```
console: Ctrl+C — shutting down
console: stopping
console: stopped cleanly, network restored
```

Before this change, Ctrl+C killed the process outright. If you see the process
exit *without* those three lines, that is a finding.

Also worth one run each: `urnetworkd help`, and `urnetworkd` with no argument
from a shell (it should log `not started by the SCM; falling back to console
mode` and behave as above).

---

## 4. The two tests that matter

### Test A — R1: the service must not loop into its own tunnel

This is the top risk in `NEXTSTEPS.md`. With the tunnel up, `urnetworkd`'s own
platform and provider sockets must egress on the **physical** adapter. If they
follow the tun, the client talks to itself and wedges.

**A1. The service picks the physical adapter, and picks it early.**
Start the tunnel from the tray app, then read the log. You are looking for these
lines *in this order*:

```
tunnel: [1/8] adapter up: luid 0x…, interface 27 "URnetwork" (…)
tunnel: [2/8] binding sdk egress to the physical interface (R1)
egress: bound to v4=[12 "Wi-Fi" (Intel …, type=71, connected)] src=192.168.1.23 v6=[…]
tunnel: [3/8] opening the network space in …
…
netcfg: installed 31 tun routes (private ranges excluded)
```

Check all four:

- [ ] the `egress:` interface index equals `$PHYS` from §2 — **not** the
      `URnetwork` interface index printed on the `[1/8]` line;
- [ ] `src=` is your LAN address, not `169.254.x.x`;
- [ ] the `egress:` line appears **before** `netcfg: installed 31 tun routes`
      (the binding must be chosen while the route table is still clean);
- [ ] the `egress:` line appears **before** `[3/8]`/`[4/8]` (no SDK object, so
      no socket, exists yet — a socket created unbound would follow the route
      table once the tun routes land).

If instead you see `egress: NO physical ipv4 interface bound`, R1 protection is
not in force. Expected only when the machine genuinely has no network.

**A2. The sockets actually landed on the physical adapter.** With the tunnel
up:

```powershell
$pid = (Get-Process urnetworkd).Id
Get-NetTCPConnection -OwningProcess $pid -State Established |
  Select-Object LocalAddress, LocalPort, RemoteAddress, RemotePort
```

- [ ] every `LocalAddress` is the physical adapter's address (or a loopback
      address for the device RPC the tray app dials) — **none** is the tun's
      `169.254.2.1`.

This is the single most direct evidence for R1. The mechanism is
`IP_UNICAST_IF` (`connect/egress_windows.go`), which forces the outgoing
interface, so a connected socket's local address reflects the physical adapter.

> Note for UDP/QUIC: the platform transport's UDP socket is wildcard-bound, so
> `Get-NetUDPEndpoint` will show `0.0.0.0` regardless. That is expected and is
> not evidence either way — `IP_UNICAST_IF` changes interface selection, not the
> bound address. Judge QUIC by A3 instead.

**A3. It behaves.** With a provider connected:

- [ ] browsing works through the tunnel;
- [ ] the connection stays up for several minutes (a self-loop typically shows
      as: the tunnel comes up, then the client's own platform connection stalls
      and never recovers);
- [ ] `netsh interface ipv4 show interfaces` shows the tun at metric 1 and the
      physical adapter still holding its default route.

**A4. Network change.** Move networks while connected (Wi-Fi off/on, unplug
Ethernet, or switch between the two):

- [ ] a new `egress: bound to v4=[…]` line appears with the new interface;
- [ ] if the network is momentarily gone, you see `no ipv4 default route right
      now — keeping the last physical interface … bound` rather than an unbind.
      (Deliberate: unbinding while the tun routes are installed is exactly the
      loop R1 exists to prevent.)

### Test B — revert: an abnormal exit must not strand the machine

Run these in order, and after **each** one check that the network is back:

```powershell
Get-NetAdapter -Name URnetwork -ErrorAction SilentlyContinue     # expect: nothing
Get-NetRoute -AddressFamily IPv4 | Where-Object InterfaceAlias -eq 'URnetwork'  # expect: nothing
Test-NetConnection -ComputerName 1.1.1.1 -InformationLevel Quiet # expect: True
```

**B1 — orderly stop.** Tunnel up, then `sc stop urnetworkd` (or Ctrl+C in
console mode). Expect `netcfg: reverted (31 of 31 routes removed, dns cleared)`
and `tunnel: stopped, network restored`. Then the three checks above.

**B2 — hard kill. This is the important one.** Tunnel up and browsing, then:

```powershell
Stop-Process -Name urnetworkd -Force
```

- [ ] the three checks above pass **within a few seconds** and with no
      intervention. This is §1's core claim: the adapter dies with the process
      and takes the routes with it.
- [ ] if the installed service is in play it restarts itself after ~5s; its log
      should then say `the previous run exited with tunnel routes installed …`.
      Note which of the two variants it prints — `its adapter outlived it` is
      the interesting one.
- [ ] **If the network does NOT come back**, that is the finding this whole
      work package exists for. Recover with `.\urnetworkd.exe revert` (elevated)
      and send the log plus `route print`.

**B3 — sleep/resume.** Tunnel up, sleep the machine, resume. Expect an
`egress:` line on resume; expect the tunnel either to recover or to fail
visibly. (Known gap: the service does **not** currently tell the SDK about a
power-state change — see §6.)

**B4 — reboot with the tunnel up.** Reboot while connected. After login:

- [ ] no `URnetwork` adapter, no stale routes, network normal. Routes created
      this way are non-persistent, so this should hold even if everything else
      failed.

**B5 — the escape hatch.** Run `.\urnetworkd.exe revert` elevated with nothing
running. Expect `marker=no orphaned_interfaces=0` and `no URnetwork tun
interface present`. Confirms the recovery command works *before* you need it.

---

## 5. What the audit found (code reading, not runtime)

**R1 wiring — correct, and now correct for a second reason.**
`EgressMonitor` → `NetworkConfig::DiscoverEgress` picks the lowest
route-metric + interface-metric default route (`PrefixLength == 0`) that is not
the tun LUID — the same ordering Windows itself uses — and pushes it through
`urnet::setEgressInterfaceIndex` → `sdk.SetEgressInterfaceIndex` →
`connect.SetEgressInterfaceIndex`. The tun cannot be picked even by accident: it
never gets a `0.0.0.0/0` route, only the 31 complement prefixes.

On the connect side the index covers all three socket families the service
opens (`IP_UNICAST_IF` / `IPV6_UNICAST_IF`, `egress_windows.go`):

| Sockets | Path | Applied by |
| --- | --- | --- |
| every TCP dial | `ConnectSettings.NetDialer` → `egressDialer` | `net.go:129` |
| the platform QUIC `UDPConn` | `applyEgress(udpConn)` | `transport.go:1274` |
| pion ICE / p2p UDP | `egressNet.ListenUDP` → `applyEgress` | `egress_net.go` |

The third one is worth knowing about, because it is why the "never push 0"
change matters more than it looks: `transport_p2p_webrtc_pc.go` chooses
`newEgressNet()` **only when `EgressInterfaceIndex()` is non-zero**, and
otherwise falls back to `newIceInterfaceNet`, whose `dialLocalIP` opens a
connect-only UDP socket to discover the local address. With the tunnel up and
the index cleared, that socket resolves through the tun and reports
`169.254.2.1` as the host's address — wrong ICE candidates on top of the
unbound-socket problem. Unbinding is worse than pinning to a downed NIC in two
separate ways.

The binding was already applied before the routes. It is now also applied before
the `NetworkSpace` and `DeviceLocal` are constructed, so no SDK socket can be
created while the binding is unset. Ordering is asserted in a comment at the
call site because it is the entire mechanism.

Two behaviours were changed as a result of the audit:

- losing the default route no longer pushes `0` down to the SDK (see A4);
- the chosen interface, its description and its source address are logged, so
  R1 can be audited from the log rather than from a debugger.

**Revert path — sound, and it was already the right shape.** Nothing outside the
tun interface is mutated, which is what makes an abnormal exit survivable. What
was missing was any path that ran when `Stop()` did not, any way to notice that a
previous run had died dirty, and any way for the owner to recover by hand. All
three now exist (§1).

**Also fixed while in here:** `Revert()` deletes the tun address as well as the
routes; a partial route install is reported instead of silently reverting; a
failed DNS set is reported as the R6 leak it is.

## 6. Known gaps — deliberately not fixed here

- **Power events.** The service does not accept `SERVICE_CONTROL_POWEREVENT` and
  does not call `DeviceLocal::networkChanged()` on resume. The egress binding
  follows the interface change; the SDK is not told to re-dial. Worth doing once
  B3 shows what actually happens.
- **`SERVICE_ACCEPT_PRESHUTDOWN`.** The service accepts `SHUTDOWN` (~5s of
  grace). Preshutdown would give minutes. Not needed while the adapter teardown
  is the real mechanism, but it is the cheap upgrade if B4 ever misbehaves.
- **R6 (DNS leak) and R7 (IPv6 leak)** remain open, as `NEXTSTEPS.md` §4 says.
  The tun DNS is set; other adapters' resolvers are not blocked, and IPv6 is not
  blackholed. Note that both fixes are *machine-wide* state (NRPT rules, WFP
  filters) rather than interface-scoped, so whoever implements them inherits a
  revert problem this document does not currently have. Read §1 before writing
  either.
- **The split-tunnel driver's abnormal-exit residue** (§1). It cannot load today.
- **`Log.cpp` reads `g_tag` outside the mutex** that `LogInit` writes it under —
  a pre-existing data race in `Common/`, harmless in practice (both writes happen
  at startup) but real. Left alone because `Common/Log` is shared with WP1.
