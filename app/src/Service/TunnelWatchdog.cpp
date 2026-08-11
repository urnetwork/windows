// SPDX-License-Identifier: MPL-2.0
#include "TunnelWatchdog.h"

#include <chrono>
#include <string_view>

#include "Heartbeat.h"    // PublishedTunnelState — the lock-free state mirror
#include "Log.h"
#include "StopBudget.h"   // NoteTeardownAbandoned, for an abandoned sampler
#include "ThreadGuard.h"

namespace urnw {
namespace {

// THE ONE CLOCK. Steady, because every threshold in this file is a duration and
// a wall clock that jumps — a laptop resuming, an NTP correction — would make
// them all wrong at once, in the direction of a spurious teardown.
int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The tunnel is UP right now, read WITHOUT the session lock.
//
// The heartbeat's mirror is the only lock-free reading of the tunnel's state in
// this process, and it exists for exactly this reason: a connect attempt wedged
// inside the SDK holds TunnelController::mutex_ forever, and that is precisely
// the condition the failsafe most needs to be able to judge. Status() would
// block on the hang it is watching for.
//
// It answers `routes_installed` too, and that is not a shortcut. This watchdog
// is started only after step 6/8 has installed the routes and is cancelled at
// the TOP of StopLocked, before RevertMachineStateLocked runs — so for the whole
// of its life the two facts coincide by construction, and the state mirror
// leaving "up" is the first observable edge of either changing.
bool TunnelIsUp() {
  const char* state = PublishedTunnelState();
  return state != nullptr && std::string_view(state) == "up";
}

}  // namespace

TunnelWatchdog::~TunnelWatchdog() { Stop(); }

void TunnelWatchdog::Start(urnet::DeviceLocal* device,
                           std::shared_ptr<PacketCounters> counters,
                           DeadHandler onDead) {
  if (device == nullptr) return;
  Stop();  // idempotent; also joins anything a previous session left

  auto channel = std::make_shared<WatchdogChannel>();
  channel->device.store(device);
  channel->onDead = std::move(onDead);
  const int64_t upSince = NowMillis();

  {
    std::scoped_lock lock(stateMutex_);
    channel_ = channel;
  }
  armed_.store(false, std::memory_order_relaxed);
  millisToFailsafe_.store(0, std::memory_order_relaxed);

  // Two threads, and the split is the design. The SAMPLER touches the SDK and
  // can therefore wedge; the EVALUATOR touches nothing that can block and is
  // the only thing that reaches a verdict. A single thread would have exactly
  // one failure mode — the wedge — and would be unable to report it.
  sampler_ = StartGuardedThread("tunnel-watchdog-sdk",
                                [this, channel] { RunSampler(channel); });
  evaluator_ = StartGuardedThread(
      "tunnel-watchdog", [this, channel, counters, upSince] {
        RunEvaluator(channel, counters, upSince);
      });

  LogInfo("watchdog: watching this tunnel — the sdk is sampled every {}ms, the "
          "verdict is recomputed every {}ms, and a network change is pushed to "
          "the sdk at most once per {}ms. With the kill switch OFF a tunnel that "
          "cannot carry traffic is turned off automatically rather than left "
          "blocking this machine (no exit for {}s, or {}s of sending with "
          "nothing coming back, or {}s of sdk silence).",
          kSdkSampleIntervalMillis, kEvaluateIntervalMillis,
          kNetworkNotifyDebounceMillis, kDeadSlowMillis / 1000,
          kDeadFastMillis / 1000, kSdkUnresponsiveMillis / 1000);
}

void TunnelWatchdog::Cancel() {
  std::shared_ptr<WatchdogChannel> channel;
  {
    std::scoped_lock lock(stateMutex_);
    channel = channel_;
  }
  if (!channel) return;
  channel->cancelled.store(true);
  // Cleared BEFORE anything waits, so a sampler that wakes late finds nothing
  // to call rather than a device being torn down under it.
  channel->device.store(nullptr);
  {
    // The handler goes NOW, not at the join: a verdict reached in the gap must
    // not re-enter a teardown that is already running. Taking the lock is also
    // what publishes `cancelled` against the two waits.
    std::scoped_lock lock(channel->mutex);
    channel->onDead = nullptr;
  }
  channel->wake.notify_all();
  armed_.store(false, std::memory_order_relaxed);
  millisToFailsafe_.store(0, std::memory_order_relaxed);
}

void TunnelWatchdog::Stop() {
  std::shared_ptr<WatchdogChannel> channel;
  {
    std::scoped_lock lock(stateMutex_);
    // Dropped HERE so NoteNetworkEvent — which runs on a system worker thread
    // that EgressMonitor::Stop() waits for — becomes a no-op immediately
    // instead of queueing behind the joins below.
    channel = std::move(channel_);
    channel_.reset();
  }
  if (channel) {
    channel->cancelled.store(true);
    channel->device.store(nullptr);
    {
      std::scoped_lock lock(channel->mutex);
      channel->onDead = nullptr;
    }
    channel->wake.notify_all();
  }

  // ---- the evaluator ------------------------------------------------------
  //
  // MAY BE THIS VERY THREAD. The verdict is delivered on the evaluator, the
  // handler tears the session down, and the teardown stops the watchdog — so
  // joining unconditionally here would be a thread joining itself. Detaching is
  // sound because RunEvaluator invokes the handler as its LAST act and touches
  // neither this object nor the device afterwards; see the note there.
  //
  // AND IT IS BOUNDED EVEN WHEN IT IS NOT US. Picture the user pressing
  // Disconnect at the same instant the failsafe fires: the RPC thread takes the
  // session lock and arrives here to join, while the evaluator is inside the
  // failsafe handler waiting for that same lock.
  //
  // That is NOT a deadlock — the failsafe's own acquire is timed, so it gives up
  // after kStopLockBudget and this join returns. It is worse than it sounds
  // anyway: this join sits at the TOP of StopLocked, ahead of the route revert,
  // so an unbounded one would hold the machine's routes installed for that whole
  // second — in the exact scenario where somebody is pressing Disconnect because
  // their internet is already gone. Abandoning the evaluator instead lets the
  // revert start now; the teardown it is still trying to run is idempotent
  // against the one running here, and both converge on the same answer.
  if (evaluator_.joinable()) {
    if (evaluator_.get_id() == std::this_thread::get_id()) {
      evaluator_.detach();
    } else if (JoinWithin(channel, channel ? &channel->evaluatorDone : nullptr)) {
      evaluator_.join();
    } else {
      LogWarn("watchdog: the failsafe evaluator did not return within {}ms — it "
              "is inside its own teardown, waiting for the session lock THIS "
              "stop is holding. Letting it run rather than joining it: two "
              "teardowns of one session are idempotent, and a join here would "
              "deadlock the pair.",
              kWatchdogJoinBudgetMillis);
      evaluator_.detach();
    }
  }

  // ---- the sampler, on a budget -------------------------------------------
  //
  // BOUNDED, for kSdkTeardownBudget's reason and by the same argument. A
  // healthy getExits() returns in microseconds, so this can only expire when
  // the SDK is wedged — and a stop that waits on a wedge is the exact failure
  // this whole area exists to remove. There is no way to cancel a thread inside
  // a cgo call, so an expired budget ABANDONS it.
  //
  // Why abandoning is safe rather than a use-after-free waiting to happen: the
  // sampler publishes only into the shared channel, of which it owns a share,
  // and it re-checks `cancelled` immediately on the far side of every SDK call
  // before touching the device again. The one object it could still be inside
  // is the DeviceLocal — and the wedge that stranded it here is the same lock
  // that will strand TearDownSessionLocked's own device->close(), so that
  // teardown is abandoned too and the device is never destroyed. The latch
  // below is what makes the process then leave by TerminateProcess rather than
  // by unwinding through it.
  //
  // UNLIKE the evaluator, this abandonment IS an escalation: the evaluator holds
  // nothing but our own state, while an abandoned sampler is a thread parked
  // inside the SDK forever.
  //
  // AND IT IS THE EXIT LATCH ONLY — IT MUST NOT REFUSE A LATER START. Both
  // questions used to be one bool (StopBudget.h says why they are now two), so
  // a detached sampler bricked every subsequent Connect for the life of the
  // process. It has no business doing that: it OWNS NOTHING. It reads through a
  // raw DeviceLocal* that channel->device was cleared of above, it holds no
  // wintun adapter, no packet pump and no device, and it cannot make wintun
  // issue a second adapter on the pinned guid — which is the entire hazard the
  // start refusal exists for. Nor is anything lost by not counting it: if the
  // wedge that stranded it is the SDK-wide one, TearDownSessionLocked's worker
  // is abandoned on the same lock moments later and registers itself as a real
  // device holder on its own account. A sampler that is merely slow, on a
  // teardown that then completes, is not a reason anybody should have to
  // restart a service.
  if (sampler_.joinable()) {
    if (JoinWithin(channel, channel ? &channel->samplerDone : nullptr)) {
      sampler_.join();
    } else {
      LogError("watchdog: the sdk sampler did not return within {}ms — it is "
               "wedged inside the sdk, so it is being ABANDONED rather than "
               "waited on. This machine's network is not affected by that "
               "(the revert runs ahead of it); what it does mean is that this "
               "process must now exit by TerminateProcess.",
               kWatchdogJoinBudgetMillis);
      sampler_.detach();
      NoteTeardownAbandoned();
    }
  }

  armed_.store(false, std::memory_order_relaxed);
  millisToFailsafe_.store(0, std::memory_order_relaxed);
}

// True when `done` was set inside the budget, i.e. the thread has returned and
// joining it will not block. A null channel or flag means there is nothing to
// wait on, and the honest answer there is "go ahead and join" — a joinable
// thread with no channel cannot exist, and a plain join is the safe reading of
// an invariant that has somehow been broken rather than a null dereference in
// the teardown of a service holding this machine's routes.
bool TunnelWatchdog::JoinWithin(const std::shared_ptr<WatchdogChannel>& channel,
                                std::atomic<bool>* done) {
  if (!channel || !done) return true;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kWatchdogJoinBudgetMillis);
  while (!done->load() && std::chrono::steady_clock::now() < deadline) {
    std::unique_lock lock(channel->mutex);
    channel->wake.wait_for(lock, std::chrono::milliseconds(10));
  }
  return done->load();
}

