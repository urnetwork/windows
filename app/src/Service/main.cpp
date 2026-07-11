// urnetworkd — the URnetwork Windows service (LocalSystem). Hosts the
// DeviceLocal + wintun tunnel and serves the control pipe. Runs under the SCM in
// production; supports console/install/uninstall for development.
//
// SPDX-License-Identifier: MPL-2.0
#include <atomic>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "ControlServer.h"
#include "Ids.h"
#include "Log.h"
#include "Paths.h"
#include "Sdk.h"

using namespace urnw;

namespace {

constexpr int64_t kServiceMemoryLimit = 64ll * 1024 * 1024;

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;
ControlServer* g_server = nullptr;

void SetState(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = exitCode;
  g_status.dwWaitHint = waitHint;
  g_status.dwControlsAccepted =
      (state == SERVICE_START_PENDING) ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
  if (g_statusHandle) ::SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI HandlerEx(DWORD control, DWORD, LPVOID, LPVOID) {
  switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
      SetState(SERVICE_STOP_PENDING, NO_ERROR, 5000);
      if (g_stopEvent) ::SetEvent(g_stopEvent);
      return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
      return NO_ERROR;
    default:
      return ERROR_CALL_NOT_IMPLEMENTED;
  }
}

void Run() {
  LogInit(LogDir(/*isService=*/true) / L"urnetworkd.log", "urnetworkd");
  SdkInit(/*isService=*/true, kServiceMemoryLimit);

  ControlServer server;
  g_server = &server;
  if (!server.Start()) {
    LogError("service: control server failed to start");
    SetState(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
    return;
  }
  SetState(SERVICE_RUNNING);
  LogInfo("service: running");

  ::WaitForSingleObject(g_stopEvent, INFINITE);

  LogInfo("service: stopping");
  server.Stop();
  g_server = nullptr;
  SetState(SERVICE_STOPPED);
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
  g_statusHandle = ::RegisterServiceCtrlHandlerExW(ids::kServiceName, HandlerEx, nullptr);
  if (!g_statusHandle) return;
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  SetState(SERVICE_START_PENDING, NO_ERROR, 5000);
  g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Run();
  if (g_stopEvent) ::CloseHandle(g_stopEvent);
}

// --- dev helpers -----------------------------------------------------------

int InstallService() {
  wchar_t path[MAX_PATH];
  ::GetModuleFileNameW(nullptr, path, MAX_PATH);
  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (!scm) return 1;
  SC_HANDLE svc = ::CreateServiceW(
      scm, ids::kServiceName, ids::kServiceDisplayName, SERVICE_ALL_ACCESS,
      SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, path,
      nullptr, nullptr, nullptr, nullptr, nullptr);
  int rc = svc ? 0 : 1;
  // configure crash recovery: restart the service on failure (plan M4)
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
  }
  ::CloseServiceHandle(scm);
  return rc;
}

int UninstallService() {
  SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!scm) return 1;
  SC_HANDLE svc = ::OpenServiceW(scm, ids::kServiceName, DELETE | SERVICE_STOP);
  int rc = 1;
  if (svc) {
    SERVICE_STATUS st{};
    ::ControlService(svc, SERVICE_CONTROL_STOP, &st);
    rc = ::DeleteService(svc) ? 0 : 1;
    ::CloseServiceHandle(svc);
  }
  ::CloseServiceHandle(scm);
  return rc;
}

// Run the service logic in the console for local debugging (not under SCM).
int RunConsole() {
  g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  LogInit(LogDir(true) / L"urnetworkd.log", "urnetworkd-console");
  SdkInit(true, kServiceMemoryLimit);
  ControlServer server;
  if (!server.Start()) return 1;
  LogInfo("console: running; press Ctrl+C to stop");
  ::WaitForSingleObject(g_stopEvent, INFINITE);
  server.Stop();
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc >= 2) {
    std::wstring cmd = argv[1];
    if (cmd == L"install") return InstallService();
    if (cmd == L"uninstall") return UninstallService();
    if (cmd == L"console") return RunConsole();
  }

  SERVICE_TABLE_ENTRYW table[] = {
      {const_cast<LPWSTR>(ids::kServiceName), ServiceMain}, {nullptr, nullptr}};
  if (!::StartServiceCtrlDispatcherW(table)) {
    // launched interactively without an arg — hint the developer
    return RunConsole();
  }
  return 0;
}
