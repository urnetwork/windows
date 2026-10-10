// SPDX-License-Identifier: MPL-2.0
#include "ApplyUpdate.h"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <msi.h>
#include <msiquery.h>
#include <sddl.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "HelperLog.h"
#include "Http.h"
#include "InstallLocationWin32.h"
#include "ReleaseJson.h"
#include "ReleaseSelection.h"
#include "ShellStart.h"
#include "UpdateApply.h"
#include "UpdateFormats.h"
#include "UpdateResult.h"
#include "UpdateResultJson.h"
#include "Version.h"

namespace urnw::updater {
namespace {

namespace fs = std::filesystem;
using update::Refusal;

// The release list is a few hundred KB; the package about 100 MB.
constexpr std::uint64_t kMaxListBytes = 8 * 1024 * 1024;
constexpr std::uint64_t kMaxPackageBytes = 1024 * 1024 * 1024;

// A binary only ever updates itself to its own architecture.
#if defined(_M_ARM64)
constexpr char kArch[] = "arm64";
#else
constexpr char kArch[] = "x64";
#endif

#if defined(URN_UPDATE_RUNNER_TEST_REPO_ID)
#define URN_UPDATER_STRINGIZE_(x) #x
#define URN_UPDATER_STRINGIZE(x) URN_UPDATER_STRINGIZE_(x)
// A runner test's feed: its repository, a tag prefix its throwaway release
// carries, and prereleases. Compiled in only when a build passes all four
// /p:UrnUpdateRunnerTest* properties (Updater.vcxproj, which refuses them from
// the environment); no file in this repository that a build reads sets them,
// and update_apply_wiring_test.go fails if one ever does. Such a helper says
// so in its FileDescription, and app\build.ps1 refuses to package it.
constexpr update::Feed kRunnerTestFeed{
    .id = "runner-test",
    .numericRepoId = URN_UPDATE_RUNNER_TEST_REPO_ID,
    .owner = URN_UPDATER_STRINGIZE(URN_UPDATE_RUNNER_TEST_OWNER),
    .repo = URN_UPDATER_STRINGIZE(URN_UPDATE_RUNNER_TEST_REPO),
    .acceptBetaPrereleases = false,
    .requireImmutable = false,
    .soakSeconds = 0,
    .tagPrefix = URN_UPDATER_STRINGIZE(URN_UPDATE_RUNNER_TEST_TAG_PREFIX),
    .acceptAnyPrerelease = true,
};
#endif

// The feed of the update channel, which the arguments never name.
//
// TODO(opt-in channel): read the machine-wide channel here. It decides which
// repository's releases this process installs with administrator rights, so
// the invariant holds only while all of these do (update_apply_wiring_test.go
// checks the first two today):
//   - the channel is only a feed id, resolved to one of the feeds compiled in
//     (ReleaseSelection.h kFeeds, FeedById); never a repository id, owner,
//     name or URL read at run time;
//   - it is read only here, and never from the arguments, the environment, the
//     user's registry hive or anything under the user's profile;
//   - it is stored where only an administrator can write: a value under an
//     HKLM key whose ACL grants no one else write, or a file in the admin-only
//     install folder, checked the way InstallLocationWin32.h AdminOnlyPath
//     checks the package, before it is read;
//   - only this elevated helper writes it.
// Until then the channel is official.
const update::Feed& ChannelFeed() {
#if defined(URN_UPDATE_RUNNER_TEST_REPO_ID)
  return kRunnerTestFeed;
#else
  return update::kOfficialFeed;
#endif
}

// What every folder and file the helper creates under updates\ carries: owned
// by Administrators, full control for SYSTEM and Administrators, read and
// execute for Users, and no ACE inherited from the folder above. Inheritance
// alone is not enough: Program Files grants CREATOR OWNER full control, and
// under the policy "System objects: Default owner for objects created by
// members of the Administrators group" set to "Object creator", what this
// process creates would be owned by the user's own account, which a process
// of that user without admin rights could then rewrite. Among what it holds
// is the package Windows Installer keeps as the product's repair source.
constexpr wchar_t kAdminOnlyFolderSddl[] = L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)";
constexpr wchar_t kAdminOnlyFileSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)";

