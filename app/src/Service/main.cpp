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
void ReportAndClearPriorState() {
  const bool crashed = TunnelController::TakeActiveMarker();
  const int orphans = NetworkConfig::SweepOrphanedTunnel(ids::kTunAdapterGuid,
                                                         ids::kTunAdapterName);
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
      // Windows kills the process when this handler returns (roughly 5s of
      // grace), so the teardown has to finish here rather than on the main
      // thread. Wait for it, then sweep whatever it did not get to.
      LogWarn("console: window closed / session ending — reverting");
      if (g_stopEvent) ::SetEvent(g_stopEvent);
      if (g_consoleDrainedEvent)
        ::WaitForSingleObject(g_consoleDrainedEvent, 4000);
      NetworkConfig::CrashRevert();
      return TRUE;
    default:
      return FALSE;
  }
}

void Run() {
  SdkInit(/*isService=*/true, kServiceMemoryLimit);
  ReportAndClearPriorState();

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
int RevertNetwork() {
  LogSetConsoleEcho(true);
  std::wprintf(L"urnetworkd: reverting any leftover tunnel network state\n");
  if (!TokenHas(WinLocalSystemSid) && !TokenHas(WinBuiltinAdministratorsSid)) {
    std::fwprintf(stderr, L"must run elevated to change routes\n");
    return 1;
  }
  const bool marker = TunnelController::TakeActiveMarker();
  const int orphans = NetworkConfig::SweepOrphanedTunnel(ids::kTunAdapterGuid,
                                                         ids::kTunAdapterName);
  std::wprintf(L"marker=%s orphaned_interfaces=%d\n", marker ? L"yes" : L"no",
               orphans);
  if (orphans == 0)
    std::wprintf(L"no URnetwork tun interface present; nothing of ours is in "
                 L"the route table\n");
  return 0;
}

// Run the service logic in the console for local debugging (not under SCM).
int RunConsole() {
  LogSetConsoleEcho(true);
  LogInfo("console: starting as {}", DescribeIdentity());
  if (!TokenHas(WinLocalSystemSid) && !TokenHas(WinBuiltinAdministratorsSid)) {
    LogError("console: NOT elevated — creating the wintun adapter and writing "
             "routes both require LocalSystem/administrator, so a start_tunnel "
             "will fail at step 1. Re-run from an elevated prompt (or use "
             "`urnetworkd install` + `sc start urnetworkd`).");
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

  SdkInit(/*isService=*/true, kServiceMemoryLimit);
  ReportAndClearPriorState();

  ControlServer server;
  g_server = &server;
  if (!server.Start()) {
    LogError("console: control server failed to start");
    return 1;
  }
  LogInfo("console: running on {}; press Ctrl+C to stop",
          Narrow(ids::kControlPipeName));

  ::WaitForSingleObject(g_stopEvent, INFINITE);

  LogInfo("console: stopping");
  server.Stop();  // stops the tunnel, which reverts routes and DNS
  g_server = nullptr;
  ::SetEvent(g_consoleDrainedEvent);
  LogInfo("console: stopped cleanly, network restored");

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
      L"  urnetworkd install        register the service (elevated)\n"
      L"  urnetworkd uninstall      stop and deregister the service (elevated)\n"
      L"  urnetworkd revert         take back any leftover tunnel routes/DNS\n"
      L"                            without starting anything (elevated)\n"
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

  if (cmd == L"install") return InstallService();
  if (cmd == L"uninstall") return UninstallService();
  if (cmd == L"console" || cmd == L"--console") return RunConsole();
  if (cmd == L"revert" || cmd == L"--revert") return RevertNetwork();
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
      return RunConsole();
    }
    LogError("service: StartServiceCtrlDispatcher failed: {}", err);
    return 1;
  }
  return 0;
}
