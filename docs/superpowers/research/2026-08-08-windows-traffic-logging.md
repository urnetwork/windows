# Connection logging and per-process attribution on Windows

External research, 2026-08-08, against `beta/custom-server` `852bfd1`. **Decision
document, no code.** Nothing in this repo was edited to produce it.

The ask, in the owner's words: *"Advanced Mode should let us take a look at those
network requests deeply just like Portmaster."* Portmaster's bar is that **every
connection is attributed to a process**, with hostname, destination, protocol and
a verdict. This document establishes how much of that we can actually source, in
tiers, and names the fields we must refuse to show.

**Evidence discipline.** Every claim below is either (a) cited to a Microsoft doc
or to a file in a public repo, (b) read directly out of this repo, or (c)
explicitly marked **UNVERIFIED**. There is a consolidated list of the unverified
claims at the end. Nothing here was tested on this machine — no driver was
loaded, no service was started, no elevated command was run.

---

## The one-paragraph answer

The single most valuable finding is **not** on the OS side. It is that the SDK
already asks us the question we want answered: `DeviceLocal::setFlowOwnerLookup`
takes a callback with the signature
`std::string(int64_t version, int64_t protocol, std::string source_ip, int64_t
source_port, std::string destination_ip, int64_t destination_port)`
(`app/third_party/urnetwork-sdk/amd64/urnetwork_sdk.hpp:9568`, declared on
`DeviceLocal` at `:10066`). That callback fires **in the service process, for a
flow, carrying protocol and both ports** — three of the five fields the D6 plan
lists under "Not available, do not fake". We do not need WFP or a driver to get
them; we need to implement a callback the SDK is already prepared to call. The
process attribution that hangs off it costs one `GetExtendedTcpTable` snapshot.
Meanwhile the user-mode WFP net-event route — the thing this research was sent to
evaluate as the likely sweet spot — **does not work for a connection feed**, for
a reason that is documented and decisive: the WFP net-event log is a *128 KB
circular buffer holding about 100–150 events*, and its documented contents are
drops, not allows.

---

## 1. The bar: what Portmaster actually does

Read from `safing/portmaster` at `development` (the `safing/portmaster-kext`
repo 404s — the kernel extension was folded into the main repo under
`windows_kext/`). Repo last pushed 2026-08-07, so this is current.

### 1.1 It is a kernel callout driver, and that is where the PID comes from

`windows_kext/driver/src/callouts.rs` registers **sixteen callouts**:

| Layer | Action | Purpose (their comment) |
|---|---|---|
| `AleAuthConnectV4` / `V6` | `CALLOUT_TERMINATING` | block/permit outgoing connections |
| `AleEndpointClosureV4` / `V6` | `CALLOUT_INSPECTION` | detect when a connection has **ended** |
| `AleResourceReleaseV4` / `V6` | `CALLOUT_INSPECTION` | detect when a port has been released |
| `StreamV4` / `V6` | `CALLOUT_INSPECTION` | **bandwidth statistics** for TCP |
| `DatagramDataV4` / `V6` | `CALLOUT_INSPECTION` | bandwidth statistics for UDP |
| `Outbound`/`InboundIppacketV4` / `V6` | `CALLOUT_TERMINATING` | redirect/block/permit packets |

The PID comes from the **classify metadata at the ALE layer**, not from any
lookup: `windows_kext/driver/src/ale_callouts.rs` builds an `AleLayerData` whose
`process_id: u64` field is filled by `data.get_process_id().unwrap_or(0)`, in the
same struct literal as `protocol`, `local_ip`, `local_port`, `remote_ip`,
`remote_port`, `interface_index`. One classify call yields the whole tuple **plus
the owning process, atomically, at connect() time**. There is no race, because
there is no lookup.

That struct is shipped to user mode verbatim.
`windows_kext/kextinterface/info.go` defines `connectionV4Internal` as
`{ ID, ProcessID uint64, Direction, Protocol byte, LocalIP, RemoteIP, LocalPort,
RemotePort, PayloadLayer }`, with matching `ConnectionEndV4` and
`BandwidthValueV4 { …, TransmittedBytes, ReceivedBytes uint64 }` messages. So the
driver gives user mode: **start, end, and per-connection byte counters**, each
tagged with a PID.

**This is the thing we cannot replicate in user mode.** Everything in tiers 1–2
below is an attempt to approximate `get_process_id()` from outside the kernel.

### 1.2 The connection cache and its lifetime

`windows_kext/PacketFlow.md` documents the cache: it "holds information for all
TCP and UDP connections. Local and destination ip addresses and ports, verdict,
protocol, process id. It also holds last active time and end time." Entries are
evicted "1 minute after an end state has been set or after 10 minutes of
inactivity." The end state is set by the endpoint-closure and resource-release
callouts — which is how Portmaster can show a genuine **duration** and mark a
connection dead. We have no equivalent signal, and that matters below.

### 1.3 DNS → connection correlation: they own the resolver

This is the part that is directly transferable to us. Portmaster does **not**
sniff DNS off the wire and it does **not** use the DNS-Client ETW provider. It
*is* the system resolver, and it keeps two maps (`service/network/dns.go`):

- `dnsRequestConnections`, keyed `"<protocol>-<local ip>-<local port>"` — used to
  attribute the DNS query itself to the process that made it.
- `openDNSRequests`, keyed `"<pid>/<fqdn><qtype>"` — a *pending* answer waiting
  for the connection that will follow it.

