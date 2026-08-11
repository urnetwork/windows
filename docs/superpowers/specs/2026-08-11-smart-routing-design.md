# Smart routing — learned, class-aware exit selection

Date: 2026-08-11. Status: owner-directed design, pending owner review of this document.
Research basis: probes/multi-exit audit (live telemetry, both testers), DPI feasibility
study (incl. a local nDPI compile spike), gopacket lineage investigation, DWLC-vs-bandit
literature review (19 cited sources), platform constraints research, and a build-seams
audit of sdk/connect/android/apple/windows. Owner decisions incorporated: auto-learn +
manual overrides; separate ndpi library file; 5 traffic classes; learning on by default
with opt-out, reset, 90-day default retention settable to unlimited; every heavy piece
optional per platform with legacy compatibility.

## 1. Goal and principles

Evolve exit selection from evidence-based-but-uniform to class-aware and self-improving:
bandwidth-, latency-, and program-aware placement that trains locally and gets better the
longer it runs — without touching what already works.

- **Learning optimizes; verdicts protect.** The existing safety machinery (probes
  qualify, traffic convicts, quarantine, shared-fate holds, verdict budget) remains the
  SOLE removal authority. The learner ranks healthy exits only; it can never keep traffic
  on an exit the evidence has condemned, never bypass the verdict budget.
- **Deterministic first, learner thin.** Our telemetry shows exits degrade to conviction
  within minutes and slots turn over ~14 min on average — the regime where bandit regret
  is worst and fresh measurement pays most. So placement is a deterministic score over
  live metrics; the learner is a small additive bonus, shipped dark until logs prove it.
