// urnetworkd — the URnetwork Windows service (LocalSystem). Hosts the
// DeviceLocal + wintun tunnel and serves the control pipe. Runs under the SCM in
// production; supports console/install/uninstall/revert for development.
//
// This process rewrites the machine's routes and DNS. Everything it writes
// hangs off the tun interface it creates, and that interface dies with the
// process — see the crash-safety note in NetworkConfig.h. The hooks installed
// here (unhandled exception filter, terminate handler, console control handler)
// are the belt to that braces, and the startup sweep reports when either was
// needed.
//
// SPDX-License-Identifier: MPL-2.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <format>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ConsoleArgs.h"
#include "ControlServer.h"
#include "CrashDumps.h"
#include "Heartbeat.h"
#include "Ids.h"
#include "InstallVerb.h"
#include "Log.h"
#include "NetworkConfig.h"
#include "Paths.h"
#include "Sdk.h"
#include "SelfTest.h"
#include "StopBudget.h"
#include "Strings.h"
#include "ThreadGuard.h"
#include "TunnelController.h"
#include "WfpPolicy.h"

using namespace urnw;

namespace {

constexpr int64_t kServiceMemoryLimit = 64ll * 1024 * 1024;

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;
// Signaled once a console run has finished tearing down, so the console control
// handler knows the network is back before it lets the process die.
HANDLE g_consoleDrainedEvent = nullptr;
ControlServer* g_server = nullptr;

// True only once ServiceMain has been entered, i.e. only when the SCM is what
// started this process and is therefore what can restart it. Read by the
// self-restart handler, which must behave completely differently in a console
// the operator is watching. Set before anything can ask.
std::atomic<bool> g_underScm{false};

const char* StateName(DWORD state) {
  switch (state) {
    case SERVICE_START_PENDING: return "start_pending";
    case SERVICE_RUNNING: return "running";
    case SERVICE_STOP_PENDING: return "stop_pending";
    case SERVICE_STOPPED: return "stopped";
    default: return "?";
  }
}

void SetState(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = exitCode;
  g_status.dwWaitHint = waitHint;
  g_status.dwControlsAccepted =
      (state == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  LogInfo("service: scm state -> {} (exit={})", StateName(state), exitCode);
  if (g_statusHandle) ::SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI HandlerEx(DWORD control, DWORD, LPVOID, LPVOID) {
  // THE SCM CALLS THIS ON ITS OWN THREAD, NOT ON ServiceMain's. Nothing this
  // process ran armed a terminate handler there, so an escape from SetState's
  // formatting or from LogInfo would have reached the default handler: abort,
  // no line, no NetworkConfig::CrashRevert(). This is the thread that carries
  // every stop and shutdown request, which makes it the last one that should be
  // able to die without saying so. Idempotent and ~free after the first call.
  ArmThreadGuard("scm-control");
  switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      LogInfo("service: control {} received",
              control == SERVICE_CONTROL_STOP ? "STOP" : "SHUTDOWN");
      // The hint comes from StopBudget.h rather than being typed here, because
      // it is a promise about the budgets in that header and the two silently
      // disagreeing is how a service gets recorded as hung for taking exactly as
      // long as it was designed to. `selftest` pins hint > worst-case stop.
      SetState(SERVICE_STOP_PENDING, NO_ERROR, kServiceStopWaitHintMillis);
      if (g_stopEvent) ::SetEvent(g_stopEvent);
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

// --- process identity ------------------------------------------------------

// Whether this process's token carries a well-known SID. Used only to report
// what we are running as: creating the wintun adapter and rewriting routes need
// LocalSystem (the SCM path) or at least elevation (a developer console run),
// and "it did nothing" is otherwise indistinguishable from "it was not allowed
// to do anything".
bool TokenHas(WELL_KNOWN_SID_TYPE type) {
  BYTE buffer[SECURITY_MAX_SID_SIZE];
  DWORD size = sizeof(buffer);
  PSID sid = buffer;
  if (!::CreateWellKnownSid(type, nullptr, sid, &size)) return false;
  BOOL member = FALSE;
  if (!::CheckTokenMembership(nullptr, sid, &member)) return false;
  return member != FALSE;
}

std::string DescribeIdentity() {
  if (TokenHas(WinLocalSystemSid)) return "LocalSystem";
  if (TokenHas(WinBuiltinAdministratorsSid)) return "elevated administrator";
  return "UNPRIVILEGED (tunnel operations will fail)";
}

// --- startup / shutdown ----------------------------------------------------

// Anything that must be true before the control server accepts a client.
// Reports state left behind by a previous run: the marker file says the last
// run had routes installed and never took them back, and the sweep finds a tun
// interface that outlived it. Both are expected to be absent; both are loud
// when they are not, because between them they are the only way the owner
// learns that a crash cost them their network.
//
// observeOnly (the unelevated rpc-only mode) REPORTS without mutating: it
// deletes no route, clears no DNS and does not consume the marker. Both halves
// matter. The sweep's DeleteTunnelRoutes/ClearTunnelDns need privilege this
// process does not have, and a mode whose entire promise is "this will not
// touch your network" must not open by rewriting the route table. Consuming the
// marker would be worse than useless: an unelevated developer run would eat the
// evidence that an earlier ELEVATED run died with routes installed, and the
// next real start would then report a clean exit that never happened.
void ReportAndClearPriorState(bool observeOnly) {
  const bool crashed = observeOnly ? TunnelController::PeekActiveMarker()
                                   : TunnelController::TakeActiveMarker();
  const int orphans = NetworkConfig::SweepOrphanedTunnel(
      ids::kTunAdapterGuid, ids::kTunAdapterName, /*remove=*/!observeOnly);
  // Belt to the dynamic session's braces. WfpPolicy registers everything on a
  // session BFE tears down when the process dies, so on a healthy machine this
  // finds nothing — which is exactly why it is worth running: anything it DOES
  // find is static or persistent state that a dynamic session could not have
  // left, i.e. an older build or a hand-installed lockdown. Same observe-only
  // contract as the interface sweep above.
  const int wfpObjects = WfpPolicy::SweepOrphanedObjects(/*remove=*/!observeOnly);
  if (observeOnly) {
    if (crashed || orphans > 0 || wfpObjects > 0) {
      LogWarn("service: leftover tunnel state from a previous run (marker={} "
              "orphaned_interfaces={} wfp_objects={}) — this process is "
              "OBSERVE-ONLY (rpc-only), so nothing was cleaned and the marker "
              "was LEFT IN PLACE for the next real start. If your network is "
              "wrong: STOP THIS PROCESS FIRST, then run `urnetworkd revert` "
              "from an elevated prompt — revert REFUSES while any urnetworkd is "
              "serving the control pipe, including this one.",
              crashed ? "yes" : "no", orphans, wfpObjects);
    } else {
      LogInfo("service: no leftover tunnel state from a previous run "
              "(observe-only: nothing swept, no marker consumed)");
    }
    return;
  }
  if (crashed) {
    LogWarn("service: the previous run exited with tunnel routes installed "
            "(crash, kill, or power loss) — {}",
            orphans > 0 ? "its adapter outlived it; swept the stale routes"
                        : "its adapter was already gone, so the routes went "
                          "with it (nothing to clean)");
  } else if (orphans > 0) {
    LogWarn("service: found {} tun interface(s) from an earlier run with no "
            "active marker; swept them",
            orphans);
  } else if (wfpObjects > 0) {
    LogWarn("service: found {} leftover filter-engine object(s) from an earlier "
            "run with no active marker; purged them",
            wfpObjects);
  } else {
    LogInfo("service: no leftover tunnel state from a previous run");
  }
}

// Is a urnetworkd already serving the control pipe? Two instances cannot both
// serve it (the pipe is single-instance), and the loser's accept loop dies
// quietly — so every entry point refuses up front instead.
//
// Declared HERE, above Run(), rather than next to the dev helpers where it used
// to live, because Run() is the path that most needs it. See the note at the
// top of Run().
bool ControlPipeInUse() {
  if (::WaitNamedPipeW(ids::kControlPipeName, 1)) return true;
  return ::GetLastError() != ERROR_FILE_NOT_FOUND;
}

// Last-chance handlers. The machine's actual restore path is the wintun adapter
// dying with this process; these exist because "actual" is not "verified", and
// because a revert that has already happened costs nothing to attempt twice.
LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
  // Revert BEFORE logging: formatting allocates, and on a stack-overflow or
  // heap-corruption exception the allocation is the thing most likely to fail.
  NetworkConfig::CrashRevert();
  const DWORD code = info && info->ExceptionRecord
                         ? info->ExceptionRecord->ExceptionCode
                         : 0;
  LogError("service: UNHANDLED EXCEPTION {:#x} — reverted tunnel routes before "
           "dying; the adapter teardown will remove the rest",
           code);
  return EXCEPTION_CONTINUE_SEARCH;  // let WER/the SCM restart policy take it
}

// COVERS THE wmain THREAD AND NOTHING ELSE. std::set_terminate is PER-THREAD on
// MSVC — the CRT stores the handler in the per-thread data block — so this one
// is invisible to every thread the service creates afterwards. Its counterpart
// for those is ThreadGuard.h, which arms an equivalent handler as the first
// statement on each of them; the two do the same three things in the same order
// (revert, log, abort) on purpose, so which thread died changes the attribution
// in the log and nothing else.
void OnTerminate() {
  NetworkConfig::CrashRevert();
  LogError("service: std::terminate on the wmain thread — reverted tunnel "
           "routes before dying");
  std::abort();
}

// --- unblinding the crash-reporting channels (task #39) ---------------------

// Give this process back the ability to report a crash, which the Go runtime
// took away before wmain ever ran.
//
// WHAT GO DOES, AND WHEN. runtime.preventErrorDialogs() (runtime/
// signal_windows.go) is called from osinit(), inside schedinit(), as part of the
// Go runtime's own start-up. It ORs SEM_FAILCRITICALERRORS |
// SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX into the process error mode and
// then ORs WER_FAULT_REPORTING_NO_UI into the WER flags. For a c-shared DLL that
// start-up is DLL_PROCESS_ATTACH.
//
// URnetworkSdk.dll is a LOAD-TIME import of urnetworkd.exe. That is verified
// against the shipped binary rather than assumed: `dumpbin /DEPENDENTS
// urnetworkd.exe` lists URnetworkSdk.dll under "Image has the following
// dependencies" and prints no delay-load section at all. So the loader resolves
// it, the Go runtime initialises, and the bit is set before the CRT has run and
// long before wmain gets control.
//
// WHY IT MATTERS MORE THAN ANYTHING ELSE IN THIS FILE. SEM_NOGPFAULTERRORBOX
// makes UnhandledExceptionFilter short-circuit — it returns
// EXCEPTION_EXECUTE_HANDLER without ever invoking WER. With it set, a native
// access violation, an abort() or a fail-fast produces NO WER report, NO
// minidump and NO Application Error 1000, which makes a native death
// byte-for-byte indistinguishable from a clean ExitProcess. Every "there is no
// crash report, so it did not crash" conclusion drawn about task #39 was drawn
// from a channel that had been switched off. This is the prerequisite for every
// other Windows crash tool working at all.
//
// WHAT IS CLEARED AND WHAT IS LEFT. Only SEM_NOGPFAULTERRORBOX.
// SEM_FAILCRITICALERRORS and SEM_NOOPENFILEERRORBOX suppress unrelated dialogs
// (missing removable media, an unopenable file) and clearing them would buy
// nothing while risking a modal box on the machine of someone trying to use
// their VPN. WER_FAULT_REPORTING_NO_UI is left alone too: it suppresses the UI,
// not the report, and under the SCM there is no interactive desktop anyway.
//
// THE SIDE EFFECT, STATED PLAINLY: in `urnetworkd console` — an interactive,
// elevated developer run — a crash can now raise the ordinary Windows
// error-reporting UI instead of the process silently vanishing. For a session
// whose entire purpose is to find out how this thing dies, that is the point.
//
// CALLED AFTER SdkInit, NOT ONLY AT THE TOP OF wmain. With today's load-time
// import the top of wmain is already after the Go runtime, and the call there
// is what covers the whole process life. But if this project ever delay-loads
// the SDK, the runtime would initialise inside the FIRST SDK call — i.e. inside
// SdkInit — and re-set the bit after a wmain-only clear. Anchoring a second
// clear to "just after the SDK has certainly been initialised" is the version
// that cannot silently stop working. That is not hypothetical caution: this
// codebase already paid for exactly that mistake once with GOTRACEBACK, where a
// correct-looking assignment ran unconditionally too late.
//
// Both call sites report the before/after value, so which one actually cleared
// the bit is a fact in the log rather than a claim in this comment. On a
// load-time build the wmain call reports a change and the post-SdkInit call
// reports "already clear"; if that ever inverts, the DLL has become delay-loaded.
void UnblindErrorMode(const char* where) {
  const UINT before = ::GetErrorMode();
  const UINT wanted = before & ~static_cast<UINT>(SEM_NOGPFAULTERRORBOX);
  ::SetErrorMode(wanted);
  const UINT after = ::GetErrorMode();
  LogInfo("service: error mode at {}: {:#x} -> {:#x} (SEM_NOGPFAULTERRORBOX was "
          "{}, is now {}). The Go runtime sets that bit from osinit() at "
          "DLL_PROCESS_ATTACH; while it is set, UnhandledExceptionFilter "
          "short-circuits and a native fault produces no WER report and no "
          "Application Error 1000 — which is why this service's four deaths "
          "left nothing behind.",
          where, before, after,
          (before & SEM_NOGPFAULTERRORBOX) ? "SET (crash reporting blinded)"
                                           : "already clear",
          (after & SEM_NOGPFAULTERRORBOX) ? "STILL SET — the clear did NOT take"
                                          : "clear (crash reporting restored)");
}

// The ~1 Hz heartbeat tick's extra work.
//
// urnet::flushGlog() had exactly ONE call site in this repo before this change
// (WindowTrace.cpp, at the teardown of an opt-in trace). glog buffers, so the
// SDK's own INFO log — the single most likely place for the cause of a death to
// be written — routinely lost its tail: one of the four deaths lost 28 seconds
// of it, which is 28 seconds of exactly the evidence being looked for.
//
// A timer rather than a buffering knob, because there is no buffering knob to
// reach. The generated wrapper exposes setLogDir, setMemoryLimit and flushGlog
// and nothing else — there is no logbuflevel/logbufsecs entry point in
// third_party/urnetwork-sdk/amd64/urnetwork_sdk.hpp, and glog's own flags are
// not addressable from this side of the C ABI. The timer is cheap regardless:
// a flush of already-formatted lines under glog's own lock.
void FlushSdkLogsTick() { urnet::flushGlog(); }

// The last breath.
//
// ITS PRESENCE OR ABSENCE ANSWERS THE ONE QUESTION FOUR INVESTIGATIONS COULD
// NOT SETTLE: did this process leave through the CRT, or was it stopped where
// it stood?
//
//   * present at the end of a log -> the process returned from wmain or called
//     exit(). However surprising the moment, the exit was orderly.
//   * ABSENT -> ExitProcess, TerminateProcess, RaiseFailFastException, an
//     abort() that did not unwind, or a kill from outside. atexit handlers run
//     for none of those.
//
// It writes through the ordinary logger, which is one unbuffered WriteFile per
// line (Log.cpp), so the line is on disk before this function returns.
//
// REGISTERED FROM wmain rather than from ServiceMain. wmain runs first on every
// path, so this covers `selftest`, `install`, `revert` and console mode as well
// as the SCM path — and `selftest` is the only path that can be run to
// completion on a machine that is not allowed to start the real service, which
// makes it the only way to DEMONSTRATE that the handler fires at all.
void OnProcessExit() {
  // Belt for the paths that did not stop the ticker themselves. Bounded, so it
  // cannot turn a shutdown into a hang.
  StopHeartbeat();
  LogInfo("service: ATEXIT — this process is leaving through the CRT's exit "
          "path (a return from wmain, or exit()) after {} of uptime. IF YOU ARE "
          "READING A LOG THAT ENDS WITHOUT THIS LINE, THE PROCESS DID NOT LEAVE "
          "THAT WAY: it was ExitProcess'd, TerminateProcess'd, fail-fast'd or "
          "killed, and that distinction is the whole of task #39.",
          FormatUptime(ProcessUptime().count()));
}

// How many times the operator has asked this process to stop. Windows runs each
// console control event on its OWN thread, so this is genuinely concurrent and
// has to be atomic — and it is also why the escalation below can make progress
// while the main thread is stuck.
std::atomic<int> g_stopPresses{0};

// Give the machine its routes back and kill this process outright. The last rung
// of the ladder, and the only one that cannot itself block.
//
// Everything here is deliberately minimal. CrashRevert takes no lock and issues
// route ioctls only (see NetworkConfig.h for why it skips the DNS clear).
// TerminateProcess then runs NO user code at all — not the exception filter
// above, not the terminate handler, not this handler on a later press — which is
// exactly the property being bought: nothing left in this process can wedge
// again. The rest of the machine's state comes back without us: the wintun
// adapter dies as a pnp surprise removal and takes its routes and dns, and the
// WFP filters go with the dynamic BFE session.
[[noreturn]] void ForceExitNow(const char* why) {
  NetworkConfig::CrashRevert();
  LogError("console: {} — TERMINATING THIS PROCESS NOW. Routes reverted on the "
           "way out; the wintun adapter and every filter die with the process. "
           "If your network still looks wrong, run `urnetworkd revert` from an "
           "elevated prompt.",
           why);
  ::TerminateProcess(::GetCurrentProcess(), kForcedStopExitCode);
  ::ExitProcess(kForcedStopExitCode);  // unreachable; satisfies [[noreturn]]
}

// --- the failure-action policy the self-restart is standing on ---------------
//
// THE THREE-STRIKES CLIFF, WHICH NEARLY MADE THE CURE WORSE THAN THE DISEASE.
// This service used to be registered with SC_ACTION_RESTART, SC_ACTION_RESTART,
// SC_ACTION_NONE and a 24-hour reset period, and those three slots are not three
// retries of one incident — they are a running count of every unexpected death in
// a day, reset only by a full day without one. The third one is SC_ACTION_NONE:
// the process dies and NOTHING BRINGS IT BACK. The counter is shared with real
// crashes (task #39's ntdll AV is exactly the kind of thing that spends a slot),
// so a machine having a bad day can arrive at the self-restart path with its
// budget already gone.
//
// That matters here more than anywhere else, because the self-restart path
// DELIBERATELY KILLS A HEALTHY-ENOUGH PROCESS. If the SCM then declines to bring
// it back, the user is left with no service at all — no tunnel, no disconnect, no
// status, an app polling a pipe that will never answer — which is strictly worse
// than the refusal message that started this, and worse while having just
// promised "reconnecting in a few seconds". A recovery that can strand the user
// is not a recovery.
//
// THE LAST ACTION IS THEREFORE A RESTART, AND THAT IS THE WHOLE FIX. The SCM
// repeats the FINAL entry of the array for every failure past the end of the
// list, so a trailing SC_ACTION_RESTART means "always come back" regardless of
// what the failure count has reached. The delays climb — 5 s, 10 s, 60 s — so the
// pathological case (a process that dies immediately on every start) settles into
// one attempt a minute rather than a hot loop, and the ordinary case still gets
// its first retry in five seconds. For a VPN service that is holding nothing when
// it dies (dynamic BFE session, PnP surprise removal), a minute of downtime is a
// nuisance; never coming back is the product being gone until someone reboots.
bool ApplyRestartOnFailure(SC_HANDLE svc) {
  // The table lives in InstallVerb.h so `selftest` can pin the one property the
  // self-restart is standing on — that the LAST action is a restart — on a
  // machine where no SCM is involved. Translated to winsvc here, which is the
  // only place that knows what SC_ACTION_RESTART is called.
  static_assert(install::RestartsIndefinitely(),
                "the last failure action must be a RESTART: the self-restart "
                "path kills a live process and would strand the machine with no "
                "service at all if the scm declined to bring it back");
  constexpr int n =
      static_cast<int>(sizeof(install::kFailureActions) /
                       sizeof(install::kFailureActions[0]));
  SC_ACTION actions[n]{};
  for (int i = 0; i < n; ++i) {
    actions[i].Type = install::kFailureActions[i].restart ? SC_ACTION_RESTART
                                                          : SC_ACTION_NONE;
    actions[i].Delay = install::kFailureActions[i].delayMs;
  }
  SERVICE_FAILURE_ACTIONS fa{};
  // Unchanged, and now almost decorative: with no SC_ACTION_NONE to fall off the
  // end onto, the reset period only decides which delay a failure gets, not
  // whether it is answered at all.
  fa.dwResetPeriod = install::kFailureResetPeriodSeconds;
  fa.cActions = n;
  fa.lpsaActions = actions;
  return ::ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa) != 0;
}

// Make the promise true immediately before relying on it.
//
// A SELF-RESTART IS A BET ON CONFIGURATION THIS PROCESS DID NOT NECESSARILY
// WRITE. The service on this machine may have been registered by an older build
// (the one with SC_ACTION_NONE in the last slot), by an in-place binary update
// that never re-ran the install verb, or by an administrator with their own
// ideas. Reading the policy out of a comment in InstallService and hoping is how
// the bug this whole change is about happened in the first place: a decision made
// against a stale belief instead of against the present.
//
// So the policy is re-applied here, at the one moment it is about to be depended
// on, and the answer is used. LocalSystem holds SERVICE_ALL_ACCESS in the default
// service DACL, so this costs two SCM handles and no elevation. If it fails, the
// caller does NOT self-terminate — it falls back to the old, honest message that
// asks for a manual restart, because a process that is still running can at least
// still turn the tunnel off.
//
// These are RPCs to services.exe, made on a thread holding TunnelController's
// mutex_, which is a thing worth saying out loud rather than discovering. It is
// acceptable for exactly one reason: the only path that reaches here is a start
// already being refused, and the operation the operator cannot be denied — Stop()
// — takes that lock with a timed acquire and reverts the machine without it if it
// cannot have it (kStopLockBudget). Nothing here can cost anyone their network.
bool EnsureRestartOnFailure() {
  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) {
    LogError("service: cannot open the scm to confirm the restart-on-failure "
             "policy ({}), so a self-restart cannot be proved to come back",
             ::GetLastError());
    return false;
  }
  SC_HANDLE svc = ::OpenServiceW(scm, ids::kServiceName, SERVICE_CHANGE_CONFIG);
  if (!svc) {
    LogError("service: cannot open our own service record to confirm the "
             "restart-on-failure policy ({})",
             ::GetLastError());
    ::CloseServiceHandle(scm);
    return false;
  }
  const bool ok = ApplyRestartOnFailure(svc);
  if (!ok)
    LogError("service: could not set the restart-on-failure policy ({})",
             ::GetLastError());
  ::CloseServiceHandle(svc);
  ::CloseServiceHandle(scm);
  return ok;
}

// --- self-restart: the recovery for a device that really is still held -------
//
// Installed as StopBudget.h's SelfRestartHandler and called from exactly one
// place: TunnelController::StartLocked, when a sweep proves an abandoned SDK
// teardown is STILL holding the previous session's device. Everything else
// about that state is already handled — the machine's routes, DNS and firewall
// policy went back in phase 1, long before anyone got here — and what is left is
// the one thing the process cannot fix from the inside: a wintun adapter held by
// a thread that will not return.
//
// So it stops trying to fix it from the inside. Under the SCM the process ends
// itself, the adapter dies with it as a PnP surprise removal, the failure-action
// policy (ApplyRestartOnFailure — re-confirmed here, not assumed) starts a clean
// one in ~5 s, and the app reattaches on its own. The user presses nothing; the
// error they were shown says "reconnecting in a few seconds" and then it is.
//
// Returns whether a restart is actually coming, so the caller can say something
// true rather than something hopeful.
bool RestartServiceProcess(const char* why) {
  // THE LATCH IS ON THE TERMINATOR, NOT ON THE ASKING, and that distinction is
  // one this file has already been bitten by once. A latch set the moment the
  // question was asked would remember "yes" for a call that answered NO — the
  // console branch, or a restart policy that could not be confirmed — and every
  // later caller would be told a restart is coming that nobody ever armed. Same
  // shape as the stale teardown latch: a fact recorded at the wrong moment, read
  // forever afterwards as if it were the present. So the only thing latched here
  // is the existence of a terminator, and it is latched where one is created.
  static std::atomic<bool> armed{false};
  if (armed.load()) return true;

  if (!g_underScm.load()) {
    // CONSOLE MODE, WHERE KILLING THE PROCESS WOULD BE A SURPRISE, NOT A FIX.
    // Nothing is watching this process to restart it: the operator is, from a
    // terminal, and a foreground process that vanishes on its own is how a
    // developer loses an afternoon deciding whether they crashed it. Say what is
    // wrong and what to press; they have a prompt and they are already looking
    // at it.
    LogError("console: {} — and NOTHING WILL RESTART THIS PROCESS FOR YOU here, "
             "so it is being left running rather than killed under you. Your "
             "network is already back (routes, dns and the firewall policy were "
             "reverted before that teardown was abandoned); what is stuck is one "
             "sdk thread holding the wintun adapter. Press Ctrl+C and run "
             "`urnetworkd console` again. Under the scm this case restarts "
             "itself.",
             why);
    return false;
  }

  // PROVE THE NET IS THERE BEFORE JUMPING. Everything below kills a process that
  // is still answering RPCs, on the strength of a policy stored in the service
  // database — which this build did not necessarily write and which, in the shape
  // it shipped in until now, gives up after the third death in a day. Re-applied
  // and checked here rather than assumed; if it cannot be established, we do not
  // jump, and the user keeps a running service and the old instruction.
  if (!EnsureRestartOnFailure()) {
    LogError("service: {} — but the scm's restart-on-failure policy could NOT be "
             "confirmed, so this process is NOT ending itself. Terminating "
             "without a proven restart would leave this machine with no service "
             "at all: no tunnel, no disconnect, no status, and an app polling a "
             "pipe that never answers. That is worse than the refusal. Your "
             "network is already back; restart the urnetworkd service to clear "
             "the held device.",
             why);
    return false;
  }

  LogError("service: {} — ENDING THIS PROCESS ON PURPOSE so the scm restarts it "
           "clean (SC_ACTION_RESTART, ~5s). This is the recovery, not a crash: "
           "the held wintun adapter dies with the process as a pnp surprise "
           "removal, the next start runs the sweep, and the app reattaches "
           "itself. The machine's routes, dns and firewall policy are ALREADY "
           "back and nothing below can change that. Exiting in {}ms, after the "
           "reply explaining it has reached the app.",
           why, kSelfRestartGrace.count());

  // ON ITS OWN THREAD, because the caller is inside an RPC handler holding the
  // session lock with the app blocked on the control pipe. Terminating on the
  // spot would drop the pipe with the answer still unsent, so the app would show
  // "the service went away" instead of "it is restarting itself".
  //
  // DETACHED and owning nothing — the same contract RunBounded's workers keep,
  // for the same reason: this thread outlives the frame that created it by
  // design, and the process it is going to end is the only thing it touches.
  //
  // The exchange is HERE, at the creation of the one thing worth being unique.
  // Every caller that reaches this line is getting a true "yes"; only the first
  // of them arms anything.
  if (armed.exchange(true)) return true;
  std::thread([] {
    ArmThreadGuard("self-restart");
    std::this_thread::sleep_for(kSelfRestartGrace);
    // A worker that happens to finish inside this grace window does NOT buy a
    // reprieve. The app has already been told a restart is coming and has
    // clamped its surfaces to match; a process that then silently stayed would
    // leave every one of them waiting for a drop that never arrives. One
    // restart costs five seconds. Being wrong about this costs another refusal.
    NetworkConfig::CrashRevert();  // idempotent; a no-op after the orderly path
    // NO SetState(SERVICE_STOPPED) ANYWHERE ON THIS PATH, and that omission is
    // the mechanism. The SCM restarts a service that DIES; one that reports
    // STOPPED is a service that meant to stop, and the failure actions are
    // deliberately not applied to it (Run() relies on exactly that distinction
    // for an ordinary user-requested stop).
    ::TerminateProcess(::GetCurrentProcess(), kSelfRestartExitCode);
  }).detach();
  return true;
}

BOOL WINAPI OnConsoleControl(DWORD type) {
  // Windows runs each console control event on its OWN thread (see g_stopPresses
  // above), created by the OS, which has therefore never armed one of our
  // terminate handlers. Same reasoning as HandlerEx: this is a shutdown path,
  // and a shutdown path that can die silently is how a machine keeps its routes.
  ArmThreadGuard("console-control");
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT: {
      // EACH PRESS MUST MEAN SOMETHING DIFFERENT.
      //
      // This used to log one identical line and re-signal an already-signalled
      // manual-reset event, forever. On the first run that ever reached UP the
      // operator pressed Ctrl+C sixteen times over eighty seconds and got
      // sixteen identical "shutting down" lines, no progress, and no hint that
      // pressing again was pointless — then had to kill the process from another
      // window and run `urnetworkd revert` to get their machine back.
      const char* key = type == CTRL_C_EVENT ? "Ctrl+C" : "Ctrl+Break";
      const int press = g_stopPresses.fetch_add(1) + 1;
      switch (DecideConsoleStop(press)) {
        case ConsoleStopAction::Graceful:
          LogInfo("console: {} — shutting down. This reverts your routes, dns "
                  "and firewall policy FIRST and then releases the sdk. Press "
                  "{} again to force it.",
                  key, key);
          if (g_stopEvent) ::SetEvent(g_stopEvent);
          return TRUE;  // handled; the main thread runs the orderly teardown

        case ConsoleStopAction::Force:
          LogWarn("console: {} AGAIN — ESCALATING. The graceful stop has not "
                  "finished, so every remaining wait in this process is being "
                  "collapsed to {}ms and the sdk teardown will be ABANDONED "
                  "rather than waited on. Your network state is reverted before "
                  "that teardown is even attempted. One more {} terminates this "
                  "process immediately.",
                  key, kForcedTeardownBudget.count(), key);
          RequestForcedStop();
          if (g_stopEvent) ::SetEvent(g_stopEvent);
          // Wait for the (now collapsed) teardown to report. If it drains, the
          // main thread exits on its own and this is a clean forced stop; if it
          // does not, fall through to the kill rather than hand the operator
          // another silent wait.
          if (g_consoleDrainedEvent &&
              ::WaitForSingleObject(
                  g_consoleDrainedEvent,
                  static_cast<DWORD>(kConsoleForceGrace.count())) ==
                  WAIT_OBJECT_0) {
            LogWarn("console: forced teardown completed; exiting");
            return TRUE;
          }
          ForceExitNow("the forced teardown did not drain either");

        case ConsoleStopAction::Terminate:
          ForceExitNow("third stop request");
      }
      return TRUE;
    }
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      // Windows kills the process when this handler returns, and the grace
      // period is about 5s TOTAL for everything this handler does — not 5s on
      // top of what we have already spent. So: revert first, log second, and
      // budget the wait well inside the window rather than at its edge. A
      // 4000ms wait plus a route sweep plus the logging around it can exceed
      // the budget and get us killed mid-revert, which is the one outcome this
      // handler exists to avoid.
      if (g_stopEvent) ::SetEvent(g_stopEvent);
      if (g_consoleDrainedEvent)
        ::WaitForSingleObject(g_consoleDrainedEvent, 2500);
      NetworkConfig::CrashRevert();  // no-op if the orderly teardown got there
      LogWarn("console: window closed / session ending — routes reverted");
      return TRUE;
    default:
      return FALSE;
  }
}