// What the helper's lock file carries: owned by Administrators, a protected
// DACL, and full control for SYSTEM and Administrators and nothing for anyone
// else. Tighter than the folders and files above, which let Users read: a
// process without administrator rights can neither open the lock for its data,
// to hold it, nor create it first, because updates\ grants it no right to add
// a file.
constexpr wchar_t kLockFileSddl[] = L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";

class Security {
 public:
  explicit Security(const wchar_t* sddl) {
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor_,
                                                               nullptr)) {
      attributes_.nLength = sizeof(attributes_);
      attributes_.lpSecurityDescriptor = descriptor_;
      attributes_.bInheritHandle = FALSE;
    }
  }
  ~Security() {
    if (descriptor_) ::LocalFree(descriptor_);
  }
  Security(const Security&) = delete;
  Security& operator=(const Security&) = delete;
  bool valid() const { return descriptor_ != nullptr; }
  SECURITY_ATTRIBUTES* attributes() { return &attributes_; }
  PSECURITY_DESCRIPTOR descriptor() const { return descriptor_; }

 private:
  PSECURITY_DESCRIPTOR descriptor_ = nullptr;
  SECURITY_ATTRIBUTES attributes_{};
};

class Handle {
 public:
  explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
  ~Handle() { Close(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : handle_(other.handle_) { other.handle_ = INVALID_HANDLE_VALUE; }
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      Close();
      handle_ = other.handle_;
      other.handle_ = INVALID_HANDLE_VALUE;
    }
    return *this;
  }
  HANDLE get() const { return handle_; }
  bool valid() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
  void Close() {
    if (valid()) ::CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_;
};

class MsiHandle {
 public:
  MsiHandle() = default;
  ~MsiHandle() {
    if (handle_) ::MsiCloseHandle(handle_);
  }
  MsiHandle(const MsiHandle&) = delete;
  MsiHandle& operator=(const MsiHandle&) = delete;
  MSIHANDLE* put() { return &handle_; }
  MSIHANDLE get() const { return handle_; }

 private:
  MSIHANDLE handle_ = 0;
};

std::string Utf8(std::wstring_view text) {
  if (text.empty()) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size : 0, '\0');
  if (size > 0) {
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                          nullptr, nullptr);
  }
  return out;
}

std::wstring WidenAscii(std::string_view text) { return std::wstring(text.begin(), text.end()); }

// The host of a redirect IsAllowedAssetRedirect accepted, for the log: the
// rest of that URL is a signed, short-lived address.
std::string RedirectHost(std::string_view url) {
  constexpr std::string_view kScheme = "https://";
  if (url.substr(0, kScheme.size()) != kScheme) return {};
  url.remove_prefix(kScheme.size());
  return std::string(url.substr(0, url.find_first_of("/:")));
}

// The tag as ASCII, or empty when it holds anything else.
std::string AsciiTag(std::wstring_view tag) {
  std::string ascii;
  for (const wchar_t c : tag) {
    if (c < 0x21 || c > 0x7e) return {};
    ascii.push_back(static_cast<char>(c));
  }
  return ascii;
}

bool IsElevated() {
  HANDLE raw = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
  Handle token(raw);
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  return ::GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) &&
         elevation.TokenIsElevated != 0;
}

