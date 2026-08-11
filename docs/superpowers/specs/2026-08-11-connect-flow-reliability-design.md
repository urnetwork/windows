# Connect-flow reliability — design

Date: 2026-08-11. Owner-approved approach: root-cause first, three tracks.
Forensic basis: four-lens deep dive over both testers' logs (owner + klets/Legion),
2026-08-11 02:55–03:12 local, builds 1016132620/1016372210/1016413550, plus a
success-baseline capture (10 attempts, 9 hung, 1 green). Defect IDs D1–D11 refer
to that synthesis.

## The problem

Connect attempts hang on yellow "connecting to providers" on both test machines,
first connects and reconnects equally; disconnect+reconnect recovers. The hang is
NOT provider evaluation being slow — it is the SDK's own control plane (JWT
refresh, window enumerate, contract setup, DoH) timing out while the tunnel's
routes + WFP policy are in force. Proof: those same calls complete in ~300ms the
instant disconnect reverts routes; one attempt flipped CONNECTED 2.6s AFTER
disconnect was pressed (friend), and the owner's grid flashed CONNECTED +0.4s
after his disconnect click, twice. One green attempt (9.3s, textbook) proves the
escape path works when the race is won: this is a capture hole, not a wall.

A second, independent defect makes the hole invisible: the connect window has no
outcome deadline and no failure diagnosis. EvaluationFailed transitions log only
at V(1)/V(2) verbosity, terminal states are deleted from the monitor map, the
create-client mint loop retries at 1s forever, and the UI renders climbing yellow
dots indefinitely.

## Track 1 — close the capture hole (D1 root cause)

1. **Audit every outbound dial site** in connect@beta/custom-server and the sdk
   used by the SERVICE process during Connecting/Connected: platform API HTTP +
   WSS, DoH warm (9.9.9.9 i/o timeout was observed captured), provider transport
   dials (TCP/UDP/QUIC/WebRTC/STUN), cping probes. For each: which escape
   mechanism carries it past the tun default route and the WFP policy — egress
   interface binding (step [2/8] "egress bind v4=ifN") or a WFP permit — and
   whether any path can dial UNBOUND (fresh sockets after a route change, v6,
   bootstrap before egress discovery, reconnect dials inside long-lived clients).
2. **Fix**: route every control dial through the bound dialer. Prefer binding
   over new WFP permits; any new permit must be scoped so user traffic cannot
   ride it (the leak-prevention layer and kill switch must not weaken; Armed ⊂
   Connected invariant pinned in selftest stays).
3. **Instrument**: at Connecting/Connected, log each control dial's local bind
   address + interface + target + path tag at default verbosity, so the
   checkpoint test proves the fix on both machines from logs alone.
4. **Selftest**: pin "every dial site resolves to the bound dialer" statically
   where the code structure allows.

## Track 2 — window honesty (D1 visibility + retry defense-in-depth)

1. sdk/connect: unconditional (V0) single-line logs for evaluation failure
   transitions (channel-create EvaluationFailed, ping-ack timeout,
   zero-providers) with reasons.
2. WindowStatus carries a machine-readable stall reason (enum: platform
   unreachable / providers unresponsive / rate-limited / auth failing /
   evaluating) derived from the "verdicts held with no restore" signal and the
   dominant recent failure class.
3. Outcome deadline: zero Added after 45s ⇒ one automatic silent window rebuild
   (mimics the manual disconnect+reconnect that always worked); zero Added 45s
   after that ⇒ surface failure state in the UI with the reason and a Retry
   action. No infinite yellow, ever.
4. App renders the reason line under the grid while yellow ("Contacting the
   platform…", "Providers not responding — retrying…", and the failure state).

## Track 3 — UI hardening batch

- **D2 phantom green**: gesture-generation gate — once disconnect is issued,
  window-monitor events from the old session generation must not drive the grid.
  Same generation tag hardens the D10 stale-monitor seam (#40).
- **D4 app hang**: the service-dropped path must do zero blocking work on the
  XAML thread (no synchronous RPC waits in the presentation-close path); Windows
  killed the app 3× for this, 4–5s after each daemon death.
- **D3 tray-quit crash (0xc000027b)**: drain/observe or detach all pending WinRT
  async completions before DispatcherQueue teardown on quit; quit with failed
  session-start chains pending must not crash.
- **D8 auto-start**: owner decision — tunnel starts ONLY on an explicit Connect
  gesture. Remove auto-start on app-launch "resume" and on login/space-switch.
  Reattach to an already-running tunnel is unchanged (that is not a start).
  Every start_tunnel logs its reason; the space-switch path currently logs none.

## Out of scope (tracked, not in this change)

- D6 contract-wait floor (5–15s "ok=true c=false s(00000000-…)") — question for
  SDK owners; affects time-to-green, not correctness.
- D7 beta-test.net app-level 401s (balance/settings) — needs server-side answer;
  may be a custom-server surface gap.
- D11 receive-side contract verification failures — server/platform side.
- #39 native ntdll crash — all five faults were on build 1016132620; watching
  for recurrence on ≥1016413550 with LocalDumps armed.

## Checkpoint test (both testers, one build)

1. Connect 5× in a row, disconnect between: expect green ≤ ~10s each, or a
   visible reason + retry — never silent yellow > 90s.
2. Logs show every control dial bound to the physical interface during
   Connecting.
3. Disconnect never flips the grid green afterward.
4. Kill the service mid-connect: app stays responsive, reattaches when the
   service returns.
5. Tray-quit immediately after a failed connect: no crash.
6. App restart and login never start the tunnel by themselves.
