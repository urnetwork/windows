// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "UpdateChecker.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "Config.h"
#include "Log.h"
#include "Paths.h"
#include "Strings.h"
#include "UpdateFormats.h"
#include "Version.h"
#include "VersionGrammar.h"

namespace urnw {
namespace {

namespace fs = std::filesystem;
using std::chrono::steady_clock;

// The launch check waits out the startup rush (SDK init, service reconnect,
// the tray settling) rather than adding an HTTP request to it; the cadence is
// the spec's 6 hours.
constexpr auto kLaunchDelay = std::chrono::seconds(30);
constexpr auto kCheckInterval = std::chrono::hours(6);

// Response caps. The release LIST is JSON that should be tens of KB;
// SHA256SUMS is a handful of lines; the zip is ~100 MB self-contained today.
// A cap is not a guess about the future, it is the refusal to stream an
// unbounded body into a file because a server said so.
constexpr std::uint64_t kMaxJsonBytes = 8ull * 1024 * 1024;
constexpr std::uint64_t kMaxSumsBytes = 1ull * 1024 * 1024;
constexpr std::uint64_t kMaxZipBytes = 1ull * 1024 * 1024 * 1024;

// The arch half of the asset name grammar
// URnetwork-v<version>-windows-<x64|arm64>-portable.zip — decided at compile
// time because a binary can only ever swap itself for its own architecture.
#if defined(_M_ARM64)
constexpr const char kArch[] = "arm64";
#else
constexpr const char kArch[] = "x64";
#endif

// The files a release zip MUST stage before any swap begins. The allowlist
// (UpdateFormats.h) says what MAY move; this says what must exist — a zip
// missing the service exe would otherwise half-update into a broken install
// and look fine until the next service restart.
constexpr const char* kRequiredPayload[] = {
    "URnetwork.exe", "urnetworkd.exe", "URnetworkSdk.dll", "wintun.dll",
    "resources.pri"};

// ---- the app's own preferences ----------------------------------------------
//
// Deliberately the same 20 lines as SdkHost.cpp's file-local pair, not a call
// into them: that unit keeps its helpers private on purpose, and the whole-
// object read-modify-write discipline (never serialize just your own key —
// that deletes everyone else's) is the part that must match, which the
// selftest cannot check but a reviewer can. A third preference site is the
// signal to promote this into Common/Paths.
nlohmann::json LoadAppPrefs() {
  std::ifstream f(AppPrefsFile());
  if (!f) return nlohmann::json::object();
  try {
    nlohmann::json j = nlohmann::json::parse(f);
    if (j.is_object()) return j;
  } catch (...) {
  }
  return nlohmann::json::object();
}

void SaveAppPref(const char* key, const nlohmann::json& value) {
  nlohmann::json j = LoadAppPrefs();
  j[key] = value;
  std::ofstream f(AppPrefsFile(), std::ios::trunc);
  if (f) f << j.dump();
}

constexpr char kAutoCheckPrefKey[] = "check_updates_automatically";

// ---- paths -------------------------------------------------------------------

// %LOCALAPPDATA%\URnetwork\updates (spec §5): a sibling of the app storage
// root rather than a second known-folder lookup, so a worktree's
// URNETWORK_APP_ROOT override isolates update downloads the same way it
// isolates everything else.
fs::path UpdatesDir() { return StorageRoot(/*isService=*/false).parent_path() / L"updates"; }

fs::path OwnExePath() {
  wchar_t buf[MAX_PATH];
  const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return fs::path(std::wstring(buf, n));
}

// The probe the swap gates on: can this user create (and delete) a file in the
// app's own directory? CREATE + DELETE_ON_CLOSE makes the cleanup part of the
// close, so a probe interrupted by anything still leaves nothing behind.
bool DirWritable(fs::path const& dir) {
  const fs::path probe = dir / L"urnetwork-update-probe.tmp";
  HANDLE h = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                           nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  ::CloseHandle(h);
  return true;
}

// ---- WinHTTP -----------------------------------------------------------------

struct HInternet {
  HINTERNET h = nullptr;
  ~HInternet() {
    if (h) ::WinHttpCloseHandle(h);
  }
};

struct UrlParts {
  std::wstring host;
  std::wstring path;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

// https only, always: both the API and the asset hosts are https, and a
// redirect that tried to step down to http is refused by WinHTTP's default
// redirect policy anyway — this just refuses it one step earlier.
std::optional<UrlParts> CrackHttpsUrl(std::wstring const& url) {
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength =
      static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc))
    return std::nullopt;
  if (uc.nScheme != INTERNET_SCHEME_HTTPS) return std::nullopt;
  UrlParts p;
  p.host.assign(uc.lpszHostName, uc.dwHostNameLength);
  p.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
  if (uc.dwExtraInfoLength) p.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
  p.port = uc.nPort;
  return p;
}

// One GET, streamed into `sink` chunk by chunk. GitHub requires a User-Agent
// on every request (a bare WinHTTP GET gets 403), and the asset download is a
// browser_download_url that 302s to a storage host — WinHTTP's default
// redirect policy follows https->https transparently, which is exactly the
// hop those URLs make. `cancelled` is polled between reads so a Stop() during
// a 100 MB download aborts within one chunk instead of finishing it.
bool FetchUrl(std::wstring const& url, const wchar_t* accept,
              std::uint64_t maxBytes,
              std::function<bool(const char*, DWORD)> const& sink,
              std::function<bool()> const& cancelled, std::string& error) {
  const auto parts = CrackHttpsUrl(url);
  if (!parts) {
    error = "not an https url";
    return false;
  }

  const std::wstring userAgent =
      L"URnetwork-Windows/" + Widen(version::kString);
  HInternet session{::WinHttpOpen(userAgent.c_str(),
                                  WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                  0)};
  if (!session.h) {
    error = std::format("WinHttpOpen failed: {}", ::GetLastError());
    return false;
  }
  ::WinHttpSetTimeouts(session.h, 10000, 10000, 30000, 30000);

  HInternet connection{
      ::WinHttpConnect(session.h, parts->host.c_str(), parts->port, 0)};
  if (!connection.h) {
    error = std::format("WinHttpConnect failed: {}", ::GetLastError());
    return false;
  }

  HInternet request{::WinHttpOpenRequest(connection.h, L"GET",
                                         parts->path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)};
  if (!request.h) {
    error = std::format("WinHttpOpenRequest failed: {}", ::GetLastError());
    return false;
  }
  if (accept) {
    const std::wstring header = std::wstring(L"Accept: ") + accept;
    ::WinHttpAddRequestHeaders(request.h, header.c_str(),
                               static_cast<DWORD>(-1),
                               WINHTTP_ADDREQ_FLAG_ADD);
  }

  if (!::WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(request.h, nullptr)) {
    error = std::format("request failed: {}", ::GetLastError());
    return false;
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  ::WinHttpQueryHeaders(request.h,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
  if (status != 200) {
    error = std::format("http status {}", status);
    return false;
  }

  std::uint64_t total = 0;
  std::vector<char> chunk;
  for (;;) {
    if (cancelled && cancelled()) {
      error = "cancelled";
      return false;
    }
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(request.h, &available)) {
      error = std::format("WinHttpQueryDataAvailable failed: {}", ::GetLastError());
      return false;
    }
    if (available == 0) return true;  // the body is complete
    chunk.resize(available);
    DWORD read = 0;
    if (!::WinHttpReadData(request.h, chunk.data(), available, &read)) {
      error = std::format("WinHttpReadData failed: {}", ::GetLastError());
      return false;
    }
    total += read;
    if (total > maxBytes) {
      error = std::format("response larger than the {} byte cap", maxBytes);
      return false;
    }
    if (read && !sink(chunk.data(), read)) {
      error = "write failed";
      return false;
    }
  }
}

// ---- SHA-256 (CNG) -----------------------------------------------------------

// The zip's hash, streamed through BCrypt, as lowercase hex — the same
// canonical form Sha256ForFile returns, so the comparison is bytewise. Empty
// on any failure: an unreadable file must fail verification, not pass it.
std::string Sha256File(fs::path const& file) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  if (::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    return {};
  do {
    if (::BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) break;
    std::ifstream in(file, std::ios::binary);
    if (!in) break;
    std::vector<char> buf(64 * 1024);
    bool failed = false;
    while (in) {
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      const std::streamsize n = in.gcount();
      if (n <= 0) break;
      if (::BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()),
                           static_cast<ULONG>(n), 0) != 0) {
        failed = true;
        break;
      }
    }
    if (failed || in.bad()) break;
    UCHAR digest[32];
    if (::BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0) break;
    hex.reserve(64);
    for (UCHAR b : digest) hex += std::format("{:02x}", b);
  } while (false);
  if (hash) ::BCryptDestroyHash(hash);
  ::BCryptCloseAlgorithmProvider(alg, 0);
  return hex;
}

