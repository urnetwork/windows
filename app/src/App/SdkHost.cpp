// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "SdkHost.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <random>
#include <thread>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "Ids.h"
#include "Log.h"
#include "Paths.h"
#include "Strings.h"

namespace urnw {
namespace {

// Persisted RPC session (last-good), mirroring macOS RpcSessionStore. Lets the
// app reattach its DeviceRemote to a still-running service tunnel.
struct RpcSession {
  std::string client_pem;
  std::string server_cert_pem;
  std::string host_port;
};

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
  nlohmann::json j = {{"client_pem", s.client_pem},
                      {"server_cert_pem", s.server_cert_pem},
                      {"host_port", s.host_port}};
  std::ofstream f(RpcSessionFile(), std::ios::trunc);
  if (f) f << j.dump();
}

std::optional<RpcSession> LoadRpcSession() {
  std::ifstream f(RpcSessionFile());
  if (!f) return std::nullopt;
  try {
    nlohmann::json j = nlohmann::json::parse(f);
    RpcSession s;
    s.client_pem = j.value("client_pem", "");
    s.server_cert_pem = j.value("server_cert_pem", "");
    s.host_port = j.value("host_port", "");
    if (s.host_port.empty()) return std::nullopt;
    return s;
  } catch (...) {
    return std::nullopt;
  }
}

void ClearRpcSession() {
  std::error_code ec;
  std::filesystem::remove(RpcSessionFile(), ec);
}

}  // namespace

SdkHost::~SdkHost() {
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
  values.sso_google = false;
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
  if (const auto host = EnvVar(L"URNETWORK_NETWORK_HOST"); !host.empty()) {
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

  return spaceManager_->updateNetworkSpaceValues(key, values);
}

bool SdkHost::Initialize() {
  std::scoped_lock lock(mutex_);
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
      if (onTunnel_) onTunnel_(st);
    });
    service_.Connect();  // ok if the service isn't up yet; retried on demand

    if (!localState_->getByClientJwt().empty()) {
      SetAuthState(AuthState::LoggedIn);
      // Resume the session off the UI path.
      //
      // The result is CONSUMED. It used to be discarded, so every bootstrap
      // failure on resume â€” service down, service too old, a mode refusal â€”
      // produced a logged-in home screen with no DeviceRemote, no dialog and no
      // error state, with the only evidence a LogError in a file a WinUI3 app
      // never shows anyone. A failure the user cannot see is a failure that
      // gets reported as "the app just doesn't work".
      std::thread([this] {
        bool ok = false;
        std::string why;
        {
          std::scoped_lock lock(mutex_);
          ok = BootstrapSession();
          why = bootstrapError_;
        }
        if (!ok) {
          // NOT AuthState::Error. That enum means "authentication failed", and
          // the window derives `loggedIn = (state == LoggedIn)` from it â€” so
          // reporting a transport failure that way dumps a user whose JWT is
          // completely intact onto the sign-in screen, and it LATCHES: nothing
          // re-runs the bootstrap, and the stored state is re-applied on every
          // window show, so starting the service does not recover it. The
          // trigger is the default state of a dev box: signed in once, service
          // not running.
          //
          // The auth state stays LoggedIn (already set above) and the reason
          // goes out on the notice channel, which exists precisely to carry
          // "why this app is not carrying traffic" without touching auth.
          LogError("sdkhost: session bootstrap failed on resume: {}",
                   why.empty() ? "unknown" : why);
          PublishSessionFailure(why);
        }
      }).detach();
    } else {
      SetAuthState(AuthState::LoggedOut);
    }
    return true;
  } catch (const std::exception& e) {
    LogError("sdkhost: initialize failed: {}", e.what());
    SetAuthState(AuthState::Error, e.what());
    return false;
  }
}

