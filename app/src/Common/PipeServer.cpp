// SPDX-License-Identifier: MPL-2.0
#include "PipeServer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>

#include "Ids.h"
#include "Log.h"
#include "Protocol.h"

#pragma comment(lib, "advapi32.lib")

namespace urnw {
namespace {

// SYSTEM and Administrators: full control. Authenticated users: read/write, so
// the interactive tray app can connect. The device RPC's mTLS keys are the real
// authorization for tunnel control (plan R8); this ACL just keeps the pipe off
// anonymous/network principals.
constexpr wchar_t kPipeSddl[] = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)";

constexpr DWORD kBufferSize = 64 * 1024;

}  // namespace

PipeServer::~PipeServer() { Stop(); }

bool PipeServer::Start(RequestHandler handler) {
  handler_ = std::move(handler);
  stopping_.store(false);
  stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) return false;
  acceptThread_ = std::thread([this] { AcceptLoop(); });
  return true;
}

void PipeServer::Stop() {
  stopping_.store(true);
  if (stopEvent_) ::SetEvent(static_cast<HANDLE>(stopEvent_));
  {
    std::scoped_lock lock(writeMutex_);
    if (activePipe_) ::CancelIoEx(static_cast<HANDLE>(activePipe_), nullptr);
  }
  if (acceptThread_.joinable()) acceptThread_.join();
  if (stopEvent_) {
    ::CloseHandle(static_cast<HANDLE>(stopEvent_));
    stopEvent_ = nullptr;
  }
}

void PipeServer::AcceptLoop() {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kPipeSddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) {
    LogError("pipe: failed to build security descriptor: {}", ::GetLastError());
    return;
  }

  while (!stopping_.load()) {
    HANDLE pipe = ::CreateNamedPipeW(
        ids::kControlPipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1 /* single instance: one tray app at a time */, kBufferSize, kBufferSize,
        0, &sa);
    if (pipe == INVALID_HANDLE_VALUE) {
      LogError("pipe: CreateNamedPipe failed: {}", ::GetLastError());
      break;
    }

    // Overlapped connect so Stop() can break the wait via stopEvent_.
    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL connected = ::ConnectNamedPipe(pipe, &ov);
    DWORD err = ::GetLastError();
    if (!connected && err == ERROR_IO_PENDING) {
      HANDLE waits[2] = {ov.hEvent, static_cast<HANDLE>(stopEvent_)};
      DWORD w = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (w != WAIT_OBJECT_0) {
        ::CloseHandle(ov.hEvent);
        ::CloseHandle(pipe);
        break;  // stopping
      }
    } else if (!connected && err != ERROR_PIPE_CONNECTED) {
      ::CloseHandle(ov.hEvent);
      ::CloseHandle(pipe);
      // back off before retrying so a persistent ConnectNamedPipe error can't
      // spin the accept thread at 100% CPU; wake immediately if we're stopping
      ::WaitForSingleObject(static_cast<HANDLE>(stopEvent_), 100);
      continue;
    }
    ::CloseHandle(ov.hEvent);

    {
      std::scoped_lock lock(writeMutex_);
      activePipe_ = pipe;
    }
    ServeConnection(pipe);
    {
      std::scoped_lock lock(writeMutex_);
      activePipe_ = nullptr;
    }
    ::DisconnectNamedPipe(pipe);
    ::CloseHandle(pipe);
  }

  ::LocalFree(sa.lpSecurityDescriptor);
}

void PipeServer::ServeConnection(void* pipeHandle) {
  HANDLE pipe = static_cast<HANDLE>(pipeHandle);
  std::string buffer;
  char chunk[4096];
  OVERLAPPED ov{};
  ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

  while (!stopping_.load()) {
    ::ResetEvent(ov.hEvent);
    DWORD read = 0;
    BOOL ok = ::ReadFile(pipe, chunk, sizeof(chunk), nullptr, &ov);
    if (!ok && ::GetLastError() != ERROR_IO_PENDING) break;
    if (!::GetOverlappedResult(pipe, &ov, &read, TRUE)) break;
    if (read == 0) continue;

    buffer.append(chunk, read);
    size_t nl;
    while ((nl = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, nl);
      buffer.erase(0, nl + 1);
      if (line.empty()) continue;

      nlohmann::json request, reply;
      try {
        request = nlohmann::json::parse(line);
        reply = handler_ ? handler_(request) : nlohmann::json::object();
      } catch (const std::exception& e) {
        proto::Reply r;
        r.ok = false;
        r.error = e.what();
        reply = r;
      }
      reply["type"] = proto::msg::kReply;
      std::string out = reply.dump() + "\n";

      std::scoped_lock lock(writeMutex_);
      OVERLAPPED wov{};
      wov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
      DWORD written = 0;
      if (::WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), nullptr,
                      &wov) ||
          ::GetLastError() == ERROR_IO_PENDING) {
        ::GetOverlappedResult(pipe, &wov, &written, TRUE);
      }
      ::CloseHandle(wov.hEvent);
    }
  }
  ::CloseHandle(ov.hEvent);
}

void PipeServer::PushEvent(const nlohmann::json& event) {
  std::scoped_lock lock(writeMutex_);
  if (!activePipe_) return;
  nlohmann::json e = event;
  e["type"] = proto::msg::kEvent;
  std::string out = e.dump() + "\n";
  OVERLAPPED ov{};
  ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD written = 0;
  if (::WriteFile(static_cast<HANDLE>(activePipe_), out.data(),
                  static_cast<DWORD>(out.size()), nullptr, &ov) ||
      ::GetLastError() == ERROR_IO_PENDING) {
    ::GetOverlappedResult(static_cast<HANDLE>(activePipe_), &ov, &written, TRUE);
  }
  ::CloseHandle(ov.hEvent);
}

}  // namespace urnw