// ---- tar ---------------------------------------------------------------------

// Extraction is the OS's own tar.exe (bsdtar; Windows 10 1803+, and it reads
// zip), addressed by its System32 path rather than PATH so nothing a user
// installed can interpose. bsdtar refuses absolute and ..-traversal member
// paths by default, but the swap does not lean on that: the allowlist copy
// out of staging is the actual zip-slip defence (UpdateFormats.h).
bool ExtractZip(fs::path const& zip, fs::path const& dest, std::string& error) {
  wchar_t sys[MAX_PATH];
  const UINT n = ::GetSystemDirectoryW(sys, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    error = "GetSystemDirectory failed";
    return false;
  }
  const std::wstring tar = std::wstring(sys, n) + L"\\tar.exe";
  std::wstring cmd = L"\"" + tar + L"\" -xf \"" + zip.wstring() + L"\" -C \"" +
                     dest.wstring() + L"\"";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  // cmd.data(): CreateProcess may write into the command-line buffer, which is
  // why it is a mutable wstring rather than a literal.
  if (!::CreateProcessW(tar.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    error = std::format("CreateProcess(tar) failed: {}", ::GetLastError());
    return false;
  }
  ::CloseHandle(pi.hThread);
  const DWORD wait = ::WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
  DWORD exitCode = 1;
  if (wait == WAIT_OBJECT_0) ::GetExitCodeProcess(pi.hProcess, &exitCode);
  if (wait != WAIT_OBJECT_0) ::TerminateProcess(pi.hProcess, 1);
  ::CloseHandle(pi.hProcess);
  if (wait != WAIT_OBJECT_0) {
    error = "tar did not finish within its budget";
    return false;
  }
  if (exitCode != 0) {
    error = std::format("tar exited {}", exitCode);
    return false;
  }
  return true;
}

}  // namespace

