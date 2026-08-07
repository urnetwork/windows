// SdkHost is the app's DeviceManager equivalent: it owns the NetworkSpace, Api,
// LocalState, and the DeviceRemote, and coordinates the service to bring up the
// tunnel. Auth results and tunnel/connection state are surfaced to the UI via
// handlers (invoked on background threads; the UI marshals to its thread).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
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
  // A wallet signed in but isn't linked to a network yet: the signed wallet
  // auth is retained (see CreateNetwork), and the UI routes to the
  // create-network step (name + terms, no password).
  bool wallet_needs_network = false;
};

// Outcome of the authLogin account discovery (macOS LoginInitialViewModel
// routing): an existing password account goes to the password step, an unknown
// user auth goes to sign-up, an unverified account goes to the verify step.
enum class LoginRoute {
  Login,          // the discovery itself yielded a session (jwt)
  Password,       // existing account: prompt for the password
  Create,         // no account: create a network
  Verify,         // account exists but is unverified: enter the code
  IncorrectAuth,  // the user auth belongs to another sign-in method
  Error,
};

struct LoginRouting {
  LoginRoute route = LoginRoute::Error;
  std::string userAuth;     // echoed user auth (password / create / verify)
  std::string authAllowed;  // comma-joined methods (IncorrectAuth)
  std::string error;
};

// Everything the create-network form collects (macOS CreateNetworkView).
struct CreateNetworkParams {
  std::string userAuth;      // empty in wallet mode
  std::string password;      // empty in wallet mode
  std::string networkName;
  bool terms = false;
  std::string referralCode;  // optional bonus referral code
  // create with the retained wallet auth from the wallet sign-in instead of
  // user_auth + password
  bool useWalletAuth = false;
};

// Snapshot of live connection / throughput / provide stats. Pushed to the UI on
// SDK listener callbacks (macOS parity: listener-push, not polling).
struct LiveStats {
  // getConnectionStatus() (CONNECTED/CONNECTING/DESTINATION_SET/DISCONNECTED)
  // -- EXCEPT in an rpc-only session, where it is forced to the deliberately
  // unrecognised "RPC_ONLY" so the connect page renders as disconnected. There
  // is no tunnel in that mode and nothing may claim otherwise. See the clamp at
  // the end of SdkHost::ReadStats.
  std::string connectionStatus;
  bool connected = false;         // forced false in an rpc-only session
  int64_t providerCount = 0;      // grid window current size (providers in window)
  int64_t downBitsPerSecond = 0;  // remote (tunneled) ingress bit rate
  int64_t upBitsPerSecond = 0;    // remote (tunneled) egress bit rate
  // This snapshot came from an rpc-only session: no tunnel exists, nothing is
  // carried, and the four fields above have been clamped to say so.
  bool rpcOnly = false;
  // What the SDK actually reported before the clamp. For the developer surface
  // (P2), which is the one place that should see through it. Empty/false unless
  // rpcOnly.
  std::string rawConnectionStatus;
  bool rawConnected = false;
  bool insufficientBalance = false;
  bool provideEnabled = false;
  bool providePaused = false;
  int64_t provideClients = 0;     // connected peers while providing
  // the LIVE effective provide mode (protocol values: 0 none, 1 network,
  // 2 friends-and-family, 3 public — a bit set, compare per-case)
  int64_t provideMode = 0;
  // the provider holds a Network-mode provide key: with provideEnabled this
  // means the device is discoverable/connectable as a same-network peer
  bool provideHasNetworkKey = false;
  std::string locationName;       // selected connect location (empty = best available)
  std::string countryCode;        // selected location country code (dns recommendations)
  std::string countryName;
};

// ---- connect drawer snapshots (macOS ConnectStatsSections parity) ----------

// Connection mode segmented control state (device performance profile).
enum class ConnectionMode { Auto, Web, Streaming };

struct PerformanceSettings {
  ConnectionMode mode = ConnectionMode::Auto;
  bool fixedIp = false;       // window size pinned to [1,1]
  bool allowDirect = false;   // inverse of the "Strong Anonymization" toggle
  bool postQuantum = false;   // "Post Quantum Encryption" toggle (not inverted)
};

