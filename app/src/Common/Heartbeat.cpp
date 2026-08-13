// SPDX-License-Identifier: MPL-2.0
#include "Heartbeat.h"

#include <atomic>
#include <format>
#include <memory>
#include <thread>

// WIN32_LEAN_AND_MEAN is already on this project's command line; redefining it
// here only produces a C4005 (same note as Sdk.cpp).
#include <windows.h>

#include "Log.h"
#include "ThreadGuard.h"

namespace urnw {
namespace {

// Static-init time, which for this process is the loader's DLL_PROCESS_ATTACH
// pass — a few milliseconds before wmain and near enough to "process start" for
// a number whose job is to say how far into a run the death landed.
const std::chrono::steady_clock::time_point g_processStart =
    std::chrono::steady_clock::now();

// The published tunnel state. A bare pointer into the string literals
// ToString(TunnelState) returns, so the read side allocates nothing — see the
// header for why the heartbeat may not simply ask TunnelController.
std::atomic<const char*> g_tunnelState{"stopped"};

std::atomic<void*> g_quitEvent{nullptr};
std::atomic<void*> g_doneEvent{nullptr};
std::atomic<bool> g_started{false};

// How long StopHeartbeat waits for the ticker to acknowledge. The ticker wakes
// on a manual-reset event and writes one 256-byte record, so this is three
// orders of magnitude above any honest exit — it can only expire if the thread
// is already stuck, and in that case waiting longer would be the bug. Bounded
// at all because the alternative is a detached thread still calling std::format
// while the CRT tears down underneath it.
constexpr DWORD kQuiesceBudgetMs = 250;

constexpr unsigned char kBom[] = {0xEF, 0xBB, 0xBF};

}  // namespace

void PublishTunnelState(const char* state) {
  g_tunnelState.store(state ? state : "unknown", std::memory_order_relaxed);
}

const char* PublishedTunnelState() {
  return g_tunnelState.load(std::memory_order_relaxed);
}

std::chrono::seconds ProcessUptime() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - g_processStart);
}

std::string FormatUptime(long long seconds) {
  if (seconds < 0) seconds = 0;
  return std::format("{:02}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60,
                     seconds % 60);
}

std::string FormatHeartbeatRecord(unsigned long pid, long long uptimeSeconds,
                                  const char* tunnelState,
                                  std::string_view stamp) {
  std::string out(reinterpret_cast<const char*>(kBom), sizeof(kBom));
  out += std::format(
      "urnetworkd heartbeat  at={}  pid={}  uptime={}  tunnel={}  — this line "
      "is rewritten in place about once a second. If it is older than the "
      "clock, THIS IS THE LAST SECOND THE PROCESS WAS ALIVE.",
      stamp, pid, FormatUptime(uptimeSeconds),
      tunnelState ? tunnelState : "unknown");

  // The width is the contract (see the header): a shorter record must never
  // leave the tail of a longer one on disk. Truncation stops at a UTF-8
  // boundary so a clipped record is still decodable text rather than a broken
  // sequence PowerShell renders as a replacement character.
  const size_t body = kHeartbeatRecordBytes - 2;  // room for the CRLF
  if (out.size() > body) {
    out.resize(body);
    while (!out.empty() &&
           (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
      out.pop_back();
    }
  }
  out.resize(body, ' ');
  out += "\r\n";
  return out;
}

// --- the file ---------------------------------------------------------------

HeartbeatFile::~HeartbeatFile() { Close(); }

bool HeartbeatFile::Open(const std::filesystem::path& file) {
  Close();
  path_ = file;
  error_.clear();

  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);

  // GENERIC_WRITE and not FILE_APPEND_DATA: this file is rewritten at offset 0,
  // which is the opposite of what the log and the go-crash capture want and is
  // the whole point here. FILE_SHARE_DELETE alongside READ/WRITE so the owner
  // can read or delete a live service's heartbeat without stopping it.
  const HANDLE h = ::CreateFileW(
      file.c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    error_ = std::format("CreateFileW failed: {}", ::GetLastError());
    return false;
  }
  handle_ = h;
  return true;
}

