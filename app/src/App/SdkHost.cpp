// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "SdkHost.h"

#include <urnetwork_sdk.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <limits>
#include <random>
#include <thread>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include "AuthLogoutCause.h"
#include "BalanceGate.h"
#include "Config.h"
#include "BittensorWalletFlow.h"
#include "Ids.h"
#include "Localization.h"
#include "Log.h"
#include "LogUpload.h"
#include "NetworkSpaceStartup.h"
#include "Paths.h"
#include "PeerLocation.h"
#include "RpcSessionBlob.h"
#include "SdkErrorId.h"
#include "Strings.h"
#include "SystemProxy.h"
#include "VlessPresentation.h"
#include "WalletBridgeRoute.h"

namespace urnw {

namespace {
// The string claim `claim` of a JWT's payload, read WITHOUT verifying the token:
// the server verifies the signature; this only checks that the token the bridge
// handed back is the one this attempt asked for (its nonce).
std::optional<std::string> JwtClaimString(const std::string& jwt, const char* claim) {
  const auto first = jwt.find('.');
  if (first == std::string::npos) return std::nullopt;
  const auto second = jwt.find('.', first + 1);
  if (second == std::string::npos) return std::nullopt;
  // base64url -> base64 (RFC 7515: '-' '_' and no padding)
  std::string b64 = jwt.substr(first + 1, second - first - 1);
  for (auto& c : b64) {
    if (c == '-') c = '+';
    else if (c == '_') c = '/';
  }
  while (b64.size() % 4 != 0) b64.push_back('=');
  DWORD n = 0;
  if (!CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64,
                            nullptr, &n, nullptr, nullptr) || n == 0) {
    return std::nullopt;
  }
  std::string payload(n, '\0');
  if (!CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64,
                            reinterpret_cast<BYTE*>(payload.data()), &n, nullptr, nullptr)) {
    return std::nullopt;
  }
  payload.resize(n);
  try {
    const auto j = nlohmann::json::parse(payload);
    if (!j.contains(claim) || !j[claim].is_string()) return std::nullopt;
    return j[claim].get<std::string>();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// What the verify step says about a code the server was asked to send; no
// error (an older server, or a sent code) reads sent.
VerifySendNotice VerifySendNoticeOf(std::optional<urnet::AuthVerifySendError> const& error) {
  if (!error) return VerifySendNoticeFor(false, std::string(), std::string(), 0);
  return VerifySendNoticeFor(false, error->code, error->message,
                             error->retry_after_seconds.value_or(0));
}
}  // namespace
namespace {

// Persisted RPC session, mirroring macOS RpcSessionStore. Lets the app reattach
// its DeviceRemote to a still-running service tunnel.
//
// The struct and both conversions live in Common/RpcSessionBlob.h — pure, so
// the service selftest pins the round trip, the missing-instance_id migration
// and the malformed cases. Only the two file handles are left here.
using RpcSession = rpcsession::Blob;

struct RpcSessionLoad {
  std::optional<RpcSession> record;
  std::string diagnostic = "missing";
  bool legacyPlaintext = false;
};

constexpr char kRpcSessionFileMagic[] = "URNETRPC1\n";
constexpr char kRpcSessionEntropy[] = "URnetwork Windows RPC session v1";
constexpr std::uint64_t kMaxRpcSessionFileBytes = 1024 * 1024;

std::mutex& RpcSessionFileMutex() {
  static std::mutex mutex;
  return mutex;
}

DATA_BLOB RpcSessionEntropy() {
  DATA_BLOB entropy{};
  entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kRpcSessionEntropy));
  entropy.cbData = static_cast<DWORD>(sizeof(kRpcSessionEntropy) - 1);
  return entropy;
}

std::optional<std::string> ProtectRpcSession(std::string_view plaintext) {
  if (plaintext.size() > std::numeric_limits<DWORD>::max()) return std::nullopt;
  DATA_BLOB input{};
  input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
  input.cbData = static_cast<DWORD>(plaintext.size());
  DATA_BLOB output{};
  DATA_BLOB entropy = RpcSessionEntropy();
  if (!::CryptProtectData(&input, L"URnetwork RPC session", &entropy, nullptr,
                          nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return std::nullopt;
  }
  std::string encrypted(reinterpret_cast<const char*>(output.pbData), output.cbData);
  ::LocalFree(output.pbData);
  return encrypted;
}

std::optional<std::string> UnprotectRpcSession(std::string_view encrypted) {
  if (encrypted.size() > std::numeric_limits<DWORD>::max()) return std::nullopt;
  DATA_BLOB input{};
  input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(encrypted.data()));
  input.cbData = static_cast<DWORD>(encrypted.size());
  DATA_BLOB output{};
  DATA_BLOB entropy = RpcSessionEntropy();
  if (!::CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return std::nullopt;
  }
  std::string plaintext(reinterpret_cast<const char*>(output.pbData), output.cbData);
  ::SecureZeroMemory(output.pbData, output.cbData);
  ::LocalFree(output.pbData);
  return plaintext;
}

std::optional<std::string> ReadRpcSessionBytes() {
  const std::filesystem::path path = RpcSessionFile();
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::nullopt;
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > kMaxRpcSessionFileBytes) {
    ::CloseHandle(file);
    return std::nullopt;
  }
  std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const bool ok = ::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                             &read, nullptr) &&
                  read == bytes.size();
  ::CloseHandle(file);
  if (!ok) return std::nullopt;
  return bytes;
}

bool WriteRpcSessionBytes(std::string_view bytes) {
  if (bytes.empty() || bytes.size() > std::numeric_limits<DWORD>::max()) return false;
  const std::filesystem::path path = RpcSessionFile();
  std::filesystem::path temp = path;
  temp += L".tmp." + std::to_wstring(::GetCurrentProcessId());
  HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
                              nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool wrote = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                                 &written, nullptr) &&
                     written == bytes.size();
  const bool flushed = wrote && ::FlushFileBuffers(file);
  ::CloseHandle(file);
  if (!flushed || !::MoveFileExW(temp.c_str(), path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

// URNETWORK_RPC_ONLY: ask the service for a session that stops before it would
// touch the machine's routes or DNS (spec P1).
//
// Parsed as an explicit allow-list of truthy values rather than "anything that
// is not falsy". The earlier version accepted anything unrecognised as ON, so
// `URNETWORK_RPC_ONLY=off`, `=no` and `=0 ` (trailing space) all turned the
// mode ON â€” a stray `setx` giving a client that silently refuses to connect.
// Unrecognised now means OFF *and says so*, because the failure of guessing
// wrong is a developer confused about why nothing connects.
// Reads an environment variable as a trimmed narrow string, empty when unset.
// Kept deliberately dumb: callers decide what an unset or unrecognised value
// means, because those two are not the same thing (see StartModeFromEnvironment,
// where guessing wrong leaves a developer wondering why nothing connects).
std::string EnvVar(const wchar_t* name) {
  constexpr DWORD kMax = 256;
  wchar_t buf[kMax] = {0};
  const DWORD n = ::GetEnvironmentVariableW(name, buf, kMax);
  // n == 0: unset. n >= kMax: longer than anything we accept; treat as unset
  // rather than silently truncating into a host name.
  if (n == 0 || n >= kMax) return {};
  std::wstring v(buf, n);
  const size_t first = v.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return {};
  const size_t last = v.find_last_not_of(L" \t\r\n");
  return urnw::Narrow(v.substr(first, last - first + 1));
}

proto::StartMode StartModeFromEnvironment() {
  constexpr DWORD kMax = 64;
  wchar_t buf[kMax] = {0};
  const DWORD n = ::GetEnvironmentVariableW(L"URNETWORK_RPC_ONLY", buf, kMax);
  // n == 0: unset. n >= kMax: too long to be one of ours; treat as unset rather
  // than truncating into a comparison.
  if (n == 0 || n >= kMax) return proto::StartMode::Tunnel;

  std::wstring v(buf, n);
  const size_t first = v.find_first_not_of(L" \t\r\n");
  const size_t last = v.find_last_not_of(L" \t\r\n");
  v = (first == std::wstring::npos) ? L"" : v.substr(first, last - first + 1);
  std::transform(v.begin(), v.end(), v.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });

  if (v == L"1" || v == L"true" || v == L"yes" || v == L"on")
    return proto::StartMode::RpcOnly;
  if (v.empty() || v == L"0" || v == L"false" || v == L"no" || v == L"off")
    return proto::StartMode::Tunnel;
  LogWarn("sdkhost: URNETWORK_RPC_ONLY is set to an unrecognised value; "
          "ignoring it and using the normal tunnel mode. Use 1/true/yes/on to "
          "enable rpc-only.");
  return proto::StartMode::Tunnel;
}

bool SaveRpcSession(const RpcSession& s) {
  std::scoped_lock lock(RpcSessionFileMutex());
  std::string plaintext = rpcsession::Serialize(s);
  auto encrypted = ProtectRpcSession(plaintext);
  ::SecureZeroMemory(plaintext.data(), plaintext.size());
  if (!encrypted) return false;
  std::string fileBytes(kRpcSessionFileMagic, sizeof(kRpcSessionFileMagic) - 1);
  fileBytes.append(*encrypted);
  const bool saved = WriteRpcSessionBytes(fileBytes);
  ::SecureZeroMemory(encrypted->data(), encrypted->size());
  return saved;
}

RpcSessionLoad LoadRpcSession() {
  std::scoped_lock lock(RpcSessionFileMutex());
  std::error_code existenceError;
  const bool exists = std::filesystem::exists(RpcSessionFile(), existenceError);
  auto bytes = ReadRpcSessionBytes();
  if (!bytes) {
    RpcSessionLoad result;
    if (exists || existenceError) result.diagnostic = "present-but-unreadable";
    return result;
  }

  const std::string_view all(*bytes);
  const std::string_view magic(kRpcSessionFileMagic,
                               sizeof(kRpcSessionFileMagic) - 1);
  std::string plaintext;
  RpcSessionLoad result;
  if (all.starts_with(magic)) {
    auto decrypted = UnprotectRpcSession(all.substr(magic.size()));
    ::SecureZeroMemory(bytes->data(), bytes->size());
    if (!decrypted) {
      result.diagnostic = "encrypted-but-unreadable";
      return result;
    }
    plaintext = std::move(*decrypted);
  } else {
    // Migration only. Old builds left the client private key in plaintext.
    // Parse it so diagnostics can identify it, but its version/session id make
    // it structurally non-adoptable; the caller removes it promptly.
    plaintext = std::move(*bytes);
    result.legacyPlaintext = true;
  }

  result.record = rpcsession::Parse(plaintext);
  if (!result.record) {
    result.diagnostic = "corrupt";
  } else if (result.legacyPlaintext) {
    result.diagnostic = "legacy-plaintext";
  } else if (!rpcsession::IsAdoptable(*result.record)) {
    result.diagnostic = "unsupported-or-incomplete";
  } else {
    result.diagnostic = rpcsession::ToString(result.record->state);
  }
  ::SecureZeroMemory(plaintext.data(), plaintext.size());
  return result;
}

void ClearRpcSession() {
  std::scoped_lock lock(RpcSessionFileMutex());
  std::error_code ec;
  std::filesystem::remove(RpcSessionFile(), ec);
}


// ---- the app's own preferences (D5) ----------------------------------------
//
// See Paths.h AppPrefsFile for why this is not the SDK LocalState. It reads and
// writes the WHOLE object rather than one key, because this file will acquire a
// second preference eventually and a writer that serialises only its own key
// silently deletes every other one. An unreadable or corrupt file is an empty
// object, never a throw: a preference is not worth taking the app down for.

// LoadAppPrefs / SaveAppPref moved to Common/Paths at the third
// preference site, as the note below prescribed.

// A service older than a verb answers it with "unknown request type"
// (ControlServer::Handle) and runs nothing of the kind the verb names.
bool IsUnknownRequestReply(const std::string& error) {
  return error.rfind("unknown request type", 0) == 0;
}

}  // namespace

SdkHost::~SdkHost() {
  // The feedback log request first: its steps take mutex_ and the pipe, and
  // one that runs is waited out; its old path's call, which touches nothing of
  // this host, is left past a short budget (FeedbackLogUpload.h), by the rule
  // the network country's read follows.
  feedbackLogUpload_.reset();
  // The network country's notifications, then its thread, ended above the
  // lock like the loops below. A report it is running is waited out: it pushes
  // over the pipe and must not run against a host being destroyed. A read the
  // WWAN service never answers is not, past a short budget, so that service
  // cannot hold up the exit (NetworkCountryWatch.h); the read uses nothing of
  // this host.
  networkCountryChanges_.reset();
  networkCountryWatch_.reset();
  // BEFORE mutex_, and joined rather than detached: the watchdog takes mutex_
  // (through the session worker it wakes), so stopping it from inside the lock
  // would deadlock, and letting it outlive this object would leave a thread
  // dialling a pipe on behalf of a destroyed host.
  StopServiceWatchdog();
  // The presentation worker takes mutex_ too (D4), so it is stopped under the
  // same rule: outside the lock, joined.
  StopPresentationWorker();
  // Same rule, same reason: the rpc-sync watchdog takes mutex_ to look at the
  // device, so it is stopped and JOINED here, above the lock.
  StopSyncWatchdog();
  // And the provider-only statistics loop, which takes mutex_ to publish.
  StopProviderOnlyStats();
  // Drain the session-request slot. The worker is detached by design (see
  // RequestSession) and a request mid-flight at destruction has always been a
  // shutdown race the process exit wins; but the row-click settle added a
  // worker that deliberately SLEEPS on pendingCv_ for the settle window, and
  // that one can and must be told to get up now — it wakes, finds the slot
  // empty, and exits without ever touching the wider object.
  {
    std::scoped_lock lock(pendingMutex_);
    pending_ = SessionRequest{};
    pendingRequested_ = false;
    pendingCv_.notify_all();
  }
  std::scoped_lock lock(mutex_);
  subs_.clear();
  apiLogoutSub_.reset();
}

std::string SdkHost::RandomLoopbackHostPort() {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<int> dist(12000, 12100);
  return "127.0.0.1:" + std::to_string(dist(gen));
}

std::string SdkHost::DeviceDescription() {
  wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
  if (::GetComputerNameW(name, &n)) return Narrow(std::wstring(name, n));
  return "windows-desktop";
}

std::string SdkHost::DeviceSpec() {
#if defined(_M_ARM64)
  return "windows arm64";
#else
  return "windows amd64";
#endif
}

// The one-time re-keying of the official space from the key earlier builds
// bundled it under (NetworkSpaceStartup.h). Best-effort: the SDK answers false
// for "nothing to move" as well as for a destination it will not overwrite,
// and either way the launch goes on against the space BuildNetworkSpace writes.
static void MigrateLegacyNetworkSpace(const urnet::NetworkSpaceManager& manager,
                                      const netspace::Key& from, const netspace::Key& to) {
  urnet::NetworkSpaceKey fromKey;
  fromKey.host_name = from.hostName;
  fromKey.env_name = from.envName;
  urnet::NetworkSpaceKey toKey;
  toKey.host_name = to.hostName;
  toKey.env_name = to.envName;
  try {
    if (manager.migrateNetworkSpace(fromKey, toKey)) {
      LogInfo("sdkhost: re-keyed the network space '{}/{}' to the operator host "
              "'{}/{}'; its stored credentials and preferences carry over",
              from.hostName, from.envName, to.hostName, to.envName);
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: migrate network space '{}/{}' -> '{}/{}' failed: {}", from.hostName,
            from.envName, to.hostName, to.envName, e.what());
  }
}

urnet::NetworkSpace SdkHost::BuildNetworkSpace() {
  // Matches macOS DeviceManager.initializeNetworkSpace.
  //
  // URNETWORK_NETWORK_HOST points the client at a different backend, so that a
  // throwaway account on a test network can exercise the success paths. Until
  // this existed nothing in the client had ever seen a 200: every screen was
  // verified against layout, empty states and 401s only.
  //
  // Env name follows the same rule the SDK uses: "main" (the default) gives
  // api.<host>, anything else gives <env>-api.<host>.
  //
  // This is the programmatic form of the network selector P5 is building; when
  // that lands, both should end up driving setActiveNetworkSpace rather than
  // each carrying their own idea of how a space is assembled.
  const std::string hostOverride = EnvVar(L"URNETWORK_NETWORK_HOST");
  const netspace::BundledSpace bundled =
      netspace::ResolveBundledSpace(hostOverride, EnvVar(L"URNETWORK_NETWORK_ENV"));
  urnet::NetworkSpaceKey key;
  key.host_name = bundled.key.hostName;
  key.env_name = bundled.key.envName;

  if (!bundled.official) {
    LogWarn("sdkhost: NETWORK OVERRIDE - host={} env={}. "
            "This client is NOT talking to production.",
            bundled.key.hostName, bundled.key.envName);
  }

  // These values replace the stored ones whole, at every launch, so the
  // bundled space's own values go over what the space stores: the extender
  // settings, the private extender, the bootstrap DNS-over-HTTPS servers and
  // the VLESS server the user saved in it are not among them, and a write from
  // nothing dropped them all on every launch. No migration host and no url
  // overrides (NetworkSpaceStartup.h BundledSpaceValuesOver).
  const urnet::NetworkSpaceValues values = netspace::BundledSpaceValuesOver(
      StoredSpaceValuesLocked(key).value_or(urnet::NetworkSpaceValues{}));

  urnet::NetworkSpace space = spaceManager_->updateNetworkSpaceValues(key, values);

  // ---- THE SPACE THE USER LAST CHOSE, NOT THE ONE THIS BUILD DEFAULTS TO ----
  //
  // THE BUG THIS FIXES, in the order it happens:
  //
  //   1. This function used to END at the line above, so every launch derived
  //      api_/asyncLocalState_/localState_ from the COMPILED-IN host
  //      (ids::kNetworkSpaceHostName). The space manager persists an `active`
  //      key — ApplyNetworkServer calls setActiveNetworkSpace, and the on-disk
  //      .network_spaces file records it — and nothing here ever read it.
  //   2. A jwt is stored PER SPACE. On the beta line the user's credentials
  //      live under network_spaces/<their host>/main/.by; the default host's
  //      .by directory is empty.
  //   3. So Initialize() read an empty getByClientJwt(), set loggedIn_ = false,
  //      and TOOK THE LOGGED-OUT BRANCH: the resume bootstrap thread was never
  //      spawned. That is why the failing run logged NEITHER "session
  //      bootstrapped" NOR "session bootstrap failed on resume" — not a hang,
  //      not a lock, the thread never existed. Initialize() returned in 5.6 ms.
  //   4. The user then re-picked their server in the network sheet, which
  //      restored loggedIn_ from the right LocalState — and, before this
  //      change, still created no session (see ApplyNetworkServer). Signed in,
  //      no DeviceRemote, Connect a no-op.
  //
  // URNETWORK_NETWORK_HOST still wins: it is an explicit instruction for THIS
  // process, and honouring a stored preference over it would make the override
  // silently ineffective — the exact failure its own comment above warns about.
  //
  // Everything here is best-effort. A manager with no active space, a handle
  // this build cannot read, an entry for a host that no longer resolves: all of
  // them fall through to the default space rather than take the app down.
  if (hostOverride.empty()) {
    // NetworkSpaceKey's fields are std::optional<std::string> (an unset one is
    // omitted on the wire), so take the value out once for both the comparison
    // and the log rather than formatting an optional.
    const std::string defaultHost = key.host_name.value_or(std::string());
    try {
      urnet::NetworkSpace active = spaceManager_->getActiveNetworkSpace();
      const std::string activeHost = active ? active.getHostName() : std::string();
      if (!activeHost.empty() && activeHost != defaultHost) {
        LogInfo("sdkhost: restoring the network space this client was last "
                "pointed at: '{}' (the build default is '{}'). The stored "
                "credentials live in THAT space.",
                activeHost, defaultHost);
        return active;
      }
    } catch (const std::exception& e) {
      LogWarn("sdkhost: could not read the active network space ({}); using the "
              "build default '{}'", e.what(), defaultHost);
    }
  }
  return space;
}

std::optional<urnet::NetworkSpaceValues> SdkHost::StoredSpaceValuesLocked(
    const urnet::NetworkSpaceKey& key) {
  if (!spaceManager_) return std::nullopt;
  try {
    // A handle of 0 is the sdk's nil: the manager has no space under this key.
    const urnet::NetworkSpace stored = spaceManager_->getNetworkSpace(key);
    if (!stored) return std::nullopt;
    // The space's own json is the only reading of its stored values the C ABI
    // offers (the getters return EFFECTIVE values), as SetNetExtender found.
    return vless::StoredValuesFor<urnet::NetworkSpaceKey, urnet::NetworkSpaceValues>(
        key, nlohmann::json::parse(stored.toJson()));
  } catch (const std::exception& e) {
    LogWarn("sdkhost: read the stored values of network space '{}' failed: {}",
            key.host_name.value_or(std::string()), e.what());
  } catch (...) {
    LogWarn("sdkhost: read the stored values of network space '{}' failed",
            key.host_name.value_or(std::string()));
  }
  return std::nullopt;
}

namespace {
// The device type this app reports (sdk ClientInfo, server
// session/REVOKE-UI-FINAL.md §2).
constexpr const char* kClientDeviceType = "windows";
}  // namespace

void SdkHost::BindApiLocked() {
  // Every request, renewal and connect auth this Api makes carries it; the
  // sdk fills in its own version.
  api_->setClientInfo(urnet::newClientInfo(kClientDeviceType, appVersion_));
  // The sdk drops the account's credential on a confirmed 401 against it, and
  // after a sign-out of this session from the Sessions page, and tells this
  // Api's logout listeners. The app then signs out the way it does when the
  // device's credential is rejected; without this it stayed signed in on a
  // credential the sdk no longer had. A sign-out already done ignores it. The
  // rejection's cause is read here, by the Api's handle, before the hop
  // (AuthLogoutCause.h). On an sdk thread: the handler marshals, and signs
  // out once for this report and the device's of the same rejection
  // (AppController, AuthLogoutNotice.h).
  apiLogoutSub_.reset();
  apiLogoutSub_.emplace(api_->addAuthLogoutListener([this, api = api_->handle()] {
    if (!loggedIn_.load(std::memory_order_acquire) || !onAuthInvalid_) return;
    onAuthInvalid_(authlogout::ApiCause(api));
  }));
}

bool SdkHost::Initialize() {
  std::scoped_lock lock(mutex_);
  // Advanced Mode, BEFORE anything else here. It is a preference on disk, and
  // this function runs on startup — the main window, and therefore any handler
  // that could receive an "advanced mode is on" event, does not exist until the
  // first tray click. Loading it into standing state now is what lets a window
  // built thirty seconds later ask, rather than having to have been listening.
  // See the field comment on advancedMode_ in SdkHost.h.
  advancedMode_.store(LoadAppPrefs().value("advanced_mode", false),
                      std::memory_order_release);
  // A sign-out an earlier run could not deliver, read before any pass: the
  // launch's first pass delivers it before it adopts or starts anything.
  signOut_.Load();
  if (signOut_.Owed()) {
    LogWarn("sdkhost: a sign-out is still owed to the service from an earlier run; "
            "the first pass delivers it before anything starts");
  }
  requestedMode_ = StartModeFromEnvironment();
  if (requestedMode_ == proto::StartMode::RpcOnly) {
    LogWarn("sdkhost: URNETWORK_RPC_ONLY is set â€” asking the service for an "
            "RPC-ONLY session. The DeviceRemote will be live and every screen "
            "driveable, but NO tunnel is created and no traffic is carried; the "
            "connect state will never report 'up'.");
  }
  try {
    // The network country before the space manager: the spaces it builds dial
    // at once, and an extender dial under a name the network refuses holds
    // that extender's address for minutes.
    StartNetworkCountryWatch();
    spaceManager_ =
        urnet::newNetworkSpaceManager(Narrow(SdkStorageDir(false).wstring()));
    // The legacy official space is re-keyed BEFORE the bundled space is
    // written, bound or read (NetworkSpaceStartup.h has the order and the SDK
    // contract behind it); the DeviceRemote is built later, in the session
    // bootstrap, from the space this returns.
    networkSpace_ = netspace::StartBundledSpace(
        [this](const netspace::Key& from, const netspace::Key& to) {
          MigrateLegacyNetworkSpace(*spaceManager_, from, to);
        },
        [this] { return BuildNetworkSpace(); });
    api_ = networkSpace_->getApi();
    BindApiLocked();
    asyncLocalState_ = networkSpace_->getAsyncLocalState();
    localState_ = asyncLocalState_->getLocalState();
    // the SDK's client event queue over this network space: it persists,
    // batches and sends the product events (ClientEvents.h)
    events_ = std::make_unique<ClientEventQueue>(networkSpace_->handle(), appVersion_,
                                                 ClientEventLocale());
    // sign-up network-name availability (bound once; api-scoped)
    networkNameVc_ = urnet::newNetworkNameValidationViewController(*api_);
    networkNameVc_->start();
    SetupWalletCallbacks();

    service_.SetStateHandler([this](const proto::TunnelStatus& st) {
      const proto::TunnelState before = lastServiceState_.load();
      // Remember the two facts only the SERVICE can know, so the statuses this
      // process synthesises (SessionStatus) do not overwrite them with their
      // defaults and render a healthy tunnel as degraded.
      AdoptServiceFacts(st);
      // A reset_extenders the service refused as busy goes again once a status
      // ends the operation that held its lock (Common/ExtenderReset.h). Not
      // from here: this reader thread must stay free to read the answer.
      if (auto due = owedExtenderReset_.TakeDue(st.state)) {
        QueueExtenderResetResend(std::move(*due));
      }
      if (onTunnel_) onTunnel_(st);
      // Terminal activation failure may close the SDK feed before another
      // stats event arrives. Publish the service truth without an SDK getter
      // on the control-pipe reader, where a blocking getter could deadlock RPC.
      if (st.state != proto::TunnelState::Up && onStats_) {
        LiveStats stats;
        stats.rpcOnly = st.mode == proto::StartMode::RpcOnly;
        // With no session the provider-only device is what provides, and this
        // very status says how (adopted just above) — not a default "off".
        if (!HasSession()) FillProviderOnlyStats(stats);
        ClampCaptureStats(stats);
        onStats_(stats);
      }
      // An unexpected drop: the service tore a live session down by itself (the
      // dead-tunnel failsafe), on the edge. The DeviceRemote this side holds now
      // points at a listener that is gone, and with it went the provider. Ask
      // for a session the D8 way — the table drops the stale device, the
      // attach-only bootstrap finds nothing and starts nothing, which keeps the
      // failsafe's "it never reconnects" — and the pass ends with the provider
      // reconcile: providing resumes without a tunnel unless the kill switch's
      // armed floor holds the machine, which the reconcile respects.
      if (proto::IsFailsafeStop(st.stop_reason) && proto::IsSessionLive(before) &&
          !proto::IsSessionLive(st.state) && st.state != proto::TunnelState::Starting) {
        EnsureSession("unexpected drop", /*automaticRecovery=*/true);
      }
    });
    service_.SetDisconnectHandler([this] { OnServiceDisconnected(); });
    service_.Connect();  // ok if the service isn't up yet; retried on demand
    // The provider-only device's statistics (ProviderOnlyStatsLoop): idle until
    // there is a provider-only device to ask about and a window to show it in.
    if (!providerOnlyThread_.joinable()) {
      providerOnlyThread_ = std::thread([this] { ProviderOnlyStatsLoop(); });
    }

    // RESTORE THE API'S AUTHORIZATION FROM THE PERSISTED SESSION.
    //
    // Without this every authenticated Api call 401s on the second and every
    // subsequent launch, for every signed-in user. api_->setByJwt was called in
    // exactly ONE place - RegisterNetworkClient, on the fresh-login path - so
    // the token lived only in the Api object of the process that did the login.
    // Relaunch rebuilt the Api with no token while the app still LOOKED signed
    // in: the client jwt is on disk, the by jwt parses locally, the home view
    // renders and the network name is right, and then every request comes back
    // "401 Unauthorized: Not authorized."
    //
    // Measured on the beta test network: sign in, restart, and getNetworkUser,
    // getNetworkReferralCode, getReferralNetwork, accountPreferencesGet and
    // subscriptionBalance all 401. This is why no screen in this client had
    // ever rendered a 200 - every Class A surface was being judged against
    // "empty states" that were really auth failures.
    //
    // getByJwt() is the USER jwt, which is what the Api authorizes with;
    // getByClientJwt() is the device credential the tunnel session needs.
    if (const std::string byJwt = localState_->getByJwt(); !byJwt.empty()) {
      api_->setByJwt(byJwt);
      LogInfo("sdkhost: restored the api session from local state");
    }

    loggedIn_.store(!localState_->getByClientJwt().empty(), std::memory_order_release);
    if (loggedIn_.load(std::memory_order_acquire)) {
      SetAuthState(AuthState::LoggedIn);
      // Resume the session off the UI path.
      //
      // The result is CONSUMED. It used to be discarded, so every bootstrap
      // failure on resume â€” service down, service too old, a mode refusal â€”
      // produced a logged-in home screen with no DeviceRemote, no dialog and no
      // error state, with the only evidence a LogError in a file a WinUI3 app
      // never shows anyone. A failure the user cannot see is a failure that
      // gets reported as "the app just doesn't work".
      //
      // It is the SHARED worker now rather than a thread of its own. Bootstrap
      // is no longer a thing that happens once at launch: Connect asks for it,
      // a network-server change asks for it, and the service coming back asks
      // for it. One worker means those can never race into two concurrent
      // start_tunnels, and the failure reporting is written once.
      //
      // NOT AuthState::Error on failure. That enum means "authentication
      // failed", and the window derives `loggedIn = (state == LoggedIn)` from
      // it — so reporting a transport failure that way dumps a user whose JWT
      // is completely intact onto the sign-in screen, and it LATCHES. The auth
      // state stays LoggedIn (already set above) and the reason goes out on the
      // notice channel, which exists precisely to carry "why this app is not
      // carrying traffic" without touching auth.
      //
      // D8: "resume" REATTACHES ONLY. An app launch is not a Connect gesture,
      // so a launch that finds a live session adopts it (that is #40's whole
      // flow, unchanged) and a launch that finds none starts nothing — the
      // forensics have app-launch resumes installing capture routes on
      // machines nobody touched, and the owner's decision is click-only.
      //
      // A launch that finds none still provides when the stored mode says so:
      // the pass ends with the provider reconcile, and the provider-only
      // device installs nothing on this machine (ProvideLifecycle.h), so D8 is
      // untouched by it.
      EnsureSession("resume");
    } else {
      SetAuthState(AuthState::LoggedOut);
      // Signed out on THIS space, which on a custom-server build is a normal
      // and recoverable state rather than an error — but it is also the state
      // the app used to enter by accident every launch (see BuildNetworkSpace),
      // so say which space the answer came from.
      LogInfo("sdkhost: no stored device credentials in network space '{}' — "
              "starting signed out",
              networkSpace_->getHostName());
      // A provider-only device an earlier run left for another space's account
      // outlives the app with the service; signed out, nothing provides.
      RequestProviderReconcile("launch, signed out");
    }
    return true;
  } catch (const std::exception& e) {
    LogError("sdkhost: initialize failed: {}", e.what());
    SetAuthState(AuthState::Error, e.what());
    return false;
  }
}

// LOCK-FREE ON PURPOSE. This used to take mutex_ and read
// localState_->getByClientJwt(), which put it behind whatever else held the
// lock — and on a resume that is the detached bootstrap thread, holding mutex_
// for the WHOLE of BootstrapSession (service connect, Hello, start_tunnel:
// seconds, and on a machine with no service running, the full timeout).
// MainWindow's constructor calls this, so the main window did not appear until
// the tunnel bootstrap had finished, measured at roughly ten seconds. The
// answer to "is there a stored session" cannot be worth waiting on a network
// round trip for.
//
// loggedIn_ is written wherever the stored client jwt changes: Initialize,
// RegisterNetworkClient's success, ApplyNetworkServer's re-derive, and Logout.
// Note that Logout's own commit is asynchronous (asyncLocalState_->logout),
// so the old lock-taking version ALSO returned stale-true for a while after a
// sign-out; the flag is if anything the more accurate of the two.
bool SdkHost::IsLoggedIn() { return loggedIn_.load(std::memory_order_acquire); }

void SdkHost::SetAuthState(AuthState s, const std::string& error) {
  authState_ = s;
  if (onAuth_) onAuth_(s, error);
}

void SdkHost::LoginWithPassword(const std::string& userAuth,
                                const std::string& password,
                                std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  urnet::AuthLoginWithPasswordArgs args;
  args.user_auth = userAuth;
  args.password = password;
  // an unverified account gets a numeric one-time code (macOS parity); the UI
  // routes verification_required into the verify step
  args.verify_otp_numeric = true;

  api_->authLoginWithPassword(
      args, [this, done](std::optional<urnet::AuthLoginWithPasswordResult> result,
                         std::optional<std::string> err) {
        if (err || !result) {
          AuthResult r{false, false, err ? *err : "no result"};
          SetAuthState(AuthState::Error, r.error);
          if (done) done(r);
          return;
        }
        if (result->error && !result->error->message.empty()) {
          AuthResult r{false, false, result->error->message};
          SetAuthState(AuthState::Error, r.error);
          if (done) done(r);
          return;
        }
        if (result->verification_required) {
          AuthResult r{false, true, ""};
          r.verify_send = VerifySendNoticeOf(result->verification_required->send_error);
          SetAuthState(AuthState::LoggedOut);
          if (done) done(r);  // UI routes to the verify screen
          return;
        }
        if (result->network && result->network->by_jwt) {
          RegisterNetworkClient(*result->network->by_jwt, done);
        } else {
          AuthResult r{false, false, "login returned no network"};
          SetAuthState(AuthState::Error, r.error);
          if (done) done(r);
        }
      });
}

void SdkHost::LoginWithCode(const std::string& authCode,
                            std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  urnet::AuthCodeLoginArgs args;
  args.auth_code = authCode;
  api_->authCodeLogin(args, [this, done](std::optional<urnet::AuthCodeLoginResult> result,
                                         std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (!result->by_jwt.empty()) {
      RegisterNetworkClient(result->by_jwt, done);
    } else {
      AuthResult r{false, false, "code login returned no jwt"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
    }
  });
}

// ---- account discovery / sign-up / verify / reset ---------------------------
// macOS Authenticate/** parity. All results are delivered on SDK callback
// threads; the UI marshals onto its thread.

void SdkHost::StartLogin(const std::string& userAuth,
                         std::function<void(LoginRouting)> done) {
  urnet::AuthLoginArgs args;
  args.user_auth = userAuth;

  api_->authLogin(args, [this, userAuth, done](std::optional<urnet::AuthLoginResult> result,
                                               std::optional<std::string> err) {
    LoginRouting routing;
    routing.userAuth = userAuth;
    if (err || !result) {
      routing.route = LoginRoute::Error;
      routing.error = err ? *err : "no result";
      if (done) done(routing);
      return;
    }
    if (result->user_auth && !result->user_auth->empty()) {
      routing.userAuth = *result->user_auth;  // the normalized echo
    }
    if (result->error && !result->error->message.empty()) {
      routing.route = LoginRoute::Error;
      routing.error = result->error->message;
      if (done) done(routing);
      return;
    }
    // a jwt straight from discovery (not the user-auth path, but handle it)
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, [done](AuthResult r) {
        LoginRouting routed;
        routed.route = r.ok ? LoginRoute::Login : LoginRoute::Error;
        routed.error = r.error;
        if (done) done(routed);
      });
      return;
    }
    if (result->auth_allowed && !result->auth_allowed->empty()) {
      const auto& allowed = *result->auth_allowed;
      if (std::find(allowed.begin(), allowed.end(), "password") != allowed.end()) {
        routing.route = LoginRoute::Password;
      } else {
        // the account exists under another sign-in method (e.g. a wallet)
        routing.route = LoginRoute::IncorrectAuth;
        for (const auto& method : allowed) {
          if (!routing.authAllowed.empty()) routing.authAllowed += ", ";
          routing.authAllowed += method;
        }
      }
      if (done) done(routing);
      return;
    }
    // unknown user auth: create a new network
    routing.route = LoginRoute::Create;
    if (done) done(routing);
  });
}

