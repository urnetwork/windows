// Per-flow app attribution — "which program owns this connection" — feeding
// the SDK's FlowOwnerLookup seam (sdk `reliability_controls.go:546`; hpp
// `urnetwork_sdk.hpp:9579`, `DeviceLocal::setFlowOwnerLookup`).
//
// THIS IS THE FIRST CALLER OF SetFlowOwnerLookup IN THIS REPO. The seam has
// existed with a nil default (today's behavior: no attribution, one branch
// cost on the SDK's egress path) since the smart-routing design; this file is
// what turns it on for Windows.
//
// ---------------------------------------------------------------------------
// ASYNC-WITH-DEFAULT IS MANDATORY, NOT AN OPTIMIZATION.
//
// The lookup this class exposes is invoked from the SDK on the packet path,
// through a C -> Go -> C callback chain: urnet_device_local_set_flow_owner_lookup
// hands the Go side a C function pointer, Go calls it via cgo for a live flow,
// and the call lands back on THIS process's C++ on whatever thread the SDK's
// runtime happened to be draining the tun on. A synchronous
// GetExtendedTcpTable/GetExtendedUdpTable enumeration in that callback would
// therefore stall the entire tun drain for every flow it is asked about — the
// exact re-entrancy hazard the DPI feasibility study flagged for this class of
// seam (see docs/superpowers/specs/2026-08-11-smart-routing-design.md §2,
// "App attribution ... async-with-default").
//
// So the shape is fixed: the callback (LookupCached / Lookup, below) does a
// CACHE-ONLY read and returns "" on a miss, unconditionally and immediately.
// A background worker refreshes the table on a ~1s interval and on demand
// after a miss (WorkerLoop/RefreshOnce, in the .cpp) — it is the ONLY thing in
// this class that may call a Windows API, and it never runs on the SDK's
// calling thread.
//
// ---------------------------------------------------------------------------
// WHAT IS PURE AND WHAT IS NOT, AND WHY THE SPLIT IS DRAWN HERE
//
// Following ConnectionHealth.h / NetPolicy.h: the decision logic — the 5-tuple
// key, the bounded cache and its eviction policy, and the cache-only lookup
// that decides hit vs. miss-and-schedule — is pure, allocates only ordinary
// STL containers, and calls no Windows API. It is defined INLINE, right here,
// so `urnetworkd selftest` can drive it directly with no mocks and no
// elevation. Only the table enumeration, the pid->path resolution and the
// worker thread are declared here and DEFINED in FlowOwner.cpp, which is the
// only translation unit in this feature that includes a Windows header.
//
// ---------------------------------------------------------------------------
// WHY THE KEY IS BINARY, NOT THE WIRE'S TEXT
//
// The SDK hands the lookup whatever textual form Go's net.IP.String() produces
// for the two addresses. Nothing promises that text is byte-identical to
// whatever this class's own table enumeration would stringify the same
// address as (leading zeros, IPv6 compression, case). Rather than hope two
// independent stacks agree on formatting, both sides of every comparison are
// parsed down to the same 16-byte form (FlowKey::sourceAddr/destAddr) before
// they are ever compared — see FlowOwner::Lookup in the .cpp, the one place
// that parse happens.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace urnw {

// Only the two values the SDK ever passes for `version`; anything else is
// read as V4 (see FlowOwner::Lookup) rather than rejected outright, since a
// wrong-but-consistent bucket is recoverable (the lookup just always misses)
// and a thrown/rejected flow is not.
enum class FlowIpVersion : int64_t { V4 = 4, V6 = 6 };

// A flow's 5-tuple, in the binary form described above.
struct FlowKey {
  FlowIpVersion version = FlowIpVersion::V4;
  int64_t protocol = 0;                   // IPPROTO_TCP (6) / IPPROTO_UDP (17)
  std::array<uint8_t, 16> sourceAddr{};   // v4 lives in the first 4 bytes
  int64_t sourcePort = 0;
  std::array<uint8_t, 16> destAddr{};     // v4 lives in the first 4 bytes
  int64_t destPort = 0;

  // Memberwise, including the two byte arrays (std::array has operator==).
  // This is the whole reason two flows that differ only in port, or only in
  // ip version, are never the same cache entry: every field participates.
  bool operator==(const FlowKey&) const = default;
};