// One contract, un-aggregated: its own used/total byte counts and bit rate.
// Contracts are never paired -- a peer's send and receive contracts are
// fundamentally many-to-many, so each is presented on its own (SDK
// ContractEntry parity).
struct ContractEntry {
  std::string contractId;  // stable identity a circle keeps for its whole life
  int64_t usedByteCount = 0;
  int64_t totalByteCount = 0;
  int64_t bitRate = 0;
  // a stream contract (its transfer path carries a stream id): the circle is
  // drawn with a second concentric outer ring so streams read as distinct from
  // direct contracts (SDK ContractEntry.HasStream)
  bool hasStream = false;
};

inline bool operator==(const ContractEntry& a, const ContractEntry& b) {
  return a.contractId == b.contractId && a.usedByteCount == b.usedByteCount &&
         a.totalByteCount == b.totalByteCount && a.bitRate == b.bitRate &&
         a.hasStream == b.hasStream;
}
inline bool operator!=(const ContractEntry& a, const ContractEntry& b) { return !(a == b); }

// One peer client's open contracts, as two independent stacks (newest first):
// contracts sending to the peer and contracts receiving from it. The per-peer
// grouping, ordering, activity signal, and closing lifecycle all live in the
// SDK ContractDetailsViewController, shared by every platform (macOS
// ContractDetailsStore parity); the sheet just renders these rows.
struct ContractPeerRow {
  std::string clientId;
  std::vector<ContractEntry> send;     // newest first
  std::vector<ContractEntry> receive;  // newest first
  // cumulative bytes moved to / from this peer in the current run (accumulated
  // across the peer's contracts, reset when it goes idle), for the direction headers
  int64_t sendByteCount = 0;
  int64_t receiveByteCount = 0;
  // unix-millis of this peer's last byte movement (any contract with a positive
  // bit rate), or 0 if it has not moved bytes since appearing. The list floats
  // rows with recent activity above idle ones; freshness is judged against the
  // device clock (the view controller runs in-app, same wall clock as the view).
  int64_t lastActivityMillis = 0;
  // the peer's last contract closed and the row is being ejected: the view
  // controller keeps it briefly (empty stacks) so the circles slide off, then
  // removes it
  bool closing = false;
};

inline bool operator==(const ContractPeerRow& a, const ContractPeerRow& b) {
  return a.clientId == b.clientId && a.send == b.send && a.receive == b.receive &&
         a.sendByteCount == b.sendByteCount && a.receiveByteCount == b.receiveByteCount &&
         a.lastActivityMillis == b.lastActivityMillis && a.closing == b.closing;
}
inline bool operator!=(const ContractPeerRow& a, const ContractPeerRow& b) {
  return !(a == b);
}

// A recent routing decision (block action), flattened for the UI. Newest first.
struct BlockActionItem {
  std::string id;
  int64_t timeMillis = 0;
  std::vector<std::string> hosts;
  std::vector<std::string> ips;
  // the exact hosts/ips that matched an override (disjoint from hosts/ips), shown
  // as green chips at the front of the row (iOS BlockActionItem.matchedHosts/Ips)
  std::vector<std::string> matchedHosts;
  std::vector<std::string> matchedIps;
  bool block = false;
  bool local = false;
  std::string overrideId;  // deciding override id ("" when none)
  bool hasBlockOverride = false;
  bool hasRouteOverride = false;
  int64_t packetCount = 0;
  int64_t byteCount = 0;
};

inline bool operator==(const BlockActionItem& a, const BlockActionItem& b) {
  return a.id == b.id && a.timeMillis == b.timeMillis && a.hosts == b.hosts &&
         a.ips == b.ips && a.matchedHosts == b.matchedHosts &&
         a.matchedIps == b.matchedIps && a.block == b.block && a.local == b.local &&
         a.overrideId == b.overrideId && a.hasBlockOverride == b.hasBlockOverride &&
         a.hasRouteOverride == b.hasRouteOverride && a.packetCount == b.packetCount &&
         a.byteCount == b.byteCount;
}
inline bool operator!=(const BlockActionItem& a, const BlockActionItem& b) {
  return !(a == b);
}