void HeartbeatFile::Beat(long long uptimeSeconds, const char* tunnelState) {
  if (!handle_) return;
  const auto now = std::chrono::system_clock::now();
  const std::string record = FormatHeartbeatRecord(
      ::GetCurrentProcessId(), uptimeSeconds, tunnelState,
      std::format("{:%F %T}", std::chrono::floor<std::chrono::seconds>(now)));

  LARGE_INTEGER zero{};
  if (!::SetFilePointerEx(static_cast<HANDLE>(handle_), zero, nullptr,
                          FILE_BEGIN))
    return;
  DWORD written = 0;
  ::WriteFile(static_cast<HANDLE>(handle_), record.data(),
              static_cast<DWORD>(record.size()), &written, nullptr);
}

void HeartbeatFile::Close() {
  if (!handle_) return;
  ::CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
}

// --- the thread -------------------------------------------------------------

void StartHeartbeat(const std::filesystem::path& file, HeartbeatTick tick) {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) return;

  const HANDLE quit = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  const HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!quit || !done) {
    if (quit) ::CloseHandle(quit);
    if (done) ::CloseHandle(done);
    LogError("service: no heartbeat — CreateEvent failed ({}). Refusing to run "
             "the ticker without a way to stop it: a diagnostic thread still "
             "writing while the CRT tears down would be a crash of our own "
             "making.",
             ::GetLastError());
    return;
  }
  g_quitEvent.store(quit, std::memory_order_release);
  g_doneEvent.store(done, std::memory_order_release);

  auto beat = std::make_shared<HeartbeatFile>();
  if (!beat->Open(file)) {
    LogError("service: no heartbeat — could not open {} ({}). The death window "
             "for this run stays as wide as the gap between log lines.",
             file.string(), beat->LastError());
    // Nothing will ever signal this, and StopHeartbeat would otherwise spend
    // its whole budget waiting for a thread that was never started.
    ::SetEvent(done);
    return;
  }
  LogInfo("service: heartbeat armed -> {} (one {}-byte record, rewritten in "
          "place ~1/s, carrying uptime and tunnel state; the sdk's glog is "
          "flushed on the same tick)",
          file.string(), kHeartbeatRecordBytes);

  // The thread OWNS everything it touches — the file by shared_ptr, the event
  // by handle — for the same reason StopBudget's RunBounded worker does: it is
  // never joined, so nothing may be looking at what it holds.
  std::thread([beat, quit, done, tick] {
    // Armed but NOT wrapped in RunGuarded, and the difference is deliberate.
    // The guard's escape path aborts the process, which is right for a data-path
    // thread and exactly wrong for an instrumentation thread: a heartbeat that
    // can kill the service it is watching would be the worst possible trade. So
    // the per-thread terminate handler is armed (a terminate here still logs and
    // reverts) while an ordinary exception only ends the ticker.
    ArmThreadGuard("heartbeat");
    try {
      for (;;) {
        beat->Beat(ProcessUptime().count(), PublishedTunnelState());
        if (tick) tick();
        if (::WaitForSingleObject(quit, 1000) != WAIT_TIMEOUT) break;
      }
      beat->Beat(ProcessUptime().count(), PublishedTunnelState());
    } catch (const std::exception& e) {
      LogError("service: the heartbeat ticker stopped: {}. The service keeps "
               "running; the death window just widens back to the gap between "
               "log lines.",
               e.what());
    } catch (...) {
      LogError("service: the heartbeat ticker stopped on a non-std exception. "
               "The service keeps running.");
    }
    // Published last, and outside the try, so StopHeartbeat's wait ends on
    // every path this thread can take — including the ones that got here by
    // failing.
    ::SetEvent(done);
  }).detach();
}

void StopHeartbeat() {
  const HANDLE quit =
      static_cast<HANDLE>(g_quitEvent.load(std::memory_order_acquire));
  if (!quit) return;
  ::SetEvent(quit);
  // A BOUNDED wait, and it is the only thing in this file that waits at all.
  // The ticker is detached and never joined, so without this the process could
  // return from wmain while the thread was inside std::format with the CRT
  // already unwinding — an exit-time crash of our own making, in the code whose
  // whole job is to explain crashes. Bounded so it can never become the reason
  // a shutdown hangs: past the budget we simply leave, exactly as a
  // TerminateProcess would.
  if (const HANDLE done =
          static_cast<HANDLE>(g_doneEvent.load(std::memory_order_acquire)))
    ::WaitForSingleObject(done, kQuiesceBudgetMs);
}

}  // namespace urnw