void Run() {
  // THE CONTROL-PIPE CONFLICT IS DETECTED FIRST, BEFORE THE SWEEP.
  //
  // This used to sit further down, at server.Start(), which meant the sweep ran
  // first — and the sweep is destructive. `sc start urnetworkd` against a live
  // `urnetworkd console` tunnel therefore deleted that tunnel's routes and
  // cleared its DNS, while the console kept reporting Up, the pump kept pumping
  // and the tray still said Connected. Every packet would have left in the
  // clear with nothing reporting it. That is precisely the scenario
  // RevertNetwork() below calls unacceptable and refuses to cause — and this
  // was the one path that had no such guard, even though RevertNetwork and
  // RunConsole both did.
  //
  // Ordering is the fix and it costs nothing: if another process holds the
  // pipe, this one cannot serve it and has no business touching the network on
  // the way to finding that out.
  //
  // The check is inherently a TOCTOU — a console could claim the pipe in the
  // window between here and server.Start() — so server.Start() keeps its own
  // failure path below. That is a race between two deliberate acts, and its
  // worst outcome is a service that declines to start. The bug this fixes was
  // the opposite: a race-free, unconditional strip of a live tunnel.
  if (ControlPipeInUse()) {
    LogError("service: REFUSING to start — another urnetworkd is already "
             "serving {} (probably `urnetworkd console`). Nothing was swept and "
             "no route or DNS entry was touched: this process cannot serve the "
             "pipe anyway, and starting the sweep against a LIVE tunnel would "
             "delete its routes and clear its DNS while it kept reporting "
             "Connected. Stop the other instance first.",
             Narrow(ids::kControlPipeName));
    SetState(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    return;
  }

  // Sweep before anything else can fail or block: if a previous run left the
  // machine pointed at a tun that is gone, giving the routes back is more
  // urgent than getting the sdk up.
  ReportAndClearPriorState(/*observeOnly=*/false);
  SdkInit(/*isService=*/true, kServiceMemoryLimit);
  // Immediately after the SDK is up, for the reason spelled out on the function:
  // this is the anchor that survives the SDK becoming delay-loaded.
  UnblindErrorMode("after SdkInit (service)");
  StartHeartbeat(LogDir(/*isService=*/true) / L"heartbeat.txt",
                 &FlushSdkLogsTick);

  ControlServer server;
  g_server = &server;
  if (!server.Start()) {
    LogError("service: control server failed to start (is another urnetworkd "
             "already holding {}?)",
             Narrow(ids::kControlPipeName));
    // The ticker is already running by here; quiesce it before unwinding so it
    // cannot still be writing while static destructors run.
    StopHeartbeat();
    SetState(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    return;
  }
  SetState(SERVICE_RUNNING);
  LogInfo("service: running; waiting for the app on {}",
          Narrow(ids::kControlPipeName));

  ::WaitForSingleObject(g_stopEvent, INFINITE);

  LogInfo("service: stopping");
  const auto stopStart = std::chrono::steady_clock::now();
  server.Stop();  // reverts routes/dns/firewall FIRST, then releases the sdk
  g_server = nullptr;
  // AFTER the teardown, not before it: a death during teardown is exactly as
  // worth timestamping as one during a live session, and the teardown is
  // bounded (~3.2 s, StopBudget.h) so keeping the ticker across it cannot delay
  // anything. StopHeartbeat's own wait is bounded too.
  StopHeartbeat();
  const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - stopStart)
                          .count();
  // Tell the SCM we are stopped BEFORE any forced exit below. A LocalSystem
  // service that vanishes without reporting STOPPED is recorded as a crash and
  // trips the SC_ACTION_RESTART policy — which would start a fresh urnetworkd on
  // a machine whose owner just asked for the VPN to be off.
  SetState(SERVICE_STOPPED);
  if (TeardownAbandoned()) {
    LogError("service: stopped in {}ms with the SDK TEARDOWN ABANDONED. The "
             "machine's routes, dns and firewall policy were reverted; what is "
             "still held is ours alone, on a thread that will not return. "
             "Terminating rather than unwinding — see StopBudget.h.",
             stopMs);
    NetworkConfig::CrashRevert();  // idempotent; a no-op if the orderly path ran
    ::TerminateProcess(::GetCurrentProcess(), kForcedStopExitCode);
  }
  LogInfo("service: stopped cleanly in {}ms", stopMs);
}

// Whether the Go runtime's stderr is being captured to a file, and where. Read
// only by the status/diagnostic paths; set once, on the SCM path, before
// anything else in ServiceMain.
GoCrashCapture g_goCrash;

// Give the Go runtime somewhere durable to die. FIRST THING ON THE SCM PATH,
// ahead of even the control handler registration, because everything after this
// point runs with a live Go runtime underneath it and the only window this
// cannot cover is the one before the process had a chance to run any code at
// all.
//
// This is the SCM path and only the SCM path — see Sdk.h for why console mode
// is deliberately left alone, and what the operator does there instead.
void ArmGoCrashCapture() {
  g_goCrash = RedirectGoCrashOutput(LogDir(/*isService=*/true));
  if (!g_goCrash.armed) {
    LogError("service: could NOT arm the go crash capture at {} ({}). A Go "
             "runtime fatal in this run will print to a stderr the SCM did not "
             "give us and vanish, exactly as in task #39.",
             g_goCrash.path.string(), g_goCrash.error);
  } else {
    LogInfo("service: go crash capture armed -> {} (stays 0 bytes unless the "
            "Go runtime dies; nothing else writes to stderr here)",
            g_goCrash.path.string());
  }
  // The loudest line this service can emit, and the reason the capture is worth
  // having: the start AFTER a crash is the first moment anyone can find out the
  // crash happened, because the crashing process itself got no chance to say so.
  if (!g_goCrash.carried_over.empty()) {
    LogError("service: THE PREVIOUS RUN DIED INSIDE THE GO RUNTIME. It left "
             "{} bytes of fatal output, kept at {}. That file holds the panic "
             "or fatal-error message and the goroutine stacks — it is the "
             "evidence task #39 has been missing. Read it before restarting "
             "anything; the next Go-runtime death overwrites it.",
             g_goCrash.carried_over_bytes, g_goCrash.carried_over.string());
  }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
  // THE BIGGEST HOLE IN THE BELT, AND THE LEAST OBVIOUS ONE.
  //
  // wmain calls std::set_terminate, and on MSVC that handler is PER-THREAD. But
  // StartServiceCtrlDispatcher does not call ServiceMain on the wmain thread —
  // it creates a thread per service entry and calls it there. So on the SCM
  // path, the thread that runs SdkInit, the control server, the whole session
  // and the entire teardown had NO terminate handler at all: an exception
  // escaping any of it reached the default handler, which aborts without one
  // log line and without NetworkConfig::CrashRevert().
  //
  // That is the same silent, revert-less death ThreadGuard was written to end,
  // on the single most important thread in the process, on the only path the
  // owner actually runs. Every worker thread was covered and this one was not.
  //
  // FIRST, ahead of even ArmGoCrashCapture: arming costs a thread-local store
  // and a std::set_terminate, touches nothing else in the process, and cannot
  // be what fails — while ArmGoCrashCapture opens files and formats strings,
  // which is already something worth being covered for.
  ArmThreadGuard("service-main");
  // The SCM started us, so the SCM can restart us — which is what makes ending
  // this process a recovery rather than a disappearance. Set before anything can
  // ask, and never cleared: a process that reached here was launched by the SCM
  // for the whole of its life.
  g_underScm.store(true);
  ArmGoCrashCapture();
  g_statusHandle = ::RegisterServiceCtrlHandlerExW(ids::kServiceName, HandlerEx, nullptr);
  if (!g_statusHandle) {
    LogError("service: RegisterServiceCtrlHandlerEx failed: {}", ::GetLastError());
    return;
  }
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  SetState(SERVICE_START_PENDING, NO_ERROR, 5000);
  g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_stopEvent) {
    LogError("service: CreateEvent failed: {}", ::GetLastError());
    SetState(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    return;
  }
  Run();
  ::CloseHandle(g_stopEvent);
  g_stopEvent = nullptr;
}

// --- dev helpers -----------------------------------------------------------

// The states in InstallVerb.h are restated without winsvc.h; this is the one
// translation unit that sees both, so it is where a divergence becomes a
// compile error instead of a verdict about the wrong state.
static_assert(install::kStateStopped == SERVICE_STOPPED);
static_assert(install::kStateStartPending == SERVICE_START_PENDING);
static_assert(install::kStateStopPending == SERVICE_STOP_PENDING);
static_assert(install::kStateRunning == SERVICE_RUNNING);

// This executable's own absolute path, unbounded. MAX_PATH is not a real
// ceiling on this box — the portable zip can be unpacked anywhere, including
// under a long-path-enabled tree — and a truncated binPath would register a
// service pointing at a file that does not exist.
std::wstring OwnExePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n =
        ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0) return {};
    if (n < path.size()) {
      path.resize(n);
      return path;
    }
    if (path.size() >= 0x8000) return {};  // beyond the NT path limit: give up
    path.resize(path.size() * 2);
  }
}