// ---- lifecycle ---------------------------------------------------------------

UpdateChecker::~UpdateChecker() { Stop(); }

void UpdateChecker::Start() {
  // Before the thread exists, so no lock is needed; the worker takes the
  // value from the member under the lock like every later reader.
  autoCheck_ = AutoCheckEnabled();
  if (version::kCode == 0) {
    LogInfo(
        "update: dev build (code 0) — automatic checking disabled; the "
        "developer screen's manual check still runs and reports");
  }
  worker_ = std::thread([this] { WorkerLoop(); });
}

void UpdateChecker::Stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

UpdateChecker::Snapshot UpdateChecker::Current() {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

void UpdateChecker::SetHandler(Handler h) {
  std::lock_guard lock(handlerMutex_);
  handler_ = std::move(h);
}

void UpdateChecker::SetRelaunchHandler(RelaunchHandler h) {
  std::lock_guard lock(handlerMutex_);
  relaunch_ = std::move(h);
}

UpdateChecker::Handler UpdateChecker::HandlerCopy() {
  std::lock_guard lock(handlerMutex_);
  return handler_;
}

UpdateChecker::RelaunchHandler UpdateChecker::RelaunchCopy() {
  std::lock_guard lock(handlerMutex_);
  return relaunch_;
}

void UpdateChecker::CheckNow() {
  {
    std::lock_guard lock(mutex_);
    checkRequested_ = true;
  }
  cv_.notify_all();
}

void UpdateChecker::BeginApply() {
  {
    std::lock_guard lock(mutex_);
    applyRequested_ = true;
  }
  cv_.notify_all();
}

bool UpdateChecker::AutoCheckEnabled() {
  return LoadAppPrefs().value(kAutoCheckPrefKey, true);
}

void UpdateChecker::SetAutoCheckEnabled(bool on) {
  SaveAppPref(kAutoCheckPrefKey, on);
  {
    std::lock_guard lock(mutex_);
    autoCheck_ = on;
    // The user just asked for updates; answer now, not in six hours.
    if (on) nextAuto_ = steady_clock::now();
  }
  cv_.notify_all();
  LogInfo("update: automatic checking {}", on ? "enabled" : "disabled");
}

void UpdateChecker::RevealInExplorer(std::wstring const& file) {
  const std::wstring args = L"/select,\"" + file + L"\"";
  ::ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr,
                  SW_SHOWNORMAL);
}

