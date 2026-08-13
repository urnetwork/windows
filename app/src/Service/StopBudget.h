// THE SHUTDOWN BUDGET — what "stop" is allowed to cost, and what happens when
// it costs more.
//
// A VPN THAT CANNOT BE TURNED OFF IS WORSE THAN ONE THAT WILL NOT CONNECT.
//
// This header exists because that sentence stopped being rhetorical. On the
// first run that ever reached UP (2026-08-09 05:31:13), Ctrl+C logged
// "tunnel: stopping (was up)" and then nothing — ever. The owner pressed Ctrl+C
// fifteen more times over eighty seconds, got fifteen identical log lines and
// no progress, and had to kill the process and run `urnetworkd revert` from a
// second elevated prompt to get the machine's network back.
//
// Two things were wrong and they are separable:
//
//   1. THE UNWIND WAS ORDERED WRONG. StopLocked stopped the packet pump BEFORE
//      it gave the routes back, so the one part of shutdown that is cheap,
//      local and safety-critical (routes, DNS, firewall policy) was sequenced
//      behind the one part that can block on a network that has already
//      failed. See the two phases in TunnelController::StopLocked.
//
//   2. NOTHING WAS BOUNDED. Every wait on the stop path — the session lock, the
//      pump's thread join, DeviceLocal::close() — was infinite. An infinite
//      wait is a correct-looking way to write "this machine is now stuck".
//
// What this header provides is the second half: named budgets with a stated
// justification, a way to run a teardown that MIGHT NOT RETURN without being
// held hostage by it, and the escalation ladder for a human hitting Ctrl+C.
//
// WHAT RECOVERY ACTUALLY DEPENDS ON, AND WHAT IT MUST NOT DEPEND ON.
// TerminateProcess runs NO user code: not the unhandled-exception filter, not
// the terminate handler, not the console control handler. When the process dies
// that way the machine still comes back, and not because of anything written
// here: wintun never calls SwDeviceSetLifetime, so process death is a PnP
// surprise removal that takes the adapter and every route pointed at it, and
// the WFP filters go because they live on a dynamic BFE session. That is the
// FLOOR. Everything in this file is about reaching a better outcome than the
// floor, quickly and on purpose — never about replacing it.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "ThreadGuard.h"