struct FlowKeyHash {
  size_t operator()(const FlowKey& k) const noexcept {
    size_t h = std::hash<int64_t>{}(static_cast<int64_t>(k.version));
    auto mix = [&h](size_t v) {
      h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(std::hash<int64_t>{}(k.protocol));
    mix(std::hash<int64_t>{}(k.sourcePort));
    mix(std::hash<int64_t>{}(k.destPort));
    // Two 64-bit halves per address rather than a byte loop: this hash is on
    // the read path for every packet's flow lookup, however cheap.
    uint64_t a0, a1, b0, b1;
    std::memcpy(&a0, k.sourceAddr.data(), 8);
    std::memcpy(&a1, k.sourceAddr.data() + 8, 8);
    std::memcpy(&b0, k.destAddr.data(), 8);
    std::memcpy(&b1, k.destAddr.data() + 8, 8);
    mix(std::hash<uint64_t>{}(a0));
    mix(std::hash<uint64_t>{}(a1));
    mix(std::hash<uint64_t>{}(b0));
    mix(std::hash<uint64_t>{}(b1));
    return h;
  }
};

// A hashmap capped at `capacity` entries. Two write paths, matching
// FlowOwner's two different staleness stories:
//
//   Put()        — incremental upsert. A NEW key evicts the OLDEST-INSERTED
//                  entry once the cache is at capacity (FIFO); updating an
//                  EXISTING key's value never evicts anything and does not
//                  move it in eviction order. This is the pid->exe-path
//                  cache's policy: that mapping is expensive to produce and
//                  essentially never goes stale on its own (a pid's image
//                  path does not change; pid reuse inside a ~1s window is the
//                  small, accepted risk this whole feature class carries), so
//                  the only reason to evict one is to make room, and the
//                  fairest room-maker with no access-time bookkeeping is "the
//                  one resolved longest ago".
//
//   ReplaceAll() — wholesale rebuild from a fresh enumeration. This is the
//                  5-tuple cache's policy: GetExtendedTcpTable/UdpTable IS
//                  the ground truth for "what flows exist right now", so a
//                  flow that closed between two refreshes must vanish from
//                  the cache on the very next one rather than wait for FIFO
//                  eviction — lingering here is a MISATTRIBUTION risk (a new
//                  flow reusing the same 5-tuple would read the old owner),
//                  not just memory bloat. Entries beyond `capacity` are
//                  dropped (first `capacity` kept) and the drop count is
//                  returned so the caller can log it once instead of losing
//                  the fact silently.
//
// mutex_ guards plain map/deque bookkeeping only — no syscall, no I/O, no
// unbounded work under the lock — which is the "very cheap lock" the
// FlowOwner header comment promises the SDK's calling thread.
template <class Key, class Value, class Hash = std::hash<Key>>
class BoundedCache {
 public:
  explicit BoundedCache(size_t capacity) : capacity_(capacity) {}

  std::optional<Value> Get(const Key& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    return it->second;
  }

  void Put(Key key, Value value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      it->second = std::move(value);
      return;
    }
    if (capacity_ == 0) return;
    if (order_.size() >= capacity_) {
      const Key oldest = std::move(order_.front());
      order_.pop_front();
      map_.erase(oldest);
    }
    order_.push_back(key);
    map_.emplace(std::move(key), std::move(value));
  }

  // Rebuilds the whole cache from `entries`. Returns how many did not fit and
  // were dropped (the entries AFTER the first `capacity`, in the order given —
  // callers that care about which ones survive should put the ones that
  // matter first). Entries beyond capacity are not accepted even bumping an
  // existing key.
  size_t ReplaceAll(std::vector<std::pair<Key, Value>> entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_.clear();
    order_.clear();
    size_t dropped = 0;
    for (auto& [key, value] : entries) {
      if (map_.size() >= capacity_) {
        ++dropped;
        continue;
      }
      // De-dup within the batch: an enumeration should not repeat a 5-tuple,
      // but nothing upstream promises it, and a repeat must not count twice
      // against `capacity_` or double-push into `order_`.
      if (map_.contains(key)) continue;
      order_.push_back(key);
      map_.emplace(std::move(key), std::move(value));
    }
    return dropped;
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
  }

 private:
  mutable std::mutex mutex_;
  size_t capacity_;
  std::deque<Key> order_;
  std::unordered_map<Key, Value, Hash> map_;
};