void UpdateChecker::Mutate(std::function<void(Snapshot&)> const& fn) {
  Snapshot copy;
  {
    std::lock_guard lock(mutex_);
    fn(snapshot_);
    copy = snapshot_;
  }
  if (auto handler = HandlerCopy()) handler(copy);
}

// ---- the worker --------------------------------------------------------------

void UpdateChecker::WorkerLoop() {
  // MTA for this thread: RevealInExplorer's ShellExecuteW wants COM up when it
  // runs from an apply on this thread.
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  CleanupStaleFiles();

  std::unique_lock lock(mutex_);
  nextAuto_ = steady_clock::now() + kLaunchDelay;
  for (;;) {
    if (stop_) break;
    if (applyRequested_) {
      applyRequested_ = false;
      lock.unlock();
      RunApply();
      lock.lock();
      continue;
    }
    // A dev build never schedules its own checks; only CheckNow lands here.
    const bool timed = autoCheck_ && version::kCode != 0;
    if (checkRequested_ || (timed && steady_clock::now() >= nextAuto_)) {
      checkRequested_ = false;
      lock.unlock();
      RunCheck();
      lock.lock();
      // Any completed check — manual or automatic — restarts the cadence; two
      // checks 30 seconds apart cannot say different things.
      nextAuto_ = steady_clock::now() + kCheckInterval;
      continue;
    }
    if (timed)
      cv_.wait_until(lock, nextAuto_);
    else
      cv_.wait(lock);
  }
  lock.unlock();
  winrt::uninit_apartment();
}

void UpdateChecker::CleanupStaleFiles() {
  // The .old files next to the exe are the images the LAST update renamed
  // away. Best-effort on purpose, twice over: the previous app instance can
  // still be releasing URnetwork.exe.old in the first seconds after a
  // relaunch, and urnetworkd.exe.old stays locked until the service-update
  // banner restarts the service onto the new exe. Whatever is still held now
  // is deleted on a later launch instead.
  std::error_code ec;
  const fs::path exe = OwnExePath();
  if (!exe.empty()) {
    int removed = 0;
    for (auto const& entry : fs::directory_iterator(exe.parent_path(), ec)) {
      if (!entry.is_regular_file(ec)) continue;
      const std::wstring name = entry.path().filename().wstring();
      const std::size_t pos = name.rfind(L".old");
      // "<anything>.old" or "<anything>.old-<code>" and nothing else — a
      // release file that merely CONTAINS ".old" must not match.
      if (pos == std::wstring::npos ||
          (pos + 4 != name.size() && name[pos + 4] != L'-')) {
        continue;
      }
      if (::DeleteFileW(entry.path().c_str())) ++removed;
    }
    if (removed) LogInfo("update: removed {} stale .old file(s)", removed);
  }

  // Download dirs whose tag no longer outranks this build are spent — either
  // this very update applied, or a newer one superseded it. A dev build
  // (kCode 0) removes nothing: every tag outranks it by definition.
  for (auto const& entry : fs::directory_iterator(UpdatesDir(), ec)) {
    if (!entry.is_directory(ec)) continue;
    const std::uint64_t code =
        version::ParseReleaseCode(Narrow(entry.path().filename().wstring()));
    if (code != 0 && code <= version::kCode) {
      std::error_code rmec;
      fs::remove_all(entry.path(), rmec);
      if (!rmec)
        LogInfo("update: removed spent download dir {}",
                Narrow(entry.path().filename().wstring()));
    }
  }
}