void SdkHost::CreateNetwork(const CreateNetworkParams& params,
                            std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  if (params.useWalletAuth) {
    std::optional<urnet::WalletAuthArgs> identity;
    {
      std::scoped_lock lock(mutex_);
      identity = pendingWalletAuth_;
    }
    if (!identity) {
      AuthResult r{false, false, "no wallet sign-in is pending"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }

    const std::string blockchain = identity->blockchain.value_or(std::string());
    const std::string expectedAddress =
        identity->wallet_address.value_or(std::string());
    // The creation is a wallet flow from here: it supersedes any other at once, and
    // its challenge opens the bridge only while it is still the current flow.
    const uint64_t flow = CancelPendingWalletFlows("superseded by wallet network creation");
    if (blockchain == urnet::TAO) {
      // a second proof with the same wallet, bound to the same address (the
      // session refuses another signing account)
      const std::string walletId = pendingBittensorWalletId_.empty()
                                       ? std::string(bittensor::kWalletTalisman)
                                       : pendingBittensorWalletId_;
      BeginBittensorProof(
          flow, walletId, std::string(bittensor::kPurposeCreate), expectedAddress,
          [this, params, identity = *identity, flow, walletId,
           done = std::move(done)](BittensorProofOutcome outcome) mutable {
            if (!outcome.ok) {
              if (!walletFlows_.IsCurrent(flow)) {
                // another wallet flow superseded the creation; the auth state is its
                if (done) done({false, false, outcome.error});
                return;
              }
              AuthResult r{false, false, outcome.error};
              SetAuthState(AuthState::LoggedOut, r.error);
              if (done) done(r);
              return;
            }
            auto createAuth = identity;
            createAuth.wallet_address = outcome.proof.Address;
            createAuth.wallet_message = outcome.proof.Message;
            createAuth.wallet_signature = outcome.proof.Signature;
            SubmitCreateNetwork(params, std::move(createAuth), std::move(done), walletId);
          });
      return;
    }
    RequestWalletChallenge(
        blockchain, expectedAddress,
        [this, params, identity = *identity, expectedAddress, blockchain, flow,
         done = std::move(done)](std::optional<std::string> message,
                                 std::string error) mutable {
      if (!walletFlows_.IsCurrent(flow)) {
        // Another wallet flow took the bridge meanwhile. Nothing waits in a slot
        // for this creation, so it answers its own caller; the auth state is the
        // newer flow's to move.
        LogWarn("sdkhost: a superseded network creation's wallet challenge arrived, dropping it");
        if (done) done({false, false, "superseded by another wallet request"});
        return;
      }
      if (!message) {
        AuthResult r{false, false,
                     error.empty() ? "could not fetch wallet challenge" : error};
        SetAuthState(AuthState::LoggedOut, r.error);
        if (done) done(r);
        return;
      }

      walletSignMessage_ = *message;
      walletSignDone_ =
          [this, params, identity, expectedAddress, message = *message,
           done = std::move(done)](bool ok, std::string publicKey,
                                   std::string signature,
                                   std::string signError) mutable {
        if (!ok) {
          AuthResult r{false, false,
                       signError.empty() ? "wallet signing failed" : signError};
          SetAuthState(AuthState::LoggedOut, r.error);
          if (done) done(r);
          return;
        }
        if (publicKey != expectedAddress) {
          AuthResult r{false, false,
                       "wallet account changed; use the same account to create the network"};
          SetAuthState(AuthState::LoggedOut, r.error);
          if (done) done(r);
          return;
        }

        auto createAuth = identity;
        createAuth.wallet_address = publicKey;
        createAuth.wallet_message = message;
        createAuth.wallet_signature = signature;
        SubmitCreateNetwork(params, std::move(createAuth), std::move(done));
      };

      wallet_.SignMessage(*message);
    });
    return;
  }

  SubmitCreateNetwork(params, std::nullopt, std::move(done));
}

void SdkHost::SubmitCreateNetwork(const CreateNetworkParams& params,
                                  std::optional<urnet::WalletAuthArgs> walletAuth,
                                  std::function<void(AuthResult)> done,
                                  const std::string& bittensorWalletId) {
  urnet::NetworkCreateArgs args;
  ApplySignupPreferences(args);
  args.user_name = std::string();
  args.network_name = params.networkName;
  args.terms = params.terms;
  args.verify_use_numeric = true;
  if (params.useWalletAuth) {
    if (!walletAuth) {
      AuthResult r{false, false, "no wallet sign-in is pending"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    args.wallet_auth = std::move(walletAuth);
    // a signature from another account than the address comes back as
    // result.error.code (a 401 error otherwise)
    args.result_errors = true;
  } else if (params.useAuthJwt) {
    std::scoped_lock lock(mutex_);
    if (!pendingAuthJwt_) {
      AuthResult r{false, false, "no SSO sign-in is pending"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    args.auth_jwt_type = pendingAuthJwtType_.empty() ? std::string("google") : pendingAuthJwtType_;
    args.auth_jwt = *pendingAuthJwt_;
  } else {
    args.user_auth = params.userAuth;
    args.password = params.password;
  }
  if (!params.referralCode.empty()) args.referral_code = params.referralCode;

  api_->networkCreate(args, [this, done, bittensorWalletId](
                                std::optional<urnet::NetworkCreateResult> result,
                                std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false,
                   WalletProofRefusalText(result->error->code.value_or(std::string()),
                                          result->error->message, bittensorWalletId)};
      SetAuthState(AuthState::LoggedOut);  // a form error, not a session error
      if (done) done(r);
      return;
    }
    if (result->verification_required) {
      AuthResult r{false, true, ""};
      r.verify_send = VerifySendNoticeOf(result->verification_required->send_error);
      SetAuthState(AuthState::LoggedOut);
      if (done) done(r);  // the UI routes to the verify step
      return;
    }
    if (result->network && result->network->by_jwt && !result->network->by_jwt->empty()) {
      {
        std::scoped_lock lock(mutex_);
        // consumed by this create, or unused by it
        pendingWalletAuth_.reset();
        pendingAuthJwt_.reset();
        pendingAuthJwtType_.clear();
      }
      RegisterNetworkClient(*result->network->by_jwt, done);
      return;
    }
    AuthResult r{false, false, "create network returned no network"};
    SetAuthState(AuthState::Error, r.error);
    if (done) done(r);
  });
}

void SdkHost::VerifyCode(const std::string& userAuth, const std::string& code,
                         std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  urnet::AuthVerifyArgs args;
  args.user_auth = userAuth;
  args.verify_code = code;

  api_->authVerify(args, [this, done](std::optional<urnet::AuthVerifyResult> result,
                                      std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::LoggedOut);  // a wrong code, not a session error
      if (done) done(r);
      return;
    }
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, done);
      return;
    }
    AuthResult r{false, false, "verify returned no network"};
    SetAuthState(AuthState::Error, r.error);
    if (done) done(r);
  });
}

void SdkHost::ResendVerifyCode(const std::string& userAuth,
                               std::function<void(VerifySendNotice)> done) {
  urnet::AuthVerifySendArgs args;
  args.user_auth = userAuth;
  args.use_numeric = true;
  // a code the server did not send comes back as result.error; a server that
  // predates the flag answers an error status instead
  args.result_errors = true;
  api_->authVerifySend(args, [done](std::optional<urnet::AuthVerifySendResult> result,
                                    std::optional<std::string> err) {
    if (!done) return;
    if (err || !result) {
      done(VerifySendNoticeFor(true, std::string(), std::string(), 0));
      return;
    }
    done(VerifySendNoticeOf(result->error));
  });
}

void SdkHost::SendPasswordResetLink(const std::string& userAuth,
                                    std::function<void(VerifySendNotice)> done) {
  urnet::AuthPasswordResetArgs args;
  args.user_auth = userAuth;
  // a link the server did not send comes back as result.error; a server that
  // predates the flag answers an error status instead
  args.result_errors = true;
  api_->authPasswordReset(args, [done](std::optional<urnet::AuthPasswordResetResult> result,
                                       std::optional<std::string> err) {
    if (err || !result) {
      LogWarn("sdkhost: authPasswordReset failed: {}", err ? *err : std::string());
      if (done) done(VerifySendNoticeFor(true, std::string(), std::string(), 0));
      return;
    }
    if (done) done(VerifySendNoticeOf(result->error));
  });
}

// ---- seedphrase -------------------------------------------------------------
// macOS LoginSeedphrase / CreateNetworkInstant parity. A seedphrase is the whole
// credential and has no reset path, so: it is never written to the log, never
// persisted by this app, and the instant-account flow refuses to leave a live
// session behind a seedphrase the user has not been shown.

namespace {

// lowercase, trimmed, single-spaced — the normalization every client applies
// before sending a seedphrase, so a phrase pasted with newlines or double
// spaces authenticates (macOS LoginSeedphraseViewModel.normalizedSeedphrase).
std::string NormalizeSeedphrase(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  bool pendingSpace = false;
  for (unsigned char c : raw) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

}  // namespace

void SdkHost::LoginWithSeedphrase(const std::string& seedphrase,
                                  std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  urnet::AuthLoginArgs args;
  args.seedphrase = NormalizeSeedphrase(seedphrase);

  // NOTE: nothing on any path below may echo the args. An error log that
  // included the request would put the credential in a file on disk.
  api_->authLogin(args, [this, done](std::optional<urnet::AuthLoginResult> result,
                                     std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      // a wrong phrase is a form error, not a session error
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::LoggedOut);
      if (done) done(r);
      return;
    }
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, done);
      return;
    }
    AuthResult r{false, false, "seedphrase login returned no network"};
    SetAuthState(AuthState::LoggedOut);
    if (done) done(r);
  });
}

void SdkHost::CreateInstantAccount(std::function<void(InstantAccount)> done) {
  // NO user_auth, password, auth_jwt or wallet_auth: that combination is what
  // makes the server mint a seedphrase-secured network and return the phrase.
  urnet::NetworkCreateArgs args;
  ApplySignupPreferences(args);
  args.terms = true;  // the form's button is gated on the terms consent

  api_->networkCreate(args, [this, done](std::optional<urnet::NetworkCreateResult> result,
                                         std::optional<std::string> err) {
    InstantAccount out;
    if (err || !result) {
      out.error = err ? *err : "no result";
      if (done) done(out);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      out.error = result->error->message;
      if (done) done(out);
      return;
    }
    if (result->verification_required) {
      // An instant account carries no user auth, so there is nothing to verify
      // and no step that could take a code. Say so rather than routing the user
      // to a dead verify screen (macOS parity).
      out.error = "the server asked to verify an account with no user auth";
      if (done) done(out);
      return;
    }
    if (!result->seedphrase || result->seedphrase->empty()) {
      // Refuse to register. A network whose only credential never reached the
      // user is an account nobody can ever get back into.
      out.error = "instant account returned no seedphrase";
      if (done) done(out);
      return;
    }
    if (!result->network || !result->network->by_jwt || result->network->by_jwt->empty()) {
      out.error = "instant account returned no network";
      if (done) done(out);
      return;
    }
    {
      std::scoped_lock lock(mutex_);
      pendingInstantJwt_ = *result->network->by_jwt;
    }
    out.ok = true;
    out.seedphrase = *result->seedphrase;
    if (done) done(out);
  });
}

void SdkHost::ConfirmInstantAccount(std::function<void(AuthResult)> done) {
  std::string jwt;
  {
    std::scoped_lock lock(mutex_);
    if (!pendingInstantJwt_) {
      if (done) done({false, false, "no instant account is pending"});
      return;
    }
    jwt = *pendingInstantJwt_;
    pendingInstantJwt_.reset();
  }
  SetAuthState(AuthState::Authenticating);
  RegisterNetworkClient(jwt, done);
}

void SdkHost::DiscardInstantAccount() {
  std::scoped_lock lock(mutex_);
  pendingInstantJwt_.reset();
}

// ---- network server (iOS NetworkServerSheet parity) ------------------------

SdkHost::NetworkServer SdkHost::CurrentNetworkServer() {
  std::scoped_lock lock(mutex_);
  NetworkServer out;
  out.managerAvailable = spaceManager_.has_value();
  // The same resolution BuildNetworkSpace does, so "Use default network" means
  // the network this process was started against and not, silently, production.
  out.defaultHostName = EnvVar(L"URNETWORK_NETWORK_HOST");
  if (out.defaultHostName.empty()) out.defaultHostName = std::string(ids::kNetworkSpaceHostName);
  if (!networkSpace_) return out;
  try {
    out.hostName = networkSpace_->getHostName();
    out.apiUrl = networkSpace_->getApiUrl();
    out.connectUrl = networkSpace_->getPlatformUrl();
    out.configuredApiUrl = networkSpace_->getConfiguredApiUrl();
    out.configuredConnectUrl = networkSpace_->getConfiguredPlatformUrl();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: read network space failed: {}", e.what());
  }
  return out;
}

bool SdkHost::ApplyNetworkServer(const std::string& hostName, const std::string& apiUrl,
                                 const std::string& connectUrl) {
  if (hostName.empty()) return false;
  bool ok = false;
  bool loggedIn = false;
  {
    std::scoped_lock lock(mutex_);
    if (!spaceManager_) return false;

    // A different space is a different LocalState and so a different stored
    // jwt: the running session belongs to the OLD server and cannot survive it.
    if (device_) {
      try {
        TeardownSessionLocked();
      } catch (const std::exception& e) {
        LogWarn("sdkhost: teardown before network switch failed: {}", e.what());
      }
    }
    if (networkNameVc_) {
      try {
        networkNameVc_->stop();
        networkNameVc_->close();
      } catch (...) {
      }
      networkNameVc_.reset();
    }
    pendingWalletAuth_.reset();
    pendingAuthJwt_.reset();
    pendingAuthJwtType_.clear();
    pendingInstantJwt_.reset();

    try {
      const bool official = (hostName == std::string(ids::kNetworkSpaceHostName));

      urnet::NetworkSpaceKey key;
      key.host_name = hostName;
      key.env_name = std::string(ids::kNetworkSpaceEnvName);

      // The same value set BuildNetworkSpace writes, with the host-dependent
      // parts varied (iOS DeviceManager.applyNetworkSpace parity), and written
      // over what the space stores under this key, because these values
      // replace the stored ones whole: what the user saved in that space -- its
      // extender settings, private extender, bootstrap DNS-over-HTTPS servers
      // and VLESS server, each edited on its own screen -- survives applying
      // the domain or its urls again. A host never applied before has none.
      // Only the host's values and the url overrides change
      // (NetworkSpaceStartup.h ServerSpaceValuesOver).
      const urnet::NetworkSpaceValues values = netspace::ServerSpaceValuesOver(
          StoredSpaceValuesLocked(key).value_or(urnet::NetworkSpaceValues{}), official, hostName,
          apiUrl, connectUrl);

      networkSpace_ = spaceManager_->updateNetworkSpaceValues(key, values);
      spaceManager_->setActiveNetworkSpace(*networkSpace_);

      // Everything derived from the space has to be re-derived: the Api talks
      // to the new host, and the LocalState holds the new host's jwt.
      api_ = networkSpace_->getApi();
      BindApiLocked();
      asyncLocalState_ = networkSpace_->getAsyncLocalState();
      localState_ = asyncLocalState_->getLocalState();
      events_ = std::make_unique<ClientEventQueue>(networkSpace_->handle(), appVersion_,
                                                   ClientEventLocale());
      networkNameVc_ = urnet::newNetworkNameValidationViewController(*api_);
      networkNameVc_->start();
      loggedIn = !localState_->getByClientJwt().empty();
      loggedIn_.store(loggedIn, std::memory_order_release);
      // the new space's Api needs the new space's jwt, for the same reason
      // Initialize() does (see the note there)
      if (const std::string byJwt = localState_->getByJwt(); !byJwt.empty()) {
        api_->setByJwt(byJwt);
      }
      ok = true;
    } catch (const std::exception& e) {
      LogError("sdkhost: switch network space to '{}' failed: {}", hostName, e.what());
      ok = false;
    }
  }
  if (!ok) return false;
  LogInfo("sdkhost: active network space is now '{}' (api '{}', connect '{}')", hostName,
          apiUrl.empty() ? "derived" : apiUrl,
          connectUrl.empty() ? "derived" : connectUrl);
  // The new space's stored auth decides what the window shows. It is almost
  // always LoggedOut — a fresh server has no jwt — and saying so is the point:
  // the old session is genuinely gone.
  SetAuthState(loggedIn ? AuthState::LoggedIn : AuthState::LoggedOut);
  // ...and when it is NOT LoggedOut, the app is now signed in with NO SESSION,
  // which is the state Connect could not recover from.
  //
  // This is the second half of the launch bug BuildNetworkSpace describes. Every
  // recent run on this machine went: launch into the default space (signed out,
  // no bootstrap), user re-picks their server here, `loggedIn` comes back true,
  // the shell switches to Home — and nothing anywhere created a DeviceRemote,
  // because this function's only two callers of BootstrapSession were the resume
  // thread and a fresh sign-in, and this is neither. Sign in, look connected-
  // capable, press Connect, nothing happens, no reason given.
  //
  // Either way the provider-only device follows the space: that pass ends with
  // the provider reconcile, whose request now carries the new space and jwt
  // (so the service builds a new device), and a space with no stored
  // credentials stops the one the old account was running.
  if (loggedIn) {
    EnsureSession("network server change");
  } else {
    RequestProviderReconcile("network server change, signed out");
  }
  return true;
}

void SdkHost::CheckNetworkName(const std::string& networkName,
                               std::function<void(bool ok, bool available)> done) {
  if (!networkNameVc_) {
    if (done) done(false, false);
    return;
  }
  networkNameVc_->networkCheck(
      networkName, [done](std::optional<urnet::NetworkCheckResult> result,
                          std::optional<std::string> err) {
        if (err || !result) {
          if (done) done(false, false);
          return;
        }
        if (done) done(true, result->available);
      });
}

bool SdkHost::HasPendingWalletAuth() {
  std::scoped_lock lock(mutex_);
  return pendingWalletAuth_.has_value();
}

std::optional<urnet::ByJwt> SdkHost::ParsedJwt() {
  std::scoped_lock lock(mutex_);
  if (!localState_) return std::nullopt;
  try {
    return localState_->parseByJwt();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: parse jwt failed: {}", e.what());
    return std::nullopt;
  }
}

void SdkHost::RefreshJwt() {
  std::scoped_lock lock(mutex_);
  if (!device_) return;
  try {
    device_->refreshToken(0);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: refresh token failed: {}", e.what());
  }
}

void SdkHost::RegisterNetworkClient(const std::string& byJwt,
                                    std::function<void(AuthResult)> done) {
  {
    // A new network jwt invalidates a running session (guest upgrade, verify
    // after an upgrade): tear the device + tunnel down so the registration
    // below rebuilds them under the new auth (linux SdkHost parity). Fresh
    // sign-ins have no device and skip this.
    std::scoped_lock lock(mutex_);
    if (device_) {
      try {
        TeardownSessionLocked();
      } catch (const std::exception& e) {
        LogWarn("sdkhost: pre-registration teardown failed: {}", e.what());
      }
    }
  }
  // Persist the network JWT, then register this device to obtain a client JWT.
  localState_->getByJwt();  // touch
  api_->setByJwt(byJwt);

  urnet::AuthNetworkClientArgs args;
  args.description = DeviceDescription();
  args.device_spec = DeviceSpec();

  AuthNetworkClientWithLocale(args, [this, byJwt, done](std::optional<urnet::AuthNetworkClientResult> result,
                                                        std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->by_client_jwt) {
      {
        std::scoped_lock lock(mutex_);
        try {
          // SYNCHRONOUS setters, under the lock. These were
          // asyncLocalState_->set*(..., [](bool){}): hand the commit to the
          // SDK's own thread and carry on — and the next reader (then an
          // immediate BootstrapSession, today the first Connect press's
          // bootstrap) raced it. On a fresh install there is no earlier value
          // to read, so the FIRST sign-in lost that race and reported "no
          // client credentials are stored for this device" over an authLogin
          // and an authNetworkClient that had both just succeeded. Pressing
          // sign in again worked, because by then the async commit had landed
          // — which is exactly what made it look like a flaky server rather
          // than our own ordering. A fast Connect press can still arrive
          // within milliseconds of this callback, so the setters stay
          // synchronous.
          localState_->setByJwt(byJwt);
          localState_->setByClientJwt(*result->by_client_jwt);
          loggedIn_.store(true, std::memory_order_release);
        } catch (const std::exception& e) {
          LogWarn("sdkhost: persist jwt failed: {}", e.what());
        }
      }
      // THE SIGN-IN SUCCEEDED, AND THAT IS ALL THAT HAPPENED. D8, owner
      // decision: signing in does not start the tunnel. BootstrapSession used
      // to run right here — the unlogged session start the forensics found
      // (start #5, no gesture, no reason line, because this call site never
      // went through the session worker and its 'starting a session' log). A
      // session now exists only when a Connect gesture asks for one, and the
      // one start_tunnel call site logs its reason every time.
      //
      // Auth state goes LoggedIn unconditionally: a sign-in with the service
      // down is still a successful sign-in, and reporting anything else here
      // used to dump a user with a perfectly valid credential onto the
      // sign-in screen (the seedphrase step rendered it as "your phrase is
      // wrong" — the most alarming sentence this app can say to somebody
      // whose credential has no reset path).
      LogInfo("sdkhost: signed in; no session started — the tunnel starts "
              "only on a Connect gesture");
      SetAuthState(AuthState::LoggedIn);
      // ...but a stored provide mode that provides while disconnected starts
      // the provider-only device, under the new client jwt (the service
      // replaces one built from an older jwt: the request differs). It installs
      // nothing on this machine, so this is not the session start D8 forbids
      // here.
      RequestProviderReconcile("signed in");
      AuthResult r{true, false, ""};
      if (done) done(r);
    } else {
      AuthResult r{false, false, "device registration returned no client jwt"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
    }
  });
}

// ---- Sign in with a wallet (Solana / Bittensor via ur.io/wallet-connect) ----

void SdkHost::SetupWalletCallbacks() {
  // Every return is routed to the flow waiting for it (WalletBridgeRoute.h), and
  // one nobody waits for is dropped. The bridge page keeps "Return to URnetwork"
  // on screen after its automatic redirect and the key pair lives until the next
  // Connect, so a second delivery of a connect return decrypts again: it must
  // never become a wallet sign-in in a signed-in app. A wallet sign-in is waiting
  // when walletAuthDone_ is set and no sso attempt owns it.
  wallet_.on_public_key = [this](std::string publicKey, WalletConnect::Provider provider) {
    // Solana connects first, then signs. Bittensor has no connect step (it
    // returns the address with the signature), so nothing to chain here.
    const bool bittensor = provider == WalletConnect::Provider::Bittensor;
    switch (bridge::RoutePublicKey(bittensor, static_cast<bool>(walletConnectDone_),
                                   static_cast<bool>(walletSignDone_),
                                   walletAuthDone_ && !ssoAttempt_)) {
      case bridge::PublicKeyRoute::Drop:
        if (!bittensor) {
          LogWarn("sdkhost: a wallet connect return arrived with no flow in flight, ignoring it");
        }
        return;
      case bridge::PublicKeyRoute::AnswerConnect:
        // A bare connect request (ConnectSolanaWallet) wants the address and
        // nothing more: no challenge, no signature.
        std::exchange(walletConnectDone_, nullptr)(true, std::move(publicKey), std::string());
        return;
      case bridge::PublicKeyRoute::SignForRequest:
        // A bare signature request carries its own message (SignSolanaForAdd).
        wallet_.SignMessage(walletSignMessage_);
        return;
      case bridge::PublicKeyRoute::SignIn:
        break;
    }

    // the sign-in's challenge opens the bridge only while that sign-in still owns it
    const uint64_t flow = walletFlows_.Current();
    RequestWalletChallenge(urnet::SOL, publicKey,
                           [this, flow](std::optional<std::string> message, std::string error) {
      if (!walletFlows_.IsCurrent(flow)) {
        LogWarn("sdkhost: a superseded wallet sign-in's challenge arrived, dropping it");
        return;
      }
      if (!message) {
        if (wallet_.on_error)
          wallet_.on_error(error.empty() ? "could not fetch wallet challenge" : error);
        return;
      }
      walletAuthMessage_ = *message;
      wallet_.SignMessage(*message);
    });
  };
  wallet_.on_signature = [this](std::string publicKey, std::string signature,
                                WalletConnect::Provider provider) {
    // NO SIGN-IN IN FLIGHT: a signature from a superseded or abandoned tab (a
    // Bittensor connect the user replaced with a Solana one, a stale challenge)
    // must not reach AuthLoginWithWallet, which would move the auth state.
    switch (bridge::RouteSignature(static_cast<bool>(walletSignDone_),
                                   walletAuthDone_ && !ssoAttempt_)) {
      case bridge::SignatureRoute::Drop:
        LogWarn("sdkhost: a wallet signature arrived with no flow in flight, ignoring it");
        return;
      case bridge::SignatureRoute::AnswerRequest:
        std::exchange(walletSignDone_, nullptr)(true, std::move(publicKey), std::move(signature),
                                                std::string());
        return;
      case bridge::SignatureRoute::SignIn:
        break;
    }
    AuthLoginWithWallet(publicKey, signature, walletAuthMessage_, provider);
  };
  wallet_.on_sso = [this](std::string provider, std::string authJwt, std::string state,
                          std::string error) {
    // NO ATTEMPT IN FLIGHT (see on_error below): a late or replayed callback
    // must not be able to move the auth state.
    if (!ssoAttempt_) {
      LogWarn("sdkhost: an sso callback arrived with no sign-in in flight, ignoring it");
      return;
    }
    // Not this attempt: the api's callback echoes `state` untouched, so a mismatch is a
    // stale tab or a forged link, not an answer.
    if (state.empty() || state != ssoAttempt_->state || provider != ssoAttempt_->provider) {
      LogWarn("sdkhost: an sso callback did not match the sign-in in flight, ignoring it");
      return;
    }
    const SsoAttempt attempt = *ssoAttempt_;
    ssoAttempt_.reset();
    if (!error.empty() || authJwt.empty()) {
      if (wallet_.on_error) wallet_.on_error(error.empty() ? "sign-in returned no identity token" : error);
      return;
    }
    // The token must be the one this attempt asked for: the provider put the
    // attempt's nonce inside it.
    const auto nonce = JwtClaimString(authJwt, "nonce");
    if (!nonce || *nonce != attempt.nonce) {
      if (wallet_.on_error) wallet_.on_error("the identity token did not match this sign-in");
      return;
    }
    // Who started the attempt decides where its token goes: an add-owned
    // attempt adds the identity to the current network and never signs in.
    switch (add_sign_in::RouteSsoReturn(attempt.purpose)) {
      case add_sign_in::SsoReturnRoute::AddAuth:
        if (auto addDone = std::exchange(ssoAddDone_, nullptr)) addDone(authJwt, std::string());
        return;
      case add_sign_in::SsoReturnRoute::AuthLogin:
        break;
    }
    auto done = std::exchange(walletAuthDone_, nullptr);
    AuthLoginWithSso(attempt.provider, authJwt, done ? done : [](AuthResult) {});
  };
  // Bittensor hand-backs go to the session helper, never through the routes
  // above: it decides whether the link belongs to the proof in flight.
  wallet_.on_bittensor_return = [this](std::string url) { HandleBittensorReturn(url); };
  wallet_.on_error = [this](std::string err) {
    // The browser could not open for a Bittensor proof: answer that proof.
    {
      uint64_t serial = 0;
      bool pending = false;
      {
        std::scoped_lock lock(bittensorLock_);
        pending = static_cast<bool>(bittensorDone_);
        serial = bittensorSerial_;
      }
      if (pending) {
        FinishBittensorProof(serial, {false, {}, err});
        return;
      }
    }
    // A failed connect or signature request is NOT a failed sign-in: the user is
    // signed in throughout, and pushing AuthState::Error here would tear the
    // session down because a browser tab was closed.
    if (auto connectDone = std::exchange(walletConnectDone_, nullptr)) {
      connectDone(false, std::string(), err);
      return;
    }
    if (auto signDone = std::exchange(walletSignDone_, nullptr)) {
      signDone(false, std::string(), std::string(), err);
      return;
    }
    // an add-owned sso attempt failed: the add sheet shows it, the session stays
    if (auto addDone = std::exchange(ssoAddDone_, nullptr)) {
      ssoAttempt_.reset();
      addDone(std::string(), err);
      return;
    }
    // NO FLOW IS IN FLIGHT. The bridge is a pair of process-wide callbacks with
    // no request id, so a deep link arriving late - from an attempt the user
    // abandoned, or one already answered - lands here looking exactly like a
    // fresh failure. It must not be able to move the auth state: doing that is
    // how a signed-in user gets thrown back to the login screen by a browser tab
    // they closed ten minutes ago.
    if (!walletAuthDone_) {
      LogWarn("sdkhost: a wallet-bridge error arrived with no flow in flight, "
              "ignoring it: {}",
              err);
      return;
    }
    auto done = std::exchange(walletAuthDone_, nullptr);
    SetAuthState(AuthState::Error, err);
    done({false, false, err});
  };
}

// The bridge exposes ONE pair of callbacks, so starting either flow supersedes
// the other. Superseding it must ANSWER it: dropping the callback on the floor
// left whatever was waiting on it - a busy flag, a greyed-out button - waiting
// for a reply that could no longer come. Neither caller can see that from
// where it stands.
uint64_t SdkHost::CancelPendingWalletFlows(const char* reason) {
  // First: from here on every earlier flow's challenge continuation is stale.
  const uint64_t flow = walletFlows_.Start();
  // an sso attempt answers through walletAuthDone_ below; its state/nonce die
  // with it so the bridge's late answer is ignored rather than acted on
  ssoAttempt_.reset();
  // a Bittensor proof is told, and its session refuses a late hand-back
  std::function<void(BittensorProofOutcome)> bittensorDone;
  {
    std::scoped_lock lock(bittensorLock_);
    bittensorDone = std::exchange(bittensorDone_, nullptr);
    if (bittensorSession_) bittensorSession_->cancel();
    bittensorSession_.reset();
    ++bittensorSerial_;
  }
  if (bittensorDone) {
    LogWarn("sdkhost: a Bittensor wallet proof was superseded ({})", reason);
    bittensorDone({false, {}, reason});
  }
  if (auto connectDone = std::exchange(walletConnectDone_, nullptr)) {
    LogWarn("sdkhost: a wallet connect request was superseded ({})", reason);
    connectDone(false, std::string(), reason);
  }
  if (auto signDone = std::exchange(walletSignDone_, nullptr)) {
    LogWarn("sdkhost: a wallet signature request was superseded ({})", reason);
    signDone(false, std::string(), std::string(), reason);
  }
  if (auto addDone = std::exchange(ssoAddDone_, nullptr)) {
    LogWarn("sdkhost: adding a sign-in method was superseded ({})", reason);
    addDone(std::string(), reason);
  }
  if (auto authDone = std::exchange(walletAuthDone_, nullptr)) {
    LogWarn("sdkhost: a wallet sign-in was superseded ({})", reason);
    authDone({false, false, reason});
  }
  return flow;
}

void SdkHost::SignInWithSolana(WalletConnect::Provider provider,
                               std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  {
    std::scoped_lock lock(mutex_);
    pendingWalletAuth_.reset();  // a fresh sign-in supersedes any retained auth
  }
  // ...and any pending bare signature request, which is TOLD it was superseded
  CancelPendingWalletFlows("superseded by a wallet sign-in");
  walletAuthDone_ = std::move(done);
  wallet_.Connect(provider);  // opens the browser; the rest continues on the deep-link callback
}

void SdkHost::SignInWithBittensor(const std::string& walletId,
                                  std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  {
    std::scoped_lock lock(mutex_);
    pendingWalletAuth_.reset();  // a fresh sign-in supersedes any retained auth
  }
  const uint64_t flow = CancelPendingWalletFlows("superseded by a wallet sign-in");
  walletAuthDone_ = std::move(done);
  pendingBittensorWalletId_ = walletId;
  BeginBittensorProof(
      flow, walletId, std::string(bittensor::kPurposeLogin), std::string(),
      [this, flow, walletId](BittensorProofOutcome outcome) {
        // A newer flow superseded this sign-in and has answered it already.
        if (!walletFlows_.IsCurrent(flow)) return;
        if (!outcome.ok) {
          auto authDone = std::exchange(walletAuthDone_, nullptr);
          if (!authDone) return;
          if (bridge::IsSuperseded(outcome.error)) {
            // the user closed the wallet form: back to the sign-in buttons
            SetAuthState(AuthState::LoggedOut);
            authDone({false, false, std::string()});
            return;
          }
          SetAuthState(AuthState::Error, outcome.error);
          authDone({false, false, outcome.error});
          return;
        }
        AuthLoginWithWallet(outcome.proof.Address, outcome.proof.Signature,
                            outcome.proof.Message, WalletConnect::Provider::Bittensor, walletId);
      });
}

namespace {

int64_t BittensorNowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// A wallet_error from the bridge page reads in this app's words for the page's
// code when the app knows it, else in the page's own text.
std::string BittensorErrorText(std::string const& code, std::string const& walletMessage,
                               std::string const& bridgeCode = std::string(),
                               std::string const& walletId = std::string()) {
  const std::string key = bittensor::ErrorKey(code, "wallet_connect_failed");
  if (key.empty()) {
    const bittensor::BridgeErrorText bridge = bittensor::BridgeErrorTextFor(bridgeCode);
    if (!bridge.key.empty()) {
      return bridge.takesWalletName
                 ? Narrow(urnw::Format(bridge.key, Widen(urnet::bittensorWalletDisplayName(walletId))))
                 : Narrow(Localized(bridge.key));
    }
    return walletMessage.empty() ? Narrow(Localized("wallet_connect_failed")) : walletMessage;
  }
  return Narrow(Localized(key));
}

}  // namespace

std::string WalletProofRefusalText(const std::string& code, const std::string& message,
                                   const std::string& bittensorWalletId) {
  if (bittensorWalletId.empty()) return message;
  const std::string key = bittensor::ConnectErrorKey(
      code, urnet::bittensorWalletTransportFor(bittensorWalletId, std::string(bittensor::kPlatform)));
  if (key.empty()) return message;
  return Narrow(Format(key, Widen(urnet::bittensorWalletDisplayName(bittensorWalletId))));
}

void SdkHost::BeginBittensorProof(uint64_t flow, const std::string& walletId,
                                  const std::string& purpose, const std::string& expectedAddress,
                                  std::function<void(BittensorProofOutcome)> done,
                                  std::function<void(BittensorManualRequest)> manualHandler) {
  std::shared_ptr<urnet::BittensorWalletSession> session;
  try {
    session = std::make_shared<urnet::BittensorWalletSession>(urnet::newBittensorWalletSession(
        walletId, std::string(bittensor::kPlatform), purpose,
        std::string(bittensor::kRedirectLink)));
    // the WalletConnect page pairs with the app's configured project id, as
    // the pre-helper bridge did ("" = the page's own)
    if (bittensor::NeedsWalletConnectProjectId(walletId)) {
      session->setWalletConnectProjectId(urnw::config::kWalletConnectProjectId);
    }
  } catch (std::exception const& e) {
    LogError("sdkhost: no Bittensor wallet session for {}: {}", walletId, e.what());
    done({false, {}, Narrow(Localized("wallet_connect_failed"))});
    return;
  }
  uint64_t serial = 0;
  {
    std::scoped_lock lock(bittensorLock_);
    serial = ++bittensorSerial_;
    bittensorSession_ = session;
    bittensorDone_ = std::move(done);
  }
  auto args = session->challengeArgs(expectedAddress);
  if (!args) {
    FinishBittensorProof(serial, {false, {}, Narrow(Localized("wallet_connect_failed"))});
    return;
  }
  api_->authWalletChallenge(*args, [this, flow, serial, session, walletId, purpose,
                                    expectedAddress, manualHandler](
                                       std::optional<urnet::AuthWalletChallengeResult> result,
                                       std::optional<std::string> err) {
    // A newer flow took over while the challenge was on its way: it has
    // answered this proof, and no tab or form may open for it now.
    if (!walletFlows_.IsCurrent(flow)) {
      LogWarn("sdkhost: a superseded Bittensor proof's challenge arrived, dropping it");
      return;
    }
    if (err || !result) {
      FinishBittensorProof(serial, {false, {}, err ? *err : std::string("wallet challenge returned no result")});
      return;
    }
    if (result->error && !result->error->message.empty()) {
      FinishBittensorProof(serial, {false, {}, result->error->message});
      return;
    }
    try {
      session->setChallenge(result, BittensorNowMillis());
    } catch (std::exception const& e) {
      LogError("sdkhost: the Bittensor challenge was refused: {}", e.what());
      FinishBittensorProof(serial, {false, {}, Narrow(Localized("wallet_connect_failed"))});
      return;
    }
    switch (bittensor::NextStepFor(session->transport())) {
      case bittensor::NextStep::OpenBrowser: {
        std::string url;
        try {
          url = session->bridgeUrl();
        } catch (std::exception const& e) {
          LogError("sdkhost: no Bittensor bridge url: {}", e.what());
          FinishBittensorProof(serial, {false, {}, Narrow(Localized("wallet_connect_failed"))});
          return;
        }
        wallet_.OpenBittensorBridge(url);
        return;
      }
      case bittensor::NextStep::ManualEntry: {
        // the add sheet's own form, else the window's
        std::function<void(BittensorManualRequest)> handler = manualHandler;
        if (!handler) {
          std::scoped_lock lock(bittensorLock_);
          handler = bittensorManualHandler_;
        }
        if (!handler) {
          FinishBittensorProof(serial, {false, {}, Narrow(Localized("wallet_connect_failed"))});
          return;
        }
        handler({walletId, urnet::bittensorWalletDisplayName(walletId), session->message(),
                 expectedAddress, purpose});
        return;
      }
      case bittensor::NextStep::Unsupported:
        break;
    }
    FinishBittensorProof(serial, {false, {}, Narrow(Localized("wallet_connect_failed"))});
  });
}

void SdkHost::HandleBittensorReturn(const std::string& url) {
  std::shared_ptr<urnet::BittensorWalletSession> session;
  uint64_t serial = 0;
  {
    std::scoped_lock lock(bittensorLock_);
    session = bittensorSession_;
    serial = bittensorSerial_;
  }
  if (!session) {
    LogWarn("sdkhost: a Bittensor hand-back arrived with no proof in flight, ignoring it");
    return;
  }
  auto result = session->handleBridgeReturn(url, BittensorNowMillis());
  if (!result) {
    FinishBittensorProof(serial, {false, {}, Narrow(Localized("wallet_connect_failed"))});
    return;
  }
  if (result->Proof && result->ErrorCode.empty()) {
    FinishBittensorProof(serial, {true, *result->Proof, std::string()});
    return;
  }
  // another flow's tab, the bridge page's second "Return to URnetwork", a replay
  if (bittensor::IsForeignReturn(result->ErrorCode)) {
    LogWarn("sdkhost: ignoring a Bittensor hand-back that is not this proof's ({})",
            result->ErrorCode);
    return;
  }
  FinishBittensorProof(serial, {false, {}, BittensorErrorText(result->ErrorCode, result->ErrorMessage,
                                                             result->BridgeErrorCode,
                                                             session->walletId())});
}

void SdkHost::FinishBittensorProof(uint64_t serial, BittensorProofOutcome outcome) {
  std::function<void(BittensorProofOutcome)> done;
  {
    std::scoped_lock lock(bittensorLock_);
    if (serial != bittensorSerial_) return;
    done = std::exchange(bittensorDone_, nullptr);
    if (!outcome.ok && bittensorSession_) bittensorSession_->cancel();
    bittensorSession_.reset();
  }
  if (done) done(std::move(outcome));
}

void SdkHost::SetBittensorManualHandler(std::function<void(BittensorManualRequest)> handler) {
  std::scoped_lock lock(bittensorLock_);
  bittensorManualHandler_ = std::move(handler);
}

SdkHost::BittensorManualAnswer SdkHost::SubmitBittensorManual(const std::string& address,
                                                              const std::string& signature) {
  std::shared_ptr<urnet::BittensorWalletSession> session;
  uint64_t serial = 0;
  {
    std::scoped_lock lock(bittensorLock_);
    session = bittensorSession_;
    serial = bittensorSerial_;
  }
  BittensorManualAnswer answer;
  if (!session) {
    // superseded or abandoned meanwhile: the flow was answered already
    answer.closed = true;
    return answer;
  }
  auto result = session->handleSignature(address, signature, BittensorNowMillis());
  if (result && result->Proof && result->ErrorCode.empty()) {
    answer.accepted = true;
    answer.closed = true;
    FinishBittensorProof(serial, {true, *result->Proof, std::string()});
    return answer;
  }
  const std::string code = result ? result->ErrorCode : std::string();
  answer.error = BittensorErrorText(code, result ? result->ErrorMessage : std::string());
  if (bittensor::IsCorrectable(code)) return answer;  // the form stays open
  answer.closed = true;
  FinishBittensorProof(serial, {false, {}, answer.error});
  return answer;
}

void SdkHost::CancelBittensorProof() {
  uint64_t serial = 0;
  {
    std::scoped_lock lock(bittensorLock_);
    serial = bittensorSerial_;
  }
  // the user's own choice, answered like a superseded flow: no error shown
  FinishBittensorProof(serial, {false, {}, std::string(bridge::kSupersededPrefix) + "the user closing the wallet form"});
}

void SdkHost::RequestWalletChallenge(
    const std::string& blockchain, const std::string& walletAddress,
    std::function<void(std::optional<std::string> message, std::string error)> done) {
  urnet::AuthWalletChallengeArgs args;
  args.blockchain = blockchain;
  if (!walletAddress.empty()) args.wallet_address = walletAddress;
  api_->authWalletChallenge(args, [done = std::move(done)](
                                      std::optional<urnet::AuthWalletChallengeResult> result,
                                      std::optional<std::string> err) mutable {
    if (err) {
      done(std::nullopt, *err);
      return;
    }
    if (!result) {
      done(std::nullopt, "wallet challenge returned no result");
      return;
    }
    if (result->error && !result->error->message.empty()) {
      done(std::nullopt, result->error->message);
      return;
    }
    if (!result->message_template || result->message_template->empty()) {
      done(std::nullopt, "wallet challenge returned no message");
      return;
    }
    done(*result->message_template, std::string());
  });
}

void SdkHost::SignWithSolanaWallet(
    WalletConnect::Provider provider, const std::string& message,
    std::function<void(bool, std::string, std::string, std::string)> done) {
  // No SetAuthState here on purpose: this is not a sign-in and the session must
  // not move (see on_error above).
  CancelPendingWalletFlows("superseded by a wallet signature request");
  walletSignMessage_ = message;
  walletSignDone_ = std::move(done);
  wallet_.Connect(provider);  // continues on the deep-link callback
}

