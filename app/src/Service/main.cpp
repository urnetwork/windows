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
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ControlServer.h"
#include "Ids.h"
#include "Log.h"
#include "NetworkConfig.h"
#include "Paths.h"
#include "Sdk.h"
#include "Strings.h"
#include "TunnelController.h"

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
  if (observeOnly) {
    if (crashed || orphans > 0) {
      LogWarn("service: leftover tunnel state from a previous run (marker={} "
              "orphaned_interfaces={}) — this process is OBSERVE-ONLY "
              "(rpc-only), so nothing was cleaned and the marker was LEFT IN "
              "PLACE for the next real start. If your network is wrong, run "
              "`urnetworkd revert` from an elevated prompt.",
              crashed ? "yes" : "no", orphans);
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
  } else {
    LogInfo("service: no leftover tunnel state from a previous run");
  }
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

BOOL WINAPI OnConsoleControl(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
      LogInfo("console: {} — shutting down",
              type == CTRL_C_EVENT ? "Ctrl+C" : "Ctrl+Break");
      if (g_stopEvent) ::SetEvent(g_stopEvent);
      return TRUE;  // handled; the main thread runs the orderly teardown
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
  server.Stop();  // stops the tunnel, which reverts routes and DNS
  g_server = nullptr;
  SetState(SERVICE_STOPPED);
  LogInfo("service: stopped cleanly");
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

int InstallService() {
  wchar_t path[MAX_PATH];
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (!scm) {
    std::fwprintf(stderr, L"OpenSCManager failed: %lu (run elevated)\n", ::GetLastError());
    return 1;
  }
  SC_HANDLE svc = ::CreateServiceW(
      scm, ids::kServiceName, ids::kServiceDisplayName, SERVICE_ALL_ACCESS,
      SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, path,
      nullptr, nullptr, nullptr, nullptr, nullptr);
  int rc = svc ? 0 : 1;
  if (!svc) std::fwprintf(stderr, L"CreateService failed: %lu\n", ::GetLastError());
  // configure crash recovery: restart the service on failure (plan M4). The
  // restarted instance runs the startup sweep, so a crash that DID leave an
  // adapter behind gets cleaned within the restart delay.
  if (svc) {
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000},
                            {SC_ACTION_RESTART, 5000},
                            {SC_ACTION_NONE, 0}};
    SERVICE_FAILURE_ACTIONS fa{};
    fa.dwResetPeriod = 86400;
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ::ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    ::CloseServiceHandle(svc);
    std::wprintf(L"installed %s\n", ids::kServiceName);
  }
  ::CloseServiceHandle(scm);
  return rc;
}

int UninstallService() {
  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) {
    std::fwprintf(stderr, L"OpenSCManager failed: %lu (run elevated)\n", ::GetLastError());
    return 1;
  }
  SC_HANDLE svc = ::OpenServiceW(scm, ids::kServiceName, DELETE | SERVICE_STOP);
  int rc = 1;
  if (svc) {
    SERVICE_STATUS st{};
    ::ControlService(svc, SERVICE_CONTROL_STOP, &st);
    rc = ::DeleteService(svc) ? 0 : 1;
    ::CloseServiceHandle(svc);
  }
  ::CloseServiceHandle(scm);
  std::wprintf(L"%s\n", rc == 0 ? L"uninstalled" : L"uninstall failed");
  return rc;
}

// Is a urnetworkd already serving the control pipe? Two instances cannot both
// serve it (the pipe is single-instance), and the loser's accept loop dies
// quietly — so a console run refuses up front instead.
bool ControlPipeInUse() {
  if (::WaitNamedPipeW(ids::kControlPipeName, 1)) return true;
  return ::GetLastError() != ERROR_FILE_NOT_FOUND;
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
  LogInfo("revert: marker={} orphaned_interfaces={}", marker ? "yes" : "no",
          orphans);
  if (orphans == 0)
    LogInfo("revert: no URnetwork tun interface present; nothing of ours is in "
            "the route table");
  return 0;
}

// Run the service logic in the console for local debugging (not under SCM).
//
// rpcOnly clamps the process so that no start_tunnel from any client can reach
// the destructive half of TunnelController::StartLocked. That is what makes it
// safe — and useful — to run unelevated: the app gets a live DeviceRemote and
// the machine's routes and DNS are never written.
int RunConsole(bool rpcOnly) {
  LogSetConsoleEcho(true);
  LogInfo("console: starting as {}{}", DescribeIdentity(),
          rpcOnly ? " in RPC-ONLY mode" : "");
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
             "`urnetworkd install` + `sc start urnetworkd`), or run "
             "`urnetworkd console --rpc-only`, which needs no elevation.");
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
  // single request.
  if (rpcOnly) server.ClampToRpcOnly();
  if (!server.Start()) {
    LogError("console: control server failed to start");
    return 1;
  }
  LogInfo("console: running on {} (mode={}); press Ctrl+C to stop",
          Narrow(ids::kControlPipeName), rpcOnly ? "rpc-only" : "tunnel");

  ::WaitForSingleObject(g_stopEvent, INFINITE);

  LogInfo("console: stopping");
  server.Stop();  // stops the tunnel, which reverts routes and DNS
  g_server = nullptr;
  ::SetEvent(g_consoleDrainedEvent);
  LogInfo("console: stopped cleanly{}",
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
      L"  urnetworkd install        register the service (elevated)\n"
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

  // --rpc-only is accepted only alongside `console`. It is deliberately NOT a
  // flag on the SCM path: the installed service exists to run tunnels, and a
  // mode that silently makes it not do so would be indistinguishable from a
  // broken tunnel.
  const bool rpcOnlyFlag = argc >= 3 && std::wstring(argv[2]) == L"--rpc-only";
  if (cmd == L"install") return InstallService();
  if (cmd == L"uninstall") return UninstallService();
  if (cmd == L"console" || cmd == L"--console") {
    // Check EVERY extra argument, not just argv[2]: `console --rpc-only --xyz`
    // used to ignore argv[3] silently while `console --xyz` errored, so a typo
    // was swallowed exactly when the flag most needed to be read carefully.
    for (int i = 2; i < argc; ++i) {
      if (std::wstring(argv[i]) == L"--rpc-only") continue;
      std::fwprintf(stderr, L"unknown option for console: %s\n", argv[i]);
      Usage();
      return 2;
    }
    return RunConsole(rpcOnlyFlag);
  }
  // `urnetworkd --rpc-only` with no subcommand: the obvious shorthand, and it
  // resolves to the mode that does less, so honouring it is safe.
  if (cmd == L"--rpc-only") return RunConsole(true);
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