The key builder is scoped by PID: `getDNSRequestCacheKey(pid int, fqdn string,
qType uint16)`. Only `A`, `AAAA`, `SVCB`, `HTTPS` are "supported" record types
(`supportedDomainToIPRecordTypes`) — i.e. only records that yield an IP get
merged into a subsequent connection. There is an explicit 3-second window,
`openDNSRequestLimit = 3 * time.Second`, after which a DNS request with no
following connection is logged on its own.

Two details worth stealing:

- They refuse to attribute when the PID is unusable. `SaveDNSRequestConnection`
  bails if `conn.PID == UndefinedProcessID || conn.PID == SystemProcessID`, with
  the comment *"When re-injecting packets on Windows, they are reported with
  kernel PID (4)."* That is a real Windows trap and it will bite us the same way.
- The reverse map is **many-to-many and scoped to a process group**.
  `service/resolver/ipinfo.go` stores `IPInfo { IP, ProfileID, ResolvedDomains
  []ResolvedDomain }`, where `ProfileID` scopes the entry to a process group and
  `ResolvedDomain` carries the CNAME chain. An IP does not have *a* hostname; it
  has a list, per profile. `ResolvedDomains.String()` joins them with `" or "` —
  they render the ambiguity rather than picking a winner. That is exactly the
  discipline this project already applies elsewhere.

### 1.4 Storage: SQLite, split live/history

`service/netquery/database.go`. Connections are persisted to SQLite (live db and
a separate `historyPath`). The row (`netquery.Conn`) is the honest inventory of
what Portmaster believes it knows per connection:

```
id, profile, path, type, external, ip_version, ip_protocol,
local_ip, local_port, remote_ip, remote_port, domain,
country, asn, as_owner, latitude, longitude, scope,
worst_verdict, verdict, firewall_verdict,
started, ended, tunneled, encrypted, internal, direction,
extra_data, allowed, profile_revision, exit_node,
bytes_received, bytes_sent, active, profile_name
```

Note `path` (the process image), `worst_verdict` **and** `verdict` **and**
`firewall_verdict` as three separate columns, and `exit_node`.

Retention is two-stage (`service/netquery/module_api.go`,
`service/network/clean.go`):

- a "netquery live db cleaner" worker deletes connections ended longer ago than
  `network.DeleteConnsAfterEndedThreshold = 10 * time.Minute`;
- a separate history cleaner (`CleanupHistory`) runs on a delay of 10 minutes and
  applies a user-configured retention threshold; there are API endpoints
  `netquery/history/clear` and `netquery/history/cleanup`, per-profile clearing,
  and a `VacuumHistory` on shutdown;
- the in-memory connection state has its own cleaner on a
  `cleanerTickDuration = 5 * time.Second` tick, with
  `DeleteIncompleteConnsAfterStartedThreshold = 1 * time.Minute`.

So: **a 5-second in-memory sweep, a 10-minute live retention, and a separate
user-controlled history database.** Three tiers of retention, not one buffer.

---

## 2. Per-process attribution from user mode — the four candidate routes

### 2.1 `GetExtendedTcpTable` / `GetExtendedUdpTable` polling

The classic route: snapshot the TCP/UDP tables with `TCP_TABLE_OWNER_PID_ALL`,
match the 5-tuple, read `dwOwningPid`. Portmaster still carries this
(`service/network/iphelper/get.go` — `GetTCP4Table`, `GetTCP6Table`,
`GetUDP4Table`, `GetUDP6Table`), and on Windows `state/system_windows.go`'s
`CheckPID` is a **no-op** returning `socketInfo.GetPID()`, i.e. the PID is
already present from the table walk. Note their own TODO in `get.go`: *"It's
unproven if we can access the iphlpapi.dll concurrently"* — they serialise every
call behind a single global lock.

**The race, concretely.** The table is a *point-in-time* snapshot of sockets that
currently exist. A connection that opens and closes between two polls is simply
absent — it was never in any snapshot. Published characterisations of this
approach agree: *"a drawback of polling every second is that short-lived (i.e.
subsecond) connections will typically be missed"*, and *"the more frequent the
polling, the shorter the connection types that are likely to be captured."*

Practically:

- **TCP is partially rescued by `TIME_WAIT`.** A closed TCP connection lingers in
  `TIME_WAIT` (commonly ~30s, Windows default 2×MSL = 240s unless tuned) and
  remains in `GetExtendedTcpTable` — but **`TIME_WAIT` entries do not carry a
  usable owning PID**, because the owning process may be gone. So `TIME_WAIT`
  saves the *connection* from being missed and loses the *attribution*, which is
  the only field we wanted. **UNVERIFIED** in detail — the exact PID reported for
  a `TIME_WAIT` row on current Windows 11 should be checked by a spike; the
  standard behaviour is that it is 0 or the system process.
- **UDP is worse.** There is no state machine, so a DNS query's ephemeral socket
  may exist for single-digit milliseconds. A 1 Hz poll will miss essentially all
  of them; even a 10 Hz poll will miss most.
- **Cost.** Each call allocates and copies the whole table. On an idle desktop
  that is a few hundred rows across the four tables; on a busy one, low
  thousands. This is not free at 10 Hz, and Portmaster's global lock means the
  four calls serialise. Treat "poll at 1 Hz, cheap; poll at 10 Hz, measurable"
  as the working assumption. **UNVERIFIED** — no measurement was taken.

