// SPDX-License-Identifier: MPL-2.0
#include "PacketPump.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

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
  // an SDK thread; wintun send is thread-safe. The returned Sub keeps the
  // subscription alive and unsubscribes on Stop()/destruction.
  //
  // The callback captures a shared_ptr to the gate, NOT `this`: closing the
  // subscription does not drain a dispatch that is already running (see the
  // note in the header), so a late callback has to find a live object rather
  // than a freed one. Batching does not soften that — it widens it, because a
  // late dispatch now carries up to a whole batch of work into the window.
  gate_ = std::make_shared<ReceiveGate>();
  gate_->adapter = &adapter_;
  //
  // GUARDED, AND THE THREAD IT RUNS ON IS WHY. This body executes on a thread
  // the SDK created — a Go/cgo callback thread that has never run one line of
  // our startup code, so nothing on it had a terminate handler, and an
  // exception escaping into cgo is the least diagnosable death this process can
  // have. RunGuarded arms the handler once per SDK thread (a thread_local
  // latch, so the per-batch cost is a predicted branch) and names the thread
  // in whatever gets logged. See ThreadGuard.h.
  receiveSub_ = device_.addReceivePacketBatch(
      [gate = gate_, counters = counters_](const uint8_t* packetBatchBytes,
                                           int32_t packetBatchByteCount) {
        RunGuarded("sdk-receive", [&] {
          if (!packetBatchBytes || packetBatchByteCount <= 0) return;
          // ONE LOCK FOR THE WHOLE BATCH, not one per packet. It is uncontended
          // except against Stop(), and taking it once is what keeps "Stop()
          // waits out a callback already inside" true of the whole dispatch
          // rather than of one packet of it — a batch that lost the lock
          // halfway would be a batch half-delivered into a dying adapter.
          std::scoped_lock lock(gate->mutex);
          const size_t byteCount = static_cast<size_t>(packetBatchByteCount);
          size_t offset = 0;
          while (2 <= byteCount - offset) {
            const size_t packetByteCount =
                (static_cast<size_t>(packetBatchBytes[offset]) << 8) |
                static_cast<size_t>(packetBatchBytes[offset + 1]);
            offset += 2;
            if (packetByteCount == 0 || byteCount - offset < packetByteCount)
              return;
            // COUNTED BEFORE THE GATE IS CONSULTED, and that ordering is the
            // measurement. What the failsafe asks is "did anything come back
            // through the tunnel", not "did the adapter still exist when it
            // did" — a packet that arrives during teardown is still proof the
            // tunnel was carrying. Bailing out of this loop on a closed gate
            // would make a stopping pump look like a dead one for the length
            // of the stop.
            //
            // PER PACKET, NOT PER BATCH, on both counters. These numbers are
            // compared against packet-denominated thresholds
            // (kDeadFastOutboundPackets, TunnelWatchdog.h); counting dispatches
            // would silently rescale every one of them.
            counters->inbound.fetch_add(1, std::memory_order_relaxed);
            // null once Stop() has run; the adapter may already be gone. The
            // rest of the batch is still walked, and still counted.
            if (gate->adapter)
              gate->adapter->Send(std::span<const uint8_t>(
                  packetBatchBytes + offset, packetByteCount));
            offset += packetByteCount;
          }
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
  // UNSUBSCRIBE -- which `reset()` does NOT do. This line was
  // `receiveSub_.reset()`, and that is a use-after-free against a live SDK.
  //
  // `urnet::Sub` does not override `reset()`, so it resolved to
  // `detail::Handle::reset()` (urnetwork_sdk.hpp:126-132), which
  //   1. calls `urnet_release(h_)` -- that drops the handle-registry entry ONLY.
  //      It never calls `urnet_sub_close`, so the Go-side subscription installed
  //      by `DeviceLocal::addReceivePacket` stayed registered; then
  //   2. sets `h_ = 0`, which makes `~Sub()` skip its own `urnet_sub_close`
  //      too -- so the subscription leaked for the life of the PROCESS, not
  //      merely the session; and
  //   3. calls `retained_.clear()`, destroying the `shared_ptr<ReceivePacket>`
  //      that is the ONLY owner of the callback installed above.
  //      `addReceivePacket` hands Go the RAW pointer (`receive_packet_fn.get()`,
  //      hpp:16493) and keeps it alive purely through that retain (hpp:16495).
  //
  // Those names and hpp line numbers are the ones from the incident, when this
  // data path was the single-packet API; the call site above is now
  // `addReceivePacketBatch`. Nothing else here changes — the retain/release
  // mechanism, and therefore both the bug and its fix, are identical for the
  // batch subscription.
  //
  // From this line onward, every inbound packet therefore invoked a DESTROYED
  // std::function from a cgo thread -- a freed object that then copies two
  // shared_ptr control blocks and takes a lock. The window is this line to
  // "sdk teardown complete", with the tunnel still carrying: 403ms in one
  // observed teardown and 1434ms in another, over a session that moved ~210k
  // messages.
  //
  // Move-assignment is the operation that does this correctly, and the ORDER is
  // the whole point: `Sub::operator=(Sub&&)` calls `urnet_sub_close(h_)` FIRST
  // (hpp:9681-9687), so Go has dropped the callback before `Handle::operator=`
  // frees it.
  //
  // The gate above still matters and is not redundant: unsubscribing does not
  // drain a dispatch already in flight, so a callback that is mid-body when we
  // close still has to find a live gate and a null adapter.
  receiveSub_ = urnet::Sub{};
  gate_.reset();  // the callback holds its own share until the sdk drops it
  if (stopEvent_) {
    ::CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
  }
  LogInfo("pump: stopped");
}

void PacketPump::OutboundLoop() {
  HANDLE readEvent = adapter_.ReadWaitEvent();
  HANDLE waits[2] = {readEvent, stopEvent_};
  constexpr size_t kMaxPacketCount = 64;
  std::vector<uint8_t> packetBatchBytes;
  packetBatchBytes.reserve(kMaxPacketCount * (2 + 1440));

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
    // therefore refills at least as fast as it drains, the batch below is never
    // empty, and the loop never reaches the WaitForMultipleObjects below that
    // was the ONLY place the stop event was read. Stop() sets running_ false,
    // sets the event, and then joins a thread structurally incapable of
    // noticing either.
    //
    // The livelock is worst precisely when the tunnel is most broken, which is
    // when the operator is most likely to be trying to turn it off.
    //
    // Batching BOUNDS how late that test can be read; it does not remove the
    // need for it. The gather loop is capped at kMaxPacketCount, so the stop
    // flag is now consulted at worst one batch late instead of never.
    while (running_.load()) {
      packetBatchBytes.clear();
      size_t packetCount = 0;
      while (packetCount < kMaxPacketCount) {
        std::span<const uint8_t> packet = adapter_.Receive();
        if (packet.empty()) break;
        if (packet.size() <= UINT16_MAX) {
          const uint16_t packetByteCount = static_cast<uint16_t>(packet.size());
          packetBatchBytes.push_back(
              static_cast<uint8_t>(packetByteCount >> 8));
          packetBatchBytes.push_back(static_cast<uint8_t>(packetByteCount));
          packetBatchBytes.insert(packetBatchBytes.end(), packet.begin(),
                                  packet.end());
          ++packetCount;
        }
        adapter_.ReleaseReceived(packet);
      }
      if (packetBatchBytes.empty()) break;
      // hand the outbound IP packets to the SDK, length-prefixed, in one call
      device_.sendPacketBatch(packetBatchBytes.data(),
                              static_cast<int32_t>(packetBatchBytes.size()));
      // COUNTED AFTER THE SEND, AND IN PACKETS. What the failsafe wants to know
      // is how much the host has committed to a tunnel that is giving nothing
      // back, and a batch counted before the call would be counted even when the
      // call threw or the pump was stopping mid-drain.
      //
      // The UNIT is the part batching could quietly break: kDeadFastOutboundPackets
      // is eight PACKETS (TunnelWatchdog.h), so counting one per sendPacketBatch
      // would let a machine commit up to kMaxPacketCount times that much traffic
      // into a dead tunnel before the fast window arms.
      counters_->outbound.fetch_add(packetCount, std::memory_order_relaxed);
    }
    if (!running_.load()) break;  // re-checked: the drain loop may have exited on it
    DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w != WAIT_OBJECT_0) break;  // stop signaled (or wait failed)
  }
  LogInfo("pump: outbound thread exited");
}

}  // namespace urnw