std::int64_t NowUnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Creates `folder` with the admin-only security `security` describes, or,
// when it exists, gives it that security again: the owner, and the DACL with
// nothing inherited. Refuses one that is not a plain directory: a reparse
// point would carry what is written below it somewhere else.
bool PrepareFolder(const fs::path& folder, Security& security, std::string& error) {
  if (::CreateDirectoryW(folder.c_str(), security.attributes())) return true;
  if (::GetLastError() != ERROR_ALREADY_EXISTS) {
    error = std::format("{} could not be created: {}", Utf8(folder.native()), ::GetLastError());
    return false;
  }
  Handle existing(::CreateFileW(folder.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr));
  BY_HANDLE_FILE_INFORMATION info{};
  if (!existing.valid() || !::GetFileInformationByHandle(existing.get(), &info)) {
    error = std::format("{} could not be opened: {}", Utf8(folder.native()), ::GetLastError());
    return false;
  }
  if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    error = std::format("{} is not a plain folder (attributes 0x{:x})", Utf8(folder.native()),
                        info.dwFileAttributes);
    return false;
  }
  PSID owner = nullptr;
  BOOL ownerDefaulted = FALSE;
  BOOL present = FALSE;
  PACL dacl = nullptr;
  BOOL daclDefaulted = FALSE;
  if (!::GetSecurityDescriptorOwner(security.descriptor(), &owner, &ownerDefaulted) ||
      !::GetSecurityDescriptorDacl(security.descriptor(), &present, &dacl, &daclDefaulted) ||
      !owner || !present || !dacl) {
    error = "the admin-only security descriptor has no owner or DACL";
    return false;
  }
  const DWORD set = ::SetSecurityInfo(
      existing.get(), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      owner, nullptr, dacl, nullptr);
  if (set != ERROR_SUCCESS) {
    error = std::format("{}'s security could not be set: {}", Utf8(folder.native()), set);
    return false;
  }
  return true;
}

