// SPDX-License-Identifier: MPL-2.0
#include "PacketPump.h"

#include "Log.h"

namespace urnw {

PacketPump::~PacketPump() { Stop(); }

bool PacketPump::Start() {
  stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) {
    // OutboundLoop waits on this handle. With it null the wait fails
    // immediately and the loop exits, so the tunnel would come up, report Up,
    // and never send a single packet from the host. Fail the start instead.
    LogError("pump: CreateEvent failed: {}", ::GetLastError());
    return false;
  }
  running_.store(true);

  // inbound: the SDK delivers decrypted IP packets from the tunnel; write them
  // into the wintun ring so the host stack receives them. The callback fires on
  // an SDK thread; wintun send is thread-safe.
  //
  // The callback captures a shared_ptr to the gate, NOT `this`: closing the
  // subscription does not drain a dispatch that is already running (see the
  // note in the header), so a late callback has to find a live object rather
  // than a freed one.
  gate_ = std::make_shared<ReceiveGate>();
  gate_->adapter = &adapter_;
  receiveSub_ = device_.addReceivePacket(
      [gate = gate_](int64_t /*ipVersion*/, int64_t /*ipProtocol*/,
                     const uint8_t* packet, int32_t len) {
        if (!packet || len <= 0) return;
        std::scoped_lock lock(gate->mutex);
        if (!gate->adapter) return;  // stopped; the adapter may already be gone
        gate->adapter->Send(
            std::span<const uint8_t>(packet, static_cast<size_t>(len)));
      });

  outbound_ = std::thread([this] { OutboundLoop(); });
  LogInfo("pump: started");
  return true;
}

void PacketPump::Stop() {
  if (!running_.exchange(false)) return;
  if (stopEvent_) ::SetEvent(stopEvent_);
  if (outbound_.joinable()) outbound_.join();
  // Close the gate BEFORE unsubscribing, and take the lock to do it: taking the
  // lock is what waits out a callback already inside, which unsubscribing alone
  // does not. After this, no callback can reach the adapter.
  if (gate_) {
    std::scoped_lock lock(gate_->mutex);
    gate_->adapter = nullptr;
  }
  receiveSub_.reset();  // unsubscribe inbound
  gate_.reset();        // the callback holds its own share until the sdk drops it
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
