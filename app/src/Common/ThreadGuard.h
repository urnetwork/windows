// THE PER-THREAD HALF OF THIS PROCESS'S CRASH INSTRUMENTATION.
//
// WHY THIS EXISTS. Service/main.cpp arms two last-chance hooks in wmain:
// SetUnhandledExceptionFilter (process-wide) and std::set_terminate. The second
// one is NOT process-wide. On MSVC std::set_terminate installs a PER-THREAD
// handler — the CRT keeps it in the per-thread data block — so the handler
// armed on the wmain thread is invisible to every thread this service creates
// later. An exception that escapes a worker thread body therefore reaches the
// DEFAULT terminate handler, which calls abort() without running one line of
// ours: no log line naming the thread, no exception text, and — the part that
// costs the owner their network — no NetworkConfig::CrashRevert().
//
// That is not a theoretical gap. urnetworkd has died silently four times with a
// live tunnel, leaving nothing in any channel, and one of those deaths left
// routes installed with no revert. A revert that never ran is exactly what a
// worker-thread terminate looks like from the outside.
//
// WHAT THIS BUYS AND WHAT IT DOES NOT. It does not make the process survive.
// The failure path here ends in std::abort(), which is the same place an
// unguarded escape ends — deliberately, because when an invariant we cannot
// name has broken on a data-path thread, a process that limps on with one dead
// thread is the worse outcome (a tunnel that reports Up and moves no packet).
// What changes is everything that happens BEFORE the abort: the thread is
// named, the exception text is recorded through the unbuffered WriteFile logger
// (Log.cpp writes one WriteFile per line, so it is on disk before we die), and
// the machine's routes are given back. With SEM_NOGPFAULTERRORBOX cleared after
// the Go runtime sets it (see main.cpp), that abort can also reach WER instead
// of vanishing.
//
// THREAD NAMES MUST BE STRING LITERALS. They are stored as a bare const char*
// in thread-local storage and read again from a terminate handler, where
// anything that owns memory is a liability. Pass a literal; never a temporary.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <exception>
#include <thread>
#include <utility>

namespace urnw {

// The revert this process performs on the way out of a fatal thread. Registered
// once from Service/main.cpp as &NetworkConfig::CrashRevert; left null in the
// tray app, which has no routes to give back.
//
// A function pointer rather than a direct call because this header lives in
// Common, which the app links too and which must not depend on the service's
// network layer. It is also the reason the guard is safe in the app: with
// nothing registered, the failure path logs and aborts and touches no network
// state at all.
using CrashRevertFn = void (*)();
void SetThreadGuardCrashRevert(CrashRevertFn revert);

// Arm THIS thread: install the per-thread terminate handler and remember the
// name. Idempotent and nearly free after the first call on a given thread (one
// thread_local bool), which is what makes it safe to call from a per-packet SDK
// callback rather than only from thread entry points.
void ArmThreadGuard(const char* threadName);

// The name armed on this thread, or "unnamed". For diagnostics only.
const char* ThreadGuardName();

namespace detail {
// Log what escaped, revert, abort. `what` is null when the exception was not a
// std::exception.
[[noreturn]] void ReportGuardedThreadFailure(const char* threadName,
                                             const char* what);
}  // namespace detail

// Run `body` on the current thread with the guard armed and everything caught.
//
// The try/catch and the terminate handler cover different things and both are
// needed: the catch handles an ordinary exception escaping the body, the
// terminate handler handles the cases a catch structurally cannot see — a throw
// during unwinding, a noexcept violation, a throw from a destructor, an
// explicit std::terminate — including ones raised BENEATH us, on this thread,
// by code that never returns to this frame.
template <class Fn>
void RunGuarded(const char* threadName, Fn&& body) {
  ArmThreadGuard(threadName);
  try {
    body();
  } catch (const std::exception& e) {
    detail::ReportGuardedThreadFailure(threadName, e.what());
  } catch (...) {
    detail::ReportGuardedThreadFailure(threadName, nullptr);
  }
}

// Start a long-lived thread whose body is guarded. Every std::thread this
// service owns should be created through this, so that a thread added later
// inherits the instrumentation instead of quietly opting out of it.
template <class Fn>
std::thread StartGuardedThread(const char* threadName, Fn body) {
  return std::thread([threadName, body = std::move(body)]() mutable {
    RunGuarded(threadName, body);
  });
}

}  // namespace urnw
