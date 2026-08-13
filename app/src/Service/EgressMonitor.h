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
  // tunLuid is the interface to EXCLUDE from egress selection. A zero LUID
  // means "there is no tun" — the rpc-only start mode, which creates no
  // adapter. That is not a sentinel bolted on: DiscoverEgress excludes by
  // `row.InterfaceLuid.Value == tunLuid.Value`, and no real interface has LUID
  // 0 (NET_LUID packs a non-zero IANA ifType into its high bits), so a zero
  // LUID excludes exactly nothing, which is exactly right when nothing needs
  // excluding. No LUID is faked and no invariant is papered over.
  //
  // With no tun, R1 does not apply — the SDK's sockets cannot loop into a
  // tunnel that does not exist — so the binding is a plain "prefer the physical
  // default route", and the retention behaviour in Refresh() is conservative
  // rather than load-bearing.
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

  // "THE OS SAID SOMETHING ABOUT IP STATE CHANGED." Fired on EVERY observation,
  // whether or not the bound index moved, and that difference is the whole
  // reason it exists as a second callback rather than a second use of the one
  // above.
  //
  // SetOnChange fires only when the INDEX MOVES, which is false for the single
  // most common failure this service has: the cable comes out, the adapter
  // keeps its default route for a while, DiscoverEgress deliberately falls back
  // to it (NetworkConfig.cpp) and Refresh deliberately retains the last good
  // index anyway — so `changed` is false, the handler never runs, and the log
  // says "egress: unchanged". Correct for R1, and blind for everything else:
  // the SDK is never told the network moved, its transports die on timeouts,
  // and the tunnel stays "up" over nothing while the firewall blocks every
  // other path.
  //
  // Same threading contract as SetOnChange — a system worker thread, no lock of
  // this monitor held — with one ADDITIONAL rule that is not advice: THIS
  // HANDLER MUST NOT CALL INTO THE SDK. Stop() blocks until in-flight callbacks
  // return, so a handler that can block wedges Stop(), which wedges
  // TearDownSessionLocked. Record the event and return; do the work on a thread
  // of your own (TunnelWatchdog.h).
  using NetworkEventHandler = std::function<void()>;
  void SetOnNetworkEvent(NetworkEventHandler handler);

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
  // The DEFAULT ROUTE half of "the network moved". A default route appearing or
  // vanishing is a ROUTE event, and this monitor watched only interface events —
  // so the cable coming out, the case that strands the owner, could produce no
  // observation at all. DiscoverEgress reads the forward table, so the two
  // notifications answer the same question and both must feed Refresh.
  //
  // FILTERED, and the filter is load-bearing rather than an optimisation: the
  // tun carries 31 capture routes (NetPolicy.h) that are installed and reverted
  // in a burst at every bring-up and teardown, and an unfiltered subscription
  // would turn each of those bursts into 31 notifications and 31 SDK
  // network-change kicks — during the exact sequences that must not be
  // perturbed. Only prefix-length-0 routes on interfaces other than our tun get
  // through.
  static void __stdcall OnRouteChange(void* context, MIB_IPFORWARD_ROW2* row,
                                      MIB_NOTIFICATION_TYPE type);

  NET_LUID tunLuid_;
  HANDLE notifyHandle_ = nullptr;
  HANDLE routeNotifyHandle_ = nullptr;

  // Serializes Refresh: NotifyIpInterfaceChange callbacks arrive on system
  // worker threads and can overlap each other and Start().
  mutable std::mutex mutex_;
  EgressInterfaces current_;
  ChangeHandler onChange_;
  NetworkEventHandler onNetworkEvent_;
};

}  // namespace urnw
