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
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

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
// 2000 ms is 15x the 133 ms measured full healthy teardown, so no honest stop
// can trip it. It is also short enough to fit the two contracts shutdown
// actually has to honour:
//
//   * the SCM is told STOP_PENDING with a 5000 ms wait hint (main.cpp), and
//     worst case here is lock (1000) + network rollback (~150) + SDK (2000)
//     = ~3.2 s, comfortably inside it;
//   * CTRL_CLOSE_EVENT gives ~5 s TOTAL for the whole handler, of which the
//     existing code spends at most 2500 ms waiting for the teardown to drain.
//     The network rollback now completes in phase 1 — inside ~1.2 s worst case
//     — so that wait observes a reverted machine even when the SDK half is
//     still stuck.
//
// And it is short enough for the human: the observed gap between the owner's
// Ctrl+C presses was 2-3 s, so a process that goes within two seconds is gone
// before the second press is a reflex rather than a decision.
inline constexpr std::chrono::milliseconds kSdkTeardownBudget{2000};

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
}  // namespace detail

// The operator has escalated. Collapses every bounded wait in the process to
// kForcedTeardownBudget, including waits that are ALREADY RUNNING — which is
// the whole point, since by definition the escalation arrives while the first
// attempt is still stuck. One-way: there is no un-escalating.
inline void RequestForcedStop() { detail::g_forcedStop.store(true); }
inline bool ForcedStopRequested() { return detail::g_forcedStop.load(); }

// Latched when a bounded teardown ran out of budget and its worker was
// abandoned. Two callers care, for two different reasons:
//
//   * StartLocked, because an abandoned teardown means the previous session's
//     DeviceLocal and wintun adapter are STILL ALIVE on a detached thread.
//     Starting a second session on top of that would create a second adapter
//     carrying the same pinned GUID. It must refuse instead.
//   * main(), because the process must then leave via TerminateProcess rather
//     than by returning through static destructors. Returning would run
//     ExitProcess, which kills the abandoned worker at an arbitrary
//     instruction — possibly inside wintun or the SDK, possibly holding the
//     loader lock — and then runs DLL_PROCESS_DETACH on top of that wreckage.
//     TerminateProcess skips all of it, and by that point the network is
//     already back.
inline void NoteTeardownAbandoned() { detail::g_teardownAbandoned.store(true); }
inline bool TeardownAbandoned() { return detail::g_teardownAbandoned.load(); }

// Test-only. `urnetworkd selftest` exercises the real flags rather than a copy
// of them — a budget mechanism proved against a mock of itself proves nothing —
// so it needs to put them back afterwards. Never called by the service paths.
inline void ResetStopBudgetForTest() {
  detail::g_forcedStop.store(false);
  detail::g_teardownAbandoned.store(false);
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
template <class Work>
bool RunBounded(std::chrono::milliseconds budget, Work work) {
  struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
  };
  auto gate = std::make_shared<Gate>();

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
  if (!done) NoteTeardownAbandoned();
  return done;
}

}  // namespace urnw