// Takes the machine-wide lock that lets one helper run at a time:
// updates\helper.lock, opened for its data and shared with nobody, and kept
// open (through `lock`) for the life of the process. A process without
// administrator rights cannot open that file for its data, because the DACL
// grants no one but SYSTEM and Administrators anything, and cannot create it
// first, because updates\ grants it no right to add a file; so it can neither
// hold the lock nor hold the name to block it.
//
// #7 took a named mutex here, Global\URnetworkUpdateHelper, whose comment
// assumed that only an administrator can name an object in the global
// namespace. That is not so of a mutex: any process can create that name first
// and hold it, and every update on the machine then ends in Busy for as long
// as that process runs.
//
// updates\ is prepared first (admin-only, a plain directory), so the lock file
// is somewhere only an administrator can write, and so a run refused past here
// leaves an admin-only updates\ behind. A sharing violation is Busy; anything
// else, or a thing there that is not a plain file, is Staging. Returns an empty
// optional once the lock is held.
std::optional<Refusal> TakeHelperLock(const fs::path& updates, Security& folderSecurity,
                                      Handle& lock, HelperLog& log) {
  std::string error;
  if (!PrepareFolder(updates, folderSecurity, error)) {
    log.Line("refused: {}", error);
    return Refusal::Staging;
  }
  Security lockSecurity(kLockFileSddl);
  if (!lockSecurity.valid()) {
    log.Line("refused: the lock's security descriptor could not be built: {}", ::GetLastError());
    return Refusal::Staging;
  }
  const fs::path lockPath = updates / L"helper.lock";
  // The extra rights beyond read data are for giving a lock file that was
  // already there its security again below; the share mode is still nothing,
  // so the lock is exclusive all the same.
  const HANDLE raw = ::CreateFileW(
      lockPath.c_str(),
      FILE_READ_DATA | READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES, 0,
      lockSecurity.attributes(), OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  const DWORD opened = ::GetLastError();
  lock = Handle(raw);
  if (!lock.valid()) {
    if (opened == ERROR_SHARING_VIOLATION) {
      log.Line("refused: another update helper is running");
      return Refusal::Busy;
    }
    log.Line("refused: {} could not be opened: {}", Utf8(lockPath.native()), opened);
    return Refusal::Staging;
  }
  BY_HANDLE_FILE_INFORMATION info{};
  if (!::GetFileInformationByHandle(lock.get(), &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    log.Line("refused: {} is not a plain file (attributes 0x{:x})", Utf8(lockPath.native()),
             info.dwFileAttributes);
    lock.Close();
    return Refusal::Staging;
  }
  // A lock file that was already there keeps whatever security it had, so give
  // it the admin-only one again, as PrepareFolder does for a folder.
  if (opened == ERROR_ALREADY_EXISTS) {
    PSID owner = nullptr;
    BOOL ownerDefaulted = FALSE;
    BOOL present = FALSE;
    PACL dacl = nullptr;
    BOOL daclDefaulted = FALSE;
    if (!::GetSecurityDescriptorOwner(lockSecurity.descriptor(), &owner, &ownerDefaulted) ||
        !::GetSecurityDescriptorDacl(lockSecurity.descriptor(), &present, &dacl, &daclDefaulted) ||
        !owner || !present || !dacl) {
      log.Line("refused: the lock's security descriptor has no owner or DACL");
      lock.Close();
      return Refusal::Staging;
    }
    const DWORD reapplied = ::SetSecurityInfo(
        lock.get(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        owner, nullptr, dacl, nullptr);
    if (reapplied != ERROR_SUCCESS) {
      log.Line("refused: {}'s security could not be set: {}", Utf8(lockPath.native()), reapplied);
      lock.Close();
      return Refusal::Staging;
    }
  }
  return std::nullopt;
}

// Whether the folders, the package and the log msiexec uses are as only an
// administrator can change them, judged the way the install folder is
// (InstallLocationWin32.h AdminOnlyPath), once they are all in place and just
// before msiexec is started.
bool CheckAdminOnly(const std::vector<fs::path>& paths, std::string& error) {
  for (const fs::path& path : paths) {
    std::string why;
    if (!install::AdminOnlyPath(path, why)) {
      error = std::format("{} is not admin-only: {}", Utf8(path.native()), why);
      return false;
    }
  }
  return true;
}

// Removes the images earlier helpers moved into a download folder while their
// msiexec ran (see ApplyUpdate), now that they have ended.
void RemoveStaleImages(const fs::path& updates, HelperLog& log) {
  std::error_code ec;
  for (const auto& folder : fs::directory_iterator(updates, ec)) {
    if (!folder.is_directory(ec) || folder.is_symlink(ec)) continue;
    for (const auto& file : fs::directory_iterator(folder.path(), ec)) {
      if (file.path().extension() != L".running") continue;
      if (::DeleteFileW(file.path().c_str())) {
        log.Line("removed an earlier helper's image {}", Utf8(file.path().native()));
      }
    }
  }
}

bool FileIdOf(HANDLE file, FILE_ID_INFO& id) {
  return ::GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) != 0;
}

bool SameFile(const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
  return a.VolumeSerialNumber == b.VolumeSerialNumber &&
         std::memcmp(&a.FileId, &b.FileId, sizeof(a.FileId)) == 0;
}

// SHA-256 of everything `file` holds, read from its start through the handle,
// as lowercase hex; empty on any failure.
std::string Sha256(HANDLE file) {
  LARGE_INTEGER zero{};
  if (!::SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return {};
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  do {
    if (::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) break;
    std::vector<unsigned char> buffer(1024 * 1024);
    bool failed = false;
    for (;;) {
      DWORD read = 0;
      if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
        failed = true;
        break;
      }
      if (read == 0) break;
      if (::BCryptHashData(hash, buffer.data(), read, 0) != 0) {
        failed = true;
        break;
      }
    }
    if (failed) break;
    unsigned char digest[32];
    if (::BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) break;
    for (const unsigned char b : digest) hex += std::format("{:02x}", b);
  } while (false);
  if (hash) ::BCryptDestroyHash(hash);
  ::BCryptCloseAlgorithmProvider(algorithm, 0);
  return hex;
}

// One value of the package's Property table, or nullopt unless there is
// exactly one row for it.
std::optional<std::string> PackageProperty(MSIHANDLE database, const wchar_t* name) {
  const std::wstring query =
      std::format(L"SELECT `Value` FROM `Property` WHERE `Property`='{}'", name);
  MsiHandle view;
  if (::MsiDatabaseOpenViewW(database, query.c_str(), view.put()) != ERROR_SUCCESS ||
      ::MsiViewExecute(view.get(), 0) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  MsiHandle record;
  if (::MsiViewFetch(view.get(), record.put()) != ERROR_SUCCESS) return std::nullopt;
  DWORD size = 0;
  wchar_t empty[1] = {};
  if (::MsiRecordGetStringW(record.get(), 1, empty, &size) != ERROR_MORE_DATA &&
      size != 0) {
    return std::nullopt;
  }
  std::wstring value(size + 1, L'\0');
  size = static_cast<DWORD>(value.size());
  if (::MsiRecordGetStringW(record.get(), 1, value.data(), &size) != ERROR_SUCCESS) {
    return std::nullopt;
  }
  value.resize(size);
  MsiHandle extra;
  if (::MsiViewFetch(view.get(), extra.put()) != ERROR_NO_MORE_ITEMS) return std::nullopt;
  return Utf8(value);
}

// Writes `result` as last-result.json in `updates`, whole or not at all: a
// new file beside it, with the admin-only `security`, flushed, then renamed
// over the old one, so it keeps that security.
bool WriteResult(const fs::path& updates, const update::UpdateResult& result, Security& security,
                 std::string& error) {
  const fs::path final = updates / L"last-result.json";
  const fs::path written = updates / L"last-result.json.new";
  ::DeleteFileW(written.c_str());
  Handle file(::CreateFileW(written.c_str(), GENERIC_WRITE, 0, security.attributes(), CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.valid()) {
    error = std::format("last-result.json.new could not be created: {}", ::GetLastError());
    return false;
  }
  const std::string text = update::FormatUpdateResult(result);
  DWORD written_ = 0;
  if (!::WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written_, nullptr) ||
      written_ != text.size() || !::FlushFileBuffers(file.get())) {
    error = std::format("last-result.json.new could not be written: {}", ::GetLastError());
    return false;
  }
  file.Close();
  if (!::MoveFileExW(written.c_str(), final.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error = std::format("last-result.json could not be replaced: {}", ::GetLastError());
    return false;
  }
  return true;
}

}  // namespace

int ApplyUpdate(std::wstring_view tagArgument) {
  HelperLog log;
  log.Line("update helper {} (code {}) for {}", version::kString, version::kCode,
           Utf8(tagArgument));

  // ---- 1. who, where and what ----------------------------------------------
  // Not elevated, nothing below could be written; outside an admin-only
  // install location nothing below could be trusted. Neither writes a result.
  if (!IsElevated()) {
    log.Line("refused: not elevated");
    return static_cast<int>(Refusal::NotElevated);
  }
  const fs::path executable = install::OwnExecutablePath();
  std::string why;
  if (executable.empty() || !install::AdminOnlyLocation(executable, why)) {
    log.Line("refused: {} {}", Utf8(executable.native()), why);
    return static_cast<int>(Refusal::NotInstalled);
  }
  if (version::kCode == 0) {
    log.Line("refused: a dev build (code 0) never updates itself");
    return static_cast<int>(Refusal::DevBuild);
  }
  // One helper at a time, machine-wide, on a lock no process without
  // administrator rights can take or hold (TakeHelperLock). PrepareFolder of
  // updates\ moves up with the lock, before the feed and the tag, so Busy is
  // reported before a bad tag, and a run refused past here leaves an admin-only
  // updates\ with its lock file.
  const fs::path installFolder = executable.parent_path();
  const fs::path updates = installFolder / L"updates";
  Security folderSecurity(kAdminOnlyFolderSddl);
  Security fileSecurity(kAdminOnlyFileSddl);
  if (!folderSecurity.valid() || !fileSecurity.valid()) {
    log.Line("refused: the admin-only security descriptors could not be built: {}", ::GetLastError());
    return static_cast<int>(Refusal::Staging);
  }
  Handle helperLock;
  if (const std::optional<Refusal> refusal = TakeHelperLock(updates, folderSecurity, helperLock, log)) {
    return static_cast<int>(*refusal);
  }

  const update::Feed& feed = ChannelFeed();
  const std::string tag = AsciiTag(tagArgument);
  if (!update::IsTagArgument(feed, tag)) {
    log.Line("refused: {} is not a tag of the {} feed", Utf8(tagArgument), feed.id);
    return static_cast<int>(Refusal::BadArguments);
  }
  const std::uint64_t code = update::ParseFeedTag(feed, tag)->code;

  const fs::path tagFolder = updates / WidenAscii(tag);
  std::string error;
  if (!PrepareFolder(tagFolder, folderSecurity, error)) {
    log.Line("refused: {}", error);
    return static_cast<int>(Refusal::Staging);
  }
  log.Open(tagFolder / L"update-helper.log", fileSecurity.attributes());
  RemoveStaleImages(updates, log);

  // From here every ending writes last-result.json.
  auto finish = [&](std::int64_t exitCode) {
    const update::UpdateResult result{.tag = tag,
                                      .code = code,
                                      .exitCode = exitCode,
                                      .finishedUtc = update::FormatUtcSecond(NowUnixSeconds())};
    std::string writeError;
    if (WriteResult(updates, result, fileSecurity, writeError)) {
      log.Line("result: exit {} written to last-result.json", exitCode);
    } else {
      log.Line("result: exit {}, but {}", exitCode, writeError);
    }
    return static_cast<int>(exitCode);
  };
  auto refuse = [&](Refusal refusal, std::string_view reason) {
    log.Line("refused: {}", reason);
    return finish(static_cast<std::int64_t>(refusal));
  };

  // ---- 2 and 3. the release list, fetched here -------------------------------
  // The same request the tray app's check makes (ReleaseListUrl), so the two
  // judge the same page of releases.
  const std::wstring listUrl = WidenAscii(update::ReleaseListUrl(feed));
  std::string body;
  HttpResponse list;
  if (!HttpGet(
          listUrl, L"application/vnd.github+json", kMaxListBytes,
          [&body](const char* data, unsigned long size) {
            body.append(data, size);
            return true;
          },
          list, error)) {
    return refuse(Refusal::ReleaseList, "the release list: " + error);
  }
  if (list.status == 403 || list.status == 429) {
    // GitHub's limit on anonymous requests, shared by everyone behind this
    // network's address
    return refuse(Refusal::RateLimited,
                  std::format("the release list: GitHub refused it with {}", list.status));
  }
  if (list.status != 200) {
    return refuse(Refusal::ReleaseList, std::format("the release list: http status {}", list.status));
  }
  if (list.serverUnixSeconds == 0) {
    return refuse(Refusal::ReleaseList, "the release list has no Date header");
  }
  const std::optional<std::vector<update::Release>> releases = update::ParseReleaseList(body);
  if (!releases) return refuse(Refusal::ReleaseList, "the release list is not a JSON array");

  // ---- 4. the release it offers ------------------------------------------------
  // Judged against the list's own Date header, the soak included: how long a
  // release has been out, unchanged, is GitHub's clock against the times
  // GitHub gives the release, and nothing this machine or its user can set
  // moves either.
  const update::Selection selection =
      update::SelectRelease(*releases, kArch, feed, list.serverUnixSeconds);
  for (const auto& skip : selection.skipped) log.Line("release {} {}: skipped", skip.tag, skip.reason);
  if (!update::SelectionOffers(selection, tag, version::kCode)) {
    return refuse(Refusal::NotOffered,
                  std::format("the {} feed offers {} (code {}), not {} above this build's {}",
                              feed.id, selection.tag.empty() ? "nothing" : selection.tag,
                              selection.code, tag, version::kCode));
  }
  if (!update::IsFeedAssetUrl(feed, selection.tag, selection.assetName, selection.assetUrl)) {
    return refuse(Refusal::Download, "the release's download URL is not its feed's: " +
                                         selection.assetUrl);
  }

  // ---- 5. the download --------------------------------------------------------
  HttpResponse redirect;
  if (!HttpGet(WidenAscii(selection.assetUrl), nullptr, 0, nullptr, redirect, error)) {
    return refuse(Refusal::Download, "the download: " + error);
  }
  if (redirect.status != 302 || !update::IsAllowedAssetRedirect(redirect.location) ||
      !HttpUrlHostIsOneOf(WidenAscii(redirect.location), update::kAssetRedirectHosts)) {
    return refuse(Refusal::Download,
                  std::format("the download answered {} with Location '{}', not one redirect to "
                              "GitHub's release assets",
                              redirect.status, redirect.location));
  }
  log.Line("release {} is offered; downloading {} from {}", selection.tag, selection.assetName,
           RedirectHost(redirect.location));
  const fs::path package = tagFolder / WidenAscii(selection.assetName);
  ::DeleteFileW(package.c_str());  // an earlier, interrupted attempt's
  FILE_ID_INFO writtenId{};
  {
    Handle file(::CreateFileW(package.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                              fileSecurity.attributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid() || !FileIdOf(file.get(), writtenId)) {
      return refuse(Refusal::Staging,
                    std::format("{} could not be created: {}", selection.assetName, ::GetLastError()));
    }
    HttpResponse download;
    const bool fetched = HttpGet(
        WidenAscii(redirect.location), nullptr, kMaxPackageBytes,
        [&file](const char* data, unsigned long size) {
          DWORD written = 0;
          return ::WriteFile(file.get(), data, size, &written, nullptr) && written == size;
        },
        download, error);
    const bool flushed = fetched && ::FlushFileBuffers(file.get());
    file.Close();
    if (!fetched || download.status != 200 || !flushed) {
      ::DeleteFileW(package.c_str());
      return refuse(Refusal::Download,
                    fetched ? std::format("the download answered {}", download.status)
                            : "the download: " + error);
    }
  }

  // ---- 6. the digest, through the handle that keeps the bytes as they are ------
  // Read only, sharing read only: from here until msiexec has ended, nothing
  // can open the package to write, rename or delete it.
  Handle held(::CreateFileW(package.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  FILE_ID_INFO heldId{};
  BY_HANDLE_FILE_INFORMATION heldInfo{};
  if (!held.valid() || !FileIdOf(held.get(), heldId) || !SameFile(writtenId, heldId) ||
      !::GetFileInformationByHandle(held.get(), &heldInfo) ||
      (heldInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    held.Close();
    ::DeleteFileW(package.c_str());
    return refuse(Refusal::Staging, "the download is not the file this helper wrote");
  }
  const std::string actual = Sha256(held.get());
  if (actual.empty() || !update::EqualsAsciiCaseless(actual, selection.digestHex)) {
    held.Close();
    ::DeleteFileW(package.c_str());
    return refuse(Refusal::Digest, std::format("SHA-256 {} is not GitHub's {}",
                                               actual.empty() ? "unreadable" : actual,
                                               selection.digestHex));
  }
  log.Line("SHA-256 {} matches GitHub's for {}", actual, selection.assetName);

  // ---- 7. the package is this product's, at this release's version --------------
  {
    MsiHandle database;
    std::optional<std::string> upgradeCode;
    std::optional<std::string> productVersion;
    if (::MsiOpenDatabaseW(package.c_str(), MSIDBOPEN_READONLY, database.put()) == ERROR_SUCCESS) {
      upgradeCode = PackageProperty(database.get(), L"UpgradeCode");
      productVersion = PackageProperty(database.get(), L"ProductVersion");
    }
    if (!upgradeCode || !productVersion ||
        !update::PackageMatches(*upgradeCode, *productVersion, selection.code)) {
      held.Close();
      ::DeleteFileW(package.c_str());
      return refuse(Refusal::Package,
                    std::format("the package is UpgradeCode {} ProductVersion {}, not {} {}",
                                upgradeCode.value_or("(unreadable)"),
                                productVersion.value_or("(unreadable)"), update::kUpgradeCode,
                                update::UrMsiVersion(selection.code)));
    }
    log.Line("package {} is this product's ProductVersion {}", selection.assetName, *productVersion);
  }

  // ---- 8. out of the installer's way, then msiexec --------------------------------
  // msiexec's log is created here, new and admin-only: msiexec opens the file
  // it finds there and keeps its security. Then everything msiexec and the
  // repair source will rely on is checked for what a non-admin may do to it.
  const fs::path installLog = tagFolder / L"install.log";
  ::DeleteFileW(installLog.c_str());
  const bool logCreated = Handle(::CreateFileW(installLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                               fileSecurity.attributes(), CREATE_NEW,
                                               FILE_ATTRIBUTE_NORMAL, nullptr))
                              .valid();
  if (!logCreated || !CheckAdminOnly({updates, tagFolder, package, installLog}, error)) {
    held.Close();
    ::DeleteFileW(package.c_str());
    return refuse(Refusal::Staging,
                  logCreated ? error : std::format("install.log could not be created: {}",
                                                   ::GetLastError()));
  }

  // This image is one of the files the package replaces. Moved into the
  // download folder (same volume, so a rename, which a running image allows),
  // it holds nothing the installer needs; the next helper deletes it.
  const fs::path running =
      tagFolder / std::format(L"URnetworkUpdate.{}.running", ::GetCurrentProcessId());
  const bool moved =
      ::MoveFileExW(executable.c_str(), running.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
  if (moved) {
    log.Line("moved this helper's image out of the install folder");
  } else {
    log.Line("this helper's image could not be moved ({}); the installer will replace it at the "
             "next restart",
             ::GetLastError());
  }
  wchar_t system[MAX_PATH];
  const UINT systemLength = ::GetSystemDirectoryW(system, MAX_PATH);
  if (systemLength == 0 || systemLength >= MAX_PATH) {
    if (moved) ::MoveFileExW(running.c_str(), executable.c_str(), MOVEFILE_WRITE_THROUGH);
    held.Close();
    ::DeleteFileW(package.c_str());
    return refuse(Refusal::InstallerNotStarted, "GetSystemDirectory failed");
  }
  const fs::path systemFolder(std::wstring(system, systemLength));
  const fs::path msiexec = systemFolder / L"msiexec.exe";
  std::wstring command = update::MsiexecCommandLine(msiexec.native(), package.native(),
                                                    installLog.native());
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!::CreateProcessW(msiexec.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        systemFolder.c_str(), &startup, &process)) {
    const DWORD startError = ::GetLastError();
    if (moved) ::MoveFileExW(running.c_str(), executable.c_str(), MOVEFILE_WRITE_THROUGH);
    held.Close();
    ::DeleteFileW(package.c_str());
    return refuse(Refusal::InstallerNotStarted, std::format("msiexec did not start: {}", startError));
  }
  Handle installer(process.hProcess);
  ::CloseHandle(process.hThread);
  log.Line("msiexec started (process {}): {}", process.dwProcessId, Utf8(command));
  ::WaitForSingleObject(installer.get(), INFINITE);
  DWORD exitCode = 1603;
  ::GetExitCodeProcess(installer.get(), &exitCode);
  held.Close();
  log.Line("msiexec ended with {}", exitCode);

  // ---- 9. report, and keep only what the outcome needs ---------------------------
  if (update::KeepsPackage(exitCode)) {
    // the package is now the product's repair source; older downloads go
    std::error_code ec;
    std::vector<fs::path> older;
    for (const auto& folder : fs::directory_iterator(updates, ec)) {
      if (folder.is_directory(ec) && folder.path().filename() != tagFolder.filename())
        older.push_back(folder.path());
    }
    for (const fs::path& folder : older) {
      fs::remove_all(folder, ec);
      log.Line("removed the earlier download {}{}", Utf8(folder.native()),
               ec ? " (partly: " + ec.message() + ")" : "");
    }
    return finish(exitCode);
  }
  ::DeleteFileW(package.c_str());
  // the installed product is still the one this image belongs to
  if (moved && ::GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES &&
      ::MoveFileExW(running.c_str(), executable.c_str(), MOVEFILE_WRITE_THROUGH)) {
    log.Line("moved this helper's image back into the install folder");
  }
  const int ended = finish(exitCode);

  // ---- 10. after an install that failed, the relaunch ------------------------------
  // The installer starts the app again (RelaunchAfterUpdate) only after an
  // install that took, and its close (CloseApplication, before
  // InstallValidate) may already have ended the app, with the VPN session
  // stopped. So the relaunch starts here, now that the report is written:
  // this helper's own no-argument mode, through the signed-in user's shell,
  // unelevated, as RelaunchAfterUpdate starts it (main.cpp). It waits for this
  // helper to end and then shows the report; an app the installer had not
  // closed takes it as a launch and opens its window.
  std::string shellError;
  if (::GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
    log.Line("no relaunch: {} is not in the install folder", Utf8(executable.native()));
  } else if (StartThroughShell(executable, shellError)) {
    log.Line("started the relaunch through the user's shell");
  } else {
    log.Line("the relaunch could not be started: {}", shellError);
  }
  return ended;
}

}  // namespace urnw::updater