**Verdict:** usable as a *best-effort enrichment*, never as the connection feed.
It answers "who owns this socket **right now**", which is a different question
from "who opened this connection".

### 2.2 ETW

Three providers are relevant.

**`Microsoft-Windows-Kernel-Network`** (`{7DD42A49-5329-4832-8DFD-43D979153A88}`)
emits TCPv4/TCPv6/UDPv4/UDPv6 events. The fatal caveat is well documented in the
network-tracing literature: **on received packets the PID reflects whatever
process happened to be running when the packet arrived (interrupt or DPC
context), not the socket owner.** Outbound send events are generally in the
caller's context and are better, but this is precisely the "plausible wrong
value" failure mode the inspector is built to avoid. Correlating with
`Microsoft-Windows-Winsock-AFD` or `Microsoft-Windows-TCPIP` is the documented
workaround, which means running and joining two or three trace sessions.

**`Microsoft-Windows-Winsock-AFD`** (`{E53C6823-7BB8-44BB-90DC-3F86090D48A6}`)
has `AfdConnectWithAddress` / `AfdConnectWithAddressConnected` events. These fire
in the calling thread's context on the user-mode `connect()` path, so the PID is
the real one. **UNVERIFIED**: whether the address is present on the event in a
form we can parse without the manifest, and what the event volume is on a busy
machine.

**Privileges.** Kernel-flagged providers can only be enabled in trace sessions
that require **administrator** privileges, and the classic NT Kernel Logger
family is limited to 8 concurrent sessions (2 reserved). Our service runs as
LocalSystem, so this is satisfiable — but **it lands the work in the service
process**, same as everything else here, and it means the UI process can never
do it directly.

**Verdict:** ETW is a real option but it is a second, independent telemetry
pipeline with its own session lifetime, its own manifest-parsing burden, its own
buffer-loss failure mode, and a documented PID-correctness caveat on the inbound
path. It buys us nothing that §2.4 or §3 does not buy more cheaply, unless we
specifically want the DNS provider (see §3.3).

### 2.3 User-mode WFP net events — **this does not work, and here is why**

This was flagged as the likely sweet spot. It is not. Three independent findings
kill it, and they compound.

**Finding A — there is no PID in the event.** `FWPM_NET_EVENT_HEADER3`
(fwpmtypes.h, Win10 1607+) has, in full: `timeStamp, flags, ipVersion,
ipProtocol, localAddrV4/V6, remoteAddrV4/V6, localPort, remotePort, scopeId,
appId, userId, addressFamily, packageSid, enterpriseId, policyFlags,
effectiveName`. There is **no process-id member**. The flags enumerate exactly
what may be set — `IP_PROTOCOL_SET`, `LOCAL_ADDR_SET`, `REMOTE_ADDR_SET`,
`LOCAL_PORT_SET`, `REMOTE_PORT_SET`, `APP_ID_SET`, `USER_ID_SET`, `SCOPE_ID_SET`,
`IP_VERSION_SET`, `REAUTH_REASON_SET`, `PACKAGE_ID_SET` — and there is no PID
flag either. What we get instead is `appId`, *"the application ID of the local
application associated with the event"*: the NT device path of the image
(`\device\harddiskvolume2\...\chrome.exe`). That is the process **image**, not the
process **instance**. For a UI that says "which app is talking", the image path is
arguably enough. For anything that needs to distinguish two instances, or to join
to a live process, it is not.

**Finding B — the log is 128 KB and holds ~100–150 events.** The WFP *Logging*
page is unambiguous: *"Logged events are stored in a circular log, that is new
events override old ones when the log reaches its maximum size… The event log has
a maximum size of 128 KB and it can hold about 100 to 150 events."* It also says
*"The WFP event log is emptied after a reboot"*, and that `FwpmNetEventEnum`
*"returns only events that were logged prior to the creation of the enumHandle"*
— it is a snapshot of a ring. On a machine making thousands of connections an
hour, a 150-entry ring wraps in **single-digit seconds**. `FwpmNetEventEnum` as a
connection feed is structurally impossible.

`FwpmNetEventSubscribe4` is a *push* subscription rather than a read of the ring,
so it is not bounded by the ring in the same way — but it is fed by the same
generation path, and nothing in the documentation suggests subscription widens
what is generated.

**Finding C — the documented content is drops, not allows.** The same *Logging*
page enumerates what WFP logs: *"IKE/AuthIP main mode failures. IKE/AuthIP quick
mode failures. AuthIP extended mode failures. **Packets dropped during
classification.** Packets dropped by IPsec."* Allows are not in the list.
`FWPM_NET_EVENT_TYPE_CLASSIFY_ALLOW` exists as an enum value (Win8+) and
`FWPM_NET_EVENT_CLASSIFY_ALLOW0` exists as a struct — carrying only `filterId,
layerId, reauthReason, originalProfile, currentProfile, msFwpDirection,
isLoopback` — but the platform's own description of what it logs by default does
not include allowed traffic. Enabling it appears to be the
`Filtering Platform Connection` **audit** subcategory, not an engine option:
`FwpmEngineSetOption0`'s `FWPM_ENGINE_COLLECT_NET_EVENTS` is a global on/off, and
`FWPM_ENGINE_NET_EVENT_MATCH_ANY_KEYWORDS` takes exactly two values —
`FWPM_NET_EVENT_KEYWORD_INBOUND_MCAST (1)` and
`FWPM_NET_EVENT_KEYWORD_INBOUND_BCAST (2)`. Neither is "also give me allows".