void SdkHost::ConnectSolanaWallet(
    WalletConnect::Provider provider,
    std::function<void(bool, std::string, std::string)> done) {
  // Not a sign-in: the auth state does not move (see on_error above).
  CancelPendingWalletFlows("superseded by a wallet connect request");
  walletConnectDone_ = std::move(done);
  // continues on the deep-link callback (on_public_key). The payout sheet also
  // takes a typed address, so a missing extension points at it.
  wallet_.Connect(provider, /*offersManualEntry=*/true);
}

void SdkHost::SignWithBittensorWallet(
    const std::string& walletId, const std::string& walletAddress, const std::string& purpose,
    std::function<void(bool, std::string, std::string, std::string, std::string)> done) {
  // Not a sign-in: the auth state does not move (see on_error above).
  const uint64_t flow = CancelPendingWalletFlows("superseded by a wallet signature request");
  BeginBittensorProof(flow, walletId, purpose, walletAddress,
                      [done = std::move(done)](BittensorProofOutcome outcome) {
                        if (!outcome.ok) {
                          done(false, std::string(), std::string(), std::string(),
                               std::move(outcome.error));
                          return;
                        }
                        done(true, outcome.proof.Address, outcome.proof.Signature,
                             outcome.proof.Message, std::string());
                      });
}

void SdkHost::HandleDeepLink(const std::string& url) {
  // the campaign emails' buttons: urnetwork://onboarding/<step>
  if (url.rfind("urnetwork://onboarding/", 0) == 0) {
    if (onOnboardingLink_) onOnboardingLink_(url);
    return;
  }
  // Every browser round trip answers here: the wallet bridge hosts and the
  // urnetwork://oauth/<provider> return of the api's Google / Apple callbacks
  // (on_sso below).
  wallet_.HandleDeepLink(url);
}

void SdkHost::SetProductUpdatesOptOut(bool optOut) {
  productUpdatesOptOut_ = optOut;
  if (optOut && events_) events_->SignupOptoutChanged(false);
}

void SdkHost::ApplySignupPreferences(urnet::NetworkCreateArgs& args) const {
  if (productUpdatesOptOut_) args.product_updates = false;
}

void SdkHost::AuthNetworkClientWithLocale(const urnet::AuthNetworkClientArgs& args,
                                          urnet::AuthNetworkClientCallback callback) {
  nlohmann::json json = args;
  json["time_zone"] = LocalTimeZoneId();
  json["locale"] = ClientEventLocale();
  const std::string body = json.dump();
  auto* fn = new urnet::AuthNetworkClientCallback(std::move(callback));
  urnet_api_auth_network_client(api_->handle(), body.c_str(),
                                &urnet::detail::oneshot_auth_network_client, fn);
}

// ---- Sign in with Google / Apple (the provider's web flow, the api's callback) ---


bool SdkHost::HasPendingAuthJwt() {
  std::scoped_lock lock(mutex_);
  return pendingAuthJwt_.has_value();
}

void SdkHost::SignInWithSso(const std::string& provider, std::function<void(AuthResult)> done) {
  if (provider != "google" && provider != "apple") {
    if (done) done({false, false, "unknown sign-in provider"});
    return;
  }
  SetAuthState(AuthState::Authenticating);
  {
    std::scoped_lock lock(mutex_);
    // a fresh sign-in supersedes any retained token or wallet auth
    pendingAuthJwt_.reset();
    pendingAuthJwtType_.clear();
    pendingWalletAuth_.reset();
  }
  // the browser round trip has ONE pair of callbacks: whatever was waiting is TOLD
  CancelPendingWalletFlows("superseded by a sign-in");
  walletAuthDone_ = std::move(done);
  OpenSsoAttempt(provider, add_sign_in::SsoPurpose::SignIn);
}

void SdkHost::OpenSsoAttempt(const std::string& provider, add_sign_in::SsoPurpose purpose) {
  // Fresh per attempt: `state` is echoed by the provider and `nonce` rides
  // inside the identity token it issues, so a stale or replayed callback can
  // match neither. Both come from the SDK's random source, like a wallet nonce.
  // Both providers run their own web flow: the state carries the platform
  // claim the api's callback reads to redirect back to this app
  // (urnetwork://oauth/<provider>).
  const std::string state = WalletConnect::OAuthState(urnet::generateNonce());
  ssoAttempt_ = SsoAttempt{provider, state, urnet::generateNonce(), purpose};
  std::string apiUrl;
  {
    std::scoped_lock lock(mutex_);
    if (networkSpace_) apiUrl = networkSpace_->getApiUrl();
  }
  // opens the browser; the rest continues on the deep-link callback (on_sso)
  // both callers admit only these two providers: no other flow exists
  if (provider == "apple") {
    wallet_.OpenAppleOAuth(apiUrl, ssoAttempt_->state, ssoAttempt_->nonce);
  } else if (provider == "google") {
    wallet_.OpenGoogleOAuth(apiUrl, ssoAttempt_->state, ssoAttempt_->nonce);
  }
}

// ---- adding a sign-in method (AddSignIn.h) ---------------------------------
// None of these touch the auth state, the pending sign-in auth or the jwt:
// the add sheet posts what they return to addAuth on the current network.

void SdkHost::SsoTokenForAdd(const std::string& provider,
                             std::function<void(std::string, std::string)> done) {
  if (provider != "google" && provider != "apple") {
    if (done) done(std::string(), "unknown sign-in provider");
    return;
  }
  // the browser round trip has ONE pair of callbacks: whatever was waiting is TOLD
  CancelPendingWalletFlows("superseded by adding a sign-in method");
  ssoAddDone_ = std::move(done);
  OpenSsoAttempt(provider, add_sign_in::SsoPurpose::Add);
}

void SdkHost::SignSolanaForAdd(
    WalletConnect::Provider provider,
    std::function<void(std::string, std::string, std::string, std::string)> done) {
  const uint64_t flow = CancelPendingWalletFlows("superseded by adding a sign-in method");
  // the server's add-auth accepts only a message it issued, so always a fresh one
  RequestWalletChallenge(urnet::SOL, std::string(), [this, flow, provider, done](
                                                       std::optional<std::string> message,
                                                       std::string error) {
    if (!walletFlows_.IsCurrent(flow)) {
      done(std::string(), std::string(), std::string(),
           std::string(bridge::kSupersededPrefix) + "another wallet flow");
      return;
    }
    if (!message) {
      done(std::string(), std::string(), std::string(),
           error.empty() ? std::string("could not fetch wallet challenge") : error);
      return;
    }
    SignWithSolanaWallet(provider, *message,
                         [done, message = *message](bool ok, std::string address,
                                                    std::string signature, std::string signError) {
                           if (!ok) {
                             done(std::string(), std::string(), std::string(), std::move(signError));
                             return;
                           }
                           done(std::move(address), std::move(signature), message, std::string());
                         });
  });
}

void SdkHost::SignBittensorForAdd(
    const std::string& walletId, std::function<void(BittensorManualRequest)> manualHandler,
    std::function<void(std::string, std::string, std::string, std::string)> done) {
  const uint64_t flow = CancelPendingWalletFlows("superseded by adding a sign-in method");
  BeginBittensorProof(
      flow, walletId, std::string(bittensor::kPurposeAdd), std::string(),
      [done = std::move(done)](BittensorProofOutcome outcome) {
        if (!outcome.ok) {
          done(std::string(), std::string(), std::string(), std::move(outcome.error));
          return;
        }
        done(outcome.proof.Address, outcome.proof.Signature, outcome.proof.Message,
             std::string());
      },
      std::move(manualHandler));
}

void SdkHost::CancelAddSignIn() {
  CancelPendingWalletFlows("superseded by the add sheet closing");
}

void SdkHost::AuthLoginWithSso(const std::string& provider, const std::string& idToken,
                               std::function<void(AuthResult)> done) {
  urnet::AuthLoginArgs args;
  args.auth_jwt_type = provider;
  args.auth_jwt = idToken;
  // UNDER mutex_: the browser answers minutes after the pill was pressed, and
  // in between the user is free to open Change Network API, whose
  // ApplyNetworkServer reassigns api_ under this same lock and RELEASES the
  // handle this line is about to call through. urnet::Api is move-only, so
  // holding the lock across the dispatch is what there is. authLogin queues its
  // callback onto an SDK thread rather than running it inline, so this does
  // not re-enter.
  std::scoped_lock lock(mutex_);
  if (!api_) {
    AuthResult r{false, false, "the network session went away during sign-in"};
    SetAuthState(AuthState::Error, r.error);
    if (done) done(r);
    return;
  }
  // The identity token is a bearer credential; nothing below logs the args.
  api_->authLogin(args, [this, provider, idToken, done](std::optional<urnet::AuthLoginResult> result,
                                                        std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::LoggedOut);
      if (done) done(r);
      return;
    }
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, done ? done : [](AuthResult) {});
      return;
    }
    // Authenticated, but this identity has no network yet: retain the token
    // (and which provider issued it) and let the UI route to the
    // create-network step (name + terms, no password), the same shape the
    // wallet path uses.
    {
      std::scoped_lock lock(mutex_);
      pendingAuthJwt_ = idToken;
      pendingAuthJwtType_ = provider;
    }
    AuthResult r;
    r.auth_needs_network = true;
    SetAuthState(AuthState::LoggedOut);
    if (done) done(r);
  });
}

void SdkHost::AuthLoginWithWallet(const std::string& address, const std::string& signature,
                                  const std::string& message,
                                  WalletConnect::Provider provider,
                                  const std::string& bittensorWalletId) {
  urnet::WalletAuthArgs w;
  w.wallet_address = address;
  w.wallet_signature = signature;
  w.wallet_message = message;
  // TAO is sr25519 over an ss58 address; SOL is ed25519 over a base58 pubkey.
  w.blockchain = provider == WalletConnect::Provider::Bittensor ? urnet::TAO : urnet::SOL;
  urnet::AuthLoginArgs args;
  args.wallet_auth = w;
  // a signature from another account than the address comes back as
  // result.error.code (a 401 error otherwise)
  args.result_errors = true;
  api_->authLogin(args, [this, w, bittensorWalletId](std::optional<urnet::AuthLoginResult> result,
                                                     std::optional<std::string> err) {
    auto done = walletAuthDone_;
    walletAuthDone_ = nullptr;
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false,
                   WalletProofRefusalText(result->error->code.value_or(std::string()),
                                          result->error->message, bittensorWalletId)};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->network && !result->network->by_jwt.empty()) {
      RegisterNetworkClient(result->network->by_jwt, done ? done : [](AuthResult) {});
      return;
    }
    // The wallet authenticated but isn't linked to a network yet: retain the
    // signed wallet auth and let the UI route to the create-network step, which
    // calls CreateNetwork{useWalletAuth} (name + terms, no password).
    {
      std::scoped_lock lock(mutex_);
      pendingWalletAuth_ = w;
    }
    AuthResult r;
    r.wallet_needs_network = true;
    SetAuthState(AuthState::LoggedOut);
    if (done) done(r);
  });
}

// Invert a BlockActionOverride list into the driver's {paths, allowlist}, the same
// way the SDK's getLocalOverrideAppIds does: RouteOverride.Local=true => bypass,
// false => through-tunnel. Android's "inclusions take precedence": if any app is
// through-tunnel, use ALLOWLIST (keep only those on the tunnel); else DENYLIST
// (bypass those). App rules only (AppIds present); host rules are ignored here.
static void ComputeAppSplit(const urnet::BlockActionOverrideList& overrides,
                            std::vector<std::string>& paths, bool& allowlist) {
  std::vector<std::string> bypass, tunnel;
  for (const auto& over : overrides) {
    if (!over.AppIds || over.AppIds->empty()) continue;
    std::vector<std::string>& dst =
        (over.RouteOverride && over.RouteOverride->Local) ? bypass : tunnel;
    for (const auto& id : *over.AppIds) dst.push_back(id);
  }
  if (!tunnel.empty()) { paths = tunnel; allowlist = true; }
  else { paths = bypass; allowlist = false; }
}

// Upsert / remove one app rule in a BlockActionOverride list (app rules are keyed
// by the exe image path in AppIds; host rules with Hosts are left untouched).
// Shared by the localState_ (offline) and device_ (live) writes.
static void UrstUpsertAppRule(urnet::BlockActionOverrideList& list,
                              const std::string& imagePath, bool includeInTunnel) {
  for (auto& over : list) {
    if (over.AppIds && !over.AppIds->empty() && over.AppIds->front() == imagePath) {
      urnet::RouteOverride route;
      route.Local = !includeInTunnel;
      over.RouteOverride = route;
      return;
    }
  }
  urnet::BlockActionOverride over;
  over.OverrideId = urnet::newId();
  over.AppIds = urnet::StringList{imagePath};
  urnet::RouteOverride route;
  route.Local = !includeInTunnel;
  over.RouteOverride = route;
  list.push_back(std::move(over));
}

static void UrstRemoveAppRule(urnet::BlockActionOverrideList& list,
                              const std::string& imagePath) {
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const urnet::BlockActionOverride& over) {
                              return over.AppIds && !over.AppIds->empty() &&
                                     over.AppIds->front() == imagePath;
                            }),
             list.end());
}

// "The device says it is on a location" -> the status the UI should see.
//
// In an rpc-only session there is no tunnel, so being on a location does NOT
// mean connected: the DeviceLocal will happily negotiate with providers, but no
// route exists and no packet is carried. Reporting RpcOnly rather than Up is
// what keeps every `state == TunnelState::Up` test in the UI reading false,
// which is the whole reason RpcOnly is a distinct state and not a flag beside
// Up.
void SdkHost::SetModeNoticeHandler(ModeNoticeHandler h) {
  std::scoped_lock lock(noticeMutex_);
  onModeNotice_ = std::move(h);
}

void SdkHost::SetModeNoticeObserver(ModeNoticeHandler h) {
  std::scoped_lock lock(noticeMutex_);
  onModeNoticeObserver_ = std::move(h);
}

SdkHost::ModeNoticeHandler SdkHost::ModeNoticeHandlerCopy() const {
  std::scoped_lock lock(noticeMutex_);
  return onModeNotice_;
}

SdkHost::ModeNoticeHandler SdkHost::ModeNoticeObserverCopy() const {
  std::scoped_lock lock(noticeMutex_);
  return onModeNoticeObserver_;
}

// Both subscribers, observer first, with the lock released before either runs.
void SdkHost::DeliverModeNotice(const ModeNotice& notice) const {
  if (auto observer = ModeNoticeObserverCopy()) observer(notice);
  if (auto handler = ModeNoticeHandlerCopy()) handler(notice);
}

// ---- Advanced Mode (D5) ----------------------------------------------------
//
// The same shape as the mode notice above, for the same reason: the value
// exists before any view does. See the block on CurrentAdvancedMode in SdkHost.h.

void SdkHost::SetAdvancedModeHandler(AdvancedModeHandler h) {
  std::scoped_lock lock(advancedMutex_);
  onAdvancedMode_ = std::move(h);
}

SdkHost::AdvancedModeHandler SdkHost::AdvancedModeHandlerCopy() const {
  std::scoped_lock lock(advancedMutex_);
  return onAdvancedMode_;
}

void SdkHost::SetAdvancedMode(bool on) {
  // PERSIST FIRST, PUBLISH SECOND — the order PublishSessionFailure uses, and
  // for the same reason. The publish is best-effort; the recorded value is what
  // actually reaches a surface built later, through RefreshAdvancedMode().
  advancedMode_.store(on, std::memory_order_release);
  SaveAppPref("advanced_mode", on);
  LogInfo("sdkhost: advanced mode {}", on ? "on" : "off");
  RefreshAdvancedMode();
}

void SdkHost::RefreshAdvancedMode() {
  // No mutex_ anywhere on this path. It reads one atomic and copies one
  // std::function, and taking mutex_ here would put a UI-thread call behind
  // whatever BootstrapSession is doing.
  auto handler = AdvancedModeHandlerCopy();
  if (!handler) return;
  handler(CurrentAdvancedMode());
}

// The persistent "this app is not carrying traffic" notice.
void SdkHost::PublishModeNotice() {
  // No early return on "no handler": there are TWO subscribers now and the
  // bookkeeping below (clearing sessionFailure_ once a session exists) is state,
  // not presentation — skipping it because nobody happened to be listening left
  // a stale failure standing over a healthy session.
  // No session, nothing to say about one. This gate is load-bearing rather
  // than defensive: the notice derives from sessionMode_, whose default is
  // RpcOnly (the mode that claims less, so a stray read cannot render as
  // connected) â€” so without it an ordinary LOGGED-OUT launch publishes a
  // confident claim that the service is running with --rpc-only.
  // RefreshModeNotice() is public and is exactly what a view calls when it is
  // constructed, which makes that the common path, not an edge case.
  // A bounded secure-control failure can coexist with a live DeviceRemote: the
  // object was constructed, but it never reached an authenticated RPC level.
  // Keep that notice standing across window recreation just like a bootstrap
  // failure; CheckSessionSync clears it when the same generation recovers.
  if (!sessionFailure_.empty()) {
    ModeNotice failed;
    failed.active = true;
    failed.kind = ModeNotice::Kind::SessionFailed;
    failed.message = sessionFailure_;
    DeliverModeNotice(failed);
    return;
  }
  if (!device_) {
    DeliverModeNotice(ModeNotice{});
    return;
  }
  ModeNotice n;
  if (sessionMode_.load() == proto::StartMode::RpcOnly) {
    n.active = true;
    n.kind = ModeNotice::Kind::RpcOnly;
    n.requestedTunnel = requestedMode_ == proto::StartMode::Tunnel;
    n.message =
        n.requestedTunnel
            ? "Developer mode: the service is running with --rpc-only, so this "
              "app asked for a tunnel and did not get one. Nothing is "
              "connected and no traffic is carried."
            : "Developer mode: rpc-only session. Nothing is connected and no "
              "traffic is carried.";
  }
  DeliverModeNotice(n);
}

// "There is no session, and here is why." The user remains SIGNED IN: this is
// a transport/service failure, not an authentication one, and routing them to
// the sign-in screen would destroy a perfectly good session.
void SdkHost::PublishSessionFailure(const std::string& why) {
  // RECORD FIRST, PUBLISH SECOND. The publish is best-effort — on the startup
  // path there is usually no handler yet, because the window is not built until
  // the first tray click — so the record is what actually reaches the user, via
  // RefreshModeNotice() when the view is finally constructed.
  // The reasons are written as clause fragments ("the URnetwork service is not
  // running or cannot be reached"), because until now nothing ever RENDERED
  // one — the notice was dropped before it reached a view. Glued naively they
  // produce "...cannot be reached Nothing is connected.", which is what
  // actually appeared on screen the first time this was made visible. Make the
  // fragment a sentence: capitalise it, and give it a full stop if it has no
  // terminal punctuation of its own.
  if (why.empty()) {
    sessionFailure_ =
        "Could not start a session with the URnetwork service. Nothing is connected.";
  } else {
    std::string sentence = why;
    sentence[0] = static_cast<char>(
        std::toupper(static_cast<unsigned char>(sentence[0])));
    const char last = sentence.back();
    if (last != '.' && last != '!' && last != '?') sentence += '.';
    sessionFailure_ = sentence + " Nothing is connected.";
  }
  ModeNotice n;
  n.active = true;
  n.kind = ModeNotice::Kind::SessionFailed;
  n.message = sessionFailure_;
  DeliverModeNotice(n);
}

void SdkHost::AdoptServiceFacts(const proto::TunnelStatus& st) {
  lastServiceState_.store(st.state);
  lastServiceDnsApplied_.store(st.dns_applied);
  // The third service-owned fact, adopted here for the same reason as the other
  // two: this process cannot observe it, and inferring it from a mode flag is
  // what let the app report a captured machine as disconnected.
  lastServiceRoutesInstalled_.store(st.routes_installed);
  // ...and the provider-only device, which only the service holds: what the
  // provide indicator shows while there is no session (FillProviderOnlyStats).
  const bool providerWasRunning = serviceProviderRunning_.exchange(st.provider_running);
  serviceProviderMode_.store(st.provider_running ? st.provider_mode : 0);
  serviceProviderNetworkKey_.store(st.provider_running && st.provider_network_key);
  serviceProviderKnown_.store(true);
  // Its client count is get_provider_stats' to say again; a provider that
  // started or stopped is that loop's to show or take off now, not next tick.
  if (!st.provider_running) serviceProviderClients_.store(-1);
  if (providerWasRunning != st.provider_running) KickProviderOnlyStats();
  // Bind this process's SDK during bootstrap, before the service installs
  // routes. The connected route flag keeps that binding through activation.
  const bool bindEgress = st.routes_installed ||
                         st.state == proto::TunnelState::Preparing;
  ApplySdkEgressBind(bindEgress ? st.egress_index4 : 0,
                     bindEgress ? st.egress_index6 : 0,
                     bindEgress ? "the service is preparing or carrying traffic"
                                : "the service reports no tunnel session");
  // The outcome of a feedback's log upload rides on the status the service
  // pushes when the upload ends.
  FollowServiceLogUpload(st);
  std::scoped_lock lock(wfpStateMutex_);
  lastServiceWfpState_ = st.wfp_state;
}

void SdkHost::ApplySdkEgressBind(int64_t index4, int64_t index6, const char* why) {
  const int64_t packed = (index4 << 32) | (index6 & 0xFFFFFFFFll);
  {
    std::scoped_lock lock(egressMutex_);
    if (sdkEgressBound_ == packed) return;
    sdkEgressBound_ = packed;
    // Inside the lock, so the last thread to decide is also the last to tell the
    // SDK. See the field comment: the interleaving this prevents leaves the
    // process pinned to a stale interface.
    urnet::setEgressInterfaceIndex(index4, index6);
  }
  if (index4 != 0) {
    LogInfo("sdkhost: [R1] this process's sdk sockets are now pinned to "
            "ifIndex v4={} v6={} ({}). The UI has its OWN sdk instance, so the "
            "service's bind never covered it and its platform traffic was being "
            "carried by the tunnel it reports on. It is only reachable because "
            "the service also permits URnetwork.exe by app id while connected — "
            "without that permit this bind would make the app fail faster, not "
            "work.",
            index4, index6, why);
  } else {
    LogInfo("sdkhost: [R1] this process's sdk egress binding is CLEARED ({}); "
            "sockets follow the route table again, which is correct with no "
            "tunnel in force.",
            why);
  }
}

// ---- the network country (P052) ---------------------------------------------
//
// See the contract in the header and Common/NetworkCountry.h.

namespace {
// How long a launch waits for the first reading. Microseconds on a PC whose
// default route is not a mobile broadband adapter (two IP helper reads), tens
// of milliseconds when it is (a COM call into the WWAN service); this bounds
// the case where that service does not answer.
constexpr std::chrono::milliseconds kNetworkCountryFirstReadWait{1000};
}  // namespace

void SdkHost::StartNetworkCountryWatch() {
  if (networkCountryWatch_) return;
  networkCountryWatch_ = std::make_unique<NetworkCountryWatch>(
      [] { return ReadNetworkCountry(); },
      [this](const netcountry::Reading& reading) { ApplyNetworkCountry(reading); });
  networkCountryChanges_ =
      std::make_unique<DefaultRouteChanges>(networkCountryWatch_->NetworkEventSink());
  if (!networkCountryWatch_->WaitFirstReport(kNetworkCountryFirstReadWait)) {
    LogWarn("sdkhost: the network country was not read within {}ms; the network "
            "spaces are built without it, and it applies in place when the read "
            "lands",
            kNetworkCountryFirstReadWait.count());
  }
}

void SdkHost::ApplyNetworkCountry(const netcountry::Reading& reading) {
  {
    std::scoped_lock lock(networkCountryMutex_);
    networkCountry_ = reading;
  }
  // This process's own dials (sign-in, the api) first: they need no service.
  urnet::setNetworkCountryCode(reading.code);
  LogInfo("sdkhost: network country \"{}\" ({})", reading.code, reading.source);
  PushNetworkCountry("the network country changed");
}

void SdkHost::PushNetworkCountry(const char* why) {
  std::scoped_lock pushLock(networkCountryPushMutex_);
  if (!service_.IsConnected()) return;
  const netcountry::Reading reading = CurrentNetworkCountry();
  proto::SetNetworkCountry country;
  country.network_country_code = reading.code;
  country.network_country_source = reading.source;
  if (!service_.SetNetworkCountry(country)) {
    LogInfo("sdkhost: the service did not take the network country \"{}\" ({}); one "
            "older than set_network_country keeps none",
            reading.code, why);
  }
}

void SdkHost::PushNetworkCountryIfMoved(const netcountry::Reading& sent, const char* why) {
  if (CurrentNetworkCountry() == sent) return;
  LogInfo("sdkhost: the network country moved while a request carried \"{}\" ({}); "
          "pushing the current one",
          sent.code, why);
  PushNetworkCountry(why);
}

netcountry::Reading SdkHost::CurrentNetworkCountry() const {
  std::scoped_lock lock(networkCountryMutex_);
  return networkCountry_;
}

// The control channel dropped and nobody asked it to. Runs on the pipe reader
// thread; both handlers it invokes marshal to the UI thread themselves
// (AppController::OnUi), and neither reconnects — see PipeClient.h.
void SdkHost::OnServiceDisconnected() {
  // WHAT IS TRUE AT THIS INSTANT. The service is what holds the tunnel: the
  // wintun adapter is a software device owned by ITS process, and the WFP
  // policy lives on a session opened with FWPM_SESSION_FLAG_DYNAMIC. If that
  // process exited, both are already gone and the machine is back on its
  // physical adapter in the clear. So the honest reading is "no routes, no DNS
  // applied, no leak guard", and it is also the fail-safe one: it claims less
  // than the truth if the channel dropped for some other reason.
  //
  // WHY IT HAS TO BE PUSHED. Nothing else notices. connectVc_ belongs to a
  // DeviceRemote whose mTLS listener has just gone away and getConnectionStatus()
  // keeps returning the last value it was told, so with no push here the hero
  // stays green, the button still reads Disconnect, the strip still says routes
  // are on, and the last throughput sample stays on screen — for as long as the
  // window is open, because no further push is coming from anywhere.
  lastServiceDnsApplied_.store(false);
  lastServiceRoutesInstalled_.store(false);
  lastServiceState_.store(proto::TunnelState::Stopped);
  // The provider-only device lived in that process too, so nothing provides
  // now. The service-reconnect watchdog's recovery pass re-reads the service
  // (hello) and ends with the provider reconcile, which starts it again on the
  // service that comes back.
  serviceProviderRunning_.store(false);
  serviceProviderMode_.store(0);
  serviceProviderNetworkKey_.store(false);
  serviceProviderClients_.store(-1);
  KickProviderOnlyStats();  // its plots come off now
  // ...and the tun went with it, so nothing must stay pinned to the interface
  // that existed to avoid it. A binding retained across the service's death
  // would outlive the reason for it and pin this process to one NIC for the rest
  // of its life, with nothing left to correct it.
  ApplySdkEgressBind(0, 0, "the service's control channel dropped");
  {
    std::scoped_lock lock(wfpStateMutex_);
    lastServiceWfpState_ = "off";
  }
  // The session went with it. device_ is still constructed on this side, but it
  // points at an mTLS listener inside a process that is gone, so anything that
  // treats "there is a DeviceRemote" as "there is a session" is now wrong — the
  // strip included. The worker checks this same pair (device_ AND a live
  // channel) before it decides whether it has to bootstrap.
  hasSession_.store(false, std::memory_order_release);
  // Struct defaults are exactly the honest reading: Stopped, routes_installed
  // false, dns_applied false, wfp_state "off". The mode is kept so the advanced
  // strip still says which KIND of session this was.
  proto::TunnelStatus st;
  st.mode = sessionMode_.load();
  if (onTunnel_) onTunnel_(st);
  // ...and the connect surface, which does not read TunnelStatus at all. See the
  // control-channel clamp at the end of ReadStats.
  PublishStats();
  // NOTHING USED TO PUT THIS BACK. The app noticed the drop, said so in the log,
  // clamped every surface — and then waited forever. Restarting the service
  // under a running app produced an app that could never see it again, which on
  // this machine is the most common way of getting into "Connect does nothing":
  // the tunnel service is started and stopped by hand between runs.
  ScheduleServiceRetry();
}

proto::TunnelStatus SdkHost::SessionStatus(bool haveLocation) const {
  proto::TunnelStatus st;
  const proto::StartMode mode = sessionMode_.load();
  st.mode = mode;
  // THE LAST VALUE THE SERVICE REPORTED, not an inference. This used to read
  // `mode == Tunnel && service_.IsConnected()` — true for the whole life of a
  // tunnel session by construction, because both halves survive a stop. So
  // after a Disconnect, every status this process synthesised still asserted
  // "routes are installed right now" over a machine whose routes had just been
  // given back (or, worse, still claimed them honestly while the button said
  // Disconnected — which was the bug). It is the service that owns the routes;
  // carry what it said, exactly as dns_applied and wfp_state below already do.
  st.routes_installed = lastServiceRoutesInstalled_.load();
  // These two are the SERVICE's to report — this process cannot observe either
  // — so carry the last value it sent rather than the struct default. Defaulting
  // them here would make every app-synthesised push claim "dns not applied, no
  // leak guard" over a perfectly healthy tunnel.
  st.dns_applied = lastServiceDnsApplied_.load();
  {
    std::scoped_lock lock(wfpStateMutex_);
    st.wfp_state = lastServiceWfpState_;
    // The live session's rpc endpoint. Synthesised statuses used to leave this
    // empty, and they are the ONLY statuses a settled session produces — so the
    // advanced strip's RPC field read "none" over a working tunnel, which is
    // the same class of lie as "Session rpc-only" with no session at all.
    //
    // Under wfpStateMutex_ rather than mutex_ deliberately: this function is
    // called from SDK listener callbacks that do NOT hold mutex_, so the string
    // needs a lock of its own, and this is already the "last known session
    // facts" lock. It is never held together with mutex_ in the other order.
    st.rpc_listen_hostport = sessionRpcHostPort_;
  }
  if (mode == proto::StartMode::Tunnel &&
      lastServiceState_.load() == proto::TunnelState::Preparing) {
    st.state = proto::TunnelState::Preparing;
  } else if (!haveLocation) {
    st.state = proto::TunnelState::Stopped;
  } else {
    st.state = mode == proto::StartMode::RpcOnly ? proto::TunnelState::RpcOnly
                                                : lastServiceState_.load();
  }
  return st;
}