void TunnelWatchdog::NoteNetworkEvent() {
  std::shared_ptr<WatchdogChannel> channel;
  {
    std::scoped_lock lock(stateMutex_);
    channel = channel_;
  }
  if (!channel || channel->cancelled.load()) return;
  {
    std::scoped_lock lock(channel->mutex);
    channel->coalescer.Observe(NowMillis());
  }
  channel->wake.notify_all();
}

void TunnelWatchdog::Fire(const std::shared_ptr<WatchdogChannel>& channel,
                          DeadTunnelReason reason) {
  DeadHandler handler;
  {
    std::scoped_lock lock(channel->mutex);
    handler = channel->onDead;
  }
  // Outside the lock, and copied first: the handler runs the whole teardown,
  // which calls back into Stop(), which wants this same lock — and which may
  // abandon this very thread partway through, so nothing here may hold anything
  // the stopping thread needs.
  if (handler) handler(reason);
}

// --- the sdk thread ---------------------------------------------------------
//
// Two jobs, both of them cgo calls that can block, which is why they share a
// thread and why that thread is the abandonable one:
//
//   * TELL THE SDK THE NETWORK MOVED. networkChanged() kicks every platform
//     transport in the process so connections bound to the old path re-dial now;
//     notifyNetworkChange() rebases the uplink staleness epoch. NEITHER IS A
//     SUPERSET OF THE OTHER, so both are called, networkChanged() first.
//   * SAMPLE THE EXIT WINDOW, so the verdict has a proven-exit count and — more
//     importantly — so the ABSENCE of a completed sample becomes the wedge
//     detector that needs no cooperation from the SDK at all.
void TunnelWatchdog::RunSampler(std::shared_ptr<WatchdogChannel> channel) {
  // Set on EVERY exit path, including an exception unwinding through
  // RunGuarded, because Stop()'s budget is waiting on it and a missed set means
  // an abandoned thread that had actually finished.
  struct DoneLatch {
    std::shared_ptr<WatchdogChannel> ch;
    ~DoneLatch() {
      ch->samplerDone.store(true);
      ch->wake.notify_all();
    }
  } done{channel};

  // Latched, not per-tick: a device that cannot answer cannot answer every two
  // seconds, and an unlatched warning would bury the log it is meant to help
  // someone read.
  static std::atomic<bool> exitsLogged{false};

  int64_t nextSampleMillis = NowMillis();
  for (;;) {
    bool notifyDue = false;
    int64_t burst = 0;
    {
      std::unique_lock lock(channel->mutex);
      const int64_t now = NowMillis();
      int64_t waitMillis = nextSampleMillis - now;
      if (channel->coalescer.pending()) {
        const int64_t untilNotify = channel->coalescer.deadlineMillis() - now;
        if (untilNotify < waitMillis) waitMillis = untilNotify;
      }
      if (waitMillis > 0) {
        channel->wake.wait_for(lock, std::chrono::milliseconds(waitMillis),
                               [&] { return channel->cancelled.load(); });
      }
      if (channel->cancelled.load()) return;
      notifyDue = channel->coalescer.TakeDue(NowMillis());
      burst = channel->coalescer.lastBurstSize();
    }

    urnet::DeviceLocal* device = channel->device.load();
    if (device == nullptr) return;

    if (notifyDue) {
      LogInfo("watchdog: the os reported an ip/route change ({} event(s) "
              "coalesced) — telling the sdk the network moved, so transports "
              "bound to the path that just went away re-dial now instead of "
              "timing out against it",
              burst);
      try {
        device->networkChanged();
        device->notifyNetworkChange();
      } catch (const std::exception& e) {
        LogWarn("watchdog: the sdk network-change notification failed: {}",
                e.what());
      }
      // The device may have been torn down while we were inside those calls.
      if (channel->cancelled.load()) return;
      device = channel->device.load();
      if (device == nullptr) return;
    }

    if (NowMillis() < nextSampleMillis) continue;
    nextSampleMillis = NowMillis() + kSdkSampleIntervalMillis;

    int64_t proven = 0;
    int64_t total = 0;
    if (auto exits = ReadSdkList(exitsLogged, "getExits",
                                 [&] { return device->getExits(); })) {
      for (const auto& e : *exits) {
        ++total;
        if (e.Proven) ++proven;
      }
    }
    // Re-checked on the far side of the SDK call, BEFORE anything is published:
    // Stop() may have run while we were inside it, and a sample published after
    // the session ended would be evidence about a tunnel that no longer exists.
    if (channel->cancelled.load()) return;

    const int64_t now = NowMillis();
    channel->provenCount.store(proven);
    channel->exitCount.store(total);
    if (proven >= 1) channel->lastProvenMillis.store(now);
    // LAST, and that ordering is the wedge detector's whole contract: this
    // stamp means "a getExits() COMPLETED and its result is already published",
    // never "one was attempted".
    channel->lastSampleMillis.store(now);
  }
}