// A block action override ("split rule"): forces the host cluster local.
struct SplitRule {
  std::string overrideId;
  std::vector<std::string> hosts;
  bool routeLocal = false;
};

inline bool operator==(const SplitRule& a, const SplitRule& b) {
  return a.overrideId == b.overrideId && a.hosts == b.hosts && a.routeLocal == b.routeLocal;
}
inline bool operator!=(const SplitRule& a, const SplitRule& b) { return !(a == b); }

// A per-app split rule (Android parity): a BlockActionOverride keyed by the app's
// exe IMAGE PATH. includeInTunnel=true routes the app THROUGH the tunnel
// (RouteOverride.Local=false); false makes it BYPASS the tunnel (Local=true).
struct AppRule {
  std::string imagePath;
  bool includeInTunnel = true;
};
inline bool operator==(const AppRule& a, const AppRule& b) {
  return a.imagePath == b.imagePath && a.includeInTunnel == b.includeInTunnel;
}
inline bool operator!=(const AppRule& a, const AppRule& b) { return !(a == b); }

class SdkHost {
 public:
  using AuthStateHandler = std::function<void(AuthState, const std::string& error)>;
  // Fired when the sdk finds the stored auth is no longer valid on the server
  // (e.g. the client was removed): the sdk has already cleared its local auth
  // state. Runs on an sdk callback thread and must only marshal -- the ui
  // marshals onto its thread and calls Logout().
  using AuthInvalidHandler = std::function<void()>;
  using JwtRefreshedHandler = std::function<void()>;
  using TunnelStateHandler = std::function<void(const proto::TunnelStatus&)>;
  using StatsHandler = std::function<void(const LiveStats&)>;
  // Connect drawer feeds (invoked on SDK callback threads; payloads by value so
  // the UI can marshal them onto its thread).
  using ThroughputHandler =
      std::function<void(std::vector<urnet::ThroughputPoint>, int64_t windowSeconds)>;
  using ContractRowsHandler = std::function<void(std::vector<ContractPeerRow>)>;
  using BlockActionsHandler = std::function<void(std::vector<BlockActionItem>)>;
  using BlockStatsHandler = std::function<void(int64_t allowed, int64_t blocked)>;
  using SplitRulesHandler = std::function<void(std::vector<SplitRule>)>;
  using DnsSettingsHandler = std::function<void(std::optional<urnet::DnsResolverSettings>)>;
  using BlockerEnabledHandler = std::function<void(bool)>;
  // Location/provider chooser feeds (invoked on SDK callback threads; payloads
  // by value so the UI can marshal them onto its thread).
  using LocationsHandler =
      std::function<void(std::optional<urnet::FilteredLocations>, std::string state)>;
  using PeersHandler = std::function<void(std::optional<urnet::NetworkPeerList>)>;
  using RemoteChangedHandler = std::function<void(bool remoteConnected)>;

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

  // Guest mode (macOS GuestModeSheet / linux LoginAsGuest parity): one tap
  // creates a throwaway network — networkCreate{guest_mode, terms}, no user
  // auth — then registers this device like any other sign-in. Upgradeable to a
  // full account later (UpgradeGuest).
  void LoginAsGuest(std::function<void(AuthResult)> done);

  // Account discovery: authLogin{user_auth} routes an email/phone to the
  // password, create or verify step (macOS LoginInitialViewModel parity).
  void StartLogin(const std::string& userAuth, std::function<void(LoginRouting)> done);

  // Sign-up: networkCreate with user_auth + password, or with the retained
  // wallet auth (params.useWalletAuth). verification_required in the result
  // routes the UI to the verify step; a jwt registers this device.
  void CreateNetwork(const CreateNetworkParams& params, std::function<void(AuthResult)> done);

  // Guest -> full account (Api::upgradeGuest; linux SdkHost parity). The
  // upgraded network keeps its id but issues a new jwt, so on success
  // RegisterNetworkClient tears the live guest session down and re-registers
  // this device under the new auth. verification_required routes the UI to the
  // verify step (authVerify then lands the new jwt the same way).
  void UpgradeGuest(const std::string& networkName, const std::string& userAuth,
                    const std::string& password, std::function<void(AuthResult)> done);