namespace urnw {

// --- the budgets -----------------------------------------------------------
//
// Every number here is justified against a MEASURED healthy teardown, not
// guessed. The reference measurement is the `--stop-after=6` unwind, which runs
// byte-for-byte the same StopLocked a user disconnect runs — routes reverted,
// resolver cache flushed, marker cleared, 43 WFP filters removed, DeviceLocal
// closed, wintun adapter closed and the DLL unloaded. It completed in 133 ms
// (05:29:30.936 -> 05:29:31.069) and did so four consecutive times.

// How long Stop() will wait for the session lock before giving up on an orderly
// teardown and reverting the network without it.
//
// An uncontended mutex costs nanoseconds and the longest LEGITIMATE hold on
// this lock is a status query — microseconds. A whole second is six orders of
// magnitude above any honest hold, so it can only expire when someone is
// wedged, which is exactly the case it exists for. TunnelController's own
// header already documents that case ("a connect attempt wedged inside the SDK
// holds mutex_ forever") as the reason the connecting watchdog must take no
// lock; Stop() had no such escape and would simply block before it had logged
// or reverted anything.
inline constexpr std::chrono::milliseconds kStopLockBudget{1000};

// How long the SDK-side teardown gets before it is abandoned.
//
// RAISED FROM 2000 ms TO 6000 ms, and the reason is a measurement, not a taste.
// On 2026-08-11 a tester's machine finished this phase in 2013 ms — THIRTEEN
// MILLISECONDS over the old budget. It was abandoned, and the abandoned worker
// then completed 392 ms later having released everything it held. So the old
// number did not discriminate "wedged" from "healthy"; on that hardware it
// discriminated nothing at all, and the cost of getting it wrong was a service
// that refused to connect until it was restarted by hand.
//
// WHAT THIS BUDGET IS AND IS NOT PROTECTING. It is NOT protecting the operator's
// machine. StopLocked's two phases are ordered so that routes, DNS, the resolver
// cache, the marker and the firewall policy are ALL BACK before this phase is
// even attempted; everything still held here is ours alone. So an overrun costs
// nobody their network — it costs this process, which must then leave by
// TerminateProcess. What the budget actually protects is the SCM's STOP_PENDING
// contract and the operator's patience, and both of those can afford six
// seconds far more easily than a user can afford a bricked Connect button.
//
// WHY 6000 AND NOT MORE, AND NOT UNBOUNDED. 6000 ms is 45x the 133 ms measured
// full healthy teardown and 3x the worst real-world time ever observed, so a
// slow disk, a cold DLL load or a busy machine cannot trip it the way 15x on the
// developer's box did. It stays BOUNDED because abandoning is not free: the
// worker keeps a wintun adapter and a DeviceLocal alive, and the honest recovery
// from that is still a process restart. A budget with no end is just the
// infinite wait this header was written to delete.
//
// The two contracts it has to fit, restated for the new number:
//
//   * the SCM is told STOP_PENDING with kServiceStopWaitHintMillis (main.cpp),
//     which is sized from this constant rather than guessed alongside it —
//     worst case is lock (1000) + both watchdog joins (2x250) + network
//     rollback (~150) + SDK (6000) = ~7.7 s;
//   * CTRL_CLOSE_EVENT gives ~5 s TOTAL for the whole handler, of which the
//     existing code spends at most 2500 ms waiting for the teardown to drain.
//     That wait already could not cover the SDK half at 2000 ms and does not
//     pretend to now: what it covers is PHASE 1, which completes inside ~1.4 s
//     worst case, so the handler still observes a reverted machine and its
//     CrashRevert floor still applies. Unchanged by this raise.
//
// At SYSTEM SHUTDOWN, WaitToKillServiceTimeout (5000 ms by default on Windows
// 10/11) can now expire before a wedged stop returns. That is a deliberate
// trade and it lands on the documented FLOOR at the top of this file: the
// process is killed, the adapter goes as a PnP surprise removal and takes its
// routes with it, and the filters go with the dynamic BFE session. Being killed
// during a wedge costs the machine nothing; refusing to connect afterwards cost
// two testers their VPN.
//
// The human half of the old argument survives intact for the path that actually
// involves a human: a second Ctrl+C collapses this to kForcedTeardownBudget
// (250 ms) mid-wait, so an operator who does not want to wait six seconds never
// has to.
inline constexpr std::chrono::milliseconds kSdkTeardownBudget{6000};

// The STOP_PENDING wait hint handed to the SCM, in milliseconds.
//
// DERIVED HERE rather than typed into main.cpp, because it is not an independent
// number: it is a promise about how long the budgets above can take, and the two
// drifting apart is how a service gets recorded as hung for honouring its own
// design. Worst-case Stop() is kStopLockBudget + both watchdog joins + the SDK
// budget + the ~150 ms rollback ≈ 7.7 s; 12 s leaves headroom for logging and
// for a machine slow enough to be the reason we are here at all. `selftest`
// pins the inequality so a future budget raise cannot silently outgrow it.
inline constexpr unsigned long kServiceStopWaitHintMillis = 12000;

// The same budget once the operator has ESCALATED (second Ctrl+C). They have
// already told us the graceful path failed; the only remaining question is how
// fast we can get out of their way. 250 ms is enough for a teardown that was
// about to finish anyway and far too little for one that is wedged, which is
// precisely the discrimination we want at this point.
inline constexpr std::chrono::milliseconds kForcedTeardownBudget{250};

// How often a bounded wait re-checks whether the operator has escalated. The
// wait is on a condition variable, so this is only the latency of noticing a
// force request that arrives DURING a wait that is already running — not a spin.
inline constexpr std::chrono::milliseconds kBudgetPollInterval{50};

// After the second Ctrl+C, how long the console handler lets the (now
// collapsed) teardown drain before it terminates the process outright. Larger
// than kForcedTeardownBudget by design: the forced teardown has to be able to
// finish and report, or escalation would just be a slower kill.
inline constexpr std::chrono::milliseconds kConsoleForceGrace{1500};

// Exit code for a forced stop. STATUS_CONTROL_C_EXIT — the code Windows itself
// uses when Ctrl+C ends a process — because that is exactly what happened, and
// a recognisable code beats an invented one in a bug report.
inline constexpr unsigned long kForcedStopExitCode = 0xC000013AUL;

// Exit code for a SELF-RESTART: the process ending itself on purpose so the
// SCM's SC_ACTION_RESTART policy brings up a clean one. Application-defined
// (severity=error with the customer bit set, so it can never collide with a
// system NTSTATUS), and deliberately NOT kForcedStopExitCode — "the operator
// pressed Ctrl+C twice" and "this process decided it was finished" are different
// events, and a bug report that cannot tell them apart is a bug report that
// blames the user.
inline constexpr unsigned long kSelfRestartExitCode = 0xE0555201UL;

// How long a self-restart waits before it terminates the process.
//
// Not a safety margin — the machine state is already reverted before anything
// asks for a restart — but a COURTESY WINDOW for the reply that explains it. The
// decision is taken inside an RPC handler with the app blocked on the control
// pipe; terminating on the spot would drop the pipe with the answer still in the
// send buffer, so the app would render "the service went away" instead of "the
// service is restarting itself, hold on". A named pipe write to a local reader
// is microseconds, so a second is three orders of magnitude of slack, and it is
// three orders of magnitude of slack the user spends ONCE, in the rare state
// this exists for.
inline constexpr std::chrono::milliseconds kSelfRestartGrace{1000};

// --- the escalation ladder -------------------------------------------------
//
// Fifteen Ctrl+C presses producing fifteen identical "shutting down" lines and
// no progress is the worst possible feedback: it tells the operator their input
// was received and implies it did something. The ladder below makes each press
// mean something DIFFERENT, and makes the difference visible in the log.
//
// Pure and total so it can be proved by `urnetworkd selftest` on a machine
// where the wedge itself cannot be reproduced.
enum class ConsoleStopAction {
  // First press: the ordinary orderly teardown. Everything the machine needs
  // back comes back on this path.
  Graceful,
  // Second press: collapse the remaining budgets, then give the teardown a
  // short grace to report. Not another graceful attempt — QUEUEING a second
  // identical attempt behind a stuck first one is what produced the fifteen
  // useless log lines.
  Force,
  // Third and beyond: the operator has asked three times. Revert the routes
  // with the lock-free crash path and TerminateProcess. No further waiting, and
  // nothing that could itself block.
  Terminate,
};

// `press` is 1 for the first Ctrl+C/Ctrl+Break of the process, 2 for the
// second, and so on. Values below 1 cannot happen from the counter that feeds
// this, and are treated as the first press: of the three, Graceful is the only
// one that is never destructive, so an impossible input resolves to the safest
// action rather than to a kill.
constexpr ConsoleStopAction DecideConsoleStop(int press) {
  if (press <= 1) return ConsoleStopAction::Graceful;
  if (press == 2) return ConsoleStopAction::Force;
  return ConsoleStopAction::Terminate;
}

namespace detail {
inline std::atomic<bool> g_forcedStop{false};
inline std::atomic<bool> g_teardownAbandoned{false};

// The rendezvous between a bounded wait and the worker it may have to abandon.
//
// HOISTED OUT OF RunBounded, and that is the whole fix for the stale latch. It
// used to be a local type, which meant the only thing that outlived an abandoned
// wait was a process-global bool saying "this happened once". `done` — the flag
// the worker sets when it eventually FINISHES — died with the caller's stack
// frame, so the one fact that could have answered "is it still held?" was thrown
// away at the exact moment it started mattering.
struct TeardownGate {
  std::mutex mutex;
  std::condition_variable cv;
  // Published by the worker AFTER the callable and everything it owns have been
  // destroyed (see the scoping note in RunBounded). That ordering is what makes
  // this readable as "the device, the adapter and the pump are RELEASED", not
  // merely "the worker is nearly done" — and it is the whole reason a start can
  // be allowed on the strength of it.
  bool done = false;
};

// Every abandoned worker that was holding a session's device when we walked away
// from it. Not a bool and not a count: workers can be abandoned more than once
// in a process lifetime and can finish OUT OF ORDER, so the only honest
// representation is the set of gates that have not reported done yet.
//
// LOCK ORDER, WHICH IS LOAD-BEARING: g_heldMutex is acquired BEFORE any
// TeardownGate::mutex, never after. The sweep below walks the vector under
// g_heldMutex and locks each gate inside it; RunBounded therefore has to release
// its gate lock before registering, and does, with a comment saying so.
inline std::mutex g_heldMutex;
inline std::vector<std::shared_ptr<TeardownGate>> g_heldDevices;
}  // namespace detail

// The operator has escalated. Collapses every bounded wait in the process to
// kForcedTeardownBudget, including waits that are ALREADY RUNNING — which is
// the whole point, since by definition the escalation arrives while the first
// attempt is still stuck. One-way: there is no un-escalating.
inline void RequestForcedStop() { detail::g_forcedStop.store(true); }
inline bool ForcedStopRequested() { return detail::g_forcedStop.load(); }

// --- TWO DIFFERENT QUESTIONS, WHICH USED TO BE ONE BOOL ---------------------
//
// This file once answered both of the following with `g_teardownAbandoned`, a
// one-way latch with no way back. They are not the same question, they do not
// have the same answer, and conflating them shipped a bug that bricked Connect
// for two testers on 2026-08-11:
//
//   Q1. "HOW MUST THIS PROCESS LEAVE?"  Asked once, by main(), on the way out.
//       Any thread abandoned anywhere inside the SDK makes unwinding unsafe:
//       returning from wmain runs ExitProcess, which kills that thread at an
//       arbitrary instruction — possibly inside wintun, possibly holding the
//       loader lock — and then runs DLL_PROCESS_DETACH on top of the wreckage.
//       TerminateProcess skips all of it, and by then the network is back.
//       ONE-WAY IS CORRECT HERE. Once a thread has been abandoned this process
//       has permanently lost the right to unwind, no matter what that thread
//       does later, because nothing can prove where it is now.
//
//   Q2. "IS A PREVIOUS SESSION'S DEVICE STILL HELD RIGHT NOW?"  Asked by
//       StartLocked, every time, about the present moment. One-way is WRONG
//       here, and wrongly answered it reads: 08:49:53.903 teardown abandoned
//       thirteen milliseconds over budget -> 08:49:54.335 the abandoned worker
//       finishes and releases everything -> 08:49:56.506 "REFUSING to start —
//       its device is still held". The device had been free for 2.2 seconds.
//       Nothing was held. The latch was stale, not true.
//
// Q1 keeps the flag below. Q2 is answered by SweepAbandonedTeardowns(), which
// re-reads the gates and can therefore say no.

// Q1's latch. Set by every abandonment, of any kind, and never cleared.
inline void NoteTeardownAbandoned() { detail::g_teardownAbandoned.store(true); }
inline bool TeardownAbandoned() { return detail::g_teardownAbandoned.load(); }

// What an abandoned worker would still be HOLDING, which is what decides whether
// it may block the next start.
//
// NoteTeardownAbandoned() is reached from four places and they do not mean the
// same thing. Three of them own nothing the next session needs:
//
//   * TunnelWatchdog's SDK sampler (TunnelWatchdog.cpp), detached when it will
//     not come back. It reads through a raw DeviceLocal* that the channel has
//     ALREADY been cleared of, owns no adapter, no pump and no device, and
//     creates no second wintun GUID. A detached sampler that holds nothing has
//     no business bricking tunnel starts for the life of the process — and if
//     the wedge that stranded it is the SDK-wide one, the real teardown is
//     abandoned too and registers itself here on its own account, so nothing is
//     lost by not counting it twice.
//   * TunnelController::Stop() and ::FailsafeStop() on their lock-free escapes.
//     No worker exists on those paths at all: the session objects are still
//     owned by the controller, behind a mutex_ some wedged operation is holding.
//     A later StartLocked can only run if that lock became available — i.e. if
//     the wedge cleared — and its very first act is StopLocked, which tears the
//     session down through the bounded path and registers a gate here if THAT
//     is abandoned. Refusing on their behalf would be refusing on the strength
//     of a wedge we can prove is over.
//
// Only one owns the hazard: the SDK teardown in TearDownSessionLocked, which was
// handed the DeviceLocal, the wintun adapter and the packet pump by move.
enum class AbandonHazard {
  // The worker owns nothing a later session would collide with. Its abandonment
  // still forces TerminateProcess (Q1) but must not gate a start (Q2).
  //
  // THE DEFAULT, deliberately. A future caller of RunBounded that forgets to
  // classify itself fails towards "does not block starts" rather than towards
  // "brick Connect until reboot", which is the direction this whole change
  // exists to move in.
  ProcessExitOnly,
  // The worker owns the session's DeviceLocal, wintun adapter and packet pump.
  // While it is outstanding, a second session would ask wintun for a second
  // adapter carrying the same pinned GUID while the first still holds it. This
  // is the real hazard and the only thing that may refuse a start.
  HoldsSessionDevice,
};

// The result of asking Q2. Two numbers rather than a bool because the second one
// is the log line the stale-latch bug did not have: an abandoned worker that
// finished late is GOOD NEWS, and a service that silently benefits from it
// leaves the next reader of the log with no way to tell this fix from luck.
struct AbandonedTeardownSweep {
  // Workers still holding a device right now. Non-zero is the ONLY condition
  // that may refuse a start.
  std::size_t outstanding = 0;
  // Workers that were outstanding and have completed since the last sweep.
  // Reported exactly once, on the sweep that observes them.
  std::size_t completed_late = 0;
};

// Register an abandoned worker that is still holding a session's device.
inline void NoteSessionDeviceAbandoned(
    std::shared_ptr<detail::TeardownGate> gate) {
  if (!gate) return;
  std::scoped_lock lock(detail::g_heldMutex);
  detail::g_heldDevices.push_back(std::move(gate));
}

// Re-evaluate Q2 against the present, retiring every worker that has since
// finished. `done` is published only after the worker's owned objects are
// destroyed, so retiring a gate here is a statement that the adapter, the device
// and the pump are RELEASED — not that the worker is nearly done with them.
inline AbandonedTeardownSweep SweepAbandonedTeardowns() {
  AbandonedTeardownSweep result;
  std::scoped_lock lock(detail::g_heldMutex);
  const std::size_t before = detail::g_heldDevices.size();
  std::erase_if(detail::g_heldDevices,
                [](const std::shared_ptr<detail::TeardownGate>& gate) {
                  if (!gate) return true;
                  std::scoped_lock gateLock(gate->mutex);
                  return gate->done;
                });
  result.outstanding = detail::g_heldDevices.size();
  result.completed_late = before - result.outstanding;
  return result;
}

// --- making the refusal recoverable ----------------------------------------
//
// When a device genuinely IS still held, the answer is that this process is
// finished — and the user must not have to find that out from an error message
// containing an sc.exe command line. The installed service has SC_ACTION_RESTART
// configured (InstallService in main.cpp), so a process that ends itself is
// restarted clean within seconds and the app reattaches on its own.
//
// A FUNCTION POINTER RATHER THAN A DIRECT CALL, for the reason
// SetThreadGuardCrashRevert exists: only main() knows whether this run is under
// the SCM (where ending the process IS the recovery) or in a console (where
// nothing would restart it and killing the operator's foreground process would
// be a nasty surprise, not a fix). The handler reports which of those it did.
//
// Returns true if a restart is genuinely coming, so the caller can word what it
// tells the user against what is actually about to happen.
using SelfRestartHandler = bool (*)(const char* why);

namespace detail {
inline std::atomic<SelfRestartHandler> g_selfRestart{nullptr};
inline std::atomic<bool> g_selfRestartPending{false};
}  // namespace detail

inline void SetSelfRestartHandler(SelfRestartHandler handler) {
  detail::g_selfRestart.store(handler);
}

// True once a restart has been ACCEPTED and this process is therefore already
// counting down to its own TerminateProcess.
//
// THIS EXISTS BECAUSE THE COUNTDOWN IS NOT INSTANT. kSelfRestartGrace deliberately
// leaves a second on the clock so the RPC reply explaining the restart reaches the
// app — and a second is long enough for the abandoned worker to finish, for the
// next sweep to answer "nothing is held" and for a start to be allowed through it.
// That start would build a wintun adapter, apply routes, DNS and firewall policy,
// and then be shot in the head mid-bring-up by a terminator armed before it began.
// Recoverable, but only by the floor, and it would look to the user exactly like
// the bug this file is about: press Connect, watch it die for no stated reason.
//
// So the decision is a LATCH, not a re-derivation. Once this process has said it
// is leaving, it is leaving, and nothing may start a tunnel it will not live long
// enough to keep.
inline bool SelfRestartPending() { return detail::g_selfRestartPending.load(); }

inline bool RequestSelfRestart(const char* why) {
  SelfRestartHandler handler = detail::g_selfRestart.load();
  // No handler is not a failure to report loudly here — `selftest` and the
  // one-shot verbs install none because none of them can be in this state. The
  // honest return is "no restart is coming", which is exactly what the caller
  // then tells the user.
  if (!handler) return false;
  const bool restarting = handler(why);
  // Only on TRUE. A console run is told no restart is coming and is left running
  // on purpose, so latching there would refuse every future start for a reason
  // that never arrives — the stale-latch bug again, wearing a different hat.
  if (restarting) detail::g_selfRestartPending.store(true);
  return restarting;
}

// Test-only. `urnetworkd selftest` exercises the real flags rather than a copy
// of them — a budget mechanism proved against a mock of itself proves nothing —
// so it needs to put them back afterwards. Never called by the service paths.
inline void ResetStopBudgetForTest() {
  detail::g_forcedStop.store(false);
  detail::g_teardownAbandoned.store(false);
  detail::g_selfRestartPending.store(false);
  std::scoped_lock lock(detail::g_heldMutex);
  detail::g_heldDevices.clear();
}

// --- running work that might never return ----------------------------------
//
// Runs `work` on its own thread and waits up to `budget` for it. Returns true
// if it finished, false if the budget expired — in which case THE WORKER IS
// ABANDONED, NOT CANCELLED. There is no way to cancel a thread blocked inside a
// cgo call into the SDK, and every mechanism that claims to (TerminateThread,
// APCs) corrupts the process instead.
//
// THE OWNERSHIP CONTRACT, WHICH IS THE ENTIRE SAFETY ARGUMENT:
//
//   `work` MUST OWN EVERYTHING IT TOUCHES, BY VALUE OR BY MOVE. It must capture
//   no reference, no pointer and no `this` belonging to the caller.
//
// That is what makes abandonment safe rather than a use-after-free waiting to
// happen. An abandoned worker keeps running with objects it alone owns; when it
// eventually returns (or never does) nothing else is looking at them. The
// alternative — bounding each sub-call and destroying the objects on timeout —
// would free a wintun adapter out from under a pump thread still writing into
// its ring, inside a LocalSystem service. Leaking an adapter is a wart; that is
// a kernel-adjacent crash.
//
// The wait itself re-checks ForcedStopRequested() so an escalation shortens a
// wait that is already in progress.
//
// `hazard` says what the worker would still be holding if it IS abandoned, and
// therefore whether it may refuse a later start. See AbandonHazard for why the
// default is the one that cannot brick anything.
template <class Work>
bool RunBounded(std::chrono::milliseconds budget, Work work,
                AbandonHazard hazard = AbandonHazard::ProcessExitOnly) {
  auto gate = std::make_shared<detail::TeardownGate>();

  std::thread([gate, work = std::move(work)]() mutable {
    // Scoped so the callable — and therefore everything it owns — is DESTROYED
    // before `done` is published. Signalling first would let the caller return
    // "finished" while an adapter destructor was still running, which is the
    // subtle version of exactly the bug this function exists to prevent.
    //
    // The guard wraps the scope and not the whole lambda, so the destruction
    // order above is unchanged: RunGuarded returns only after `owned` has been
    // destroyed, and `done` is still published after that. This is the teardown
    // path, so it is also the thread most likely to be running when something
    // else in the process is already wrong — an escape here used to abort with
    // no attribution and, being a detached thread, with no crash revert either
    // (ThreadGuard.h).
    RunGuarded("teardown-worker", [&] {
      auto owned = std::move(work);
      owned();
    });
    {
      std::scoped_lock lock(gate->mutex);
      gate->done = true;
    }
    gate->cv.notify_all();
  }).detach();

  const auto start = std::chrono::steady_clock::now();
  std::unique_lock lock(gate->mutex);
  for (;;) {
    if (gate->done) return true;
    const auto effective = ForcedStopRequested()
                               ? std::min(budget, kForcedTeardownBudget)
                               : budget;
    const auto deadline = start + effective;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    // Wake at the deadline OR at the next poll tick, whichever is sooner, so an
    // escalation that arrives mid-wait is noticed within kBudgetPollInterval
    // instead of at the original deadline.
    gate->cv.wait_until(lock, std::min(deadline, now + kBudgetPollInterval));
  }
  const bool done = gate->done;
  // RELEASED BEFORE THE REGISTRATION BELOW, AND THAT IS NOT TIDINESS. The sweep
  // takes g_heldMutex and then each gate's mutex; registering while still
  // holding this gate's mutex would take them in the opposite order and deadlock
  // the two against each other — in the teardown path of a service holding this
  // machine's routes, which is the last place to discover a lock cycle.
  lock.unlock();
  if (!done) {
    // Q1: however this worker ends, this process has lost the right to unwind.
    NoteTeardownAbandoned();
    // Q2: only a worker that is holding the session's device can refuse a later
    // start — and it is the GATE that is retained, not a bool, so `done` remains
    // readable after this frame is gone and the refusal can be withdrawn the
    // moment the worker finishes.
    if (hazard == AbandonHazard::HoldsSessionDevice)
      NoteSessionDeviceAbandoned(gate);
  }
  return done;
}

}  // namespace urnw