// Poll until the service reports `wanted`, the budget runs out, or — on a wait
// for RUNNING — it reports STOPPED, which is a terminal verdict worth having
// early: the SCM moves a starting service to START_PENDING before StartServiceW
// returns, so STOPPED here can only mean it came up and refused (see the pipe
// refusal at the top of Run()). lastState always carries what was last seen
// (kStateQueryFailed if the query itself failed) so the caller can say what
// happened instead of only that something did.
//
// A wait for STOPPED also RE-ISSUES the stop control whenever a poll observes
// RUNNING. The caller sent its one stop before this wait, and a service that
// was START_PENDING then refused it (ERROR_SERVICE_CANNOT_ACCEPT_CTRL) — so
// without the retry, the start finishes, the service sits RUNNING and
// perfectly willing to accept the control this function's caller already
// tried, and the whole budget burns down to a failure text telling the user
// to run the `sc stop` that one more ControlService here performs itself.
// Harmless when the handle lacks SERVICE_STOP (the call just fails) and on
// the RUNNING wait (the branch never triggers).
bool WaitForServiceState(SC_HANDLE svc, DWORD wanted, DWORD budgetMs,
                         DWORD& lastState) {
  const ULONGLONG deadline = ::GetTickCount64() + budgetMs;
  for (;;) {
    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    if (!::QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                                &needed)) {
      lastState = install::kStateQueryFailed;
      return false;
    }
    lastState = ssp.dwCurrentState;
    if (lastState == wanted) return true;
    if (wanted == SERVICE_RUNNING && lastState == SERVICE_STOPPED) return false;
    if (wanted == SERVICE_STOPPED && lastState == SERVICE_RUNNING) {
      SERVICE_STATUS st{};
      ::ControlService(svc, SERVICE_CONTROL_STOP, &st);
    }
    if (::GetTickCount64() >= deadline) return false;
    ::Sleep(install::kScmPollIntervalMs);
  }
}

