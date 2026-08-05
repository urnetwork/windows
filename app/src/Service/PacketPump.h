// The packet data path, mirroring the macOS PacketTunnelProvider:
//   outbound (host -> tunnel): wintun ring -> DeviceLocal.sendPacket
//   inbound  (tunnel -> host): DeviceLocal receive callback -> wintun ring
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "Sdk.h"
#include "Wintun.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace urnw {

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
  std::shared_ptr<ReceiveGate> gate_;
  urnet::Sub receiveSub_;  // device -> wintun; unsubscribes on destruction
  std::thread outbound_;
  std::atomic<bool> running_{false};
  HANDLE stopEvent_ = nullptr;
};

}  // namespace urnw
