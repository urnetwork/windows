// The decision half of `urnetworkd install`, as PURE FUNCTIONS over what the
// SCM reported.
//
// The install verb is invoked two ways, and the second is why this header
// exists. A developer runs it from an elevated prompt and reads the output; the
// app runs it through a UAC prompt (ShellExecuteEx + runas), where there is no
// console, nobody sees a word it prints, and THE EXIT CODE IS THE ENTIRE
// INTERFACE. A wrong exit code is not a cosmetic bug there: exit 0 with the
// service not running strands the app polling for a Running that never comes,
// and exit 1 with the service running makes it report a failure the user can
// see is false.
//
// So the code that decides "what does this observation mean, what do we tell
// the human, and what do we exit with" is split from the code that makes SCM
// calls, for the same reason ConsoleArgs.h is split from wmain: the decision
// must be testable on a box where performing the action is not allowed.
// `urnetworkd selftest` runs unelevated and must stay that way, so it can never
// register, start or stop a real service — but it can and does assert every row
// of the verdict table below, and the quoting rule, without touching the SCM.
//
// Nothing in this header includes windows.h. The SCM state numbers are restated
// as local constants; main.cpp static_asserts them against the real SERVICE_*
// macros, so a mismatch is a compile error in the one file that has both.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>

namespace urnw::install {

// --- SCM service states, restated ------------------------------------------
//
// Numerically identical to the SERVICE_* constants in winsvc.h (asserted in
// main.cpp). 0 is not a real SCM state; it is this header's sentinel for
// "QueryServiceStatusEx itself failed", which the verdict below must treat as
// its own failure rather than fold into "not running yet".
inline constexpr unsigned long kStateQueryFailed = 0;
inline constexpr unsigned long kStateStopped = 1;       // SERVICE_STOPPED
inline constexpr unsigned long kStateStartPending = 2;  // SERVICE_START_PENDING
inline constexpr unsigned long kStateStopPending = 3;   // SERVICE_STOP_PENDING
inline constexpr unsigned long kStateRunning = 4;       // SERVICE_RUNNING

// --- the wait budgets -------------------------------------------------------
//
// Both waits are bounded because the caller may be a UAC child with no console:
// an install that hangs forever hangs a UAC elevation the user already
// approved, with nothing on screen to say why. Ten seconds is generous against
// measurement — a healthy service reaches RUNNING in well under a second, and
// the measured orderly teardown is ~133ms (StopBudget.h) — so a budget miss
// means something is genuinely wrong, not that the box is slow.
inline constexpr unsigned long kStopWaitBudgetMs = 10000;
inline constexpr unsigned long kStartWaitBudgetMs = 10000;
inline constexpr unsigned long kScmPollIntervalMs = 250;

// --- binPath quoting --------------------------------------------------------
//
// The service's image path MUST be stored quoted. An unquoted path with spaces
// is not merely fragile, it is the classic unquoted-service-path bug: the SCM
// resolves `C:\Program Files\URnetwork\urnetworkd.exe` by trying
// `C:\Program.exe` first, so a writable-root machine gets arbitrary code as
// LocalSystem and everyone else gets a service that may simply fail to start.
// The original CreateServiceW call here passed the raw path; this function is
// the fix, applied on BOTH the create path and the re-point path so the two
// can never diverge.
//
// Always quote, spaces or not: one output shape means the stored config is
// greppable and comparable without knowing which rule produced it. Idempotent,
// so a path that is somehow already quoted is not double-quoted into a name
// that resolves to nothing. A literal '"' cannot occur inside a Windows path
// (the filesystem rejects it), so wrapping is the whole job.
inline std::wstring QuoteServiceBinPath(const std::wstring& path) {
  if (path.empty()) return path;  // nothing to protect; caller errors out first
  if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
    return path;
  return L"\"" + path + L"\"";
}

// --- the start verdict ------------------------------------------------------
//
// After StartServiceW, the install verb polls the state until it sees RUNNING,
// sees STOPPED, or runs out of budget. This function turns that final
// observation into the exit code and the one stderr line. It is total over its
// inputs — every state the SCM can report maps to exactly one row — because
// the failure mode of an ad-hoc if-chain is a state nobody thought about
// falling through to "success".
//
// `pipeBusy` is whether the control pipe had a server at verdict time. It
// matters in exactly one row: a service that started and immediately STOPPED
// while something else holds the pipe did so because Run() refuses to start
// against a live `urnetworkd console` (see the refusal at the top of Run()).
// That is an operator-caused, operator-fixable condition, and a generic "it
// stopped" would send them to a log whose answer is one line here. When the
// state is RUNNING the pipe is busy because the service itself is serving it,
// so that row ignores the flag.
struct StartVerdict {
  int exit_code;       // 0 exactly when the service is RUNNING
  std::wstring error;  // empty on success; otherwise the single stderr line
};

inline StartVerdict JudgeStartWait(unsigned long finalState, bool pipeBusy) {
  if (finalState == kStateRunning) return {0, L""};
  if (finalState == kStateStopped) {
    if (pipeBusy)
      return {1,
              L"install: the service started and immediately stopped because "
              L"another urnetworkd is already serving the control pipe — "
              L"probably a `urnetworkd console` run in some window. The "
              L"service refuses to start against it rather than sweep a live "
              L"tunnel. Stop that console (Ctrl+C in its window), then run "
              L"`urnetworkd install` again."};
    return {1,
            L"install: the service started and then stopped itself — see the "
            L"urnetworkd log for its refusal or failure reason"};
  }
  if (finalState == kStateQueryFailed)
    return {1,
            L"install: could not query the service state after starting it — "
            L"the service may or may not be running; check `sc query "
            L"urnetworkd`"};
  // START_PENDING, STOP_PENDING, or any state numbering surprise: the budget
  // ran out without a terminal answer. Say what was seen; do not guess.
  return {1, L"install: the service did not reach RUNNING within " +
                 std::to_wstring(kStartWaitBudgetMs / 1000) +
                 L"s (last reported state " + std::to_wstring(finalState) +
                 L") — check `sc query urnetworkd` and the urnetworkd log"};
}

// The stop half of the idempotent path: an already-registered service is
// stopped before its binPath is re-pointed, and that wait can fail too. Same
// contract as above — one observation in, one stderr line out. The uninstall
// verb shares this wait-for-STOPPED (DeleteService on an unstopped service
// only marks it delete-pending, poisoning every later verb with
// ERROR_SERVICE_MARKED_FOR_DELETE), so `verb` names whichever one is
// reporting; a text that told the user to re-run `install` after a failed
// uninstall would be advice to undo their own intent.
inline std::wstring StopFailureText(unsigned long finalState,
                                    const wchar_t* verb = L"install") {
  if (finalState == kStateQueryFailed)
    return std::wstring(verb) +
           L": could not query the existing service's state — check "
           L"`sc query urnetworkd` and re-run";
  return std::wstring(verb) + L": the existing service did not stop within " +
         std::to_wstring(kStopWaitBudgetMs / 1000) +
         L"s (last reported state " + std::to_wstring(finalState) +
         L") — stop it yourself (`sc stop urnetworkd`), then run `urnetworkd " +
         verb + L"` again";
}

// Printed when the pipe is already busy BEFORE StartServiceW is even attempted.
// At that point the registered service is stopped (the idempotent path just
// stopped it, or it was never registered), so the only thing that can be
// serving the pipe is a console-mode urnetworkd — and starting the service
// against it would just buy a slower copy of the same refusal.
inline constexpr wchar_t kPipeBusyBeforeStartText[] =
    L"install: not starting the service — another urnetworkd is already "
    L"serving the control pipe (probably `urnetworkd console` in some "
    L"window). The service would refuse to start against it. Stop that "
    L"console first, then run `urnetworkd install` again.";

}  // namespace urnw::install