bool SdkHost::IsLoggedIn() {
  std::scoped_lock lock(mutex_);
  return localState_ && !localState_->getByClientJwt().empty();
}

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
        pendingWalletAuth_.reset();  // consumed (wallet mode) or unused
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
      try {
        // persist the network + client JWT for the device
        asyncLocalState_->setByJwt(byJwt, [](bool) {});
        asyncLocalState_->setByClientJwt(*result->by_client_jwt, [](bool) {});
      } catch (const std::exception& e) {
        LogWarn("sdkhost: persist jwt failed: {}", e.what());
      }
      bool ok = false;
      std::string why;
      {
        std::scoped_lock lock(mutex_);
        ok = BootstrapSession();
        why = bootstrapError_;
      }
      // Name the ACTUAL cause. The old hardcoded "failed to start tunnel
      // session" was misleading for a mode mismatch or an out-of-date service,
      // and it was the only thing the user ever saw.
      AuthResult r{ok, false,
                   ok ? "" : (why.empty() ? "failed to start a session with the "
                                            "URnetwork service"
                                          : why)};
      SetAuthState(ok ? AuthState::LoggedIn : AuthState::Error, r.error);
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
    auto done = walletAuthDone_;
    walletAuthDone_ = nullptr;
    SetAuthState(AuthState::Error, err);
    if (done) done({false, false, err});
  };
}

void SdkHost::SignInWithSolana(WalletConnect::Provider provider,
                               std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  {
    std::scoped_lock lock(mutex_);
    pendingWalletAuth_.reset();  // a fresh sign-in supersedes any retained auth
  }
  walletSignDone_ = nullptr;  // ...and any pending bare signature request
  walletAuthDone_ = std::move(done);
  wallet_.Connect(provider);  // opens the browser; the rest continues on the deep-link callback
}

void SdkHost::SignInWithBittensor(std::function<void(AuthResult)> done) {
  SetAuthState(AuthState::Authenticating);
  {
    std::scoped_lock lock(mutex_);
    pendingWalletAuth_.reset();  // a fresh sign-in supersedes any retained auth
  }
  walletSignDone_ = nullptr;
  walletAuthDone_ = std::move(done);
  // one step: the bridge returns the address and the signature together
  wallet_.SignMessageBittensor(kWalletSignInMessage);
}

void SdkHost::SignWithSolanaWallet(
    WalletConnect::Provider provider, const std::string& message,
    std::function<void(bool, std::string, std::string, std::string)> done) {
  // No SetAuthState here on purpose: this is not a sign-in and the session must
  // not move (see on_error above).
  walletAuthDone_ = nullptr;  // a signature request supersedes a pending sign-in
  walletSignMessage_ = message;
  walletSignDone_ = std::move(done);
  wallet_.Connect(provider);  // continues on the deep-link callback
}

