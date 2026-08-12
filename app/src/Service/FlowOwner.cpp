// SPDX-License-Identifier: MPL-2.0
#include "FlowOwner.h"

// Same ordering discipline as EgressMonitor.h/NetworkConfig.cpp: winsock2
// before the IP helpers, and ws2tcpip before iphlpapi so iphlpapi's IPv6
// table types (which reference ws2ipdef's SOCKADDR_INET-adjacent types) see
// them already declared.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <unordered_set>

#include "Log.h"
#include "Strings.h"
#include "ThreadGuard.h"

namespace urnw {
namespace {

// Parse a textual address into FlowKey's 16-byte form. v4 addresses land in
// the first 4 bytes with the rest left zero, matching how CollectTcp/CollectUdp
// place a DWORD address — so a v4 key built from the wire and a v4 key built
// from the live table always compare equal byte-for-byte.
bool ParseAddr(const std::string& text, int64_t version, std::array<uint8_t, 16>& out) {
  out.fill(0);
  if (text.empty()) return false;
  if (version == 6) {
    return ::inet_pton(AF_INET6, text.c_str(), out.data()) == 1;
  }
  in_addr a4{};
  if (::inet_pton(AF_INET, text.c_str(), &a4) != 1) return false;
  std::memcpy(out.data(), &a4, sizeof(a4));
  return true;
}

// GetExtendedTcpTable/GetExtendedUdpTable want a buffer they size themselves;
// the accepted pattern is "guess, then resize once on ERROR_INSUFFICIENT_
// BUFFER" — looped a few times because the live table can grow between the
// sizing call and the fetch on a busy box, and one retry is not guaranteed to
// still be enough.
template <class Fn>
DWORD FetchTable(std::vector<uint8_t>& buf, Fn&& fetch) {
  DWORD size = static_cast<DWORD>(buf.size());
  DWORD err = fetch(buf.data(), &size);
  for (int attempt = 0; err == ERROR_INSUFFICIENT_BUFFER && attempt < 3; ++attempt) {
    buf.resize(size);
    err = fetch(buf.data(), &size);
  }
  return err;
}

}  // namespace

std::string FlowOwner::Lookup(int64_t version, int64_t protocol, const std::string& sourceIp,
                               int64_t sourcePort, const std::string& destIp,
                               int64_t destPort) {
  FlowKey key;
  key.version = version == 6 ? FlowIpVersion::V6 : FlowIpVersion::V4;
  key.protocol = protocol;
  if (!ParseAddr(sourceIp, version, key.sourceAddr)) return {};
  key.sourcePort = sourcePort;
  // GetExtendedUdpTable's rows carry only the LOCAL endpoint: a UDP socket is
  // not "connected" the way TCP's is, so there is no remote address/port for
  // the table to report, and RefreshOnce()/CollectUdp therefore key a UDP
  // flow on its source alone (dest zeroed). Mirror that normalisation here —
  // otherwise a real UDP lookup, which DOES carry a real destination from the
  // wire, could never hit what CollectUdp put in the cache.
  if (protocol == IPPROTO_UDP) {
    key.destAddr.fill(0);
    key.destPort = 0;
  } else {
    if (!ParseAddr(destIp, version, key.destAddr)) return {};
    key.destPort = destPort;
  }
  return LookupCached(key);
}

void FlowOwner::Start() {
  if (running_.exchange(true)) return;  // already running: idempotent
  stop_.store(false);
  worker_ = StartGuardedThread("flowowner-refresh", [this] { WorkerLoop(); });
}

void FlowOwner::Stop() {
  if (!running_.exchange(false)) return;  // never started, or already stopped
  stop_.store(true);
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

FlowOwner::~FlowOwner() { Stop(); }

void FlowOwner::WorkerLoop() {
  // Run one pass immediately rather than waiting out the first interval, so
  // the callback installed right after Start() is not stuck answering ""
  // for a full second before the cache has ever been filled once.
  RefreshOnce();
  std::unique_lock<std::mutex> lock(wakeMutex_);
  while (!stop_.load(std::memory_order_relaxed)) {
    wake_.wait_for(lock, kRefreshInterval, [this] {
      return refreshPending_ || stop_.load(std::memory_order_relaxed);
    });
    if (stop_.load(std::memory_order_relaxed)) break;
    refreshPending_ = false;
    // Unlocked across the actual work: RefreshOnce can take real (if small)
    // time doing syscalls, and holding wakeMutex_ across it would block
    // RequestRefresh() — called from the SDK's thread on every miss — behind
    // exactly the enumeration this whole design exists to keep off that
    // thread.
    lock.unlock();
    RefreshOnce();
    lock.lock();
  }
}

void FlowOwner::RefreshOnce() {
  std::vector<std::pair<FlowKey, uint32_t>> entries;
  entries.reserve(512);
  CollectTcp(AF_INET, entries);
  CollectTcp(AF_INET6, entries);
  CollectUdp(AF_INET, entries);
  CollectUdp(AF_INET6, entries);

  // Resolve exe paths for any pid this cycle saw that is not already cached —
  // deliberately BEFORE the flows_ swap below: LookupCached checks exePaths_
  // right after flows_, so a pid that is new in BOTH caches this instant
  // still resolves on THIS pass rather than costing the caller a second
  // miss-and-refresh round trip.
  std::unordered_set<uint32_t> pids;
  pids.reserve(entries.size());
  for (const auto& [key, pid] : entries) pids.insert(pid);
  for (uint32_t pid : pids) {
    if (exePaths_.Get(pid)) continue;  // the expensive part, cached separately
    std::string path = ResolveExePath(pid);
    if (!path.empty()) exePaths_.Put(pid, std::move(path));
  }

  const size_t dropped = flows_.ReplaceAll(std::move(entries));
  static std::atomic<bool> warnedOnce{false};
  if (dropped > 0 && !warnedOnce.exchange(true)) {
    LogWarn("flowowner: this refresh saw more live flows than the {}-entry "
            "cache holds; {} were dropped and will report no owner until they "
            "age out of the live table (logged once)",
            FlowOwner::kMaxFlows, dropped);
  }
}

void FlowOwner::CollectTcp(int addressFamily,
                           std::vector<std::pair<FlowKey, uint32_t>>& out) {
  std::vector<uint8_t> buf(1 << 15);
  const DWORD err = FetchTable(buf, [&](void* p, DWORD* size) {
    return ::GetExtendedTcpTable(p, size, /*bOrder=*/FALSE, addressFamily,
                                 TCP_TABLE_OWNER_PID_ALL, 0);
  });
  if (err != NO_ERROR) {
    LogWarn("flowowner: GetExtendedTcpTable({}) failed: {}",
            addressFamily == AF_INET ? "v4" : "v6", err);
    return;
  }
  if (addressFamily == AF_INET) {
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
      const auto& row = table->table[i];
      FlowKey key;
      key.version = FlowIpVersion::V4;
      key.protocol = IPPROTO_TCP;
      std::memcpy(key.sourceAddr.data(), &row.dwLocalAddr, sizeof(row.dwLocalAddr));
      key.sourcePort = ntohs(static_cast<u_short>(row.dwLocalPort));
      std::memcpy(key.destAddr.data(), &row.dwRemoteAddr, sizeof(row.dwRemoteAddr));
      key.destPort = ntohs(static_cast<u_short>(row.dwRemotePort));
      out.emplace_back(key, row.dwOwningPid);
    }
  } else {
    const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
      const auto& row = table->table[i];
      FlowKey key;
      key.version = FlowIpVersion::V6;
      key.protocol = IPPROTO_TCP;
      std::memcpy(key.sourceAddr.data(), row.ucLocalAddr, sizeof(row.ucLocalAddr));
      key.sourcePort = ntohs(static_cast<u_short>(row.dwLocalPort));
      std::memcpy(key.destAddr.data(), row.ucRemoteAddr, sizeof(row.ucRemoteAddr));
      key.destPort = ntohs(static_cast<u_short>(row.dwRemotePort));
      out.emplace_back(key, row.dwOwningPid);
    }
  }
}

