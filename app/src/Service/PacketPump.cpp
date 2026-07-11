// SPDX-License-Identifier: MPL-2.0
#include "PacketPump.h"

#include "Log.h"

namespace urnw {

PacketPump::~PacketPump() { Stop(); }

void PacketPump::Start() {
  running_.store(true);
  stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

  // inbound: the SDK delivers decrypted IP packets from the tunnel; write them
  // into the wintun ring so the host stack receives them. The callback fires on
  // an SDK thread; wintun send is thread-safe. The returned Sub keeps the
  // subscription alive and unsubscribes on Stop()/destruction.
  receiveSub_ = device_.addReceivePacket(
      [this](int64_t /*ipVersion*/, int64_t /*ipProtocol*/, const uint8_t* packet,
             int32_t len) {
        if (packet && len > 0) {
          adapter_.Send(std::span<const uint8_t>(packet, static_cast<size_t>(len)));
        }
      });

  outbound_ = std::thread([this] { OutboundLoop(); });
  LogInfo("pump: started");
}

void PacketPump::Stop() {
  if (!running_.exchange(false)) return;
  if (stopEvent_) ::SetEvent(stopEvent_);
  if (outbound_.joinable()) outbound_.join();
  receiveSub_.reset();  // unsubscribe inbound
  if (stopEvent_) {
    ::CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
  }
  LogInfo("pump: stopped");
}

void PacketPump::OutboundLoop() {
  HANDLE readEvent = adapter_.ReadWaitEvent();
  HANDLE waits[2] = {readEvent, stopEvent_};

  while (running_.load()) {
    // Drain everything currently in the ring, then wait for more.
    for (;;) {
      std::span<const uint8_t> packet = adapter_.Receive();
      if (packet.empty()) break;
      // hand the outbound IP packet to the SDK; n is the valid byte count
      device_.sendPacket(packet.data(), static_cast<int32_t>(packet.size()),
                         static_cast<int64_t>(packet.size()));
      adapter_.ReleaseReceived(packet);
    }
    DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w != WAIT_OBJECT_0) break;  // stop signaled (or wait failed)
  }
}

}  // namespace urnw
