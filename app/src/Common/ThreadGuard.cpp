// SPDX-License-Identifier: MPL-2.0
#include "ThreadGuard.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <exception>

#include "Log.h"

namespace urnw {
namespace {

std::atomic<CrashRevertFn> g_revert{nullptr};

// Bare pointers, deliberately. This is read from a terminate handler, where the
// process is already dying and anything that owns memory (a std::string, a
// std::function) is one allocation away from turning a diagnosable death into
// an undiagnosable one. The name is a string literal by contract — see the
// header.
thread_local const char* t_name = nullptr;
thread_local bool t_armed = false;

void RunCrashRevert() {
  if (const CrashRevertFn fn = g_revert.load(std::memory_order_relaxed)) fn();
}

// Bounded, non-allocating, non-deprecated copy. Hand-written rather than
// strncpy (C4996) or strncpy_s (which invokes the invalid-parameter handler on
// truncation, i.e. it can terminate the process — from inside the code that is
// reporting a termination).
void CopyInto(char* out, size_t size, const char* src) {
  if (size == 0) return;
  size_t i = 0;
  for (; src && src[i] && i + 1 < size; ++i) out[i] = src[i];
  out[i] = '\0';
}

// Recover the in-flight exception's text into `out`, and say so when there is
// none. Rethrowing std::current_exception() is the only way to see what the
// exception was from inside a terminate handler.
//
// A FIXED STACK BUFFER, NOT A std::string. A handler that throws while
// reporting a throw produces the exact silence this file exists to end: the
// runtime calls terminate on the terminate handler and the process is gone with
// nothing written. A copy into automatic storage cannot fail.
void DescribeInFlightException(char* out, size_t size) {
  out[0] = '\0';
  try {
    if (const std::exception_ptr e = std::current_exception()) {
      std::rethrow_exception(e);
    }
    CopyInto(out, size,
             "no in-flight exception — an explicit std::terminate, a noexcept "
             "violation, or a throw during unwinding");
  } catch (const std::exception& e) {
    CopyInto(out, size, e.what());
  } catch (...) {
    CopyInto(out, size, "a non-std exception");
  }
}

// The per-thread terminate handler. Ordered revert-then-log for the same reason
// OnUnhandledException in Service/main.cpp is: formatting allocates, and on a
// heap-corruption or stack-overflow death the allocation is the thing most
// likely to fail. Giving the machine its routes back must not be sequenced
// behind it.
[[noreturn]] void OnThreadTerminate() {
  const char* name = t_name ? t_name : "unnamed";
  RunCrashRevert();
  char what[512];
  DescribeInFlightException(what, sizeof(what));
  LogError("thread '{}': std::terminate ON A WORKER THREAD — {}. Reverted "
           "tunnel routes before dying. This handler exists because "
           "std::set_terminate is PER-THREAD on MSVC, so the one armed in wmain "
           "never covered this thread; a death here used to be completely "
           "silent (task #39).",
           name, what);
  std::abort();
}

}  // namespace

void SetThreadGuardCrashRevert(CrashRevertFn revert) {
  g_revert.store(revert, std::memory_order_relaxed);
}

void ArmThreadGuard(const char* threadName) {
  // The latch is what makes this callable from the SDK's per-packet receive
  // callback: after the first packet on a given SDK thread this is one
  // thread_local load and a predicted branch. Without it, every packet would
  // pay for a std::set_terminate.
  if (t_armed) return;
  t_armed = true;
  t_name = threadName;
  std::set_terminate(&OnThreadTerminate);
}

const char* ThreadGuardName() { return t_name ? t_name : "unnamed"; }

namespace detail {

void ReportGuardedThreadFailure(const char* threadName, const char* what) {
  RunCrashRevert();
  LogError("thread '{}': ESCAPED EXCEPTION — {}. Reverted tunnel routes; "
           "aborting rather than continuing, because an invariant this process "
           "cannot name has broken and a service that limps on with one dead "
           "thread reports Up while moving no packets.",
           threadName ? threadName : "unnamed",
           what ? what : "a non-std exception (caught by catch(...))");
  std::abort();
}

}  // namespace detail

}  // namespace urnw