bool SdkHost::BootstrapSession(const char* reason, bool attachOnly) {
  // caller holds mutex_
  // Cleared on entry and set on every failure path, so a caller that gets false
  // can tell the user WHY. Both callers used to report the same hardcoded
  // "failed to start tunnel session", which is actively misleading for a mode
  // mismatch or an out-of-date service.
  bootstrapError_.clear();
  bootstrapServiceRetryable_ = false;
  // ...and the D8 outcome flag with it: a false return with this set is not a
  // failure, it is the click-only policy declining a cold start. The worker
  // reads it to keep the decline off the failure-notice channel.
  bootstrapDeclined_ = false;
  // Signed out: no session, adopted or started, and nothing to report. Read off
  // loggedIn_ rather than the stored jwt, which a sign-out's asynchronous local
  // logout may not have removed yet (Logout).
  if (!loggedIn_.load(std::memory_order_acquire)) {
    bootstrapDeclined_ = true;
    LogInfo("sdkhost: '{}' found the app signed out: no session is adopted or started",
            reason);
    return false;
  }
  const std::string clientJwt = localState_->getByClientJwt();
  if (clientJwt.empty()) {
    bootstrapError_ = "no client credentials are stored for this device";
    return false;
  }
  // The instance id THIS bootstrap will pair its DeviceRemote with.
  //
  // Mutable, and the reattach branch below overwrites it. Current SDK builds
  // preserve LocalState.instance_id across a JWT refresh, but installations
  // affected by the older rotation bug can still have a disk id different from
  // the one the service's running DeviceLocal was born with. Reattaching with
  // that drifted id is what made DeviceLocalRpc.Sync refuse every sync of a
  // reattached session for its whole life (see RpcSessionBlob.h).
  std::string instanceId = localState_->getInstanceId();

  if (!service_.IsConnected() && !service_.Connect()) {
    bootstrapServiceRetryable_ = true;
    bootstrapError_ =
        "the URnetwork service is not running or cannot be reached";
    LogError("sdkhost: service not reachable");
    return false;
  }
  // A sign-out the service has not done yet: what it runs may still be the old
  // account's, and its identity the old account's. Nothing is adopted or
  // started until it is done; this pass tried first, and the watchdog keeps
  // trying (SignOut.h).
  if (signOut_.Owed()) {
    bootstrapServiceRetryable_ = true;
    bootstrapError_ =
        "the URnetwork service has not yet stopped what the previous sign-in ran";
    LogWarn("sdkhost: '{}' adopts and starts no session: a sign-out is still owed to "
            "the service",
            reason);
    return false;
  }

  try {
    std::string clientPem, serverCertPem, hostPort, rpcSessionId;
    RpcSession persistenceRecord;

    proto::TunnelStatus hello = service_.Hello();
    // hello is a full TunnelStatus and it is the ONLY status a reattach ever
    // sees: nothing pushes an event for a session that was already running when
    // this process started. Its dns_applied and wfp_state were read for `state`
    // and `mode` and then dropped, so a reattached tunnel rendered with the
    // struct defaults — "dns not applied", "no leak guard" — over a session that
    // may be perfectly healthy, until some unrelated start/stop happened to
    // correct it.
    AdoptServiceFacts(hello);
    // The service's running devices may hold an older network country than
    // this process reads — a reattach after a relaunch, or a service that
    // restarted — and a hello is the first word with it either way.
    PushNetworkCountry("the service answered hello");

    if (requestedMode_ == proto::StartMode::Tunnel &&
        hello.protocol_version < proto::kFirstDeferredCaptureVersion) {
      bootstrapServiceRetryable_ = true;
      bootstrapError_ = "the running service must be updated before connecting "
                        "(provider readiness requires control protocol v4)";
      LogError("sdkhost: stage=bootstrap protocol={} deferred_capture=unsupported",
               hello.protocol_version);
      return false;
    }

    // Version 3 is the first protocol in which a live status proves which
    // DeviceLocal and which mTLS generation own the listener. Refuse before a
    // start: an older service would discard rpc_session_id and its Start()
    // begins by tearing down any existing tunnel.
    if (hello.protocol_version < proto::kFirstRpcSessionIdentityVersion) {
      bootstrapServiceRetryable_ = true;
      bootstrapError_ = std::format(
          "the running URnetwork service uses control protocol v{}; v{}+ is "
          "required for safe RPC session adoption. Update the service.",
          hello.protocol_version, proto::kFirstRpcSessionIdentityVersion);
      LogError("sdkhost: REFUSING to start or adopt an RPC session: service "
               "protocol v{} cannot report instance_id/rpc_session_id "
               "(requires v{}+)",
               hello.protocol_version, proto::kFirstRpcSessionIdentityVersion);
      return false;
    }
    if (hello.protocol_version > proto::kProtocolVersion) {
      bootstrapServiceRetryable_ = true;
      bootstrapError_ = std::format(
          "the running URnetwork service uses control protocol v{}, but this "
          "app understands through v{}. Update the app or reinstall the "
          "matching app and service together.",
          hello.protocol_version, proto::kProtocolVersion);
      LogError("sdkhost: REFUSING service protocol v{} with app protocol v{}; "
               "a partial upgrade/downgrade cannot be adopted safely",
               hello.protocol_version, proto::kProtocolVersion);
      return false;
    }

    if (requestedMode_ == proto::StartMode::RpcOnly) {
      // A service older than kFirstStartModeVersion has no `mode` handler: it
      // drops the field, runs all eight steps and rewrites this machine's
      // routes and DNS. Refuse BEFORE start_tunnel â€” after it the damage is
      // done and all we could do is revert. This check is the ONLY thing that
      // distinguishes "honours mode" from "ignores mode"; without it the
      // safest-looking configuration in the tree is the one that silently
      // builds a real tunnel.
      if (hello.protocol_version < proto::kFirstStartModeVersion) {
        LogError("sdkhost: REFUSING to start a session. URNETWORK_RPC_ONLY is "
                 "set, but the running service speaks control protocol v{} and "
                 "only v{}+ understands the start mode â€” it would ignore the "
                 "field, build a REAL TUNNEL and rewrite this machine's routes "
                 "and dns. Update the installed service, or run `urnetworkd "
                 "console --rpc-only` from this build.",
                 hello.protocol_version, proto::kFirstStartModeVersion);
        bootstrapError_ = std::format(
            "URNETWORK_RPC_ONLY is set, but the running service is too old to "
            "honour it (control protocol v{}, needs v{}+). It would build a "
            "real tunnel. Update the service, or run `urnetworkd console "
            "--rpc-only` from this build.",
            hello.protocol_version, proto::kFirstStartModeVersion);
        return false;
      }
      // A live TUNNEL when we asked for rpc-only. Attaching would be honestly
      // REPORTED, but it also hands this process the authority to tear that
      // tunnel down: TeardownSessionLocked -> StopTunnel -> NetworkConfig::
      // Revert, reachable from Logout and from re-registration. Somebody who
      // set the env var to guarantee "this run cannot touch my network" must
      // not find that Log out reverted the tunnel they were using.
      if (proto::IsSessionLive(hello.state) &&
          hello.mode == proto::StartMode::Tunnel) {
        LogError("sdkhost: REFUSING to attach. URNETWORK_RPC_ONLY is set, but "
                 "the service is running a REAL TUNNEL (state={} "
                 "routes_installed={}). This process would be able to stop it â€” "
                 "a log out or a re-registration reverts its routes. Stop the "
                 "tunnel first, or unset URNETWORK_RPC_ONLY.",
                 proto::ToString(hello.state),
                 hello.routes_installed ? "yes" : "no");
        bootstrapError_ =
            "URNETWORK_RPC_ONLY is set, but the service is running a real "
            "tunnel. This app could stop it, so it will not attach. Stop the "
            "tunnel first, or unset URNETWORK_RPC_ONLY.";
        return false;
      }
    }

    // Reattach only when the live session's mode is EXACTLY the one we asked
    // for. "At least as capable" was wrong in the rpc-only direction: a tunnel
    // does carry rpc-only traffic, but attaching to it also confers the power
    // to revert it â€” refused above.
    RpcSessionLoad loaded = LoadRpcSession();
    auto& saved = loaded.record;
    LogInfo("sdkhost: rpc session storage is {}", loaded.diagnostic);
    if (loaded.legacyPlaintext) {
      // Old builds persisted the client private key in cleartext. It lacks the
      // generation required for adoption anyway; remove it now rather than
      // leave credential material exposed until the next successful start.
      ClearRpcSession();
      LogWarn("sdkhost: removed legacy plaintext rpc session credentials; a "
              "future explicit Connect will create a protected v1 record");
    }
    const bool liveIsSufficient =
        proto::IsSessionLive(hello.state) && hello.mode == requestedMode_;
    const bool reattaching =
        liveIsSufficient && saved &&
        rpcsession::MatchesLiveSession(*saved, hello.instance_id,
                                       hello.rpc_session_id,
                                       hello.rpc_listen_hostport);
    if (liveIsSufficient && !reattaching) {
      LogWarn("sdkhost: refusing RPC adoption: the live service identity does "
              "not exactly match an adoptable saved record (storage={}, "
              "status_instance={}, status_session={}, status_rpc={})",
              loaded.diagnostic, hello.instance_id.empty() ? "missing" : "set",
              hello.rpc_session_id.empty() ? "missing" : "set",
              hello.rpc_listen_hostport);
    }
    // D8, owner decision: THE TUNNEL STARTS ONLY ON AN EXPLICIT CONNECT
    // GESTURE. An EnsureSession — the resume path at launch, a network-server
    // change, the service-reconnect watchdog — may ADOPT a session that is
    // already running (a reattach changes nothing on the machine), and may do
    // nothing else. The cold start below this gate installs routes, DNS and a
    // firewall policy on a machine nobody touched; the forensics have three
    // app launches and a space switch each doing exactly that with no gesture
    // anywhere in the log. Everything above this line was read-only (hello,
    // the saved blob); declining here leaves the service exactly as found.
    if (attachOnly && !reattaching) {
      bootstrapDeclined_ = true;
      LogInfo("sdkhost: '{}' found no live {} session to reattach to "
              "(service state={}) — NOT starting one. The tunnel starts only "
              "on a Connect gesture.",
              reason, proto::ToString(requestedMode_),
              proto::ToString(hello.state));
      return false;
    }
    if (reattaching) {
      clientPem = saved->client_pem;
      serverCertPem = saved->server_cert_pem;
      hostPort = saved->host_port;
      rpcSessionId = saved->rpc_session_id;
      persistenceRecord = *saved;
      // THE FIX. Pair with the id the session was STARTED with, which the blob
      // carries, not the id sitting on disk now — those differ after any JWT
      // refresh, and DeviceLocalRpc.Sync refuses a nonzero id that is not its
      // own, forever, with no retry that can ever succeed.
      instanceId = saved->instance_id;
      sessionMode_.store(hello.mode);
      LogInfo("sdkhost: reattaching to live {} session at {} (routes_installed={} "
              "instance={})",
              proto::ToString(hello.mode), hostPort,
              hello.routes_installed ? "yes" : "no", instanceId);
    } else {
      // THE ONE START SITE. Every path that can create a service session funnels
      // through this branch, so this line is the complete audit trail of "who
      // started a tunnel and why" — the space-switch start the forensics could
      // not attribute (start #5, 09:57:18, no gesture, no reason line) predates
      // it. Grep for "starting a session (" and every start has a reason.
      LogInfo("sdkhost: starting a session ({})", reason);
      // fresh session: generate per-session RPC key material
      urnet::DeviceRpcKeyMaterial km = urnet::generateDeviceRpcKeyMaterial();
      hostPort = RandomLoopbackHostPort();
      rpcSessionId = urnet::generateNonce();
      clientPem = km.getClientPem();
      serverCertPem = km.getServerCertPem();

      proto::StartTunnel cfg;
      cfg.by_jwt = clientJwt;
      cfg.network_space_json = networkSpace_->toJson();
      cfg.instance_id = instanceId;
      cfg.device_description = DeviceDescription();
      cfg.device_spec = DeviceSpec();
      cfg.app_version = appVersion_;
      cfg.rpc_server_pem = km.getServerPem();
      cfg.rpc_client_cert_pem = km.getClientCertPem();
      cfg.rpc_listen_hostport = hostPort;
      cfg.rpc_session_id = rpcSessionId;
      cfg.mode = requestedMode_;
      // The kill switch now drives a WFP policy in the SERVICE, not just the
      // SDK's routeLocal flag, so the service has to know the persisted
      // preference before it installs the first route. CurrentKillSwitch()
      // reads LocalState when there is no device yet, which is exactly the
      // state we are in here.
      cfg.kill_switch = CurrentKillSwitch();
      // The network country, which the service applies before it builds the
      // device.
      const netcountry::Reading networkCountry = CurrentNetworkCountry();
      cfg.network_country_code = networkCountry.code;
      cfg.network_country_source = networkCountry.source;
      // The user's system proxy, by kind only, for the service's line in the
      // log feedback uploads (SystemProxy.h): this process is the one that
      // can read the user's setting.
      cfg.system_proxy = ReadUserProxyKind();
      // Seed split tunneling from the persisted per-app overrides so the driver is
      // correct at tunnel-up (device_ isn't connected yet - read the app LocalState).
      // PushLocalOverrideAppsToDriver re-applies it live once the device is up.
      if (localState_) {
        static std::atomic<bool> logged{false};
        if (auto ov = ReadSdkList(logged, "getBlockActionOverrides (local seed)",
                               [&] { return localState_->getBlockActionOverrides(); }))
          ComputeAppSplit(*ov, cfg.excluded_app_paths, cfg.allowlist_mode);
      }

      // Pending is durable BEFORE the service starts. If this process dies
      // after the service binds RPC but before DeviceRemote syncs, the next
      // process still has the one set of credentials that can adopt it.
      persistenceRecord = {.version = rpcsession::kCurrentVersion,
                           .state = rpcsession::State::Pending,
                           .client_pem = clientPem,
                           .server_cert_pem = serverCertPem,
                           .host_port = hostPort,
                           .instance_id = instanceId,
                           .rpc_session_id = rpcSessionId};
      if (!SaveRpcSession(persistenceRecord)) {
        bootstrapError_ =
            "could not securely persist the RPC credentials before tunnel start";
        LogError("sdkhost: refusing to start: DPAPI/atomic persistence of the "
                 "pending rpc session failed");
        return false;
      }

      proto::TunnelStatus st = service_.StartTunnel(cfg);
      // The start reply is a full status and it is the FIRST one that can carry
      // a live egress index — the pushed event may or may not beat us here, and
      // waiting for it would leave the app's sockets in the tun for the gap.
      // Adopting it is idempotent with whatever arrives next.
      AdoptServiceFacts(st);
      // Whatever the outcome, the service may have applied the request's
      // network country (it does so partway through the start), and a newer
      // one the watch pushed meanwhile may have reached it first.
      PushNetworkCountryIfMoved(networkCountry, "start_tunnel");
      // Live, not "up": an rpc-only session reports state rpc_only and that is
      // success for this call. What the app must never do is treat it as a
      // tunnel, which is why sessionMode_ is taken from the SERVICE's answer
      // and not from what we asked for â€” the service can be clamped to
      // rpc-only, in which case the two differ.
      if (!proto::IsSessionLive(st.state)) {
        bootstrapError_ = st.error.empty()
                              ? std::string("the service could not start a ") +
                                    proto::ToString(cfg.mode) + " session"
                              : st.error;
        LogError("sdkhost: service failed to start a {} session: {}",
                 proto::ToString(cfg.mode), st.error);
        return false;
      }
      if (st.instance_id != instanceId || st.rpc_session_id != rpcSessionId ||
          st.rpc_listen_hostport != hostPort) {
        bootstrapError_ =
            "the service returned a different RPC session identity; the new "
            "session was stopped rather than paired with unknown credentials";
        LogError("sdkhost: REFUSING fresh RPC session: service identity did not "
                 "echo the requested instance/session/endpoint exactly");
        service_.StopTunnel();
        ClearRpcSession();
        return false;
      }
      // The mode we GOT, stored only once the mismatch below has been resolved.
      // Storing it above the check left the refusal branch with sessionMode_ ==
      // Tunnel and no session, which is exactly the state SessionStatus() reads
      // to report routes_installed = true.
      if (st.mode != cfg.mode) {
        if (cfg.mode == proto::StartMode::RpcOnly) {
          // The dangerous direction. We asked for no network changes and the
          // service built a tunnel: routes and DNS have ALREADY been rewritten.
          // The version gate above should make this unreachable, so reaching it
          // means a peer is misreporting its version â€” give the routes back
          // rather than keep a tunnel nobody asked for. This one stays a
          // refusal: adopting it would mean keeping a tunnel that the user
          // explicitly asked not to have.
          bootstrapError_ =
              "the service built a real tunnel for a request that asked for "
              "rpc-only; it has been stopped. Update the service.";
          LogError("sdkhost: the service returned a TUNNEL for an rpc-only "
                   "request (routes_installed={}). This machine's routes and "
                   "dns have already been rewritten by a request that asked for "
                   "the opposite. Stopping it and refusing the session.",
                   st.routes_installed ? "yes" : "no");
          service_.StopTunnel();
          sessionMode_.store(proto::StartMode::RpcOnly);  // no session; claim less
          return false;
        }
        // The safe direction: we asked for a tunnel and the service says it
        // served rpc-only â€” but VERIFY that rather than assume it. `mode` is
        // the peer's label; `routes_installed` is the field Protocol.h
        // designates as the one to trust for "was this machine's network
        // touched". They can disagree: an unrecognised mode string on the wire
        // degrades to RpcOnly, which was fail-safe while a mismatch meant
        // refusal and is NOT fail-safe under adopt. Refusing to check here
        // while checking in the branch above would apply "the peer may be
        if (st.routes_installed) {
          bootstrapError_ =
              "the service reported an rpc-only session but also reported that "
              "it installed routes; it has been stopped.";
          LogError("sdkhost: REFUSING to adopt. The service reports "
                   "mode=rpc_only but routes_installed=yes â€” those cannot both "
                   "be true, and an rpc-only session is defined by having "
                   "written nothing. Stopping it rather than adopting a session "
                   "that may be carrying traffic.");
          service_.StopTunnel();
          sessionMode_.store(proto::StartMode::RpcOnly);  // no session; claim less
          return false;
        }
        // Nothing was written to this machine.
        //
        // ADOPT it rather than refuse. Refusing was the wrong trade: the spec
        // defines the whole Class-B workflow as "needs the service in
        // --rpc-only mode", so a UI agent who starts that console and launches
        // the app must get a driveable app, not a dead one â€” and the previous
        // behaviour put the explanation in the SERVICE's help text, where
        // somebody debugging a dead APP has no reason to look.
        //
        // This is not a silent downgrade, which is the thing S3 exists to
        // prevent. The mechanism that makes it loud TODAY is the clamp at the
        // source: every rendered connect value says disconnected (see the end
        // of ReadStats). The persistent notice raised below is the second
        // mechanism and has NO CONSUMER on this branch â€” P2 binds it â€” so as
        // merged an adopted session shows no banner. Do not describe this as
        // two working mechanisms until that binding exists. Adopting also
        // writes nothing and can only happen when somebody deliberately started
        // a console with --rpc-only; the installed service never does.
        LogWarn("sdkhost: asked the service for a {} session and it served {} â€” "
                "the service is clamped (`urnetworkd console --rpc-only`). "
                "ADOPTING the rpc-only session: nothing was written to this "
                "machine and no traffic will be carried. Raising a persistent "
                "in-app notice so this is not a silent downgrade.",
                proto::ToString(cfg.mode), proto::ToString(st.mode));
      }
      sessionMode_.store(st.mode);
      if (st.mode == proto::StartMode::RpcOnly) {
        LogWarn("sdkhost: RPC-ONLY session at {} â€” the DeviceRemote is live and "
                "every screen is driveable, but no routes exist and no traffic "
                "is carried (routes_installed={}).",
                hostPort, st.routes_installed ? "yes" : "no");
      }
    }

    // The controlling DeviceRemote dials the service's mTLS RPC listener.
    device_ = urnet::newDeviceRemoteWithDefaults(*networkSpace_, clientJwt, instanceId);
    ++sessionGeneration_;
    const std::uint64_t generation = sessionGeneration_;
    activeRpcPersistenceGeneration_.store(generation, std::memory_order_release);
    confirmedRpcPersistenceGeneration_.store(
        persistenceRecord.state == rpcsession::State::Confirmed ? generation : 0,
        std::memory_order_release);

    // Register BEFORE setRpcServer: a loopback sync may complete immediately,
    // and a transition-only listener installed afterwards would miss the only
    // event that confirms the pending credential record.
    auto remoteChanged = [this, generation,
                          record = persistenceRecord](bool remoteConnected) mutable {
      if (remoteConnected) {
        std::scoped_lock persistenceLock(rpcPersistenceMutex_);
        if (activeRpcPersistenceGeneration_.load(std::memory_order_acquire) ==
                generation &&
            confirmedRpcPersistenceGeneration_.load(std::memory_order_acquire) !=
                generation) {
          record.state = rpcsession::State::Confirmed;
          if (SaveRpcSession(record)) {
            confirmedRpcPersistenceGeneration_.store(generation,
                                                      std::memory_order_release);
            LogInfo("sdkhost: rpc session credentials confirmed after authenticated "
                    "DeviceRemote sync");
          } else {
            LogWarn("sdkhost: authenticated rpc sync succeeded, but the confirmed "
                    "credential state could not be persisted");
          }
        }
      }
      if (onRemoteChanged_) onRemoteChanged_(remoteConnected);
    };
    subs_.push_back(device_->addRemoteChangeListener(remoteChanged));
    device_->setRpcServer(clientPem, serverCertPem, hostPort);
    // Listener APIs are edge-triggered. Perform the level check as well so an
    // already-connected remote cannot remain pending forever.
    remoteChanged(device_->getRemoteConnected());
    {
      std::scoped_lock lock(wfpStateMutex_);
      sessionRpcHostPort_ = hostPort;
    }
    // There IS a session, from here on. Set before the listeners below, because
    // the first status they push reads it.
    hasSession_.store(true, std::memory_order_release);

    // These session listeners are functional rather than presentational: auth
    // invalidation and tray connection state must keep working while hidden.
    // The device is built on this space's Api, so its logout reports the
    // rejection the Api's listener reports (BindApiLocked), after it, a 401
    // that came back over the rpc included. It reads the device's cause by the
    // device's handle before the hop (AuthLogoutCause.h), and the app signs
    // out once for the two reports (AppController, AuthLogoutNotice.h).
    subs_.push_back(device_->addAuthLogoutListener([this, device = device_->handle()] {
      if (onAuthInvalid_) onAuthInvalid_(authlogout::DeviceCause(device));
    }));
    subs_.push_back(device_->addJwtRefreshListener([this](std::string) {
      if (onJwtRefreshed_) onJwtRefreshed_();
    }));
    subs_.push_back(device_->addConnectLocationChangeListener(
        [this](std::optional<urnet::ConnectLocation> location) {
          if (!onTunnel_) return;
          onTunnel_(SessionStatus(location.has_value()));
        }));
    // Re-apply the persisted performance profile onto the (re)created device
    // (macOS DeviceManager parity: LocalState is the profile's persistence).
    try {
      device_->setPerformanceProfile(localState_->getPerformanceProfile());
    } catch (const std::exception& e) {
      LogWarn("sdkhost: restore performance profile failed: {}", e.what());
    }
    // Seed the provide control mode the same way (macOS parity): the service's
    // DeviceLocal does not restore it from local state itself. There is no
    // provide toggle on windows yet, so this applies the stored default
    // ("never" â€” providing is opt-in) and keeps the device consistent with
    // local state once the toggle lands.
    try {
      device_->setProvideControlMode(localState_->getProvideControlMode());
    } catch (const std::exception& e) {
      LogWarn("sdkhost: restore provide control mode failed: {}", e.what());
    }
    // Seed the transport policies (TRANSPORTSTATS) from the app-side mirror,
    // ONLY when one exists: nullopt means never edited here, and the service's
    // persisted (or default) policy stands. Setting a nullopt would normalize
    // the service's policy back to the default on every bootstrap. The seed
    // queues on the remote and is applied on the next sync, before the
    // destination, then persisted service-side; offline reads answer with it.
    // See ApplyTransportSettings for why the mirror exists.
    try {
      if (auto settings = localState_->getTransportSettings()) {
        device_->setTransportSettings(settings);
      }
      if (auto settings = localState_->getProviderTransportSettings()) {
        device_->setProviderTransportSettings(settings);
      }
    } catch (const std::exception& e) {
      LogWarn("sdkhost: restore transport settings failed: {}", e.what());
    }
    if (presentationActive_) {
      SubscribeStats();
      SubscribeDrawer();
      // The locations/peers feeds are presentation-scoped too, and EnsureLocations
      // is gated on `device_`. A window that was already presenting while the
      // session bootstrapped (the normal case: the user is looking at Network or
      // has the chooser open while the service comes up) had NO device when it
      // last asked, and nothing re-asked afterwards - so the pane stayed empty
      // for the life of the window. Re-arm here, where the device first exists.
      EnsureLocationsLocked();
    } else {
      // nothing presents, so nothing subscribes: cache the device's answer for the
      // seed a window reads when it is built or navigates to Earnings (O8)
      const bool hasProviderStats = DeviceHasProviderStatsLocked();
      std::scoped_lock drawerLock(drawerMutex_);
      lastHasProviderStats_ = hasProviderStats;
    }

    if (onTunnel_) onTunnel_(SessionStatus(device_->getConnectLocation().has_value()));

    // Raise the persistent notice LAST, once the session is actually usable, so
    // it can never appear on a bootstrap that then failed. It is deliberately
    // not dismissible: it describes a property of the whole session, not an
    // event, and it stays true until the app is restarted against a normal
    // service.
    sessionFailure_.clear();  // there is a session; the standing reason is gone
    PublishModeNotice();

    LogInfo("sdkhost: session bootstrapped (mode={} rpc={})",
            proto::ToString(sessionMode_.load()), hostPort);
    // …and then GO AND CHECK, because every line above this one succeeded
    // without the service having answered once. See the rpc-sync watchdog in
    // SdkHost.h: this log used to be the last word on a session that was in
    // fact refused, and the user's only symptom was a screen that said nothing.
    ArmSyncWatchdogLocked(sessionGeneration_, reattaching);
    return true;
  } catch (const std::exception& e) {
    bootstrapError_ = e.what();
    bootstrapServiceRetryable_ = !service_.IsConnected();
    LogError("sdkhost: bootstrap failed: {}", e.what());
    return false;
  }
}

// ---- live stats (macOS parity: listener-push, not polling) ----------------

void SdkHost::SubscribeStats() {
  if (!device_ || connectVc_) return;
  connectVc_ = device_->openConnectViewController();
  connectVc_->start();
  contractVc_ = device_->openContractViewController();  // live throughput feed
  auto pub = [this] { PublishStats(); };
  // ConnectViewController: status, provider grid/window size, selected location.
  presentationSubs_.push_back(connectVc_->addConnectionStatusListener(pub));
  presentationSubs_.push_back(connectVc_->addGridListener(pub));
  presentationSubs_.push_back(connectVc_->addSelectedLocationListener(
      [this](std::optional<urnet::ConnectLocation>) { PublishStats(); }));
  presentationSubs_.push_back(device_->addConnectLocationChangeListener(
      [this](std::optional<urnet::ConnectLocation>) { PublishStats(); }));
  // ContractViewController: throughput points (bytes/bit rate up/down).
  presentationSubs_.push_back(contractVc_->addThroughputListener(pub));
  // Device: contract status (balance/permission), provide on/off/paused,
  // provide secret keys (network-visible bit), tunnel.
  presentationSubs_.push_back(device_->addContractStatusChangeListener(
      [this](std::optional<urnet::ContractStatus>) { PublishStats(); }));
  presentationSubs_.push_back(device_->addProvideChangeListener([this](bool) { PublishStats(); }));
  presentationSubs_.push_back(
      device_->addProvidePausedChangeListener([this](bool) { PublishStats(); }));
  presentationSubs_.push_back(device_->addProvideSecretKeysListener(
      [this](std::optional<urnet::ProvideSecretKeyList> keys) {
        bool hasNetworkKey = false;
        if (keys) {
          for (const auto& key : *keys) {
            if (key.provide_mode == 1 /* network â€” bit set, per-case */) {
              hasNetworkKey = true;
              break;
            }
          }
        }
        provideHasNetworkKey_.store(hasNetworkKey);
        PublishStats();
      }));
  presentationSubs_.push_back(device_->addTunnelChangeListener([this](bool) { PublishStats(); }));
  PublishStats();  // initial snapshot
}

LiveStats SdkHost::ReadStats() {
  LiveStats s;
  // D4: the DeviceRemote getters below are NOT cached reads. While the rpc
  // transport thinks it is attached, every one of them is a synchronous
  // DeviceLocalRpc.Get* call (device_rpc.go), and against a service that is
  // DYING — socket open, process gone — each blocks until the transport's
  // write timeout tears it down. ReadStats runs on the UI thread (ConnectPage's
  // 1s health tick via RepublishStats, and window activation), and the friend's
  // app glog ends mid Get* burst 4-5s before Windows recorded AppHangB1. The
  // pipe drop is this process's EARLY notice of the death — Go's transport
  // needs seconds more to notice — so with the channel down the device getters
  // are skipped outright. Nothing rendered is lost: the SERVICE_DOWN clamp at
  // the end of this function already replaces everything they would produce.
  // The view-controller reads stay — those objects live in this process and
  // answer from their own pushed state, no rpc involved.
  const bool serviceUp = service_.IsConnected();
  if (connectVc_) {
    s.connectionStatus = connectVc_->getConnectionStatus();
    s.connected = connectVc_->getConnected();
    // getGrid() returns a HANDLE, not a value, and while nothing is connected
    // the Go side has no grid object at all — the call comes back as handle 0.
    // The old code called all four grid getters on that zero handle anyway,
    // and each call was a nil-receiver panic on the Go side, recovered and
    // logged by the cgo guard (handles.go: resolveHandle(0) answers ok=true).
    // MEASURED at ~570 "[cgo]urnet_connect_grid_get_* panicked" lines across a
    // session left sitting disconnected, four per stats push, all noise (the
    // owner's beta logs; signed-out never even opens this controller, so the
    // spam was the signed-in idle state — the commonest state there is). A
    // zero handle is the
    // SDK saying "there is no grid"; treat it as the empty grid it is — the
    // defaults below (0 providers, 0x0, no points) are exactly what the
    // rpc-only and service-down clamps already produce, and the hero renders
    // them as its bare lattice. The guard stays correct against the coming SDK
    // fix too (resolveHandle(0) -> ok=false): the calls are simply never made.
    if (auto grid = connectVc_->getGrid()) {
      s.providerCount = grid.getWindowCurrentSize();
      // The provider grid itself, for the hero canvas. getWidth/getHeight return
      // scalars and cannot throw; the point LIST goes through ReadSdkList like
      // every other list getter, because a nil Go slice marshals as the four-byte
      // document `null` and seven of eleven list getters were observed throwing
      // type_error.302 against a live session. An empty grid is a normal state
      // here, so a nullopt simply leaves the vector empty and the hero renders
      // its bare lattice.
      s.gridWidth = grid.getWidth();
      s.gridHeight = grid.getHeight();
      static std::atomic<bool> gridLogged{false};
      if (auto pts = ReadSdkList(gridLogged, "getProviderGridPointList",
                                 [&] { return grid.getProviderGridPointList(); })) {
        s.gridPoints = std::move(*pts);
      }
    }
  } else if (device_ && serviceUp) {  // getConnectLocation is an rpc — see above
    s.connected = device_->getConnectLocation().has_value();
    s.connectionStatus = s.connected ? "DESTINATION_SET" : "DISCONNECTED";
  }
  if (contractVc_) {
    // Most recent throughput point that has a Remote (tunneled) sample.
    // Guarded: see ReadList. This is the site the spec predicted would fire
    // first, because it is on the window-activation path.
    static std::atomic<bool> logged{false};
    auto pts = ReadSdkList(logged, "getThroughputPoints (stats)",
                        [&] { return contractVc_->getThroughputPoints(); });
    if (pts && !pts->empty()) {
      for (auto it = pts->rbegin(); it != pts->rend(); ++it) {
        if (it->Remote) {
          s.downBitsPerSecond = it->Remote->IngressBitRate;
          s.upBitsPerSecond = it->Remote->EgressBitRate;
          break;
        }
      }
    }
  }
  if (device_ && serviceUp) {  // five more rpc getters — see the note at the top
    if (auto cs = device_->getContractStatus(); cs) s.insufficientBalance = cs->InsufficientBalance;
    s.provideEnabled = device_->getProvideEnabled();
    s.providePaused = device_->getProvidePaused();
    s.provideMode = static_cast<int64_t>(device_->getProvideMode());
    s.provideHasNetworkKey = provideHasNetworkKey_.load();
    if (auto np = device_->getNetworkPeers(); np && np->Connected) {
      s.provideClients = static_cast<int64_t>(np->Connected->size());
    }
    // selected provider (read-only row + dns regional recommendations)
    if (auto loc = device_->getConnectLocation()) {
      if (loc->name) s.locationName = *loc->name;
      if (loc->country_code) s.countryCode = *loc->country_code;
      if (loc->country) s.countryName = *loc->country;
    }
    // The window honesty diagnosis (track 2): the SDK's stall reason and the
    // terminal failed latch. One more rpc getter under the same serviceUp
    // gate as the five above; an old service simply omits the JSON keys and
    // both fields stay at their defaults, which renders as before this
    // feature existed.
    if (auto ws = device_->getWindowStatus()) {
      s.windowStallReason = ws->StallReason;
      s.windowFailed = ws->Failed;
    }
  } else if (!device_) {
    // No session, so no DeviceRemote: what provides now, if anything, is the
    // service's provider-only device, as its last status said. The provide
    // dot, its ring and the discoverable line read these; without them they
    // said "not providing" over a device that is.
    FillProviderOnlyStats(s);
  }

  // ---- rpc-only: clamp the RENDERED connection state ----------------------
  //
  // LAST, so it covers every field the window renders, including the throughput
  // rates filled in above. It belongs here rather than in the window for two
  // reasons: this is where the fields the UI renders are produced, and it is
  // the only place a fix can reach them without touching the UI layer.
  //
  // Clamping TunnelState was NOT enough, because the user-visible connect
  // status never read TunnelState. The chain that reaches the pixels is
  // getConnectionStatus() -> LiveStats::connectionStatus ->
  // MainWindow::ParseConnectStatus -> ApplyConnectStatus. In rpc-only the
  // DeviceLocal is live and negotiates provider transports normally â€” that is
  // the POINT of the mode, and the spec puts connect controls inside it â€” so
  // the moment a location is picked getConnectionStatus() returns CONNECTED and
  // the window shows "Connected", a green dot, a Disconnect button, "Connected
  // to N providers" and a live rate, with zero packets carried. The tray
  // meanwhile reads TunnelState and stays disconnected, so the app contradicts
  // itself.
  //
  // "RPC_ONLY" is deliberately a value the window does not recognise:
  // ParseConnectStatus documents that anything unrecognised reads as
  // Disconnected, precisely so an unknown status cannot leave the button
  // claiming a connection the SDK never made. This uses that existing fail-safe
  // rather than adding a parallel one.
  s.rpcOnly = sessionMode_.load() == proto::StartMode::RpcOnly;
  if (s.rpcOnly) {
    // The true SDK values are kept, not discarded: the developer surface (P2)
    // is the one place that SHOULD see them. Everything the connect page
    // renders is clamped.
    s.rawConnectionStatus = s.connectionStatus;
    s.rawConnected = s.connected;
    s.connectionStatus = "RPC_ONLY";
    s.connected = false;  // gates "Connected to N providers" and the rate line
    s.providerCount = 0;
    s.downBitsPerSecond = 0;
    s.upBitsPerSecond = 0;
    // and the grid, for the same reason: the hero canvas renders provider
    // points, and an rpc-only session's DeviceLocal negotiates provider
    // transports normally, so an unclamped grid would draw a live, populated
    // provider window for a mode that carries no traffic.
    s.gridPoints.clear();
    s.gridWidth = 0;
    s.gridHeight = 0;
    // and the window honesty diagnosis: a session that carries no traffic by
    // design cannot claim the connect attempt failed
    s.windowStallReason.clear();
    s.windowFailed = false;
  }

  // ---- the control channel is gone: the same clamp, the same reason -------
  //
  // The service owns the tunnel. Its process dying takes the wintun adapter and
  // the dynamic WFP session with it, so at that instant nothing is carried and
  // nothing is protected — but connectVc_ hangs off a DeviceRemote whose mTLS
  // listener has just disappeared, and getConnectionStatus() keeps returning the
  // last value it was told. Unclamped, that is a hero that stays green, a button
  // that still says Disconnect and a rate line frozen at its last sample, over a
  // tunnel that no longer exists. Optimistic, and permanent: no correcting push
  // is coming.
  //
  // Deliberately the SAME mechanism as the rpc-only clamp rather than a second
  // one: an unrecognised connection status renders as disconnected
  // (ConnectPage::ParseConnectStatus), so this needs no new UI branch anywhere.
  // Skipped when the rpc-only clamp already fired — that one has already zeroed
  // everything and its notice is the more specific explanation.
  if (!s.rpcOnly && !service_.IsConnected()) {
    s.rawConnectionStatus = s.connectionStatus;
    s.rawConnected = s.connected;
    s.connectionStatus = "SERVICE_DOWN";
    s.connected = false;
    s.providerCount = 0;
    s.downBitsPerSecond = 0;
    s.upBitsPerSecond = 0;
    s.gridPoints.clear();
    s.gridWidth = 0;
    s.gridHeight = 0;
    s.windowStallReason.clear();
    s.windowFailed = false;
  }

  // ---- aggregate connection health (#27) -----------------------------------
  // LAST, after both clamps, so the tracker consumes exactly the fields the UI
  // will render: an rpc-only or service-down snapshot has already had its
  // status replaced with a sentinel ActivityFromStatus reads as Inactive and
  // its grid zeroed. The derivation itself — the transition table, the degrade
  // hold, why an absent grid feed holds rather than sharpens — lives in
  // ConnectionHealth.h where the service selftest pins it.
  {
    health::Signals hs;
    hs.serviceConnected = service_.IsConnected();
    hs.activity = health::ActivityFromStatus(s.connectionStatus);
    // Evidence only counts as evidence while a live grid feed produced it.
    // connectVc_ is read lock-free here like every other field in this
    // function (see the function's own locking notes).
    hs.gridKnown = !s.rpcOnly && connectVc_.has_value() && hs.serviceConnected;
    int64_t cells = 0;
    int64_t proven = 0;
    for (const auto& point : s.gridPoints) {
      if (health::CellOccupiesWindow(point.State)) ++cells;
      if (health::CellProven(point.State)) ++proven;
    }
    // Whichever of the SDK's own window figure and the live cell count says
    // the window is populated: the tracker only asks "is there a window", and
    // the two figures bracket the answer whatever windowCurrentSize counts.
    hs.windowSize = (std::max)(s.providerCount, cells);
    hs.provenCount = proven;
    // the SDK's terminal verdict (already clamped above with everything else)
    hs.windowFailed = s.windowFailed;
    const int64_t nowMillis =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    std::scoped_lock healthLock(healthMutex_);
    s.health = healthTracker_.Update(hs, nowMillis);
    s.provenProviderCount = proven;
    s.healthReevalAtMillis = healthTracker_.ReevalAtMillis();
  }
  ClampCaptureStats(s);
  return s;
}

void SdkHost::ClampCaptureStats(LiveStats& stats) const {
  const auto state = lastServiceState_.load();
  const health::CaptureSignals capture{
      .serviceConnected = service_.IsConnected(),
      .preparing = !stats.rpcOnly && (state == proto::TunnelState::Preparing ||
                                     state == proto::TunnelState::Starting),
      .active = !stats.rpcOnly && state == proto::TunnelState::Up &&
                lastServiceRoutesInstalled_.load() && lastServiceDnsApplied_.load(),
      .failed = state == proto::TunnelState::Error};
  stats.health = health::WithCapture(stats.health, capture);
  if (!capture.active) {
    if (stats.rawConnectionStatus.empty()) {
      stats.rawConnectionStatus = stats.connectionStatus;
      stats.rawConnected = stats.connected;
    }
    stats.connected = false;
    stats.downBitsPerSecond = stats.upBitsPerSecond = 0;
    stats.healthReevalAtMillis = 0;
    stats.connectionStatus = !capture.serviceConnected ? "SERVICE_DOWN"
        : stats.rpcOnly ? "RPC_ONLY"
        : stats.health == health::State::Failed ? "CONNECT_FAILED"
        : capture.preparing ? "CONNECTING" : "DISCONNECTED";
  }
}

void SdkHost::FillProviderOnlyStats(LiveStats& stats) const {
  if (!serviceProviderRunning_.load()) return;
  stats.provideMode = serviceProviderMode_.load();
  // The sdk's own definition (DeviceLocal.GetProvideEnabled): a provider
  // exists exactly when the tier is not none.
  stats.provideEnabled = stats.provideMode != 0;
  stats.provideHasNetworkKey = serviceProviderNetworkKey_.load();
  // Its peers, as the last get_provider_stats answer said. Until one says (or
  // from an older service, which never does) a count would be a guess.
  const int64_t clients = serviceProviderClients_.load();
  stats.provideClients = clients < 0 ? 0 : clients;
  stats.provideClientsUnknown = stats.provideEnabled && clients < 0;
}

void SdkHost::PublishStats() {
  if (onStats_) onStats_(ReadStats());
}

LiveStats SdkHost::CurrentStats() { return ReadStats(); }

// ---- connect drawer feeds (macOS ThroughputStore/ContractDetailsStore/
// BlockActionsStore/DnsSettingsStore parity) --------------------------------

void SdkHost::SubscribeDrawer() {
  // caller holds mutex_ (BootstrapSession)
  if (!device_ || !contractVc_) return;
  blockVc_ = device_->openBlockActionViewController();
  // single-feed view controller: the current sheet is client-only, so open the
  // client feed (open a provider feed into a second VC if a provider sheet is
  // ever added). It owns the display order, the scrolled-away freeze, and the
  // "N new" pending count -- the sheet just reports scroll and renders its rows.
  contractDetailsVc_ = device_->openClientContractDetailsViewController();

  // Offline reconcile: the app LocalState is the source of truth for per-app rules;
  // on connect merge them into the device (which also holds host rules) so the live
  // tunnel matches what the user configured while disconnected. Host rules are kept.
  if (localState_) {
    try {
      if (auto local = localState_->getBlockActionOverrides()) {
        auto merged = device_->getBlockActionOverrides();
        if (!merged) merged = urnet::BlockActionOverrideList{};
        merged->erase(std::remove_if(merged->begin(), merged->end(),
                                     [](const urnet::BlockActionOverride& o) {
                                       return o.AppIds && !o.AppIds->empty();
                                     }),
                      merged->end());
        for (const auto& o : *local)
          if (o.AppIds && !o.AppIds->empty()) merged->push_back(o);
        device_->setBlockActionOverrides(merged);
      }
    } catch (const std::exception& e) {
      LogWarn("sdkhost: merge offline app rules failed: {}", e.what());
    }
  }

  // throughput points feed the three transfer charts
  presentationSubs_.push_back(
      contractVc_->addThroughputListener([this] { PublishThroughput(); }));
  // aggregated per-peer contract rows: the ContractDetailsViewController coalesces
  // the egress + ingress change streams and does the per-peer aggregation +
  // closing lifecycle, then fires one settled ContractRowsChanged we re-read
  presentationSubs_.push_back(
      contractDetailsVc_->addContractRowsListener([this] { PublishContractRows(); }));
  contractDetailsVc_->start();
  // live routing decisions + allow/block counters + overrides ("split rules")
  presentationSubs_.push_back(
      blockVc_->addBlockActionsListener([this] { PublishBlockActions(); }));
  presentationSubs_.push_back(
      blockVc_->addBlockActionStatsListener([this] { PublishBlockStats(); }));
  presentationSubs_.push_back(device_->addBlockActionOverridesChangeListener(
      [this](std::optional<urnet::BlockActionOverrideList>) {
        PublishSplitRules();
        PushLocalOverrideAppsToDriver();  // re-drive the split-tunnel driver on any override change
      }));
  // dns resolver settings + ad/tracker blocker
  presentationSubs_.push_back(device_->addDnsResolverSettingsChangeListener(
      [this](std::optional<urnet::DnsResolverSettings> settings) {
        if (onDnsSettings_) onDnsSettings_(std::move(settings));
      }));
  presentationSubs_.push_back(device_->addBlockerEnabledChangeListener([this](bool on) {
    if (onBlockerEnabled_) onBlockerEnabled_(on);
  }));
  // transport settings, client + provider policies (TRANSPORTSTATS): the dns
  // settings pattern -- fired by the service's DeviceLocal on change, forwarded
  // over the rpc, re-fired with the service's truth on every sync, and locally
  // by the DeviceRemote for an edit queued while the rpc is down. Seeded below
  // with the getters, like dns.
  presentationSubs_.push_back(device_->addTransportSettingsChangeListener(
      [this](std::optional<urnet::TransportSettings> settings) {
        if (onTransportSettings_) {
          onTransportSettings_(TransportSettingsKind::Client, std::move(settings));
        }
      }));
  presentationSubs_.push_back(device_->addProviderTransportSettingsChangeListener(
      [this](std::optional<urnet::TransportSettings> settings) {
        if (onTransportSettings_) {
          onTransportSettings_(TransportSettingsKind::Provider, std::move(settings));
        }
      }));
  // NO routeLocal LISTENER HERE, and its absence is deliberate. This block used
  // to also push `addRouteLocalChangeListener` into `onRouteLocal_`, which the
  // window turned into ApplyKillSwitchUi. That whole mechanism was replaced: the
  // kill switch is now read through the service facts and driven by
  // SetKillSwitch (AppController), so `onRouteLocal_` no longer exists. Re-adding
  // the listener would install a second, silent source of truth for the one
  // control whose whole job is to be unambiguous about whether this machine is
  // blocked.
  //
  // The provider-locations view controller: the SDK's, so the display order
  // (west to east about the providers' centroid), the selection and the wheel's
  // clamped ends are identical in every app.
  //
  // OPENED BEFORE the connected-provider listener below, and that order is
  // load-bearing: the controller subscribes to the same device listener when it
  // is opened, callbacks fire in subscription order, and PublishProviderLocations
  // reads the controller's ordered window. Registering first would read a window
  // one notify behind.
  providerLocationsVc_ = device_->openProviderLocationsViewController();
  // signal only: this fires from inside SetSelectedProviderClientId /
  // StepProviderSelection, on the thread that is already holding mutex_, so it
  // must not read the selection back here -- the handler marshals first
  presentationSubs_.push_back(providerLocationsVc_->addSelectedProviderLocationChangeListener(
      [this] {
        if (onProviderSelection_) onProviderSelection_();
      }));
  providerLocationsVc_->start();
  // connected provider locations: signal-only (no payload), so re-read the
  // getter and publish only when the rows actually changed
  presentationSubs_.push_back(device_->addConnectedProviderLocationChangeListener(
      [this] { PublishProviderLocations(); }));
  // provider identities (the e2e-verified set behind the locations badge):
  // signal-only, same as the locations feed -- re-read and value-compare
  presentationSubs_.push_back(device_->addProviderIdentityChangeListener(
      [this] { PublishProviderIdentities(); }));

  // The extender network (EXTENDER.md K4, K5). The status listener is on the
  // DEVICE, not the space: DeviceRemote answers it over the rpc with the last
  // value cached, exactly as the transport settings do, so this app sees the
  // service's truth rather than its own process's empty directory. The SDK
  // already coalesces to one callback per second, so there is no throttle here.
  presentationSubs_.push_back(device_->addExtenderStatusChangeListener(
      [this](std::optional<urnet::ExtenderStatus> status) {
        PublishExtenderStatus(std::move(status));
      }));
  // The provider extender role on this device (EXTENDER.md N2, N7): its status
  // and, for a status whose row shows, its setting. Relayed by the DeviceRemote
  // through the rpc listener registry with the last value cached, like the
  // extender status above. The device pushes after any change of the setting,
  // the provide state or the role, coalesced to one status per epoch, and never
  // on registration, so the seed below is the first reading. A device process
  // too old to have the listener keeps its session and reports the role
  // unsupported, which hides the rows.
  presentationSubs_.push_back(device_->addExtenderProvideStatusChangeListener(
      [this](std::optional<urnet::ExtenderProvideStatus> status) {
        PublishExtenderProvideStatus(std::move(status));
      }));
  // The view controller behind the account section (K6, K7). Opened with the
  // rest of the drawer so its lifetime is the session's, and deliberately NOT
  // started: start() only subscribes it to the device's extender status -- a
  // second rpc listener for a stream this app already takes directly above --
  // and the settings, share, decode and import calls it is opened for need no
  // subscription at all.
  {
    auto controller = std::make_shared<urnet::ExtenderViewController>(
        device_->openExtenderViewController());
    std::scoped_lock lock(drawerMutex_);
    extenderVc_ = std::move(controller);
  }

  // initial snapshots
  // The throughput one carries the device's own answer for the provider
  // section's gate: the controller SubscribeStats just opened reports no
  // provider stats until it samples, and notifies only after its second sample.
  PublishThroughput(DeviceHasProviderStatsLocked());
  PublishContractRows();
  PublishBlockActions();
  PublishBlockStats();
  PublishSplitRules();
  PushLocalOverrideAppsToDriver();  // seed the driver once the device + service are up
  if (onDnsSettings_) onDnsSettings_(device_->getDnsResolverSettings());
  if (onBlockerEnabled_) onBlockerEnabled_(device_->getBlockerEnabled());
  if (onTransportSettings_) {
    onTransportSettings_(TransportSettingsKind::Client, device_->getTransportSettings());
    onTransportSettings_(TransportSettingsKind::Provider,
                         device_->getProviderTransportSettings());
  }
  {
    static std::atomic<bool> loggedExtenderStatus{false};
    PublishExtenderStatus(ReadSdkList(loggedExtenderStatus, "getExtenderStatus",
                                      [&] { return device_->getExtenderStatus(); }));
  }
  {
    static std::atomic<bool> loggedExtenderProvideStatus{false};
    PublishExtenderProvideStatus(
        ReadSdkList(loggedExtenderProvideStatus, "getExtenderProvideStatus",
                    [&] { return device_->getExtenderProvideStatus(); }));
  }
}