// ---- the check ---------------------------------------------------------------

void UpdateChecker::RunCheck() {
  Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::InFlight; });

  const std::wstring url = std::wstring(L"https://api.github.com/repos/") +
                           config::kUpdateRepo + L"/releases?per_page=15";
  std::string body;
  std::string error;
  const bool fetched = FetchUrl(
      url, L"application/vnd.github+json", kMaxJsonBytes,
      [&body](const char* data, DWORD n) {
        body.append(data, n);
        return true;
      },
      [this] {
        std::lock_guard lock(mutex_);
        return stop_;
      },
      error);
  if (!fetched) {
    LogWarn("update: release check failed: {}", error);
    Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::Failed; });
    return;
  }

  // parse(…, false): a malformed body comes back as `discarded`, not a throw.
  const nlohmann::json releases = nlohmann::json::parse(body, nullptr, false);
  if (!releases.is_array()) {
    LogWarn("update: release list was not a JSON array");
    Mutate([](Snapshot& s) { s.lastCheck = CheckOutcome::Failed; });
    return;
  }

  // Two maxima, deliberately separate: the newest release that PARSES (the
  // honest answer to "is there something newer") and the newest release this
  // build can actually FETCH (both own-arch zip and SHA256SUMS attached).
  // When they differ, that is a broken release and the log says so.
  std::uint64_t newestCode = 0;
  std::string newestVersion;
  Offer offer;
  for (auto const& rel : releases) {
    if (!rel.is_object()) continue;
    if (rel.value("draft", false)) continue;
    const std::string tag = rel.value("tag_name", "");
    const std::uint64_t code = version::ParseReleaseCode(tag);
    if (code == 0) continue;
    std::string ver = tag;
    if (!ver.empty() && ver.front() == 'v') ver.erase(0, 1);
    if (code > newestCode) {
      newestCode = code;
      newestVersion = ver;
    }
    if (code <= offer.code) continue;

    const std::string zipName =
        "URnetwork-v" + ver + "-windows-" + kArch + "-portable.zip";
    std::string zipUrl;
    std::string sumsUrl;
    if (auto assets = rel.find("assets");
        assets != rel.end() && assets->is_array()) {
      for (auto const& asset : *assets) {
        if (!asset.is_object()) continue;
        const std::string name = asset.value("name", "");
        if (name == zipName)
          zipUrl = asset.value("browser_download_url", "");
        else if (name == "SHA256SUMS")
          sumsUrl = asset.value("browser_download_url", "");
      }
    }
    if (zipUrl.empty() || sumsUrl.empty()) {
      LogWarn("update: release {} lacks {} — skipped", tag,
              zipUrl.empty() ? zipName : "SHA256SUMS");
      continue;
    }
    offer = Offer{Widen(ver), code, Widen(tag), Widen(zipUrl), Widen(sumsUrl),
                  zipName};
  }

  LogInfo("update: check complete — own code {}, newest release {} (code {})",
          static_cast<unsigned long long>(version::kCode),
          newestVersion.empty() ? "none" : newestVersion,
          static_cast<unsigned long long>(newestCode));

  Snapshot copy;
  {
    std::lock_guard lock(mutex_);
    snapshot_.newestCode = newestCode;
    snapshot_.newestVersion = Widen(newestVersion);
    if (version::kCode == 0) {
      // Dev builds never self-update (spec §1) — report, offer nothing.
      snapshot_.lastCheck =
          newestCode ? CheckOutcome::DevBuild : CheckOutcome::NoUpdate;
    } else if (offer.code > version::kCode) {
      offer_ = offer;
      snapshot_.lastCheck = CheckOutcome::UpdateFound;
      // A different (newer) release replaces whatever the banner said about
      // an older one; the SAME release keeps its standing ManualUnzip/Failed
      // state — a periodic check must not wipe the outcome of a click.
      if (snapshot_.phase == Phase::None || snapshot_.code != offer.code) {
        snapshot_.phase = Phase::Available;
        snapshot_.stage = Stage::Idle;
        snapshot_.failure = Failure::None;
        snapshot_.version = offer.version;
        snapshot_.code = offer.code;
        snapshot_.zipPath.clear();
      }
    } else {
      snapshot_.lastCheck = CheckOutcome::NoUpdate;
      // A banner for a release that stopped outranking us (it was deleted, or
      // this build updated by hand) closes; a click outcome for it is moot.
      if (snapshot_.phase != Phase::None) {
        snapshot_ = Snapshot{.lastCheck = CheckOutcome::NoUpdate,
                             .newestVersion = Widen(newestVersion),
                             .newestCode = newestCode};
        offer_ = Offer{};
      }
    }
    copy = snapshot_;
  }
  if (auto handler = HandlerCopy()) handler(copy);
}