// See the file header. Bounds are generous relative to what a busy desktop's
// live connection count and distinct-process count would realistically be; a
// machine with legitimately more concurrent flows than kMaxFlows loses
// attribution for the overflow (BoundedCache::ReplaceAll's drop count),
// never correctness for the flows that DO fit.
class FlowOwner {
 public:
  static constexpr size_t kMaxFlows = 8192;
  static constexpr size_t kMaxExePaths = 4096;
  static constexpr std::chrono::milliseconds kRefreshInterval{1000};

  explicit FlowOwner(size_t maxFlows = kMaxFlows, size_t maxExePaths = kMaxExePaths)
      : flows_(maxFlows), exePaths_(maxExePaths) {}
  ~FlowOwner();

  FlowOwner(const FlowOwner&) = delete;
  FlowOwner& operator=(const FlowOwner&) = delete;

  // THE CACHE-ONLY FAST PATH. Pure: touches only the two BoundedCaches above
  // and an atomic counter — no lock is held across anything that can block,
  // and there is no code path from here to CollectTcp/CollectUdp/
  // ResolveExePath (those are reachable only from WorkerLoop, which only
  // exists once Start() has been called). A miss — either the flow itself is
  // unknown, or its owning pid is known but the pid's exe path is not
  // resolved yet — schedules a background refresh and returns "" rather than
  // resolving inline.
  std::string LookupCached(const FlowKey& key) {
    auto pid = flows_.Get(key);
    if (!pid) {
      RequestRefresh();
      return {};
    }
    auto path = exePaths_.Get(*pid);
    if (!path) {
      RequestRefresh();
      return {};
    }
    return *path;
  }

  // THE SDK-FACING TRAMPOLINE — install this verbatim as the FlowOwnerLookup
  // std::function (see TunnelController::StartLocked, step 4/8). Defined in
  // the .cpp because turning the wire's textual addresses into FlowKey's
  // binary form needs inet_pton; everything past that parse is the pure path
  // above. This is also where the version/protocol values the SDK passes are
  // interpreted (V4/V6, and the UDP wildcard-destination normalisation — see
  // the .cpp for why GetExtendedUdpTable cannot supply a remote endpoint).
  std::string Lookup(int64_t version, int64_t protocol, const std::string& sourceIp,
                      int64_t sourcePort, const std::string& destIp, int64_t destPort);

  // Idempotent: the first call after construction (or after Stop()) starts
  // the worker; later calls are no-ops. Safe to call every session — a
  // reconnect must not spin up a second worker thread.
  void Start();
  // Idempotent; safe even if Start() was never called.
  void Stop();

  // Test-only observation of the miss -> schedule contract. Incremented ONLY
  // by RequestRefresh(), which is pure and reachable from LookupCached — never
  // by the real enumeration — so a selftest can prove "a miss schedules
  // background work instead of doing it inline" without a worker thread
  // existing at all (Start() need never be called).
  uint64_t RefreshRequestCount() const {
    return refreshRequests_.load(std::memory_order_relaxed);
  }

 private:
  // Pure: bumps the counter and flags the worker's wait, never touches a
  // Windows API. Safe to call whether or not the worker thread is running —
  // with no worker started this just leaves refreshPending_ set, which is
  // exactly right (nothing is owed to a caller who never started one).
  void RequestRefresh() {
    refreshRequests_.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(wakeMutex_);
      refreshPending_ = true;
    }
    wake_.notify_one();
  }

  // --- the syscall layer: declared here, defined in FlowOwner.cpp ----------
  void WorkerLoop();
  // One full pass: enumerate TCP+UDP, v4+v6, resolve any newly-seen owning
  // pid's exe path, then swap the result into flows_.
  void RefreshOnce();
  static void CollectTcp(int addressFamily, std::vector<std::pair<FlowKey, uint32_t>>& out);
  static void CollectUdp(int addressFamily, std::vector<std::pair<FlowKey, uint32_t>>& out);
  static std::string ResolveExePath(uint32_t pid);

  BoundedCache<FlowKey, uint32_t, FlowKeyHash> flows_;  // 5-tuple -> owning pid
  BoundedCache<uint32_t, std::string> exePaths_;        // pid -> module path
  std::atomic<uint64_t> refreshRequests_{0};

  std::thread worker_;
  std::mutex wakeMutex_;
  std::condition_variable wake_;
  bool refreshPending_ = false;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
};

}  // namespace urnw