namespace {
// The SDK's TransportDistribution onto the app snapshot the bar draws. A
// struct-shaped getter (the generated from_json early-returns a default value
// for a null document), so unlike the *List getters this cannot throw on nil --
// but the read is still routed through ReadSdkList by the caller for the one
// remaining hazard, a malformed document, which must never take the listener
// thread down.
TransportDistributionSnapshot MapTransportDistribution(
    std::optional<urnet::TransportDistribution> const& distribution) {
  TransportDistributionSnapshot snapshot;
  if (!distribution) return snapshot;
  if (distribution->Shares) {
    snapshot.shares.reserve(distribution->Shares->size());
    for (const auto& share : *distribution->Shares) {
      TransportShareRow row;
      row.transportType = share.TransportType;
      row.h1PlusActive = share.TransportType == urnet::TransportTypeH1 && share.H1PlusConnectionCount > 0;
      row.egressByteCount = share.EgressByteCount;
      row.ingressByteCount = share.IngressByteCount;
      row.share = share.Share;
      row.boundary = share.Boundary;
      row.percent = share.Percent;
      row.used = share.Used;
      row.enabled = share.Enabled;
      snapshot.shares.push_back(std::move(row));
    }
  }
  snapshot.byteCount = distribution->ByteCount;
  snapshot.active = distribution->Active;
  return snapshot;
}
}  // namespace

// The provider section's gate asked of the device itself (EXTENDER.md O8). A
// ContractViewController opened a moment ago reports no provider stats until
// its first sample and notifies only after its second, so the moments that open
// one (a bootstrap, a presentation) ask the device; the throughput tick reads
// the controller, which has sampled by then.
bool SdkHost::DeviceHasProviderStatsLocked() {
  if (!device_) return false;
  static std::atomic<bool> logged{false};
  return ReadSdkList(logged, "getProviderPacketStats (device)",
                     [&] { return device_->getProviderPacketStats(); })
      .has_value();
}

void SdkHost::PublishThroughput(std::optional<bool> deviceHasProviderStats) {
  if (!contractVc_) return;
  std::vector<urnet::ThroughputPoint> points;
  static std::atomic<bool> logged{false};
  if (auto p = ReadSdkList(logged, "getThroughputPoints",
                        [&] { return contractVc_->getThroughputPoints(); }))
    points = std::move(*p);
  int64_t window = contractVc_->getWindowDurationSeconds();
  if (window <= 0) window = 60;
  // The window's remote traffic by transport, read on the SAME tick as the
  // points (TRANSPORTSTATS): the SDK view controller computes the shares,
  // boundaries, percents, used and enabled flags; the app only draws them.
  // Published only when it actually changed, so an idle tick (the series keeps
  // notifying while any retained point is active) does not retrigger the bar.
  static std::atomic<bool> loggedDistribution{false};
  TransportDistributionSnapshot distribution = MapTransportDistribution(
      ReadSdkList(loggedDistribution, "getTransportDistribution",
                  [&] { return contractVc_->getTransportDistribution(); }));
  // The Earnings page's statistics (EXTENDER.md O5, O8), from the same view
  // controller on the same tick: the provider series, the extender series,
  // whether provider packet stats exist and the provider distribution. They are
  // view-controller reads, answered from its own sampled state without an rpc,
  // except the stats gate when the caller brings the device's own answer. No
  // extender stats are read here: whether the role runs is the pushed status's
  // `enabled`, and the view controller samples the stats for its series itself.
  ProviderThroughputSnapshot provider;
  provider.windowSeconds = window;
  static std::atomic<bool> loggedProviderPoints{false};
  if (auto p = ReadSdkList(loggedProviderPoints, "getProviderThroughputPoints",
                           [&] { return contractVc_->getProviderThroughputPoints(); }))
    provider.providerPoints = std::move(*p);
  static std::atomic<bool> loggedExtenderPoints{false};
  if (auto p = ReadSdkList(loggedExtenderPoints, "getExtenderThroughputPoints",
                           [&] { return contractVc_->getExtenderThroughputPoints(); }))
    provider.extenderPoints = std::move(*p);
  if (deviceHasProviderStats) {
    provider.hasProviderStats = *deviceHasProviderStats;
  } else {
    static std::atomic<bool> loggedProviderStats{false};
    provider.hasProviderStats =
        ReadSdkList(loggedProviderStats, "getProviderPacketStats",
                    [&] { return contractVc_->getProviderPacketStats(); })
            .has_value();
  }
  static std::atomic<bool> loggedProviderDistribution{false};
  TransportDistributionSnapshot providerDistribution = MapTransportDistribution(
      ReadSdkList(loggedProviderDistribution, "getProviderTransportDistribution",
                  [&] { return contractVc_->getProviderTransportDistribution(); }));
  bool distributionChanged = false;
  {
    std::scoped_lock lock(drawerMutex_);
    lastThroughputPoints_ = points;
    throughputWindowSeconds_ = window;
    if (distribution != lastTransportDistribution_) {
      lastTransportDistribution_ = distribution;
      distributionChanged = true;
    }
    lastProviderPoints_ = provider.providerPoints;
    lastExtenderPoints_ = provider.extenderPoints;
    lastHasProviderStats_ = *provider.hasProviderStats;
    // published only when it changed, as the client distribution is
    if (providerDistribution != lastProviderDistribution_) {
      lastProviderDistribution_ = providerDistribution;
      provider.providerDistribution = std::move(providerDistribution);
    }
  }
  if (onThroughput_) onThroughput_(std::move(points), window);
  if (distributionChanged && onTransportDistribution_) {
    onTransportDistribution_(std::move(distribution));
  }
  if (onProviderThroughput_) onProviderThroughput_(std::move(provider));
}

// ---- the provider-only device's statistics (no session) ---------------------
//
// See the contract in the header (ProviderOnlyStatsLoop, and the provider-only
// provider status beside CurrentProviderThroughput).

void SdkHost::ProviderOnlyStatsLoop() {
  // What this loop has on screen, so taking it off never touches what a
  // session's own feed published.
  bool shown = false;
  bool wasWanted = false;
  int64_t lastClients = -1;
  std::chrono::steady_clock::time_point nextStatus{};
  for (;;) {
    bool wanted = false;
    std::optional<bool> extenderWrite;
    std::optional<proto::ResetExtenders> extenderReset;
    {
      std::unique_lock lock(providerOnlyMutex_);
      providerOnlyCv_.wait_for(lock, kProviderOnlyStatsInterval,
                               [this] { return providerOnlyStop_ || providerOnlyKick_; });
      if (providerOnlyStop_) return;
      providerOnlyKick_ = false;
      wanted = providerOnlyStatusWanted_;
      extenderWrite = std::exchange(providerOnlyExtenderWrite_, std::nullopt);
      extenderReset = std::exchange(extenderResetResend_, std::nullopt);
    }
    try {
      // The Extender switch's write, outside mutex_ like the read below and
      // before it, so this pass's answer already carries it.
      if (extenderWrite) WriteProviderOnlyExtender(*extenderWrite);
      // An owed extender reset, sent once more, outside mutex_ too.
      if (extenderReset) ResendExtenderReset(*extenderReset);
      bool presenting = false;
      {
        std::scoped_lock lock(presentationMutex_);
        presenting = presentationDesired_;
      }
      // Outside mutex_, which a bootstrap holds for seconds. Nothing is asked
      // of a service that runs no provider-only device, nor while nothing
      // presents.
      const bool asking = presenting && !HasSession() && serviceProviderRunning_.load() &&
                          service_.IsConnected();
      proto::ProviderStats stats;
      const bool answered = asking && service_.GetProviderStats(stats) && stats.available;
      // A want polls at once, as the controller's start() does.
      if (wanted && !wasWanted) nextStatus = {};
      wasWanted = wanted;
      if (!answered && !shown && !wanted) {
        // Nothing on screen and nothing asked for, which is most passes: no
        // session lock, only a snapshot and an extender status whose source
        // went to forget. A hide kept the status (ClearDrawer's rule).
        if (HasSession() || !serviceProviderRunning_.load() || !service_.IsConnected()) {
          ResetProviderOnlyStatus();
          ForgetProviderOnlyExtenderStatus();
        }
      } else {
        std::scoped_lock lock(mutex_);
        if (device_) {
          // A session's own device feeds every surface now, and its
          // controller the provider status.
          shown = false;
          serviceProviderClients_.store(-1);
          lastClients = -1;
          ResetProviderOnlyStatus();
          ForgetProviderOnlyExtenderStatus();
          continue;
        }
        const bool providerGone = !serviceProviderRunning_.load() || !service_.IsConnected();
        if (answered) {
          ShowProviderOnlyStatsLocked(stats);
          shown = true;
          const auto now = std::chrono::steady_clock::now();
          if (wanted && now >= nextStatus && !stats.client_id.empty()) {
            nextStatus = now + kProviderOnlyStatusInterval;
            FetchProviderOnlyStatusLocked(stats.client_id);
          }
        } else if (shown) {
          ClearProviderOnlyStatsLocked(providerGone);
          shown = false;
        }
        if (providerGone) {
          ResetProviderOnlyStatus();
          ForgetProviderOnlyExtenderStatus();
        }
        // Wanted, and the service cannot say (an older service, no
        // provider-only device, no channel): unavailable, never loading for
        // good.
        if (wanted && !answered && (asking || providerGone)) ProviderOnlyStatusUnavailable();
      }
      // The Connect page's count follows the answers.
      if (const int64_t clients = serviceProviderClients_.load(); clients != lastClients) {
        lastClients = clients;
        PublishStats();
      }
    } catch (const std::exception& e) {
      LogWarn("sdkhost: provide: a provider statistics pass failed: {}", e.what());
    }
  }
}

void SdkHost::StopProviderOnlyStats() {
  {
    std::scoped_lock lock(providerOnlyMutex_);
    providerOnlyStop_ = true;
  }
  providerOnlyCv_.notify_all();
  // Joined, not detached, for StopPresentationWorker's reason. At worst it
  // waits out a get_provider_stats in flight: a loopback round trip, or the
  // pipe's own timeout against a service that stopped answering.
  if (providerOnlyThread_.joinable()) providerOnlyThread_.join();
}

void SdkHost::KickProviderOnlyStats() {
  {
    std::scoped_lock lock(providerOnlyMutex_);
    providerOnlyKick_ = true;
  }
  providerOnlyCv_.notify_all();
}

void SdkHost::QueueProviderOnlyExtenderWrite(bool on) {
  {
    std::scoped_lock lock(providerOnlyMutex_);
    // a flip that has not gone out yet is replaced by the newer one
    providerOnlyExtenderWrite_ = on;
    providerOnlyKick_ = true;
  }
  providerOnlyCv_.notify_all();
}

void SdkHost::WriteProviderOnlyExtender(bool on) {
  std::string error;
  const bool written = service_.IsConnected() && service_.SetProvideExtender(on, &error);
  if (!written) {
    LogWarn("sdkhost: provide: the service did not write the provide extender setting: {}",
            error.empty() ? "no control channel" : error);
  }
  // Written or refused, the next status replaces the switch's guess. Set only
  // after the answer, so a status read before the write cannot replace it.
  std::scoped_lock lock(drawerMutex_);
  extenderProvideRepublish_ = true;
}

void SdkHost::QueueExtenderResetResend(proto::ResetExtenders request) {
  {
    std::scoped_lock lock(providerOnlyMutex_);
    extenderResetResend_ = std::move(request);
    providerOnlyKick_ = true;
  }
  providerOnlyCv_.notify_all();
}

void SdkHost::ResendExtenderReset(const proto::ResetExtenders& request) {
  // Once (Common/ExtenderReset.h): the answer is only logged, never owed again,
  // and what this does not deliver the service's next import of the space does.
  if (!service_.IsConnected()) {
    LogInfo("sdkhost: the owed extender reset was dropped: no control channel; the service's "
            "next import applies it");
    return;
  }
  bool reset = false;
  std::string error;
  const extenderreset::ServiceAnswer answer = service_.ResetExtenders(request, &reset, &error);
  LogInfo("sdkhost: the owed extender reset went again: {}{}", extenderreset::ToString(answer),
          answer == extenderreset::ServiceAnswer::Taken
              ? (reset ? " (applied)" : " (no such space, or applied already)")
              : "; the service's next import applies it");
}

void SdkHost::ShowProviderOnlyStatsLocked(const proto::ProviderStats& stats) {
  // caller holds mutex_, with no session
  serviceProviderClients_.store(stats.client_count);
  // The same snapshot PublishThroughput builds from a session's controller,
  // from the provider-only device's own controller in the service.
  ProviderThroughputSnapshot provider;
  provider.windowSeconds = stats.window_seconds > 0 ? stats.window_seconds : 60;
  provider.providerPoints = proto::ProviderPointsOf<urnet::ThroughputPoint>(stats);
  provider.extenderPoints = proto::ExtenderPointsOf<urnet::ThroughputPoint>(stats);
  provider.hasProviderStats = stats.has_provider_stats;
  TransportDistributionSnapshot distribution = MapTransportDistribution(
      proto::ProviderDistributionOf<urnet::TransportDistribution>(stats));
  {
    std::scoped_lock lock(drawerMutex_);
    lastProviderPoints_ = provider.providerPoints;
    lastExtenderPoints_ = provider.extenderPoints;
    lastHasProviderStats_ = stats.has_provider_stats;
    // published only when it changed, as PublishThroughput does
    if (distribution != lastProviderDistribution_) {
      lastProviderDistribution_ = distribution;
      provider.providerDistribution = std::move(distribution);
    }
  }
  if (onProviderThroughput_) onProviderThroughput_(std::move(provider));
  // The extender role (EXTENDER.md N7): the rows, the switch and the extender
  // plot's gate, read as a session's listener reads its push, from the status
  // and the setting the service read off the device. No status (an older
  // service, or a reading that could not be opened) is the role unsupported:
  // all hidden.
  ExtenderProvideStatusView extender = ExtenderProvideStatusViewOf(
      proto::ExtenderProvideStatusOf<urnet::ExtenderProvideStatus>(stats),
      [&stats] { return stats.provide_extender; });
  extender.providerOnly = true;
  extender.serviceWritable = stats.provide_extender_writable;
  PublishExtenderProvideView(std::move(extender));
}

void SdkHost::ClearProviderOnlyStatsLocked(bool providerGone) {
  // caller holds mutex_, with no session
  if (providerGone) serviceProviderClients_.store(-1);
  {
    std::scoped_lock lock(drawerMutex_);
    lastProviderPoints_.clear();
    lastExtenderPoints_.clear();
    lastProviderDistribution_ = {};
    if (providerGone) lastHasProviderStats_ = false;
  }
  if (onProviderThroughput_) {
    ProviderThroughputSnapshot empty;
    empty.providerDistribution = TransportDistributionSnapshot{};
    if (providerGone) empty.hasProviderStats = false;
    onProviderThroughput_(std::move(empty));
  }
}

void SdkHost::FetchProviderOnlyStatusLocked(const std::string& clientId) {
  // caller holds mutex_: ApplyNetworkServer reassigns api_ under it
  if (!api_) return;
  uint64_t generation = 0;
  {
    std::scoped_lock lock(providerOnlyMutex_);
    generation = providerOnlyStatusGeneration_;
  }
  try {
    api_->getProviderStatus([this, generation, clientId](
                                std::optional<urnet::GetProviderStatusResult> result,
                                std::optional<std::string> error) {
      ProviderOnlyStatus status;
      {
        std::scoped_lock lock(providerOnlyMutex_);
        // unwanted or reset while this poll was in flight: it answers nobody
        if (generation != providerOnlyStatusGeneration_) return;
        providerOnlyStatus_.Fetched(result, error, clientId);
        status = providerOnlyStatus_;
      }
      if (onProviderOnlyStatus_) onProviderOnlyStatus_(std::move(status));
    });
  } catch (const std::exception& e) {
    LogWarn("sdkhost: provide: the provider status request failed: {}", e.what());
  }
}

void SdkHost::ResetProviderOnlyStatus() {
  ProviderOnlyStatus status;
  {
    std::scoped_lock lock(providerOnlyMutex_);
    // a poll in flight belongs to the source that went
    ++providerOnlyStatusGeneration_;
    // Only a snapshot is forgotten: an "unavailable" stays until an answer
    // replaces it, so a source that stays gone publishes nothing per tick.
    if (!providerOnlyStatus_.loaded) return;
    providerOnlyStatus_ = ProviderOnlyStatus{};
    status = providerOnlyStatus_;
  }
  if (onProviderOnlyStatus_) onProviderOnlyStatus_(std::move(status));
}

void SdkHost::ProviderOnlyStatusUnavailable() {
  ProviderOnlyStatus status;
  {
    std::scoped_lock lock(providerOnlyMutex_);
    // a failed poll already says so, and so does an earlier pass
    if (!providerOnlyStatus_.error.empty()) return;
    providerOnlyStatus_.Failed("the service reports no provider-only device statistics");
    status = providerOnlyStatus_;
  }
  if (onProviderOnlyStatus_) onProviderOnlyStatus_(std::move(status));
}

void SdkHost::SetProviderOnlyStatusWanted(bool wanted) {
  {
    std::scoped_lock lock(providerOnlyMutex_);
    if (providerOnlyStatusWanted_ == wanted) return;
    providerOnlyStatusWanted_ = wanted;
    // An unwant drops the poll in flight and keeps the snapshot, the
    // controller's stop().
    if (!wanted) ++providerOnlyStatusGeneration_;
    providerOnlyKick_ = true;
  }
  providerOnlyCv_.notify_all();
}

ProviderOnlyStatus SdkHost::CurrentProviderOnlyStatus() {
  std::scoped_lock lock(providerOnlyMutex_);
  return providerOnlyStatus_;
}

void SdkHost::PublishContractRows() {
  if (!contractDetailsVc_) return;

  // The view controller returns render-ready rows: per-peer send/receive stacks
  // (newest first), the two bit-rate sums, the last-activity timestamp, and the
  // closing flag. Map them onto the app row type -- the grouping, ordering,
  // activity signal, and closing lifecycle all live in the VC (macOS
  // ContractDetailsStore.update parity).
  auto entries = [](const std::optional<urnet::ContractEntryList>& list) {
    std::vector<ContractEntry> out;
    if (list) {
      out.reserve(list->size());
      for (const auto& e : *list) {
        out.push_back(ContractEntry{e.ContractId, e.UsedByteCount, e.TotalByteCount, e.BitRate,
                                    e.HasStream});
      }
    }
    return out;
  };
  std::vector<ContractPeerRow> rows;
  static std::atomic<bool> logged{false};
  if (auto list = ReadSdkList(logged, "getContractRows",
                           [&] { return contractDetailsVc_->getContractRows(); })) {
    rows.reserve(list->size());
    for (const auto& r : *list) {
      ContractPeerRow row;
      row.clientId = r.ClientId;
      row.send = entries(r.SendContracts);
      row.receive = entries(r.ReceiveContracts);
      row.sendByteCount = r.SendByteCount;
      row.receiveByteCount = r.ReceiveByteCount;
      row.lastActivityMillis = r.LastActivityMillis;
      row.closing = r.Closing;
      rows.push_back(std::move(row));
    }
  }

  bool changed = false;
  {
    std::scoped_lock lock(drawerMutex_);
    changed = rows != lastContractRows_;
    if (changed) lastContractRows_ = rows;
  }
  if (changed && onContractRows_) onContractRows_(std::move(rows));
}

void SdkHost::PublishBlockActions() {
  if (!blockVc_) return;
  std::vector<BlockActionItem> items;
  static std::atomic<bool> logged{false};
  if (auto list = ReadSdkList(logged, "getBlockActions",
                           [&] { return blockVc_->getBlockActions(); })) {
    items.reserve(list->size());
    // the sdk window is oldest first; the UI wants newest first
    for (auto it = list->rbegin(); it != list->rend(); ++it) {
      BlockActionItem item;
      item.id = it->BlockActionId ? *it->BlockActionId
                                  : std::to_string(it->Time) + ":" +
                                        (it->Hosts && !it->Hosts->empty() ? (*it->Hosts)[0] : "");
      item.timeMillis = it->Time;
      if (it->Hosts) item.hosts = *it->Hosts;
      if (it->Ips) item.ips = *it->Ips;
      // the sdk keeps the matched hosts/ips disjoint from Hosts/Ips
      if (it->MatchedHosts) item.matchedHosts = *it->MatchedHosts;
      if (it->MatchedIps) item.matchedIps = *it->MatchedIps;
      item.block = it->Block;
      item.local = it->Local;
      if (it->OverrideId) item.overrideId = *it->OverrideId;
      item.hasBlockOverride = it->BlockOverride.has_value();
      item.hasRouteOverride = it->RouteOverride.has_value();
      item.reason = it->Reason;
      item.packetCount = it->PacketCount;
      item.byteCount = it->ByteCount;
      items.push_back(std::move(item));
    }
  }
  bool changed = false;
  {
    // the sdk re-emits per routing decision; only publish when the list changed
    std::scoped_lock lock(drawerMutex_);
    changed = items != lastBlockActions_;
    if (changed) lastBlockActions_ = items;
  }
  if (changed && onBlockActions_) onBlockActions_(std::move(items));
}

void SdkHost::PublishBlockStats() {
  if (!blockVc_) return;
  int64_t allowed = 0, blocked = 0;
  if (auto stats = blockVc_->getBlockStats()) {
    allowed = stats->AllowedCount;
    blocked = stats->BlockedCount;
  }
  bool changed = false;
  {
    std::scoped_lock lock(drawerMutex_);
    changed = allowed != lastAllowedCount_ || blocked != lastBlockedCount_;
    lastAllowedCount_ = allowed;
    lastBlockedCount_ = blocked;
  }
  if (changed && onBlockStats_) onBlockStats_(allowed, blocked);
}

void SdkHost::PublishSplitRules() {
  if (!device_) return;
  std::vector<SplitRule> rules;
  static std::atomic<bool> logged{false};
  if (auto list = ReadSdkList(logged, "getBlockActionOverrides (split rules)",
                           [&] { return device_->getBlockActionOverrides(); })) {
    rules.reserve(list->size());
    for (const auto& over : *list) {
      if (!over.OverrideId) continue;
      SplitRule rule;
      rule.overrideId = *over.OverrideId;
      if (over.Hosts) rule.hosts = *over.Hosts;
      rule.routeLocal = over.RouteOverride && over.RouteOverride->Local;
      rules.push_back(std::move(rule));
    }
  }
  bool changed = false;
  {
    std::scoped_lock lock(drawerMutex_);
    changed = rules != lastSplitRules_;
    if (changed) lastSplitRules_ = rules;
  }
  if (changed && onSplitRules_) onSplitRules_(std::move(rules));
}