  // Verify-code entry + resend (authVerify / authVerifySend). A successful
  // verify yields the network jwt and registers this device.
  void VerifyCode(const std::string& userAuth, const std::string& code,
                  std::function<void(AuthResult)> done);
  void ResendVerifyCode(const std::string& userAuth, std::function<void(bool ok)> done);

  // Password reset: emails a reset link to the user auth.
  void SendPasswordResetLink(const std::string& userAuth, std::function<void(bool ok)> done);

  // Debounced-by-the-caller network name availability check, through the SDK's
  // NetworkNameValidationViewController. done(ok, available): ok=false means
  // the check itself failed.
  void CheckNetworkName(const std::string& networkName,
                        std::function<void(bool ok, bool available)> done);

  // A wallet signed in without a linked network and its signed auth is waiting
  // for CreateNetwork{useWalletAuth}.
  bool HasPendingWalletAuth();

  // The claims baked into the stored network jwt (Pro, GuestMode, network
  // name) — readable offline. Empty when logged out.
  std::optional<urnet::ByJwt> ParsedJwt();
  // Refresh the device token when the server's Pro state and the jwt disagree
  // (macOS device.refreshToken(0)).
  void RefreshJwt();

  // Sign in with a Solana wallet (Phantom/Solflare) via the ur.io/wallet-connect
  // browser bridge: connect -> sign a challenge -> authLogin{wallet_auth}. The
  // urnetwork:// callback must be routed back in via HandleDeepLink.
  void SignInWithSolana(WalletConnect::Provider provider, std::function<void(AuthResult)> done);

  // Sign in with a Bittensor wallet through the same bridge. One step (no
  // connect handshake): the bridge signs the challenge with an injected
  // substrate wallet and returns the ss58 address + sr25519 signature, which go
  // to authLogin{wallet_auth{blockchain=TAO}}.
  void SignInWithBittensor(std::function<void(AuthResult)> done);

  // Route a urnetwork:// deep link (wallet callback; later OAuth) into the host.
  // Called from the app's protocol-activation handler.
  void HandleDeepLink(const std::string& url);

  void Logout();

  // Connect flow via the ConnectViewController.
  void ConnectBestAvailable();
  void Connect(const std::string& connectLocationJson);
  void Disconnect();

  // ---- location/provider chooser -------------------------------------------
  // LocationsViewController buckets provider locations into sections and owns
  // the search; PeerViewController surfaces the connected, provide-enabled
  // network peers pinned atop the chooser. Both are opened lazily on the first
  // chooser open (EnsureLocations) and torn down with the session; reads are
  // graceful (empty) before the view controllers exist.
  void EnsureLocations();
  void SetLocationFilter(const std::string& query);
  std::optional<urnet::FilteredLocations> CurrentFilteredLocations();
  std::string CurrentFilteredLocationState();
  std::optional<urnet::NetworkPeerList> ConnectedProvidePeers();
  // count of ALL connected peers (online, provide or not)
  int64_t ConnectedPeerCount();
  // Whether the service rpc is attached. The peers state lives in the
  // service's device: while this is false the peer count is unavailable
  // (not zero) and the peers status line shows a disabled-discovery state.
  bool RemoteConnected();
  // The selected connect location (the chooser's selection check + the drawer's
  // selected-peer name resolution).
  std::optional<urnet::ConnectLocation> SelectedLocation();
  // Connect to a chosen provider location as-is: the chooser holds the typed
  // ConnectLocation (an SDK one, or one it built from a peer), so skip the json
  // round-trip that Connect(const std::string&) does.
  void Connect(const urnet::ConnectLocation& location);
  // Own presentation-only view controllers only while the WinUI window is
  // visible. The DeviceRemote and service tunnel remain alive in the tray.
  void SetPresentationActive(bool active);

