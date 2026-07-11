// Watches for network changes and keeps the SDK's egress interface binding (R1)
// pointed at the current physical default-route interface, so the service's own
// platform/provider sockets never loop into the tunnel. Mirrors what
// wireguard-windows does with NotifyIpInterfaceChange.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ifdef.h>

namespace urnw {

class EgressMonitor {
 public:
  explicit EgressMonitor(NET_LUID tunLuid) : tunLuid_(tunLuid) {}
  ~EgressMonitor();

  // Compute the current egress interfaces, push them to the SDK, and register
  // for change notifications to keep them current.
  bool Start();
  void Stop();

  // Recompute now (also called from the change callback).
  void Refresh();

 private:
  static void __stdcall OnChange(void* context, MIB_IPINTERFACE_ROW* row,
                                 MIB_NOTIFICATION_TYPE type);

  NET_LUID tunLuid_;
  HANDLE notifyHandle_ = nullptr;
};

}  // namespace urnw
