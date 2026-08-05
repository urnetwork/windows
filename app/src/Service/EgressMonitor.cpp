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

void EgressMonitor::Refresh() {
  std::scoped_lock lock(mutex_);
  EgressInterfaces egress = NetworkConfig::DiscoverEgress(tunLuid_);

  // Losing the physical default route (Wi-Fi drops, cable out) makes
  // DiscoverEgress return 0. Do NOT push 0 down while we have a good index:
  // unbinding means the SDK's next socket follows the route table, which with
  // the tunnel up is the tun — the exact self-deadlock R1 exists to prevent. A
  // socket pinned to a down interface fails fast and retries; a socket pinned
  // to the tun wedges the client against itself. Keep the last known good and
  // wait for the next change notification.
  if (egress.index4 == 0 && current_.index4 != 0) {
    LogWarn("egress: no ipv4 default route right now — keeping the last physical "
            "interface {} bound rather than unbinding (R1)",
            current_.index4);
    egress.index4 = current_.index4;
  }
  if (egress.index6 == 0 && current_.index6 != 0) {
    egress.index6 = current_.index6;
  }

  bool changed = egress.index4 != current_.index4 || egress.index6 != current_.index6;
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
    // Nothing to pin to at all. Harmless before the tun routes exist; once they
    // do it is the R1 loop condition, so it is an error either way.
    LogError("egress: NO physical ipv4 interface bound — the sdk's own sockets "
             "will follow the route table (R1 exposure once tun routes are up)");
  }
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