- **Everything heavy is optional.** cgo classification and per-app attribution are
  platform-gated capability seams (nil interface = today's behavior, cost one branch).
  Pure-Go layers ship everywhere. Zero-value-off on every new knob; old clients interop
  with the same servers by construction (client-local feature, no protocol change).

## 2. Classification — what is this flow

Five classes: latency-sensitive, streaming, bulk, browsing, background.

Precedence: manual per-app override > nDPI verdict > exe/app default > port+SNI heuristic.

- **nDPI (full tier)**: shipped as a SEPARATE library file (ndpi.dll / libndpi.so /
  libndpi.dylib) — clean LGPL-3.0 separation, and file-presence doubles as the runtime
  full/light switch on every platform. Our own thin binding (~12 externs, opaque pointers
  only, 2-3 one-line C shims; no struct mirroring — that is what killed every prior Go
  binding) against a PINNED nDPI release tag, in a new package in the sdk/cgo module
  (separate Go module — gomobile builds cannot see it by construction).
- **Budget**: first 8 packets per flow, both directions, then classify-or-giveup and free
  (1.3 KB/flow while under classification, ≤512 flows concurrently, queue 1024). One
  dedicated worker goroutine owning one detection module (~14 MB once, ~1-2 µs/packet
  measured). Hot path pays one nil-check when idle; when classifying, a mandatory bounded
  copy (≤1600 B) — payload slices alias pooled buffers, queueing a live slice is
  use-after-recycle (review-blocking invariant). Queue full → drop the job, count it,
  flow keeps its heuristic class. nDPI is never called from the pump thread.
- **Light tier** (no library present / iOS): pure-Go classifier using connect's existing
  SNI sniffer, DNS reverse index, and ports. Same 5 classes, lower confidence.
- **App attribution**: implement the existing FlowOwnerLookup seam per platform
  (async-with-default — it must never run synchronously inside the pump's C→Go call):
  Windows service = TCP/UDP owner table → exe; Linux = sock_diag netlink → /proc;
  Android = getConnectionOwnerUid (API 29+, once per flow + 5-tuple cache; measure
  binder cost under flow storms before finalizing); macOS = none at launch (needs a
  future companion filter extension); iOS = none (no API exists).
- **gopacket**: NOT a production dependency (stays test-only per the policy comment in
  connect). Housekeeping: retarget the 6 test files from github.com/google/gopacket
  v1.1.19 (2020 code; carries unfixed remotely-triggerable decoder panics, e.g.
  CVE-2026-65819-class bugs never advisoried there) to github.com/gopacket/gopacket
  ≥ v1.7.1 (all High decoder-panic advisories fixed + regression-tested). If its TLS
  ClientHello decode is ever used outside tests: feed exact-capacity slices or a copy —
  its parser re-slices data[:cap(data)]. QUIC classification comes from nDPI, not
  gopacket (no lineage has a QUIC layer).

## 3. Placement — deterministic scored selection

At the existing single placement site (one shared code path on all platforms; sticky
affinity continues to govern already-placed flows):

- Composite score per (class, exit) from: probe RTT, goodput EWMA, stall/receive
  evidence, jitter; per-class metric weights (latency-sensitive weights RTT/jitter,
  streaming weights sustained throughput + stability, bulk weights throughput, etc.).
- **Incumbent hysteresis**: a challenger must beat the incumbent by >10% composite score
  (Fortinet-style A = R/(1+L/100)) — kills score-noise churn.
- **N-of-M demotion**: rank demotion requires 2-of-3 consecutive bad intervals, never one
  sample (Cisco EAAR multiplier idiom). Convictions are unaffected — they are
  evidence-based, not score-based.
- **Anti-herding tie-break**: when the top exits are within the hysteresis margin, place
  the new flow on the LESS-LOADED one (power-of-two-choices) — directly fixes the
  measured 37-flows-on-one-exit accretion.
- Score reacts within one metric interval (1-5 s); failover remains the verdict layer's
  job and is faster still.

## 4. Learning — thin, dark-first, background-funded

- **Per-(class, exit) Sliding-Window UCB** (disjoint tables, ~5-min window; explicitly
  NOT LinUCB — at 5 classes × ≤18 exits, feature models add complexity without coverage).
  The UCB optimism bonus is ADDITIVE to the deterministic score and keeps under-sampled
  exits measured.
- **Exploration budget**: exploration placements only ever use background-class flows,
  hard-capped at ≤1-in-8 background placements, suppressed during shared-fate holds and
  above the flow soft cap. Interactive traffic never pays the tuition.
- **Dark launch**: the learner logs its would-be choices (counterfactuals) with zero
  behavioral effect first; it goes live only when logged uplift is positive (Phase 0
  reward instrumentation defines the metric: class-normalized goodput + stall-free time,
  counting only backlogged flows to avoid demand confounding).
- **Cold providers / probation**: optimism-under-uncertainty replaces an ad-hoc probation
  flag — cold or bench-returned exits carry a decaying uncertainty bonus and earn rank
  through background traffic. The same prior biases recruitment ranking in the 2x
  evaluation pool.
- **Persistence**: coarse per-provider-IDENTITY priors only (score EWMA, conviction
  count, last-seen; TTL per §7) — never raw UCB windows or posteriors (stale-on-load at
  our churn, and a poisoning surface for adversarial providers).

## 5. Upgrades to the existing machinery (from the same research)

1. **Quarantine flap damping** (observed: same exits benched 4-5x, bench migrate always
   movable=0): RFC 2439-style escalation — re-conviction bench 60 s → 120 s → 240 s with
   penalty half-life ~10-15 min; release-on-receive-progress preserved.
2. **Re-entry ramp**: a released exit returns at reduced score weight and ramps to full
   eligibility (fast to leave, slow to return) — implemented as a temporary score
   penalty, not a state-machine change.
3. **Removal census fix rides along** (task #51): self-closing channels emit a
   reason=channel-closed removal line where exitLost fires.
4. Shared-fate holds and the verdict budget are independently validated by the same
   literature (rate-limited reaction to shared signals) — unchanged.

## 6. Platform tiers and guards

| Platform | nDPI | Attribution | Pure-Go scoring+learner | Gate |
|---|---|---|---|---|
| Windows | Full — ndpi.dll, LoadLibrary | Full (service, owner table) | On | DLL presence + settings; nil seam |
| Linux | Full — libndpi.so, dlopen | Full (sock_diag) | On | .so presence + settings; nil seam |
| macOS | Full — libndpi.dylib in app bundle, dlopen from the NE extension (macOS NE has NO memory limit, unlike iOS) | Off at launch (needs future filter extension; packet tunnel cannot attribute) | On | build tag + presence; nil seam |
| Android | Optional — per-ABI libndpi.so in the APP's jniLibs (the .aar cannot carry it), lazy dlopen; absent → light | getConnectionOwnerUid (API ≥29) | On | dlopen probe → nil seam; API-level check |
| iOS | **None** — LGPL-3.0 vs sealed/signed bundles is effectively prohibited (FSF position; VLC precedent), and ~14 MB vs the ~50 MB NE jetsam cap is marginal anyway | None (no API in a packet tunnel; NEAppRule is MDM-only) | On (light classifier) | cgo package invisible to gomobile by module boundary; nothing to exclude |

Verified guard mechanics (build-seams audit):
- sdk/cgo is a separate Go module (`sdk/cgo/go.mod`); gomobile binds package
  `github.com/urnetwork/sdk` only — a new `sdk/cgo/ndpi` package is invisible to Android
  and Apple builds by construction. No build tags needed for anything under sdk/cgo.
- The classifier/attributor seams copy the FlowOwnerLookup pattern exactly: nil-by-default
  field, gomobile-safe basic types, atomic pointer, one-branch nil cost on the egress
  path, re-applied on every multi rebuild.
- The tier knob follows the ReliabilitySettings pattern: zero value = off/legacy; runtime
  swappable; logged in the session settings banner.
- **Trap to respect**: the Android aar build greps exported sources against an allowlist —
  any new setter/type in package sdk must use gomobile-exportable basic types (as
  FlowOwnerLookup does) or the Android build fails.
- **Second-DLL touch list** (Windows packaging, all name files explicitly and must change
  in lockstep): sdk/cgo/Makefile build_windows recipes (note: `; \` chaining means a
  failed sub-build does not fail make — add explicit failure), build-sdk.ps1, the CI
  "Verify the SDK actually produced DLLs" step (extend or a missing ndpi.dll ships
  silently), fetch-deps.ps1, App/Service .vcxproj copy steps, installer Package.wxs,
  package-portable.ps1 $required list. Runtime LoadLibrary avoids a second .def/.lib.
- Apple/Android CI: provably zero changes (apple fork has no CI; android CI never
  references sdk/cgo and gomobile cannot compile it).

## 7. Persistence, privacy, controls

- New dot-file(s) under the existing per-network-space `.by` dir (precedent:
  `.doh_server_scores`), keyed by provider identity. Follows automatically on every
  platform via the app-supplied storage root.
- **On by default; opt-out toggle; retention default 90 days, user-settable to unlimited;
  reset button.** Reset = delete the dot-file (and Logout already wipes the whole `.by`
  dir — existing hook). All data is local only, never uploaded.
- Advanced Mode panel: per-app table (learned class + override), tier switch
  (full/light/off), retention setting, reset, and a live "why this exit" explainer for
  the current placements (score components + any UCB bonus).

## 8. Phasing (each phase independently shippable)

- **Phase 0 — gates, no behavior change**: (a) prove the nDPI ARM64 windows cross-build
  (its configure hard-rejects aarch64-mingw — small vendored Makefile over src/lib +
  third_party with pregenerated ndpi_define.h; amd64 uses upstream's own proven autotools
  cross); (b) reward instrumentation logging per-(class, exit) outcomes to answer "do
  per-class rankings actually diverge" (the go/no-go for Phase 3); (c) fix or exclude the
  dead DNS-through-exit probe metric before any score consumes it.
- **Phase 1 — deterministic scorer** (+hysteresis, N-of-M, anti-herding tie-break,
  quarantine damping+ramp, recruitment prior, priors dot-file). Pure Go, all platforms.
  gopacket test retarget rides along. Windows FlowOwnerLookup service implementation
  (async) lands here — exe-class placement works with zero DPI.
- **Phase 2 — classification**: nDPI binding + worker + 5-class mapping on Windows
  (separate ndpi.dll); light classifier everywhere; per-class scoring live.
- **Phase 3 — learner**: SW-UCB dark → live on proven uplift. Android optional .so +
  UID attribution when the Android fork next takes a feature train.
- **Phase 4 — declined by default**: LinUCB/TS upgrade only if context cardinality grows
  far beyond 5 classes; macOS attribution extension as its own future project.

## 9. Testing

- Scorer/hysteresis/tie-break/damping: table-driven with deterministic goldens (SW-UCB is
  deterministic given the window — golden-testable; another reason not TS).
- Binding: compile-time sizeof assert against the pinned nDPI tag; the spike harness kept
  as a per-upgrade smoke test; classification correctness via captured-flow fixtures.
- Seams: nil-classifier/nil-attributor paths pinned (mobile equivalence); aar validate
  allowlist check in CI already enforces exportability.
- Live checkpoint greps: classifier verdict lines, placement "because" lines, dark-launch
  counterfactual log, exploration budget counter never exceeding 1-in-8.

## 10. Out of scope

Split tunneling (separate feature, blocked on driver attestation signing — task #23/#24);
iOS nDPI (revisit only with an ntop commercial license — one email if ever wanted);
macOS per-app attribution extension; server-side changes of any kind (none needed).

## 11. Known risks / honest caveats

ARM64 nDPI build is the one unproven leg (Phase 0 gates everything on it). The iOS 50 MB
figure is Apple-forum-stated, not contractual. Android attribution binder cost under flow
storms is unmeasured (Phase 3 pre-task). macOS App Store review of a dlopened LGPL dylib
is untested — direct distribution is the safe path. nDPI ABI churns every minor release —
survivable only via the opaque-pointer + pinned-tag discipline; every prior Go binding
died of struct mirroring. Sticky-affinity flow-cap exceedance (owner 37, friend 1324 vs
cap 16) still lacks per-placement attribution to confirm/refute the known TOCTOU race —
Phase 0 instrumentation covers it (ties to task S2/#15).
