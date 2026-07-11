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
// go to the debugger via OutputDebugString.
void LogInit(const std::filesystem::path& logFile, std::string_view tag);

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
