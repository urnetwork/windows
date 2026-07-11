// SPDX-License-Identifier: MPL-2.0
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include "Strings.h"

namespace urnw {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::string g_tag = "urnet";

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

void LogInit(const std::filesystem::path& logFile, std::string_view tag) {
  std::scoped_lock lock(g_mutex);
  g_tag = std::string(tag);
  if (g_file) {
    std::fclose(g_file);
    g_file = nullptr;
  }
  // append; the SDK rotates its own glog files, this native log is small
  _wfopen_s(&g_file, logFile.c_str(), L"a, ccs=UTF-8");
}

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
}

}  // namespace urnw