// --- the verdict thread -----------------------------------------------------
//
// TOUCHES NOTHING THAT CAN BLOCK. No SDK call, no session lock, no I/O but the
// log — which is one unbuffered WriteFile per line. That is what lets it reach
// a verdict about a wedge instead of joining it.
void TunnelWatchdog::RunEvaluator(std::shared_ptr<WatchdogChannel> channel,
                                  std::shared_ptr<PacketCounters> counters,
                                  int64_t upSinceMillis) {
  // Set on every exit path, including one through the failsafe handler, because
  // Stop()'s bounded join is reading it — and the whole reason that join is
  // bounded is the case where this flag is deliberately NOT set for a while.
  struct DoneLatch {
    std::shared_ptr<WatchdogChannel> ch;
    ~DoneLatch() {
      ch->evaluatorDone.store(true);
      ch->wake.notify_all();
    }
  } done{channel};

  TrafficTracker traffic;
  if (counters) {
    traffic.Reset(counters->outbound.load(std::memory_order_relaxed),
                  counters->inbound.load(std::memory_order_relaxed));
  }

  // The instant every window below is measured from. It STARTS as the session's
  // start and is rebased whenever this process is found not to have been
  // running (see kEvaluatorFrozenMillis) — a session nobody watched for eight
  // hours is owed a grace period, not a verdict.
  int64_t sessionStartMillis = upSinceMillis;
  int64_t lastTickMillis = NowMillis();

  for (;;) {
    {
      std::unique_lock lock(channel->mutex);
      if (channel->wake.wait_for(lock,
                                 std::chrono::milliseconds(kEvaluateIntervalMillis),
                                 [&] { return channel->cancelled.load(); })) {
        return;
      }
    }
    if (channel->cancelled.load()) return;

    const int64_t now = NowMillis();

    // ---- was this process actually running for the interval it just waited? --
    //
    // Checked BEFORE anything is measured, because if the answer is no then
    // every age below is the length of a sleep rather than the length of a
    // failure. See kEvaluatorFrozenMillis for why this is not hypothetical on
    // the machine this ships to.
    const int64_t tickGap = now - lastTickMillis;
    lastTickMillis = now;
    if (EvaluatorFroze(tickGap)) {
      LogWarn("watchdog: this process did not run for {}ms (it asked to wait "
              "{}ms) — a modern-standby resume, a hibernate, a suspended vm or "
              "a machine that could not schedule this thread. Every failsafe "
              "window is a duration, so they are all rebased from now: this "
              "tunnel gets a full fresh grace period rather than being torn "
              "down for silence it was never awake to hear. The sdk is being "
              "told the network moved by the os events the resume raises.",
              tickGap, kEvaluateIntervalMillis);
      sessionStartMillis = now;
      if (counters) {
        traffic.Reset(counters->outbound.load(std::memory_order_relaxed),
                      counters->inbound.load(std::memory_order_relaxed));
      }
      armed_.store(false, std::memory_order_relaxed);
      millisToFailsafe_.store(0, std::memory_order_relaxed);
      continue;
    }

    if (counters) {
      traffic.Observe(counters->outbound.load(std::memory_order_relaxed),
                      counters->inbound.load(std::memory_order_relaxed), now);
    }

    DeadTunnelSignals signals;
    signals.tunnelUp = TunnelIsUp();
    signals.routesInstalled = signals.tunnelUp;  // see TunnelIsUp
    signals.upSinceMillis = sessionStartMillis;
    signals.provenCount = channel->provenCount.load();
    // Folded to "never in this session" when they predate a rebase: the sampler
    // publishes across a freeze without knowing one happened, and a sample
    // completed before the machine went away says nothing about the sdk now.
    signals.lastProvenMillis =
        StampInSession(channel->lastProvenMillis.load(), sessionStartMillis);
    signals.lastSampleMillis =
        StampInSession(channel->lastSampleMillis.load(), sessionStartMillis);
    signals.lastInboundMillis = traffic.lastInboundMillis();
    signals.outboundSinceInbound = traffic.outboundSinceInbound();

    const DeadTunnelVerdict verdict = Evaluate(signals, now);
    armed_.store(verdict.armed, std::memory_order_relaxed);
    millisToFailsafe_.store(verdict.millisToFailsafe, std::memory_order_relaxed);
    if (verdict.reason == DeadTunnelReason::None) continue;

    // ---- the loud block ----------------------------------------------------
    //
    // Same register as "tunnel: this machine's network is BACK", because it is
    // the same kind of moment: the service is about to change what this machine
    // can reach, and the operator has to be able to read WHY off the log
    // without reconstructing it.
    LogError("watchdog: ======== THIS TUNNEL IS NOT CARRYING TRAFFIC ========");
    // Every age is measured from sessionStartMillis, not from the session's own
    // start: after a freeze rebase they are ages within the stretch this thread
    // was actually awake for, and those are the only ones the verdict used.
    LogError("watchdog: {} ({}). Session up {}s, watched for {}s; sdk last "
             "answered {}ms ago reporting {} exit(s), {} proven; last proven "
             "{}ms ago; last packet IN from the tunnel {}ms ago; {} packet(s) "
             "sent since then.",
             DescribeDeadTunnel(verdict.reason), StopReasonOf(verdict.reason),
             (now - upSinceMillis) / 1000, (now - sessionStartMillis) / 1000,
             detail::AgeSince(signals.lastSampleMillis, sessionStartMillis, now),
             channel->exitCount.load(), signals.provenCount,
             detail::AgeSince(signals.lastProvenMillis, sessionStartMillis, now),
             detail::AgeSince(signals.lastInboundMillis, sessionStartMillis, now),
             signals.outboundSinceInbound);
    LogError("watchdog: stopping the tunnel through the ORDINARY teardown. With "
             "the kill switch OFF this gives the machine its internet back — "
             "leaving it blocked by a tunnel that cannot carry is the one "
             "outcome this service must never produce. With the kill switch ON "
             "the policy narrows to ARMED instead and the machine STAYS "
             "blocked, because that is what the setting promises; turning the "
             "switch off lifts it immediately. NOTHING IS RECONNECTED "
             "AUTOMATICALLY — the next attempt is the user's.");

    // THE LAST STATEMENT OF THIS THREAD, AND THAT IS A SAFETY PROPERTY.
    //
    // Fire() runs the teardown, which calls Stop(), which either recognises its
    // own thread and detaches this one, or gives up waiting for it and abandons
    // it. Both outcomes mean this thread may outlive the watchdog's interest in
    // it, so everything from here on must touch only the CHANNEL — of which it
    // owns a share — and never this object or the device. `return` and the
    // done-latch above satisfy that; nothing else may be added below.
    Fire(channel, verdict.reason);
    return;
  }
}

}  // namespace urnw
