// SPDX-License-Identifier: MPL-2.0
#include "PipeClient.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <stdexcept>

#include "Ids.h"
#include "Log.h"
#include "Protocol.h"

namespace urnw {

PipeClient::~PipeClient() { Close(); }

bool PipeClient::Connect() {
  // The pipe is a message-mode byte stream we frame by newline. Retry briefly
  // if the single instance is momentarily busy between clients.
  for (int attempt = 0; attempt < 20; ++attempt) {
    HANDLE h = ::CreateFileW(ids::kControlPipeName, GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                             nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      pipe_ = h;
      connected_.store(true);
      stopping_.store(false);
      reader_ = std::thread([this] { ReaderLoop(); });
      return true;
    }
    if (::GetLastError() != ERROR_PIPE_BUSY) {
      return false;  // service not running
    }
    ::WaitNamedPipeW(ids::kControlPipeName, 500);
  }
  return false;
}

void PipeClient::Close() {
  stopping_.store(true);
  if (pipe_) {
    ::CancelIoEx(static_cast<HANDLE>(pipe_), nullptr);
    ::CloseHandle(static_cast<HANDLE>(pipe_));
    pipe_ = nullptr;
  }
  if (reader_.joinable()) reader_.join();
  connected_.store(false);
}

bool PipeClient::WriteLine(const std::string& line) {
  // Overlapped write with a completion wait; the control channel messages are
  // tiny (JSON), so a synchronous-style overlapped write is fine.
  OVERLAPPED ov{};
  ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!ov.hEvent) return false;
  bool ok = false;
  DWORD written = 0;
  if (::WriteFile(static_cast<HANDLE>(pipe_), line.data(),
                  static_cast<DWORD>(line.size()), nullptr, &ov) ||
      ::GetLastError() == ERROR_IO_PENDING) {
    if (::GetOverlappedResult(static_cast<HANDLE>(pipe_), &ov, &written, TRUE)) {
      ok = (written == line.size());
    }
  }
  ::CloseHandle(ov.hEvent);
  return ok;
}

void PipeClient::ReaderLoop() {
  std::string buffer;
  char chunk[4096];
  OVERLAPPED ov{};
  ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

  while (!stopping_.load()) {
    ::ResetEvent(ov.hEvent);
    DWORD read = 0;
    BOOL ok = ::ReadFile(static_cast<HANDLE>(pipe_), chunk, sizeof(chunk),
                         nullptr, &ov);
    if (!ok && ::GetLastError() != ERROR_IO_PENDING) break;
    if (!::GetOverlappedResult(static_cast<HANDLE>(pipe_), &ov, &read, TRUE)) break;
    if (read == 0) continue;

    buffer.append(chunk, read);
    size_t nl;
    while ((nl = buffer.find('\n')) != std::string::npos) {
      std::string line = buffer.substr(0, nl);
      buffer.erase(0, nl + 1);
      if (line.empty()) continue;

      nlohmann::json j;
      try {
        j = nlohmann::json::parse(line);
      } catch (const std::exception& e) {
        LogWarn("control: bad json from service: {}", e.what());
        continue;
      }

      const std::string type = proto::TypeOf(j);
      if (type == proto::msg::kEvent) {
        if (onEvent_) onEvent_(j);
      } else {
        // a reply — hand it to the waiting Call
        std::scoped_lock lock(replyMutex_);
        reply_ = std::move(j);
        haveReply_ = true;
        replyCv_.notify_all();
      }
    }
  }

  ::CloseHandle(ov.hEvent);
  connected_.store(false);
}

nlohmann::json PipeClient::Call(const nlohmann::json& request, int timeoutMs) {
  std::scoped_lock callLock(callMutex_);
  if (!connected_.load()) throw std::runtime_error("control channel not connected");

  {
    std::scoped_lock lock(replyMutex_);
    haveReply_ = false;
  }

  const std::string line = request.dump() + "\n";
  if (!WriteLine(line)) throw std::runtime_error("control channel write failed");

  std::unique_lock lock(replyMutex_);
  if (!replyCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                         [this] { return haveReply_; })) {
    throw std::runtime_error("control channel timeout");
  }
  return std::move(reply_);
}

}  // namespace urnw