// Ask the SCM to put GOTRACEBACK=crash in the service's environment.
//
// WHY THIS IS HERE AND NOT IN wmain, WHICH IS WHERE IT LOOKS LIKE IT BELONGS.
// GOTRACEBACK controls how much the Go runtime prints when it dies — the
// crashing goroutine only, or every goroutine plus a fail-fast that WER can
// turn into a dump. The runtime reads it in parsedebugvars(), which runs inside
// schedinit(); for a c-shared DLL that is DLL_PROCESS_ATTACH, i.e. while the
// loader is still resolving this exe's imports, BEFORE the CRT has run and long
// before wmain gets control. A _wputenv_s or SetEnvironmentVariableW at the top
// of wmain is therefore not "early" — it is unconditionally too late, and it
// would read like working diagnostics while doing nothing at all.
//
// That is measured, not reasoned: with the DLL loaded first and GODEBUG (the
// other variable parsedebugvars consumes) set afterwards, the runtime emitted
// nothing. It never re-reads them. Worse, Go snapshots the whole environment
// block once at startup into runtime.envs, so a later Win32 write is not merely
// late — it is invisible.
//
// The only actor that can put a variable in this process's environment before
// the loader runs is whoever creates the process. Under the SCM that is the
// SCM, and the documented way to tell it is a REG_MULTI_SZ `Environment` value
// on the service key, which it merges into the environment of every start. So
// this is the correct home for it, and install is the correct moment.
//
// Best-effort by design: it is a diagnostic, and a service that refuses to
// install because a debugging aid could not be configured would be a worse
// trade than the one it is trying to make. Every outcome is logged.
//
// On the tradeoff, because it will be asked: with GOTRACEBACK=crash a Go fatal
// ends in RaiseFailFastException instead of ExitProcess(2), so it does NOT run
// OnUnhandledException and therefore does not get the CrashRevert() belt. That
// is not a regression — ExitProcess(2) runs no exception filter either, so that
// belt has never been on this path. What changes is purely additive: an
// Application Error 1000 and a WER dump where today there is silence.
void SetServiceGoTraceback() {
  // REG_MULTI_SZ: one NUL-terminated entry, then the empty string that ends the
  // list. Spelled as an array so both terminators are visible.
  static constexpr wchar_t kEnvironment[] = L"GOTRACEBACK=crash\0";
  static_assert(kEnvironment[17] == L'\0' && kEnvironment[18] == L'\0',
                "the value must end in a double NUL to be a valid REG_MULTI_SZ");

  const std::wstring key =
      std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + ids::kServiceName;
  HKEY handle = nullptr;
  LSTATUS status =
      ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_SET_VALUE, &handle);
  if (status != ERROR_SUCCESS) {
    LogWarn("install: could not open the service key to set GOTRACEBACK ({}); "
            "a Go runtime fatal will still be captured to go-crash.log, but it "
            "will only carry the crashing goroutine's stack",
            status);
    return;
  }
  status = ::RegSetValueExW(handle, L"Environment", 0, REG_MULTI_SZ,
                            reinterpret_cast<const BYTE*>(kEnvironment),
                            sizeof(kEnvironment));
  ::RegCloseKey(handle);
  if (status != ERROR_SUCCESS) {
    LogWarn("install: could not write the service Environment value ({}); "
            "GOTRACEBACK is unset for the service", status);
    return;
  }
  LogInfo("install: service environment set to GOTRACEBACK=crash — a Go "
          "runtime fatal now dumps every goroutine and fails fast into WER "
          "instead of exiting quietly");
}