**The empirical confirmation.** Windows Firewall Notifier (`wokhan/WFN`) is the
best-known user-mode Windows connection monitor that ships without a driver. A
code search of that repo returns **zero** hits for `FwpmNetEvent`. What it does
instead: `Console/Helpers/InstallHelper.cs` shells out to
`auditpol.exe /set /subcategory:{0CCE9226-69AE-11D9-BED3-505054503030}
/success:enable /failure:enable` (that GUID is `Filtering Platform Connection`),
sets `HKLM\SYSTEM\CurrentControlSet\Control\Lsa\SCENoApplyLegacyAuditPolicy=1`,
and then reads the **Security event log** in
`Common/Security/EventLogAsyncReader.cs`, filtering on instance ids 5156 (allow),
5157 (block), 5152/5150/5151 (drops), 5154/5155 (listen). The one tool whose
entire product is user-mode connection monitoring chose the audit log over the
WFP net-event API. That is a strong signal.

**Verdict: tier 2 as originally scoped does not exist.** There is no user-mode
WFP subscription that yields a per-connection allow feed with a PID.

### 2.4 The Security audit log (5156/5157) — the route that *does* carry a PID

This is the surprise. The audit event carries **more** than the WFP API does.
Event 5156 (`Microsoft-Windows-Security-Auditing`, subcategory *Filtering
Platform Connection*) has this `EventData`:

```
ProcessID, Application, Direction, SourceAddress, SourcePort,
DestAddress, DestPort, Protocol, FilterRTID, LayerName, LayerRTID,
RemoteUserID, RemoteMachineID
```

`ProcessID` is *"hexadecimal Process ID of the process that received the
connection"*; `Application` is *"full path and the name of the executable"* in
`\device\harddiskvolume#` form. So: **PID + image path + full 5-tuple + direction
+ which WFP filter and layer decided it** — from user mode, with no driver.

The costs are severe and must be stated plainly:

- **It requires enabling a system-wide audit policy.** By default *"the
  Success/Failure entries for Filtering Platform Connection aren't
  audited/logged"* and *"By default, auditing for WFP is disabled."* Turning it
  on is a machine-wide security-configuration change made on the user's behalf by
  a VPN client. That is a genuine consent question, not just an implementation
  detail — and WFN ships a `DisableWFN.cmd` precisely because it is a change that
  must be reversible.
- **The volume is catastrophic.** Microsoft's own guidance: *"Events are logged
  for every network connection that is associated with a process, as such the
  volume of events is generally very high."* Field reports cite 500+ events/sec
  on busy hosts overwhelming log pipelines. This floods the Security log — the
  same log an enterprise's EDR and SIEM depend on — and can roll legitimate
  security events out of retention. **We would be degrading the machine's
  security posture to draw a UI.**
- **It is global, not scoped to us.** We cannot ask for only our own traffic.
- **Filter attribution is unreliable on allows.** From the WFP auditing page:
  *"Permitted connections do not always audit the ID of the associated filter.
  The FilterID for TCP will be 0 unless a subset of these filtering conditions
  are used: UserID, AppID, Protocol, Remote Port."*

**Verdict:** technically the strongest no-driver route, and **I recommend against
shipping it on by default.** If it is ever offered it must be an explicit,
clearly-worded, off-by-default opt-in inside Advanced Mode that says what it
changes and offers to change it back — and it should probably wait until someone
has asked for it.

---

## 3. DNS → connection correlation: how we show `github.com`, not `140.82.x.x`

### 3.1 We are the resolver path — this is the easy win

P7 gate F already commits us to owning DNS while the tunnel is up: the plan says
*"Windows resolves per-adapter, so setting the tun's DNS is not enough while other
adapters keep resolvers. Validate with a DNS-leak test; pick and implement NRPT or
a WFP port-53 block scoped to non-tun interfaces."* Whichever we pick, the
outcome is the same: **while connected, plain DNS goes to a resolver we control,
through a tunnel we own.** Every `A`/`AAAA`/`HTTPS`/`SVCB` answer that crosses it
is an IP→name mapping we can record for free, in the service, with no new OS
surface at all.

This is exactly Portmaster's mechanism (§1.3) and it is the correct one for us
because we get it as a side effect of work P7 must do anyway.

### 3.2 The hard cases, all four of which we must render honestly

- **DoH/DoT from the browser.** Chrome and Firefox ship DoH; when active, DNS
  leaves as HTTPS to the browser's own resolver and *"because the queries are
  encrypted, your network cannot inspect or redirect them."* We will see the
  connection and have **no name for it**. Mitigating detail: Firefox's default
  ("Default protection") *disables* DoH when it detects a VPN, parental controls
  or enterprise policy — so in our own tunnel this may partially self-resolve.
  **UNVERIFIED**: whether Firefox's VPN heuristic fires for a wintun adapter, and
  Chrome's behaviour differs.
- **Raw-IP connections.** No DNS ever happened. There is no name to show, ever.
- **Shared CDN IPs.** One IP legitimately maps to many hostnames. Portmaster's
  answer is to store `ResolvedDomains` as a **list** scoped by profile and render
  them joined by `" or "`. Copy that: show all candidates, or show the IP.