  void SetAuthStateHandler(AuthStateHandler h) { onAuth_ = std::move(h); }
  void SetAuthInvalidHandler(AuthInvalidHandler h) { onAuthInvalid_ = std::move(h); }
  void SetJwtRefreshedHandler(JwtRefreshedHandler h) { onJwtRefreshed_ = std::move(h); }
  void SetTunnelStateHandler(TunnelStateHandler h) { onTunnel_ = std::move(h); }
  // Live stats push (connection/throughput/provide). Fired on SDK listener
  // callbacks; the UI marshals to its thread and applies visibility gating.
  void SetStatsHandler(StatsHandler h) { onStats_ = std::move(h); }
  LiveStats CurrentStats();  // snapshot on demand (e.g. resync when window shows)

  // ---- connect drawer (stats cards + sheets) -------------------------------
  // Push handlers, set once by the window; fired on SDK callback threads.
  void SetThroughputHandler(ThroughputHandler h) { onThroughput_ = std::move(h); }
  void SetContractRowsHandler(ContractRowsHandler h) { onContractRows_ = std::move(h); }
  void SetBlockActionsHandler(BlockActionsHandler h) { onBlockActions_ = std::move(h); }
  void SetBlockStatsHandler(BlockStatsHandler h) { onBlockStats_ = std::move(h); }
  void SetSplitRulesHandler(SplitRulesHandler h) { onSplitRules_ = std::move(h); }
  void SetDnsSettingsHandler(DnsSettingsHandler h) { onDnsSettings_ = std::move(h); }
  void SetBlockerEnabledHandler(BlockerEnabledHandler h) { onBlockerEnabled_ = std::move(h); }
  void SetLocationsHandler(LocationsHandler h) { onLocations_ = std::move(h); }
  void SetPeersHandler(PeersHandler h) { onPeers_ = std::move(h); }
  void SetRemoteChangedHandler(RemoteChangedHandler h) { onRemoteChanged_ = std::move(h); }

  // Snapshots on demand (seed / resync when the window shows).
  std::vector<urnet::ThroughputPoint> CurrentThroughputPoints(int64_t& windowSeconds);
  std::vector<ContractPeerRow> CurrentContractRows();
  // Contract-details sheet surface into the single-feed view controller, which
  // owns the ordering, the scrolled-away freeze, and the pending count. The
  // sheet reports its scroll position and reads the "N new" count; the ordered
  // rows arrive via SetContractRowsHandler / CurrentContractRows.
  void SetContractsAtTop(bool atTop);
  int64_t ContractsPendingCount();
  std::vector<BlockActionItem> CurrentBlockActions();
  void CurrentBlockCounts(int64_t& allowed, int64_t& blocked);
  std::vector<SplitRule> CurrentSplitRules();
  std::optional<urnet::DnsResolverSettings> CurrentDnsSettings();
  bool CurrentBlockerEnabled();
  PerformanceSettings CurrentPerformanceSettings();

  // Drawer mutations (called from the UI thread).
  // Connection mode / fixed ip / strong anonymization / post quantum -> device
  // performance profile. Always writes a profile — Auto carries window_type
  // "auto" with no window size — so the orthogonal settings (allowDirect,
  // postQuantum) persist and apply in every mode (macOS DeviceManager
  // createPerformanceProfile parity). Persisted in the app LocalState.
  void SetPerformanceSettings(const PerformanceSettings& settings);
  // Ad/tracker blocker: the device applies and persists it; the app stores nothing.
  void SetBlockerEnabled(bool on);
  // Kill switch (settings). The SDK stores the INVERSE: routeLocal=true means
  // "when the tunnel is down, let traffic route out the local interface", so the
  // kill switch is on exactly when routeLocal is off. Kept as the inversion here
  // rather than in the view so no screen has to remember which way it runs.
  //
  // Readable and writable with no tunnel: the preference lives in the app
  // LocalState and DeviceLocal restores it at construction, so a signed-in user
  // with no service session still sees and sets the real value. Without the
  // LocalState leg this control would be permanently dead outside a live tunnel.
  bool CurrentKillSwitch();
  void SetKillSwitch(bool on);
  // Provide/earn control mode: "never"|"always"|"network"|"auto"|"manual".
  // "network" is the private provider: the provider is always on, but provides
  // ONLY to same-network peers — never publicly. Persisted in LocalState like
  // macOS (DeviceLocal does not persist the control mode itself).
  std::string CurrentProvideControlMode();
  void SetProvideControlMode(const std::string& mode);
  void ApplyDnsSettings(const urnet::DnsResolverSettings& settings);
  void CreateSplitRule(const std::vector<std::string>& hosts);
  void UpdateSplitRule(const std::string& overrideId, const std::vector<std::string>& hosts);
  void RemoveSplitRule(const std::string& overrideId);

