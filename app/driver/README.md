# URnetwork split-tunnel driver (SplitTunnel.sys)

Per-app split tunneling: excluded processes bypass the VPN tunnel and egress via
the physical interface. WFP callout driver that redirects excluded processes'
outbound socket binds/connects to the physical interface — the Mullvad/NordVPN-
class mechanism, implemented **clean-room** (see `PROVENANCE.md`), MPL-2.0.

Controlled over IOCTL by `urnetworkd` (see `Ioctl.h`, the shared contract).

## Behavioral spec (the authority for the implementation)

**Exclusion set.** A list of full image paths. A process is *excluded* if its
main image path matches (case-insensitive) an entry, OR it is a descendant of an
excluded process (child-process inheritance, tracked via the create-process
notify routine at launch time). Rationale: launchers/helpers of an excluded app
should also bypass.

**What is redirected — the criterion is the process, never the destination.**
At `ALE_BIND_REDIRECT` (v4 and v6), the owning process id is in the classify
metadata. If that process is excluded and redirection is enabled, rewrite the
socket's bind local address to the physical interface's source address, so the
flow egresses the physical NIC and bypasses the tun. No remote/destination
address is examined — this is per-socket (per-process) redirection, chosen at
bind() before any destination is known. Non-excluded processes, and all flows
when disabled, are passed through untouched. The physical source address is
supplied by the service (`IOCTL_URST_SET_PHYSICAL_ADDRS`) and refreshed on every
network change.

**Physical interface.** Supplied by the service (`IOCTL_URST_SET_PHYSICAL_ADDRS`)
as interface indices (+ optional source addresses), refreshed on every network
change. If none is set, redirection is inert (fail-open: never blackhole).

**Sockets already open.** Only *new* binds/connects are redirected; sockets a
process opened before it became excluded keep their existing path until they
reconnect. Documented, accepted for v1 (matches how bind-redirect works).

**Loopback.** Rebinding an excluded socket to the physical source would break
that process talking to 127.0.0.1. Because exclusion is decided at bind() (no
destination in scope), this is handled by a *complementary* connect-redirect
fixup that, for loopback/local **destinations only**, reverts the source to
loopback. That fixup is keyed on the destination being local — it never changes
which sockets are excluded (still purely the process set). Deferred to the
hardening pass (R10). The app↔service pipe and device RPC run in the service
(not an excluded app), so they are unaffected regardless.

**DNS.** Excluded apps resolve via the shared `dnscache` service, whose queries
originate from `svchost`, not the app — so DNS for excluded apps is NOT
inherently split. v1 behavior: excluded apps' DNS follows the system resolver
(may go through the tunnel). This is the known hard corner (plan R10); a later
revision may special-case port-53 flows. Documented so behavior is explicit,
not accidental.

**Fail-open.** Any driver error, missing physical interface, or disabled state
results in pass-through, never a blocked flow. A VPN split-tunnel driver must
never strand traffic.

## Structure

- `Driver.c` — DriverEntry, WDM device + symbolic link, IOCTL dispatch, WFP
  engine/sublayer/callout/filter setup, the classifyFn redirect, and the
  create-process notify routine + exclusion state.
- `Ioctl.h` — the shared user/kernel IOCTL contract.
- `SplitTunnel.inf` — install information.
- `SplitTunnel.vcxproj` — WDK (KMDF/WDM) project.

## Status

Skeleton + spec (plan M3.5). The device/IOCTL plumbing and WFP registration are
laid out; the classify redirect and process-tree tracking are implemented to the
spec above and must go through Driver Verifier + stress + leak testing before
ship (plan R10 — the hardening burden is ours since this is clean-room, not a
battle-tested upstream).

## Building & loading (dev)

Needs the WDK matching the installed SDK. Build `SplitTunnel.vcxproj`. To load a
dev (unsigned) build: `bcdedit /set testsigning on`, reboot, then
`sc create URnetworkSplitTunnel type= kernel binPath= <path>\SplitTunnel.sys` and
`sc start URnetworkSplitTunnel` (or install via the INF). Production builds are
attestation-signed (see `PROVENANCE.md`).