void FlowOwner::CollectUdp(int addressFamily,
                           std::vector<std::pair<FlowKey, uint32_t>>& out) {
  std::vector<uint8_t> buf(1 << 15);
  const DWORD err = FetchTable(buf, [&](void* p, DWORD* size) {
    return ::GetExtendedUdpTable(p, size, /*bOrder=*/FALSE, addressFamily,
                                 UDP_TABLE_OWNER_PID, 0);
  });
  if (err != NO_ERROR) {
    LogWarn("flowowner: GetExtendedUdpTable({}) failed: {}",
            addressFamily == AF_INET ? "v4" : "v6", err);
    return;
  }
  // Destination is left at FlowKey's zero default for every UDP row: the
  // owner-pid table is keyed on the LOCAL endpoint only (see the WHY-comment
  // on Lookup()'s UDP branch). Callers normalise the same way before probing
  // the cache, so this is not a partial key — it is the whole key for UDP.
  if (addressFamily == AF_INET) {
    const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buf.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
      const auto& row = table->table[i];
      FlowKey key;
      key.version = FlowIpVersion::V4;
      key.protocol = IPPROTO_UDP;
      std::memcpy(key.sourceAddr.data(), &row.dwLocalAddr, sizeof(row.dwLocalAddr));
      key.sourcePort = ntohs(static_cast<u_short>(row.dwLocalPort));
      out.emplace_back(key, row.dwOwningPid);
    }
  } else {
    const auto* table = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(buf.data());
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
      const auto& row = table->table[i];
      FlowKey key;
      key.version = FlowIpVersion::V6;
      key.protocol = IPPROTO_UDP;
      std::memcpy(key.sourceAddr.data(), row.ucLocalAddr, sizeof(row.ucLocalAddr));
      key.sourcePort = ntohs(static_cast<u_short>(row.dwLocalPort));
      out.emplace_back(key, row.dwOwningPid);
    }
  }
}

std::string FlowOwner::ResolveExePath(uint32_t pid) {
  // QUERY_LIMITED_INFORMATION is enough for QueryFullProcessImageNameW and,
  // unlike PROCESS_QUERY_INFORMATION, is grantable against a protected/
  // elevated process from this (SYSTEM) service without also asking for
  // rights this code never uses.
  HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return {};  // exited between the table snapshot and here, or denied
  wchar_t buf[MAX_PATH];
  DWORD len = MAX_PATH;
  std::string result;
  if (::QueryFullProcessImageNameW(h, 0, buf, &len)) {
    result = Narrow(std::wstring(buf, len));
  }
  ::CloseHandle(h);
  return result;
}

}  // namespace urnw