  // Per-app split tunneling (Android parity). A rule is a BlockActionOverride keyed
  // by the app's exe image path; the SDK persists it and the change listener re-
  // drives the driver from getLocalOverrideAppIds. includeInTunnel=true routes the
  // app through the tunnel; false bypasses it. SetAppRule on an app that already
  // has a rule updates it; RemoveAppRule clears it (back to the default = tunneled).
  void SetAppRule(const std::string& imagePath, bool includeInTunnel);
  void RemoveAppRule(const std::string& imagePath);
  std::vector<AppRule> CurrentAppRules();

  // Accessors for the UI/view models to drive the SDK directly.
  bool apiReady() { return api_.has_value(); }
  urnet::Api& api() { return *api_; }
  bool hasDevice() { return device_.has_value(); }
  urnet::DeviceRemote& device() { return *device_; }
  // Account page opens billing/upgrade in the browser at this host.
  std::string linkHostName() const { return "ur.io"; }
  // The version this app reports to the SDK/server. Settings shows it because
  // urnet::version() is empty in this SDK build, so it is the only build
  // identifier the version row can actually display.
  std::string appVersion() const { return appVersion_; }

  // ---- start mode ----------------------------------------------------------
  // Which kind of service session this app asks for, and which kind it got.
  //
  // RpcOnly is the development mode (spec P1): the service brings up the
  // DeviceLocal and the mTLS rpc listener and STOPS before it would touch the
  // machine's routes or DNS, so device() below is live and every Class-B
  // surface — developer/reliability screen, connect controls, locations,
  // provide, DNS, split rules — can be driven with no tunnel and no elevation.
  // Nothing is connected in this mode and the UI must not say it is:
  // sessionMode() == RpcOnly means TunnelState::Up is never reported, so the
  // existing `state == Up` tests already read it as "not connected".
  //
  // Requested by the URNETWORK_RPC_ONLY environment variable, read once at
  // Initialize(). The value is an explicit allow-list: "1"/"true"/"yes"/"on"
  // (case-insensitive, surrounding whitespace ignored) turn it ON; empty,
  // "0"/"false"/"no"/"off" turn it off; ANYTHING ELSE is off and logs a
  // warning. Do not assume a value like "enable" works — it does not.
  //
  // Setting it is NOT required to get an rpc-only session. A service started
  // with `urnetworkd console --rpc-only` serves every request as rpc-only, and
  // the app ADOPTS that (see BootstrapSession) and raises a persistent notice.
  // The env var is for asking explicitly when the service is unclamped.
  proto::StartMode requestedStartMode() const { return requestedMode_; }
  // The mode the live session actually runs in, as reported by the service.
  // It can differ from the requested one: a service clamped with `urnetworkd
  // console --rpc-only` serves rpc-only whatever was asked, and the app adopts
  // it. (Reattaching cannot cause a difference — reattach requires an exact
  // mode match.) Defaults to RpcOnly, the mode that claims less, so a read with
  // no session in force can never render as connected.
  proto::StartMode sessionMode() const { return sessionMode_.load(); }

