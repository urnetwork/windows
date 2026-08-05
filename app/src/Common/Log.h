// Lightweight logging that writes to OutputDebugString, an optional file, and
// (in the service) can be pointed at the Windows event log by the caller.
// Application/SDK logs proper go through the SDK's glog (urnw::setLogDir); this
// is for the native host code around the SDK.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <format>
#include <string_view>

namespace urnw {

enum class LogLevel { Debug, Info, Warn, Error };

// Initialize file logging. Safe to call once at startup; without it, logs still
// go to the debugger via OutputDebugString. Returns false when the file could
// not be opened (a read-only or missing directory) — the caller is expected to
// say so rather than run with logging silently off.
bool LogInit(const std::filesystem::path& logFile, std::string_view tag);

// The file LogInit opened, or empty when logging is debugger-only. Error paths
// name it so the user knows where to look (App/Startup.cpp).
std::filesystem::path LogFilePath();

// Also mirror every line to stdout. `urnetworkd console` turns this on: there
// the operator is watching a terminal, not tailing the log file, and a dev mode
// that prints nothing is indistinguishable from one that hung. Off by default,
// so the SCM service and the tray app are unaffected.
void LogSetConsoleEcho(bool enabled);

void LogWrite(LogLevel level, std::string_view message);

template <class... Args>
void LogInfo(std::format_string<Args...> fmt, Args&&... args) {
  LogWrite(LogLevel::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void LogWarn(std::format_string<Args...> fmt, Args&&... args) {
  LogWrite(LogLevel::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void LogError(std::format_string<Args...> fmt, Args&&... args) {
  LogWrite(LogLevel::Error, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void LogDebug(std::format_string<Args...> fmt, Args&&... args) {
  LogWrite(LogLevel::Debug, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace urnw
