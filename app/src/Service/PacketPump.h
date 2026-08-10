// The packet data path, mirroring the macOS PacketTunnelProvider:
//   outbound (host -> tunnel): wintun ring -> DeviceLocal.sendPacketBatch
//   inbound  (tunnel -> host): DeviceLocal batch callback -> wintun ring
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "Sdk.h"
#include "Wintun.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace urnw {

// THE ONLY SIGNAL IN THIS PROCESS THAT MEASURES "can this tunnel carry
// traffic" RATHER THAN INFERRING IT.
//
// Every other health reading here is second-hand: the SDK's proven-exit count
// is a claim about transports, the reliability metrics are counters about
// dials. These two numbers are the packets themselves — what the host handed
// the tunnel, and what the tunnel handed back. A tunnel with outbound climbing
// and inbound frozen is not "degraded"; it is a hole, and with the connected
// firewall policy in force it is a hole the whole machine is falling into.
//
// SEPARATE FROM THE PUMP'S LIFETIME, ON PURPOSE. The dead-tunnel failsafe reads
// these from its own thread while TunnelController is free to move the pump
// into an abandonable teardown worker (StopBudget.h). A raw pointer to the pump
// would be a use-after-free the first time a teardown ran over its budget; a
// shared block cannot be, and the reader simply sees the counters stop moving —
// which is the honest reading of "the pump is gone".
//
// Relaxed atomics: these are counters read at 1 Hz for a verdict measured in
// tens of seconds, incremented once per packet on the two hottest paths in the
// service. Nothing orders anything against them.
struct PacketCounters {
  std::atomic<uint64_t> outbound{0};  // host -> tunnel (wintun ring -> sdk)
  std::atomic<uint64_t> inbound{0};   // tunnel -> host (sdk -> wintun ring)
};

class PacketPump {
 public:
  PacketPump(WintunAdapter& adapter, urnet::DeviceLocal& device)
      : adapter_(adapter), device_(device) {}
  ~PacketPump();

  PacketPump(const PacketPump&) = delete;
  PacketPump& operator=(const PacketPump&) = delete;

  // False if the pump could not start; the caller must not report the tunnel
  // up, because nothing would move through it.
  bool Start();
  void Stop();

  // A SHARE of the counter block, valid for as long as the caller holds it —
  // including after this pump has been destroyed or abandoned. See the note on
  // PacketCounters for why that outliving is the point rather than a leak.
  std::shared_ptr<PacketCounters> Counters() const { return counters_; }

 private:
  void OutboundLoop();  // wintun -> device

  // Keeps the inbound callback from touching a destroyed adapter.
  //
  // Closing the subscription does NOT drain a callback that is already
  // running. DeviceLocal's receive dispatcher snapshots the callback slice
  // under a lock and then invokes the callbacks OUTSIDE it, while Remove()
  // swaps in a fresh slice copy-on-write (connect's CallbackList). An
  // in-flight dispatch therefore still holds — and still calls — a callback
  // that has just been unsubscribed. With the pump and the wintun session torn
  // down immediately after, that late call is a use-after-free on the adapter,
  // and it is the one a first run could plausibly hit: it needs only a packet
  // arriving while the tunnel stops.
  //
  // So the callback owns a share of this block instead of a pointer to the
  // pump. Stop() takes the lock to clear it, which also waits out any callback
  // currently inside; after that a late callback finds a null adapter and
  // returns.
  struct ReceiveGate {
    std::mutex mutex;
    WintunAdapter* adapter = nullptr;  // null once Stop() has run
  };

  WintunAdapter& adapter_;
  urnet::DeviceLocal& device_;
  // Constructed HERE, with the pump, and never reset: a reader that took a
  // share before Start() must not find a different block afterwards, and a
  // reader that holds one across Stop() must keep seeing the final totals
  // rather than a null.
  std::shared_ptr<PacketCounters> counters_ = std::make_shared<PacketCounters>();
  std::shared_ptr<ReceiveGate> gate_;
  // device -> wintun. Unsubscribes on DESTRUCTION or MOVE-ASSIGNMENT, which are
  // the only two operations on this type that call `urnet_sub_close`. Never
  // `reset()` it: that is inherited from `detail::Handle`, releases the handle
  // WITHOUT unsubscribing, and frees the retained callback out from under a Go
  // side that still holds a raw pointer to it. See PacketPump::Stop.
  urnet::Sub receiveSub_;
  std::thread outbound_;
  std::atomic<bool> running_{false};
  HANDLE stopEvent_ = nullptr;
};

}  // namespace urnw
