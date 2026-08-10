// SPDX-License-Identifier: MPL-2.0
#include "PacketPump.h"

#include <chrono>

#include "Log.h"
#include "ThreadGuard.h"

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
  //
  // GUARDED, AND THE THREAD IT RUNS ON IS WHY. This body executes on a thread
  // the SDK created — a Go/cgo callback thread that has never run one line of
  // our startup code, so nothing on it had a terminate handler, and an
  // exception escaping into cgo is the least diagnosable death this process can
  // have. RunGuarded arms the handler once per SDK thread (a thread_local
  // latch, so the per-packet cost is a predicted branch) and names the thread
  // in whatever gets logged. See ThreadGuard.h.
  receiveSub_ = device_.addReceivePacket(
      [gate = gate_](int64_t /*ipVersion*/, int64_t /*ipProtocol*/,
                     const uint8_t* packet, int32_t len) {
        RunGuarded("sdk-receive", [&] {
          if (!packet || len <= 0) return;
          std::scoped_lock lock(gate->mutex);
          if (!gate->adapter) return;  // stopped; the adapter may already be gone
          gate->adapter->Send(
              std::span<const uint8_t>(packet, static_cast<size_t>(len)));
        });
      });

  outbound_ = StartGuardedThread("pump-outbound", [this] { OutboundLoop(); });
  LogInfo("pump: started");
  return true;
}

void PacketPump::Stop() {
  if (!running_.exchange(false)) return;
  // Say so BEFORE the join. This line is not decoration: when this pump wedged
  // for the first time, the whole diagnosis had to be reconstructed from the
  // ABSENCE of "pump: stopped" between "tunnel: stopping (was up)" and eighty
  // seconds of nothing. A shutdown step that can block must announce that it is
  // about to, or its failure is invisible.
  LogInfo("pump: stopping (signalling the outbound thread and joining it)");
  if (stopEvent_) ::SetEvent(stopEvent_);
  const auto joinStart = std::chrono::steady_clock::now();
  if (outbound_.joinable()) outbound_.join();
  const auto joinMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - joinStart)
                          .count();
  // The join is deliberately UNBOUNDED here, and that is not an oversight — it
  // is the safety property. The outbound thread holds references to the wintun
  // adapter and the DeviceLocal; returning from Stop() while it still runs would
  // hand the caller permission to destroy both underneath it. The BOUND lives
  // one level up, in RunBounded (StopBudget.h), which owns this whole teardown
  // and abandons it wholesale rather than dismembering it. See the ownership
  // contract there.
  //
  // What CAN still make this slow is one in-flight DeviceLocal::sendPacket that
  // is blocked in the SDK. One is the worst case, because OutboundLoop now
  // re-checks the stop flag between packets; before that fix the drain loop
  // could refill faster than it drained and never look at the flag at all.
  if (joinMs > 250)
    LogWarn("pump: the outbound thread took {}ms to join — it was blocked "
            "inside the sdk (a send that could not complete), not looping",
            joinMs);
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
    //
    // THE DRAIN LOOP MUST TEST running_ TOO. It used to be `for (;;)`, exiting
    // only when the ring happened to be momentarily empty, and that is the bug
    // that made a shutdown with the tunnel UP unbounded.
    //
    // Consider the state this loop is in exactly when the operator reaches for
    // Ctrl+C. Thirty-one capture routes point the whole machine at the tun, and
    // the tunnel is broken — no exit proven, every transport timing out — so
    // nothing sent is ever acknowledged and the host stack retransmits. The ring
    // therefore refills at least as fast as it drains, `packet.empty()` never
    // becomes true, and the loop never reaches the WaitForMultipleObjects below
    // that was the ONLY place the stop event was read. Stop() sets running_
    // false, sets the event, and then joins a thread structurally incapable of
    // noticing either.
    //
    // The livelock is worst precisely when the tunnel is most broken, which is
    // when the operator is most likely to be trying to turn it off.
    while (running_.load()) {
      std::span<const uint8_t> packet = adapter_.Receive();
      if (packet.empty()) break;
      // hand the outbound IP packet to the SDK; n is the valid byte count
      device_.sendPacket(packet.data(), static_cast<int32_t>(packet.size()),
                         static_cast<int64_t>(packet.size()));
      adapter_.ReleaseReceived(packet);
    }
    if (!running_.load()) break;  // re-checked: the drain loop may have exited on it
    DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w != WAIT_OBJECT_0) break;  // stop signaled (or wait failed)
  }
  LogInfo("pump: outbound thread exited");
}

}  // namespace urnw
