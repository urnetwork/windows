// The app's own log files, opened by the service as the app (Common/AppLogFiles.h),
// for the one zip a feedback keeps.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "AppLogFiles.h"
#include "Strings.h"

namespace urnw {

// Handles of the app's log files, each under its glog name, owned: closed when
// this is destroyed or CloseAll runs. Move-only, so a handle has one owner from
// its open to its close.
class AppLogHandles {
 public:
  struct File {
    std::string name;
    HANDLE handle = INVALID_HANDLE_VALUE;
  };

  AppLogHandles() = default;
  ~AppLogHandles() { CloseAll(); }

  AppLogHandles(const AppLogHandles&) = delete;
  AppLogHandles& operator=(const AppLogHandles&) = delete;

  AppLogHandles(AppLogHandles&& other) noexcept : files_(std::exchange(other.files_, {})) {}
  AppLogHandles& operator=(AppLogHandles&& other) noexcept {
    if (this != &other) {
      CloseAll();
      files_ = std::exchange(other.files_, {});
    }
    return *this;
  }

  // Takes `handle`, under `name`.
  void Add(std::string name, HANDLE handle) { files_.push_back(File{std::move(name), handle}); }

  const std::vector<File>& Files() const { return files_; }
  size_t Size() const { return files_.size(); }

  void CloseAll() {
    for (const File& file : files_) {
      if (file.handle != INVALID_HANDLE_VALUE && file.handle != nullptr) ::CloseHandle(file.handle);
    }
    files_.clear();
  }

 private:
  std::vector<File> files_;
};

// Lists the log files in the app's log directory `dir` (its glog files, its Go
// crash file and its own log: applogs::LooksLikeAppLogFileName) and opens the newest
// (applogs::PickAppLogFiles) for synchronous reads. Call it only while acting
// as the app's pipe client (PipeServer::RunAsClient), so that Windows checks
// the listing and every open against the app's rights, never this service's.
// A directory that is not a local one is not listed at all, a link (a reparse
// point) is neither followed nor kept, and only a disk file is kept. Nothing
// here logs: the thread is not the service's own while this runs.
inline AppLogHandles OpenAppLogHandles(const std::string& dir) {
  AppLogHandles files;
  if (!applogs::LooksLikeLocalDirectory(dir)) return files;
  std::wstring wideDir = Widen(dir);
  while (!wideDir.empty() && (wideDir.back() == L'\\' || wideDir.back() == L'/')) {
    wideDir.pop_back();
  }

  // A directory holds a handful of glog files; this only bounds a listing of
  // one that holds far more.
  constexpr size_t kMaxListedEntries = 4096;
  std::vector<applogs::AppLogEntry> entries;
  WIN32_FIND_DATAW data{};
  HANDLE find = ::FindFirstFileExW((wideDir + L"\\*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (find == INVALID_HANDLE_VALUE) return files;
  do {
    if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DEVICE)) != 0) {
      continue;
    }
    std::string name = Narrow(data.cFileName);
    if (!applogs::LooksLikeAppLogFileName(name)) continue;
    const uint64_t lastWriteTime =
        (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        data.ftLastWriteTime.dwLowDateTime;
    entries.push_back(applogs::AppLogEntry{std::move(name), lastWriteTime});
  } while (entries.size() < kMaxListedEntries && ::FindNextFileW(find, &data));
  ::FindClose(find);

  for (const std::string& name : applogs::PickAppLogFiles(std::move(entries))) {
    // Shared for reading, writing and deleting: the app's glog writes its live
    // file all along, and may prune an old one.
    HANDLE handle = ::CreateFileW((wideDir + L"\\" + Widen(name)).c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) continue;
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileType(handle) != FILE_TYPE_DISK || !::GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      ::CloseHandle(handle);
      continue;
    }
    files.Add(name, handle);
  }
  return files;
}

}  // namespace urnw
