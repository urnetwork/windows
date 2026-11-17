// The app's own log files in "send feedback with logs". The app and the service
// are separate processes, each with its own glog directory (Paths.h: the app's
// under %LOCALAPPDATA%, the service's under %ProgramData%), and the server
// keeps one zip per feedback, which the service sends (LogUpload.h). So the
// app's files ride in the service's zip, under app/ (the sdk's
// DeviceLocal.UploadLogsWithFiles).
//
// The service runs as LocalSystem and answers every signed-in user on the
// control pipe (PipeServer.cpp kPipeSddl), so it never reads a path a client
// names with its own rights. The app names its log directory (upload_logs
// app_log_dir), and the service lists and opens the glog files there while it
// acts as the app's pipe client (PipeServer::RunAsClient), so every open is
// checked against the app's own rights: the service reads nothing the app
// could not. It takes only regular files under glog names, the newest
// kMaxAppLogFiles, never through a link, and the sdk reads them only through
// duplicates of the handles, before its call returns, after which the service
// closes them. Nothing goes anywhere the upload did not go before: the same
// zip, to the same POST, within the same cap.
//
// Pure, so it is unit-tested on any host (app/tools/log-upload-tests.cpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::applogs {

// The most of the app's files one upload takes. A glog directory holds a
// handful (retention keeps 4 at a start, and a long run adds one each time a
// 16 MiB file fills); the sdk's cap decides which of them fit.
inline constexpr size_t kMaxAppLogFiles = 16;

// The app's folder in the upload's zip.
inline constexpr const char* kAppLogFilesSource = "app";

// A name glog writes (<program>.<host>.<user>.log.<SEVERITY>.<time>.<pid>) that
// can name one zip entry: at most 255 bytes, no leading dot, no path separator,
// no colon and no control character. The sdk applies the same test
// (isUploadLogsFileName), so the service opens nothing the upload would leave
// out.
inline bool LooksLikeGlogFileName(std::string_view name) {
  if (name.empty() || name.size() > 255 || name.front() == '.') return false;
  for (const char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f || c == '/' || c == '\\' || c == ':') return false;
  }
  constexpr std::array<std::string_view, 4> kSeverities = {".log.INFO", ".log.WARNING",
                                                           ".log.ERROR", ".log.FATAL"};
  return std::any_of(kSeverities.begin(), kSeverities.end(), [name](std::string_view severity) {
    return name.find(severity) != std::string_view::npos;
  });
}

// The app's files the service takes besides its glog files, as the sdk's
// upload takes them (sdk log_special_files.go): the sdk's Go crash file, which
// only a Go fatal of the app writes, and the app's own log (Log.cpp). Neither
// rotates under a glog name, so without these a crash or a host-side failure
// of the app never reached support. The sdk masks both, and carries the log's
// newest lines.
inline constexpr const char* kAppGoCrashFileName = "go-crash.txt";
inline constexpr const char* kAppLogFileName = "urnetwork-app.log";

// A name the service takes from the app's directory: a glog name, or one of
// the app's files above.
inline bool LooksLikeAppLogFileName(std::string_view name) {
  return LooksLikeGlogFileName(name) || name == kAppGoCrashFileName || name == kAppLogFileName;
}

// A directory the service may act as the app in: a drive-absolute path
// ("C:\..."), never a share or a device path (\\server\share, \\?\, \\.\),
// with no "." or ".." segment, no colon past the drive's (a stream) and no
// control character. Anything else is refused before the service acts as the
// app at all: the app only names its own directory under %LOCALAPPDATA%, and a
// share would have the service connect out as the user.
inline bool LooksLikeLocalDirectory(std::string_view path) {
  if (path.size() < 3 || path.size() > 1024) return false;
  const char drive = path[0];
  if (!((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z'))) return false;
  if (path[1] != ':' || (path[2] != '\\' && path[2] != '/')) return false;
  size_t segmentStart = 3;
  for (size_t i = 3; i <= path.size(); ++i) {
    if (i < path.size()) {
      const unsigned char u = static_cast<unsigned char>(path[i]);
      if (u < 0x20 || u == 0x7f || path[i] == ':') return false;
      if (path[i] != '\\' && path[i] != '/') continue;
    }
    const std::string_view segment = path.substr(segmentStart, i - segmentStart);
    if (segment == "." || segment == "..") return false;
    segmentStart = i + 1;
  }
  return true;
}

// One file the service found in the app's directory.
struct AppLogEntry {
  std::string name;
  // when it was last written, in any unit that orders (the FILETIME's ticks)
  uint64_t lastWriteTime = 0;
};

// The entries the service opens: those under the names it takes
// (LooksLikeAppLogFileName), the Go crash file first, since it is old and
// rarely written and is the one that says why the app died, then newest first,
// files written at the same time in name order, at most kMaxAppLogFiles. The
// sdk's cap then decides which of these fit beside the service's own.
inline std::vector<std::string> PickAppLogFiles(std::vector<AppLogEntry> entries) {
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [](const AppLogEntry& entry) {
                                 return !LooksLikeAppLogFileName(entry.name);
                               }),
                entries.end());
  std::sort(entries.begin(), entries.end(), [](const AppLogEntry& a, const AppLogEntry& b) {
    const bool aCrash = a.name == kAppGoCrashFileName;
    const bool bCrash = b.name == kAppGoCrashFileName;
    if (aCrash != bCrash) return aCrash;
    if (a.lastWriteTime != b.lastWriteTime) return a.lastWriteTime > b.lastWriteTime;
    return a.name < b.name;
  });
  std::vector<std::string> names;
  for (const AppLogEntry& entry : entries) {
    if (names.size() >= kMaxAppLogFiles) break;
    names.push_back(entry.name);
  }
  return names;
}

}  // namespace urnw::applogs