// `urnetworkd install` — register the service AND leave it RUNNING, no matter
// what was there before. Idempotent by design: the app's one-click service
// setup fires this through a single UAC prompt for first install, for repair,
// and for the post-update re-point alike, so "already exists" must be a path
// through, not an error. Already registered -> stop it (bounded), re-point its
// binPath at THIS exe, start; not registered -> create + start. Exit 0 exactly
// when the service is RUNNING at the end — the UAC child has no visible
// console, so the exit code is the interface and stdout is a courtesy for the
// human who runs it by hand. The one-line-per-failure stderr text and the exit
// codes are decided by InstallVerb.h, where selftest can reach them.
int InstallService() {
  const std::wstring path = OwnExePath();
  if (path.empty()) {
    std::fwprintf(stderr, L"install: cannot resolve this exe's own path: %lu\n",
                  ::GetLastError());
    return 1;
  }
  const std::wstring binPath = install::QuoteServiceBinPath(path);

  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr,
                                   SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
  if (!scm) {
    std::fwprintf(stderr, L"install: OpenSCManager failed: %lu (run elevated)\n",
                  ::GetLastError());
    return 1;
  }

  bool created = false;
  SC_HANDLE svc = ::OpenServiceW(scm, ids::kServiceName, SERVICE_ALL_ACCESS);
  if (!svc && ::GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST) {
    std::fwprintf(stderr, L"install: OpenService failed: %lu (run elevated)\n",
                  ::GetLastError());
    ::CloseServiceHandle(scm);
    return 1;
  }
  if (svc) {
    // Already registered — possibly running, possibly pointing at an exe that
    // an update just renamed to .old. Stop it first: ChangeServiceConfig on a
    // running service succeeds but only applies at the NEXT start, which would
    // make this verb report success while the old binary keeps running.
    SERVICE_STATUS st{};
    if (!::ControlService(svc, SERVICE_CONTROL_STOP, &st) &&
        ::GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
      // START_PENDING / STOP_PENDING cannot accept a stop control; the bounded
      // wait below is the arbiter either way, so this is not an error yet.
      LogWarn("install: stop control not accepted ({}); waiting on the state",
              ::GetLastError());
    }
    DWORD state = install::kStateQueryFailed;
    if (!WaitForServiceState(svc, SERVICE_STOPPED, install::kStopWaitBudgetMs,
                             state)) {
      const std::wstring text = install::StopFailureText(state);
      std::fwprintf(stderr, L"%s\n", text.c_str());
      LogError("install: {}", Narrow(text));
      ::CloseServiceHandle(svc);
      ::CloseServiceHandle(scm);
      return 1;
    }
    if (!::ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS,
                                SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                binPath.c_str(), nullptr, nullptr, nullptr,
                                nullptr, nullptr, ids::kServiceDisplayName)) {
      std::fwprintf(stderr, L"install: ChangeServiceConfig failed: %lu\n",
                    ::GetLastError());
      ::CloseServiceHandle(svc);
      ::CloseServiceHandle(scm);
      return 1;
    }
    std::wprintf(L"%s already registered — stopped and re-pointed at %s\n",
                 ids::kServiceName, binPath.c_str());
  } else {
    svc = ::CreateServiceW(
        scm, ids::kServiceName, ids::kServiceDisplayName, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        binPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) {
      std::fwprintf(stderr, L"install: CreateService failed: %lu\n",
                    ::GetLastError());
      ::CloseServiceHandle(scm);
      return 1;
    }
    created = true;
    std::wprintf(L"registered %s -> %s\n", ids::kServiceName, binPath.c_str());
  }
  // Crash recovery on BOTH paths, so a re-install converges to the same config
  // a fresh install gets (plan M4): restart on failure, and the restarted
  // instance runs the startup sweep, so a crash that DID leave an adapter
  // behind gets cleaned within the restart delay. The policy itself lives in
  // ApplyRestartOnFailure, shared with the self-restart path — see the note
  // there for why its last action must be a RESTART and not a NONE.
  ApplyRestartOnFailure(svc);

  // Also on both paths, and BEFORE the StartService below, so the start this
  // verb performs is already covered rather than the one after it.
  SetServiceGoTraceback();

  // The registered service is stopped by now on every path, so a busy control
  // pipe can only be a console-mode urnetworkd. The service refuses to start
  // against one (Run() checks the pipe before its destructive sweep) — starting
  // it anyway would spend the whole wait budget buying a slower copy of that
  // refusal, so say the real reason now.
  if (ControlPipeInUse()) {
    std::fwprintf(stderr, L"%s\n", install::kPipeBusyBeforeStartText);
    LogError("install: {}", Narrow(install::kPipeBusyBeforeStartText));
    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return 1;
  }

  if (!::StartServiceW(svc, 0, nullptr)) {
    std::fwprintf(stderr, L"install: StartService failed: %lu\n",
                  ::GetLastError());
    ::CloseServiceHandle(svc);
    ::CloseServiceHandle(scm);
    return 1;
  }
  DWORD state = install::kStateQueryFailed;
  WaitForServiceState(svc, SERVICE_RUNNING, install::kStartWaitBudgetMs, state);
  // Query the pipe only for the verdict's STOPPED row; when the service is
  // RUNNING the pipe is busy because the service itself serves it, and
  // JudgeStartWait ignores the flag there.
  const install::StartVerdict verdict =
      install::JudgeStartWait(state, ControlPipeInUse());
  if (verdict.exit_code == 0) {
    std::wprintf(L"%s %s and RUNNING (binPath %s)\n", ids::kServiceName,
                 created ? L"installed" : L"updated", binPath.c_str());
    LogInfo("install: {} {} and running", Narrow(ids::kServiceName),
            created ? "installed" : "updated");
  } else {
    std::fwprintf(stderr, L"%s\n", verdict.error.c_str());
    // The UAC child's stderr is invisible; the log is where this line can
    // actually be read after the fact.
    LogError("install: {}", Narrow(verdict.error));
  }
  ::CloseServiceHandle(svc);
  ::CloseServiceHandle(scm);
  return verdict.exit_code;
}