void SdkHost::PublishProviderLocations() {
  if (!providerLocationsVc_) return;
  std::vector<ProviderLocationRow> rows;
  try {
    // the view controller's window, not the device's: same providers, in the
    // shared display order, and read from the controller so the rows and the
    // selection always come from one snapshot
    if (auto locations = providerLocationsVc_->getProviderLocations()) {
      rows.reserve(locations->size());
      for (const auto& location : *locations) {
        ProviderLocationRow row;
        row.clientId = location.ClientId.value_or(std::string());
        row.country = location.Country;
        row.countryCode = location.CountryCode;
        row.region = location.Region;
        row.city = location.City;
        row.hasLocation = location.HasLocation;
        // the city centroid when known, else the region centroid
        if (location.HasCityCoordinates) {
          row.hasCoordinates = true;
          row.lat = location.CityLat;
          row.lon = location.CityLon;
        } else if (location.HasRegionCoordinates) {
          row.hasCoordinates = true;
          row.lat = location.RegionLat;
          row.lon = location.RegionLon;
        }
        row.connectedSinceMillis = location.ConnectedSinceMillis;
        row.ipFamily = location.IpFamily;
        row.ipFamilyLabel = location.IpFamilyLabel;
        rows.push_back(std::move(row));
      }
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: read connected provider locations failed: {}", e.what());
    return;
  }
  // Value compare, not identity: the listener is signal-only and fires on every
  // window event, so publishing unconditionally would rebuild the sheet (and
  // restart its globe animation) many times a second.
  bool changed = false;
  {
    std::scoped_lock lock(drawerMutex_);
    changed = rows != lastProviderLocations_;
    if (changed) lastProviderLocations_ = rows;
  }
  if (changed && onProviderLocations_) onProviderLocations_(std::move(rows));
}

std::vector<ProviderLocationRow> SdkHost::CurrentProviderLocations() {
  std::scoped_lock lock(drawerMutex_);
  return lastProviderLocations_;
}

void SdkHost::PublishProviderIdentities() {
  if (!device_) return;
  std::vector<ProviderIdentityRow> rows;
  try {
    rows = ReadProviderIdentityRows(device_->getProviderIdentities());
  } catch (const std::exception& e) {
    LogWarn("sdkhost: read provider identities failed: {}", e.what());
    return;
  }
  // Value compare, not identity: the listener is signal-only and re-fires on
  // window churn, so publishing unconditionally would rebuild the sheet.
  bool changed = false;
  {
    std::scoped_lock lock(drawerMutex_);
    changed = !SameProviderIdentityRows(rows, lastProviderIdentities_);
    if (changed) lastProviderIdentities_ = rows;
  }
  if (changed && onProviderIdentities_) onProviderIdentities_(std::move(rows));
}

std::vector<ProviderIdentityRow> SdkHost::CurrentProviderIdentities() {
  std::scoped_lock lock(drawerMutex_);
  return lastProviderIdentities_;
}

void SdkHost::RemoveConnectedProvider(const std::string& clientId) {
  std::scoped_lock lock(mutex_);
  if (!device_ || clientId.empty()) return;
  try {
    if (providerLocationsVc_) {
      // through the view controller: it hands the selection to the next older
      // provider when the removed one is selected, the same as every other app
      providerLocationsVc_->removeProvider(clientId);
    } else {
      device_->removeConnectedProvider(clientId);
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: remove connected provider failed: {}", e.what());
  }
  // the window takes a moment to drop the client; the monitor's change event
  // publishes the trimmed list when it does
}

std::string SdkHost::SelectedProviderClientId() {
  std::scoped_lock lock(mutex_);
  if (!providerLocationsVc_) return std::string();
  try {
    return providerLocationsVc_->getSelectedClientId();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: read selected provider failed: {}", e.what());
    return std::string();
  }
}

void SdkHost::SetSelectedProviderClientId(const std::string& clientId) {
  std::scoped_lock lock(mutex_);
  if (!providerLocationsVc_) return;
  try {
    providerLocationsVc_->setSelectedClientId(clientId);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: select provider failed: {}", e.what());
  }
}

void SdkHost::StepProviderSelection(int steps) {
  std::scoped_lock lock(mutex_);
  if (!providerLocationsVc_ || steps == 0) return;
  try {
    providerLocationsVc_->stepSelection(steps);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: step provider selection failed: {}", e.what());
  }
}

void SdkHost::PushLocalOverrideAppsToDriver() {
  if (!device_) return;
  // getLocalOverrideAppIds() already inverts: Included = Local (bypass), Excluded =
  // remote (through the tunnel). Android's "inclusions take precedence": any through-
  // tunnel app => ALLOWLIST with the tunnel set; else DENYLIST with the bypass set.
  std::vector<std::string> paths;
  bool allowlist = false;
  try {
    if (auto ids = device_->getLocalOverrideAppIds()) {
      if (ids->Excluded && !ids->Excluded->empty()) {
        paths = *ids->Excluded;   // through-tunnel apps => allowlist keep-set
        allowlist = true;
      } else if (ids->Included) {
        paths = *ids->Included;   // bypass apps => denylist redirect-set
      }
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: read local override app ids failed: {}", e.what());
    return;
  }
  if (service_.IsConnected()) service_.SetSplitTunnel(paths, allowlist);
}

void SdkHost::ClearDrawer(bool sessionEnding) {
  {
    std::scoped_lock lock(drawerMutex_);
    lastThroughputPoints_.clear();
    lastContractRows_.clear();
    lastBlockActions_.clear();
    lastAllowedCount_ = 0;
    lastBlockedCount_ = 0;
    lastSplitRules_.clear();
    lastProviderLocations_.clear();
    lastTransportDistribution_ = {};
    lastExtenderStatus_ = {};
    lastProviderPoints_.clear();
    lastExtenderPoints_.clear();
    lastProviderDistribution_ = {};
    // a hide keeps these for the window that comes back (O8), and the next
    // presentation's reads confirm them
    if (sessionEnding) {
      lastExtenderProvideStatus_ = {};
      extenderProvideRepublish_ = false;
      lastHasProviderStats_ = false;
    }
  }
  if (onThroughput_) onThroughput_({}, 60);
  if (onTransportDistribution_) onTransportDistribution_({});
  // an empty status, not a stale one: with no session the panel says 0 of 0
  // with a red dot, which is the truth
  if (onExtenderStatus_) onExtenderStatus_({});
  // With no session there is no role and no provider to report (N1, O8). A
  // window that only hid keeps both: its rows and groups stay as they were, and
  // the next presentation's reads confirm them. The charts and the bar empty
  // either way, and refill from the next tick.
  if (sessionEnding && onExtenderProvideStatus_) onExtenderProvideStatus_({});
  if (onProviderThroughput_) {
    ProviderThroughputSnapshot empty;
    empty.providerDistribution = TransportDistributionSnapshot{};
    if (sessionEnding) empty.hasProviderStats = false;
    onProviderThroughput_(std::move(empty));
  }
  if (onTransportSettings_) {
    onTransportSettings_(TransportSettingsKind::Client, std::nullopt);
    onTransportSettings_(TransportSettingsKind::Provider, std::nullopt);
  }
  if (onContractRows_) onContractRows_({});
  if (onBlockActions_) onBlockActions_({});
  if (onBlockStats_) onBlockStats_(0, 0);
  if (onSplitRules_) onSplitRules_({});
  if (onProviderLocations_) onProviderLocations_({});
  if (onDnsSettings_) onDnsSettings_(std::nullopt);
  // clear the chooser's peer-count sub-label + any open sheet on logout
  if (onLocations_) onLocations_(std::nullopt, std::string());
  if (onPeers_) onPeers_(std::nullopt);
  // R4: the Network destination observes the same two feeds as the chooser
  // sheet, so it has to be cleared with them.
  if (onLocationsObserver_) onLocationsObserver_(std::nullopt, std::string());
  if (onPeersObserver_) onPeersObserver_(std::nullopt);
}

std::vector<urnet::ThroughputPoint> SdkHost::CurrentThroughputPoints(int64_t& windowSeconds) {
  std::scoped_lock lock(drawerMutex_);
  windowSeconds = throughputWindowSeconds_;
  return lastThroughputPoints_;
}

std::vector<ContractPeerRow> SdkHost::CurrentContractRows() {
  std::scoped_lock lock(drawerMutex_);
  return lastContractRows_;
}

void SdkHost::SetContractsAtTop(bool atTop) {
  // report the sheet's scroll position to the shared view controller, which owns
  // the at-top activity sort and the scrolled-away freeze (macOS setAtTop parity)
  std::scoped_lock lock(mutex_);
  if (contractDetailsVc_) contractDetailsVc_->setAtTop(atTop);
}

int64_t SdkHost::ContractsPendingCount() {
  // the VC's "N new" count: rows that arrived while scrolled away and are not yet
  // merged (0 at the top)
  std::scoped_lock lock(mutex_);
  return contractDetailsVc_ ? contractDetailsVc_->pendingCount() : 0;
}

std::vector<BlockActionItem> SdkHost::CurrentBlockActions() {
  std::scoped_lock lock(drawerMutex_);
  return lastBlockActions_;
}

void SdkHost::CurrentBlockCounts(int64_t& allowed, int64_t& blocked) {
  std::scoped_lock lock(drawerMutex_);
  allowed = lastAllowedCount_;
  blocked = lastBlockedCount_;
}

std::vector<SplitRule> SdkHost::CurrentSplitRules() {
  std::scoped_lock lock(drawerMutex_);
  return lastSplitRules_;
}

std::optional<urnet::DnsResolverSettings> SdkHost::CurrentDnsSettings() {
  if (!device_) return std::nullopt;
  try {
    return device_->getDnsResolverSettings();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get dns settings failed: {}", e.what());
    return std::nullopt;
  }
}

bool SdkHost::CurrentBlockerEnabled() {
  if (!device_) return false;
  try {
    return device_->getBlockerEnabled();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get blocker failed: {}", e.what());
    return false;
  }
}

TransportDistributionSnapshot SdkHost::CurrentTransportDistribution() {
  std::scoped_lock lock(drawerMutex_);
  return lastTransportDistribution_;
}

namespace {
// The SDK's ExtenderStatus onto the plain view the panel draws (K4, K5).
// Mirrored rather than passed through so the UI layer never sees an SDK type
// and the mapping is exercised by tools/extender-tests.cpp.
ExtenderStatusView MapExtenderStatus(std::optional<urnet::ExtenderStatus> const& status) {
  ExtenderStatusView view;
  if (!status) return view;
  view.gossipState = status->GossipState;
  view.activeCount = status->ActiveCount;
  view.reserveCount = status->ReserveCount;
  view.eventCountLastMinute = status->EventCountLastMinute;
  if (status->Extenders) {
    view.extenders.reserve(status->Extenders->size());
    for (urnet::ExtenderInfo const& extenderInfo : *status->Extenders) {
      view.extenders.push_back(
          ExtenderInfoView{extenderInfo.Ip, extenderInfo.ColorHex, extenderInfo.InUse});
    }
  }
  return view;
}
}  // namespace

void SdkHost::PublishExtenderStatus(std::optional<urnet::ExtenderStatus> status) {
  ExtenderStatusView view = MapExtenderStatus(status);
  {
    std::scoped_lock lock(drawerMutex_);
    // The SDK fires once a second whether or not anything moved (it coalesces a
    // change stream, it does not suppress a repeat), and the panel's rebuild
    // tears down and rebuilds a row of shapes. Comparing here is what keeps an
    // idle extender network off the UI thread entirely.
    if (view == lastExtenderStatus_) return;
    lastExtenderStatus_ = view;
  }
  if (onExtenderStatus_) onExtenderStatus_(std::move(view));
}

ExtenderStatusView SdkHost::CurrentExtenderStatus() {
  std::scoped_lock lock(drawerMutex_);
  return lastExtenderStatus_;
}

void SdkHost::PublishExtenderProvideStatus(std::optional<urnet::ExtenderProvideStatus> status) {
  // The view reads only the fields the apps may read (N7), and the setting only
  // for a status whose row shows (N1). The setting is the switch's position: the
  // DeviceRemote answers the queued or last-known value while the device process
  // is out of contact, so the switch holds through a daemon restart. It hands a
  // listener its status after releasing its own lock, so the read cannot
  // deadlock it, and the read is made without mutex_, as every listener callback
  // here reads the device: the subscription is dropped in ClosePresentationLocked
  // before the device is, and mutex_ is held across a whole bootstrap.
  ExtenderProvideStatusView view = ExtenderProvideStatusViewOf(status, [this] {
    // D4: with the control pipe down the getter is an rpc into a dying service
    // that waits out the transport timeout on the callback goroutine, holding the
    // DeviceRemote lock every UI-thread setter needs. The setting last published
    // stands in.
    if (device_ && service_.IsConnected()) return device_->getProvideExtender();
    std::scoped_lock lock(drawerMutex_);
    return lastExtenderProvideStatus_.provideExtender;
  });
  PublishExtenderProvideView(std::move(view));
}

void SdkHost::PublishExtenderProvideView(ExtenderProvideStatusView view) {
  {
    std::scoped_lock lock(drawerMutex_);
    // The device pushes after any change of the setting, the provide state or
    // the role, coalesced to one status per epoch and never on registration. A
    // push equal to the last one stays off the UI thread, unless a write since
    // the last publish left a guess on screen that only a push replaces.
    if (view == lastExtenderProvideStatus_ && !extenderProvideRepublish_) return;
    lastExtenderProvideStatus_ = view;
    extenderProvideRepublish_ = false;
  }
  if (onExtenderProvideStatus_) onExtenderProvideStatus_(std::move(view));
}

void SdkHost::ForgetProviderOnlyExtenderStatus() {
  {
    std::scoped_lock lock(drawerMutex_);
    // a session's own status, or none, is not this one to take off
    if (!lastExtenderProvideStatus_.providerOnly) return;
    lastExtenderProvideStatus_ = {};
  }
  if (onExtenderProvideStatus_) onExtenderProvideStatus_({});
}

ExtenderProvideStatusView SdkHost::CurrentExtenderProvideStatus() {
  std::scoped_lock lock(drawerMutex_);
  return lastExtenderProvideStatus_;
}

ProviderThroughputSnapshot SdkHost::CurrentProviderThroughput() {
  std::scoped_lock lock(drawerMutex_);
  ProviderThroughputSnapshot snapshot;
  snapshot.providerPoints = lastProviderPoints_;
  snapshot.extenderPoints = lastExtenderPoints_;
  snapshot.windowSeconds = throughputWindowSeconds_;
  snapshot.hasProviderStats = lastHasProviderStats_;
  snapshot.providerDistribution = lastProviderDistribution_;
  return snapshot;
}

std::shared_ptr<urnet::ExtenderViewController> SdkHost::ExtenderController() {
  std::scoped_lock lock(drawerMutex_);
  return extenderVc_;
}

std::optional<urnet::NetExtender> SdkHost::CurrentNetExtender() {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return std::nullopt;
  try {
    return networkSpace_->getNetExtender();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get net extender failed: {}", e.what());
    return std::nullopt;
  }
}

bool SdkHost::SetNetExtender(const std::optional<urnet::NetExtender>& value) {
  std::scoped_lock lock(mutex_);
  if (!spaceManager_ || !networkSpace_) return false;
  try {
    // The values have to go back WHOLE (updateNetworkSpaceValues replaces
    // them), and the space's own json is the only reading of them the C ABI
    // offers -- the getters return EFFECTIVE values, and writing those back
    // would pin every derived default as an explicit override.
    const nlohmann::json document = nlohmann::json::parse(networkSpace_->toJson());
    urnet::NetworkSpaceKey key{};
    if (auto it = document.find("key"); it != document.end() && !it->is_null()) {
      it->get_to(key);
    }
    if (!key.host_name || key.host_name->empty()) {
      // A default-constructed key names a DIFFERENT space, so an unreadable
      // one must refuse rather than write the private extender somewhere else.
      LogWarn("sdkhost: set net extender refused: the space json carries no key");
      return false;
    }
    urnet::NetworkSpaceValues values{};
    if (auto it = document.find("values"); it != document.end() && !it->is_null()) {
      it->get_to(values);
    }
    values.net_extender = value;
    // Only the extender values changed, so the manager applies this in place
    // (sdk network_space.go onlyExtenderValuesChanged) and hands back a handle
    // to the SAME space; nothing derived from it is invalidated.
    networkSpace_ = spaceManager_->updateNetworkSpaceValues(key, values);
    // No session: the provider-only device runs on the space it was built
    // from. The reconcile's request carries this one (start_provider's
    // network_space_json), so the service builds the device again on it.
    if (!device_) RequestProviderReconcile("private extender saved");
    return true;
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set net extender failed: {}", e.what());
    return false;
  } catch (...) {
    LogWarn("sdkhost: set net extender failed");
    return false;
  }
}

bool SdkHost::ResetExtenders() {
  std::optional<proto::ResetExtenders> request;
  {
    std::scoped_lock lock(mutex_);
    if (!networkSpace_) return false;
    try {
      // In place, through the space's manager: this handle, the DeviceRemote
      // bound to it and the view controller opened on that stay valid.
      const std::string resetId = networkSpace_->resetExtenders();
      request = proto::ResetExtendersRequestFor(networkSpace_->getKey(), resetId);
    } catch (const std::exception& e) {
      LogWarn("sdkhost: reset extenders failed: {}", e.what());
      return false;
    } catch (...) {
      LogWarn("sdkhost: reset extenders failed");
      return false;
    }
  }
  LogInfo("sdkhost: extenders reset in the app's network space");
  // No provider reconcile, unlike SetNetExtender and the other space saves:
  // the verb resets the space the provider-only device runs in where it runs,
  // so there is nothing to rebuild it for now. The next reconcile's request
  // carries the reset space, and an applied reset is a no-op there.
  if (!request) {
    LogWarn("sdkhost: the space names no key for the service's extender reset; its next "
            "import applies it");
    return true;
  }
  // Outside mutex_: the pipe serializes calls, and this one can wait behind a
  // start_tunnel.
  if (!service_.IsConnected()) {
    owedExtenderReset_.Answered(*request, extenderreset::ServiceAnswer::NotTaken);
    LogInfo("sdkhost: no control channel; the service applies the extender reset at its "
            "next import");
    return true;
  }
  bool reset = false;
  std::string error;
  const extenderreset::ServiceAnswer answer = service_.ResetExtenders(*request, &reset, &error);
  // A busy refusal is owed until a pushed status ends the operation that held
  // the service's lock (the state handler); any other answer owes nothing.
  owedExtenderReset_.Answered(*request, answer);
  switch (answer) {
    case extenderreset::ServiceAnswer::Taken:
      LogInfo("sdkhost: the service {} the extender reset",
              reset ? "applied" : "held no such space or had already applied");
      break;
    case extenderreset::ServiceAnswer::Busy:
      LogInfo("sdkhost: the service is busy with a tunnel operation; the extender reset goes "
              "again once it ends");
      break;
    case extenderreset::ServiceAnswer::NotTaken:
      LogWarn("sdkhost: the service did not take the extender reset ({}); its next import "
              "applies it",
              error.empty() ? "no detail" : error);
      break;
  }
  return true;
}

// ---- VLESS ------------------------------------------------------------------
//
// Nothing here logs a link or a field of the settings: the user id IS the
// credential of the user's server.

// The error-id calls here and below (setVlessSettings, validateVlessSettings,
// setControlDohUrls) answer URNET_ERROR_ID_INTERNAL when the call could not
// run, and the sheets know it by the app's one copy of it (SdkErrorId.h). This
// file includes the C header, so the two are held equal here; a header from
// before the define has nothing to hold it to.
#if defined(URNET_ERROR_ID_INTERNAL)
static_assert(std::string_view{URNET_ERROR_ID_INTERNAL} == kSdkErrorIdInternal,
              "SdkErrorId.h no longer mirrors urnetwork_sdk.h's URNET_ERROR_ID_INTERNAL");
#endif

std::optional<urnet::VlessSettings> SdkHost::CurrentVlessSettings() {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return std::nullopt;
  try {
    return networkSpace_->getVlessSettings();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get vless settings failed: {}", e.what());
  } catch (...) {
    LogWarn("sdkhost: get vless settings failed");
  }
  return std::nullopt;
}

std::optional<std::string> SdkHost::SetVlessSettings(const urnet::VlessSettings& settings) {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return std::nullopt;
  try {
    // The sdk persists the change through the space's manager and applies it
    // in place (network_space.go updateInPlaceValues): the client strategy's
    // VLESS dialer is replaced and this handle stays the same space, so unlike
    // SetNetExtender there is nothing to re-take.
    std::string errorId = networkSpace_->setVlessSettings(settings);
    if (errorId.empty()) {
      LogInfo("sdkhost: vless settings saved (enabled={})", settings.enabled.value_or(false));
      // No session: the provider-only device reaches the platform through the
      // space it was built from; the reconcile rebuilds it on this one.
      if (!device_) RequestProviderReconcile("vless settings saved");
    }
    return errorId;
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set vless settings failed: {}", e.what());
  } catch (...) {
    LogWarn("sdkhost: set vless settings failed");
  }
  return std::nullopt;
}

std::optional<urnet::VlessLinkResult> SdkHost::ParseVlessLink(const std::string& link) {
  try {
    return urnet::parseVlessLink(link);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: parse vless link failed: {}", e.what());
  } catch (...) {
    LogWarn("sdkhost: parse vless link failed");
  }
  return std::nullopt;
}

std::string SdkHost::VlessSettingsLink(const urnet::VlessSettings& settings) {
  try {
    return urnet::vlessSettingsLink(settings);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: vless settings link failed: {}", e.what());
  } catch (...) {
    LogWarn("sdkhost: vless settings link failed");
  }
  return {};
}

std::optional<std::string> SdkHost::ValidateVlessSettings(const urnet::VlessSettings& settings) {
  try {
    return urnet::validateVlessSettings(settings);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: validate vless settings failed: {}", e.what());
  } catch (...) {
    LogWarn("sdkhost: validate vless settings failed");
  }
  return std::nullopt;
}

// ---- bootstrap DNS-over-HTTPS servers ---------------------------------------
//
// Nothing here logs the servers: which resolver a user can reach says where
// they are.

std::optional<std::vector<std::string>> SdkHost::CurrentControlDohUrls() {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return std::nullopt;
  try {
    return networkSpace_->getControlDohUrls().value_or(urnet::StringList{});
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get control doh urls failed: {}", e.what());
  } catch (...) {
    LogWarn("sdkhost: get control doh urls failed");
  }
  return std::nullopt;
}

std::optional<std::string> SdkHost::SetControlDohUrls(const std::vector<std::string>& urls) {
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return std::nullopt;
  try {
    // Through the space's own setter, not SetNetExtender's write of the values
    // json: the setter validates each line (an https url on an ip literal),
    // drops repeats, normalizes and answers the error id, which a whole-values
    // write would skip. It applies in place (network_space.go
    // updateInPlaceValues): the strategy's DoH cache is swapped and this handle
    // stays the same space, so there is nothing to re-take.
    std::string errorId = networkSpace_->setControlDohUrls(urnet::StringList(urls));
    if (errorId.empty()) {
      LogInfo("sdkhost: bootstrap doh servers saved");
      // No session: the provider-only device resolves the api through the
      // servers of the space it was built from, so in China it may never reach
      // it until it is rebuilt. The reconcile's request carries the saved
      // space, and a changed request builds a new device (start_provider).
      if (!device_) RequestProviderReconcile("bootstrap doh servers saved");
    }
    return errorId;
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set control doh urls failed: {}", e.what());
  } catch (...) {
    LogWarn("sdkhost: set control doh urls failed");
  }
  return std::nullopt;
}

std::vector<std::string> SdkHost::RegionalControlDohUrls(const std::string& countryCode) {
  try {
    return urnet::regionalControlDohUrls(countryCode).value_or(urnet::StringList{});
  } catch (const std::exception& e) {
    LogWarn("sdkhost: regional control doh urls failed: {}", e.what());
  } catch (...) {
    LogWarn("sdkhost: regional control doh urls failed");
  }
  return {};
}

std::optional<urnet::TransportSettings> SdkHost::CurrentTransportSettings(
    TransportSettingsKind kind) {
  try {
    if (device_) {
      return kind == TransportSettingsKind::Provider ? device_->getProviderTransportSettings()
                                                     : device_->getTransportSettings();
    }
    // no session: the app-side mirror is what the next bootstrap will seed the
    // device with, so it is the truth the editor and the unused footer should
    // show meanwhile (nullopt when never edited here)
    if (localState_) {
      return kind == TransportSettingsKind::Provider
                 ? localState_->getProviderTransportSettings()
                 : localState_->getTransportSettings();
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get transport settings failed: {}", e.what());
  }
  return std::nullopt;
}

std::optional<urnet::TransportStatus> SdkHost::CurrentTransportStatus(
    TransportSettingsKind kind) {
  try {
    if (device_) {
      return kind == TransportSettingsKind::Provider ? device_->getProviderTransportStatus()
                                                     : device_->getTransportStatus();
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get transport status failed: {}", e.what());
  }
  return std::nullopt;
}

PerformanceSettings SdkHost::CurrentPerformanceSettings() {
  PerformanceSettings s;
  std::optional<urnet::PerformanceProfile> profile;
  try {
    if (device_) {
      profile = device_->getPerformanceProfile();
    } else if (localState_) {
      profile = localState_->getPerformanceProfile();
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get performance profile failed: {}", e.what());
  }
  if (!profile) return s;  // nil profile â‰¡ window type auto, everything off
  // a nil profile and window type "auto" mean the same thing (macOS
  // loadPerformanceProfileFromDevice parity)
  if (profile->window_type == urnet::WindowTypeQuality) {
    s.mode = ConnectionMode::Web;
  } else if (profile->window_type == urnet::WindowTypeSpeed) {
    s.mode = ConnectionMode::Streaming;
  } else {
    s.mode = ConnectionMode::Auto;
  }
  s.allowDirect = profile->allow_direct;
  s.postQuantum = profile->post_quantum_encryption;
  s.fixedIp = profile->window_size && profile->window_size->window_size_min == 1 &&
              profile->window_size->window_size_max == 1;
  return s;
}

void SdkHost::SetPerformanceSettings(const PerformanceSettings& settings) {
  std::scoped_lock lock(mutex_);
  // always a profile, even for window type auto, so the orthogonal settings
  // (allow direct, post quantum encryption) persist and apply in every mode
  // (macOS DeviceManager createPerformanceProfile parity)
  urnet::PerformanceProfile p;
  p.allow_direct = settings.allowDirect;
  p.post_quantum_encryption = settings.postQuantum;
  if (settings.mode == ConnectionMode::Auto) {
    // no fixed window type or size
    p.window_type = urnet::WindowTypeAuto;
  } else {
    p.window_type = settings.mode == ConnectionMode::Web ? urnet::WindowTypeQuality
                                                         : urnet::WindowTypeSpeed;
    urnet::WindowSizeSettings ws;
    ws.window_size_min = settings.fixedIp ? 1 : 2;
    ws.window_size_max = settings.fixedIp ? 1 : 4;
    p.window_size = ws;
  }
  const std::optional<urnet::PerformanceProfile> profile = std::move(p);
  try {
    if (localState_) localState_->setPerformanceProfile(profile);  // persistence
    if (device_) device_->setPerformanceProfile(profile);          // live device
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set performance profile failed: {}", e.what());
  }
}

void SdkHost::SetBlockerEnabled(bool on) {
  std::scoped_lock lock(mutex_);
  if (!device_) return;
  try {
    device_->setBlockerEnabled(on);  // the device persists and restores this
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set blocker failed: {}", e.what());
  }
}

bool SdkHost::CurrentKillSwitch() {
  try {
    // kill switch == !routeLocal (SdkHost.h)
    if (device_) return !device_->getRouteLocal();
    // tunnel down: the persisted preference is still the truth
    if (localState_) return !localState_->getRouteLocal();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get route local failed: {}", e.what());
  }
  return false;  // no state at all: claim the permissive default, not the strict one
}

bool SdkHost::SetKillSwitch(bool on) {
  std::scoped_lock lock(mutex_);
  // A kill switch stuck in the WRONG state is a privacy failure, not a cosmetic
  // one, so this reports whether it took instead of swallowing the throw.
  //
  // The order is deliberate too. LocalState is the persistent truth the session
  // bootstrap restores from, so it is written FIRST: if the device write then
  // throws, the two disagree only until the next session and the value that
  // survives is the one the user asked for. Writing the device first lost the
  // LocalState write entirely whenever it threw, leaving device and LocalState
  // silently disagreeing with nothing to notice it.
  bool ok = true;
  try {
    if (localState_) localState_->setRouteLocal(!on);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: persist route local failed: {}", e.what());
    ok = false;
  }
  try {
    if (device_) device_->setRouteLocal(!on);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set route local on device failed: {}", e.what());
    ok = false;
  }
  // ...and the leg that actually enforces it. routeLocal is a branch DOWNSTREAM
  // of the OS routing decision — it only sees packets the kernel already routed
  // into the tun — so it cannot cover IPv6, the LAN, another adapter's
  // resolver, a split-tunnel exclusion, or a dead service, which is the case a
  // kill switch exists for. The service's WFP policy is what covers those. Kept
  // alongside rather than instead of: the two legs disagreeing is a privacy
  // failure, so both are written and both report.
  if (service_.IsConnected() && !service_.SetKillSwitch(on)) {
    LogWarn("sdkhost: the service did not accept the kill-switch change; the "
            "firewall policy may not match the setting");
    ok = false;
  }
  // Turning it off with no session lifts the armed floor, the one state that
  // holds the provider-only device off (ProvideLifecycle.h): providing resumes.
  if (!on && !device_) RequestProviderReconcile("kill switch turned off");
  return ok;
}

std::string SdkHost::CurrentProvideControlMode() {
  try {
    if (device_) return device_->getProvideControlMode();
    // tunnel down: the persisted preference is still the truth
    if (localState_) return localState_->getProvideControlMode();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: get provide control mode failed: {}", e.what());
  }
  return "never";
}

void SdkHost::SetProvideControlMode(const std::string& mode) {
  std::scoped_lock lock(mutex_);
  try {
    if (device_) device_->setProvideControlMode(mode);
    // Persist alongside the device write (macOS handleProvideControlModeUpdate
    // parity) â€” DeviceLocal.SetProvideControlMode alone does not persist, and
    // the session bootstrap restores the persisted mode.
    if (localState_) localState_->setProvideControlMode(mode);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set provide control mode failed: {}", e.what());
  }
  // No session: the provider-only device follows the new mode — started for a
  // mode that provides while disconnected, stopped for one that does not. Off
  // this (UI) thread: starting one builds a DeviceLocal in the service.
  if (!device_) RequestProviderReconcile("provide mode changed");
}

void SdkHost::SetProvideExtender(bool on) {
  std::scoped_lock lock(mutex_);
  ExtenderProvideStatusView shown;
  {
    std::scoped_lock drawerLock(drawerMutex_);
    shown = lastExtenderProvideStatus_;
  }
  switch (ExtenderProvideWriteRouteFor(device_.has_value(), shown)) {
    case ExtenderProvideWriteRoute::Device:
      break;
    case ExtenderProvideWriteRoute::Service:
      // Not on this (UI) thread: the pipe serializes calls, and this one can
      // wait behind a start_tunnel.
      QueueProviderOnlyExtenderWrite(on);
      return;
    case ExtenderProvideWriteRoute::None:
      return;
  }
  // The device persists it in its space and applies it at once (N4); detached,
  // the DeviceRemote queues it for the next sync, and a device process with no
  // setter drops it rather than replaying it on every reconnect.
  device_->setProvideExtender(on);
  // The row paints its guess after this returns (N7). The device emits a status
  // for the write within its one-second epoch, but two flips inside one epoch
  // can land on the status already published, which the dedup would drop and so
  // leave the guess standing.
  std::scoped_lock drawerLock(drawerMutex_);
  extenderProvideRepublish_ = true;
}

void SdkHost::ApplyDnsSettings(const urnet::DnsResolverSettings& settings) {
  std::scoped_lock lock(mutex_);
  if (!device_) return;
  try {
    device_->setDnsResolverSettings(settings);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set dns settings failed: {}", e.what());
  }
  if (onDnsSettings_) onDnsSettings_(CurrentDnsSettings());
}

void SdkHost::ApplyTransportSettings(TransportSettingsKind kind,
                                     const urnet::TransportSettings& settings) {
  std::scoped_lock lock(mutex_);
  const bool provider = kind == TransportSettingsKind::Provider;
  // the device first: attached, the service applies it (make-before-break
  // migration of the live window) and persists it; detached, the DeviceRemote
  // queues it for the next sync. Both fire the change listener.
  if (device_) {
    try {
      if (provider) {
        device_->setProviderTransportSettings(settings);
      } else {
        device_->setTransportSettings(settings);
      }
    } catch (const std::exception& e) {
      LogWarn("sdkhost: set {} transport settings failed: {}",
              provider ? "provider" : "client", e.what());
    }
  }
  // then the app-side mirror, with or without a device: it seeds the device on
  // the next bootstrap and answers the offline reads (see the header note)
  if (localState_) {
    try {
      if (provider) {
        localState_->setProviderTransportSettings(settings);
      } else {
        localState_->setTransportSettings(settings);
      }
    } catch (const std::exception& e) {
      LogWarn("sdkhost: persist {} transport settings failed: {}",
              provider ? "provider" : "client", e.what());
    }
  }
  // No session: a provider-only device runs on the policy it was built with.
  // The reconcile's request now carries the new one, so the service builds a
  // new device for it.
  if (provider && !device_) RequestProviderReconcile("provider transport policy changed");
  if (onTransportSettings_) onTransportSettings_(kind, CurrentTransportSettings(kind));
}

void SdkHost::CreateSplitRule(const std::vector<std::string>& hosts) {
  std::scoped_lock lock(mutex_);
  if (!device_ || hosts.empty()) return;
  try {
    urnet::BlockActionOverride over;
    over.OverrideId = urnet::newId();
    over.Hosts = hosts;
    urnet::RouteOverride route;
    route.Local = true;
    over.RouteOverride = route;
    device_->addBlockActionOverride(over);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: create split rule failed: {}", e.what());
  }
  PublishSplitRules();
}

void SdkHost::UpdateSplitRule(const std::string& overrideId,
                              const std::vector<std::string>& hosts) {
  {
    std::scoped_lock lock(mutex_);
    if (!device_) return;
    if (!hosts.empty()) {
      try {
        // rebuild the full override list with the rule's hosts replaced
        auto list = device_->getBlockActionOverrides();
        if (!list) return;
        bool found = false;
        for (auto& over : *list) {
          if (over.OverrideId && *over.OverrideId == overrideId) {
            over.Hosts = hosts;
            found = true;
            break;
          }
        }
        if (found) device_->setBlockActionOverrides(list);
      } catch (const std::exception& e) {
        LogWarn("sdkhost: update split rule failed: {}", e.what());
      }
      PublishSplitRules();
      return;
    }
  }
  // empty selection removes the rule (RemoveSplitRule takes the lock itself)
  RemoveSplitRule(overrideId);
}

void SdkHost::RemoveSplitRule(const std::string& overrideId) {
  std::scoped_lock lock(mutex_);
  if (!device_) return;
  try {
    device_->removeBlockActionOverride(overrideId);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: remove split rule failed: {}", e.what());
  }
  PublishSplitRules();
}

void SdkHost::SetAppRule(const std::string& imagePath, bool includeInTunnel) {
  std::scoped_lock lock(mutex_);
  if (imagePath.empty()) return;
  try {
    // localState_ is the OFFLINE source of truth (persists, readable while
    // disconnected). device_ drives the LIVE tunnel when connected -
    // setBlockActionOverrides fires the change listener -> re-drives the driver.
    // Write both so the config is durable and applies immediately when up.
    if (localState_) {
      auto list = localState_->getBlockActionOverrides();
      if (!list) list = urnet::BlockActionOverrideList{};
      UrstUpsertAppRule(*list, imagePath, includeInTunnel);
      localState_->setBlockActionOverrides(list);
    }
    if (device_) {
      auto list = device_->getBlockActionOverrides();
      if (!list) list = urnet::BlockActionOverrideList{};
      UrstUpsertAppRule(*list, imagePath, includeInTunnel);
      device_->setBlockActionOverrides(list);
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: set app rule failed: {}", e.what());
  }
}

void SdkHost::RemoveAppRule(const std::string& imagePath) {
  std::scoped_lock lock(mutex_);
  if (imagePath.empty()) return;
  try {
    if (localState_) {
      if (auto list = localState_->getBlockActionOverrides()) {
        UrstRemoveAppRule(*list, imagePath);
        localState_->setBlockActionOverrides(list);
      }
    }
    if (device_) {
      if (auto list = device_->getBlockActionOverrides()) {
        UrstRemoveAppRule(*list, imagePath);
        device_->setBlockActionOverrides(list);
      }
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: remove app rule failed: {}", e.what());
  }
}

std::vector<AppRule> SdkHost::CurrentAppRules() {
  std::scoped_lock lock(mutex_);
  std::vector<AppRule> rules;
  try {
    // Read the offline source of truth so the sheet works while disconnected.
    std::optional<urnet::BlockActionOverrideList> list;
    if (localState_) list = localState_->getBlockActionOverrides();
    if (list) {
      for (const auto& over : *list) {
        if (!over.AppIds || over.AppIds->empty()) continue;  // app rules only
        AppRule rule;
        rule.imagePath = over.AppIds->front();
        rule.includeInTunnel = !(over.RouteOverride && over.RouteOverride->Local);
        rules.push_back(std::move(rule));
      }
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: current app rules failed: {}", e.what());
  }
  return rules;
}

// ---- reliability / developer surface ---------------------------------------
//
// The bridge ported for iOS (sdk#135) hangs off DeviceRemote, so every one of
// these works over the rpc with no tunnel — which is the whole point of the
// rpc-only mode.
//
// There used to be a "reachability holes" note here claiming dropExit,
// stallExit, shuffleExits and the probe-suite getters were DeviceLocal-only
// with NO DeviceRemote equivalent, so this app could not offer them. That was
// true when it was written and stopped being true when S1 landed — and it then
// sat here long enough to cost a later agent a scoping decision. All seven are
// declared on DeviceRemote (urnetwork_sdk.hpp:10114-10150) and exported
// (urnetwork_sdk.def:334-370). They are bridged below, under D6.

ReliabilitySnapshot SdkHost::ReadReliability() {
  // One lock hold for seven rpcs, deliberately: the alternative is seven holds,
  // and then the settings, the metrics and the two exit lists can come from
  // either side of a session teardown and disagree about which session they
  // describe. The cost is that a slow or unreachable service holds mutex_ for
  // the whole batch, and mutex_ is taken by UI-thread readers (RemoteConnected,
  // CurrentStats, SelectedLocation, ...). That is the shape the rest of this
  // class already has — those readers hold it across rpcs too — but this is the
  // biggest single hold in it, so it is the first thing to revisit if the
  // window is ever seen to stall while the developer screen is open.
  std::scoped_lock lock(mutex_);
  ReliabilitySnapshot snap;
  if (!device_) return snap;
  snap.haveDevice = true;
  try {
    snap.remoteConnected = device_->getRemoteConnected();
    snap.settings = device_->getReliabilitySettings();
    snap.metrics = device_->getReliabilityMetrics();
  } catch (const std::exception& e) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      LogWarn("sdkhost: read reliability state failed (logged once): {}", e.what());
  }
  // The two list-shaped getters: same null-unwrap hazard as everywhere else.
  {
    static std::atomic<bool> logged{false};
    if (auto exits = ReadSdkList(logged, "getExits", [&] { return device_->getExits(); }))
      snap.exits = std::move(*exits);
  }
  {
    static std::atomic<bool> logged{false};
    if (auto dst = ReadSdkList(logged, "getDestinationExits",
                            [&] { return device_->getDestinationExits(); }))
      snap.destinationExits = std::move(*dst);
  }
  // D6: probe-suite state, read in the SAME hold as the exits table it is shown
  // beside. probeSuiteRunning is a plain bool over the abi; getProbeResults is
  // list-shaped and gets the ReadSdkList guard like every other list here — a
  // suite that has never run returns a nil slice, which is the exact case that
  // marshals as the document `null` and throws type_error.302 on unwrap.
  try {
    snap.probeSuiteRunning = device_->probeSuiteRunning();
  } catch (const std::exception& e) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      LogWarn("sdkhost: probeSuiteRunning failed (logged once): {}", e.what());
  }
  {
    static std::atomic<bool> logged{false};
    if (auto results =
            ReadSdkList(logged, "getProbeResults", [&] { return device_->getProbeResults(); }))
      snap.probeResults = std::move(*results);
  }
  return snap;
}

std::optional<urnet::ReliabilitySettings> SdkHost::UpdateReliabilitySettings(
    const std::function<void(urnet::ReliabilitySettings&)>& mutate) {
  std::scoped_lock lock(mutex_);
  if (!device_ || !mutate) {
    // Say it. This is the path a developer-screen edit takes with no session,
    // and a silent nullopt here is indistinguishable from a write that landed.
    LogWarn("sdkhost: reliability settings write skipped: no device");
    return std::nullopt;
  }
  try {
    // FRESH read, every time. Not the snapshot the view is rendering: the whole
    // struct goes back, so anything the poll has not seen yet would be reverted.
    auto current = device_->getReliabilitySettings();
    if (!current) {
      // Nothing is in force — there is no multi client to override. Writing a
      // default-constructed struct here would install an all-zero override that
      // turns the entire reliability stack off, and sync() re-applies it. Do
      // nothing and say so.
      LogWarn("sdkhost: reliability settings write skipped: nothing in force "
              "(no multi client). Writing a zeroed struct here would disable "
              "the reliability stack.");
      return std::nullopt;
    }
    mutate(*current);
    device_->setReliabilitySettings(current);
    // Report what the device APPLIED, not what was asked for.
    return device_->getReliabilitySettings();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: reliability settings write failed: {}", e.what());
    return std::nullopt;
  }
}

ReliabilityActionResult SdkHost::RunReliabilityAction(ReliabilityAction action,
                                                      const std::string& exitClientId) {
  ReliabilityActionResult result;
  std::scoped_lock lock(mutex_);
  if (!device_) {
    LogWarn("sdkhost: reliability action skipped: no device");
    return result;
  }
  try {
    switch (action) {
      case ReliabilityAction::ResetMetrics:
        device_->resetReliabilityMetrics();
        LogInfo("sdkhost: reliability action: reset metrics");
        break;
      case ReliabilityAction::ResetSettings:
        device_->resetReliabilitySettings();
        LogInfo("sdkhost: reliability action: reset settings to shipped defaults");
        break;
      case ReliabilityAction::ProbeAllExits:
        // D6: the count DOES survive. DeviceRemote::probeAllExits is declared
        // int64_t (urnetwork_sdk.hpp:10140); the old comment here claimed the
        // export was void and dropped the number on the floor.
        result.count = device_->probeAllExits();
        result.hasCount = 0 <= result.count;
        result.declined = !result.hasCount;
        LogInfo("sdkhost: reliability action: probe all exits: {}",
                result.hasCount ? std::format("probed {}", result.count)
                                : std::format("declined by sdk (returned {})", result.count));
        break;
      case ReliabilityAction::SimulateNetworkChange:
        device_->simulateNetworkChange();
        LogInfo("sdkhost: reliability action: simulate network change");
        break;
      case ReliabilityAction::Sync:
        device_->sync();
        LogInfo("sdkhost: reliability action: sync");
        break;
      case ReliabilityAction::MigrateExit:
        if (exitClientId.empty()) {
          LogWarn("sdkhost: migrate exit skipped: no exit client id");
          return result;
        }
        // Same correction as probeAllExits: DeviceRemote::migrateExit is
        // int64_t (urnetwork_sdk.hpp:10122) and the count is the number of
        // flows moved off the exit, which is exactly what the view wanted.
        //
        // But a NEGATIVE return is the not-found sentinel, not a count. Against
        // an exit that is not in the window this returns -1, and reporting that
        // as "moved -1 flows" is a nonsense number presented as a measurement.
        // Observed live, against a real DeviceRemote. 0 stays a real answer.
        result.count = device_->migrateExit(exitClientId);
        result.hasCount = 0 <= result.count;
        result.declined = !result.hasCount;
        LogInfo("sdkhost: reliability action: migrate exit {}: {}", exitClientId,
                result.hasCount ? std::format("moved {} flows", result.count)
                                : std::format("declined by sdk (returned {})", result.count));
        break;
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: reliability action failed: {}", e.what());
    return result;
  }
  result.issued = true;
  return result;
}

// ---- D6: fault injection + the probe suite ---------------------------------
//
// Every one of these is ONE call under the lock with no retry and no queueing.
// See the contract on the declarations in SdkHost.h: a fault-injection action
// replayed after an RPC reconnect hits a different, healthy exit, which is the
// bug S1 fixed. If the rpc throws, that is reported and the action is over.
//
// Each logs the exit it acted on. Advanced Mode does not put a modal in front
// of these, so this log line is the record of what was done.

bool SdkHost::DropExit(const std::string& exitClientId) {
  std::scoped_lock lock(mutex_);
  if (!device_) {
    LogWarn("sdkhost: drop exit skipped: no device");
    return false;
  }
  if (exitClientId.empty()) {
    LogWarn("sdkhost: drop exit skipped: no exit client id");
    return false;
  }
  try {
    // The SDK's own bool: false means it declined (no such exit in the window,
    // or no multi client). That is NOT the same as "the rpc failed", but both
    // mean the exit was not dropped, so both report false to the caller.
    const bool dropped = device_->dropExit(exitClientId);
    LogInfo("sdkhost: fault injection: drop exit {}: {}", exitClientId,
            dropped ? "dropped" : "declined by sdk");
    return dropped;
  } catch (const std::exception& e) {
    LogWarn("sdkhost: drop exit {} failed: {}", exitClientId, e.what());
    return false;
  }
}

bool SdkHost::StallExit(const std::string& exitClientId, bool stalled) {
  std::scoped_lock lock(mutex_);
  if (!device_) {
    LogWarn("sdkhost: stall exit skipped: no device");
    return false;
  }
  if (exitClientId.empty()) {
    LogWarn("sdkhost: stall exit skipped: no exit client id");
    return false;
  }
  try {
    const bool applied = device_->stallExit(exitClientId, stalled);
    LogInfo("sdkhost: fault injection: {} exit {}: {}", stalled ? "stall" : "unstall", exitClientId,
            applied ? "applied" : "declined by sdk");
    return applied;
  } catch (const std::exception& e) {
    LogWarn("sdkhost: stall exit {} ({}) failed: {}", exitClientId, stalled, e.what());
    return false;
  }
}

void SdkHost::ShuffleExits() {
  std::scoped_lock lock(mutex_);
  if (!device_) {
    LogWarn("sdkhost: shuffle exits skipped: no device");
    return;
  }
  try {
    device_->shuffleExits();
    LogInfo("sdkhost: fault injection: shuffle exits (whole window)");
  } catch (const std::exception& e) {
    LogWarn("sdkhost: shuffle exits failed: {}", e.what());
  }
}

bool SdkHost::StartProbeSuite(const std::optional<urnet::ProbeSuiteConfig>& config) {
  std::scoped_lock lock(mutex_);
  if (!device_) {
    LogWarn("sdkhost: start probe suite skipped: no device");
    return false;
  }
  try {
    // A nullopt config means "use the SDK's default", and the SDK has a getter
    // for exactly that. Passing a default-CONSTRUCTED ProbeSuiteConfig instead
    // would be a suite with Concurrency 0 and TimeoutMillis 0 — the same class
    // of mistake as writing a zeroed ReliabilitySettings, which shipped once.
    auto effective = config ? config : urnet::getDefaultProbeSuiteConfig();
    const bool started = device_->startProbeSuite(effective);
    if (effective)
      LogInfo("sdkhost: probe suite start: {} (concurrency {}, timeout {}ms, dns {}, http {}, "
              "download {})",
              started ? "started" : "declined by sdk", effective->Concurrency,
              effective->TimeoutMillis, effective->IncludeDns, effective->IncludeHttp,
              effective->IncludeDownload);
    else
      LogInfo("sdkhost: probe suite start: {} (no config available)",
              started ? "started" : "declined by sdk");
    return started;
  } catch (const std::exception& e) {
    LogWarn("sdkhost: start probe suite failed: {}", e.what());
    return false;
  }
}

void SdkHost::StopProbeSuite() {
  std::scoped_lock lock(mutex_);
  if (!device_) {
    LogWarn("sdkhost: stop probe suite skipped: no device");
    return;
  }
  try {
    device_->stopProbeSuite();
    LogInfo("sdkhost: probe suite stop");
  } catch (const std::exception& e) {
    LogWarn("sdkhost: stop probe suite failed: {}", e.what());
  }
}

bool SdkHost::ProbeSuiteRunning() {
  std::scoped_lock lock(mutex_);
  if (!device_) return false;
  try {
    return device_->probeSuiteRunning();
  } catch (const std::exception& e) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true))
      LogWarn("sdkhost: probeSuiteRunning failed (logged once): {}", e.what());
    return false;
  }
}

std::vector<urnet::ProbeResult> SdkHost::GetProbeResults() {
  std::scoped_lock lock(mutex_);
  if (!device_) return {};
  static std::atomic<bool> logged{false};
  if (auto results =
          ReadSdkList(logged, "getProbeResults", [&] { return device_->getProbeResults(); }))
    return std::move(*results);
  return {};
}

// ---- location/provider chooser --------------------------------------------
// The bucketed location feed + the connected, provide-enabled peers pinned atop
// the chooser. The listeners fire on SDK callback threads and only marshal
// (never re-enter SdkHost), so pushing the initial snapshot under mutex_ here is
// safe.
//
// start() kicks the initial load (filterLocations("")) ON A GOROUTINE, so the
// seed read below ALWAYS comes back empty - measured against the shipped dll:
// getFilteredLocations() returns a NULL char* (-> std::nullopt) for ~1.2s, then
// the listener pushes LOCATIONS_LOADING with the document `null`, then
// LOCATIONS_LOADED with the buckets. NOTHING but the listener ever fills this
// pane. That is why every teardown of this feed MUST be paired with a re-open:
// a consumer that only reads the snapshot sees nothing, for good.
//
// The pairing is: ClosePresentationLocked() tears the feed down (window hidden
// OR DEACTIVATED - WindowPresentationShouldRun is `shown && activated`), and
// SetPresentationActive(true) + BootstrapSession put it back. Before that
// pairing existed, alt-tabbing away from the Network destination emptied the
// provider list permanently, because the only opener was a navigation change.
//
// ---- and the list does NOT need any of that ------------------------------
//
// Everything above is about the DEVICE feed, and the device feed is an
// OPTIMISATION, not the requirement. A provider list is not privileged
// information: GET /network/provider-locations answers 200 with no
// authorization, no device and no tunnel (measured: api.beta-test.net 1180
// bytes, api.bringyour.com 25939 bytes). Gating the pane on `device_` meant a
// user with no service running - the exact user who most wants to see what they
// could connect to - got one synthetic row and a sentence explaining that the
// list was unavailable. It was never unavailable.
//
// So `!device_` is no longer a reason to stop; it is a reason to use the OTHER
// source. api_ is built in Initialize() from the NetworkSpace and is alive from
// launch whether or not anything else is, and the chain
//
//     NetworkSpace -> Api -> getProviderLocations
//         -> getFilteredLocationsFromResult(result, query)
//
// returns the same PascalCase FilteredLocations document the view controller's
// listener delivers, so the whole UI below is unchanged.
//
// WHICH SOURCE WINS: the view controller, always, whenever it exists. It pushes
// live updates and owns server-side search; the api path is a cold snapshot.
// deviceFeedOpen_ is the gate, checked by the api path before every push.
void SdkHost::EnsureLocations() {
  std::scoped_lock lock(mutex_);
  EnsureLocationsLocked();
}

void SdkHost::EnsureLocationsLocked() {
  // caller holds mutex_
  if (!presentationActive_) return;
  if (!device_) {
    // No session. Not "no list" - see the block above.
    EnsureApiLocationsLocked();
    return;
  }
  if (locationsVc_) return;
  locationsVc_ = device_->openLocationsViewController();
  // Before start(), so the very first listener push cannot be preceded by a
  // stray api push landing on top of it.
  deviceFeedOpen_.store(true, std::memory_order_release);
  presentationSubs_.push_back(locationsVc_->addFilteredLocationsListener(
      [this](std::optional<urnet::FilteredLocations> locations, std::string state) {
        if (onLocationsObserver_) onLocationsObserver_(locations, state);
        if (onLocations_) onLocations_(std::move(locations), std::move(state));
      }));
  locationsVc_->start();
  // PeerViewController: connected AND provide-enabled peers only (SDK filters).
  peerVc_ = device_->openPeerViewController();
  presentationSubs_.push_back(peerVc_->addPeersListener(
      [this](std::optional<urnet::NetworkPeerList> peers) {
        if (onPeersObserver_) onPeersObserver_(peers);
        if (onPeers_) onPeers_(std::move(peers));
      }));
  peerVc_->start();
  // seed the chooser + the drawer's peer-count sub-label (the listeners only
  // fire on later changes)
  if (onLocations_ || onLocationsObserver_) {
    // FilteredLocations is struct-shaped, so by the Sdk.h rule it "cannot
    // throw" - but every one of its six fields IS a `*List`, and the guard is
    // the generated from_json's, not ours. Wrap it like every sibling getter
    // rather than depend on a third party's null handling staying as it is: it
    // was the ONLY list-bearing getter in this file left unguarded, and a throw
    // here is indistinguishable from an empty pane.
    static std::atomic<bool> logged{false};
    auto seedLocations =
        ReadSdkList(logged, "getFilteredLocations (seed)",
                    [&] { return locationsVc_->getFilteredLocations(); });
    auto seedState = locationsVc_->getFilteredLocationState();
    if (onLocationsObserver_) onLocationsObserver_(seedLocations, seedState);
    if (onLocations_) onLocations_(std::move(seedLocations), std::move(seedState));
  }
  if (onPeers_ || onPeersObserver_) {
    static std::atomic<bool> logged{false};
    auto seedPeers = ReadSdkList(logged, "getPeers (seed)", [&] { return peerVc_->getPeers(); });
    if (onPeersObserver_) onPeersObserver_(seedPeers);
    if (onPeers_) onPeers_(std::move(seedPeers));
  }
}

// ---- the no-device provider list ------------------------------------------
//
// A PORT OF LocationsViewController::FilterLocations onto the in-process Api.
// That function is fifteen lines of Go (sdk/locations_view_controller.go:135-193)
// and every one of them matters here, because the view controller is not doing
// anything a device is required for: it trims the query, dispatches to one of
// two Api endpoints on whether the query is empty, and buckets the answer with
// GetFilteredLocationsFromResult. All three of those are available to this
// process with no service running.
//
// The cache is deliberately NOT dropped when the presentation closes:
// alt-tabbing away and back must not empty the pane, and re-arming then finds
// the cache already good and does nothing at all.
void SdkHost::EnsureApiLocationsLocked() {
  // caller holds mutex_ (and must not hold apiLocationsMutex_)
  if (!api_) return;
  std::string query;
  uint64_t generation = 0;
  {
    std::scoped_lock lock(apiLocationsMutex_);
    // A fetch for this exact query is already on its way.
    if (apiLocationsInFlight_ && apiLocationsPendingQuery_ == apiLocationsQuery_) return;
    // The cache already answers this exact query. A LOCATIONS_ERROR cache is
    // deliberately NOT "good", so the next arming (re-entering the destination,
    // or the window coming back) is the retry - the SDK schedules none.
    if (apiLocations_ && apiLocationsState_ == urnet::LocationsLoaded &&
        apiLocationsLoadedQuery_ == apiLocationsQuery_)
      return;
    query = apiLocationsQuery_;
    apiLocationsPendingQuery_ = query;
    apiLocationsInFlight_ = true;
    apiLocationsState_ = urnet::LocationsLoading;
    generation = ++apiLocationsGeneration_;
  }
  // Say "loading" now rather than leave the pane in whatever state the last
  // arming left it in. The PREVIOUS result stays on screen underneath, which is
  // also what the view controller does - it pushes its existing snapshot with
  // the LOADING state rather than blanking (locations_view_controller.go:153).
  PublishApiLocations();
  // `this` outlives every callback: SdkHost is owned by AppController for the
  // life of the process, which is the same capture every other api_ call in
  // this file makes.
  auto done = [this, generation, query](std::optional<urnet::FindLocationsResult> result,
                                        std::optional<std::string> err) {
    {
      std::scoped_lock lock(apiLocationsMutex_);
      // A newer query superseded this one; its answer is the current one.
      if (generation != apiLocationsGeneration_) return;
      apiLocationsInFlight_ = false;
      if ((err && !err->empty()) || !result) {
        // Keep the last good cache if there is one - a failed search must not
        // wipe a list that is still perfectly serviceable - and record the
        // failure so an EMPTY pane can say why it is empty.
        apiLocationsState_ = urnet::LocationsError;
        LogWarn("sdkhost: provider locations fetch failed (query='{}'): {}", query,
                err && !err->empty() ? *err : std::string("no result"));
      } else {
        apiLocations_ = std::move(result);
        // The buckets must be computed with the query the RESULT answers, not
        // with whatever the search box says now.
        apiLocationsLoadedQuery_ = query;
        apiLocationsState_ = urnet::LocationsLoaded;
      }
    }
    PublishApiLocations();
  };
  if (query.empty()) {
    api_->getProviderLocations(done);
  } else {
    urnet::FindLocationsArgs args;
    args.query = query;
    api_->findProviderLocations(args, done);
  }
}

std::optional<urnet::FilteredLocations> SdkHost::FilteredApiLocationsLocked() {
  // caller holds apiLocationsMutex_
  if (!apiLocations_) return std::nullopt;
  static std::atomic<bool> logged{false};
  return ReadSdkList(logged, "getFilteredLocationsFromResult", [&] {
    return urnet::getFilteredLocationsFromResult(apiLocations_, apiLocationsLoadedQuery_);
  });
}

void SdkHost::PublishApiLocations() {
  // THE SINGLE-WRITER GATE. See the field comment: while the view controller is
  // open it is the only writer, and this path says nothing.
  if (deviceFeedOpen_.load(std::memory_order_acquire)) return;
  std::optional<urnet::FilteredLocations> buckets;
  std::string state;
  {
    std::scoped_lock lock(apiLocationsMutex_);
    state = apiLocationsState_;
    buckets = FilteredApiLocationsLocked();
  }
  // Handlers are invoked with NO lock held: they marshal to the UI thread, and
  // the UI thread reaches back in through CurrentFilteredLocations().
  if (onLocationsObserver_) onLocationsObserver_(buckets, state);
  if (onLocations_) onLocations_(std::move(buckets), std::move(state));
}

void SdkHost::SetLocationFilter(const std::string& query) {
  {
    std::scoped_lock lock(mutex_);
    // The device feed owns the search when it exists: it re-buckets server-side
    // and pushes the result back through the same listener.
    if (locationsVc_) {
      locationsVc_->filterLocations(query);
      return;
    }
  }
  // No view controller. Record the desired query and let EnsureApiLocations run
  // the same two-endpoint dispatch the view controller runs; see the header.
  // Trimmed exactly as FilterLocations trims (locations_view_controller.go:137),
  // so a box holding only spaces is the idle list and not a search for " ".
  // Trimmed HERE rather than via the pages:: helper: SdkHost must not take a
  // dependency on the UI layer for four lines of whitespace handling.
  std::string trimmed = query;
  {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    trimmed.erase(trimmed.begin(),
                  std::find_if(trimmed.begin(), trimmed.end(), notSpace));
    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), notSpace).base(),
                  trimmed.end());
  }
  {
    std::scoped_lock lock(apiLocationsMutex_);
    if (apiLocationsQuery_ == trimmed) return;
    apiLocationsQuery_ = trimmed;
  }
  // Not held across the block above: lock order is mutex_ -> apiLocationsMutex_.
  std::scoped_lock lock(mutex_);
  EnsureApiLocationsLocked();
}

std::optional<urnet::FilteredLocations> SdkHost::CurrentFilteredLocations() {
  {
    std::scoped_lock lock(mutex_);
    if (locationsVc_) {
      static std::atomic<bool> logged{false};
      return ReadSdkList(logged, "getFilteredLocations",
                         [&] { return locationsVc_->getFilteredLocations(); });
    }
  }
  std::scoped_lock lock(apiLocationsMutex_);
  return FilteredApiLocationsLocked();
}

std::string SdkHost::CurrentFilteredLocationState() {
  {
    std::scoped_lock lock(mutex_);
    if (locationsVc_) return locationsVc_->getFilteredLocationState();
  }
  std::scoped_lock lock(apiLocationsMutex_);
  return apiLocationsState_;
}

std::optional<urnet::NetworkPeerList> SdkHost::ConnectedProvidePeers() {
  std::scoped_lock lock(mutex_);
  if (peerVc_) {
    static std::atomic<bool> logged{false};
    return ReadSdkList(logged, "getPeers", [&] { return peerVc_->getPeers(); });
  }
  return std::nullopt;
}

int64_t SdkHost::ConnectedPeerCount() {
  std::scoped_lock lock(mutex_);
  // ALL connected peers, whether or not they provide â€” the "You have {n}
  // other devices online" count (connecting still requires provide, which is
  // what ConnectedProvidePeers captures)
  if (peerVc_) return static_cast<int64_t>(peerVc_->getConnectedCount());
  return 0;
}

bool SdkHost::RemoteConnected() {
  std::scoped_lock lock(mutex_);
  if (device_) return device_->getRemoteConnected();
  return false;
}

std::optional<urnet::ConnectLocation> SdkHost::SelectedLocation() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) return connectVc_->getSelectedLocation();
  if (device_) return device_->getConnectLocation();
  return std::nullopt;
}

