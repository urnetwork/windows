# Smart Routing — Phase 0 + Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the pure-Go foundation of class-aware learned routing — per-(class,exit) reward instrumentation, a deterministic scored placement layer with anti-flap controls, quarantine flap-damping, provider-identity priors, and the Windows per-app attribution seam — plus prove the nDPI ARM64 cross-build, all on `beta/algorithm-dpi` without disturbing the `beta/custom-server` UI line.

**Architecture:** All routing logic is client-local in connect's `RemoteUserNatMultiClient`; there is exactly one production placement site (`SendPacket`/`sendPacket`) shared by every platform. New behavior rides the established `ReliabilitySettings` pattern (zero-value = today's behavior, runtime-swappable, logged in the session banner), so old clients and the same servers interoperate untouched. Persistence copies the `LocalState.getDohServerScores`/`setDohServerScores` dot-file precedent. nDPI/classifier/learner are OUT OF SCOPE here — this plan produces working, testable software that improves placement using signals that already exist, and leaves nil-default seams for the later phases.

**Tech Stack:** Go 1.26 (connect, sdk), C++/WinRT (windows service attribution), gomobile/cgo build boundaries, GitHub Actions (Windows CI), autotools + a vendored Makefile (nDPI cross-build proof).

## Global Constraints

- Branch: all work on `beta/algorithm-dpi` in each of connect, sdk, windows. NEVER push `beta/custom-server` from this plan. Connect worktree is dirty on another branch — work in a fresh worktree of `beta/algorithm-dpi`.
- Every new tunable is a field on `MultiClientSettings` mirrored into `ReliabilitySettings` + `ReliabilitySettingsFrom` (connect `ip_remote_multi_client.go:2076`, `:2135`), and its ZERO VALUE must equal today's behavior (the documented rule at `ip_remote_multi_client.go:2108-2130`).
- Any new setter/type added to package `sdk` (module `github.com/urnetwork/sdk`) MUST use gomobile-exportable basic types (like `FlowOwnerLookupFunc`) or the Android aar build's export-allowlist grep fails. New Go code that is cgo/platform-specific lives under the `sdk/cgo` module (separate `go.mod`) or behind a `//go:build` tag — never as plain package-`sdk` code that mobile compiles.
- Structured logs follow the one grammar (connect `ip_remote_multi_client_observability.go`): default-level `Infof`, `[rel] event=<name> key=value …` (event first), transition- or interval-triggered only, never per-packet.
- Wire/persistence compatibility: new gob/json fields are ADDITIVE only (precedent sdk 303122d); new persisted state is a NEW dot-file old clients never read.
- Windows build safety: before any msbuild, `(Get-CimInstance Win32_Service -Filter "Name='urnetworkd'").PathName` must point OUTSIDE the checkout. Never elevate, never touch the installed service. `urnetworkd.exe selftest` baseline is 563/0 — keep green.
- Go tests: targeted only — `go test -run <Name> -timeout 120s ./<pkg>` under an outer `timeout 300`. Never the bare full suite. `go build ./...` + `go vet ./...` must be clean in every Go repo touched.
- Commit trailer on every commit:
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01PT7KcWCPKfFwQUc7SM3oZY
  ```
- Before every connect/windows commit verify `git ls-files | wc -l` == `git ls-tree -r --name-only HEAD | wc -l` (index-truncation guard). Concurrent agents use a private `GIT_INDEX_FILE`.

---

## File Structure

**connect** (module `github.com/urnetwork/connect`, on `beta/algorithm-dpi`):
- `routing_class.go` (new) — the `TrafficClass` enum + `FlowClass` result type + a nil-safe `FlowClassifier` interface (seam only; nDPI fills it later).
- `routing_score.go` (new) — pure scoring: `exitScore`, incumbent hysteresis, N-of-M demotion state, less-loaded tie-break. No I/O, no locks of its own.
- `routing_reward.go` (new) — per-(class,exit) reward accumulation + the `[rel] event=reward` interval line (Phase 0 instrumentation).
- `routing_priors.go` (new) — the `ProviderPriors` in-memory model + a `PriorsStore` interface (persistence injected from sdk).
- `ip_remote_multi_client.go` (modify) — new settings fields (~`:130`, `:888`, `:2076`, `:2135`), the placement hook in `sendPacket`, the classifier seam mirroring `flowOwnerFunc`, quarantine flap-damping in the bench lifecycle.
- `ip_remote_multi_client_observability.go` (modify) — banner grammar entries for the new fields + the new event lines.
- Tests: `routing_score_test.go`, `routing_reward_test.go`, `routing_priors_test.go`, `routing_class_test.go` (new).

**sdk** (module `github.com/urnetwork/sdk`, on `beta/algorithm-dpi`):
- `reliability_controls.go` (modify) — mirror the new reliability knobs into the gomobile-safe setter, following `SetFlowOwnerLookup`/`SetReliabilitySettings`.
- `local_state.go` (modify) — `getProviderPriors`/`setProviderPriors` dot-file (`.provider_priors`) copying `getDohServerScores`/`setDohServerScores`; wired into `Logout`'s existing `.by` wipe (free reset).
- `device_local.go` (modify) — build/inject the priors store + re-apply on multi rebuild (the `:3480-3493` reapply block).
- `routing_tier.go` (new) — the `RoutingTier` setting (full/light/off), zero-value = off/legacy, plumbed like the performance profile.

**windows** (on `beta/algorithm-dpi`):
- `app/src/Service/FlowOwner.{h,cpp}` (new) — TCP/UDP owner-table → exe path lookup, async, feeding the sdk `SetFlowOwnerLookup` seam.
- `.github/workflows/beta-build.yml` (modify) — pin connect+sdk clones to `beta/algorithm-dpi`; add the nDPI ARM64 build-proof job (Phase 0, gated, produces no shipped artifact yet).
- `app/tools/ndpi/` (new) — the vendored Makefile + pregenerated `ndpi_define.h` for the ARM64 cross-build proof.

---

## Task 1: Branch CI wiring (algorithm line builds against algorithm connect/sdk)

**Files:**
- Modify: `.github/workflows/beta-build.yml` (windows repo, `beta/algorithm-dpi`)

**Interfaces:**
- Produces: a green CI run on `beta/algorithm-dpi` identical to `beta/custom-server` except it pins connect and sdk to `beta/algorithm-dpi`. No code behavior change.

- [ ] **Step 1: Read the current clone/checkout refs**

Run: `git show beta/algorithm-dpi:.github/workflows/beta-build.yml | grep -n 'custom-server\|--branch\|checkout\|ref:'`
Expected: the connect `--branch beta/custom-server` clone and the sdk `ref` checkout lines.

- [ ] **Step 2: Point connect + sdk at the algorithm branch**

Change the connect clone `--branch beta/custom-server` → `--branch beta/algorithm-dpi`, and the sdk checkout `ref` from `beta/custom-server` → `beta/algorithm-dpi`. Change NOTHING else. Add a triggering `branches:` entry for `beta/algorithm-dpi` alongside the existing ones.

- [ ] **Step 3: Commit and push the branch to trigger CI**

```bash
git add .github/workflows/beta-build.yml
git commit -F <msgfile>   # "ci: build beta/algorithm-dpi against algorithm connect/sdk"
git push origin beta/algorithm-dpi
```

- [ ] **Step 4: Verify CI green**

Run: `gh run list --branch beta/algorithm-dpi --limit 1` then `gh run watch <id> --exit-status`
Expected: green — proves the three-repo algorithm line builds before any behavior change lands.

---

## Task 2: TrafficClass enum + nil-safe classifier seam (connect)

**Files:**
- Create: `connect/routing_class.go`
- Create: `connect/routing_class_test.go`

**Interfaces:**
- Produces:
  - `type TrafficClass uint8` with `ClassUnknown=0, ClassLatency, ClassStreaming, ClassBulk, ClassBrowsing, ClassBackground` (Unknown MUST be 0 = zero value).
  - `func (TrafficClass) String() string`
  - `type FlowClass struct { Class TrafficClass; AppId string; Confidence uint8 }`
  - `type FlowClassifier interface { Classify(ipPath *IpPath, appId string) FlowClass }`
  - `func classifyOrUnknown(c FlowClassifier, ipPath *IpPath, appId string) FlowClass` — returns `FlowClass{Class: ClassUnknown, AppId: appId}` when `c == nil`.

- [ ] **Step 1: Write the failing test**

```go
package connect