// ---- the apply ---------------------------------------------------------------

void UpdateChecker::RunApply() {
  Offer offer;
  {
    std::lock_guard lock(mutex_);
    const bool actionable = snapshot_.phase == Phase::Available ||
                            snapshot_.phase == Phase::Failed ||
                            snapshot_.phase == Phase::ManualUnzip;
    if (!actionable || offer_.code == 0) return;
    offer = offer_;
  }
  Mutate([&offer](Snapshot& s) {
    s.phase = Phase::Applying;
    s.stage = Stage::Downloading;
    s.failure = Failure::None;
    s.version = offer.version;
    s.code = offer.code;
    s.zipPath.clear();
  });
  const auto fail = [this](Failure f) {
    Mutate([f](Snapshot& s) {
      s.phase = Phase::Failed;
      s.stage = Stage::Idle;
      s.failure = f;
    });
  };
  const auto cancelled = [this] {
    std::lock_guard lock(mutex_);
    return stop_;
  };
  LogInfo("update: applying v{} (code {})", Narrow(offer.version),
          static_cast<unsigned long long>(offer.code));

  // ---- (a) download the own-arch zip ----------------------------------------
  // A fresh per-tag directory per attempt: nothing from a previous failed try
  // can leak into this one, and a completed try owns everything it verified.
  std::error_code ec;
  const fs::path dir = UpdatesDir() / offer.tag;
  fs::remove_all(dir, ec);
  ec.clear();
  fs::create_directories(dir, ec);
  if (ec) {
    LogError("update: could not create {}: {}", Narrow(dir.wstring()),
             ec.message());
    fail(Failure::Download);
    return;
  }
  const fs::path zipPath = dir / Widen(offer.zipName);
  {
    std::ofstream out(zipPath, std::ios::binary | std::ios::trunc);
    if (!out) {
      LogError("update: could not open {} for writing", offer.zipName);
      fail(Failure::Download);
      return;
    }
    std::string error;
    const bool ok = FetchUrl(
        offer.zipUrl, nullptr, kMaxZipBytes,
        [&out](const char* data, DWORD n) {
          out.write(data, n);
          return out.good();
        },
        cancelled, error);
    out.close();
    if (!ok || !out.good()) {
      LogWarn("update: download failed: {}", ok ? "file write failed" : error);
      fail(Failure::Download);
      return;
    }
  }

  // ---- (b) verify against the release's SHA256SUMS --------------------------
  Mutate([](Snapshot& s) { s.stage = Stage::Verifying; });
  std::string sums;
  {
    std::string error;
    const bool ok = FetchUrl(
        offer.sumsUrl, nullptr, kMaxSumsBytes,
        [&sums](const char* data, DWORD n) {
          sums.append(data, n);
          return true;
        },
        cancelled, error);
    if (!ok) {
      LogWarn("update: SHA256SUMS download failed: {}", error);
      fail(Failure::Download);
      return;
    }
  }
  const std::string expected = update::Sha256ForFile(sums, offer.zipName);
  const std::string actual = Sha256File(zipPath);
  if (expected.empty() || actual.empty() || expected != actual) {
    // The unverifiable download does not stay on disk: a later "just unzip
    // it yourself" must never be able to reach for a zip that failed its
    // check. (Same-origin SHA256SUMS protects download integrity, not
    // against repo compromise — the README says so too; real signing is the
    // MSI milestone's.)
    LogError("update: checksum mismatch for {} — expected '{}', got '{}'",
             offer.zipName, expected, actual);
    fs::remove(zipPath, ec);
    fail(Failure::Checksum);
    return;
  }
  LogInfo("update: verified {} ({})", offer.zipName, actual);

  // ---- (c) extract into fresh staging ---------------------------------------
  Mutate([](Snapshot& s) { s.stage = Stage::Extracting; });
  const fs::path staging = dir / L"staged";
  fs::remove_all(staging, ec);
  ec.clear();
  fs::create_directories(staging, ec);
  std::string tarError;
  if (ec || !ExtractZip(zipPath, staging, tarError)) {
    LogError("update: extraction failed: {}", ec ? ec.message() : tarError);
    fail(Failure::Extract);
    return;
  }

  // ---- (d) the allowlist: archive paths are never trusted -------------------
  // Only TOP-LEVEL files whose bare names pass the payload allowlist leave
  // staging. Everything else — subdirectories, a hostile member name, the
  // README — is ignored. Cost, stated: files under Assets\ are not swapped by
  // auto-update, so an asset-only change needs a manual unzip; that trade is
  // deliberate until the allowlist grows per-release manifest support.
  std::vector<std::wstring> payload;
  for (auto const& entry : fs::directory_iterator(staging, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::wstring wname = entry.path().filename().wstring();
    if (update::IsAllowedPayloadName(Narrow(wname))) payload.push_back(wname);
  }
  for (const char* required : kRequiredPayload) {
    const bool present =
        std::any_of(payload.begin(), payload.end(), [&](std::wstring const& n) {
          return update::EqualsAsciiCaseless(Narrow(n), required);
        });
    if (!present) {
      LogError("update: extracted zip is missing {} — refusing to swap",
               required);
      fail(Failure::Extract);
      return;
    }
  }

  // ---- (e) rename-swap in the app's own directory ---------------------------
  const fs::path exePath = OwnExePath();
  if (exePath.empty()) {
    LogError("update: could not resolve own module path");
    fail(Failure::Swap);
    return;
  }
  const fs::path appDir = exePath.parent_path();
  if (!DirWritable(appDir)) {
    // Someone unzipped into Program Files. The download is verified and
    // sitting in updates\; hand the finish to the user and SHOW them the
    // file rather than describing where it is.
    LogWarn("update: {} is not writable — downloaded, not applied",
            Narrow(appDir.wstring()));
    fs::remove_all(staging, ec);
    const std::wstring zipW = zipPath.wstring();
    Mutate([&zipW](Snapshot& s) {
      s.phase = Phase::ManualUnzip;
      s.stage = Stage::Idle;
      s.zipPath = zipW;
    });
    RevealInExplorer(zipW);
    return;
  }

  Mutate([](Snapshot& s) { s.stage = Stage::Swapping; });
  // NTFS renames running images fine — including URnetwork.exe under this very
  // process and the service's urnetworkd.exe — because a rename never touches
  // the open file object, only the directory entry. Deletion is what a mapped
  // image refuses, which is why stale .old files get the .old-<code> fallback
  // instead of a delete-or-die.
  struct SwapStep {
    fs::path current, oldPath, staged;
    bool renamedOld = false;
    bool movedNew = false;
  };
  std::vector<SwapStep> steps;
  steps.reserve(payload.size());
  bool swapOk = true;
  for (auto const& name : payload) {
    SwapStep st;
    st.current = appDir / name;
    st.staged = staging / name;
    st.oldPath = appDir / (name + L".old");
    if (fs::exists(st.oldPath, ec) && !::DeleteFileW(st.oldPath.c_str())) {
      // Locked — a previous update's image something still runs. Park this
      // round's rename under a code-suffixed name instead.
      st.oldPath = appDir / (name + L".old-" + std::to_wstring(offer.code));
      if (fs::exists(st.oldPath, ec)) ::DeleteFileW(st.oldPath.c_str());
    }
    if (fs::exists(st.current, ec)) {
      if (!::MoveFileExW(st.current.c_str(), st.oldPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
        LogError("update: rename {} -> .old failed: {}", Narrow(name),
                 ::GetLastError());
        swapOk = false;
        steps.push_back(st);
        break;
      }
      st.renamedOld = true;
    }
    // COPY_ALLOWED: updates\ can live on a different volume than the install.
    if (!::MoveFileExW(st.staged.c_str(), st.current.c_str(),
                       MOVEFILE_COPY_ALLOWED)) {
      LogError("update: move staged {} into place failed: {}", Narrow(name),
               ::GetLastError());
      swapOk = false;
      steps.push_back(st);
      break;
    }
    st.movedNew = true;
    steps.push_back(st);
  }
  if (!swapOk) {
    // Roll the completed renames back, newest first, so the directory ends
    // this attempt as it began it — never half-swapped and silent. A rollback
    // step that itself fails is logged loudly; the .old copy is still on disk
    // either way, so nothing is lost, only misnamed.
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
      if (it->movedNew &&
          !::MoveFileExW(it->current.c_str(), it->staged.c_str(),
                         MOVEFILE_COPY_ALLOWED)) {
        LogError("update: ROLLBACK could not return {} to staging: {}",
                 Narrow(it->current.filename().wstring()), ::GetLastError());
      }
      if (it->renamedOld &&
          !::MoveFileExW(it->oldPath.c_str(), it->current.c_str(), 0)) {
        LogError("update: ROLLBACK could not restore {}: {}",
                 Narrow(it->current.filename().wstring()), ::GetLastError());
      }
    }
    fail(Failure::Swap);
    return;
  }

  // ---- (f) relaunch ---------------------------------------------------------
  // The SERVICE still runs the renamed old exe — by design. Restarting it
  // takes the elevation this process must never hold, so ServiceSetup's
  // VersionMismatch banner (its one elevated `install` re-points binPath at
  // the new sibling exe) finishes the update with the user's second click.
  fs::remove_all(staging, ec);
  LogInfo("update: swapped {} file(s); relaunching onto v{}", payload.size(),
          Narrow(offer.version));
  // The banner closes now rather than lingering through teardown; the new
  // instance starts clean, and if the relaunch spawn fails the swapped files
  // simply take effect on the next manual start.
  Mutate([](Snapshot& s) {
    s = Snapshot{.lastCheck = s.lastCheck,
                 .newestVersion = s.newestVersion,
                 .newestCode = s.newestCode};
  });
  if (auto relaunch = RelaunchCopy()) {
    relaunch(appDir / L"URnetwork.exe");
  } else {
    LogWarn("update: no relaunch handler bound — restart the app to run v{}",
            Narrow(offer.version));
  }
}

}  // namespace urnw