// ---- selection identity (shared: sheet, Network pane, row coalescer) -------
// Declared in SdkHost.h; the narrative is there. The bodies moved verbatim
// from LocationSheets.cpp's anonymous namespace.

bool SameId(std::optional<std::string> const& a, std::optional<std::string> const& b) {
  return a && b && !a->empty() && *a == *b;
}

bool IsBestAvailableSelected(std::optional<urnet::ConnectLocation> const& selected) {
  return !selected || (selected->connect_location_id &&
                       selected->connect_location_id->best_available.value_or(false));
}

bool IsLocationSelected(std::optional<urnet::ConnectLocation> const& selected,
                        urnet::ConnectLocation const& loc) {
  if (!selected || !selected->connect_location_id || !loc.connect_location_id) return false;
  const auto& a = *selected->connect_location_id;
  const auto& b = *loc.connect_location_id;
  return SameId(a.location_id, b.location_id) || SameId(a.client_id, b.client_id) ||
         SameId(a.location_group_id, b.location_group_id);
}

// ---- connect: bring the tunnel up, then pick providers ---------------------
//
// See the block on these three in SdkHost.h. Each records an intent and returns
// immediately; the session worker below does the work.

bool SdkHost::AdmitStartConnect(const char* what, std::function<void()> again) {
  // the balance recovery's retry decided on a fresh balance already, and it is
  // not a new gesture, so the observer is not told either
  if (!startConnectFacts_ || retryingRefusedConnect_) return true;
  struct Sinks {
    SdkHost& host;
    const char* what;
    std::function<void()>& again;
    void Upgrade() {
      LogInfo("sdkhost: '{}' blocked: out of balance, showing the upgrade path", what);
      // the refused gesture goes along: it waits on the balance to run again
      if (host.startConnectUpgrade_) host.startConnectUpgrade_(again);
    }
    void FetchBalance() {
      LogInfo("sdkhost: '{}' waits for a fresh balance", what);
      if (host.startConnectFetchBalance_) {
        host.startConnectFetchBalance_(std::move(again));
      } else if (again) {
        again();
      }
    }
  } sinks{*this, what, again};
  const bool admitted = urnw::balance::AdmitStartConnect(startConnectFacts_(), sinks);
  if (admitted && connectAdmitted_) connectAdmitted_();
  return admitted;
}

void SdkHost::RetryRefusedConnect(const std::function<void()>& connect) {
  if (!connect) return;
  retryingRefusedConnect_ = true;
  connect();
  retryingRefusedConnect_ = false;
}

void SdkHost::ConnectBestAvailable() {
  if (!AdmitStartConnect("connect (best available)", [this] { ConnectBestAvailable(); })) return;
  SessionRequest r;
  r.kind = ConnectKind::BestAvailable;
  r.reason = "connect (best available)";
  RequestSession(std::move(r));
}

void SdkHost::Connect(const std::string& connectLocationJson) {
  if (!AdmitStartConnect("connect (location)",
                         [this, connectLocationJson] { Connect(connectLocationJson); })) {
    return;
  }
  SessionRequest r;
  try {
    r.location =
        nlohmann::json::parse(connectLocationJson).get<urnet::ConnectLocation>();
  } catch (const std::exception& e) {
    // A press that cannot even name a destination is not a session problem, and
    // must not start one. Reported rather than swallowed: the button has already
    // flipped to "Connecting" by the time this runs.
    LogWarn("sdkhost: connect parse failed: {}", e.what());
    std::scoped_lock lock(mutex_);
    PublishSessionFailure("that provider location could not be read");
    PublishStats();
    return;
  }
  r.kind = ConnectKind::Location;
  r.reason = "connect (location)";
  RequestSession(std::move(r));
}

// Connect to an SDK-supplied ConnectLocation as-is (the chooser already holds
// the typed struct; skip the json round-trip). connect() takes an optional.
void SdkHost::Connect(const urnet::ConnectLocation& location) {
  if (!AdmitStartConnect("connect (location)", [this, location] { Connect(location); })) return;
  SessionRequest r;
  r.kind = ConnectKind::Location;
  r.location = location;
  r.reason = "connect (location)";
  RequestSession(std::move(r));
}

// ---- row clicks: the same connects, coalesced ------------------------------
// The rules and the reason are on the declarations in SdkHost.h. In short: a
// row click's intent settles for kRowClickSettle before the worker acts, a
// later click replaces it, re-clicking the current target is a no-op, and any
// immediate request (Disconnect, the connect button, the tray) supersedes a
// settling intent through the ordinary last-request-wins slot.

namespace {
// ~1.2s: long enough to absorb a scroll-and-click hunt through the location
// list, short enough that a single deliberate click still feels acted on. The
// SDK charges 100ms-1s of shared dial budget per provider dial and refunds
// nothing on cancellation, so every click this absorbs is up to ~10s of
// staircase debt (one window's worth of dials) that never gets incurred.
constexpr auto kRowClickSettle = std::chrono::milliseconds(1200);
}  // namespace

bool SdkHost::RowClickIsCurrent(
    const std::function<bool(const std::optional<urnet::ConnectLocation>&)>& matches) {
  // No session, or no presentation-scoped controller: nothing can be current.
  // (Lock-free by design — see the declaration. The same unguarded connectVc_
  // read ReadStats has always done from these threads.)
  if (!connectVc_) return false;
  // Active means the SDK is driving at the selection NOW. A re-click during
  // CONNECTING must coalesce away just like one during CONNECTED — the whole
  // point is not to restart a window build that is already in progress — but
  // after a Disconnect the selection survives as a preference, and clicking it
  // then is a genuine "connect me again".
  const std::string status = connectVc_->getConnectionStatus();
  const bool active =
      status == "CONNECTED" || status == "CONNECTING" || status == "DESTINATION_SET";
  if (!active) return false;
  return matches(connectVc_->getSelectedLocation());
}

void SdkHost::CancelPendingRowConnect(const char* why) {
  std::scoped_lock lock(pendingMutex_);
  if (pendingRequested_ && pending_.coalesced) {
    LogInfo("sdkhost: pending '{}' cancelled ({})", pending_.reason, why);
    pending_ = SessionRequest{};
    pendingRequested_ = false;
    // A worker may be sleeping toward the cancelled intent's deadline; wake it
    // so it sees the empty slot and exits instead of oversleeping.
    pendingCv_.notify_all();
  }
}

void SdkHost::ConnectFromRow(const urnet::ConnectLocation& location) {
  // Current means the same location reached the same way: a device picked from
  // the peer list before peer rows set network_peer is still a public exit, and
  // tapping it again reconnects it as a network peer (PeerLocation.h).
  if (RowClickIsCurrent(
          [&](const std::optional<urnet::ConnectLocation>& sel) {
            return IsLocationSelected(sel, location) && SameNetworkPeer(sel, location);
          })) {
    // Already there. The only work left is un-queuing a newer intent, so a
    // "click B, regret it, click A again" round trip ends with zero rebuilds.
    CancelPendingRowConnect("re-selected the current location");
    return;
  }
  if (!AdmitStartConnect("connect (row click)", [this, location] { ConnectFromRow(location); })) {
    return;
  }
  SessionRequest r;
  r.kind = ConnectKind::Location;
  r.location = location;
  r.reason = "connect (row click)";
  r.coalesced = true;
  r.notBefore = std::chrono::steady_clock::now() + kRowClickSettle;
  RequestSession(std::move(r));
}

void SdkHost::ConnectBestAvailableFromRow() {
  if (RowClickIsCurrent([](const std::optional<urnet::ConnectLocation>& sel) {
        return IsBestAvailableSelected(sel);
      })) {
    CancelPendingRowConnect("re-selected best available");
    return;
  }
  if (!AdmitStartConnect("connect (row click, best available)",
                         [this] { ConnectBestAvailableFromRow(); })) {
    return;
  }
  SessionRequest r;
  r.kind = ConnectKind::BestAvailable;
  r.reason = "connect (row click, best available)";
  r.coalesced = true;
  r.notBefore = std::chrono::steady_clock::now() + kRowClickSettle;
  RequestSession(std::move(r));
}

void SdkHost::EnsureSession(const char* reason, bool automaticRecovery) {
  SessionRequest r;
  r.kind = ConnectKind::None;
  r.reason = reason;
  r.automaticRecovery = automaticRecovery;
  RequestSession(std::move(r));
}

// ---- the session worker ----------------------------------------------------

void SdkHost::RequestSession(SessionRequest request) {
  // ONLY pendingMutex_ HERE. This runs on the UI thread (a Connect press) and
  // mutex_ is held by the worker across a whole BootstrapSession — service
  // connect, hello, start_tunnel, up to the 30 s pipe timeout. A press that
  // waited on that is a frozen window, which is the failure this app has
  // already paid for twice (see IsLoggedIn's comment).
  std::scoped_lock lock(pendingMutex_);
  // The tray's Quit closed the slot (Quit). What it stopped in the service
  // must not be started again by a request that lands after it: the failsafe
  // edge, a pipe drop's recovery, a setting saved on the way out.
  if (quitting_.load()) {
    LogInfo("sdkhost: '{}' dropped: the app is quitting", request.reason);
    return;
  }
  // LAST REQUEST WINS. Two presses in a row, or a press while a bootstrap is
  // running, must not queue two start_tunnels — they must land on one session
  // and the destination the user chose most recently.
  //
  // ONE exception, born with the row-click settle window: a bare "make sure a
  // session exists" (kind None — the resume path, a network-server change, the
  // service watchdog) must not REPLACE a pending connect. Every connect
  // creates the session it needs, so the ensure is already implied by what is
  // sitting in the slot, and replacing would throw the user's destination
  // choice away for a request that wanted strictly less. Before the settle
  // window this race was microseconds wide; at 1.2s of deliberate delay it
  // would be a click the watchdog eats.
  //
  // A provider reconcile (kind Provider) wants less still, and is covered by
  // any pending request: every pass that leaves no session ends with the same
  // reconcile, and a pass that builds one hands providing to its device. It
  // never covers anything itself — an ensure replaces it.
  const bool covered =
      pendingRequested_ &&
      ((request.kind == ConnectKind::None && pending_.kind != ConnectKind::None &&
        pending_.kind != ConnectKind::Provider) ||
       (request.kind == ConnectKind::Provider && pending_.kind != ConnectKind::Provider));
  if (covered) {
    LogInfo("sdkhost: '{}' is covered by the pending '{}'", request.reason,
            pending_.reason);
  } else {
    pending_ = std::move(request);
  }
  pendingRequested_ = true;
  // Wake a worker that is sleeping out a settle deadline: the replacement may
  // be IMMEDIATE (a Disconnect, an explicit connect) and must not wait behind
  // the deadline of the row click it just superseded.
  pendingCv_.notify_all();
  // sessionWorkerAlive_ is read and written ONLY under this lock, by both the
  // producer here and the worker as it exits, so a request that arrives while
  // the worker is finishing cannot fall between them.
  if (sessionWorkerAlive_) {
    if (!covered) {
      LogInfo("sdkhost: '{}' folded into the session start already in flight",
              pending_.reason);
    }
    return;
  }
  sessionWorkerAlive_ = true;
  std::thread([this] { SessionWorkerLoop(); }).detach();
}

void SdkHost::SessionWorkerLoop() {
  for (;;) {
    SessionRequest req;
    {
      std::unique_lock<std::mutex> lock(pendingMutex_);
      // The row-click settle: a coalesced request is not consumed before its
      // deadline. Loop rather than a single wait — a replacement can land with
      // a LATER deadline (the next click of a burst) or an earlier one (an
      // immediate Disconnect), and a cancel can empty the slot entirely, so
      // every wakeup re-reads the slot from scratch. Immediate requests carry
      // an epoch deadline and fall straight through.
      while (pendingRequested_ &&
             std::chrono::steady_clock::now() < pending_.notBefore) {
        pendingCv_.wait_until(lock, pending_.notBefore);
      }
      if (!pendingRequested_) {
        sessionWorkerAlive_ = false;
        return;
      }
      req = std::move(pending_);
      pending_ = SessionRequest{};
      pendingRequested_ = false;
    }

    // Not a session request: keep the provider-only device in step and nothing
    // else — no gesture, no bootstrap, no attach (ReconcileProviderLocked).
    if (req.kind == ConnectKind::Provider) {
      {
        std::scoped_lock lock(mutex_);
        // An owed sign-out first, in every pass (SignOut.h).
        SettleSignOutLocked(req.reason);
        ReconcileProviderLocked(req.reason);
      }
      PublishStats();
      continue;
    }

    bool ok = false;
    {
      std::scoped_lock lock(mutex_);
      // An owed sign-out first, before the pass reads or changes anything: the
      // bootstrap and the reconcile below start nothing while it is owed.
      SettleSignOutLocked(req.reason);
      // "Is there a session" is device_ AND a live control channel, not device_
      // alone. A DeviceRemote whose service process has exited still exists and
      // still answers its cached getters — connecting into one is the "hero
      // stays green over a tunnel that is gone" failure, from the other side.
      // Drop it and build a real one.
      //
      // Kept ahead of the decision below rather than folded into it: with the
      // pipe gone there is nothing left to stop (the adapter and the dynamic
      // WFP session died with the process that held them), so this is the one
      // teardown that must NOT try to send a stop_tunnel down a dead channel.
      if (device_ && !service_.IsConnected()) {
        LogWarn("sdkhost: the session's service is gone; tearing the dead "
                "DeviceRemote down before starting a new session ({})",
                req.reason);
        try {
          TeardownSessionLocked();
        } catch (const std::exception& e) {
          LogWarn("sdkhost: teardown of the dead session failed: {}", e.what());
        }
      }

      // ---- WHAT THIS GESTURE ACTUALLY HAS TO DO ------------------------------
      //
      // This block used to be `if (device_) { ok = true; }` — a pointer standing
      // in for "there is a tunnel". It is not one: the tray escape hatch and any
      // service-side stop destroy the service's DeviceLocal and its mTLS
      // listener while leaving this side's DeviceRemote perfectly constructed,
      // so Connect re-issued connectBestAvailable() into a listener that no
      // longer existed and never sent start_tunnel. Ask the SERVICE instead —
      // once per gesture, one rpc — and let the pure table in
      // Common/ConnectAction.h say what follows. Every row of that table is
      // pinned by the service selftest.
      bool answered = false;
      const proto::TunnelStatus svc = CurrentServiceStatusLocked(answered);
      gesture::ServiceFacts facts;
      facts.pipeUp = service_.IsConnected();
      // FROM THE TRANSPORT, NEVER FROM A PAYLOAD FIELD. This was
      // `!svc.service_version.empty()`, and that field is urnet::version(),
      // which is EMPTY in this SDK build — the service's own startup line logs
      // `sdk=` with nothing after it. So `known` was false on every gesture,
      // every Connect took the unknown-fallback ("keep the session we have"),
      // and a Connect after a tray force-stop still never sent start_tunnel.
      // The fallback is a good fallback; it just must not be the normal path.
      facts.known = facts.pipeUp && answered;
      facts.state = svc.state;
      facts.mode = svc.mode;
      facts.routesInstalled = svc.routes_installed;
      facts.wfpState = svc.wfp_state;
      facts.stopReason = svc.stop_reason;
      if (facts.known) AdoptServiceFacts(svc);

      gesture::AppFacts app;
      app.haveDevice = device_.has_value();
      // The PERSISTED preference, read locally — deliberately NOT
      // CurrentKillSwitch(), which prefers the DeviceRemote and would put an
      // rpc to a possibly-dead listener on the one path that must not block.
      // Decide never reads this field (the SERVICE owns the arming decision,
      // and the table pins that no arming intent lives on this side); it is
      // carried so a plan can be explained against the setting the user chose.
      try {
        if (localState_) app.killSwitch = !localState_->getRouteLocal();
      } catch (const std::exception&) {
        app.killSwitch = false;  // no state at all: the permissive default
      }
      app.wantsTunnel = requestedMode_ == proto::StartMode::Tunnel;
      // RECORDED BEFORE THE DECISION, from the gesture itself, because the
      // decision is what consumes it. A Connect of either shape IS the user
      // asking, so it clears the flag ahead of its own Decide; a Disconnect
      // (including the one the tray's escape hatch queues) sets it.
      // EnsureSession leaves it alone — "make sure a session exists" is the
      // resume path, a network-space change and the service-reconnect watchdog,
      // none of which is anybody asking for a tunnel.
      const gesture::Gesture g = GestureOf(req.kind);
      if (g == gesture::Gesture::Connect || g == gesture::Gesture::ConnectRow)
        userDisconnected_.store(false);
      else if (g == gesture::Gesture::Disconnect)
        userDisconnected_.store(true);
      app.userDisconnected = userDisconnected_.load();
      {
        // mutex_ -> healthMutex_, the order ConnectLocked already establishes.
        std::scoped_lock healthLock(healthMutex_);
        app.health = healthTracker_.Current();
      }

      const gesture::Plan plan = gesture::Decide(g, facts, app);
      LogInfo("sdkhost: '{}' -> {} (service: state={} routes={} wfp={}{}; app: "
              "device={})",
              req.reason, plan.why, proto::ToString(facts.state),
              facts.routesInstalled ? "yes" : "no", svc.wfp_state,
              facts.known ? "" : " — NOT READ, the service did not answer",
              app.haveDevice ? "yes" : "no");

      // PHASE 1, AND IT IS FIRST FOR THE REASON THE SERVICE'S OWN TEARDOWN IS
      // ORDERED THIS WAY. Giving the machine back — routes, dns, resolver
      // cache, firewall policy — is local, cheap and the only part the user
      // experiences as "my internet is back". Everything below it can block on
      // the SDK. This single call is the whole of the owner's bug A: the
      // service has had a correct two-phase stop all along and the app simply
      // never invoked it from Disconnect.
      if (plan.stopTunnel) {
        const proto::TunnelStatus stopped = service_.StopTunnel();
        AdoptServiceFacts(stopped);
        LogInfo("sdkhost: the service tunnel is stopped (state={} routes={} "
                "wfp={}) — this machine's routes, dns and firewall policy are "
                "back",
                proto::ToString(stopped.state),
                stopped.routes_installed ? "STILL INSTALLED" : "reverted",
                stopped.wfp_state);
      }
      // The SDK side, second. ConnectLocked is where #27's NoteNewAttempt lives,
      // so a deliberate disconnect still ends the health attempt exactly as it
      // always did.
      if (plan.sdkDisconnect && device_) {
        SessionRequest stop;
        stop.kind = ConnectKind::Disconnect;
        stop.reason = req.reason;
        ConnectLocked(stop);
      }
      // stopTunnel=false: either we just sent the stop ourselves (above), or
      // this is a Connect dropping a stale DeviceRemote, where a deliberate stop
      // would drop the firewall policy to Off for the length of the bring-up
      // that follows — the exact gap a kill switch exists to cover.
      if (plan.tearDownDevice && device_) {
        try {
          TeardownSessionLocked(/*stopTunnel=*/false);
        } catch (const std::exception& e) {
          LogWarn("sdkhost: teardown of the stale session failed: {}", e.what());
        }
      }

      if (plan.startTunnel) {
        // D8: a bare "make sure a session exists" (resume, a network-server
        // change, the service watchdog) may only ATTACH to a session that is
        // already running — nobody gestured, so nobody gets a tunnel. The
        // decision table still routes EnsureSession into the bootstrap
        // (deliberately: the bootstrap is what dials the pipe, adopts the
        // service's facts and reports "service unreachable" — see the pinned
        // "CONNECT still runs the bootstrap with the pipe down" row), and the
        // bootstrap itself declines the cold start. The "starting a session"
        // line now lives at the ONE start_tunnel site inside it, so a start
        // that logs no reason cannot exist.
        const bool attachOnly = g == gesture::Gesture::EnsureSession;
        ok = BootstrapSession(req.reason, attachOnly);
      } else {
        ok = device_.has_value();
      }

      const bool connecting = req.kind == ConnectKind::BestAvailable ||
                              req.kind == ConnectKind::Location;
      if (ok) {
        serviceRecoveryNeeded_.store(false, std::memory_order_release);
        watchdogCv_.notify_all();
        if (plan.sdkConnect && req.kind != ConnectKind::None) ConnectLocked(req);
        // An rpc-only session is live and driveable and carries NOTHING. On a
        // Connect press that is the answer to "why did pressing this change
        // nothing", so re-raise the standing notice rather than let the press
        // land in silence. PublishModeNotice wants mutex_, which we hold.
        if (connecting && sessionMode_.load() == proto::StartMode::RpcOnly) {
          PublishModeNotice();
        }
      } else if (!plan.startTunnel) {
        // Nothing was attempted and nothing failed — a disconnect that found
        // nothing to disconnect from, which is a legitimate outcome, not an
        // error. Fall through to the stats push, which is what puts the button
        // back to its idle label.
      } else if (bootstrapDeclined_) {
        serviceRecoveryNeeded_.store(false, std::memory_order_release);
        watchdogCv_.notify_all();
        // D8: the attach-only bootstrap found nothing to reattach to and, by
        // the click-only rule, started nothing. NOT a failure and NOT a
        // notice: the machine is exactly as the user left it, the app renders
        // Disconnected honestly (the stats push below), and the next Connect
        // press is what changes it. The bootstrap already logged the decline
        // with its reason.
      } else {
        // EVERY failing path says why, on the channel built for it. Under the
        // lock: PublishSessionFailure writes sessionFailure_, which mutex_
        // guards, and the handlers it invokes only marshal (see the threading
        // note on SetModeNoticeHandler).
        const std::string why = bootstrapError_;
        if (req.automaticRecovery) {
          LogWarn("sdkhost: quiet automatic recovery attempt failed: {}",
                  why.empty() ? "unknown" : why);
        } else {
          LogError("sdkhost: '{}' could not start a session: {}", req.reason,
                   why.empty() ? "unknown" : why);
          PublishSessionFailure(why);
        }
        // ...and start watching, if the reason was that there is no service to
        // talk to. THE WATCHDOG CANNOT ONLY ARM ON A DROP: the commonest way
        // into this state is a launch that found no service at all, which is
        // never a "drop" because there was never a connection. That is the
        // state this machine boots into every time (the service is started by
        // hand), and without this the app would sit signed-in and sessionless
        // until something asked it again.
        //
        // A missing pipe and a listening-but-incompatible pipe are both
        // recoverable during an installer swap. The watchdog is quiet and
        // exponentially backed off, so keeping it alive cannot spam the user.
        if (bootstrapServiceRetryable_) {
          ScheduleServiceRetry();
        } else {
          serviceRecoveryNeeded_.store(false, std::memory_order_release);
          watchdogCv_.notify_all();
        }
      }

      // Keep providing while disconnected. A pass that leaves no session — a
      // Disconnect (whose stop_tunnel took the provider down with the tunnel),
      // a launch, network-server change or service recovery that found nothing
      // to reattach to, a Connect that failed — hands providing to the
      // service's provider-only device, which installs nothing on this machine
      // (ReconcileProviderLocked). With a session, its own device provides.
      if (!device_) ReconcileProviderLocked(req.reason);
    }
    // OUTSIDE the lock. On failure this is what takes the connect button off
    // "Connecting": with no session there is no listener to push a correcting
    // status, so without it the hero says Connecting for the life of the window.
    // On success it seeds the first snapshot for a window that is not presenting
    // (SubscribeStats only runs when it is).
    PublishStats();
  }
}

// See the contract in the header. ONE rpc, and it is the only admissible answer
// to "is there a tunnel right now" — not device_, not sessionMode_, not the
// cached mirror the tray reads. It cannot lie, because every field of the reply
// is read off the object that OWNS the machine state, inside the process that
// holds it (TunnelController::Status).
proto::TunnelStatus SdkHost::CurrentServiceStatusLocked(bool& answered) {
  answered = false;
  if (!service_.IsConnected()) return {};
  const proto::TunnelStatus st = service_.GetState(&answered);
  if (!answered) {
    // ServiceClient::CallStatus swallows the throw and hands back a default
    // status, which reads as "nothing is running" — and acting on that would
    // tear a healthy tunnel down over one dropped reply. Say so; the decision
    // treats it as unknown and keeps whatever it has.
    //
    // Read off the TRANSPORT, not off a field of the reply: every payload
    // field has a legitimate default that is indistinguishable from silence.
    LogWarn("sdkhost: get_state did not answer ({}). Treating the service's "
            "state as UNKNOWN rather than as 'nothing is installed'.",
            st.error.empty() ? "no error reported" : st.error);
  }
  return st;
}

// ---- keep providing while disconnected --------------------------------------
//
// See the contract in the header and Common/ProvideLifecycle.h.

void SdkHost::RequestProviderReconcile(const char* reason) {
  SessionRequest r;
  r.kind = ConnectKind::Provider;
  r.reason = reason;
  RequestSession(std::move(r));
}