  // ---- persistent session notice -------------------------------------------
  // The standing reason this app is NOT carrying traffic. A property of the
  // current state rather than an event: whatever renders it keeps it visible
  // until it is replaced, and must NOT offer a dismiss control. `active ==
  // false` means there is nothing to show, and is the normal case.
  //
  // THREADING: invoked on the bootstrap thread, and MAY be called while
  // SdkHost's internal lock is held. Marshal to the UI thread and return
  // immediately — a handler that calls back into SdkHost synchronously
  // deadlocks. Every other handler on this class already does that through
  // AppController::OnUi / DispatcherQueue; do the same.
  struct ModeNotice {
    enum class Kind {
      // A live session that deliberately carries no traffic (rpc-only).
      RpcOnly,
      // No session at all: the service is unreachable, too old, or refused.
      // The user is still SIGNED IN — this is not an authentication failure
      // and must not route anyone to the sign-in screen.
      SessionFailed,
    };
    bool active = false;
    Kind kind = Kind::RpcOnly;
    // RpcOnly only: the app asked for a real tunnel and the service refused to
    // build one (it is clamped). Worth saying separately — the user did not
    // choose this.
    bool requestedTunnel = false;
    // A complete sentence, already self-describing. Render it as-is; do NOT
    // add a "Developer mode" title on top, or the words appear twice.
    std::string message;
  };
  using ModeNoticeHandler = std::function<void(const ModeNotice&)>;
  void SetModeNoticeHandler(ModeNoticeHandler h) { onModeNotice_ = std::move(h); }
  // Re-push the current notice. Safe at any time, including from an ordinary
  // logged-out launch: with no session it publishes an INACTIVE notice. (It
  // used to derive purely from sessionMode_, whose default is RpcOnly, so
  // calling it without a session fabricated a claim that the service was
  // running with --rpc-only.)
  //
  // Takes the lock: this is a public entry point, it reads `device_`, and a
  // session teardown can be running concurrently on the bootstrap thread —
  // observed in testing, where a logout destroyed the session while a refresh
  // was in flight. The handler is therefore invoked with the lock held on this
  // path as well as from bootstrap; see the threading note above.
  void RefreshModeNotice() {
    std::scoped_lock lock(mutex_);
    PublishModeNotice();
  }

 private:
  urnet::NetworkSpace BuildNetworkSpace();
  // After obtaining a network JWT, register this device and store the client JWT.
  // A live session (guest upgrade) is torn down first: the new jwt invalidates
  // the running device, and BootstrapSession rebuilds under the new auth.
  void RegisterNetworkClient(const std::string& byJwt, std::function<void(AuthResult)> done);
  // Tear down the live session — listeners, view controllers, drawer caches,
  // the device, the service tunnel, and the saved RPC session — without
  // touching the stored auth or the service-persisted device identity (the
  // key material is device-scoped and survives re-registration). Logout
  // clears the auth and severs the identity on top of this;
  // RegisterNetworkClient replaces the auth. Caller holds mutex_.
  void TeardownSessionLocked();
  void SetupWalletCallbacks();
  // The wallet signed the challenge: authLogin{wallet_auth}. `signature` is what
  // the chain's verifier expects (base64 for SOL, hex for TAO).
  void AuthLoginWithWallet(const std::string& address, const std::string& signature,
                           const std::string& message, WalletConnect::Provider provider);
  // Bring up the tunnel (service) and the controlling DeviceRemote.
  bool BootstrapSession();
  void SetAuthState(AuthState s, const std::string& error = {});
  void SubscribeStats();          // caller holds mutex_; opens presentation controllers
  LiveStats ReadStats();          // read the current snapshot from the SDK getters
  void PublishStats();            // ReadStats() -> onStats_
  // Drawer feeds: subscribe listeners (in BootstrapSession) and publish
  // snapshots, only on change (block actions storm per routing decision).
  void SubscribeDrawer();
  void ClosePresentationLocked();
  void PublishThroughput();
  void PublishContractRows();
  void PublishBlockActions();
  void PublishBlockStats();
  void PublishSplitRules();
  // Read getLocalOverrideAppIds(), compute {paths, allowlist} (Android inversion:
  // any include-in-tunnel app => allowlist with the tunnel set, else denylist with
  // the bypass set), and push to the service -> driver. Called from the override
  // change listener and the initial drawer snapshot.
  void PushLocalOverrideAppsToDriver();
  void ClearDrawer();             // logout: reset caches and push empty snapshots
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
  // whether a network-visible provide key exists, pushed by
  // addProvideSecretKeysListener (DeviceRemote has no secret-keys getter --
  // the controller subscribes and caches the derived bit)
  std::atomic<bool> provideHasNetworkKey_{false};
  std::optional<urnet::ConnectViewController> connectVc_;
  std::optional<urnet::ContractViewController> contractVc_;  // live throughput feed
  // per-peer contract rows: this single-feed VC (the client feed) owns the
  // egress+ingress coalescing, renewal atomicity, per-peer aggregation, the
  // closing/eject lifecycle, the at-top activity sort, the scrolled-away freeze,
  // and the pending count -- getContractRows() returns the FINAL ordered rows
  std::optional<urnet::ContractDetailsViewController> contractDetailsVc_;
  std::optional<urnet::BlockActionViewController> blockVc_;  // block actions + stats
  std::optional<urnet::LocationsViewController> locationsVc_;  // provider chooser feed
  std::optional<urnet::PeerViewController> peerVc_;  // connected provide-enabled peers
  // network-name availability at sign-up; api-scoped, so it survives logout
  std::optional<urnet::NetworkNameValidationViewController> networkNameVc_;
  std::vector<urnet::Sub> subs_;
  std::vector<urnet::Sub> presentationSubs_;
  bool presentationActive_ = false;

