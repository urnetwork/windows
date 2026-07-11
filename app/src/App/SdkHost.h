// SdkHost is the app's DeviceManager equivalent: it owns the NetworkSpace, Api,
// LocalState, and the DeviceRemote, and coordinates the service to bring up the
// tunnel. Auth results and tunnel/connection state are surfaced to the UI via
// handlers (invoked on background threads; the UI marshals to its thread).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Sdk.h"
#include "ServiceClient.h"
#include "WalletConnect.h"

namespace urnw {

enum class AuthState { LoggedOut, Authenticating, LoggedIn, Error };

struct AuthResult {
  bool ok = false;
  bool verification_required = false;
  std::string error;
};

// Snapshot of live connection / throughput / provide stats. Pushed to the UI on
// SDK listener callbacks (macOS parity: listener-push, not polling).
struct LiveStats {
  std::string connectionStatus;   // getConnectionStatus() (CONNECTED/CONNECTING/...)
  bool connected = false;
  int64_t providerCount = 0;      // grid window current size (providers in window)
  int64_t downBitsPerSecond = 0;  // remote (tunneled) ingress bit rate
  int64_t upBitsPerSecond = 0;    // remote (tunneled) egress bit rate
  bool insufficientBalance = false;
  bool provideEnabled = false;
  bool providePaused = false;
  int64_t provideClients = 0;     // connected peers while providing
};

class SdkHost {
 public:
  using AuthStateHandler = std::function<void(AuthState, const std::string& error)>;
  using TunnelStateHandler = std::function<void(const proto::TunnelStatus&)>;
  using StatsHandler = std::function<void(const LiveStats&)>;

  SdkHost() = default;
  ~SdkHost();

  // Build the NetworkSpace/Api/LocalState and connect to the service. If a
  // client JWT is already persisted, resumes the session (reattaching to a live
  // tunnel or restarting it). Call once at startup.
  bool Initialize();

  bool IsLoggedIn();

  // Auth (async; result delivered on the SDK callback thread).
  void LoginWithPassword(const std::string& userAuth, const std::string& password,
                         std::function<void(AuthResult)> done);
  void LoginWithCode(const std::string& authCode, std::function<void(AuthResult)> done);

  // Sign in with a Solana wallet (Phantom/Solflare) via the ur.io/wallet-connect
  // browser bridge: connect -> sign a challenge -> authLogin{wallet_auth}. The
  // urnetwork:// callback must be routed back in via HandleDeepLink.
  void SignInWithSolana(WalletConnect::Provider provider, std::function<void(AuthResult)> done);

  // Route a urnetwork:// deep link (wallet callback; later OAuth) into the host.
  // Called from the app's protocol-activation handler.
  void HandleDeepLink(const std::string& url);

  void Logout();

  // Connect flow via the ConnectViewController.
  void ConnectBestAvailable();
  void Connect(const std::string& connectLocationJson);
  void Disconnect();

  // Split tunneling: excluded process image paths.
  void SetExcludedApps(const std::vector<std::string>& paths);

  void SetAuthStateHandler(AuthStateHandler h) { onAuth_ = std::move(h); }
  void SetTunnelStateHandler(TunnelStateHandler h) { onTunnel_ = std::move(h); }
  // Live stats push (connection/throughput/provide). Fired on SDK listener
  // callbacks; the UI marshals to its thread and applies visibility gating.
  void SetStatsHandler(StatsHandler h) { onStats_ = std::move(h); }
  LiveStats CurrentStats();  // snapshot on demand (e.g. resync when window shows)

  // Accessors for the UI/view models to drive the SDK directly.
  bool apiReady() { return api_.has_value(); }
  urnet::Api& api() { return *api_; }
  bool hasDevice() { return device_.has_value(); }
  urnet::DeviceRemote& device() { return *device_; }
  // Account page opens billing/upgrade in the browser at this host.
  std::string linkHostName() const { return "ur.io"; }

 private:
  urnet::NetworkSpace BuildNetworkSpace();
  // After obtaining a network JWT, register this device and store the client JWT.
  void RegisterNetworkClient(const std::string& byJwt, std::function<void(AuthResult)> done);
  void SetupWalletCallbacks();
  void AuthLoginWithWallet(const std::string& address, const std::string& signatureB64,
                           const std::string& message);
  // Bring up the tunnel (service) and the controlling DeviceRemote.
  bool BootstrapSession();
  void SetAuthState(AuthState s, const std::string& error = {});
  void SubscribeStats();          // subscribe live-stats listeners (in BootstrapSession)
  LiveStats ReadStats();          // read the current snapshot from the SDK getters
  void PublishStats();            // ReadStats() -> onStats_
  static std::string RandomLoopbackHostPort();
  std::string DeviceSpec();
  std::string DeviceDescription();

  std::mutex mutex_;
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::Api> api_;
  std::optional<urnet::AsyncLocalState> asyncLocalState_;
  std::optional<urnet::LocalState> localState_;
  std::optional<urnet::DeviceRemote> device_;
  std::optional<urnet::ConnectViewController> connectVc_;
  std::optional<urnet::ContractViewController> contractVc_;  // live throughput feed
  std::vector<urnet::Sub> subs_;

  ServiceClient service_;
  std::vector<std::string> excludedApps_;
  std::string appVersion_ = "0.0.1";

  WalletConnect wallet_;
  std::function<void(AuthResult)> walletAuthDone_;
  std::string walletAddress_;

  AuthStateHandler onAuth_;
  TunnelStateHandler onTunnel_;
  StatsHandler onStats_;
  AuthState authState_ = AuthState::LoggedOut;
};

}  // namespace urnw
