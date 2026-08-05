// Watches for network changes and keeps the SDK's egress interface binding (R1)
// pointed at the current physical default-route interface, so the service's own
// platform/provider sockets never loop into the tunnel it provides. Mirrors what
// wireguard-windows does with NotifyIpInterfaceChange.
//
// The binding must be in force BEFORE the SDK opens any socket and BEFORE the
// tun routes are installed — see the ordering in TunnelController::StartLocked.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Same order as NetworkConfig.cpp (which compiles): winsock2 before the IP
// helpers, and ws2tcpip pulls in ws2ipdef (SOCKADDR_INET) that netioapi needs.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>    // NET_LUID, MIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE, NotifyIpInterfaceChange

#include <functional>
#include <mutex>

#include "NetworkConfig.h"

namespace urnw {

class EgressMonitor {
 public:
  explicit EgressMonitor(NET_LUID tunLuid) : tunLuid_(tunLuid) {}
  ~EgressMonitor();

  EgressMonitor(const EgressMonitor&) = delete;
  EgressMonitor& operator=(const EgressMonitor&) = delete;

  // Notified after the binding CHANGES, with the new indices. Everything else
  // pinned to the physical interface — the split-tunnel driver's source
  // address above all — has to follow the same NIC the SDK just followed, or it
  // keeps rewriting binds to an adapter that is gone.
  //
  // Runs on the thread that observed the change: a system worker thread for a
  // notification, the caller's thread for the initial Start(). Invoked WITHOUT
  // this monitor's lock. The handler must not take a lock that a thread calling
  // Stop() could be holding — Stop() blocks until in-flight callbacks return,
  // so such a handler deadlocks against it. Set before Start().
  using ChangeHandler = std::function<void(EgressInterfaces)>;
  void SetOnChange(ChangeHandler handler);

  // Compute the current egress interfaces, push them to the SDK, and register
  // for change notifications to keep them current. Returns false if the
  // notification could not be registered; the initial binding is still applied,
  // it just will not follow later network changes.
  bool Start();
  void Stop();

  // Recompute now (also called from the change callback).
  void Refresh();

  // What is currently bound, for status and for the startup log. index4 == 0
  // means the SDK's sockets are NOT pinned to a physical interface — with the
  // tunnel up that is the R1 loop condition.
  EgressInterfaces Current() const;

 private:
  static void __stdcall OnChange(void* context, MIB_IPINTERFACE_ROW* row,
                                 MIB_NOTIFICATION_TYPE type);

  NET_LUID tunLuid_;
  HANDLE notifyHandle_ = nullptr;

  // Serializes Refresh: NotifyIpInterfaceChange callbacks arrive on system
  // worker threads and can overlap each other and Start().
  mutable std::mutex mutex_;
  EgressInterfaces current_;
  ChangeHandler onChange_;
};

}  // namespace urnw