  // Drawer caches (change detection + on-demand snapshots), guarded by
  // drawerMutex_ because the SDK listeners fire on their own threads.
  std::mutex drawerMutex_;
  std::vector<urnet::ThroughputPoint> lastThroughputPoints_;
  int64_t throughputWindowSeconds_ = 60;
  std::vector<ContractPeerRow> lastContractRows_;
  std::vector<BlockActionItem> lastBlockActions_;
  int64_t lastAllowedCount_ = 0;
  int64_t lastBlockedCount_ = 0;
  std::vector<SplitRule> lastSplitRules_;

  // The session status to report for "we have a device, and it is/isn't on a
  // location" — derived from sessionMode_ so an rpc-only session can never
  // surface TunnelState::Up. Every place that used to hand-build such a status
  // goes through here.
  proto::TunnelStatus SessionStatus(bool haveLocation) const;
  // Build and push the persistent notice from the CURRENT session state.
  // Caller holds mutex_ (it reads device_).
  void PublishModeNotice();
  // Push a "there is no session, and here is why" notice. The user stays
  // signed in; see ModeNotice::Kind::SessionFailed.
  void PublishSessionFailure(const std::string& why);

  ServiceClient service_;
  // Set once in Initialize() from URNETWORK_RPC_ONLY; never changes after.
  proto::StartMode requestedMode_ = proto::StartMode::Tunnel;
  // Set from the service's reply/hello whenever a session is established, and
  // reset here when one is torn down. Defaults to RpcOnly — the mode that
  // CLAIMS LESS — matching the policy TunnelStatus::from_json states for an
  // unreadable mode. With no session there is certainly no tunnel, so a stray
  // read before or after one must not be able to render "connected".
  std::atomic<proto::StartMode> sessionMode_{proto::StartMode::RpcOnly};
  // Why the last BootstrapSession() returned false, in words a user can act on.
  // Set on every failure path and read by both callers; guarded by mutex_, which
  // BootstrapSession's callers already hold.
  std::string bootstrapError_;
  std::string appVersion_ = "0.0.1";

  WalletConnect wallet_;
  std::function<void(AuthResult)> walletAuthDone_;
  // the signed wallet auth of a wallet that has no network yet, held for the
  // create-network step (cleared on success, logout, or a new wallet sign-in)
  std::optional<urnet::WalletAuthArgs> pendingWalletAuth_;

  AuthStateHandler onAuth_;
  AuthInvalidHandler onAuthInvalid_;
  JwtRefreshedHandler onJwtRefreshed_;
  TunnelStateHandler onTunnel_;
  StatsHandler onStats_;
  ThroughputHandler onThroughput_;
  ContractRowsHandler onContractRows_;
  BlockActionsHandler onBlockActions_;
  BlockStatsHandler onBlockStats_;
  SplitRulesHandler onSplitRules_;
  ModeNoticeHandler onModeNotice_;
  DnsSettingsHandler onDnsSettings_;
  BlockerEnabledHandler onBlockerEnabled_;
  LocationsHandler onLocations_;
  PeersHandler onPeers_;
  RemoteChangedHandler onRemoteChanged_;
  AuthState authState_ = AuthState::LoggedOut;
};

}  // namespace urnw
