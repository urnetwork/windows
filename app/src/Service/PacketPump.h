// The packet data path, mirroring the macOS PacketTunnelProvider:
//   outbound (host -> tunnel): wintun ring -> DeviceLocal.sendPacket
//   inbound  (tunnel -> host): DeviceLocal receive callback -> wintun ring
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
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

  void Start();
  void Stop();

 private:
  void OutboundLoop();  // wintun -> device

  WintunAdapter& adapter_;
  urnet::DeviceLocal& device_;
  urnet::Sub receiveSub_;  // device -> wintun; unsubscribes on destruction
  std::thread outbound_;
  std::atomic<bool> running_{false};
  HANDLE stopEvent_ = nullptr;
};

}  // namespace urnw