int UninstallService() {
  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) {
    std::fwprintf(stderr, L"OpenSCManager failed: %lu (run elevated)\n", ::GetLastError());
    return 1;
  }
  // QUERY_STATUS on top of DELETE | STOP: this verb now WAITS for STOPPED
  // before deleting, mirroring the install verb's stop half. DeleteService on
  // a service that has not stopped "succeeds" by marking it delete-pending —
  // the verb would print "uninstalled" and exit 0 over a service still
  // RUNNING, the app's AwaitState(NotInstalled) poll would time out against
  // it, and every later install OR uninstall click would fail with
  // ERROR_SERVICE_MARKED_FOR_DELETE until the service happens to stop. A
  // crash-looping service the user is trying to remove (SCM failure-actions
  // keep restarting it, so it is often START_PENDING — the one state that
  // refuses a stop control) is exactly the service this verb gets pointed at.
  SC_HANDLE svc = ::OpenServiceW(scm, ids::kServiceName,
                                 DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
  int rc = 1;
  if (svc) {
    SERVICE_STATUS st{};
    if (!::ControlService(svc, SERVICE_CONTROL_STOP, &st) &&
        ::GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
      // Same tolerance as the install verb: a pending state cannot accept the
      // control yet; the bounded wait below re-issues it when it can be.
      LogWarn("uninstall: stop control not accepted ({}); waiting on the state",
              ::GetLastError());
    }
    DWORD state = install::kStateQueryFailed;
    if (!WaitForServiceState(svc, SERVICE_STOPPED, install::kStopWaitBudgetMs,
                             state)) {
      const std::wstring text = install::StopFailureText(state, L"uninstall");
      std::fwprintf(stderr, L"%s\n", text.c_str());
      LogError("uninstall: {}", Narrow(text));
      ::CloseServiceHandle(svc);
      ::CloseServiceHandle(scm);
      return 1;
    }
    rc = ::DeleteService(svc) ? 0 : 1;
    if (rc != 0) {
      std::fwprintf(stderr, L"uninstall: DeleteService failed: %lu\n",
                    ::GetLastError());
    }
    ::CloseServiceHandle(svc);
  } else {
    std::fwprintf(stderr, L"uninstall: OpenService failed: %lu\n",
                  ::GetLastError());
  }
  ::CloseServiceHandle(scm);
  std::wprintf(L"%s\n", rc == 0 ? L"uninstalled" : L"uninstall failed");
  return rc;
}

// Give the machine its network back without starting anything. The escape hatch
// for the one failure this service must never leave unfixable: a tun adapter
// that outlived the process that owned it, still holding the default routes.
//
// It REFUSES while a urnetworkd is running, and that refusal is the whole
// safety of the command. SweepOrphanedTunnel resolves the pinned GUID against
// the LIVE interface table, so a healthy tunnel matches it exactly as an
// orphan does: running this against a live tunnel deletes all 31 routes and
// clears the tun DNS underneath it, and nothing notices. The controller still
// reports Up, the pump keeps pumping, the tray still says Connected — and every
// packet leaves in the clear. A command the owner reaches for while confused
// must not be able to silently unprotect them.
int RevertNetwork(bool force) {
  LogSetConsoleEcho(true);
  if (!TokenHas(WinLocalSystemSid) && !TokenHas(WinBuiltinAdministratorsSid)) {
    LogError("revert: must run elevated to change routes");
    return 1;
  }
  if (ControlPipeInUse()) {
    if (!force) {
      LogError(
          "revert: REFUSED — a urnetworkd is running (it is serving {}). This "
          "command cannot tell a live tunnel from an orphaned one: it would "
          "delete the routes and DNS out from under a tunnel that keeps "
          "running, and your traffic would leave unencrypted with nothing "
          "reporting it. Stop the service first (sc stop urnetworkd), then "
          "re-run. Use `urnetworkd revert --force` only if you know the tunnel "
          "is already dead.",
          Narrow(ids::kControlPipeName));
      return 1;
    }
    LogWarn("revert: --force with a urnetworkd RUNNING. If its tunnel is up, "
            "traffic will fall back to the clear while it still reports "
            "Connected. Stop the service and check the tray.");
  }
  LogInfo("revert: reverting any leftover tunnel network state");
  // Only consume the marker when nothing is running: the marker belongs to the
  // live process, and taking it would make its next start report a clean exit
  // it did not have.
  const bool marker = ControlPipeInUse() ? false : TunnelController::TakeActiveMarker();
  const int orphans = NetworkConfig::SweepOrphanedTunnel(ids::kTunAdapterGuid,
                                                         ids::kTunAdapterName);
  // The filter engine is the second place state can be stranded. With a dynamic
  // session there should be nothing here — the point of reporting it is that a
  // non-zero count means something OTHER than a dynamic session installed it.
  const int wfpObjects = WfpPolicy::SweepOrphanedObjects(/*remove=*/true);
  LogInfo("revert: marker={} orphaned_interfaces={} wfp_objects={}",
          marker ? "yes" : "no", orphans, wfpObjects);
  if (orphans == 0)
    LogInfo("revert: no URnetwork tun interface present; nothing of ours is in "
            "the route table");
  if (wfpObjects == 0)
    LogInfo("revert: no URnetwork filter-engine objects present; nothing of "
            "ours is blocking traffic");
  return 0;
}

