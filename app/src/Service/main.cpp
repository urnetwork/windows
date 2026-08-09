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
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ConsoleArgs.h"
#include "ControlServer.h"
#include "Ids.h"
#include "InstallVerb.h"
#include "Log.h"
#include "NetworkConfig.h"
#include "Paths.h"
#include "Sdk.h"
#include "SelfTest.h"
#include "StopBudget.h"
#include "Strings.h"
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
  switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      LogInfo("service: control {} received",
              control == SERVICE_CONTROL_STOP ? "STOP" : "SHUTDOWN");
      SetState(SERVICE_STOP_PENDING, NO_ERROR, 5000);
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

void OnTerminate() {
  NetworkConfig::CrashRevert();
  LogError("service: std::terminate — reverted tunnel routes before dying");
  std::abort();
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

BOOL WINAPI OnConsoleControl(DWORD type) {
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

  ControlServer server;
  g_server = &server;
  if (!server.Start()) {
    LogError("service: control server failed to start (is another urnetworkd "
             "already holding {}?)",
             Narrow(ids::kControlPipeName));
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

void WINAPI ServiceMain(DWORD, LPWSTR*) {
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
  // behind gets cleaned within the restart delay.
  SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000},
                          {SC_ACTION_RESTART, 5000},
                          {SC_ACTION_NONE, 0}};
  SERVICE_FAILURE_ACTIONS fa{};
  fa.dwResetPeriod = 86400;
  fa.cActions = 3;
  fa.lpsaActions = actions;
  ::ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

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
  SdkInit(/*isService=*/true, kServiceMemoryLimit);

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
  std::wprintf(
      L"urnetworkd — the URnetwork Windows service\n"
      L"\n"
      L"  urnetworkd                run under the SCM (what the SCM invokes)\n"
      L"  urnetworkd console        run in this console for development\n"
      L"  urnetworkd console --rpc-only\n"
      L"                            same, but clamped so no start_tunnel from\n"
      L"                            any client can create the wintun adapter or\n"
      L"                            write a route or dns entry. Brings up the\n"
      L"                            DeviceLocal + mTLS rpc listener only, so the\n"
      L"                            app has a live DeviceRemote to drive.\n"
      L"                            NEEDS NO ELEVATION and carries no traffic.\n"
      L"                            The startup sweep is observe-only here: it\n"
      L"                            reports leftover state but cleans nothing\n"
      L"                            and does not consume the crash marker.\n"
      L"                            This is a SAFETY NET, not a switch: the app\n"
      L"                            must ask for rpc-only itself (set\n"
      L"                            URNETWORK_RPC_ONLY=1), because a client that\n"
      L"                            asked for a tunnel is refused rather than\n"
      L"                            silently downgraded.\n"
      L"  urnetworkd console --stop-after=<N>        (N = 1..8, DEBUG ONLY)\n"
      L"                            run the bring-up as far as step N of 8 and\n"
      L"                            then STOP, unwinding through the same\n"
      L"                            teardown a user disconnect runs. Combinable\n"
      L"                            with --rpc-only, in either order; where they\n"
      L"                            overlap the rpc-only clamp wins, because it\n"
      L"                            is the one that does less. The steps:\n"
      L"                              1 wintun adapter (needs elevation)\n"
      L"                              2 sdk egress bound to the physical nic\n"
      L"                              3 network space   4 DeviceLocal\n"
      L"                              5 mTLS rpc listener\n"
      L"                              6 ROUTES + DNS + firewall  <- destructive\n"
      L"                              7 split tunnel    8 packet pump\n"
      L"                            1-5 write nothing to this machine; --rpc-only\n"
      L"                            stops before 6 but SKIPS 1, so --stop-after=1\n"
      L"                            is the only way to bring the adapter up and\n"
      L"                            stop before anything is routed to it. The\n"
      L"                            stop point and what is left applied are\n"
      L"                            logged at every step. An out-of-range,\n"
      L"                            malformed or repeated N is REFUSED, never\n"
      L"                            read as 'no stop'.\n"
      L"  urnetworkd selftest       run the pure-logic unit tests: the shared\n"
      L"                            route/firewall table, the WFP filter-set\n"
      L"                            construction for every policy state, and the\n"
      L"                            console option parsing above. Opens\n"
      L"                            no adapter, writes no route, and never\n"
      L"                            contacts the filter engine, so it needs no\n"
      L"                            elevation and cannot change the machine.\n"
      L"                            It CANNOT prove a filter blocks anything --\n"
      L"                            that needs the elevated leak-validation\n"
      L"                            gates in p7-gates.ps1.\n"
      L"  urnetworkd install        register the service AND START it\n"
      L"                            (elevated). Idempotent: if the service is\n"
      L"                            already registered it is stopped, re-pointed\n"
      L"                            at THIS exe, and started again — run it from\n"
      L"                            a new build to make the service run that\n"
      L"                            build. No separate `sc start` is needed.\n"
      L"                            Exit code 0 means the service is RUNNING.\n"
      L"  urnetworkd uninstall      stop and deregister the service (elevated)\n"
      L"  urnetworkd revert         take back any leftover tunnel routes/DNS\n"
      L"                            without starting anything (elevated).\n"
      L"                            Refuses while a urnetworkd is running --\n"
      L"                            it cannot tell a live tunnel from a dead\n"
      L"                            one, and reverting a live one drops your\n"
      L"                            traffic to the clear. --force overrides.\n"
      L"\n"
      L"log: %s\n",
      LogFilePath().c_str());
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
  return 0;
}