void SdkHost::ReconcileProviderLocked(const char* reason) {
  // caller holds mutex_
  //
  // A session's own device provides — the tunnel's, or an rpc-only one's — and
  // a DeviceRemote whose session the service no longer runs is the next
  // gesture's to drop (gesture::Decide), not this function's.
  if (device_ || !localState_) return;
  // Quitting: Quit stops the provider-only device next, under this same lock,
  // and a pass that was already running when it began must not start one first.
  if (quitting_.load()) return;
  std::string clientJwt;
  std::string instanceId;
  // Signed out, nothing provides: a provider an earlier run left for another
  // space's account is stopped like any mode that does not provide. Signed out
  // is loggedIn_, not only an empty stored jwt: a sign-out's local logout lands
  // asynchronously (Logout), and a pass in that gap must not start a provider
  // for the account that just left.
  const bool signedIn = loggedIn_.load(std::memory_order_acquire);
  std::string mode = "never";
  try {
    clientJwt = localState_->getByClientJwt();
    instanceId = localState_->getInstanceId();
    if (signedIn && !clientJwt.empty() && !instanceId.empty())
      mode = localState_->getProvideControlMode();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: provide: reading the stored provide mode failed: {}", e.what());
    return;
  }
  const provide::ControlMode controlMode = provide::ControlModeFrom(mode);
  // Nothing runs and nothing should: ask the service nothing.
  if (!provide::ProviderRuns(controlMode, /*connected=*/false) &&
      serviceProviderKnown_.load() && !serviceProviderRunning_.load()) {
    return;
  }
  // No service, no provider: it died with the process, and the watchdog's
  // recovery pass comes back through here.
  if (!service_.IsConnected()) return;
  bool answered = false;
  const proto::TunnelStatus st = CurrentServiceStatusLocked(answered);
  if (!answered) return;
  AdoptServiceFacts(st);
  const provide::DisconnectedStep step =
      provide::DisconnectedProviderStep(mode, proto::ProviderFactsFrom(st, answered));
  if (step == provide::DisconnectedStep::None) return;

  std::optional<proto::TunnelStatus> after;
  std::string error;
  if (step == provide::DisconnectedStep::Stop) {
    const bool stopped = service_.StopProvider(&after, &error);
    if (after) AdoptServiceFacts(*after);
    if (stopped) {
      LogInfo("sdkhost: provide: stopped the provider-only device ({}, mode {})", reason,
              provide::ToString(controlMode));
    } else {
      LogWarn("sdkhost: provide: stop_provider failed ({}): {}", reason,
              error.empty() ? "no detail" : error);
    }
    return;
  }

  // A sign-out the service has not done yet: its device identity may still be
  // the old account's, or the old account may still be providing on it. The
  // pass delivered what it could; the watchdog keeps trying (SignOut.h).
  if (signOut_.Owed()) {
    LogWarn("sdkhost: provide: not starting the provider-only device ({}): a sign-out "
            "is still owed to the service",
            reason);
    return;
  }
  // Start, with the whole request every time: the service keeps the device it
  // runs for an identical request and only applies the mode, and builds a new
  // one for a changed jwt, space or provider transport policy.
  proto::StartProvider request;
  request.by_jwt = clientJwt;
  request.instance_id = instanceId;
  request.device_description = DeviceDescription();
  request.device_spec = DeviceSpec();
  request.app_version = appVersion_;
  request.provide_mode = mode;
  // Applied by the service in place for a device it keeps, and before one it
  // builds.
  const netcountry::Reading networkCountry = CurrentNetworkCountry();
  request.network_country_code = networkCountry.code;
  request.network_country_source = networkCountry.source;
  try {
    request.network_space_json = networkSpace_->toJson();
    // The mirror BootstrapSession seeds a tunnel session's device from, sent
    // only when there is one, for the same reason.
    if (auto settings = localState_->getProviderTransportSettings()) {
      request.provider_transport_settings_json = nlohmann::json(*settings).dump();
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: provide: building the provider request failed: {}", e.what());
    return;
  }
  const bool started = service_.StartProvider(request, &after, &error);
  if (after) AdoptServiceFacts(*after);
  // As after start_tunnel: the request's country may be older than one the
  // watch pushed while it was built.
  PushNetworkCountryIfMoved(networkCountry, "start_provider");
  if (started) {
    LogInfo("sdkhost: provide: providing without a tunnel ({}, mode {}, tier {})", reason,
            provide::ToString(controlMode), serviceProviderMode_.load());
  } else {
    LogWarn("sdkhost: provide: the service did not run the provider-only device ({}): {}",
            reason, error.empty() ? "no detail" : error);
  }
}

// ---- send feedback with logs ------------------------------------------------
//
// See the contract in the header, App/FeedbackLogUpload.h and Common/LogUpload.h.

namespace {

// The DeviceRemote's upload callback (urnet_upload_logs_cb): an sdk whose
// DeviceRemote reports the upload's result calls it, and it is logged.
void OnDeviceRemoteLogUploadResult(void*, const char* resultJson, const char* error) {
  if (error != nullptr) {
    LogWarn("sdkhost: log attach failed: {}", error);
    return;
  }
  if (resultJson == nullptr) return;
  try {
    const auto result = nlohmann::json::parse(resultJson).get<urnet::UploadLogsResult>();
    if (result.error) LogWarn("sdkhost: log attach failed: {}", result.error->message);
  } catch (const std::exception& e) {
    LogWarn("sdkhost: the log attach's answer did not parse: {}", e.what());
  }
}

// The old path's upload, through the c abi by the DeviceRemote's handle, so
// that it touches nothing of SdkHost: with a service that predates upload_logs
// it answers only after the device's zip, and FeedbackLogUpload leaves it
// running past the app's exit budget.
void UploadLogsThroughDeviceRemote(uint64_t deviceHandle, const std::string& feedbackId) {
  char* error = nullptr;
  if (urnet_device_upload_logs(deviceHandle, feedbackId.c_str(), &OnDeviceRemoteLogUploadResult,
                               nullptr, &error)) {
    return;
  }
  LogWarn("sdkhost: log attach failed: {}", error != nullptr ? error : "the device is gone");
  if (error != nullptr) urnet_free_string(error);
}

}  // namespace

void SdkHost::UploadFeedbackLogs(const std::string& feedbackId) {
  if (feedbackId.empty()) {
    LogWarn("sdkhost: log attach skipped (no feedback id)");
    return;
  }
  if (!feedbackLogUpload_ || !feedbackLogUpload_->Send(feedbackId)) {
    LogWarn("sdkhost: log attach skipped (a log upload request is still being sent)");
  }
}

logupload::ServiceAnswer SdkHost::AskServiceToUploadLogs(const std::string& feedbackId) {
  // The request start_provider sends, so a service with no device builds the
  // same one. Read under the lock; the pipe call is made without it.
  proto::UploadLogs request;
  request.feedback_id = feedbackId;
  bool haveRequest = false;
  {
    std::scoped_lock lock(mutex_);
    try {
      if (localState_ && networkSpace_) {
        request.by_jwt = localState_->getByClientJwt();
        request.instance_id = localState_->getInstanceId();
        request.device_description = DeviceDescription();
        request.device_spec = DeviceSpec();
        request.app_version = appVersion_;
        request.network_space_json = networkSpace_->toJson();
        haveRequest = true;
      }
    } catch (const std::exception& e) {
      LogWarn("sdkhost: building the log upload request failed: {}", e.what());
    }
  }
  if (!haveRequest || !service_.IsConnected()) return logupload::ServiceAnswer::NotTaken;
  // This app's own log files ride in the service's zip, under app/: the
  // service opens them in the directory named here while acting as this app
  // (Common/AppLogFiles.h). Flushed first, so the lines written up to the
  // feedback are on disk when it does.
  try {
    urnet::flushGlog();
    request.app_log_dir = urnet::getLogDir();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: this app's log directory is left out of the log upload: {}", e.what());
  }
  std::string carrier;
  int64_t uploadId = 0;
  std::string error;
  const logupload::ServiceAnswer answer = service_.UploadLogs(request, &carrier, &uploadId, &error);
  switch (answer) {
    case logupload::ServiceAnswer::Accepted:
      // its outcome comes in the service's status (FollowServiceLogUpload)
      pendingLogUploadId_.store(uploadId);
      LogInfo("sdkhost: the service took the log upload ({} device)", carrier);
      break;
    case logupload::ServiceAnswer::Busy:
      LogInfo("sdkhost: the service is uploading its logs for an earlier feedback already");
      break;
    case logupload::ServiceAnswer::NotTaken:
      // "unknown request type" from a service that predates the verb
      LogWarn("sdkhost: the service did not upload its logs: {}",
              error.empty() ? "no detail" : error);
      break;
  }
  return answer;
}

std::function<void()> SdkHost::PrepareDeviceRemoteLogUpload(const std::string& feedbackId) {
  std::scoped_lock lock(mutex_);
  if (!device_.has_value()) {
    LogWarn("sdkhost: log attach skipped (the service did not take it and no device is bound)");
    return {};
  }
  const uint64_t deviceHandle = device_->handle();
  return [deviceHandle, feedbackId] { UploadLogsThroughDeviceRemote(deviceHandle, feedbackId); };
}

void SdkHost::FollowServiceLogUpload(const proto::TunnelStatus& st) {
  int64_t pendingUploadId = pendingLogUploadId_.load();
  const std::optional<logupload::FlightState> outcome = logupload::CompletionFor(
      pendingUploadId, st.log_upload_id, logupload::FlightStateFromString(st.log_upload_state));
  if (!outcome) return;
  // once: a later status that names the same outcome finds nothing pending
  if (!pendingLogUploadId_.compare_exchange_strong(pendingUploadId, 0)) return;
  if (*outcome == logupload::FlightState::Uploaded) {
    LogInfo("sdkhost: the service uploaded its logs ({} device)", st.log_upload_carrier);
  } else {
    LogWarn("sdkhost: the service's log upload ended {} ({} device)", st.log_upload_state,
            st.log_upload_carrier);
  }
}

void SdkHost::ConnectLocked(const SessionRequest& request) {
  // caller holds mutex_
  //
  // #27: a DELIBERATE connect/disconnect starts a new health attempt. Proof
  // earned by the previous target must not survive into the new window, or a
  // chosen location change renders as "Degraded" — the word for an involuntary
  // loss — while the rebuild it caused settles (see Tracker::NoteNewAttempt).
  // Here, at the single point every connect surface funnels through (button,
  // tray, coalesced row clicks), not at the entry points, so a settling row
  // intent that is superseded never resets anything.
  {
    std::scoped_lock healthLock(healthMutex_);
    healthTracker_.NoteNewAttempt();
  }
  try {
    if (connectVc_) {
      if (request.kind == ConnectKind::Disconnect) {
        connectVc_->disconnect();
      } else if (request.kind == ConnectKind::BestAvailable) {
        connectVc_->connectBestAvailable();
      } else if (request.location) {
        connectVc_->connect(*request.location);
      }
      return;
    }
    if (!device_) return;
    // No presentation, so no long-lived controller: open one for the call. This
    // is the tray "Connect" path and the path a Connect press takes when the
    // session was built by this very worker with the window hidden.
    auto controller = device_->openConnectViewController();
    if (request.kind == ConnectKind::Disconnect) {
      controller.disconnect();
    } else if (request.kind == ConnectKind::BestAvailable) {
      controller.connectBestAvailable();
    } else if (request.location) {
      controller.connect(*request.location);
    }
    device_->closeConnectViewController(controller);
  } catch (const std::exception& e) {
    LogError("sdkhost: '{}' failed against a live session: {}", request.reason,
             e.what());
    // Only a CONNECT gets a notice. A failed disconnect leaves the user
    // connected, which every surface already says; "nothing is connected" over
    // it would be the opposite of true.
    if (request.kind != ConnectKind::Disconnect) {
      PublishSessionFailure("the connection request was refused by the SDK");
    }
  }
}

// ---- the service-reconnect watchdog ----------------------------------------

void SdkHost::ScheduleServiceRetry() {
  serviceRecoveryNeeded_.store(true, std::memory_order_release);
  {
    std::scoped_lock lock(watchdogMutex_);
    if (watchdogStop_) return;
    if (watchdogRunning_) {
      watchdogCv_.notify_all();  // already waiting; nothing more to do
      return;
    }
    watchdogRunning_ = true;
    // A previous watchdog that has already returned still leaves a joinable
    // thread object behind; joining it here (it is not running) is what keeps
    // the move-assign below from calling std::terminate. Same trap PipeClient's
    // Close() documents.
    if (watchdog_.joinable()) watchdog_.join();
    watchdog_ = std::thread([this] { ServiceWatchdogLoop(); });
  }
}

void SdkHost::ServiceWatchdogLoop() {
  LogInfo("sdkhost: watching for a recoverable URnetwork service/session");
  std::size_t attempt = 0;
  // Nothing to recover and no sign-out owed. An owed sign-out keeps the watch
  // whatever a pass decided about recovery: the service has to be told
  // (SignOut.h).
  const auto idle = [this] {
    return !serviceRecoveryNeeded_.load(std::memory_order_acquire) && !signOut_.Owed();
  };
  for (;;) {
    const auto delay = recovery::ServiceRetryDelay(attempt);
    {
      std::unique_lock<std::mutex> lock(watchdogMutex_);
      watchdogCv_.wait_for(lock, delay, [this, &idle] { return watchdogStop_ || idle(); });
      if (watchdogStop_ || idle()) {
        watchdogRunning_ = false;
        return;
      }
    }
    const bool signedIn = loggedIn_.load(std::memory_order_acquire);
    if (!signedIn && !signOut_.Owed()) {
      serviceRecoveryNeeded_.store(false, std::memory_order_release);
      break;
    }
    const bool pipeAvailable =
        service_.IsConnected() ||
        ::WaitNamedPipeW(ids::kControlPipeName, NMPWAIT_NOWAIT);
    if (!pipeAvailable) {
      ++attempt;
      continue;
    }
    LogInfo("sdkhost: automatic service recovery attempt {} (next backoff {}s)",
            attempt + 1, static_cast<long long>(delay.count()));
    // Only a signed-in client has a session to restore. A signed-out one gets
    // its session from the sign-in itself (RegisterNetworkClient), and is
    // here only for the sign-out it owes the service: a provider pass delivers
    // it first, and a signed-out reconcile can only stop a provider.
    if (signedIn) {
      EnsureSession("automatic service recovery", /*automaticRecovery=*/true);
    } else {
      RequestProviderReconcile("automatic service recovery, sign-out owed");
    }
    ++attempt;
  }
  std::scoped_lock lock(watchdogMutex_);
  watchdogRunning_ = false;
}

void SdkHost::StopServiceWatchdog() {
  {
    std::scoped_lock lock(watchdogMutex_);
    watchdogStop_ = true;
    serviceRecoveryNeeded_.store(false, std::memory_order_release);
  }
  watchdogCv_.notify_all();
  if (watchdog_.joinable()) watchdog_.join();
}

// ---- the rpc-sync watchdog -------------------------------------------------
//
// See the block comment on the members in SdkHost.h for why a bootstrap that
// "succeeded" proves nothing about whether the service will talk to us.

void SdkHost::ArmSyncWatchdogLocked(std::uint64_t generation, bool reattached) {
  // caller holds mutex_
  std::scoped_lock lock(syncMutex_);
  if (syncStop_) return;
  syncGeneration_ = generation;
  syncReattached_ = reattached;
  syncPendingSince_ = std::chrono::steady_clock::now();
  syncPendingWarned_ = false;
  syncPendingFailurePublished_ = false;
  // Inspect immediately. A permanent instance/certificate refusal may already
  // be present after setRpcServer; waiting five seconds adds no information.
  syncDeadline_ = std::chrono::steady_clock::now();
  if (syncRunning_) {
    // A check for an older session is still pending. Do NOT start a second
    // thread — wake the one that exists so it re-reads the new generation and
    // deadline. Its old generation is already stale, so it has nothing to say.
    syncCv_.notify_all();
    return;
  }
  syncRunning_ = true;
  // Joining here is safe ONLY because syncRunning_ is false, which the loop
  // clears as its last act under this same lock — so the thread is past every
  // line that could want mutex_, which THIS thread is holding. Reversing that
  // order would be a deadlock, not a race. (Same joinable-but-finished trap
  // ScheduleServiceRetry documents.)
  if (syncWatchdog_.joinable()) syncWatchdog_.join();
  syncWatchdog_ = std::thread([this] { SyncWatchdogLoop(); });
}

void SdkHost::SyncWatchdogLoop() {
  for (;;) {
    std::uint64_t generation = 0;
    bool reattached = false;
    {
      std::unique_lock<std::mutex> lock(syncMutex_);
      // Re-read the deadline on every wake: a re-arm can push it later, and a
      // shutdown can end the wait early. wait_until with a predicate would hide
      // the re-arm, which is the one thing this loop must notice.
      while (!syncStop_ && std::chrono::steady_clock::now() < syncDeadline_) {
        syncCv_.wait_until(lock, syncDeadline_);
      }
      if (syncStop_) {
        syncRunning_ = false;
        return;
      }
      generation = syncGeneration_;
      reattached = syncReattached_;
    }

    const RpcSyncState state = CheckSessionSync(generation, reattached);

    bool publishPendingFailure = false;
    {
      std::scoped_lock lock(syncMutex_);
      // A bootstrap that landed while the check was running re-armed us; go
      // round again rather than exit and leave that session unwatched.
      if (!syncStop_ && syncGeneration_ != generation) continue;
      if (syncStop_ || state == RpcSyncState::Stale ||
          state == RpcSyncState::Refused) {
        syncRunning_ = false;
        return;
      }

      const auto now = std::chrono::steady_clock::now();
      if (state == RpcSyncState::Healthy) {
        syncPendingSince_ = now;
        syncPendingWarned_ = false;
        syncPendingFailurePublished_ = false;
        syncDeadline_ = now + kSyncHealthyPoll;
      } else {
        if (!syncPendingWarned_ && now - syncPendingSince_ >= kSyncSettleDeadline) {
          syncPendingWarned_ = true;
          LogWarn("sdkhost: the device rpc has not synced within {}ms and the "
                  "service has refused nothing. The sdk is still retrying and "
                  "this watchdog will keep observing the session.",
                  static_cast<long long>(kSyncSettleDeadline.count()));
        }
        if (!syncPendingFailurePublished_ &&
            now - syncPendingSince_ >= kSyncFailureDeadline) {
          syncPendingFailurePublished_ = true;
          publishPendingFailure = true;
        }
        syncDeadline_ = now + kSyncPendingPoll;
      }
    }
    if (publishPendingFailure) PublishPendingSyncFailure(generation);
  }
}

void SdkHost::PublishPendingSyncFailure(std::uint64_t generation) {
  // Non-destructive by design. A pending sync has not been refused, and a
  // reattached tunnel may still be carrying traffic for the machine. Publish
  // the loss of app control, keep observing, and clear this standing notice as
  // soon as the same generation reaches the connected level.
  std::scoped_lock lock(mutex_);
  if (generation != sessionGeneration_ || !device_) return;
  try {
    if (device_->getRemoteConnected() || !device_->getSyncError().empty()) return;
  } catch (const std::exception&) {
    // An unreadable pending level is exactly the state the bounded notice is
    // meant to expose; the watchdog will keep retrying the observation.
  }
  syncPendingFailureGeneration_ = generation;
  sessionFailure_ =
      "The VPN service is running, but this app has not completed its secure "
      "control connection. The tunnel was left running while recovery "
      "continues; use the tray emergency stop if traffic is unavailable.";
  ModeNotice notice;
  notice.active = true;
  notice.kind = ModeNotice::Kind::SessionFailed;
  notice.message = sessionFailure_;
  DeliverModeNotice(notice);
}

SdkHost::RpcSyncState SdkHost::CheckSessionSync(std::uint64_t generation,
                                               bool reattached) {
  // Must not be called holding mutex_. The check and any resulting teardown are
  // serialized with bootstrap so a stale generation cannot affect a new one.
  std::scoped_lock lock(mutex_);
  if (generation != sessionGeneration_ || !device_) return RpcSyncState::Stale;

  std::string syncError;
  bool remoteConnected = false;
  try {
    // Error first: a permanent refusal is actionable even if a remote-change
    // edge was missed.
    syncError = device_->getSyncError();
    remoteConnected = device_->getRemoteConnected();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: could not read the rpc sync state: {}", e.what());
    return RpcSyncState::Pending;
  }

  if (remoteConnected) {
    if (syncPendingFailureGeneration_ == generation) {
      syncPendingFailureGeneration_ = 0;
      sessionFailure_.clear();
      PublishModeNotice();
    }
    return RpcSyncState::Healthy;
  }
  if (syncError.empty()) return RpcSyncState::Pending;

  LogWarn("sdkhost: THE SERVICE REFUSED THIS SESSION'S DEVICE RPC — '{}'. "
          "The app will not keep presenting empty remote state as a working "
          "control session.",
          syncError);
  try {
    // A reattached tunnel may still be carrying traffic for other processes;
    // never destroy it merely because adoption failed. A fresh session belongs
    // to this process and is stopped because it cannot be controlled safely.
    TeardownSessionLocked(/*stopTunnel=*/!reattached);
  } catch (const std::exception& e) {
    LogError("sdkhost: teardown of the refused rpc session failed: {}", e.what());
  }
  PublishSessionFailure(
      reattached
          ? "The running tunnel rejected this app's saved control credentials. "
            "It was left running and was not restarted. Press Connect to replace "
            "it with a new session."
          : "The URnetwork service refused this app's control connection. The "
            "new tunnel was stopped; restart the service, then try again.");
  return RpcSyncState::Refused;
}

void SdkHost::StopSyncWatchdog() {
  {
    std::scoped_lock lock(syncMutex_);
    syncStop_ = true;
  }
  syncCv_.notify_all();
  if (syncWatchdog_.joinable()) syncWatchdog_.join();
}

// Through the SAME worker as Connect, and for the same reason: the button is one
// control with two labels, so if one half cannot block the UI thread neither can
// the other. It used to take mutex_ inline, which was harmless while nothing but
// launch ever held that lock for seconds — and stopped being harmless the moment
// a Connect press could start a bootstrap. A Disconnect that arrives mid-start
// is applied after it, which is also the right order.
//
// A Disconnect NEVER starts a session (see the worker): with no session there is
// nothing connected and nothing to do.
void SdkHost::Disconnect() {
  // the user's disconnect: a connect waiting on the balance is not run after it
  if (onUserDisconnect_) onUserDisconnect_();
  SessionRequest r;
  r.kind = ConnectKind::Disconnect;
  r.reason = "disconnect";
  RequestSession(std::move(r));
}

// See the contract in the header. Deliberately NOT routed through the session
// worker: this is the escape hatch, and an escape hatch that queues behind a
// bootstrap holding mutex_ for up to the pipe timeout is not one. It talks to
// the service directly, and the service's Stop() is itself bounded.
proto::TunnelStatus SdkHost::StopServiceTunnel() {
  if (!service_.IsConnected()) {
    LogWarn("sdkhost: stop-tunnel asked for with no control channel to the "
            "service. Nothing here can revert a tunnel this process does not "
            "own — if routes are still installed, the service is gone and its "
            "death already took them (the adapter dies with the process).");
    return {};
  }
  LogInfo("sdkhost: stopping the SERVICE tunnel (routes, dns and the firewall "
          "policy all come back) — this is the escape hatch, not a connect "
          "controller disconnect");
  proto::TunnelStatus st = service_.StopTunnel();
  AdoptServiceFacts(st);
  // The SDK side follows, so the app does not sit rendering Connecting against
  // a tunnel that no longer exists. Queued rather than inline: the point above
  // was to avoid waiting on the session worker, not to avoid using it at all.
  //
  // AND IT IS WHAT DROPS THE DEVICE. The stop above destroyed the SERVICE's
  // DeviceLocal and its mTLS listener; this side's DeviceRemote survived it and
  // used to be left in place, so the next Connect drove a handle to something
  // that no longer existed and never sent start_tunnel — the hatch fixed the
  // machine and broke the app. The queued Disconnect now runs through the same
  // decision table as every other gesture, sees no session on the service side,
  // and tears the stale DeviceRemote down. Doing it here instead would mean
  // taking mutex_ on the one path that must never wait for it.
  Disconnect();
  return st;
}

void SdkHost::ClosePresentationLocked(bool sessionEnding) {
  presentationSubs_.clear();
  // Released here rather than beside each locationsVc_.reset() below, so the two
  // exit paths cannot disagree. Once it is clear the api path may write again.
  deviceFeedOpen_.store(false, std::memory_order_release);
  if (!device_) {
    connectVc_.reset();
    contractVc_.reset();
    contractDetailsVc_.reset();
    blockVc_.reset();
    locationsVc_.reset();
    peerVc_.reset();
    providerLocationsVc_.reset();
    {
      std::scoped_lock drawerLock(drawerMutex_);
      extenderVc_.reset();
    }
    return;
  }
  // D4: the close calls below are courtesies to the SERVICE — they detach
  // listeners and window monitors on the hosted device, over the session rpc.
  // With the control channel down, the process those courtesies would reach is
  // gone and its mTLS listener with it; each call would only spend an rpc
  // timeout proving that (the presentationSubs_ clear above already pays one
  // such timeout at worst — the first failed call detaches the transport and
  // everything after it is local). Skip them and just drop this side's
  // handles: every pipe-down path ends in TeardownSessionLocked, whose
  // device close is what cleans the Go side up.
  const bool remoteUsable = service_.IsConnected();
  if (remoteUsable) {
    // The provider-locations controller closes FIRST, mirroring the order it is
    // opened in (it is opened before the connected-provider listener registers).
    // It is INSIDE the guard with every other close, which is the whole point of
    // this branch: it is an rpc to the service like the rest, so exempting it
    // would reintroduce exactly the blocking call the D4 note above describes.
    if (providerLocationsVc_) {
      device_->closeProviderLocationsViewController(*providerLocationsVc_);
    }
    if (peerVc_) device_->closePeerViewController(*peerVc_);
    if (locationsVc_) device_->closeLocationsViewController(*locationsVc_);
    if (contractDetailsVc_) {
      device_->closeContractDetailsViewController(*contractDetailsVc_);
    }
    if (blockVc_) device_->closeBlockActionViewController(*blockVc_);
    if (contractVc_) device_->closeContractViewController(*contractVc_);
    if (connectVc_) device_->closeConnectViewController(*connectVc_);
    // The extender controller closes ITSELF (the SDK gives it no
    // Device::closeExtenderViewController), but it is the same rpc courtesy as
    // the rest, so it lives inside the same guard. close() stops it too, and it
    // is what makes dropping the reference below safe while a background call
    // still holds one: the Go side is cancelled, the C handle survives until
    // that last reference goes.
    if (auto controller = ExtenderController()) controller->close();
  }
  // ...but the handles drop UNCONDITIONALLY, whether or not the courtesy was
  // paid. That asymmetry is the D4 contract, and the provider controller joins
  // it rather than being reset early inside the branch.
  providerLocationsVc_.reset();
  peerVc_.reset();
  locationsVc_.reset();
  contractDetailsVc_.reset();
  blockVc_.reset();
  contractVc_.reset();
  connectVc_.reset();
  {
    std::scoped_lock drawerLock(drawerMutex_);
    extenderVc_.reset();
  }
  ClearDrawer(sessionEnding);
}

// D4: RECORD AND RETURN — the caller is the XAML thread, and this used to be
// the app's kill. It took mutex_ inline (a lock the session worker holds for
// whole bootstraps) and then ran ClosePresentationLocked, whose teardown is
// ~ten listener unsubscribes and six view-controller closes, EACH of which is
// a synchronous DeviceLocalRpc call while the rpc transport thinks it is
// attached (device_rpc.go addListener's unsub: `if service != nil {
// rpcCallVoid(...Remove...Listener...) }`, RpcCallTimeout 60s). Against a
// service that is DYING — socket open, process gone — the first of those
// blocks until the mux write timeout tears the transport down, which is
// seconds. Windows killed this app three times for exactly that, AppHangB1,
// 4-5s after each daemon death, last app-log line "presentation stopped" —
// the line ReconcileWindowPresentation prints immediately before calling
// here. The UI thread must never wait on rpc completion; the presentation
// worker does the waiting instead.
void SdkHost::SetPresentationActive(bool active) {
  {
    std::scoped_lock lock(presentationMutex_);
    if (presentationStop_) return;
    presentationDesired_ = active;
    presentationDirty_ = true;
    // A running worker re-checks dirty before exiting.
    if (!presentationWorkerRunning_) {
      presentationWorkerRunning_ = true;
      // A previous worker that has already returned still leaves a joinable
      // thread object behind; joining it here (it is not running) is what keeps
      // the move-assign below from calling std::terminate. Same trap
      // ScheduleServiceRetry documents.
      if (presentationWorker_.joinable()) presentationWorker_.join();
      presentationWorker_ = std::thread([this] { PresentationWorkerLoop(); });
    }
  }
  // The provider-only statistics follow the window at once, not at the next
  // tick; the loop reads presentationDesired_, written above.
  KickProviderOnlyStats();
}

void SdkHost::PresentationWorkerLoop() {
  for (;;) {
    bool active = false;
    {
      std::scoped_lock lock(presentationMutex_);
      // Exit when there is nothing left to apply. The flag-clear and the
      // running-flag are under the same lock the producer holds, so a toggle
      // that lands as this worker is finishing either sets dirty before the
      // check here (we go round again) or finds running=false and starts a
      // fresh worker — never dropped.
      if (presentationStop_ || !presentationDirty_) {
        presentationWorkerRunning_ = false;
        return;
      }
      active = presentationDesired_;
      presentationDirty_ = false;
    }
    {
      std::scoped_lock lock(mutex_);
      if (presentationActive_ == active) continue;
      presentationActive_ = active;
      if (!active) {
        // The teardown that used to hang the XAML thread. On this worker a
        // dying service costs one rpc timeout at worst (the first failed call
        // detaches the transport and the rest are local), and nobody on the
        // UI thread waits for any of it.
        try {
          // a hide, not a teardown: the provider extender status and the
          // provider-stats reading stay for the window that comes back (O8)
          ClosePresentationLocked(/*sessionEnding=*/false);
        } catch (const std::exception& e) {
          LogWarn("sdkhost: presentation close failed: {}", e.what());
        }
        continue;
      }
      try {
        // Stats and the drawer are genuinely device-scoped; the provider list
        // is not, so the `if (!device_) return;` that used to sit here has
        // been narrowed to the two things it is actually true of. Returning
        // early on no-device meant alt-tabbing back with no service running
        // re-armed NOTHING - which is the same bug the block below describes,
        // one source further down.
        if (device_) {
          SubscribeStats();
          SubscribeDrawer();
        }
        // The other half of ClosePresentationLocked. That function closes FOUR
        // feeds (stats, drawer, locations, peers) and this one used to put
        // back only two - so the locations/peers view controllers, their
        // listeners and the snapshot they hold were destroyed by any window
        // DEACTIVATION and never rebuilt. Nothing else rebuilt them either:
        // the only openers were a chooser-sheet open and
        // NetworkPage::SetSelected, which runs on a navigation CHANGE, so a
        // window that came back to the destination it left on stayed empty.
        //
        // Now unconditional, so it re-arms the api source too: the cache
        // normally makes it a no-op, and a failed previous fetch is retried
        // here.
        EnsureLocationsLocked();
      } catch (const std::exception& e) {
        LogWarn("sdkhost: presentation open failed: {}", e.what());
      }
    }
  }
}

void SdkHost::StopPresentationWorker() {
  {
    std::scoped_lock lock(presentationMutex_);
    presentationStop_ = true;
    presentationDirty_ = false;
  }
  // Joined, not detached: a detached worker touching a destroyed SdkHost from
  // inside a cgo call would turn an orderly exit into a WER record. The join
  // is bounded in practice — the worker only blocks inside rpc teardown, the
  // pipe-down guard in ClosePresentationLocked skips exactly the calls that
  // could wait on a dead peer, and against a live service the closes are
  // loopback round-trips.
  if (presentationWorker_.joinable()) presentationWorker_.join();
}

void SdkHost::TeardownSessionLocked(bool stopTunnel) {
  // FIRST, AND THIS ORDER IS THE POINT. Session teardown only: stop the tunnel
  // but keep the service-persisted device identity (key material). The identity
  // is device-scoped, not session-scoped — RegisterNetworkClient's
  // re-registration under a new jwt (guest upgrade, verify after an upgrade)
  // must not rotate the key peers use to verify this device. Only the explicit
  // Logout() severs it.
  //
  // It used to sit at the BOTTOM of this function, below device_->close(). That
  // is the app-side copy of exactly the ordering bug StopBudget.h fixed in the
  // service: the cheap, local, safety-critical half (routes, dns, firewall —
  // 133 ms, measured four times) sequenced behind the half that can block
  // indefinitely. A DeviceRemote::close() that wedges must not be able to hold
  // this machine's routes hostage.
  if (stopTunnel && service_.IsConnected()) {
    service_.StopTunnel();
  }
  // D4: with the control channel gone, close the DEVICE first. Its close
  // cancels the rpc transport, which turns every courtesy unsubscribe below —
  // presentationSubs_, subs_, the view-controller closes — from "one rpc
  // timeout against a peer that died" into a local no-op. With the channel
  // up the order stays listeners-then-device, so the removals actually reach
  // the hosted device. close() is once-guarded on the Go side, so the
  // unconditional close further down stays correct in both orders.
  if (device_ && !service_.IsConnected()) {
    try {
      device_->close();
    } catch (const std::exception& e) {
      LogWarn("sdkhost: early close of the dead session's device failed: {}",
              e.what());
    }
  }
  // Invalidate persistence callbacks before removing listeners. A callback
  // already in flight can then finish without rewriting a record this teardown
  // is about to remove.
  activeRpcPersistenceGeneration_.store(0, std::memory_order_release);
  confirmedRpcPersistenceGeneration_.store(0, std::memory_order_release);
  ClosePresentationLocked(/*sessionEnding=*/true);
  subs_.clear();
  if (device_) { device_->close(); device_.reset(); }
  // A pending rpc-sync check must not act on the session that is ending — its
  // generation stops matching here, which is the whole point of the counter.
  ++sessionGeneration_;
  syncPendingFailureGeneration_ = 0;
  hasSession_.store(false, std::memory_order_release);
  {
    std::scoped_lock lock(wfpStateMutex_);
    sessionRpcHostPort_.clear();
  }
  provideHasNetworkKey_ = false;
  // No session, so no tunnel â€” reset to the mode that claims less, not to
  // Tunnel. A status built between this teardown and the next bootstrap must
  // not be able to render "connected".
  sessionMode_.store(proto::StartMode::RpcOnly);
  {
    std::scoped_lock persistenceLock(rpcPersistenceMutex_);
    ClearRpcSession();
  }
  // Not a failure — a deliberate teardown. Clearing this BEFORE the publish
  // below is what makes that publish a retraction instead of a re-raise of
  // whatever the last failure was.
  sessionFailure_.clear();
  // Retract the persistent notice. It is deliberately non-dismissible and lives
  // at window level, so without this a user who saw "Could not start a session
  // with the URnetwork service. Nothing is connected." and then signed out is
  // left staring at that sentence on the SIGN-IN screen, where it is meaningless
  // and there is no control to remove it. device_ is already cleared above, so
  // this publishes an INACTIVE notice — which is the retraction.
  PublishModeNotice();
}

// See the contract in the header.
void SdkHost::Logout() {
  // 0. A browser or wallet flow the account started is answered and forgotten
  // (each network starts fresh, owner decision 2026-10-05): an add-sign-in
  // attempt's late return would otherwise add that sign-in method to the next
  // account signed in. On the UI thread, as every caller of it, and before
  // mutex_: it answers the flows' callbacks.
  CancelPendingWalletFlows("superseded by signing out");
  // 1. The signed-out account's queued work goes: a connect, a row click still
  // settling, a reconcile. A worker sleeping out a settle wakes to the empty
  // slot and exits.
  {
    std::scoped_lock lock(pendingMutex_);
    pending_ = SessionRequest{};
    pendingRequested_ = false;
  }
  pendingCv_.notify_all();
  // 2. Signed out from here, before the lock: a pass that takes mutex_ ahead of
  // this one reads it and starts nothing for the account that is leaving
  // (BootstrapSession, ReconcileProviderLocked). Also before SetAuthState below,
  // whose handler asks IsLoggedIn().
  loggedIn_.store(false, std::memory_order_release);
  std::scoped_lock lock(mutex_);
  try {
    pendingWalletAuth_.reset();
    pendingAuthJwt_.reset();
    pendingAuthJwtType_.clear();
    // A pending instant network belongs to whoever was mid-signup, not to the
    // session being ended; dropping it here means a later Confirm cannot
    // register a device against a stale jwt.
    pendingInstantJwt_.reset();
    if (events_) events_->NewSession();  // the next sign-in is a new session
    // The local credentials first, because nothing can hold them up: the app
    // is signed out on disk even if it is ended while the service half below
    // waits on the pipe.
    if (asyncLocalState_) asyncLocalState_->logout([](bool) {});
    // and the credential the api attaches to its calls, which the next
    // sign-in's own calls would otherwise carry until it installs its own
    if (api_) api_->setByJwt("");
    // 3. The service, as Quit stops it, then the logout, which severs the
    // device identity and clears what the service's sdk stored for the
    // account. Owed until all three succeed (SignOut.h).
    const signout::Delivery delivery = signOut_.Begin(SignOutServiceLocked());
    if (delivery == signout::Delivery::Delivered) {
      LogInfo("sdkhost: sign-out: the service is stopped and has forgotten the account "
              "(stop_tunnel, stop_provider, logout)");
      serviceRecoveryNeeded_.store(false, std::memory_order_release);
      watchdogCv_.notify_all();
    } else {
      LogWarn("sdkhost: sign-out: {}; the sign-out stays owed to the service, which is "
              "told as soon as it can be, and nothing starts until it has been",
              signout::ToString(delivery));
      ScheduleServiceRetry();
    }
    // 4. This side of the session: the DeviceRemote, its feeds and the saved
    // rpc session. The device first, as Quit closes it: the DeviceLocal it
    // talks to is gone, so its close turns the courtesy unsubscribes that
    // follow into local no-ops. stopTunnel=false: sent above.
    if (device_) {
      try {
        device_->close();
      } catch (const std::exception& e) {
        LogWarn("sdkhost: sign-out: closing the DeviceRemote failed: {}", e.what());
      }
    }
    TeardownSessionLocked(/*stopTunnel=*/false);
    // The stop_tunnel and the stop_provider above retired the provider-only
    // device: nothing provides for a signed-out app.
    serviceProviderRunning_.store(false);
    serviceProviderMode_.store(0);
    serviceProviderNetworkKey_.store(false);
    serviceProviderClients_.store(-1);
    sessionFailure_.clear();  // belongs to the session that just ended
    SetAuthState(AuthState::LoggedOut);
    LogInfo("sdkhost: logged out");
  } catch (const std::exception& e) {
    LogError("sdkhost: logout failed: {}", e.what());
  }
}

signout::Service SdkHost::SignOutServiceLocked() {
  // caller holds mutex_, across the calls the delivery makes
  signout::Service service;
  service.reach = [this] {
    // A dropped channel is not a stopped service (Quit's rule): one that is
    // still running still runs what it ran, so dial it. Dialling a service
    // that is not running fails at once.
    if (!service_.IsConnected()) service_.Connect();
    return service_.IsConnected();
  };
  service.send = [this](signout::Request request) {
    switch (request) {
      case signout::Request::StopTunnel: {
        bool answered = false;
        const proto::TunnelStatus stopped = service_.StopTunnel(&answered);
        if (!answered) {
          LogWarn("sdkhost: sign-out: stop_tunnel did not answer: {}",
                  stopped.error.empty() ? "no detail" : stopped.error);
          return false;
        }
        AdoptServiceFacts(stopped);
        // Quit's warning, for the same reason: the kill switch's lock-free
        // escape keeps the policy when the session lock is wedged.
        if (!stopped.wfp_state.empty() && stopped.wfp_state != "off") {
          LogError("sdkhost: sign-out: a firewall policy is still in force after the "
                   "stop (wfp={}); restarting the urnetworkd service lifts it",
                   stopped.wfp_state);
        }
        return true;
      }
      case signout::Request::StopProvider: {
        std::optional<proto::TunnelStatus> after;
        std::string error;
        const bool stopped = service_.StopProvider(&after, &error);
        if (after) AdoptServiceFacts(*after);
        // A service older than stop_provider runs no provider-only device, and
        // its stop_tunnel ended everything it did run.
        if (stopped || IsUnknownRequestReply(error)) return true;
        LogWarn("sdkhost: sign-out: stop_provider failed: {}",
                error.empty() ? "no detail" : error);
        return false;
      }
      case signout::Request::Logout: {
        proto::Logout logout;
        try {
          // The account's space, whose sdk state the service clears.
          if (networkSpace_) logout.network_space_json = networkSpace_->toJson();
        } catch (const std::exception& e) {
          LogWarn("sdkhost: sign-out: the network space for the logout: {}", e.what());
        }
        const bool done = service_.Logout(logout);
        if (!done) LogWarn("sdkhost: sign-out: the service's logout did not complete");
        return done;
      }
    }
    return false;
  };
  return service;
}

void SdkHost::SettleSignOutLocked(const char* reason) {
  // caller holds mutex_
  if (!signOut_.Owed()) return;
  const signout::Delivery delivery = signOut_.Settle(SignOutServiceLocked());
  if (delivery == signout::Delivery::Delivered) {
    LogInfo("sdkhost: the owed sign-out is delivered ({}): stop_tunnel, stop_provider "
            "and logout",
            reason);
    // A signed-out app watched the service for this alone.
    if (!loggedIn_.load(std::memory_order_acquire)) {
      serviceRecoveryNeeded_.store(false, std::memory_order_release);
      watchdogCv_.notify_all();
    }
    return;
  }
  LogWarn("sdkhost: the sign-out is still owed ({}): {}; nothing starts until it is "
          "delivered",
          reason, signout::ToString(delivery));
  ScheduleServiceRetry();
}

signout::Marker SdkHost::SignOutMarker() {
  signout::Marker marker;
  marker.read = [] {
    std::error_code ec;
    return std::filesystem::exists(SignOutOwedFile(), ec);
  };
  marker.write = [](bool owed) {
    std::error_code ec;
    if (!owed) {
      // One left behind is delivered again at the next launch, which stops
      // whatever runs then; logged, as that is the one way it can surprise.
      std::filesystem::remove(SignOutOwedFile(), ec);
      if (ec) LogError("sdkhost: sign-out: the owed marker could not be removed: {}", ec.message());
      return;
    }
    std::ofstream file(SignOutOwedFile(), std::ios::trunc);
    file << "a sign-out the URnetwork service has not done yet\n";
    if (!file) LogError("sdkhost: sign-out: the owed marker could not be written");
  };
  return marker;
}

// See the contract in the header.
void SdkHost::Quit() {
  // 1. Nothing new, and nothing queued. A worker sleeping out a row click's
  // settle wakes to the empty slot and exits; one in the middle of a pass
  // finds nothing after it.
  {
    std::scoped_lock lock(pendingMutex_);
    quitting_.store(true);
    pending_ = SessionRequest{};
    pendingRequested_ = false;
  }
  pendingCv_.notify_all();
  // 2. The threads that act on their own, joined outside mutex_ because each
  // of them takes it (the destructor's rule; its own calls then find them
  // stopped). The watchdog first: its recovery pass is the one that dials the
  // service and ends in a provider reconcile.
  StopServiceWatchdog();
  StopPresentationWorker();
  StopSyncWatchdog();
  StopProviderOnlyStats();
  // 3. The service, under mutex_: after any pass in flight (a Connect's
  // bootstrap, a reconcile), and with none able to follow it.
  std::scoped_lock lock(mutex_);
  try {
    // A dropped channel is not a stopped service, and one that is still
    // running still runs what it ran. Dialling a service that is not running
    // fails at once, and then nothing runs: the session, its firewall policy
    // and the provider-only device all ended with its process.
    if (!service_.IsConnected()) service_.Connect();
    if (service_.IsConnected()) {
      // The machine first, as in every teardown here. stop_tunnel ends the
      // session whatever its mode (Disconnect keeps an rpc-only one; a quit
      // keeps nothing), lifts any firewall policy, the armed floor included
      // (StopLocked, finalDisarm), and retires the provider-only device with
      // it. stop_provider then asks for that by name: it costs one round trip
      // and ends a provider-only device whatever the stop above did with it.
      const proto::TunnelStatus stopped = service_.StopTunnel();
      AdoptServiceFacts(stopped);
      if (stopped.state == proto::TunnelState::Error && !stopped.error.empty()) {
        LogError("sdkhost: quit: stop_tunnel failed: {}", stopped.error);
      }
      std::optional<proto::TunnelStatus> after;
      std::string error;
      if (!service_.StopProvider(&after, &error)) {
        LogWarn("sdkhost: quit: stop_provider failed: {}",
                error.empty() ? "no detail" : error);
      }
      if (after) AdoptServiceFacts(*after);
      const proto::TunnelStatus& last = after ? *after : stopped;
      LogInfo("sdkhost: quit: the service is stopped (state={} routes={} wfp={} "
              "provider={}); nothing here starts either again",
              proto::ToString(last.state),
              last.routes_installed ? "STILL INSTALLED" : "reverted", last.wfp_state,
              last.provider_running ? "STILL RUNNING" : "retired");
      if (!last.wfp_state.empty() && last.wfp_state != "off") {
        LogError("sdkhost: quit: a firewall policy is STILL IN FORCE after the "
                 "stop (wfp={}), so this machine may stay blocked with the app "
                 "gone. The service log says why; restarting the urnetworkd "
                 "service lifts it (the policy dies with its process).",
                 last.wfp_state);
      }
    } else {
      LogInfo("sdkhost: quit: no URnetwork service is running, so it runs no "
              "session and no provider");
    }
    // This side of the session: the DeviceRemote, its feeds, and the saved
    // rpc session, which names a listener the stop just destroyed, so the
    // next launch does not try to adopt it. stopTunnel=false: sent above.
    //
    // The device is closed first, as TeardownSessionLocked closes it for a
    // dead control channel: the DeviceLocal it talks to is gone, so its close
    // cancels the rpc transport and turns the courtesy unsubscribes that follow
    // into local no-ops, instead of rpcs to a listener that no longer exists
    // holding the exit up. close() is once-guarded on the Go side.
    if (device_) {
      try {
        device_->close();
      } catch (const std::exception& e) {
        LogWarn("sdkhost: quit: closing the DeviceRemote failed: {}", e.what());
      }
    }
    TeardownSessionLocked(/*stopTunnel=*/false);
  } catch (const std::exception& e) {
    LogError("sdkhost: quit: stopping the service failed: {}", e.what());
  }
  // As Logout leaves them: nothing provides now.
  serviceProviderRunning_.store(false);
  serviceProviderMode_.store(0);
  serviceProviderNetworkKey_.store(false);
  serviceProviderClients_.store(-1);
}

}  // namespace urnw