- **Cache and TTL.** A connection to an IP resolved 20 minutes ago by a different
  process is a weak attribution. Portmaster scopes the pending-request map by PID
  and expires it in 3 seconds; the durable IP→name map is separate and
  profile-scoped. Two maps, two lifetimes.

### 3.3 `Microsoft-Windows-DNS-Client` ETW — a fallback, not a foundation

Events 3006 (query issued: name, type, options, server list, interface index) and
3008 (query completed: name, type, status, **results**) carry the querying
process. It would let us see names for traffic that bypasses our resolver via the
*system* resolver — but it is blind to DoH by construction (the browser never
calls the DNS client APIs), which is the case we actually needed help with. It
adds an ETW session to solve the case §3.1 already solves.

**Recommendation: do not build this.** If a name is not in our own resolver's
records, show the IP.

---

## 4. Volume, retention, and what our inspector can take

**What we must survive.** A busy desktop makes thousands of connections an hour;
the 5156 literature cites 500+ events/sec at the extreme. Any design that turns
one connection into one UI row, live, will fail.

**What the existing UI is.** From the UI state of record and `ConnectPage.cpp`:
the Activity pane is a fixed-row-height virtualised list (`UrPaneListRowHeight`
36 / `UrPaneRowHeight` 40 / `UrPaneRowTallHeight` 44 and nothing else). The
Inspector is not a list at all — it is a `Children().Clear()` + rebuild of
`InspectorRowsHost` on every refresh, roughly 10–19 `MakePaneKeyValueRow` calls,
and the feed itself **rebuilds on every push** (which is why selection is keyed
by block-action id, never index).

That rebuild-per-push shape is the constraint. It is fine at the current feed
rate and it will not survive an OS-sourced connection firehose: clearing and
re-appending N XAML rows at 100 Hz is not a thing WinUI does gracefully.

**The three things every comparable tool does, and we should:**

1. **Aggregate before display.** Portmaster's UI row is a *connection*, not a
   packet, and repeated flows to the same endpoint collapse into it (their cache
   is keyed by `{protocol, local_address, local_port, remote_address,
   remote_port}`). Our natural aggregation key is coarser and better for a UI:
   **process × destination host/IP × verdict**, with a count and a last-seen. One
   row per (app, destination), not one per flow.
2. **Tier the retention.** Portmaster: 5-second in-memory sweep,
   `DeleteConnsAfterEndedThreshold = 10m` live, separate history DB with
   user-controlled retention and an explicit "clear history" action. We should
   have at minimum an in-service ring with a hard cap, and push the UI a *bounded
   window* — never the whole set.
3. **Page the UI, don't stream it.** WFN's event reader implements
   `IPagedSourceProviderAsync<T>` (AlphaChiTech.Virtualization) — a virtualised,
   paged, async source, because the Security log is far too large to materialise.

**Concrete rate guidance (all UNVERIFIED — none of this was measured):** keep the
service→UI push at the existing cadence (a coalesced snapshot, ~1 Hz or slower),
cap the pushed window at a few hundred aggregated rows, and do all coalescing in
the service. The Inspector's rebuild-on-select is fine because it is driven by a
user action, not by the feed. **Do not** raise the push rate to make the feed feel
live; raise the *quality* of each row instead.

---

## 5. Driver-signing reality — and it got worse since `app/SIGNING.md` was written

`app/SIGNING.md` §2 says: attestation signing, EV cert + Partner Center Hardware
Dev Center, no HLK, *"attestation-signed retail drivers are not distributed via
Windows Update — that's fine, we ship the .sys inside our MSI."* That was
accurate. It is now incomplete in a way that matters.

**The Microsoft doc has been reframed.** `driver-signing-offerings` (ms.date
2026-03-23, updated 2026-04-14) now titles the section **"Attestation signed
drivers for testing scenarios"** and opens: *"For testing purposes only, you can
submit your drivers for attestation signing, which doesn't require HLK
testing."* `code-signing-attestation` carries a matching note that attestation
support *"continues… when you're testing scenarios with the CoDev or Test
Registry Key / Surface SSRK options."*

The enumerated restrictions, verbatim from the offerings page:

- can't be published to Windows Update for retail audiences (WHCP required for
  that);
- works on **Windows 10 Desktop and later only**;
- supports Windows Desktop kernel-mode and user-mode drivers;
- **requires an EV certificate** to submit to Partner Center;
- driver folder names < 40 chars, no special characters, no UNC paths;
- *"When a driver receives attestation signing, it's not Windows Certified…
  there are no assurances about compatibility or functionality"*;
- `Windows Server 2016 and greater doesn't accept attested device and filter
  driver signing submissions` — **server is HLK-only**.

**How to read this.** Nothing in the enumerated restrictions actually blocks our
plan: we target Windows 10+ client, we ship the `.sys` in our own MSI, we do not
need Windows Update, and a WFP callout is not a class of driver that is called
out as ineligible. The `.sys` will load on a retail Windows 11 client. But the
prose framing has shifted from "the lightweight production path" to "for testing
purposes only", and that is a policy signal, not a technical one. Combine it with
the April 2026 update that **removed default trust for cross-signed kernel
drivers** on Windows 11 24H2/25H2/26H1 and Server 2025, and the direction of
travel is unmistakable: Microsoft is tightening kernel-driver trust, and
attestation is the next most likely thing to be narrowed.

