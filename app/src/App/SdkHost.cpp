// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "SdkHost.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <random>
#include <thread>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Ids.h"
#include "Log.h"
#include "Paths.h"
#include "RpcSessionBlob.h"
#include "Strings.h"

namespace urnw {
namespace {

// Persisted RPC session (last-good), mirroring macOS RpcSessionStore. Lets the
// app reattach its DeviceRemote to a still-running service tunnel.
//
// The struct and both conversions live in Common/RpcSessionBlob.h — pure, so
// the service selftest pins the round trip, the missing-instance_id migration
// and the malformed cases. Only the two file handles are left here.
using RpcSession = rpcsession::Blob;

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

void SaveRpcSession(const RpcSession& s) {
  std::ofstream f(RpcSessionFile(), std::ios::trunc);
  if (f) f << rpcsession::Serialize(s);
}

std::optional<RpcSession> LoadRpcSession() {
  std::ifstream f(RpcSessionFile());
  if (!f) return std::nullopt;
  // Read the whole file and hand the BYTES to the pure parser, rather than
  // parsing off the stream: the selftest can then exercise exactly the code
  // path this call takes, which it could not do through an ifstream.
  const std::string text((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
  return rpcsession::Parse(text);
}

void ClearRpcSession() {
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

}  // namespace

SdkHost::~SdkHost() {
  // BEFORE mutex_, and joined rather than detached: the watchdog takes mutex_
  // (through the session worker it wakes), so stopping it from inside the lock
  // would deadlock, and letting it outlive this object would leave a thread
  // dialling a pipe on behalf of a destroyed host.
  StopServiceWatchdog();
  // Same rule, same reason: the rpc-sync watchdog takes mutex_ to look at the
  // device, so it is stopped and JOINED here, above the lock.
  StopSyncWatchdog();
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

urnet::NetworkSpace SdkHost::BuildNetworkSpace() {
  // Matches macOS DeviceManager.initializeNetworkSpace.
  urnet::NetworkSpaceKey key;
  key.host_name = std::string(ids::kNetworkSpaceHostName);
  key.env_name = std::string(ids::kNetworkSpaceEnvName);

  urnet::NetworkSpaceValues values;
  values.bundled = true;
  values.net_expose_server_ips = true;
  values.net_expose_server_host_names = true;
  values.link_host_name = "ur.io";
  values.migration_host_name = "bringyour.com";
  values.store = "";
  values.wallet = "circle";
  // Only claim Google SSO when this build can actually run the flow. The value
  // used to be a flat false; it is now tied to the compiled-in OAuth client id
  // so the space, SsoGoogleEnabled() and the login button can never disagree.
  values.sso_google = GoogleSignIn::Configured();
  values.env_secret = "";

  // URNETWORK_NETWORK_HOST points the client at a different backend, so that a
  // throwaway account on a test network can exercise the success paths. Until
  // this existed nothing in the client had ever seen a 200: every screen was
  // verified against layout, empty states and 401s only.
  //
  // MIGRATION_HOST_NAME MUST BE CLEARED WITH IT. sdk/network_space.go's
  // ServiceUrl prefers MigrationHostName over the key's HostName, so setting
  // the host alone changes nothing and the client keeps talking to
  // bringyour.com - looking like the override silently failed.
  //
  // Env name follows the same rule the SDK uses: "main" (the default) gives
  // api.<host>, anything else gives <env>-api.<host>.
  //
  // This is the programmatic form of the network selector P5 is building; when
  // that lands, both should end up driving setActiveNetworkSpace rather than
  // each carrying their own idea of how a space is assembled.
  const std::string hostOverride = EnvVar(L"URNETWORK_NETWORK_HOST");
  if (const auto host = hostOverride; !host.empty()) {
    key.host_name = host;
    // reset(), not "": these wrapper fields are std::optional<std::string> and
    // the Go side omits an unset one, which is what ServiceUrl's `!= ""` test
    // needs to fall through to the key's host name.
    values.migration_host_name.reset();
    std::string env(ids::kNetworkSpaceEnvName);
    if (const auto envOverride = EnvVar(L"URNETWORK_NETWORK_ENV"); !envOverride.empty()) {
      env = envOverride;
    }
    key.env_name = env;
    LogWarn("sdkhost: NETWORK OVERRIDE - host={} env={} (migration host cleared). "
            "This client is NOT talking to production.",
            host, env);
  }

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
  requestedMode_ = StartModeFromEnvironment();
  if (requestedMode_ == proto::StartMode::RpcOnly) {
    LogWarn("sdkhost: URNETWORK_RPC_ONLY is set â€” asking the service for an "
            "RPC-ONLY session. The DeviceRemote will be live and every screen "
            "driveable, but NO tunnel is created and no traffic is carried; the "
            "connect state will never report 'up'.");
  }
  try {
    spaceManager_ =
        urnet::newNetworkSpaceManager(Narrow(SdkStorageDir(false).wstring()));
    networkSpace_ = BuildNetworkSpace();
    api_ = networkSpace_->getApi();
    asyncLocalState_ = networkSpace_->getAsyncLocalState();
    localState_ = asyncLocalState_->getLocalState();
    // sign-up network-name availability (bound once; api-scoped)
    networkNameVc_ = urnet::newNetworkNameValidationViewController(*api_);
    networkNameVc_->start();
    SetupWalletCallbacks();

    service_.SetStateHandler([this](const proto::TunnelStatus& st) {
      // Remember the two facts only the SERVICE can know, so the statuses this
      // process synthesises (SessionStatus) do not overwrite them with their
      // defaults and render a healthy tunnel as degraded.
      AdoptServiceFacts(st);
      if (onTunnel_) onTunnel_(st);
    });
    service_.SetDisconnectHandler([this] { OnServiceDisconnected(); });
    service_.Connect();  // ok if the service isn't up yet; retried on demand

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

void SdkHost::LoginAsGuest(std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  urnet::NetworkCreateArgs args;
  args.terms = true;  // the sheet's button is gated on the terms consent
  args.guest_mode = true;

  api_->networkCreate(args, [this, done](std::optional<urnet::NetworkCreateResult> result,
                                         std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::LoggedOut);  // a request error, not a session error
      if (done) done(r);
      return;
    }
    if (result->network && result->network->by_jwt && !result->network->by_jwt->empty()) {
      RegisterNetworkClient(*result->network->by_jwt, done);
      return;
    }
    AuthResult r{false, false, "guest create returned no network"};
    SetAuthState(AuthState::Error, r.error);
    if (done) done(r);
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
  urnet::NetworkCreateArgs args;
  args.user_name = std::string();
  args.network_name = params.networkName;
  args.terms = params.terms;
  args.verify_use_numeric = true;
  if (params.useWalletAuth) {
    std::scoped_lock lock(mutex_);
    if (!pendingWalletAuth_) {
      AuthResult r{false, false, "no wallet sign-in is pending"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    args.wallet_auth = *pendingWalletAuth_;
  } else if (params.useAuthJwt) {
    std::scoped_lock lock(mutex_);
    if (!pendingAuthJwt_) {
      AuthResult r{false, false, "no SSO sign-in is pending"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    args.auth_jwt_type = "google";
    args.auth_jwt = *pendingAuthJwt_;
  } else {
    args.user_auth = params.userAuth;
    args.password = params.password;
  }
  if (!params.referralCode.empty()) args.referral_code = params.referralCode;

  api_->networkCreate(args, [this, done](std::optional<urnet::NetworkCreateResult> result,
                                         std::optional<std::string> err) {
    if (err || !result) {
      AuthResult r{false, false, err ? *err : "no result"};
      SetAuthState(AuthState::Error, r.error);
      if (done) done(r);
      return;
    }
    if (result->error && !result->error->message.empty()) {
      AuthResult r{false, false, result->error->message};
      SetAuthState(AuthState::LoggedOut);  // a form error, not a session error
      if (done) done(r);
      return;
    }
    if (result->verification_required) {
      AuthResult r{false, true, ""};
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
      }
      RegisterNetworkClient(*result->network->by_jwt, done);
      return;
    }
    AuthResult r{false, false, "create network returned no network"};
    SetAuthState(AuthState::Error, r.error);
    if (done) done(r);
  });
}

void SdkHost::UpgradeGuest(const std::string& networkName, const std::string& userAuth,
                           const std::string& password,
                           std::function<void(AuthResult)> done) {
  // No auth-state pushes on request errors: unlike the sign-in flows the caller
  // is still signed in (as the guest), and the create step surfaces the error
  // inline. Success lands in RegisterNetworkClient, which pushes LoggedIn once
  // the device is re-registered under the upgraded network's jwt.
  urnet::UpgradeGuestArgs args;
  args.network_name = networkName;
  args.user_auth = userAuth;
  args.password = password;

  api_->upgradeGuest(args, [this, done](std::optional<urnet::UpgradeGuestResult> result,
                                        std::optional<std::string> err) {
    if (err || !result) {
      if (done) done({false, false, err ? *err : "no result"});
      return;
    }
    if (result->error && !result->error->message.empty()) {
      if (done) done({false, false, result->error->message});
      return;
    }
    if (result->verification_required) {
      if (done) done({false, true, ""});  // the UI routes to the verify step
      return;
    }
    if (result->network && result->network->by_jwt && !result->network->by_jwt->empty()) {
      RegisterNetworkClient(*result->network->by_jwt, done);
      return;
    }
    if (done) done({false, false, "guest upgrade returned no network"});
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
                               std::function<void(bool ok)> done) {
  urnet::AuthVerifySendArgs args;
  args.user_auth = userAuth;
  args.use_numeric = true;
  api_->authVerifySend(args, [done](std::optional<urnet::AuthVerifySendResult> result,
                                    std::optional<std::string> err) {
    if (done) done(!err && result.has_value());
  });
}

void SdkHost::SendPasswordResetLink(const std::string& userAuth,
                                    std::function<void(bool ok)> done) {
  urnet::AuthPasswordResetArgs args;
  args.user_auth = userAuth;
  api_->authPasswordReset(args, [done](std::optional<urnet::AuthPasswordResetResult> result,
                                       std::optional<std::string> err) {
    if (done) done(!err && result.has_value());
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
    pendingInstantJwt_.reset();

    try {
      const bool official = (hostName == std::string(ids::kNetworkSpaceHostName));
      const bool explicitUrls = !apiUrl.empty() || !connectUrl.empty();

      urnet::NetworkSpaceKey key;
      key.host_name = hostName;
      key.env_name = std::string(ids::kNetworkSpaceEnvName);

      // The same value set BuildNetworkSpace writes, with the host-dependent
      // parts varied (iOS DeviceManager.applyNetworkSpace parity). `bundled` is
      // true only for the official host with no overrides: a bundled space
      // carries pinned endpoints a custom deployment does not have.
      urnet::NetworkSpaceValues values;
      values.bundled = official && !explicitUrls;
      values.net_expose_server_ips = true;
      values.net_expose_server_host_names = true;
      values.link_host_name = official ? std::string("ur.io") : hostName;
      values.migration_host_name = official ? std::string("bringyour.com") : std::string();
      values.store = "";
      values.wallet = "circle";
      values.sso_google = GoogleSignIn::Configured();
      values.env_secret = "";
      values.api_url = apiUrl;
      values.platform_url = connectUrl;

      networkSpace_ = spaceManager_->updateNetworkSpaceValues(key, values);
      spaceManager_->setActiveNetworkSpace(*networkSpace_);

      // Everything derived from the space has to be re-derived: the Api talks
      // to the new host, and the LocalState holds the new host's jwt.
      api_ = networkSpace_->getApi();
      asyncLocalState_ = networkSpace_->getAsyncLocalState();
      localState_ = asyncLocalState_->getLocalState();
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
  if (loggedIn) EnsureSession("network server change");
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

  api_->authNetworkClient(args, [this, byJwt, done](std::optional<urnet::AuthNetworkClientResult> result,
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
      bool ok = false;
      std::string why;
      {
        std::scoped_lock lock(mutex_);
        try {
          // SYNCHRONOUS setters, taken under the SAME lock as the bootstrap
          // that reads them straight back.
          //
          // These were asyncLocalState_->set*(..., [](bool){}): hand the
          // commit to the SDK's own thread and carry on. The very next
          // statement was BootstrapSession(), whose first act is
          // localState_->getByClientJwt() on THIS thread. On a fresh install
          // there is no earlier value to read, so the FIRST sign-in lost that
          // race and reported "no client credentials are stored for this
          // device" over an authLogin and an authNetworkClient that had both
          // just succeeded. Pressing sign in again worked, because by then the
          // async commit had landed — which is exactly what made it look like
          // a flaky server rather than our own ordering.
          localState_->setByJwt(byJwt);
          localState_->setByClientJwt(*result->by_client_jwt);
          loggedIn_.store(true, std::memory_order_release);
        } catch (const std::exception& e) {
          LogWarn("sdkhost: persist jwt failed: {}", e.what());
        }
        ok = BootstrapSession();
        why = bootstrapError_;
      }
      // THE SIGN-IN SUCCEEDED. Say so.
      //
      // This used to report AuthResult{ok=false} and AuthState::Error whenever
      // the TUNNEL BOOTSTRAP failed — service not running, service too old, a
      // mode refusal — over an authLogin and an authNetworkClient that had both
      // returned 200 and a jwt that is now on disk. On the seedphrase step that
      // surfaced as "There was an error signing in with your seedphrase",
      // which is the single most alarming thing this app can say to somebody
      // whose credential has no reset path: it reads as "your phrase is wrong".
      // It is not. The phrase was right and the account is fine.
      //
      // The resume path in Initialize() already got this right and explains
      // why (AuthState::Error makes the window derive loggedIn=false and dumps
      // an authenticated user onto the sign-in screen, and it LATCHES). The
      // login path now does the same thing: auth state goes LoggedIn, and the
      // reason the app is not carrying traffic goes out on the notice channel,
      // which is what that channel is for.
      SetAuthState(AuthState::LoggedIn);
      if (!ok) {
        LogError("sdkhost: signed in, but the session bootstrap failed: {}",
                 why.empty() ? "unknown" : why);
        PublishSessionFailure(why);
      }
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

// The challenge every client signs for wallet sign-in (macOS/Linux/android
// parity). No nonce: the server only enforces one when present.
static constexpr const char* kWalletSignInMessage = "Welcome to URnetwork";

void SdkHost::SetupWalletCallbacks() {
  wallet_.on_public_key = [this](std::string, WalletConnect::Provider provider) {
    // Solana connects first, then signs. Bittensor has no connect step (it
    // returns the address with the signature), so nothing to chain here.
    if (provider == WalletConnect::Provider::Bittensor) return;
    // a bare signature request carries its own message (Seeker verification);
    // sign-in signs the fixed challenge
    wallet_.SignMessage(walletSignDone_ ? walletSignMessage_ : kWalletSignInMessage);
  };
  wallet_.on_signature = [this](std::string publicKey, std::string signature,
                                WalletConnect::Provider provider) {
    if (auto done = std::exchange(walletSignDone_, nullptr)) {
      done(true, std::move(publicKey), std::move(signature), std::string());
      return;
    }
    AuthLoginWithWallet(publicKey, signature, kWalletSignInMessage, provider);
  };
  wallet_.on_error = [this](std::string err) {
    // A failed signature request is NOT a failed sign-in: the user is signed in
    // throughout, and pushing AuthState::Error here would tear the session down
    // because a browser tab was closed.
    if (auto signDone = std::exchange(walletSignDone_, nullptr)) {
      signDone(false, std::string(), std::string(), err);
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
void SdkHost::CancelPendingWalletFlows(const char* reason) {
  if (auto signDone = std::exchange(walletSignDone_, nullptr)) {
    LogWarn("sdkhost: a wallet signature request was superseded ({})", reason);
    signDone(false, std::string(), std::string(), reason);
  }
  if (auto authDone = std::exchange(walletAuthDone_, nullptr)) {
    LogWarn("sdkhost: a wallet sign-in was superseded ({})", reason);
    authDone({false, false, reason});
  }
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

void SdkHost::SignInWithBittensor(std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  {
    std::scoped_lock lock(mutex_);
    pendingWalletAuth_.reset();  // a fresh sign-in supersedes any retained auth
  }
  CancelPendingWalletFlows("superseded by a wallet sign-in");
  walletAuthDone_ = std::move(done);
  // one step: the bridge returns the address and the signature together
  wallet_.SignMessageBittensor(kWalletSignInMessage);
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

void SdkHost::HandleDeepLink(const std::string& url) {
  // Google SSO does NOT come back this way: Google issues custom-scheme
  // redirects to iOS/Android client types only, so the desktop flow uses a
  // loopback socket instead (GoogleSignIn.h). This stays wallet-only.
  wallet_.HandleDeepLink(url);
}

// ---- Sign in with Google (system browser, loopback OAuth + PKCE) ------------

bool SdkHost::SsoGoogleEnabled() {
  if (!GoogleSignIn::Configured()) return false;
  std::scoped_lock lock(mutex_);
  if (!networkSpace_) return false;
  try {
    return networkSpace_->getSsoGoogle();
  } catch (const std::exception& e) {
    LogWarn("sdkhost: read sso_google failed: {}", e.what());
    return false;
  }
}

bool SdkHost::HasPendingAuthJwt() {
  std::scoped_lock lock(mutex_);
  return pendingAuthJwt_.has_value();
}

void SdkHost::SignInWithGoogle(std::function<void(AuthResult)> done) {
  if (!GoogleSignIn::Configured()) {
    // Unreachable from the UI (the button is hidden), but a caller that got
    // here must not silently do nothing.
    if (done) done({false, false, "this build has no Google OAuth client id"});
    return;
  }
  SetAuthState(AuthState::Authenticating);
  {
    std::scoped_lock lock(mutex_);
    pendingAuthJwt_.reset();  // a fresh sign-in supersedes any retained token
  }
  google_.Start([this, done](std::string idToken, std::string error) {
    // On a GoogleSignIn worker thread. Errors here are already sentences.
    if (idToken.empty()) {
      AuthResult r{false, false, error.empty() ? "Google sign-in failed" : error};
      SetAuthState(AuthState::LoggedOut, r.error);
      if (done) done(r);
      return;
    }
    AuthLoginWithGoogle(idToken, done);
  });
}

void SdkHost::AuthLoginWithGoogle(const std::string& idToken,
                                  std::function<void(AuthResult)> done) {
  urnet::AuthLoginArgs args;
  args.auth_jwt_type = "google";
  args.auth_jwt = idToken;
  // UNDER mutex_, unlike every other caller here, because this one is the odd
  // one out: it runs on a GoogleSignIn WORKER thread, minutes after the button
  // was pressed, while the user is free to open Change Network API on the UI
  // thread — and ApplyNetworkServer reassigns api_ under this same lock, which
  // RELEASES the handle this line is about to call through. urnet::Api is
  // move-only (detail::Handle deletes its copy constructor), so there is no
  // way to take a private reference to it; holding the lock across the
  // dispatch is what there is. authLogin queues its callback onto an SDK
  // thread rather than running it inline, so this does not re-enter.
  std::scoped_lock lock(mutex_);
  if (!api_) {
    AuthResult r{false, false, "the network session went away during sign-in"};
    SetAuthState(AuthState::Error, r.error);
    if (done) done(r);
    return;
  }
  // The id token is a bearer credential; nothing below logs the args.
  api_->authLogin(args, [this, idToken, done](std::optional<urnet::AuthLoginResult> result,
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
    // Authenticated, but this Google identity has no network yet: retain the
    // token and let the UI route to the create-network step (name + terms, no
    // password), the same shape the wallet path uses.
    {
      std::scoped_lock lock(mutex_);
      pendingAuthJwt_ = idToken;
    }
    AuthResult r;
    r.auth_needs_network = true;
    SetAuthState(AuthState::LoggedOut);
    if (done) done(r);
  });
}

void SdkHost::AuthLoginWithWallet(const std::string& address, const std::string& signature,
                                  const std::string& message,
                                  WalletConnect::Provider provider) {
  urnet::WalletAuthArgs w;
  w.wallet_address = address;
  w.wallet_signature = signature;
  w.wallet_message = message;
  // TAO is sr25519 over an ss58 address; SOL is ed25519 over a base58 pubkey.
  w.blockchain = provider == WalletConnect::Provider::Bittensor ? urnet::TAO : urnet::SOL;
  urnet::AuthLoginArgs args;
  args.wallet_auth = w;
  api_->authLogin(args, [this, w](std::optional<urnet::AuthLoginResult> result,
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
      AuthResult r{false, false, result->error->message};
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
  if (!device_) {
    // ...but "no device" is not always "nothing to say". If the last bootstrap
    // FAILED, that reason stands until a session exists, and it must survive
    // long enough for a view that did not exist at the time to receive it.
    if (!sessionFailure_.empty()) {
      ModeNotice failed;
      failed.active = true;
      failed.kind = ModeNotice::Kind::SessionFailed;
      failed.message = sessionFailure_;
      DeliverModeNotice(failed);
      return;
    }
    DeliverModeNotice(ModeNotice{});
    return;
  }
  sessionFailure_.clear();  // a live session supersedes any earlier failure
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
  lastServiceDnsApplied_.store(st.dns_applied);
  // The third service-owned fact, adopted here for the same reason as the other
  // two: this process cannot observe it, and inferring it from a mode flag is
  // what let the app report a captured machine as disconnected.
  lastServiceRoutesInstalled_.store(st.routes_installed);
  // R1 for this process. Keyed on routes_installed and NOT on state: that field
  // is the one Protocol.h nominates as the answer to "is my traffic going
  // through the tunnel", it is read off the object that owns the routes, and it
  // is true for the window between the routes going in and the session being
  // reported Up — which is exactly the window in which an unbound app socket
  // would pick the tun and keep it. A status with routes down unbinds, so a
  // stop, a failure, or an rpc-only session all put this back.
  ApplySdkEgressBind(st.routes_installed ? st.egress_index4 : 0,
                     st.routes_installed ? st.egress_index6 : 0,
                     st.routes_installed ? "the service reports routes installed"
                                         : "the service reports no routes");
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
  if (!haveLocation) {
    st.state = proto::TunnelState::Stopped;
  } else {
    st.state = mode == proto::StartMode::RpcOnly ? proto::TunnelState::RpcOnly
                                                 : proto::TunnelState::Up;
  }
  return st;
}

bool SdkHost::BootstrapSession() {
  // caller holds mutex_
  // Cleared on entry and set on every failure path, so a caller that gets false
  // can tell the user WHY. Both callers used to report the same hardcoded
  // "failed to start tunnel session", which is actively misleading for a mode
  // mismatch or an out-of-date service.
  bootstrapError_.clear();
  const std::string clientJwt = localState_->getByClientJwt();
  if (clientJwt.empty()) {
    bootstrapError_ = "no client credentials are stored for this device";
    return false;
  }
  // The instance id THIS bootstrap will pair its DeviceRemote with.
  //
  // Mutable, and the reattach branch below overwrites it. The disk value is
  // only correct for a session THIS call is about to start: LocalState rotates
  // .instance_id on any changed by-client JWT string, and a JWT refresh
  // re-signs the same client, so the disk id drifts away from the id the
  // service's running DeviceLocal was born with. Reattaching with the drifted
  // id is what made DeviceLocalRpc.Sync refuse every sync of a reattached
  // session for its whole life (see RpcSessionBlob.h).
  std::string instanceId = localState_->getInstanceId();

  if (!service_.IsConnected() && !service_.Connect()) {
    bootstrapError_ =
        "the URnetwork service is not running or cannot be reached";
    LogError("sdkhost: service not reachable");
    return false;
  }

  try {
    std::string clientPem, serverCertPem, hostPort;

    proto::TunnelStatus hello = service_.Hello();
    // hello is a full TunnelStatus and it is the ONLY status a reattach ever
    // sees: nothing pushes an event for a session that was already running when
    // this process started. Its dns_applied and wfp_state were read for `state`
    // and `mode` and then dropped, so a reattached tunnel rendered with the
    // struct defaults — "dns not applied", "no leak guard" — over a session that
    // may be perfectly healthy, until some unrelated start/stop happened to
    // correct it.
    AdoptServiceFacts(hello);

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
    auto saved = LoadRpcSession();
    const bool liveIsSufficient =
        proto::IsSessionLive(hello.state) && hello.mode == requestedMode_;
    bool reattaching =
        liveIsSufficient && saved && hello.rpc_listen_hostport == saved->host_port;
    // MIGRATION. A blob written before instance_id existed (or one whose id is
    // not a shape the SDK can parse) cannot be paired, and there is no safe way
    // to fake it: an empty id maps to a nil *Id across the cgo boundary and
    // yields a DeviceRemote handle that answers nothing, and the nil UUID would
    // SKIP the service's pairing check rather than pass it. So this one launch
    // gives up the reattach and starts a session of its own, which restores the
    // user's tunnel AND writes a blob the next launch can pair with. It costs
    // the running tunnel a stop/start once, and only once, per machine.
    if (reattaching && saved->instance_id.empty()) {
      LogWarn("sdkhost: the saved rpc session at {} has no usable device "
              "instance id — it was written by a build from before the id was "
              "persisted. Reattaching without it would leave every rpc sync "
              "refused and the app reading Disconnected over a live tunnel, so "
              "this launch starts a fresh session instead. This happens once.",
              saved->host_port);
      reattaching = false;
    }
    if (reattaching) {
      clientPem = saved->client_pem;
      serverCertPem = saved->server_cert_pem;
      hostPort = saved->host_port;
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
      // fresh session: generate per-session RPC key material
      urnet::DeviceRpcKeyMaterial km = urnet::generateDeviceRpcKeyMaterial();
      hostPort = RandomLoopbackHostPort();

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
      cfg.mode = requestedMode_;
      // The kill switch now drives a WFP policy in the SERVICE, not just the
      // SDK's routeLocal flag, so the service has to know the persisted
      // preference before it installs the first route. CurrentKillSwitch()
      // reads LocalState when there is no device yet, which is exactly the
      // state we are in here.
      cfg.kill_switch = CurrentKillSwitch();
      // Seed split tunneling from the persisted per-app overrides so the driver is
      // correct at tunnel-up (device_ isn't connected yet - read the app LocalState).
      // PushLocalOverrideAppsToDriver re-applies it live once the device is up.
      if (localState_) {
        static std::atomic<bool> logged{false};
        if (auto ov = ReadSdkList(logged, "getBlockActionOverrides (local seed)",
                               [&] { return localState_->getBlockActionOverrides(); }))
          ComputeAppSplit(*ov, cfg.excluded_app_paths, cfg.allowlist_mode);
      }

      proto::TunnelStatus st = service_.StartTunnel(cfg);
      // The start reply is a full status and it is the FIRST one that can carry
      // a live egress index — the pushed event may or may not beat us here, and
      // waiting for it would leave the app's sockets in the tun for the gap.
      // Adopting it is idempotent with whatever arrives next.
      AdoptServiceFacts(st);
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
        // misreporting" to only one direction.
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
      clientPem = km.getClientPem();
      serverCertPem = km.getServerCertPem();
      // instanceId is the SAME string that went into cfg.instance_id above, so
      // the blob records the id the service's DeviceLocal was actually born
      // with. Reading LocalState again here instead would reintroduce the whole
      // bug on any refresh that lands during start_tunnel.
      SaveRpcSession({.client_pem = clientPem,
                      .server_cert_pem = serverCertPem,
                      .host_port = hostPort,
                      .instance_id = instanceId});
    }

    // The controlling DeviceRemote dials the service's mTLS RPC listener.
    device_ = urnet::newDeviceRemoteWithDefaults(*networkSpace_, clientJwt, instanceId);
    ++sessionGeneration_;
    device_->setRpcServer(clientPem, serverCertPem, hostPort);
    {
      std::scoped_lock lock(wfpStateMutex_);
      sessionRpcHostPort_ = hostPort;
    }
    // There IS a session, from here on. Set before the listeners below, because
    // the first status they push reads it.
    hasSession_.store(true, std::memory_order_release);

    // These session listeners are functional rather than presentational: auth
    // invalidation and tray connection state must keep working while hidden.
    subs_.push_back(device_->addAuthLogoutListener([this] {
      if (onAuthInvalid_) onAuthInvalid_();
    }));
    subs_.push_back(device_->addJwtRefreshListener([this](std::string) {
      if (onJwtRefreshed_) onJwtRefreshed_();
    }));
    subs_.push_back(device_->addConnectLocationChangeListener(
        [this](std::optional<urnet::ConnectLocation> location) {
          if (!onTunnel_) return;
          onTunnel_(SessionStatus(location.has_value()));
        }));
    subs_.push_back(device_->addRemoteChangeListener([this](bool remoteConnected) {
      // availability for the peers status line: with the rpc down the peer
      // count is unavailable (not zero), and the UI renders it disabled
      if (onRemoteChanged_) onRemoteChanged_(remoteConnected);
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
  } else if (device_) {
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
  if (device_) {
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
    const int64_t nowMillis =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    std::scoped_lock healthLock(healthMutex_);
    s.health = healthTracker_.Update(hs, nowMillis);
    s.provenProviderCount = proven;
    s.healthReevalAtMillis = healthTracker_.ReevalAtMillis();
  }
  return s;
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

  // initial snapshots
  PublishThroughput();
  PublishContractRows();
  PublishBlockActions();
  PublishBlockStats();
  PublishSplitRules();
  PushLocalOverrideAppsToDriver();  // seed the driver once the device + service are up
  if (onDnsSettings_) onDnsSettings_(device_->getDnsResolverSettings());
  if (onBlockerEnabled_) onBlockerEnabled_(device_->getBlockerEnabled());
}

void SdkHost::PublishThroughput() {
  if (!contractVc_) return;
  std::vector<urnet::ThroughputPoint> points;
  static std::atomic<bool> logged{false};
  if (auto p = ReadSdkList(logged, "getThroughputPoints",
                        [&] { return contractVc_->getThroughputPoints(); }))
    points = std::move(*p);
  int64_t window = contractVc_->getWindowDurationSeconds();
  if (window <= 0) window = 60;
  {
    std::scoped_lock lock(drawerMutex_);
    lastThroughputPoints_ = points;
    throughputWindowSeconds_ = window;
  }
  if (onThroughput_) onThroughput_(std::move(points), window);
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

void SdkHost::ClearDrawer() {
  {
    std::scoped_lock lock(drawerMutex_);
    lastThroughputPoints_.clear();
    lastContractRows_.clear();
    lastBlockActions_.clear();
    lastAllowedCount_ = 0;
    lastBlockedCount_ = 0;
    lastSplitRules_.clear();
  }
  if (onThroughput_) onThroughput_({}, 60);
  if (onContractRows_) onContractRows_({});
  if (onBlockActions_) onBlockActions_({});
  if (onBlockStats_) onBlockStats_(0, 0);
  if (onSplitRules_) onSplitRules_({});
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

void SdkHost::ConnectBestAvailable() {
  SessionRequest r;
  r.kind = ConnectKind::BestAvailable;
  r.reason = "connect (best available)";
  RequestSession(std::move(r));
}

void SdkHost::Connect(const std::string& connectLocationJson) {
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
  if (RowClickIsCurrent(
          [&](const std::optional<urnet::ConnectLocation>& sel) {
            return IsLocationSelected(sel, location);
          })) {
    // Already there. The only work left is un-queuing a newer intent, so a
    // "click B, regret it, click A again" round trip ends with zero rebuilds.
    CancelPendingRowConnect("re-selected the current location");
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
  SessionRequest r;
  r.kind = ConnectKind::BestAvailable;
  r.reason = "connect (row click, best available)";
  r.coalesced = true;
  r.notBefore = std::chrono::steady_clock::now() + kRowClickSettle;
  RequestSession(std::move(r));
}

void SdkHost::EnsureSession(const char* reason) {
  SessionRequest r;
  r.kind = ConnectKind::None;
  r.reason = reason;
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
  const bool covered = pendingRequested_ && request.kind == ConnectKind::None &&
                       pending_.kind != ConnectKind::None;
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

    bool ok = false;
    {
      std::scoped_lock lock(mutex_);
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
        LogInfo("sdkhost: starting a session ({})", req.reason);
        ok = BootstrapSession();
      } else {
        ok = device_.has_value();
      }

      const bool connecting = req.kind == ConnectKind::BestAvailable ||
                              req.kind == ConnectKind::Location;
      if (ok) {
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
      } else {
        // EVERY failing path says why, on the channel built for it. Under the
        // lock: PublishSessionFailure writes sessionFailure_, which mutex_
        // guards, and the handlers it invokes only marshal (see the threading
        // note on SetModeNoticeHandler).
        const std::string why = bootstrapError_;
        LogError("sdkhost: '{}' could not start a session: {}", req.reason,
                 why.empty() ? "unknown" : why);
        PublishSessionFailure(why);
        // ...and start watching, if the reason was that there is no service to
        // talk to. THE WATCHDOG CANNOT ONLY ARM ON A DROP: the commonest way
        // into this state is a launch that found no service at all, which is
        // never a "drop" because there was never a connection. That is the
        // state this machine boots into every time (the service is started by
        // hand), and without this the app would sit signed-in and sessionless
        // until something asked it again.
        //
        // Gated on the pipe genuinely not being there, NOT merely on the
        // bootstrap having failed. If the pipe IS listening and the handshake
        // still failed, retrying on a timer would rediscover the same listening
        // pipe every 3 s and fail the same way, logging it each time — a spin
        // that reports itself as progress. That case is left to the next
        // Connect press, which is a person deciding to try again.
        if (!service_.IsConnected() &&
            !::WaitNamedPipeW(ids::kControlPipeName, NMPWAIT_NOWAIT)) {
          ScheduleServiceRetry();
        }
      }
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
  // A CHEAP PROBE, not a bootstrap. Retrying BootstrapSession on a timer would
  // publish a session-failure notice every few seconds at the user, which is
  // worse than the silence it replaces. WaitNamedPipe with a zero timeout only
  // asks whether an instance is available; it opens nothing and disturbs
  // nothing (Startup.cpp's diagnostics use the same call).
  constexpr auto kInterval = std::chrono::seconds(3);
  LogInfo("sdkhost: watching for the URnetwork service to come back");
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(watchdogMutex_);
      watchdogCv_.wait_for(lock, kInterval, [this] { return watchdogStop_; });
      if (watchdogStop_) {
        watchdogRunning_ = false;
        return;
      }
    }
    if (service_.IsConnected()) break;  // something else already reconnected
    if (!::WaitNamedPipeW(ids::kControlPipeName, NMPWAIT_NOWAIT)) continue;
    LogInfo("sdkhost: the URnetwork service is listening again");
    // Only a signed-in client has a session to restore. A signed-out one gets
    // its session from the sign-in itself (RegisterNetworkClient).
    if (loggedIn_.load(std::memory_order_acquire)) {
      EnsureSession("the service came back");
    }
    break;
  }
  std::scoped_lock lock(watchdogMutex_);
  watchdogRunning_ = false;
}

void SdkHost::StopServiceWatchdog() {
  {
    std::scoped_lock lock(watchdogMutex_);
    watchdogStop_ = true;
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
  syncDeadline_ = std::chrono::steady_clock::now() + kSyncSettleDeadline;
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

    CheckSessionSync(generation, reattached);

    {
      std::scoped_lock lock(syncMutex_);
      // A bootstrap that landed while the check was running re-armed us; go
      // round again rather than exit and leave that session unwatched.
      if (!syncStop_ && syncGeneration_ != generation) continue;
      syncRunning_ = false;
      return;
    }
  }
}

void SdkHost::CheckSessionSync(std::uint64_t generation, bool reattached) {
  // Must not be called holding mutex_ — it takes mutex_ here and RELEASES it
  // before asking the session worker for a replacement, because that worker
  // takes mutex_ for the whole of the bootstrap it is being asked to run.
  bool degrade = false;
  std::optional<urnet::ConnectLocation> resume;
  {
    std::scoped_lock lock(mutex_);
    if (generation != sessionGeneration_ || !device_) return;  // superseded

    std::string syncError;
    bool remoteConnected = false;
    try {
      syncError = device_->getSyncError();
      remoteConnected = device_->getRemoteConnected();
    } catch (const std::exception& e) {
      LogWarn("sdkhost: could not read the rpc sync state: {}", e.what());
      return;
    }

    if (remoteConnected) {
      // The pairing holds. A non-empty error alongside a live remote is a sync
      // that failed and then recovered — worth one line, never worth an action.
      if (!syncError.empty()) {
        LogInfo("sdkhost: the device rpc is synced; an earlier sync had "
                "reported '{}' and it has since recovered.", syncError);
      }
      return;
    }
    if (syncError.empty()) {
      // Not connected and nothing refused: the ordinary "the local end has not
      // answered YET" case (GetSyncError's own documented pairing with
      // GetRemoteConnected). Slow is not broken, and the SDK keeps retrying.
      LogWarn("sdkhost: the device rpc has not synced within {}ms and the "
              "service has refused nothing — it is still coming up. Nothing to "
              "do; the sdk keeps retrying.",
              static_cast<long long>(kSyncSettleDeadline.count()));
      return;
    }

    // A REFUSAL. This is the state that used to be invisible: the app renders,
    // logs "session bootstrapped", and every value it shows is the empty
    // fallback of a DeviceRemote whose service pointer will never be set,
    // because the remote re-sends the same rejected pairing every 500ms for as
    // long as the process lives. Say the SDK's own words, at WARN, once.
    LogWarn("sdkhost: THE SERVICE REFUSED THIS SESSION'S DEVICE RPC — '{}'. "
            "Nothing this app shows about the tunnel can be trusted while that "
            "is true: the connect view controller has no service to ask, so it "
            "reports no location, no providers and no throughput, and the app "
            "reads Disconnected over whatever the service is really doing. The "
            "sdk will retry this forever and it can never succeed.",
            syncError);

    if (!reattached) {
      // A session THIS process just started, refused. Restarting it would
      // rebuild the same pairing and be refused the same way, so there is no
      // automatic recovery here — only the truth, on the channel built for it.
      PublishSessionFailure(
          "The URnetwork service refused this app's control connection. "
          "Nothing is connected. Restart the URnetwork service, then try "
          "again.");
      return;
    }

    // A REATTACH, refused. Here there IS something better to do: give up the
    // adopted session and start one of our own, which pairs by construction.
    // That costs the running tunnel a stop and a start — deliberately, because
    // the alternative on this branch is an app that stays wrong until the user
    // works out that killing the service is what fixes it.
    if (localState_) {
      try {
        resume = localState_->getConnectLocation();
      } catch (const std::exception& e) {
        LogWarn("sdkhost: could not read the stored connect location: {}", e.what());
      }
    }
    LogWarn("sdkhost: dropping the reattached session and starting a fresh one "
            "(the tunnel stops and restarts). {}",
            resume ? "The stored destination will be reconnected."
                   : "No stored destination — the session comes up idle.");
    try {
      // Also clears the saved rpc blob, so the next launch cannot reattach to
      // the session this refusal proved unusable.
      TeardownSessionLocked();
    } catch (const std::exception& e) {
      LogError("sdkhost: teardown of the refused session failed: {}", e.what());
      return;
    }
    degrade = true;
  }

  if (!degrade) return;
  SessionRequest r;
  r.reason = "the reattached session's device rpc was refused";
  if (resume) {
    r.kind = ConnectKind::Location;
    r.location = std::move(resume);
  }
  RequestSession(std::move(r));
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

void SdkHost::ClosePresentationLocked() {
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
    return;
  }
  if (peerVc_) device_->closePeerViewController(*peerVc_);
  peerVc_.reset();
  if (locationsVc_) device_->closeLocationsViewController(*locationsVc_);
  locationsVc_.reset();
  if (contractDetailsVc_) {
    device_->closeContractDetailsViewController(*contractDetailsVc_);
  }
  contractDetailsVc_.reset();
  if (blockVc_) device_->closeBlockActionViewController(*blockVc_);
  blockVc_.reset();
  if (contractVc_) device_->closeContractViewController(*contractVc_);
  contractVc_.reset();
  if (connectVc_) device_->closeConnectViewController(*connectVc_);
  connectVc_.reset();
  ClearDrawer();
}

void SdkHost::SetPresentationActive(bool active) {
  std::scoped_lock lock(mutex_);
  if (presentationActive_ == active) return;
  presentationActive_ = active;
  if (!active) {
    ClosePresentationLocked();
    return;
  }
  // Stats and the drawer are genuinely device-scoped; the provider list is not,
  // so the `if (!device_) return;` that used to sit here has been narrowed to
  // the two things it is actually true of. Returning early on no-device meant
  // alt-tabbing back with no service running re-armed NOTHING - which is the
  // same bug the block below describes, one source further down.
  if (device_) {
    SubscribeStats();
    SubscribeDrawer();
  }
  // The other half of ClosePresentationLocked. That function closes FOUR feeds
  // (stats, drawer, locations, peers) and this one used to put back only two -
  // so the locations/peers view controllers, their listeners and the snapshot
  // they hold were destroyed by any window DEACTIVATION and never rebuilt.
  // Nothing else rebuilt them either: the only openers were a chooser-sheet
  // open and NetworkPage::SetSelected, which runs on a navigation CHANGE, so a
  // window that came back to the destination it left on stayed empty.
  //
  // Now unconditional, so it re-arms the api source too: the cache normally
  // makes it a no-op, and a failed previous fetch is retried here.
  EnsureLocationsLocked();
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
  ClosePresentationLocked();
  subs_.clear();
  if (device_) { device_->close(); device_.reset(); }
  // A pending rpc-sync check must not act on the session that is ending — its
  // generation stops matching here, which is the whole point of the counter.
  ++sessionGeneration_;
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
  ClearRpcSession();
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

void SdkHost::Logout() {
  std::scoped_lock lock(mutex_);
  try {
    pendingWalletAuth_.reset();
    pendingAuthJwt_.reset();
    // A pending instant network belongs to whoever was mid-signup, not to the
    // session being ended; dropping it here means a later Confirm cannot
    // register a device against a stale jwt.
    pendingInstantJwt_.reset();
    TeardownSessionLocked();
    // Explicit logout deliberately severs the device identity: clear the
    // service-persisted key material (TunnelController::Logout) so the next
    // login starts with a fresh identity.
    if (service_.IsConnected()) service_.Logout();
    if (asyncLocalState_) asyncLocalState_->logout([](bool) {});
    // Before SetAuthState, and not waiting on the async logout above: the auth
    // handler runs synchronously from here and the window asks IsLoggedIn().
    loggedIn_.store(false, std::memory_order_release);
    sessionFailure_.clear();  // belongs to the session that just ended
    SetAuthState(AuthState::LoggedOut);
    LogInfo("sdkhost: logged out");
  } catch (const std::exception& e) {
    LogError("sdkhost: logout failed: {}", e.what());
  }
}

}  // namespace urnw
