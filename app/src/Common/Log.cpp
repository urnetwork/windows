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
FILE* g_file = nullptr;
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
  if (g_file) {
    std::fclose(g_file);
    g_file = nullptr;
  }
  // Append, BINARY, narrow. The lines are already UTF-8 (LogWrite formats
  // std::string), and this file is written with fwrite: a stream opened
  // "ccs=UTF-8" is put in _O_U8TEXT mode, where the CRT treats the bytes handed
  // to fwrite as UTF-16 and re-encodes them — every line would land as mojibake,
  // and an odd byte count would fail the write outright. Binary mode writes the
  // bytes as they are; "\n" without the CRLF translation is what every log
  // reader wants anyway.
  _wfopen_s(&g_file, logFile.c_str(), L"ab");
  return g_file != nullptr;
}

std::filesystem::path LogFilePath() {
  std::scoped_lock lock(g_mutex);
  return g_path;
}

void LogSetConsoleEcho(bool enabled) { g_consoleEcho.store(enabled); }

void LogWrite(LogLevel level, std::string_view message) {
  auto now = std::chrono::system_clock::now();
  std::string line =
      std::format("{:%F %T} [{}] {}: {}\n", now, g_tag, LevelTag(level), message);

  ::OutputDebugStringW(Widen(line).c_str());

  std::scoped_lock lock(g_mutex);
  if (g_file) {
    std::fwrite(line.data(), 1, line.size(), g_file);
    std::fflush(g_file);
  }
  // Under the same lock so file and console lines cannot interleave mid-line.
  if (g_consoleEcho.load()) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
  }
}

}  // namespace urnw