**Timeline.** The mechanical path — EV cert procurement, Partner Center hardware
account, first successful CAB submission — is **weeks to a couple of months**,
dominated by EV cert issuance and account vetting, both of which are lead-time
items the existing SIGNING.md correctly says to start in M0. That is not the risk.
The risk is threefold and none of it is about signing throughput:

1. **Policy risk.** Attestation may be narrowed further. Marked as a live risk.
2. **Hardening burden.** `app/driver/PROVENANCE.md` is explicit that this is a
   clean-room driver, so *"the hardening burden is ours since this is clean-room,
   not a battle-tested upstream"* — Driver Verifier, stress and leak testing
   before ship. For a *terminating* ALE callout in the packet path, a bug is a
   bugcheck on a user's machine.
3. **We would be expanding the driver's mandate.** See below.

**And the important repo fact:** we already have a WFP callout driver.
`app/driver/Driver.c` is 505 lines, registers at
`FWPM_LAYER_ALE_BIND_REDIRECT_V4/V6` only, and exposes **only**
`IRP_MJ_DEVICE_CONTROL` — five write-only IOCTLs (`SET_ENABLED`,
`SET_PHYSICAL_ADDRS`, `SET_EXCLUDED_PATHS`, `CLEAR`, `SET_MODE`, `Ioctl.h`). It
already runs `PsSetCreateProcessNotifyRoutineEx` and therefore **already
maintains a PID → image-path map in kernel**. What it does *not* have is any
kernel→user event channel: no `IRP_MJ_READ`, no inverted-call queue, nothing.

So tier 3 is not "write a driver". It is "add an ALE_AUTH_CONNECT inspection
callout and an inverted-call event queue to a driver we already ship" — much
smaller than it looks, but it does move the driver from *"redirects binds"* to
*"observes every connection"*, which is a materially larger security and
stability surface and a materially larger claim to make to users.

---

## 6. The tiers, field by field against the inspector we actually have

The Inspector's current rows, read from `ConnectPage.cpp:1290–1430`: **Host,
Addresses, Matched, Protected, Reason, Override, Packets (total), Bytes (total),
Last decision, Via exit, Flows to this destination, Exit tier, Exit flows, Dial
failures, Exit state, Warning cause, Probe age, Session exit country, Action id**,
plus the three-verdict headline (`blocked` / `tunnelled` / `local`).

### Tier 1 — no driver, no WFP, no audit policy. Ship now.

Mechanism: implement `DeviceLocal::setFlowOwnerLookup` in `urnetworkd`, record a
per-flow side table, expose it to the UI over the existing mTLS RPC.

| Inspector row | Tier-1 source | Accuracy / latency caveat |
|---|---|---|
| **Protocol** *(new)* | `FlowOwnerLookup` arg `protocol` | Exact. Given to us by the SDK. |
| **Source port** *(new)* | `FlowOwnerLookup` arg `source_port` | Exact. |
| **Destination port** *(new)* | `FlowOwnerLookup` arg `destination_port` | Exact. |
| **IP version** *(new)* | `FlowOwnerLookup` arg `version` | Exact. |
| **Process** *(new)* | `GetExtendedTcpTable`/`UdpTable` lookup on the 5-tuple **at the moment of the lookup callback** | Best-effort. This is the strongest form of the polling approach — we are not sampling on a timer, we are querying at the exact instant a flow is created, so the socket is live by construction. Still: UDP sockets can be gone by the time we look; the answer can be "unknown". |
| **Flow started** *(new)* | timestamp of the lookup call | Exact for start. **Not** a duration — nothing tells us when it ended. |
| Host | existing `BlockAction.Hosts` | unchanged |
| Addresses | existing `BlockAction.Ips` | unchanged |
| Protected / Reason / Override | existing | unchanged |
| Packets / Bytes (total) | existing; D6 upgrades to directional via `ContractViewController::getPacketStats()` | still device-wide, per D6 |
| Last decision | existing | unchanged |
| exit join (7 rows) | existing | unchanged |

**The join problem, stated honestly.** `BlockAction` (SDK, `urnetwork_sdk.hpp:830`)
is `{ BlockActionId, Time, Ips, Hosts, MatchedIps, MatchedHosts, Block, Local,
OverrideId, BlockOverride }` — **no ports, no protocol, no owner**. Our flow
side-table is keyed by 5-tuple. Joining them can only be done on **IP**, which
means two processes talking to the same IP collapse into one inspector row. Two
honest options: (a) show the process only when *all* flows to that IP in the
window share one owner, and otherwise show the list (Portmaster's `" or "`
pattern); or (b) surface flows as their own feed alongside the block-action feed.
**(a) is cheaper and fits the existing pane. Recommend (a).**

**Cost:** a `SdkHost`/service-side callback, a bounded map, one new RPC channel.
No new OS surface. No new privileges. No driver.

**Open risk, must be spiked first:** the SDK's calling cadence and thread context
for `setFlowOwnerLookup` are **UNVERIFIED**. If it is called on the packet hot
path, a synchronous `GetExtendedTcpTable` inside it is unacceptable and the
lookup must be made async with a cache. This is the single thing to measure
before committing to the design.

### Tier 2 — user-mode WFP net events. **Does not exist as scoped.**