void SdkHost::HandleDeepLink(const std::string& url) {
  wallet_.HandleDeepLink(url);  // returns false for non-wallet links (future: OAuth)
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
// The persistent "this app is not carrying traffic" notice.
void SdkHost::PublishModeNotice() {
  if (!onModeNotice_) return;
  // No session, nothing to say about one. This gate is load-bearing rather
  // than defensive: the notice derives from sessionMode_, whose default is
  // RpcOnly (the mode that claims less, so a stray read cannot render as
  // connected) â€” so without it an ordinary LOGGED-OUT launch publishes a
  // confident claim that the service is running with --rpc-only.
  // RefreshModeNotice() is public and is exactly what a view calls when it is
  // constructed, which makes that the common path, not an edge case.
  if (!device_) {
    onModeNotice_(ModeNotice{});
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
  onModeNotice_(n);
}

// "There is no session, and here is why." The user remains SIGNED IN: this is
// a transport/service failure, not an authentication one, and routing them to
// the sign-in screen would destroy a perfectly good session.
void SdkHost::PublishSessionFailure(const std::string& why) {
  if (!onModeNotice_) return;
  ModeNotice n;
  n.active = true;
  n.kind = ModeNotice::Kind::SessionFailed;
  n.message = why.empty()
                  ? "Could not start a session with the URnetwork service. "
                    "Nothing is connected."
                  : why + " Nothing is connected.";
  onModeNotice_(n);
}

proto::TunnelStatus SdkHost::SessionStatus(bool haveLocation) const {
  proto::TunnelStatus st;
  const proto::StartMode mode = sessionMode_.load();
  st.mode = mode;
  // True for the whole life of a tunnel session: step 6 ran before the service
  // ever reported it live. Not inferred from the location â€” a tunnel with no
  // location selected still has its routes installed.
  st.routes_installed = mode == proto::StartMode::Tunnel;
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
  const std::string instanceId = localState_->getInstanceId();

  if (!service_.IsConnected() && !service_.Connect()) {
    bootstrapError_ =
        "the URnetwork service is not running or cannot be reached";
    LogError("sdkhost: service not reachable");
    return false;
  }

  try {
    std::string clientPem, serverCertPem, hostPort;

    proto::TunnelStatus hello = service_.Hello();

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
    if (liveIsSufficient && saved && hello.rpc_listen_hostport == saved->host_port) {
      clientPem = saved->client_pem;
      serverCertPem = saved->server_cert_pem;
      hostPort = saved->host_port;
      sessionMode_.store(hello.mode);
      LogInfo("sdkhost: reattaching to live {} session at {} (routes_installed={})",
              proto::ToString(hello.mode), hostPort,
              hello.routes_installed ? "yes" : "no");
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
      // Seed split tunneling from the persisted per-app overrides so the driver is
      // correct at tunnel-up (device_ isn't connected yet - read the app LocalState).
      // PushLocalOverrideAppsToDriver re-applies it live once the device is up.
      if (localState_) {
        if (auto ov = localState_->getBlockActionOverrides())
          ComputeAppSplit(*ov, cfg.excluded_app_paths, cfg.allowlist_mode);
      }

      proto::TunnelStatus st = service_.StartTunnel(cfg);
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
      SaveRpcSession({clientPem, serverCertPem, hostPort});
    }

    // The controlling DeviceRemote dials the service's mTLS RPC listener.
    device_ = urnet::newDeviceRemoteWithDefaults(*networkSpace_, clientJwt, instanceId);
    device_->setRpcServer(clientPem, serverCertPem, hostPort);

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
    }

    if (onTunnel_) onTunnel_(SessionStatus(device_->getConnectLocation().has_value()));

    // Raise the persistent notice LAST, once the session is actually usable, so
    // it can never appear on a bootstrap that then failed. It is deliberately
    // not dismissible: it describes a property of the whole session, not an
    // event, and it stays true until the app is restarted against a normal
    // service.
    PublishModeNotice();

    LogInfo("sdkhost: session bootstrapped (mode={} rpc={})",
            proto::ToString(sessionMode_.load()), hostPort);
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
    auto grid = connectVc_->getGrid();
    s.providerCount = grid.getWindowCurrentSize();
  } else if (device_) {
    s.connected = device_->getConnectLocation().has_value();
    s.connectionStatus = s.connected ? "DESTINATION_SET" : "DISCONNECTED";
  }
  if (contractVc_) {
    // Most recent throughput point that has a Remote (tunneled) sample.
    if (auto pts = contractVc_->getThroughputPoints(); pts && !pts->empty()) {
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
  if (auto p = contractVc_->getThroughputPoints()) points = std::move(*p);
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
  if (auto list = contractDetailsVc_->getContractRows()) {
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
  if (auto list = blockVc_->getBlockActions()) {
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
  if (auto list = device_->getBlockActionOverrides()) {
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

// ---- location/provider chooser --------------------------------------------
// The bucketed location feed + the connected, provide-enabled peers pinned atop
// the chooser. Opened lazily on the first chooser open; start() kicks the
// initial load (filterLocations("")). The listeners fire on SDK callback
// threads and only marshal (never re-enter SdkHost), so pushing the initial
// snapshot under mutex_ here is safe.
void SdkHost::EnsureLocations() {
  std::scoped_lock lock(mutex_);
  if (!presentationActive_ || !device_ || locationsVc_) return;
  locationsVc_ = device_->openLocationsViewController();
  presentationSubs_.push_back(locationsVc_->addFilteredLocationsListener(
      [this](std::optional<urnet::FilteredLocations> locations, std::string state) {
        if (onLocations_) onLocations_(std::move(locations), std::move(state));
      }));
  locationsVc_->start();
  // PeerViewController: connected AND provide-enabled peers only (SDK filters).
  peerVc_ = device_->openPeerViewController();
  presentationSubs_.push_back(peerVc_->addPeersListener(
      [this](std::optional<urnet::NetworkPeerList> peers) {
        if (onPeers_) onPeers_(std::move(peers));
      }));
  peerVc_->start();
  // seed the chooser + the drawer's peer-count sub-label (the listeners only
  // fire on later changes)
  if (onLocations_) {
    onLocations_(locationsVc_->getFilteredLocations(),
                 locationsVc_->getFilteredLocationState());
  }
  if (onPeers_) onPeers_(peerVc_->getPeers());
}

void SdkHost::SetLocationFilter(const std::string& query) {
  std::scoped_lock lock(mutex_);
  if (locationsVc_) locationsVc_->filterLocations(query);
}

std::optional<urnet::FilteredLocations> SdkHost::CurrentFilteredLocations() {
  std::scoped_lock lock(mutex_);
  if (locationsVc_) return locationsVc_->getFilteredLocations();
  return std::nullopt;
}

std::string SdkHost::CurrentFilteredLocationState() {
  std::scoped_lock lock(mutex_);
  if (locationsVc_) return locationsVc_->getFilteredLocationState();
  return std::string();
}

std::optional<urnet::NetworkPeerList> SdkHost::ConnectedProvidePeers() {
  std::scoped_lock lock(mutex_);
  if (peerVc_) return peerVc_->getPeers();
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

void SdkHost::ConnectBestAvailable() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) {
    connectVc_->connectBestAvailable();
  } else if (device_) {
    auto controller = device_->openConnectViewController();
    controller.connectBestAvailable();
    device_->closeConnectViewController(controller);
  }
}

void SdkHost::Connect(const std::string& connectLocationJson) {
  std::scoped_lock lock(mutex_);
  try {
    urnet::ConnectLocation loc =
        nlohmann::json::parse(connectLocationJson).get<urnet::ConnectLocation>();
    if (connectVc_) {
      connectVc_->connect(loc);
    } else if (device_) {
      auto controller = device_->openConnectViewController();
      controller.connect(loc);
      device_->closeConnectViewController(controller);
    }
  } catch (const std::exception& e) {
    LogWarn("sdkhost: connect parse failed: {}", e.what());
  }
}

// Connect to an SDK-supplied ConnectLocation as-is (the chooser already holds
// the typed struct; skip the json round-trip). connect() takes an optional.
void SdkHost::Connect(const urnet::ConnectLocation& location) {
  std::scoped_lock lock(mutex_);
  if (connectVc_) {
    connectVc_->connect(location);
  } else if (device_) {
    auto controller = device_->openConnectViewController();
    controller.connect(location);
    device_->closeConnectViewController(controller);
  }
}

void SdkHost::Disconnect() {
  std::scoped_lock lock(mutex_);
  if (connectVc_) {
    connectVc_->disconnect();
  } else if (device_) {
    auto controller = device_->openConnectViewController();
    controller.disconnect();
    device_->closeConnectViewController(controller);
  }
}

void SdkHost::ClosePresentationLocked() {
  presentationSubs_.clear();
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
  if (!device_) return;
  SubscribeStats();
  SubscribeDrawer();
}

void SdkHost::TeardownSessionLocked() {
  ClosePresentationLocked();
  subs_.clear();
  if (device_) { device_->close(); device_.reset(); }
  provideHasNetworkKey_ = false;
  // Session teardown only: stop the tunnel but keep the service-persisted
  // device identity (key material). The identity is device-scoped, not
  // session-scoped â€” RegisterNetworkClient's re-registration under a new jwt
  // (guest upgrade, verify after an upgrade) must not rotate the key peers
  // use to verify this device. Only the explicit Logout() below severs it.
  if (service_.IsConnected()) {
    service_.StopTunnel();
  }
  // No session, so no tunnel â€” reset to the mode that claims less, not to
  // Tunnel. A status built between this teardown and the next bootstrap must
  // not be able to render "connected".
  sessionMode_.store(proto::StartMode::RpcOnly);
  ClearRpcSession();
}

void SdkHost::Logout() {
  std::scoped_lock lock(mutex_);
  try {
    pendingWalletAuth_.reset();
    TeardownSessionLocked();
    // Explicit logout deliberately severs the device identity: clear the
    // service-persisted key material (TunnelController::Logout) so the next
    // login starts with a fresh identity.
    if (service_.IsConnected()) service_.Logout();
    if (asyncLocalState_) asyncLocalState_->logout([](bool) {});
    SetAuthState(AuthState::LoggedOut);
    LogInfo("sdkhost: logged out");
  } catch (const std::exception& e) {
    LogError("sdkhost: logout failed: {}", e.what());
  }
}

}  // namespace urnw