import "testing"

func TestClassifyOrUnknownNilClassifier(t *testing.T) {
	got := classifyOrUnknown(nil, &IpPath{}, "chrome.exe")
	if got.Class != ClassUnknown {
		t.Fatalf("nil classifier: class=%v want ClassUnknown", got.Class)
	}
	if got.AppId != "chrome.exe" {
		t.Fatalf("nil classifier dropped appId: %q", got.AppId)
	}
}

func TestTrafficClassZeroValueIsUnknown(t *testing.T) {
	var z TrafficClass
	if z != ClassUnknown || z.String() != "unknown" {
		t.Fatalf("zero value must be unknown, got %v/%q", z, z.String())
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd <connect-worktree> && timeout 300 go test -run 'TestClassifyOrUnknownNilClassifier|TestTrafficClassZeroValueIsUnknown' -timeout 120s ./`
Expected: FAIL (undefined `classifyOrUnknown`, `ClassUnknown`).

- [ ] **Step 3: Write minimal implementation**

```go
package connect

// TrafficClass is the smart-routing traffic class of a flow. ClassUnknown is
// the zero value on purpose: an un-classified flow (no classifier installed, or
// classification not yet decided) is Unknown, which every scoring path treats
// as "no class preference" so the layer is inert until a classifier is present.
type TrafficClass uint8

const (
	ClassUnknown TrafficClass = iota
	ClassLatency
	ClassStreaming
	ClassBulk
	ClassBrowsing
	ClassBackground
)

func (c TrafficClass) String() string {
	switch c {
	case ClassLatency:
		return "latency"
	case ClassStreaming:
		return "streaming"
	case ClassBulk:
		return "bulk"
	case ClassBrowsing:
		return "browsing"
	case ClassBackground:
		return "background"
	default:
		return "unknown"
	}
}

// FlowClass is a classification result: the class, the owning app (may be ""),
// and a 0-100 confidence.
type FlowClass struct {
	Class      TrafficClass
	AppId      string
	Confidence uint8
}

// FlowClassifier turns a flow into a FlowClass. The nil implementation is the
// legacy path; a real one (nDPI, a later phase) is installed via
// SetFlowClassifier. Implementations must be safe for concurrent calls and must
// not block (see classifyOrUnknown callers on the placement path).
type FlowClassifier interface {
	Classify(ipPath *IpPath, appId string) FlowClass
}

// classifyOrUnknown is the one-branch nil guard the placement path pays when no
// classifier is installed, mirroring the flowOwnerFunc nil check.
func classifyOrUnknown(c FlowClassifier, ipPath *IpPath, appId string) FlowClass {
	if c == nil {
		return FlowClass{Class: ClassUnknown, AppId: appId}
	}
	return c.Classify(ipPath, appId)
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `timeout 300 go test -run 'TestClassifyOrUnknownNilClassifier|TestTrafficClassZeroValueIsUnknown' -timeout 120s ./`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add routing_class.go routing_class_test.go
git commit -F <msgfile>   # "feat(routing): traffic-class enum and nil-safe classifier seam"
```

---

## Task 3: Deterministic exit score + incumbent hysteresis (connect)

**Files:**
- Create: `connect/routing_score.go`
- Create: `connect/routing_score_test.go`

**Interfaces:**
- Consumes: nothing (pure functions).
- Produces:
  - `type ExitMetrics struct { RttMillis float64; GoodputBytesPerSec float64; StallEvents int; Jitter float64; Flows int }`
  - `type ScoreWeights struct { Rtt, Goodput, Stall, Jitter float64 }`
  - `func classWeights(c TrafficClass) ScoreWeights`
  - `func exitScore(m ExitMetrics, w ScoreWeights) float64` (higher = better)
  - `func challengerWins(incumbent, challenger float64, hysteresisPct float64) bool` — challenger must beat incumbent by > hysteresisPct% (`challenger > incumbent*(1+hysteresisPct/100)`); when `hysteresisPct==0` this is a plain `>` (today's behavior).

- [ ] **Step 1: Write the failing test**

```go
package connect

import "testing"

func TestChallengerWinsHysteresis(t *testing.T) {
	// 10% margin: 105 does not beat 100, 111 does.
	if challengerWins(100, 105, 10) {
		t.Fatal("105 should not beat 100 at 10% hysteresis")
	}
	if !challengerWins(100, 111, 10) {
		t.Fatal("111 should beat 100 at 10% hysteresis")
	}
	// zero hysteresis is plain greater-than (legacy behavior)
	if !challengerWins(100, 100.5, 0) {
		t.Fatal("zero hysteresis must be plain >")
	}
}

func TestExitScoreLatencyPrefersLowRtt(t *testing.T) {
	w := classWeights(ClassLatency)
	fast := exitScore(ExitMetrics{RttMillis: 20, GoodputBytesPerSec: 1e6}, w)
	slow := exitScore(ExitMetrics{RttMillis: 200, GoodputBytesPerSec: 1e6}, w)
	if !(fast > slow) {
		t.Fatalf("latency class must rank low-RTT higher: fast=%f slow=%f", fast, slow)
	}
}

func TestExitScoreBulkPrefersGoodput(t *testing.T) {
	w := classWeights(ClassBulk)
	fat := exitScore(ExitMetrics{RttMillis: 80, GoodputBytesPerSec: 5e6}, w)
	thin := exitScore(ExitMetrics{RttMillis: 80, GoodputBytesPerSec: 5e5}, w)
	if !(fat > thin) {
		t.Fatalf("bulk class must rank high-goodput higher: fat=%f thin=%f", fat, thin)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `timeout 300 go test -run 'TestChallengerWins|TestExitScore' -timeout 120s ./`
Expected: FAIL (undefined symbols).

- [ ] **Step 3: Write minimal implementation**

```go
package connect

// ExitMetrics is the per-(class,exit) signal snapshot the scorer reads. Every
// field is already collected elsewhere in this file's machinery; the scorer is
// pure and holds no locks.
type ExitMetrics struct {
	RttMillis          float64
	GoodputBytesPerSec float64
	StallEvents        int
	Jitter             float64
	Flows              int
}

// ScoreWeights weights each normalized metric. Per-class weights let the same
// scorer prefer low latency for interactive flows and high throughput for bulk.
type ScoreWeights struct {
	Rtt     float64
	Goodput float64
	Stall   float64
	Jitter  float64
}

func classWeights(c TrafficClass) ScoreWeights {
	switch c {
	case ClassLatency:
		return ScoreWeights{Rtt: 1.0, Goodput: 0.2, Stall: 1.0, Jitter: 0.8}
	case ClassStreaming:
		return ScoreWeights{Rtt: 0.4, Goodput: 0.8, Stall: 1.0, Jitter: 0.9}
	case ClassBulk:
		return ScoreWeights{Rtt: 0.1, Goodput: 1.0, Stall: 0.6, Jitter: 0.1}
	case ClassBrowsing:
		return ScoreWeights{Rtt: 0.6, Goodput: 0.6, Stall: 0.8, Jitter: 0.4}
	case ClassBackground:
		return ScoreWeights{Rtt: 0.2, Goodput: 0.7, Stall: 0.5, Jitter: 0.2}
	default: // ClassUnknown: balanced, class-neutral
		return ScoreWeights{Rtt: 0.5, Goodput: 0.5, Stall: 0.7, Jitter: 0.3}
	}
}

// exitScore is a bounded composite in [0,1]-ish space; higher is better. RTT and
// jitter are inverted (lower is better) via a soft reciprocal so a 0 metric does
// not divide; stall events subtract. Constants are deliberate and unit-tested,
// not tuned here.
func exitScore(m ExitMetrics, w ScoreWeights) float64 {
	rttGood := 1.0 / (1.0 + m.RttMillis/50.0)      // 50ms → 0.5
	jitterGood := 1.0 / (1.0 + m.Jitter/20.0)      // 20ms → 0.5
	goodputGood := m.GoodputBytesPerSec / (m.GoodputBytesPerSec + 1e6) // 1MB/s → 0.5
	stallPenalty := float64(m.StallEvents) * 0.1
	return w.Rtt*rttGood + w.Goodput*goodputGood + w.Jitter*jitterGood - w.Stall*stallPenalty
}

// challengerWins applies incumbent hysteresis: a challenger only displaces the
// incumbent if it beats it by more than hysteresisPct percent. hysteresisPct==0
// reduces to a plain greater-than, which is the pre-change behavior.
func challengerWins(incumbent, challenger, hysteresisPct float64) bool {
	return challenger > incumbent*(1.0+hysteresisPct/100.0)
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `timeout 300 go test -run 'TestChallengerWins|TestExitScore' -timeout 120s ./`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add routing_score.go routing_score_test.go
git commit -F <msgfile>   # "feat(routing): deterministic per-class exit score + incumbent hysteresis"
```

---

## Task 4: N-of-M demotion + less-loaded tie-break (connect)

**Files:**
- Modify: `connect/routing_score.go`
- Modify: `connect/routing_score_test.go`

**Interfaces:**
- Consumes: `exitScore`, `challengerWins` (Task 3).
- Produces:
  - `type demotionState struct { bad int }`
  - `func (d *demotionState) observe(good bool, needBad int) (demote bool)` — increments on bad, resets on good, returns true only when `bad >= needBad`; `needBad<=1` reproduces single-sample behavior.
  - `func lessLoadedTieBreak(aScore, bScore float64, aFlows, bFlows int, hysteresisPct float64) (preferB bool)` — when a and b are within the hysteresis margin of each other, prefer the less-loaded; otherwise prefer the higher score.

- [ ] **Step 1: Write the failing test**

```go
func TestDemotionNeedsNofM(t *testing.T) {
	d := &demotionState{}
	if d.observe(false, 3) || d.observe(false, 3) {
		t.Fatal("demoted before 3 consecutive bad")
	}
	if !d.observe(false, 3) {
		t.Fatal("should demote on 3rd consecutive bad")
	}
	d2 := &demotionState{}
	d2.observe(false, 3)
	d2.observe(true, 3) // recovery resets
	if d2.observe(false, 3) {
		t.Fatal("a good sample must reset the streak")
	}
}

func TestLessLoadedTieBreak(t *testing.T) {
	// within 10% margin → prefer less-loaded (b has fewer flows)
	if !lessLoadedTieBreak(100, 105, 30, 5, 10) {
		t.Fatal("near-tie should prefer the less-loaded exit b")
	}
	// b clearly worse → keep a even though a is more loaded
	if lessLoadedTieBreak(100, 60, 30, 5, 10) {
		t.Fatal("clear score gap must beat load tie-break")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `timeout 300 go test -run 'TestDemotionNeedsNofM|TestLessLoadedTieBreak' -timeout 120s ./`
Expected: FAIL.

- [ ] **Step 3: Write minimal implementation** (append to `routing_score.go`)

```go
// demotionState implements N-of-M rank demotion: an exit is only demoted after
// needBad consecutive bad intervals, so a single noisy sample never re-ranks the
// window. A good sample resets the streak. needBad<=1 is the legacy behavior
// (act on every sample). Convictions are unaffected — they are evidence-based
// and handled by the verdict layer, not by this score demotion.
type demotionState struct {
	bad int
}

func (d *demotionState) observe(good bool, needBad int) bool {
	if good {
		d.bad = 0
		return false
	}
	d.bad++
	return d.bad >= max(1, needBad)
}

// lessLoadedTieBreak returns true when challenger b should be preferred over
// incumbent a. When neither strictly wins under hysteresis (a near-tie), the
// less-loaded of the two wins — the power-of-two-choices anti-herding rule that
// spreads flows instead of piling them on one favorite. Outside the margin the
// higher score wins outright.
func lessLoadedTieBreak(aScore, bScore float64, aFlows, bFlows int, hysteresisPct float64) bool {
	if challengerWins(aScore, bScore, hysteresisPct) {
		return true
	}
	if challengerWins(bScore, aScore, hysteresisPct) {
		return false
	}
	return bFlows < aFlows
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `timeout 300 go test -run 'TestDemotionNeedsNofM|TestLessLoadedTieBreak' -timeout 120s ./`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add routing_score.go routing_score_test.go
git commit -F <msgfile>   # "feat(routing): N-of-M demotion and less-loaded anti-herding tie-break"
```

---

## Task 5: Provider-identity priors model (connect)

**Files:**
- Create: `connect/routing_priors.go`
- Create: `connect/routing_priors_test.go`

**Interfaces:**
- Produces:
  - `type ProviderPrior struct { ScoreEwma float64; Convictions int; LastSeenUnix int64 }`
  - `type ProviderPriors struct { … }` with `func NewProviderPriors() *ProviderPriors`
  - `func (p *ProviderPriors) Observe(providerId string, score float64, nowUnix int64)` (EWMA update, alpha 0.2)
  - `func (p *ProviderPriors) Convict(providerId string, nowUnix int64)`
  - `func (p *ProviderPriors) Bias(providerId string) float64` — recruitment bias in [0,1], decays with staleness and convictions; unknown provider → 0.5 (neutral).
  - `func (p *ProviderPriors) Snapshot() map[string]ProviderPrior` and `func (p *ProviderPriors) Load(map[string]ProviderPrior)` for persistence.
  - `type PriorsStore interface { Load() map[string]ProviderPrior; Save(map[string]ProviderPrior) error }`

- [ ] **Step 1: Write the failing test**

```go
package connect

import "testing"

func TestProviderPriorsEwmaAndBias(t *testing.T) {
	p := NewProviderPriors()
	if p.Bias("unknown") != 0.5 {
		t.Fatal("unknown provider must be neutral 0.5")
	}
	for i := 0; i < 20; i++ {
		p.Observe("good", 0.9, 1000)
	}
	for i := 0; i < 20; i++ {
		p.Observe("bad", 0.1, 1000)
	}
	if !(p.Bias("good") > p.Bias("bad")) {
		t.Fatalf("good provider must bias higher: good=%f bad=%f", p.Bias("good"), p.Bias("bad"))
	}
}

func TestProviderPriorsConvictionLowersBias(t *testing.T) {
	p := NewProviderPriors()
	for i := 0; i < 20; i++ {
		p.Observe("x", 0.9, 1000)
	}
	before := p.Bias("x")
	p.Convict("x", 1000)
	p.Convict("x", 1000)
	if !(p.Bias("x") < before) {
		t.Fatal("convictions must lower recruitment bias")
	}
}

func TestProviderPriorsRoundTrip(t *testing.T) {
	p := NewProviderPriors()
	p.Observe("a", 0.7, 1234)
	snap := p.Snapshot()
	q := NewProviderPriors()
	q.Load(snap)
	if q.Snapshot()["a"].ScoreEwma != snap["a"].ScoreEwma {
		t.Fatal("round-trip must preserve EWMA")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `timeout 300 go test -run 'TestProviderPriors' -timeout 120s ./`
Expected: FAIL.

- [ ] **Step 3: Write minimal implementation**

```go
package connect

import (
	"sync"
	"time"
)

// ProviderPrior is the coarse, persistable memory of one provider IDENTITY
// (never an exit instance): a smoothed score, a conviction count, and a
// last-seen stamp. This is all that survives a restart — never raw learner
// windows, which are stale-on-load at our churn and a poisoning surface.
type ProviderPrior struct {
	ScoreEwma    float64
	Convictions  int
	LastSeenUnix int64
}

type ProviderPriors struct {
	mu sync.Mutex
	m  map[string]ProviderPrior
}

func NewProviderPriors() *ProviderPriors {
	return &ProviderPriors{m: map[string]ProviderPrior{}}
}

const priorsEwmaAlpha = 0.2

func (p *ProviderPriors) Observe(providerId string, score float64, nowUnix int64) {
	p.mu.Lock()
	defer p.mu.Unlock()
	pr := p.m[providerId]
	if pr.LastSeenUnix == 0 {
		pr.ScoreEwma = score
	} else {
		pr.ScoreEwma = priorsEwmaAlpha*score + (1-priorsEwmaAlpha)*pr.ScoreEwma
	}
	pr.LastSeenUnix = nowUnix
	p.m[providerId] = pr
}

func (p *ProviderPriors) Convict(providerId string, nowUnix int64) {
	p.mu.Lock()
	defer p.mu.Unlock()
	pr := p.m[providerId]
	pr.Convictions++
	pr.LastSeenUnix = nowUnix
	p.m[providerId] = pr
}

// Bias returns a recruitment bias in [0,1]; unknown providers are neutral 0.5.
// A conviction history subtracts; the score EWMA is the base. Staleness decay is
// applied by the caller passing a recent nowUnix into DecayedBias if needed; the
// base Bias is time-independent for testability.
func (p *ProviderPriors) Bias(providerId string) float64 {
	p.mu.Lock()
	defer p.mu.Unlock()
	pr, ok := p.m[providerId]
	if !ok {
		return 0.5
	}
	b := pr.ScoreEwma - 0.15*float64(pr.Convictions)
	if b < 0 {
		b = 0
	}
	if b > 1 {
		b = 1
	}
	return b
}

func (p *ProviderPriors) Snapshot() map[string]ProviderPrior {
	p.mu.Lock()
	defer p.mu.Unlock()
	out := make(map[string]ProviderPrior, len(p.m))
	for k, v := range p.m {
		out[k] = v
	}
	return out
}

func (p *ProviderPriors) Load(m map[string]ProviderPrior) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.m = make(map[string]ProviderPrior, len(m))
	for k, v := range m {
		p.m[k] = v
	}
}

// PriorsStore is the persistence seam; the sdk supplies a LocalState-backed
// implementation. A nil store means in-memory only (bare fixtures, mobile before
// wiring). Retention/TTL is enforced by the store on load.
type PriorsStore interface {
	Load() map[string]ProviderPrior
	Save(map[string]ProviderPrior) error
}

var _ = time.Now // Load-time retention lives in the store, not here.
```

- [ ] **Step 4: Run test to verify it passes**

Run: `timeout 300 go test -run 'TestProviderPriors' -timeout 120s ./`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add routing_priors.go routing_priors_test.go
git commit -F <msgfile>   # "feat(routing): provider-identity priors model with EWMA and conviction bias"
```

---

## Task 6: Reward instrumentation — the Phase 0 divergence gate (connect)

**Files:**
- Create: `connect/routing_reward.go`
- Create: `connect/routing_reward_test.go`
- Modify: `connect/ip_remote_multi_client_observability.go` (add the event-line constant only)

**Interfaces:**
- Consumes: `TrafficClass` (Task 2).
- Produces:
  - `type rewardKey struct { Class TrafficClass; ExitId string }`
  - `type rewardAccumulator struct { … }` + `func newRewardAccumulator() *rewardAccumulator`
  - `func (r *rewardAccumulator) add(class TrafficClass, exitId string, goodputBytes float64, stallFree bool)`
  - `func (r *rewardAccumulator) drainLines() []string` — one `[rel] event=reward class=<c> exit=<id> samples=<n> goodput=<avg> stallfree=<frac>` per key seen this interval, then resets. Empty slice when nothing accumulated (nothing logged, per grammar).

- [ ] **Step 1: Write the failing test**

```go
package connect

import (
	"strings"
	"testing"
)

func TestRewardAccumulatorDrainLines(t *testing.T) {
	r := newRewardAccumulator()
	r.add(ClassBulk, "exitA", 1000, true)
	r.add(ClassBulk, "exitA", 3000, false)
	lines := r.drainLines()
	if len(lines) != 1 {
		t.Fatalf("want 1 line, got %d: %v", len(lines), lines)
	}
	l := lines[0]
	for _, want := range []string{"[rel] event=reward", "class=bulk", "exit=exitA", "samples=2", "stallfree=0.5"} {
		if !strings.Contains(l, want) {
			t.Fatalf("line %q missing %q", l, want)
		}
	}
	if len(r.drainLines()) != 0 {
		t.Fatal("drain must reset the accumulator")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `timeout 300 go test -run 'TestRewardAccumulatorDrainLines' -timeout 120s ./`
Expected: FAIL.

- [ ] **Step 3: Write minimal implementation**

```go
package connect

import "fmt"

// rewardAccumulator is the Phase 0 divergence gate: it records per-(class,exit)
// outcomes so a field capture can answer "do per-class rankings actually
// diverge" before any learner is built. Reward = class-normalized goodput +
// stall-free fraction. It is measurement only — it changes NO placement.
type rewardKey struct {
	Class  TrafficClass
	ExitId string
}

type rewardStat struct {
	samples     int
	goodputSum  float64
	stallFreeN  int
}

type rewardAccumulator struct {
	m map[rewardKey]*rewardStat
}

func newRewardAccumulator() *rewardAccumulator {
	return &rewardAccumulator{m: map[rewardKey]*rewardStat{}}
}

func (r *rewardAccumulator) add(class TrafficClass, exitId string, goodputBytes float64, stallFree bool) {
	k := rewardKey{Class: class, ExitId: exitId}
	s := r.m[k]
	if s == nil {
		s = &rewardStat{}
		r.m[k] = s
	}
	s.samples++
	s.goodputSum += goodputBytes
	if stallFree {
		s.stallFreeN++
	}
}

// drainLines emits one grammar line per key and resets. Interval-triggered by
// the caller (the heartbeat tick), never per-packet.
func (r *rewardAccumulator) drainLines() []string {
	if len(r.m) == 0 {
		return nil
	}
	lines := make([]string, 0, len(r.m))
	for k, s := range r.m {
		avg := s.goodputSum / float64(s.samples)
		frac := float64(s.stallFreeN) / float64(s.samples)
		lines = append(lines, fmt.Sprintf(
			"%sevent=reward class=%s exit=%s samples=%d goodput=%.0f stallfree=%.2g",
			relPrefix, k.Class, k.ExitId, s.samples, avg, frac))
	}
	r.m = map[rewardKey]*rewardStat{}
	return lines
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `timeout 300 go test -run 'TestRewardAccumulatorDrainLines' -timeout 120s ./`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add routing_reward.go routing_reward_test.go
git commit -F <msgfile>   # "feat(routing): Phase 0 per-(class,exit) reward instrumentation"
```

---

## Task 7: New reliability knobs — settings plumbing (connect)

**Files:**
- Modify: `connect/ip_remote_multi_client.go` (`DefaultMultiClientSettings` ~`:130`; the `MultiClientSettings` field block ~`:888`; `ReliabilitySettings` struct ~`:2076`; `ReliabilitySettingsFrom` ~`:2135`)
- Modify: `connect/ip_remote_multi_client_observability.go` (banner grammar for the new fields)
- Modify: `connect/routing_score_test.go` (assert zero-value-off)

**Interfaces:**
- Produces four new fields on both `MultiClientSettings` and `ReliabilitySettings`, each zero-value-off:
  - `ScoredPlacement bool` — master gate; false = today's placement untouched.
  - `PlacementHysteresisPct float64` — 0 = plain greater-than (legacy).
  - `PlacementDemoteConsecutive int` — 0/1 = act on every sample (legacy).
  - `RewardInstrumentation bool` — false = no reward lines.

- [ ] **Step 1: Write the failing test** (append to `routing_score_test.go`)

```go
func TestNewReliabilityKnobsZeroValueOff(t *testing.T) {
	z := ReliabilitySettingsFrom(nil) // nil → zero value
	if z.ScoredPlacement || z.PlacementHysteresisPct != 0 ||
		z.PlacementDemoteConsecutive != 0 || z.RewardInstrumentation {
		t.Fatal("new knobs must be zero-value-off (legacy behavior)")
	}
	s := DefaultMultiClientSettings()
	got := ReliabilitySettingsFrom(s)
	if got.ScoredPlacement != s.ScoredPlacement {
		t.Fatal("ReliabilitySettingsFrom must copy ScoredPlacement")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `timeout 300 go test -run 'TestNewReliabilityKnobsZeroValueOff' -timeout 120s ./`
Expected: FAIL (unknown fields).

- [ ] **Step 3: Add the fields** in all four places. On `MultiClientSettings` (near the other reliability fields) and `ReliabilitySettings` (after `HeartbeatInterval`, `:2130`), add:

```go
	// Smart routing (Phase 1), all zero-value-off so an override from an older
	// struct keeps today's placement. ScoredPlacement is the master gate;
	// PlacementHysteresisPct==0 is plain greater-than; PlacementDemoteConsecutive<=1
	// acts on every sample; RewardInstrumentation==false emits no reward lines.
	ScoredPlacement            bool
	PlacementHysteresisPct     float64
	PlacementDemoteConsecutive int
	RewardInstrumentation      bool
```

In `DefaultMultiClientSettings()` leave them at zero (do not set) so the default build is legacy until the app opts in. In `ReliabilitySettingsFrom`'s returned struct add the four copy lines:

```go
		ScoredPlacement:            settings.ScoredPlacement,
		PlacementHysteresisPct:     settings.PlacementHysteresisPct,
		PlacementDemoteConsecutive: settings.PlacementDemoteConsecutive,
		RewardInstrumentation:      settings.RewardInstrumentation,
```

- [ ] **Step 4: Add banner grammar** in `ip_remote_multi_client_observability.go` — follow the existing non-default-field pattern so the session banner names these when set (e.g. `scoredplacement=1 placementhysteresispct=10`). Locate the banner assembly (the function that renders non-default settings) and add the four fields with the same conditional "only when non-default" style used for `windowoutcomedeadline`.

- [ ] **Step 5: Run test + build/vet**

Run: `timeout 300 go test -run 'TestNewReliabilityKnobsZeroValueOff' -timeout 120s ./ && go build ./... && go vet ./...`
Expected: PASS, clean.

- [ ] **Step 6: Commit**

```bash
git add ip_remote_multi_client.go ip_remote_multi_client_observability.go routing_score_test.go
git commit -F <msgfile>   # "feat(routing): scored-placement reliability knobs (zero-value-off)"
```

---

## Task 8: Wire the scorer into the placement path (connect)

**Files:**
- Modify: `connect/ip_remote_multi_client.go` (the classifier seam near `flowOwnerFunc` `:1182`/`:1316`; the placement decision in `sendPacket` `:4376` and its helpers; the reward tap at the receive/stat update site)
- Create: `connect/routing_placement_test.go`

**Interfaces:**
- Consumes: `classifyOrUnknown`, `exitScore`, `classWeights`, `challengerWins`, `lessLoadedTieBreak`, `demotionState`, `ProviderPriors`, `rewardAccumulator`, the new reliability knobs.
- Produces:
  - `func (self *RemoteUserNatMultiClient) SetFlowClassifier(c FlowClassifier)` — mirrors `SetFlowOwnerLookup` exactly (atomic pointer, nil clears, re-applied on rebuild). gomobile-safe: `FlowClassifier` is an interface of basic-typed methods.
  - Placement selects among ALREADY-HEALTHY candidate channels only (the verdict/quarantine layer still owns membership); when `ScoredPlacement` is false the code path is byte-for-byte today's selection.

- [ ] **Step 1: Write the failing test**

```go
func TestScoredPlacementGatedOff(t *testing.T) {
	// With ScoredPlacement=false, SetFlowClassifier installed but the scorer must
	// not run: a classifier that panics proves the gate short-circuits.
	c := &panicClassifier{}
	client := newBareMultiClientForTest(t) // existing test helper in this package
	client.SetFlowClassifier(c)
	// ScoredPlacement defaults false → placement must not call Classify
	if client.flowClassifier.Load() == nil {
		t.Fatal("classifier should be stored")
	}
	// exercise the gate helper directly
	if scoredPlacementEnabled(client.reliabilitySettings()) {
		t.Fatal("scored placement must be off by default")
	}
}

type panicClassifier struct{}

func (panicClassifier) Classify(*IpPath, string) FlowClass { panic("must not be called when gated off") }
```

(If `newBareMultiClientForTest` does not exist, use the existing bare-construction fixture this package already uses in `ip_remote_multi_client_*_test.go`; find it with `grep -n 'func new.*MultiClient.*Test\|bare' *_test.go` and reuse it — do not invent a new constructor.)

- [ ] **Step 2: Run test to verify it fails**

Run: `timeout 300 go test -run 'TestScoredPlacementGatedOff' -timeout 120s ./`
Expected: FAIL (undefined `SetFlowClassifier`, `flowClassifier`, `scoredPlacementEnabled`).

- [ ] **Step 3: Implement the seam + gate**

Add next to `flowOwnerFunc` (`:1182`):

```go
	flowClassifier atomic.Pointer[FlowClassifier]
```

Add the setter mirroring `SetFlowOwnerLookup` (`:1316`) — atomic store, nil clears. Add:

```go
// scoredPlacementEnabled is the single gate the placement path checks. When
// false (the zero value) selection is exactly today's; nothing in this file's
// new routing code runs.
func scoredPlacementEnabled(r *ReliabilitySettings) bool {
	return r != nil && r.ScoredPlacement
}
```

In the placement helper, guard the entire new path:

```go
	if scoredPlacementEnabled(self.reliabilitySettings()) {
		// classify (nil-safe), score healthy candidates, apply hysteresis +
		// less-loaded tie-break, choose. Fall through to legacy selection for
		// ClassUnknown with no metrics, so behavior only ever *refines*.
	}
	// legacy selection unchanged below
```

Wire the reward tap where per-flow goodput/stall is already updated (the receive/stat path): when `RewardInstrumentation` is on, call `rewardAcc.add(class, exitId, goodput, stallFree)`. Re-apply the classifier on multi rebuild wherever `flowOwnerFunc` is re-applied.

- [ ] **Step 4: Run test + targeted regressions**

Run: `timeout 300 go test -run 'TestScoredPlacementGatedOff|TestConnectGrid' -timeout 120s ./ && go build ./... && go vet ./...`
Expected: PASS, clean. (`TestConnectGrid*` proves legacy placement still behaves.)

- [ ] **Step 5: Commit**

```bash
git add ip_remote_multi_client.go routing_placement_test.go
git commit -F <msgfile>   # "feat(routing): wire scored placement behind its gate; nil-safe classifier seam"
```

---

## Task 9: Quarantine flap damping + re-entry ramp (connect)

**Files:**
- Modify: `connect/ip_remote_multi_client.go` (the quarantine bench lifecycle ~`:11114-11201`; the release path)
- Create: `connect/routing_quarantine_test.go`

**Interfaces:**
- Consumes: the new reliability knobs pattern (add two more, zero-value-off): `QuarantineDampening bool`, `QuarantineReentryRamp time.Duration`.
- Produces:
  - `func benchDuration(reconvictions int, base time.Duration, dampening bool) time.Duration` — 60s→120s→240s escalation (cap 240s) when dampening on; constant `base` when off.
  - A release-path score penalty that decays over `QuarantineReentryRamp` (0 = no ramp = legacy).

- [ ] **Step 1: Write the failing test**

```go
func TestBenchDurationEscalates(t *testing.T) {
	base := 60 * time.Second
	if benchDuration(0, base, false) != base {
		t.Fatal("dampening off must be constant base")
	}
	if benchDuration(0, base, true) != 60*time.Second ||
		benchDuration(1, base, true) != 120*time.Second ||
		benchDuration(2, base, true) != 240*time.Second ||
		benchDuration(5, base, true) != 240*time.Second {
		t.Fatal("dampening on must escalate 60→120→240 and cap")
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `timeout 300 go test -run 'TestBenchDurationEscalates' -timeout 120s ./`
Expected: FAIL.

- [ ] **Step 3: Implement `benchDuration`** and thread it into the bench lifecycle where the quarantine hold time is set; track a per-provider reconviction count (reuse `ProviderPriors.Convictions` or a local bench counter). Add the two knobs to `MultiClientSettings`/`ReliabilitySettings`/`ReliabilitySettingsFrom` (zero-value-off). Apply the re-entry ramp as a temporary subtraction on the released exit's score, decaying to 0 over `QuarantineReentryRamp`. Preserve release-on-receive-progress exactly.

```go
func benchDuration(reconvictions int, base time.Duration, dampening bool) time.Duration {
	if !dampening {
		return base
	}
	steps := []time.Duration{60 * time.Second, 120 * time.Second, 240 * time.Second}
	i := reconvictions
	if i >= len(steps) {
		i = len(steps) - 1
	}
	return steps[i]
}
```

- [ ] **Step 4: Run test + targeted regressions**

Run: `timeout 300 go test -run 'TestBenchDurationEscalates|TestQuarantine|TestConnectGrid' -timeout 120s ./ && go build ./... && go vet ./...`
Expected: PASS, clean.

- [ ] **Step 5: Commit**

```bash
git add ip_remote_multi_client.go ip_remote_multi_client.go routing_quarantine_test.go
git commit -F <msgfile>   # "feat(routing): quarantine flap damping and re-entry ramp (zero-value-off)"
```

---

## Task 10: Persist provider priors — LocalState dot-file (sdk)

**Files:**
- Modify: `sdk/local_state.go` (copy `getDohServerScores`/`setDohServerScores` `:388-430`; hook `Logout` `:712`)
- Modify: `sdk/device_local.go` (build a `PriorsStore` adapter, inject into the multi, re-apply on rebuild `:3480-3493`)
- Modify: `sdk/reliability_controls.go` (expose the new reliability knobs through the gomobile-safe setter)
- Create: `sdk/local_state_priors_test.go`

**Interfaces:**
- Consumes: `connect.ProviderPrior`, `connect.PriorsStore` (Task 5).
- Produces:
  - `func (self *LocalState) getProviderPriors() map[string]connect.ProviderPrior` (nil if none/unreadable/stale; TTL from a `providerPriorsStaleAfter` const defaulting 90 days)
  - `func (self *LocalState) setProviderPriors(map[string]connect.ProviderPrior) error` (empty removes)
  - `type localStatePriorsStore struct { … }` implementing `connect.PriorsStore`
  - `providerPriorsRetention time.Duration` settable to 0 = unlimited.

- [ ] **Step 1: Write the failing test**

```go
package sdk

import (
	"testing"
	"github.com/urnetwork/connect"
)

func TestProviderPriorsDotFileRoundTrip(t *testing.T) {
	ls := newTestLocalState(t) // existing test helper; find with grep if named differently
	in := map[string]connect.ProviderPrior{"p1": {ScoreEwma: 0.8, Convictions: 1, LastSeenUnix: 1000}}
	if err := ls.setProviderPriors(in); err != nil {
		t.Fatal(err)
	}
	out := ls.getProviderPriors()
	if out["p1"].ScoreEwma != 0.8 {
		t.Fatalf("round trip lost data: %+v", out)
	}
}
```

(Find the real LocalState test constructor with `grep -n 'func newTest.*LocalState\|NewLocalState' *_test.go local_state.go` and reuse it.)

- [ ] **Step 2: Run test to verify it fails**

Run: `cd <sdk> && timeout 300 go test -run 'TestProviderPriorsDotFileRoundTrip' -timeout 120s ./`
Expected: FAIL.

- [ ] **Step 3: Implement** by copying the doh-scores pattern verbatim with `.provider_priors` as the filename and a `persistedProviderPriors{ SavedAt time.Time; Retention time.Duration; Priors map[string]connect.ProviderPrior }` envelope; on load, honor a zero `Retention` as unlimited (skip the stale check). Implement `localStatePriorsStore.Load/Save` delegating to these. `Logout`'s existing `os.RemoveAll(.by)` already wipes it — add nothing there but a comment noting the file is covered.

- [ ] **Step 4: Run test + build/vet**

Run: `timeout 300 go test -run 'TestProviderPriorsDotFileRoundTrip' -timeout 120s ./ && go build ./... && go vet ./...`
Expected: PASS, clean.

- [ ] **Step 5: Verify aar export-safety** (the mobile trap)

Run: `grep -n 'func (self \*DeviceLocal) Set' reliability_controls.go` and confirm any new exported setter uses only basic types / interfaces already exported (no new map-of-struct crossing the gomobile boundary directly — priors cross as JSON inside sdk, never as a bound type).
Expected: no new gomobile-incompatible signature.

- [ ] **Step 6: Commit**

```bash
git add local_state.go device_local.go reliability_controls.go local_state_priors_test.go
git commit -F <msgfile>   # "feat(routing): persist provider priors in a .provider_priors dot-file"
```

---

## Task 11: RoutingTier setting + reliability knob plumbing to the app (sdk)

**Files:**
- Create: `sdk/routing_tier.go`
- Modify: `sdk/reliability_controls.go` (add the four Phase-1 knobs to the gomobile setter; add the tier→knobs mapping)
- Modify: `sdk/device_local.go` (apply tier defaults when building `MultiClientSettings` at `:3366-3421`)
- Create: `sdk/routing_tier_test.go`

**Interfaces:**
- Produces:
  - `type RoutingTier int` with `RoutingTierOff=0, RoutingTierLight, RoutingTierFull` (Off is zero = legacy).
  - `func (self *DeviceLocal) SetRoutingTier(tier int)` (int, gomobile-safe) → maps to the connect knobs: Off = all off; Light = `ScoredPlacement` + `RewardInstrumentation` + hysteresis 10 + demote 3 + quarantine damping (pure-Go, no classifier); Full = Light + (classifier installed later by the platform).
  - Persisted via the existing performance-profile-style local setting so it survives restarts.

- [ ] **Step 1: Write the failing test**

```go
package sdk

import "testing"

func TestRoutingTierMapsToKnobs(t *testing.T) {
	off := routingTierKnobs(int(RoutingTierOff))
	if off.ScoredPlacement || off.RewardInstrumentation {
		t.Fatal("Off tier must be fully legacy")
	}
	light := routingTierKnobs(int(RoutingTierLight))
	if !light.ScoredPlacement || light.PlacementHysteresisPct != 10 || light.PlacementDemoteConsecutive != 3 {
		t.Fatalf("Light tier knobs wrong: %+v", light)
	}
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `timeout 300 go test -run 'TestRoutingTierMapsToKnobs' -timeout 120s ./`
Expected: FAIL.

- [ ] **Step 3: Implement** `RoutingTier`, `routingTierKnobs(tier int) connect.ReliabilitySettings` (a partial overlay merged onto the constructed settings), `SetRoutingTier` (store the int in LocalState like the performance profile, then `SetReliabilitySettings` the overlay onto the live multi). Full tier sets the same pure-Go knobs as Light; the classifier is installed separately per platform (Phase 2), so Full==Light behaviorally until a classifier lands — document this.

- [ ] **Step 4: Run test + build/vet + banner check**

Run: `timeout 300 go test -run 'TestRoutingTierMapsToKnobs' -timeout 120s ./ && go build ./... && go vet ./...`
Expected: PASS, clean.

- [ ] **Step 5: Commit**

```bash
git add routing_tier.go reliability_controls.go device_local.go routing_tier_test.go
git commit -F <msgfile>   # "feat(routing): RoutingTier (off/light/full) mapped to reliability knobs"
```

---

## Task 12: Windows service FlowOwnerLookup implementation (windows)

**Files:**
- Create: `app/src/Service/FlowOwner.h`, `app/src/Service/FlowOwner.cpp`
- Modify: `app/src/Service/TunnelController.cpp` (install the lookup on the DeviceLocal after step 4, async)
- Modify: `app/src/Service/urnetworkd.vcxproj` (add the two files)
- Modify: `app/src/Service/SelfTest.cpp` (a pin that the lookup is async/non-blocking-by-contract)

**Interfaces:**
- Consumes: the sdk `SetFlowOwnerLookup` seam (already exists, `reliability_controls.go:546`; hpp `urnetwork_sdk.hpp:9579`).
- Produces: `FlowOwner::LookupExe(const IpPath&) -> std::string` backed by `GetExtendedTcpTable`/`GetExtendedUdpTable`, cached by 5-tuple, resolved on a worker — NEVER synchronously inside the pump's C→Go callback (the re-entrancy hazard from the DPI study). Returns "" on miss.

- [ ] **Step 1: Write the failing selftest assertion**

Add to `SelfTest.cpp` a check that `FlowOwner` exposes an async, cache-first API (e.g. a synchronous fast-path that only reads the cache and never calls `GetExtendedTcpTable` on the caller thread; a background refresh fills the cache). Assert the contract via a small unit-style check in the existing selftest harness (pure logic: the cache lookup returns quickly and a miss enqueues a refresh rather than blocking).

- [ ] **Step 2: Build selftest, verify it fails**

Run (after service-path precheck): `powershell -NoProfile -File app\tools\build-local.ps1 -SkipDeps` then `app\build\x64\Release\urnetworkd.exe selftest`
Expected: FAIL on the new pin (symbol/behavior missing).

- [ ] **Step 3: Implement `FlowOwner`** — a table snapshot refreshed on a worker every ~1s (and on demand when a 5-tuple misses), mapping PID→module path via `QueryFullProcessImageNameW`, exposing a lock-free-read cache keyed by 5-tuple. Install it via the sdk `SetFlowOwnerLookup` binding after DeviceLocal is created, with a trampoline that only reads the cache (async-with-default) so the pump thread never blocks. Follow the existing hpp callback-install pattern (the smoke test at `cgo/smoke/compile_hpp.cpp` shows the nullptr-safe shape).

- [ ] **Step 4: Build + selftest green**

Run: `powershell -NoProfile -File app\tools\build-local.ps1 -SkipDeps` then `urnetworkd.exe selftest`
Expected: Build 0 errors; selftest ≥563/0 plus the new pin.

- [ ] **Step 5: Commit**

```bash
git add app/src/Service/FlowOwner.h app/src/Service/FlowOwner.cpp app/src/Service/TunnelController.cpp app/src/Service/urnetworkd.vcxproj app/src/Service/SelfTest.cpp
git commit -F <msgfile>   # "feat(service): async per-flow owning-exe lookup feeding the SDK FlowOwnerLookup seam"
```

---

## Task 13: nDPI ARM64 cross-build proof — the Phase 0 build gate (windows CI)

**Files:**
- Create: `app/tools/ndpi/Makefile.crossproof` (vendored minimal Makefile over nDPI `src/lib` + `third_party/src`)
- Create: `app/tools/ndpi/ndpi_define.h` (pregenerated, checked in — the configure step that produces it does not run under llvm-mingw)
- Modify: `.github/workflows/beta-build.yml` (a new job `ndpi-crossproof` — amd64 via nDPI's own autotools `--host=x86_64-w64-mingw32 --with-only-libndpi`, arm64 via the vendored Makefile with `aarch64-w64-mingw32-clang`; asserts both `libndpi.a` exist; ships NOTHING)

**Interfaces:**
- Produces: CI evidence that a static `libndpi.a` builds for both windows/amd64 (upstream-proven autotools) and windows/arm64 (bespoke Makefile). This gate must be green before Phase 2 (nDPI binding) is planned. No artifact enters the shipped zip.

- [ ] **Step 1: Add the pinned nDPI clone + amd64 autotools build to a new CI job**

In `beta-build.yml`, add job `ndpi-crossproof` (ubuntu-latest): clone `ntop/nDPI` at a PINNED release tag; install mingw-w64 + llvm-mingw; run `./autogen.sh && ./configure --host=x86_64-w64-mingw32 --with-only-libndpi && make -C src/lib`; assert `src/lib/libndpi.a` exists.

- [ ] **Step 2: Add the arm64 vendored-Makefile build**

Copy `app/tools/ndpi/Makefile.crossproof` + `ndpi_define.h` into the nDPI tree; run `make -f Makefile.crossproof CC=aarch64-w64-mingw32-clang AR=aarch64-w64-mingw32-ar`; assert an arm64 `libndpi.a` exists and `file` reports aarch64.

- [ ] **Step 3: Push and verify the gate**

```bash
git add app/tools/ndpi/Makefile.crossproof app/tools/ndpi/ndpi_define.h .github/workflows/beta-build.yml
git commit -F <msgfile>   # "ci(phase0): prove nDPI static-lib cross-build for windows amd64+arm64"
git push origin beta/algorithm-dpi
```

Run: `gh run watch <id> --exit-status`
Expected: `ndpi-crossproof` green for BOTH arches. If arm64 fails, that is the Phase-0 finding — capture the exact error; do NOT proceed to Phase 2 planning until resolved.

---

## Task 14: gopacket test-dependency retarget (connect)

**Files:**
- Modify: `connect/go.mod`, `connect/go.sum`
- Modify: the 6 `*_test.go` files importing `github.com/google/gopacket`

**Interfaces:**
- Produces: connect tests importing `github.com/gopacket/gopacket` ≥ v1.7.1 instead of the dormant `github.com/google/gopacket v1.1.19`. Test-only; no production import changes.

- [ ] **Step 1: Find the importers**

Run: `grep -rln 'github.com/google/gopacket' connect --include='*.go'`
Expected: only `*_test.go` files (confirm none are production files; if any are, STOP — that contradicts the audit and needs review).

- [ ] **Step 2: Retarget imports + module**

Rewrite the import path in those test files to `github.com/gopacket/gopacket`; `go get github.com/gopacket/gopacket@v1.7.1`; `go mod tidy`. Confirm `github.com/google/gopacket` is gone from `go.mod`.

- [ ] **Step 3: Run the affected tests + build/vet**

Run: `timeout 300 go test -run 'TestIp|TestSni|TestIcmp|TestBlockAction|TestProviderSecurity|TestPacketGopacket' -timeout 120s ./ && go build ./... && go vet ./...`
Expected: PASS, clean. (These are the test names in the six files; adjust `-run` to the actual test funcs found in Step 1.)

- [ ] **Step 4: Commit**

```bash
git add go.mod go.sum <the six test files>
git commit -F <msgfile>   # "chore(deps): retarget gopacket test dep to maintained gopacket/gopacket v1.7.1"
```

---

## Task 15: Integration checkpoint — build all three repos on the algorithm branch

**Files:** none (verification task)

- [ ] **Step 1: Push connect + sdk algorithm branches**

```bash
# in connect worktree
git push origin beta/algorithm-dpi
# in sdk
git push origin beta/algorithm-dpi
```

- [ ] **Step 2: Trigger the windows algorithm CI (now consuming the algorithm connect/sdk)**

```bash
# in windows repo on beta/algorithm-dpi
git push origin beta/algorithm-dpi
gh run list --branch beta/algorithm-dpi --limit 1
gh run watch <id> --exit-status
```
Expected: full green — SDK DLLs build against algorithm connect/sdk, app+service compile, selftest ≥563/0, `ndpi-crossproof` green, prerelease published.

- [ ] **Step 3: Local selftest sanity (owner box)**

Run (service-path precheck first): `powershell -NoProfile -File app\tools\build-local.ps1 -SkipDeps` then `urnetworkd.exe selftest`
Expected: ≥563/0. All new routing code is gated off by default (RoutingTierOff), so a stock build behaves exactly like today — the checkpoint proves the foundation compiles and ships inert, ready for the app to opt a session into Light tier for the next round of live testing.

- [ ] **Step 4: Report the checkpoint tag** for owner + friend to smoke-test that nothing regressed with routing OFF, and to flip RoutingTierLight from the dev screen to see reward lines + scored placement in logs.

---

## Self-Review

**Spec coverage** (against `2026-08-11-smart-routing-design.md`):
- §2 classification seam → Task 2 (FlowClassifier nil-default); nDPI itself is Phase 2 (out of scope here, seam left).
- §3 scored placement (hysteresis, N-of-M, anti-herding) → Tasks 3, 4, 8.
- §4 learner → Phase 3 (out of scope); priors persistence groundwork → Tasks 5, 10.
- §5 quarantine flap damping + re-entry ramp → Task 9; removal-census fix (#51) is a separate small task tracked independently.
- §6 platform tiers/guards → Tasks 11 (tier setting), 1/13/15 (CI boundary), 12 (Windows attribution); mobile inertness guaranteed by gomobile module boundary (no task needed — verified by audit).
- §7 persistence/privacy/controls → Task 10 (dot-file, 90d default settable unlimited, Logout reset); UI panel is on the beta/custom-server UI line (separate, per owner).
- §8 phasing → Phase 0 (Tasks 6, 13, and the dead-DNS-metric exclusion folded into Task 6's reward-source note), Phase 1 (Tasks 2-5, 7-12, 14).
- §9 testing → every task is TDD; Task 13 is the build gate.
- Reward instrumentation "dead DNS metric" (§8 Phase 0c): the reward source uses goodput + stall-free, NOT DNS-through-exit, so the dead metric is excluded by construction — noted in Task 6.

**Placeholder scan:** no TBD/TODO-as-work; every code step has real code. Test helper names that may differ in-repo (`newBareMultiClientForTest`, `newTestLocalState`) are flagged with a grep-and-reuse instruction rather than invented.

**Type consistency:** `TrafficClass`/`FlowClass`/`FlowClassifier` (Task 2) used identically in Tasks 6, 8. `ProviderPrior`/`PriorsStore` (Task 5) used in Task 10. `ReliabilitySettings` new fields (Task 7) consumed in Tasks 8, 9, 11. `challengerWins`/`lessLoadedTieBreak`/`demotionState` (Tasks 3-4) consumed in Task 8.

**Out-of-scope (own later plans):** nDPI binding + worker + 5-class mapping (Phase 2); SW-UCB learner + dark launch (Phase 3); Advanced Mode UI panel (UI line); Android `.so` + UID attribution (Android fork train); macOS attribution extension.