// Run the service logic in the console for local debugging (not under SCM).
//
// rpcOnly clamps the process so that no start_tunnel from any client can reach
// the destructive half of TunnelController::StartLocked. That is what makes it
// safe — and useful — to run unelevated: the app gets a live DeviceRemote and
// the machine's routes and DNS are never written.
//
// stopAfterStep (0 = absent) is the DEBUG-ONLY staged bring-up flag. It halts
// every start_tunnel after step N of 8 and unwinds through the ordinary
// teardown. It only ever narrows: it enables nothing, and where it overlaps with
// rpcOnly the clamp still wins (see EffectiveStopStep in ConsoleArgs.h).
int RunConsole(bool rpcOnly, int stopAfterStep = 0) {
  LogSetConsoleEcho(true);
  LogInfo("console: starting as {}{}{}", DescribeIdentity(),
          rpcOnly ? " in RPC-ONLY mode" : "",
          stopAfterStep ? std::format(" with STAGED BRING-UP (--stop-after={})",
                                      stopAfterStep)
                        : std::string());
  if (stopAfterStep) {
    // Before the rpc-only banner, because when both are given the reader needs
    // to know the sequence is being cut short BEFORE they read what rpc-only
    // promises about a session that will not survive to be used.
    const int effective = EffectiveStopStep(stopAfterStep, rpcOnly);
    if (effective < stopAfterStep) {
      // The requested stop point is BEYOND the mode's own ceiling, so it is
      // never reached and the flag never fires: rpc-only returns at the fence
      // after step 5 and the session stays up as a normal rpc-only session.
      // Said plainly, because "stop after 7" that silently does nothing would
      // otherwise be read as "it stopped, so the teardown was exercised".
      LogWarn("console: --stop-after={} is BEYOND what this process can reach. "
              "--rpc-only already ends the sequence at step {}/8 by returning "
              "at the fence, which is before step {}/8 — so this flag NEVER "
              "FIRES here and no staged teardown happens. The session ends as "
              "an ordinary rpc-only session. The clamp wins; --stop-after "
              "cannot raise its ceiling. Drop --rpc-only (and run elevated) if "
              "you meant to reach step {}/8.",
              stopAfterStep, kRpcOnlyCeilingStep, stopAfterStep, stopAfterStep);
    } else {
      LogWarn("console: --stop-after={} — DEBUG FLAG. Every start_tunnel this "
              "process serves runs steps 1/8..{}/8 and then STOPS, unwinding "
              "through the same teardown a user disconnect runs. It cannot make "
              "this process do MORE than it otherwise would: it only ever stops "
              "the sequence earlier. Nothing here is enabled by the flag; steps "
              "still run only if the mode and this process's privileges allow "
              "them.",
              stopAfterStep, effective);
    }
    if (!rpcOnly && stopAfterStep >= 6) {
      LogWarn("console: --stop-after={} is AT OR PAST STEP 6/8, the first call "
              "that rewrites this machine's routes and DNS. Everything up to and "
              "including step {}/8 really runs. Have a baseline capture and a "
              "second network path before you drive this.",
              stopAfterStep, stopAfterStep);
    }
  }
  if (rpcOnly) {
    LogWarn("console: --rpc-only — this process will bring up the DeviceLocal "
            "and the mTLS rpc listener the app dials, and will NOT create a "
            "wintun adapter, write a route, set a dns server or move a packet. "
            "The startup sweep is observe-only here, so it will not delete a "
            "stale route or consume the crash marker either. No elevation is "
            "needed and none is used. Nothing here can connect you to anything; "
            "the app will report state 'rpc_only', not 'up'.");
  } else if (!TokenHas(WinLocalSystemSid) && !TokenHas(WinBuiltinAdministratorsSid)) {
    LogError("console: NOT elevated — creating the wintun adapter and writing "
             "routes both require LocalSystem/administrator, so a start_tunnel "
             "will fail at step 1. Re-run from an elevated prompt (or use "
             "`urnetworkd install`, which registers the service and starts it), "
             "or run `urnetworkd console --rpc-only`, which needs no elevation.");
  }
  if (ControlPipeInUse()) {
    LogError("console: {} is already served — another urnetworkd (probably the "
             "installed service) is running. Stop it first: sc stop urnetworkd",
             Narrow(ids::kControlPipeName));
    return 1;
  }

  g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_consoleDrainedEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_stopEvent || !g_consoleDrainedEvent) {
    LogError("console: CreateEvent failed: {}", ::GetLastError());
    return 1;
  }
  ::SetConsoleCtrlHandler(&OnConsoleControl, TRUE);

  // See Run(): routes back before anything else — except in rpc-only, where
  // this process has neither the privilege nor the mandate to give them back,
  // and says so rather than trying and half-failing.
  ReportAndClearPriorState(/*observeOnly=*/rpcOnly);

  // Console mode keeps its real stderr — the operator is watching this terminal
  // and taking it away to write a file nobody is tailing would be a downgrade.
  // But the same failure the service captures (task #39: the process vanishes
  // with no C++ log line, no WER report and no glog FATAL, which is a Go
  // runtime fatal printing to stderr and then exiting) prints HERE and nowhere
  // else, so it is one closed window away from being lost again. Say how to
  // keep it, and say why that costs nothing: this process's own log echo goes
  // to STDOUT, so redirecting STDERR leaves the transcript below intact.
  LogInfo("console: a Go runtime fatal (panic/concurrent map write) prints to "
          "THIS TERMINAL's stderr and to no log file — it never reaches glog or "
          "the line above. To keep it: `urnetworkd console 2> go-crash.log`. "
          "That costs you nothing here, because everything you are reading is "
          "echoed on stdout. Under the SCM the service captures it to a file "
          "automatically ({}).",
          (LogDir(/*isService=*/true) / L"go-crash.log").string());

  SdkInit(/*isService=*/true, kServiceMemoryLimit);
  UnblindErrorMode("after SdkInit (console)");
  StartHeartbeat(LogDir(/*isService=*/true) / L"heartbeat.txt",
                 &FlushSdkLogsTick);

  ControlServer server;
  g_server = &server;
  // Before Start(), so the clamp is in force before the pipe can accept a
  // single request. The stop point goes in the same window and in this order:
  // the clamp is the stronger guarantee and must never be sequenced behind a
  // debug flag.
  if (rpcOnly) server.ClampToRpcOnly();
  if (stopAfterStep) server.SetStopAfterStep(stopAfterStep);
  if (!server.Start()) {
    LogError("console: control server failed to start");
    StopHeartbeat();  // see the same call in Run()
    return 1;
  }
  LogInfo("console: running on {} (mode={}{}); press Ctrl+C to stop",
          Narrow(ids::kControlPipeName), rpcOnly ? "rpc-only" : "tunnel",
          stopAfterStep ? std::format(", stop-after={}", stopAfterStep)
                        : std::string());

  ::WaitForSingleObject(g_stopEvent, INFINITE);

  LogInfo("console: stopping");
  const auto stopStart = std::chrono::steady_clock::now();
  server.Stop();  // reverts routes/dns/firewall FIRST, then releases the sdk
  g_server = nullptr;
  StopHeartbeat();  // see the note on the same call in Run()
  const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - stopStart)
                          .count();
  // Before the forced-exit check: the console control handler's escalation path
  // is waiting on this, and letting it observe a drained teardown is what turns
  // a second Ctrl+C into a clean forced stop instead of a kill.
  ::SetEvent(g_consoleDrainedEvent);
  if (TeardownAbandoned()) {
    LogError("console: stopped in {}ms with the SDK TEARDOWN ABANDONED. Your "
             "routes, dns and firewall policy are already back; the sdk objects "
             "are held by a thread that will not return, so this process exits "
             "by TerminateProcess rather than unwinding through it.",
             stopMs);
    NetworkConfig::CrashRevert();  // idempotent; a no-op if the orderly path ran
    ::TerminateProcess(::GetCurrentProcess(), kForcedStopExitCode);
  }
  LogInfo("console: stopped cleanly in {}ms{}", stopMs,
          rpcOnly ? " (this process wrote no network state, so there is nothing "
                    "to restore)"
                  : ", network restored");

  ::CloseHandle(g_stopEvent);
  g_stopEvent = nullptr;
  return 0;
}

