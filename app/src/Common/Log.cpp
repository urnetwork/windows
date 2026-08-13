// SPDX-License-Identifier: MPL-2.0
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include "Strings.h"

namespace urnw {
namespace {

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::filesystem::path g_path;
std::string g_tag = "urnet";
std::atomic<bool> g_consoleEcho{false};

const char* LevelTag(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "DBG";
    case LogLevel::Info: return "INF";
    case LogLevel::Warn: return "WRN";
    case LogLevel::Error: return "ERR";
  }
  return "INF";
}

}  // namespace

bool LogInit(const std::filesystem::path& logFile, std::string_view tag) {
  std::scoped_lock lock(g_mutex);
  g_tag = std::string(tag);
  g_path = logFile;
  if (g_file != INVALID_HANDLE_VALUE) {
    ::CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
  }

  // FILE_APPEND_DATA (without FILE_WRITE_DATA), not the CRT's "a": two
  // URnetwork.exe processes share this file — a second launch redirects its
  // activation to the first and both log — and a CRT append is a seek plus a
  // write, which interleaves into corrupted lines across processes. With
  // FILE_APPEND_DATA the filesystem does the seek-and-write atomically, so
  // concurrent writers can only interleave whole lines. FILE_SHARE_READ|WRITE
  // so the owner can tail it while the app runs.
  //
  // The bytes go out exactly as formed: LogWrite builds UTF-8 std::string, and
  // the CRT's "ccs=UTF-8" mode this used to open with would have re-read those
  // bytes as UTF-16 and re-encoded them — every line mojibake, odd-length lines
  // failing outright. There is no text mode here to get that wrong.
  g_file = ::CreateFileW(logFile.c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_file == INVALID_HANDLE_VALUE) return false;

  // A UTF-8 BOM on a brand-new file. Windows PowerShell 5.1 — which is what the
  // owner has — defaults Get-Content to the ANSI code page, so a BOM-less log
  // with any non-ascii in it (paths, the em dashes in these messages) is read
  // as mojibake. Only when the file is empty, so restarts do not sprinkle BOMs
  // through it.
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(g_file, &size) && size.QuadPart == 0) {
    static constexpr unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    ::WriteFile(g_file, kBom, sizeof(kBom), &written, nullptr);
  }
  return true;
}

std::filesystem::path LogFilePath() {
  std::scoped_lock lock(g_mutex);
  return g_path;
}

void LogSetConsoleEcho(bool enabled) { g_consoleEcho.store(enabled); }

void LogWrite(LogLevel level, std::string_view message) {
  const auto now = std::chrono::system_clock::now();

  // The whole call is under the lock: g_tag is read here and rewritten by
  // LogInit, and formatting it outside the lock is a data race on a std::string
  // — a torn read of the tag while another thread reinitializes logging.
  std::scoped_lock lock(g_mutex);

  const std::string line =
      std::format("{:%F %T} [{}] {}: {}\n", now, g_tag, LevelTag(level), message);

  ::OutputDebugStringW(Widen(line).c_str());

  if (g_file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    // One WriteFile per line: with FILE_APPEND_DATA that is the unit other
    // processes can interleave at, so lines stay whole. No flush needed —
    // there is no CRT buffer in front of it.
    ::WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  }
  // Under the same lock so file and console lines cannot interleave mid-line.
  if (g_consoleEcho.load()) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
  }
}

}  // namespace urnw