Per §2.3: no PID in the header, a 128 KB / ~150-event ring, and drops-only as the
documented content. `FwpmConnectionEnum0` is not a rescue either —
`FWPM_CONNECTION0` is `{ connectionId, ipVersion, local/remote address,
providerKey, ipsecTrafficModeType, keyModuleType, mmCrypto, mmPeer, emPeer,
bytesTransferredIn/Out/Total, startSysTime }`: it is **IPsec connection
monitoring** (gated on `FWPM_ENGINE_MONITOR_IPSEC_CONNECTIONS`), has no PID, no
ports, no protocol, and will be empty on a machine not running IPsec.

The nearest real thing is **tier 2′: the Security audit log (5156/5157)**, which
does give PID + image + 5-tuple + direction from user mode. Its cost is a
machine-wide audit-policy change and a flooded Security log (§2.4). **Recommend
against by default; opt-in only, if ever.**

### Tier 3 — extend the callout driver we already have.

Only a callout at `ALE_AUTH_CONNECT_V4/V6` can give, atomically and without a
race:

- **PID at connect time**, from classify metadata (`get_process_id()`), for every
  connection including sub-millisecond UDP;
- **connection end**, via `ALE_ENDPOINT_CLOSURE` / `ALE_RESOURCE_RELEASE` — which
  is the *only* honest source for a **Duration** field;
- **per-connection byte counters**, via `STREAM` / `DATAGRAM_DATA` inspection
  callouts — which is the only honest source for **per-connection in/out bytes**
  (as opposed to the device-wide counters we have);
- inbound connections, which nothing in tiers 1–2 sees properly.

Cost: a new inspection callout set, a kernel→user inverted-call event queue (the
driver has none today), plus Driver Verifier / stress / leak hardening on
clean-room code. Timeline is dominated not by signing (weeks–months, mechanical)
but by hardening and by the decision to widen the driver's mandate from
"redirects binds for split tunnelling" to "observes every connection on this
machine". **That is a product and trust decision before it is an engineering one.**

---

## 7. Recommendation: what to build for Advanced Mode next

**Build tier 1. Do not build tier 2. Do not open tier 3 yet.**

In order:

1. **Spike `setFlowOwnerLookup` first, before designing anything.** Wire it in
   `urnetworkd` (this is `--rpc-only`-compatible: it is `DeviceLocal`, step 4/8,
   no routes) and log the calls. Establish: how often is it called, on which
   thread, is it on a hot path, and is the returned owner string used or
   discarded. **Everything else depends on the answer and none of it is known.**
2. **Land protocol + ports.** These are the cheapest honest wins in the whole
   document — they are function arguments. They delete three entries from the D6
   plan's "Not available, do not fake" list. Do this even if process attribution
   proves hard.
3. **Add best-effort process attribution** via a 5-tuple lookup at callback time,
   with an explicit **"Unknown"** state that is shown, not hidden. Render the
   image path's file name, with the full path available on the row.
