// SPDX-License-Identifier: MPL-2.0
#include "EgressMonitor.h"

#include <winsock2.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "Log.h"
#include "NetworkConfig.h"
#include "Sdk.h"
#include "Strings.h"

#pragma comment(lib, "iphlpapi.lib")

namespace urnw {
namespace {

// The v4 source address of the chosen egress interface, for the log. This is
// the value the R1 audit hangs on: it must be the physical adapter's LAN
// address, never the tun's 169.254.x.
std::string SourceAddressOf(uint32_t ifIndex) {
  if (ifIndex == 0) return "-";
  uint8_t addr[4] = {0};
  if (!NetworkConfig::InterfaceSourceAddress(ifIndex, AF_INET, addr)) return "-";
  return std::format("{}.{}.{}.{}", addr[0], addr[1], addr[2], addr[3]);
}

// Does this interface index still name something? Retaining an index that has
// been reclaimed is worse than dropping it: setsockopt fails and the socket
// falls back to the route table, which with the tunnel up is the tun.
bool InterfaceExists(uint32_t ifIndex) {
  if (ifIndex == 0) return false;
  MIB_IF_ROW2 row{};
  row.InterfaceIndex = ifIndex;
  return ::GetIfEntry2(&row) == NO_ERROR;
}

}  // namespace

EgressMonitor::~EgressMonitor() { Stop(); }

bool EgressMonitor::Start() {
  Refresh();
  DWORD err = ::NotifyIpInterfaceChange(AF_UNSPEC, &EgressMonitor::OnChange,
                                        this, FALSE, &notifyHandle_);
  if (err != NO_ERROR) {
    // The initial binding stands, but it will not follow a Wi-Fi/ethernet
    // switch — say so, because that failure mode looks like "the tunnel stopped
    // working after I moved networks".
    LogError("egress: NotifyIpInterfaceChange failed: {} — the binding will NOT "
             "follow network changes",
             err);
    return false;
  }
  return true;
}

void EgressMonitor::Stop() {
  // Cancel first and WITHOUT the lock: CancelMibChangeNotify2 blocks until any
  // in-flight callback has returned, and that callback takes mutex_.
  if (notifyHandle_) {
    ::CancelMibChangeNotify2(notifyHandle_);
    notifyHandle_ = nullptr;
  }
}

void EgressMonitor::SetOnChange(ChangeHandler handler) {
  std::scoped_lock lock(mutex_);
  onChange_ = std::move(handler);
}

void EgressMonitor::Refresh() {
  ChangeHandler handler;
  EgressInterfaces egress;
  bool changed = false;

  // Zero LUID == the rpc-only mode's "no tun exists" (see the ctor). Every R1
  // claim below is about looping into the tunnel, so with no tunnel they are
  // not merely quieter, they are untrue — and a log that cries R1 when R1 does
  // not apply is how a real one gets ignored later.
  const bool hasTun = tunLuid_.Value != 0;

  {
    std::scoped_lock lock(mutex_);
    egress = NetworkConfig::DiscoverEgress(tunLuid_);

    // Losing the physical default route (Wi-Fi drops, cable out) makes
    // DiscoverEgress return 0. Do NOT push 0 down while we have a good index:
    // unbinding means the SDK's next socket follows the route table, which with
    // the tunnel up is the tun — the exact self-deadlock R1 exists to prevent.
    // A socket pinned to a downed interface fails fast with WSAENETUNREACH and
    // retries; a socket that follows the tun blackholes silently. Keep the last
    // known good and wait for the next change notification.
    //
    // But only while that index still names a real interface. Pinning to an
    // index the stack has reclaimed is the worst of both worlds: setsockopt
    // fails, and the socket falls back to the route table anyway — the loop,
    // arrived at quietly. Re-validating is what makes the retention safe rather
    // than merely preferable.
    if (egress.index4 == 0 && current_.index4 != 0) {
      if (InterfaceExists(current_.index4)) {
        LogWarn("egress: no ipv4 default route right now — keeping the last "
                "physical interface {} bound rather than unbinding ({})",
                current_.index4, hasTun ? "R1" : "no tun, so this is only a "
                                                 "preference, not R1");
        egress.index4 = current_.index4;
      } else if (hasTun) {
        LogError("egress: the retained ipv4 interface {} no longer exists, so it "
                 "cannot stay pinned. The sdk's sockets are unbound and will "
                 "follow the route table — R1 exposure while the tunnel is up, "
                 "until an interface reappears.",
                 current_.index4);
      } else {
        LogWarn("egress: the retained ipv4 interface {} no longer exists; "
                "unbinding. With no tun, the sdk's sockets following the route "
                "table is the correct behaviour, not an exposure.",
                current_.index4);
      }
    }
    if (egress.index6 == 0 && current_.index6 != 0 &&
        InterfaceExists(current_.index6)) {
      egress.index6 = current_.index6;
    }

    changed = egress.index4 != current_.index4 || egress.index6 != current_.index6;
    current_ = egress;
    urnet::setEgressInterfaceIndex(static_cast<int64_t>(egress.index4),
                                   static_cast<int64_t>(egress.index6));

    if (changed) {
      LogInfo("egress: bound to v4=[{}] src={} v6=[{}]",
              NetworkConfig::DescribeInterface(egress.index4),
              SourceAddressOf(egress.index4),
              NetworkConfig::DescribeInterface(egress.index6));
    } else {
      LogDebug("egress: unchanged (v4={} v6={})", egress.index4, egress.index6);
    }

    if (egress.index4 == 0) {
      // Nothing to pin to at all. Harmless before the tun routes exist; once
      // they do it is the R1 loop condition, so it is an error in tunnel mode.
      if (hasTun) {
        LogError("egress: NO physical ipv4 interface bound — the sdk's own "
                 "sockets will follow the route table (R1 exposure once tun "
                 "routes are up)");
      } else {
        LogWarn("egress: no physical ipv4 default route — the sdk's own sockets "
                "will follow the route table. With no tun that is normal; it "
                "usually just means this machine has no network right now.");
      }
    }
    handler = onChange_;
  }

  // Deliberately outside the lock: the handler takes locks of its own, and
  // Stop() blocks on this callback while its caller holds one of them.
  if (changed && handler) handler(egress);
}

EgressInterfaces EgressMonitor::Current() const {
  std::scoped_lock lock(mutex_);
  return current_;
}

void __stdcall EgressMonitor::OnChange(void* context, MIB_IPINTERFACE_ROW*,
                                       MIB_NOTIFICATION_TYPE) {
  // Called on a system worker thread. Recompute the egress binding; the SDK
  // setter is atomic and cheap, so we can react to every change.
  auto* self = static_cast<EgressMonitor*>(context);
  if (self) self->Refresh();
}

}  // namespace urnw