int Usage() {
  // NARROW UTF-8 BYTES THROUGH fwrite, NOT std::wprintf — and that is a bug
  // fix, not a style preference.
  //
  // This text begins "urnetworkd — the URnetwork Windows service", and that em
  // dash used to truncate the ENTIRE usage output. std::wprintf converts each
  // wide character to a multibyte sequence using the current C locale, which is
  // "C" here because nothing calls setlocale; U+2014 has no representation in
  // it, so the conversion fails, wprintf returns -1, and everything after the
  // eleventh character is discarded. `urnetworkd help` printed exactly
  // "urnetworkd " and stopped — verified by redirecting it to a file, 243 bytes
  // of which every one was the startup log line. So the "log:" path, and every
  // word describing every verb, had never been readable, and neither was the
  // Usage() shown after an argument error.
  //
  // The file is compiled /utf-8, so the narrow literal below is already UTF-8
  // bytes and fwrite puts them on stdout untouched, with no locale in the path
  // to reject anything. This is exactly how LogWrite emits the same em dashes
  // correctly today (Log.cpp: fwrite of the formatted narrow line), so the two
  // output paths now agree instead of one silently failing.
  const std::string text = std::format(
      "urnetworkd — the URnetwork Windows service\n"
      "\n"
      "  urnetworkd                run under the SCM (what the SCM invokes)\n"
      "  urnetworkd console        run in this console for development\n"
      "  urnetworkd console --rpc-only\n"
      "                            same, but clamped so no start_tunnel from\n"
      "                            any client can create the wintun adapter or\n"
      "                            write a route or dns entry. Brings up the\n"
      "                            DeviceLocal + mTLS rpc listener only, so the\n"
      "                            app has a live DeviceRemote to drive.\n"
      "                            NEEDS NO ELEVATION and carries no traffic.\n"
      "                            The startup sweep is observe-only here: it\n"
      "                            reports leftover state but cleans nothing\n"
      "                            and does not consume the crash marker.\n"
      "                            This is a SAFETY NET, not a switch: the app\n"
      "                            must ask for rpc-only itself (set\n"
      "                            URNETWORK_RPC_ONLY=1), because a client that\n"
      "                            asked for a tunnel is refused rather than\n"
      "                            silently downgraded.\n"
      "  urnetworkd console --stop-after=<N>        (N = 1..8, DEBUG ONLY)\n"
      "                            run the bring-up as far as step N of 8 and\n"
      "                            then STOP, unwinding through the same\n"
      "                            teardown a user disconnect runs. Combinable\n"
      "                            with --rpc-only, in either order; where they\n"
      "                            overlap the rpc-only clamp wins, because it\n"
      "                            is the one that does less. The steps:\n"
      "                              1 wintun adapter (needs elevation)\n"
      "                              2 sdk egress bound to the physical nic\n"
      "                              3 network space   4 DeviceLocal\n"
      "                              5 mTLS rpc listener\n"
      "                              6 ROUTES + DNS + firewall  <- destructive\n"
      "                              7 split tunnel    8 packet pump\n"
      "                            1-5 write nothing to this machine; --rpc-only\n"
      "                            stops before 6 but SKIPS 1, so --stop-after=1\n"
      "                            is the only way to bring the adapter up and\n"
      "                            stop before anything is routed to it. The\n"
      "                            stop point and what is left applied are\n"
      "                            logged at every step. An out-of-range,\n"
      "                            malformed or repeated N is REFUSED, never\n"
      "                            read as 'no stop'.\n"
      "  urnetworkd selftest       run the pure-logic unit tests: the shared\n"
      "                            route/firewall table, the WFP filter-set\n"
      "                            construction for every policy state, and the\n"
      "                            console option parsing above. Opens\n"
      "                            no adapter, writes no route, and never\n"
      "                            contacts the filter engine, so it needs no\n"
      "                            elevation and cannot change the machine.\n"
      "                            It CANNOT prove a filter blocks anything --\n"
      "                            that needs the elevated leak-validation\n"
      "                            gates in p7-gates.ps1.\n"
      "  urnetworkd install        register the service AND START it\n"
      "                            (elevated). Idempotent: if the service is\n"
      "                            already registered it is stopped, re-pointed\n"
      "                            at THIS exe, and started again — run it from\n"
      "                            a new build to make the service run that\n"
      "                            build. No separate `sc start` is needed.\n"
      "                            Exit code 0 means the service is RUNNING.\n"
      "  urnetworkd uninstall      stop and deregister the service (elevated)\n"
      "  urnetworkd revert         take back any leftover tunnel routes/DNS\n"
      "                            without starting anything (elevated).\n"
      "                            Refuses while a urnetworkd is running --\n"
      "                            it cannot tell a live tunnel from a dead\n"
      "                            one, and reverting a live one drops your\n"
      "                            traffic to the clear. --force overrides.\n"
      "\n"
      "log: {}\n"
      "go fatals: {} (written by the SCM path automatically; in console mode\n"
      "           redirect stderr yourself: `urnetworkd console 2> that file`.\n"
      "           An empty file is the healthy state — anything in it is a Go\n"
      "           runtime panic or fatal error, and the run before it died.)\n",
      Narrow(LogFilePath().wstring()),
      Narrow((LogDir(/*isService=*/true) / L"go-crash.log").wstring()));
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Logging first, before anything that can fail: every path below assumes it
  // can report, and a service that dies before opening its log is the failure
  // this project keeps paying for.
  const std::filesystem::path logFile = LogDir(/*isService=*/true) / L"urnetworkd.log";
  const bool logOpen = LogInit(logFile, "urnetworkd");

  // Armed for every entry point, including `install`/`revert`: cheap, and the
  // one path where it matters (a crash with routes installed) can only be
  // reached from the ones that start a tunnel.
  ::SetUnhandledExceptionFilter(&OnUnhandledException);
  std::set_terminate(&OnTerminate);
  // The per-thread half of the same belt. std::set_terminate above covers ONLY
  // this thread on MSVC; every worker thread arms its own handler through
  // ThreadGuard.h, and this is where that handler is told what "give the machine
  // its network back" means. Registered here so it is in force before anything
  // starts a thread.
  SetThreadGuardCrashRevert(&NetworkConfig::CrashRevert);
  // The other hook main() owns, and it is here for the same reason: only this
  // translation unit knows how this process was started, and therefore whether
  // ending it is a recovery (the SCM restarts it) or a disappearance (a console
  // the operator is watching). Installed before any verb runs so no path can
  // reach the state without a way out of it. See RestartServiceProcess.
  SetSelfRestartHandler(&RestartServiceProcess);
  // Before any verb runs, so a death in ANY of them is distinguishable from a
  // clean exit. See OnProcessExit.
  std::atexit(&OnProcessExit);

  // Echo the log to stdout whenever there is a stdout to echo to. `console`,
  // the dev commands and the no-argument fallback all run in a terminal; the
  // SCM starts us with no console, so the service is unaffected. Set before the
  // first log line so the terminal transcript starts at the first line rather
  // than at whatever RunConsole gets to.
  const HANDLE stdOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (stdOut && stdOut != INVALID_HANDLE_VALUE) LogSetConsoleEcho(true);

  std::wstring cmd = argc >= 2 ? argv[1] : L"";
  LogInfo("urnetworkd starting: pid={} cmd=\"{}\" identity={} sdk={} log={}",
          ::GetCurrentProcessId(), Narrow(cmd), DescribeIdentity(),
          urnet::version(), logFile.string());
  if (!logOpen) {
    // The file could not be opened (ACLs, or %ProgramData% missing). Say so on
    // stderr — the log line above went to the debugger only.
    std::fwprintf(stderr, L"urnetworkd: cannot write %s; logging to the "
                          L"debugger only\n",
                  logFile.c_str());
  }

  // The widest possible clear, and it goes here rather than beside the hooks
  // above so the operator SEES it: the console echo is only switched on a few
  // lines up. Today the SDK is a load-time import, so the Go runtime has already
  // blinded WER by the time wmain runs and this is the call that unblinds it for
  // the whole process — including `install`, `revert` and `selftest`. The
  // post-SdkInit calls are the ones that would still work if the DLL ever became
  // delay-loaded.
  UnblindErrorMode("wmain entry");
  // …and immediately: does the channel we just reopened actually PRODUCE
  // anything on this machine? Clearing the bit restores the ability to reach
  // WER; what WER writes is machine configuration this process does not own,
  // and on a box with no LocalDumps key the answer is "an event log entry and
  // no stack". Stated every start so nobody has to infer it from an empty
  // CrashDumps folder after the next death — see CrashDumps.h.
  LogInfo("{}", DescribeCrashDumpChannel(ProbeCrashDumpChannel(L"urnetworkd.exe"),
                                         "urnetworkd.exe"));

  // Before every other verb: it is the only one that is structurally incapable
  // of touching the machine, so it can never be the thing that broke something.
  if (cmd == L"selftest") return RunSelfTest();
  if (cmd == L"install") return InstallService();
  if (cmd == L"uninstall") return UninstallService();
  // --rpc-only and --stop-after are accepted only alongside `console`. Neither
  // is a flag on the SCM path: the installed service exists to run tunnels, and
  // a mode that silently makes it not do so would be indistinguishable from a
  // broken tunnel.
  if (cmd == L"console" || cmd == L"--console") {
    // EVERY extra argument is checked, not just argv[2]: `console --rpc-only
    // --xyz` used to ignore argv[3] silently while `console --xyz` errored, so a
    // typo was swallowed exactly when the flag most needed to be read carefully.
    // ParseConsoleArgs (ConsoleArgs.h) keeps that rule and extends it to
    // --stop-after's VALUE — an out-of-range, malformed or repeated one is
    // rejected here rather than falling back to "no stop", which would turn a
    // typo into a full tunnel bring-up on a machine whose operator asked for a
    // partial one. That refusal is exercised by `urnetworkd selftest`.
    std::vector<std::wstring> options;
    for (int i = 2; i < argc; ++i) options.emplace_back(argv[i]);
    const ConsoleArgs parsed = ParseConsoleArgs(options);
    if (!parsed.ok) {
      std::fwprintf(stderr, L"%s\n", parsed.error.c_str());
      Usage();
      return 2;
    }
    return RunConsole(parsed.rpc_only, parsed.stop_after);
  }
  // `urnetworkd --rpc-only` with no subcommand: the obvious shorthand, and it
  // resolves to the mode that does less, so honouring it is safe. Extra
  // arguments are rejected here too — hardening only the `console` spelling
  // left this entry point silently swallowing a typo.
  if (cmd == L"--rpc-only") {
    if (argc >= 3) {
      std::fwprintf(stderr, L"unknown option: %s\n", argv[2]);
      Usage();
      return 2;
    }
    return RunConsole(true);
  }
  if (cmd == L"revert" || cmd == L"--revert") {
    const bool force = argc >= 3 && (std::wstring(argv[2]) == L"--force" ||
                                     std::wstring(argv[2]) == L"-f");
    return RevertNetwork(force);
  }
  if (cmd == L"help" || cmd == L"--help" || cmd == L"-h" || cmd == L"/?") return Usage();
  if (!cmd.empty()) {
    std::fwprintf(stderr, L"unknown command: %s\n", cmd.c_str());
    Usage();
    return 2;
  }

  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<LPWSTR>(ids::kServiceName), ServiceMain}, {nullptr, nullptr}};
  if (!::StartServiceCtrlDispatcherW(table)) {
    const DWORD err = ::GetLastError();
    if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
      // Launched from a shell rather than by the SCM — the normal developer
      // mistake. Fall through to console mode rather than exiting silently.
      LogInfo("service: not started by the SCM; falling back to console mode "
              "(use `urnetworkd console` to be explicit)");
      return RunConsole(/*rpcOnly=*/false);
    }
    LogError("service: StartServiceCtrlDispatcher failed: {}", err);
    return 1;
  }
  // The SCM path's ONLY orderly ending. Said out loud because its absence is
  // evidence: a service log that stops after "service: scm state -> stopped"
  // without this line and without the ATEXIT line below it did not return from
  // here at all.
  LogInfo("service: StartServiceCtrlDispatcher returned — ServiceMain is done "
          "and wmain is returning 0. The ATEXIT line should follow this one; if "
          "it does not, something took the process between the two.");
  return 0;
}