4. **Record IP → hostname from our own resolver** as part of P7 gate F, storing a
   **list** per IP (Portmaster's `ResolvedDomains`), not a single winner.
5. **Aggregate in the service** — one row per (process, destination, verdict) with
   a count and last-seen — and keep the push cadence where it is.

Explicitly **not** now: audit policy, ETW sessions, `FwpmNetEventSubscribe`, DNS
ETW, and any driver change.

**Revisit tier 3 only when** (a) tier 1 has shipped and the owner has said the
attribution gap is the thing that hurts, and (b) someone has decided we are
willing to tell users our kernel driver watches every connection. Until then the
driver stays a split-tunnel driver.

---

## 8. Fields we must NOT show — we cannot source these honestly

This project has a standing rule against plausible-looking invented values, and
the inspector's own comments already enforce it ("an inspector that answers
'which exit' with a plausible wrong exit is worse than one that says it does not
know"). The following would all violate it at tier 1.

| Field | Why not |
|---|---|
| **Duration** | Nothing tells us when a connection ended. Only `ALE_ENDPOINT_CLOSURE` / `ALE_RESOURCE_RELEASE` (tier 3) does. The existing code already refuses this and says so — keep that refusal. |
| **Per-connection bytes in / out** | Our counters are device-wide; the SDK's directional counters are device-wide too (the existing comment says exactly this). Per-connection direction needs `STREAM`/`DATAGRAM_DATA` callouts. D6's `getPacketStats()` improves the *device* number, not the *connection* number — do not relabel it. |
| **RTT / latency** | Nothing measures per-connection RTT. Probe results (D6) are per-**exit**, not per-connection. Do not render a probe RTT on a connection row. |
| **ASN / AS owner / org** | Requires a GeoIP/ASN database we do not ship and do not have. Portmaster stores `asn` / `as_owner` because it ships intel data; we do not. |
| **Per-connection country / city / lat-lon** | `ConnectedProviderLocation` (D6) is the **exit's** geo, not the destination's. The existing "Session exit country" label is correct precisely because it says whose country it is. Do not promote it to a per-connection field. |
| **Hostname for a connection with no DNS record of ours** | Raw-IP and DoH connections have no name. Show the IP. Never reverse-DNS to manufacture one — PTR records frequently name the CDN, not the site. |
| **A single hostname for a shared CDN IP** | If our resolver recorded several names for that IP, show several. Picking one is a coin flip presented as a fact. |
| **PID as an identity** | If we only have `appId`/image path (any WFP-sourced route), we have the *image*, not the *instance*. Do not display a PID we inferred. |
| **Inbound connections** | Tier 1 sees flows the SDK routes. Do not render an inbound section that is structurally empty or partial. |
| **A "Blocked by rule X" free-text reason** | Already correctly handled: the SDK has no free-text reason, so the code prints the override kind and id. Do not let an OS-sourced `FilterRTID` become a fake rule name — and note the WFP auditing page warns `FilterID` for TCP allows is often 0. |

---

## 9. Consolidated list of UNVERIFIED claims

Nothing below was tested. Each needs a spike before it is designed against.

1. **`setFlowOwnerLookup` calling cadence, thread context and hot-path status.**
   The whole tier-1 recommendation rests on this. Highest priority.
2. Whether the SDK does anything observable with the returned owner string, and
   whether it is ever echoed back in any getter.
3. The cost of `GetExtendedTcpTable`/`GetExtendedUdpTable` at various rates on
   this machine — no measurement was taken.
4. The owning PID reported for a TCP `TIME_WAIT` row on current Windows 11.
5. Whether `FwpmNetEventSubscribe4` delivers `CLASSIFY_ALLOW` events at all
   without the `Filtering Platform Connection` audit subcategory enabled. The
   documentation strongly implies not; no test was run. (This only matters if
   §2.3's other two findings are somehow wrong — the 128 KB/150-event ring and
   the missing PID are each independently fatal.)
6. Whether `Microsoft-Windows-Winsock-AFD` events carry a parseable remote
   address, and their volume.
7. Whether Firefox's "disable DoH when a VPN is detected" heuristic fires for our
   wintun adapter; Chrome's behaviour is different and also unchecked.
8. The real feed rate the existing Inspector/Activity panes tolerate — the
   rebuild-on-push shape was read from source, not profiled.
9. Whether Microsoft will narrow attestation signing further. Policy risk, not
   testable; monitor `driver-signing-offerings` (currently ms.date 2026-03-23).

---

## Sources

Repo-internal (read-only):
`app/third_party/urnetwork-sdk/amd64/urnetwork_sdk.hpp` (`:830` BlockAction,
`:9568` FlowOwnerLookup, `:10015` DeviceLocal, `:10066` setFlowOwnerLookup),
`app/src/App/ConnectPage.cpp:1240–1430`, `app/src/App/SdkHost.h:194`,
`app/src/Common/Protocol.h`, `app/driver/Driver.c`, `app/driver/Ioctl.h`,
`app/driver/README.md`, `app/driver/PROVENANCE.md`, `app/SIGNING.md`,
`docs/superpowers/reports/2026-08-08-ui-state.md`,
`docs/superpowers/plans/2026-08-08-d6-p7-implementation.md`.

Portmaster (`safing/portmaster`, `development`, pushed 2026-08-07):
`windows_kext/README.md`, `windows_kext/PacketFlow.md`,
`windows_kext/driver/src/callouts.rs`, `windows_kext/driver/src/ale_callouts.rs`,
`windows_kext/kextinterface/info.go`, `service/network/dns.go`,
`service/network/clean.go`, `service/network/iphelper/get.go`,
`service/network/state/system_windows.go`, `service/resolver/ipinfo.go`,
`service/netquery/database.go`, `service/netquery/module_api.go`.

Windows Firewall Notifier (`wokhan/WFN`): `Console/Helpers/InstallHelper.cs`,
`Common/Security/EventLogAsyncReader.cs`.

Microsoft Learn:
- [FWPM_NET_EVENT_HEADER3](https://learn.microsoft.com/en-us/windows/win32/api/fwpmtypes/ns-fwpmtypes-fwpm_net_event_header3)
- [FWPM_CONNECTION0](https://learn.microsoft.com/en-us/windows/win32/api/fwpmtypes/ns-fwpmtypes-fwpm_connection0)
- [FwpmEngineSetOption0](https://learn.microsoft.com/en-us/windows/win32/api/fwpmu/nf-fwpmu-fwpmenginesetoption0)
- [FwpmNetEventEnum0](https://learn.microsoft.com/en-us/windows/win32/api/fwpmu/nf-fwpmu-fwpmneteventenum0)
- [FwpmNetEventSubscribe4](https://learn.microsoft.com/en-us/windows/win32/api/fwpmu/nf-fwpmu-fwpmneteventsubscribe4)
- [WFP Logging](https://learn.microsoft.com/en-us/windows/win32/fwp/logging)
- [WFP Auditing](https://learn.microsoft.com/en-us/windows/win32/fwp/auditing-and-logging)
- [Event 5156](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/auditing/event-5156)
- [Audit Filtering Platform Connection](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/auditing/audit-filtering-platform-connection)
- [GetExtendedTcpTable](https://learn.microsoft.com/en-us/windows/win32/api/iphlpapi/nf-iphlpapi-getextendedtcptable)
- [Driver Signing Options](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)
- [Attestation Sign Windows Drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/code-signing-attestation)
- [Removing trust for the cross-signed driver program](https://techcommunity.microsoft.com/blog/windows-itpro-blog/advancing-windows-driver-security-removing-trust-for-the-cross-signed-driver-pro/4504818)

Other: [Process attribution in network traffic (Digital Operatives)](https://www.digitaloperatives.com/2012/10/16/process-attribution-in-network-traffic/),
[Gary Nebbett, network sniffing on Windows](http://gary-nebbett.blogspot.com/2018/06/gary-gary-2-2132-2018-06-06t153500z.html),
[repnz/etw-providers-docs](https://github.com/repnz/etw-providers-docs),
[Firefox DNS over HTTPS](https://support.mozilla.org/en-US/kb/dns-over-https).
