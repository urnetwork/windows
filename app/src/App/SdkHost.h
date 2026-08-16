// SdkHost is the app's DeviceManager equivalent: it owns the NetworkSpace, Api,
// LocalState, and the DeviceRemote, and coordinates the service to bring up the
// tunnel. Auth results and tunnel/connection state are surfaced to the UI via
// handlers (invoked on background threads; the UI marshals to its thread).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ConnectAction.h"
#include "ConnectionHealth.h"
#include "GoogleSignIn.h"
#include "PostQuantumIdentity.h"
#include "ProviderLocations.h"
#include "Sdk.h"
#include "ServiceClient.h"
#include "ServiceRecoveryPolicy.h"
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
  // The same situation for an SSO identity (Google): the id token is retained
  // (see CreateNetwork{useAuthJwt}) and the UI routes to the same step. Kept
  // SEPARATE from wallet_needs_network rather than folded into it, because the
  // create step has to know which credential it is finishing — the two write
  // different fields of NetworkCreateArgs.
  bool auth_needs_network = false;
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
  // create with the retained Google id token from the SSO sign-in instead of
  // user_auth + password (SdkHost::HasPendingAuthJwt)
  bool useAuthJwt = false;
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

  // ---- provider grid (ConnectGrid) ----
  // The grid listener has been subscribed since the first build and
  // getProviderGridPointList() had NO consumer anywhere in the app: ReadStats
  // fetched the grid and kept only getWindowCurrentSize(). These three carry it
  // to the connect hero canvas, which is what finally reads it.
  //
  // An EMPTY list is a normal state, not a failure: no session, an rpc-only
  // session, or a connection that has not placed a provider yet all report one,
  // and the Go side marshals a nil slice as the four-byte document `null`, which
  // ReadSdkList turns into nullopt. The hero renders that as its bare lattice.
  std::vector<urnet::ProviderGridPoint> gridPoints;
  int64_t gridWidth = 0;   // grid columns; 0 until the grid has a point
  int64_t gridHeight = 0;  // grid rows

  // ---- aggregate connection health (#27) ----
  // THE state every user-facing surface renders — status line, strip, tray.
  // Derived in ReadStats by the one Tracker in SdkHost (ConnectionHealth.h has
  // the transition table and the reasons), so every snapshot carries a health
  // reading consistent with the other fields in the SAME snapshot. Defaults to
  // NoService, the state that claims least.
  urnw::health::State health = urnw::health::State::NoService;
  // Grid cells in the Added state — the app-side "proven" count.
  int64_t provenProviderCount = 0;
  // Non-zero while a degrade hold is pending: the steady-clock millis at which
  // the CLOCK (not a new SDK event) changes the answer. Whoever renders health
  // must ask for a fresh snapshot then (ConnectPage does, from its 1s tick,
  // via RepublishStats) — grid events stop arriving exactly when everything is
  // stuck, so waiting for one would hold "Connected" over a dead window.
  int64_t healthReevalAtMillis = 0;

  // ---- window honesty (connect-flow reliability, track 2) ----
  // The SDK's stall diagnosis for a still-forming window (WindowStatus.
  // StallReason over the device RPC): "evaluating" | "platform-unreachable" |
  // "providers-unresponsive" | "rate-limited" | "auth-failing". Empty when the
  // service predates the field or nothing is being attempted. ConnectPage
  // renders it as the reason line under the hero while yellow and in the
  // failure state.
  std::string windowStallReason;
  // WindowStatus.Failed: zero providers Added past both of the window's
  // outcome deadlines (45s to one automatic silent rebuild, 45s more to
  // this). Feeds health::Signals::windowFailed, which is what renders it.
  // Clamped false with everything else in rpc-only and service-down.
  bool windowFailed = false;
};

// ---- selection identity ----------------------------------------------------
// The tests for "is this location the selected one", against
// ConnectViewController.getSelectedLocation(): best-available when nothing is
// selected or the id flags it; a location/peer by comparing the
// connect_location_id parts. These lived in LocationSheets.cpp's anonymous
// namespace for the chooser sheet's and the Network pane's check glyphs; they
// moved here when SdkHost's row-click coalescer became a third consumer of the
// same question (ConnectFromRow's re-select no-op) — three copies of an
// identity predicate is how surfaces drift into disagreeing about what is
// selected.
bool SameId(std::optional<std::string> const& a, std::optional<std::string> const& b);
bool IsBestAvailableSelected(std::optional<urnet::ConnectLocation> const& selected);
bool IsLocationSelected(std::optional<urnet::ConnectLocation> const& selected,
                        urnet::ConnectLocation const& location);

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

// ---- reliability / developer surface ---------------------------------------
// One read of everything the developer screen shows, taken under a single lock
// so the four getters cannot disagree with each other about which session they
// came from (iOS ReliabilityStore.refresh parity: one hop, one publish).
struct ReliabilitySnapshot {
  // there is a live DeviceRemote at all
  bool haveDevice = false;
  // the service rpc is attached (device_->getRemoteConnected())
  bool remoteConnected = false;
  // NULLOPT MEANS "NOTHING IS IN FORCE", NOT "EVERYTHING IS OFF".
  //
  // getReliabilitySettings() returns null when the device has no multi client
  // to override — the reliability stack is running on its own defaults. A
  // zero-initialised ReliabilitySettings is a DIFFERENT thing: writing one back
  // installs an all-zero override that disables the whole stack, and the
  // sync re-apply latches it. This bug has shipped once already. Never
  // substitute a default-constructed struct for a nullopt read on the WRITE
  // path; the read path may substitute one for DISPLAY only.
  std::optional<urnet::ReliabilitySettings> settings;
  std::optional<urnet::ReliabilityMetrics> metrics;
  std::vector<urnet::Exit> exits;
  std::vector<urnet::DestinationExit> destinationExits;
  // D6: the probe suite. Running state and the last results ride the same
  // snapshot as everything else so the screen has ONE consistent read per poll
  // rather than a second, separately-timed one that can disagree with the exits
  // table about which session it describes.
  bool probeSuiteRunning = false;
  std::vector<urnet::ProbeResult> probeResults;
};

// The actions on the reliability bridge.
//
// D6 correction: an earlier version of this comment said none of these returns
// anything the C ABI preserves. That was wrong for two of them. migrateExit and
// probeAllExits are declared `int64_t` on DeviceRemote (urnetwork_sdk.hpp:10122,
// 10140) and their exports carry it, so "Migrated N flows" is available and is
// now reported. The other four really are void and "requested" remains the
// ceiling for them.
enum class ReliabilityAction {
  ResetMetrics,
  ResetSettings,
  ProbeAllExits,         // returns a count
  SimulateNetworkChange,
  Sync,
  MigrateExit,  // the only one that reads exitClientId; returns a count
};

// What an action actually did, as opposed to what was asked for.
//
// `issued` is the old bool: the call reached a live device and did not throw.
// `count` is only meaningful when `hasCount` is set, which happens for exactly
// the two actions whose SDK signature returns int64_t. A caller must not render
// a count for the void actions — a hardcoded 0 there reads as "migrated nothing"
// when the truth is "this action has nothing to report".
//
// `declined` is the NEGATIVE return, and it was found by RUNNING this, not by
// reading it: migrateExit against an exit that is not in the window returns -1,
// and the first version of this struct rendered that verbatim as "affected -1".
// A negative is a NOT-FOUND SENTINEL, not a flow count — those are different
// answers and only one of them is a number.
//
// Zero is NOT a sentinel. "Migrated 0 flows" is a real and useful result, so
// the test is `< 0` and must never be relaxed to `<= 0`.
struct ReliabilityActionResult {
  bool issued = false;
  bool hasCount = false;
  bool declined = false;
  int64_t count = 0;
};

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
  // The connected providers and where they are (the provider-locations sheet).
  using ProviderLocationsHandler = std::function<void(std::vector<ProviderLocationRow>)>;
  // The providers with a verified e2e session (the provider-locations badge).
  using ProviderIdentitiesHandler = std::function<void(std::vector<ProviderIdentityRow>)>;
  // The globe's selection changed. SIGNAL ONLY, like the locations feed: the
  // SDK fires it on the calling thread (a Set/Step call re-enters here), so the
  // handler must marshal onto the UI thread and re-read
  // SelectedProviderClientId() there rather than read it under our lock.
  using ProviderSelectionHandler = std::function<void()>;

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

  // ---- seedphrase ----------------------------------------------------------
  // A seedphrase is a CREDENTIAL with no recovery path. What is true of it in
  // this app, stated precisely — the previous wording here ("the only copy the
  // app ever holds is the one the display sheet is rendering") was NOT true,
  // and a false claim in a header is worse than no claim:
  //
  //   * Nothing logs one. Canary-tested across the app root: zero hits.
  //   * Nothing persists one. The only outbound copies are the SDK request
  //     body and, when the user asks for it, the clipboard — and that copy is
  //     excluded from Clipboard History and from the cloud clipboard
  //     (SeedphraseDisplaySheet::CopyToClipboard).
  //   * The in-memory copies, and what happens to each:
  //       - SeedphraseDisplaySheet::seedphrase_ — overwritten and cleared on
  //         confirm.
  //       - the by-value parameter of LoginPage::ShowSeedphraseSheet (a
  //         coroutine frame) and the InstantAccount captured by the create
  //         callback — overwritten and cleared once the sheet is done.
  //       - LoginPage's SeedphraseBox on the sign-in step — emptied on submit
  //         and on every reset of the flow. It is UIA-readable while it holds
  //         anything, which is why it is not left populated.
  //       - the SeedWords() vector and the 24 word-grid TextBlock texts.
  //         These are NOT zeroed and CANNOT be: winrt::hstring is immutable
  //         and refcounted, and XAML keeps its own copies inside the text
  //         layout. They die with the dialog's visual tree, whenever the
  //         allocator gets to them.
  //
  //   So: zeroed wherever zeroing is possible, and honest about where it is
  //   not. Do not restore the stronger claim.

  // Sign in with a 12- or 24-word seedphrase (macOS LoginSeedphraseView
  // parity): authLogin{seedphrase}. `seedphrase` is normalized here (lowercase,
  // trimmed, single-spaced) exactly as the other clients do, so a pasted phrase
  // with newlines or double spaces works.
  void LoginWithSeedphrase(const std::string& seedphrase,
                           std::function<void(AuthResult)> done);

  // Instant account (macOS CreateNetworkInstant parity): networkCreate{terms}
  // with NO user auth, password, auth jwt or wallet auth, which is what makes
  // the server mint a network secured only by a seedphrase and return it.
  //
  // Deliberately two steps. The seedphrase is shown exactly once, and the
  // device is not registered until the user confirms they saved it, so
  // dismissing the sheet cannot leave behind a signed-in account whose only
  // credential the user never read. Confirm or Discard must follow a successful
  // Create; the pending jwt is dropped either way.
  struct InstantAccount {
    bool ok = false;
    std::string error;
    // shown once by the display sheet; never logged, never persisted here
    std::string seedphrase;
  };
  void CreateInstantAccount(std::function<void(InstantAccount)> done);
  // Register this device under the instant network. Fails with a clear error if
  // no instant account is pending.
  void ConfirmInstantAccount(std::function<void(AuthResult)> done);
  // Drop the pending instant network jwt without registering (sheet dismissed).
  void DiscardInstantAccount();

  // ---- Google SSO ----------------------------------------------------------
  // Whether this build can offer "Sign in with Google" at all: the active
  // network space says the server supports it AND an OAuth client id was
  // compiled in (urnw::config::kGoogleOAuthClientId). With no client id there
  // is no flow to run, so the button is HIDDEN rather than shown-and-broken.
  bool SsoGoogleEnabled();
  // OAuth 2.0 authorization-code + PKCE through the SYSTEM BROWSER, with a
  // loopback redirect (RFC 8252). No native SSO SDK, no embedded webview. The
  // resulting Google id_token goes to authLogin{auth_jwt_type:"google"}; a
  // Google identity with no network yet routes to the create-network step the
  // same way a wallet does.
  void SignInWithGoogle(std::function<void(AuthResult)> done);
  // A Google identity authenticated but has no network: the id token is
  // retained for CreateNetwork (name + terms, no password), like a wallet.
  bool HasPendingAuthJwt();

  // ---- network server (iOS NetworkServerSheet parity) ----------------------
  // Which network API this client talks to. On a fork this is the difference
  // between the official ur.network and a self-hosted deployment.
  struct NetworkServer {
    std::string hostName;
    std::string apiUrl;      // live, derived or overridden
    std::string connectUrl;  // live platform (connect) url
    // the EXPLICIT overrides in force, or empty when the urls are derived
    std::string configuredApiUrl;
    std::string configuredConnectUrl;
    // What "the default network" means for THIS process: normally the
    // compiled-in ids::kNetworkSpaceHostName, but URNETWORK_NETWORK_HOST
    // when that is set. The sheet's "Use default network" hardcoded
    // "ur.network", so pressing it in a test-network session silently moved
    // the client to PRODUCTION — the one place a mistake is unrecoverable.
    std::string defaultHostName;
    bool managerAvailable = false;
  };
  NetworkServer CurrentNetworkServer();
  // Point the client at `hostName`, with optional explicit api/connect url
  // overrides (empty = derive from the host). Mirrors iOS
  // DeviceManager.applyNetworkSpace / android NetworkServerSelector.
  //
  // This changes which LocalState — and so which stored jwt — is in force, so
  // it tears the live session down and re-derives Api/LocalState. It is offered
  // from the SIGNED-OUT screen only, matching iOS. Returns false if the space
  // manager is unavailable or the update failed.
  bool ApplyNetworkServer(const std::string& hostName, const std::string& apiUrl,
                          const std::string& connectUrl);

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

  // Sign `message` with a Solana wallet through the same browser bridge and hand
  // the address and signature back WITHOUT authenticating. The Seeker multiplier
  // is the caller (android SettingsScreen.signAndVerifySeekerHolder): it verifies
  // the signed pair against the api rather than logging in with it.
  //
  // Only one wallet flow can be in flight, because the bridge exposes a single
  // pair of callbacks: starting a sign-in supersedes a pending signature request
  // and vice versa. `done` runs on whichever thread delivered the deep link, so
  // a UI caller marshals.
  void SignWithSolanaWallet(WalletConnect::Provider provider, const std::string& message,
                            std::function<void(bool ok, std::string address,
                                               std::string signature, std::string error)> done);

  // Route a urnetwork:// deep link (wallet callback; later OAuth) into the host.
  // Called from the app's protocol-activation handler.
  void HandleDeepLink(const std::string& url);

  void Logout();

  // ---- connect ------------------------------------------------------------
  //
  // CONNECT STARTS THE TUNNEL. It used to only ask the SDK to pick providers,
  // on the assumption that a session already existed — and the ONLY thing that
  // ever created one was BootstrapSession, called from the resume thread in
  // Initialize() and from a fresh sign-in. Every way of arriving signed-in
  // WITHOUT either of those (the service was down at launch, the app relaunched
  // into a network space with no stored jwt and the user re-picked their server,
  // the service was restarted under a running app) left the app with no
  // DeviceRemote and nothing to re-create one — so Connect was a permanent
  // no-op with no user-visible reason. Measured: a service started in tunnel
  // mode sat idle for 91 s and never received a start_tunnel.
  //
  // All three of these are now "connect to X, bringing a session up first if
  // there is not a live one". They are:
  //   * NON-BLOCKING. The intent is recorded and a worker does the work, so a
  //     press never waits on BootstrapSession (which holds mutex_ across
  //     several synchronous service rpcs, up to the 30 s pipe timeout).
  //   * IDEMPOTENT and RE-ENTRANT. At most one session worker runs at a time;
  //     a second press while one is in flight REPLACES the intent rather than
  //     starting a second bootstrap. Last press wins.
  //   * LOUD ON FAILURE. Every path that cannot produce a session publishes the
  //     reason on the notice channel (PublishSessionFailure) and pushes a stats
  //     snapshot, so the button does not sit on "Connecting" forever.
  void ConnectBestAvailable();
  void Connect(const std::string& connectLocationJson);
  void Disconnect();

  // TURN THE SERVICE'S TUNNEL OFF. Not the same thing as Disconnect(), and the
  // difference is the whole of the owner's "kill the app and my internet stays
  // blocked" report.
  //
  // Disconnect() asks the SDK's connect controller to stop connecting. It does
  // NOT touch the service: the capture routes stay installed and the WFP policy
  // stays in its Connected state, because the SERVICE owns the tunnel and only a
  // stop_tunnel takes it down. That ownership is correct and standard — it is
  // what keeps a tunnel alive across an app crash — but it means the app has to
  // offer a way to reach it that does not depend on the main window existing, or
  // a tunnel that stops working strands the user with no escape short of an
  // elevated `urnetworkd revert`.
  //
  // Synchronous and BLOCKING (one pipe rpc): the caller is a tray menu item, the
  // service's own Stop() is bounded at ~3 s by design, and an asynchronous
  // "turning it off, probably" is not what someone with no internet needs to be
  // told. Safe with no service connection — there is then nothing to stop.
  proto::TunnelStatus StopServiceTunnel();

  // ATTACH to a live service session if one is running, off the calling
  // thread, WITHOUT connecting to anything — and WITHOUT starting a session
  // that is not already there (D8, owner decision: the tunnel starts only on
  // an explicit Connect gesture; a reattach is not a start). `reason` names
  // the caller in the log. Same worker, same guarantees as the connect entry
  // points above.
  //
  // Called from: the resume path in Initialize(), a network-server change that
  // lands on a signed-in space, and the service-reconnect watchdog.
  void EnsureSession(const char* reason, bool automaticRecovery = false);

  // Whether a live service session exists, LOCK-FREE.
  //
  // Deliberately an atomic rather than a `device_.has_value()` under mutex_:
  // that lock is held by the session worker across a whole bootstrap, and this
  // is read from the UI thread inside a layout pass. (There used to be such a
  // locking variant, HasDeviceSession(); its one caller was the Network pane's
  // "no session, so no provider list" gate, and both went away when that gate
  // turned out to be wrong.) The strip is the caller — with no session at all it
  // used to
  // render "Session rpc-only" and "RPC none", because sessionMode_ defaults to
  // RpcOnly (the mode that claims less) and nothing distinguished "no session"
  // from "an rpc-only one". A status field must not name a state the app is not
  // in.
  bool HasSession() const { return hasSession_.load(std::memory_order_acquire); }

  // Whether the control channel to the service is up. Distinct from HasSession:
  // the service can be perfectly reachable with no session on it, and — the case
  // that matters — a status the app is still rendering can belong to a service
  // that has since gone away. The tray's recovery gates need to tell those
  // apart. PipeClient's flag is atomic; this takes no lock of ours.
  bool ServiceConnected() const { return service_.IsConnected(); }

  // ---- location/provider chooser -------------------------------------------
  // THE PROVIDER LIST IS ALWAYS AVAILABLE. It does not need a tunnel, a service
  // session, a DeviceRemote or elevation - GET /network/provider-locations is a
  // plain 200 with no authorization at all. Two sources feed the same pair of
  // handler slots, and exactly one of them writes at a time:
  //
  //   1. locationsVc_ (LocationsViewController, owned by the service's
  //      DeviceRemote). AUTHORITATIVE WHENEVER IT EXISTS. It pushes live
  //      updates, owns the server-side search, and is the only source that can
  //      see the network's own view of a signed-in device.
  //   2. api_ (the in-process Api built in Initialize, reachability class A -
  //      the same object that drives sign-in, alive from launch with or without
  //      a service). Used ONLY while there is no view controller.
  //
  // The single-writer rule is enforced by deviceFeedOpen_: the api path checks
  // it before every push and stays silent while the view controller is open, so
  // the two can never race into the same UI state. There is no reverse gate,
  // because there is nothing to gate - the view controller does not consult the
  // api cache.
  //
  // Both sources are presentation-scoped in the sense that they are (re)armed
  // from the same three places, and the view controller half is torn down WITH
  // THE PRESENTATION, not with the session; reads are graceful (empty) before
  // either source has answered.
  //
  // Idempotent, and cheap when it is a no-op. Call it from anywhere the
  // preconditions can newly become true - the failure it exists to prevent is a
  // torn-down feed that nothing puts back, and the snapshot getters CANNOT
  // recover from that on their own (both the SDK's initial load and the api
  // fetch are async; a read taken right after either starts is always empty).
  void EnsureLocations();
  // Set the provider-list search query. Proxies to the view controller when one
  // exists; otherwise runs the SAME two-endpoint dispatch the view controller
  // runs, against the in-process Api. Search therefore works identically with
  // and without a session.
  //
  // urnet::getFilteredLocationsFromResult does NOT do text matching - MEASURED,
  // after a first cut of this assumed otherwise and shipped a search box that
  // silently returned every country for every query. Its `filter` argument only
  // changes BUCKETING (sdk/locations_view_controller.go:205-262): a zero
  // MatchDistance goes to BestMatches instead of its type bucket, and the
  // cities/regions buckets are populated only when the filter is non-empty. The
  // narrowing itself is done SERVER-SIDE, which is why
  // LocationsViewController::FilterLocations dispatches on the query
  // (locations_view_controller.go:186-193) and why this does too:
  //
  //     query empty     -> Api::getProviderLocations      (the whole list)
  //     query non-empty -> Api::findProviderLocations{query}
  //
  // then buckets whichever result came back with that same query.
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
  // The ROW-CLICK variants of the two connects above, for the chooser sheet's
  // and the Network pane's select-and-connect rows. Same connect, two extra
  // rules (debounce + idempotence only — the rows keep their one meaning):
  //
  //   * COALESCED. Every row click is a real connect, and every connect makes
  //     the SDK tear down the current exit's provider window and queue a
  //     rebuild behind the connect repo's shared dial-pacing staircase
  //     (NextConnectTime reserves 100ms-1s of shared dial budget per cold dial
  //     and never rolls a reservation back when the waiter is cancelled). A
  //     burst of clicks therefore runs the staircase minutes ahead of `now`,
  //     and every exit born after that sits "transport down: verdicts held"
  //     until its evaluation expires — the owner's stuck pending-yellows. The
  //     staircase is the SDK's bug to fix; the app's share is to stop
  //     machine-gunning destination changes at it. A row click records the
  //     intent immediately but the session worker does not act until it has
  //     sat still for ~1.2s; a later click replaces it and restarts the clock.
  //   * IDEMPOTENT. Re-clicking the row the session is already driving at
  //     (selected AND connecting/connected) is a no-op instead of a rebuild of
  //     a window the user is happily inside — though it still cancels any
  //     newer pending row intent, so "click away, think better of it, click
  //     back" ends where it started.
  //
  // A pending row intent is CANCELLED by Disconnect and superseded by the
  // immediate entry points above (the connect page's button, the tray toggle):
  // all of them go through the same last-request-wins slot with no settle
  // delay. There is deliberately no UI timer behind this — the settle is the
  // session worker waiting on the request slot's condition variable, so it
  // works identically from every surface and thread that can issue a connect.
  void ConnectFromRow(const urnet::ConnectLocation& location);
  void ConnectBestAvailableFromRow();
  // Own presentation-only view controllers only while the WinUI window is
  // visible. The DeviceRemote and service tunnel remain alive in the tray.
  //
  // NON-BLOCKING (D4). The caller is the XAML thread on every window
  // show/hide/minimize, and the work behind this — listener unsubscribes and
  // view-controller closes on the way down, subscribes on the way up — is a
  // series of synchronous session rpcs behind mutex_, a lock the session
  // worker holds across whole bootstraps. Against a DYING service each of
  // those rpcs blocks until the transport notices, which is how Windows came
  // to kill this app as AppHangB1 three times, 4-5s after each daemon death.
  // This records the desired state and returns; the presentation worker
  // applies it (last write wins).
  void SetPresentationActive(bool active);

  void SetAuthStateHandler(AuthStateHandler h) { onAuth_ = std::move(h); }
  void SetAuthInvalidHandler(AuthInvalidHandler h) { onAuthInvalid_ = std::move(h); }
  void SetJwtRefreshedHandler(JwtRefreshedHandler h) { onJwtRefreshed_ = std::move(h); }
  void SetTunnelStateHandler(TunnelStateHandler h) { onTunnel_ = std::move(h); }
  // Live stats push (connection/throughput/provide). Fired on SDK listener
  // callbacks; the UI marshals to its thread and applies visibility gating.
  void SetStatsHandler(StatsHandler h) { onStats_ = std::move(h); }
  LiveStats CurrentStats();  // snapshot on demand (e.g. resync when window shows)
  // Read a fresh snapshot and push it through the stats handler, exactly like
  // an SDK listener firing. For LiveStats::healthReevalAtMillis: the degrade
  // hold expires on the clock, and this is the clock's way to reach every
  // consumer (window AND tray) through the one existing path instead of a
  // side-channel that could disagree with it.
  void RepublishStats() { PublishStats(); }

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
  // R4: a SECOND, independent subscriber to the same two feeds.
  //
  // The chooser sheet owns the handlers above, and the Network destination is a
  // second live consumer of exactly the same pushes - it IS the chooser, as a
  // page. One slot cannot serve both: whichever of the two set it last would
  // silently unsubscribe the other, and the failure mode is a location list that
  // never updates, which reads as a hang rather than as a bug.
  //
  // Deliberately a second SLOT rather than a subscription list. There are
  // exactly two consumers, both owned by the window for the window's lifetime,
  // and a list would need removal tokens and a lock for no behaviour anyone
  // wants. Both are invoked on the SDK callback thread, observer first, with the
  // payload COPIED to the observer and moved into the handler - so neither can
  // observe the other's move.
  void SetLocationsObserver(LocationsHandler h) { onLocationsObserver_ = std::move(h); }
  void SetPeersObserver(PeersHandler h) { onPeersObserver_ = std::move(h); }
  void SetRemoteChangedHandler(RemoteChangedHandler h) { onRemoteChanged_ = std::move(h); }
  void SetProviderLocationsHandler(ProviderLocationsHandler h) {
    onProviderLocations_ = std::move(h);
  }
  void SetProviderIdentitiesHandler(ProviderIdentitiesHandler h) {
    onProviderIdentities_ = std::move(h);
  }
  void SetProviderSelectionHandler(ProviderSelectionHandler h) {
    onProviderSelection_ = std::move(h);
  }

  // ---- provider locations (the "Connected to N providers" detail sheet) -----
  // The rows come from ProviderLocationsViewController::getProviderLocations:
  // the same window Device::getConnectedProviderLocations reports, in the SDK's
  // shared DISPLAY ORDER (west to east about the providers' centroid, then the
  // ones with no coordinates), so the list reads left to right in the order the
  // globe's wheel steps through. The change listener is signal-only -- so the
  // getter is re-read on every notify and the result compared BY VALUE before
  // anything is published (the SDK re-emits on every window event, and the rows
  // also carry a per-second duration clock, so an identity compare would thrash
  // the UI).
  std::vector<ProviderLocationRow> CurrentProviderLocations();
  // The providers with an identity-verified e2e session, joined by egress
  // client id onto the provider-locations rows to badge the encrypted ones.
  // Same signal-only-listener + value-compare discipline as the locations feed.
  std::vector<ProviderIdentityRow> CurrentProviderIdentities();
  // Drop a provider by its EGRESS client id and stop it being re-discovered for
  // the rest of this connection.
  void RemoveConnectedProvider(const std::string& clientId);

  // ---- the globe's selection and scroll wheel ------------------------------
  // The SDK's shared ProviderLocationsViewController, which every URnetwork app
  // binds so they all traverse the globe identically. StepProviderSelection
  // moves along the plottable providers ordered west to east relative to their
  // centroid and CLAMPS at the ends: stepping past the extreme west or east
  // sticks there instead of cycling round the globe. Removing the selected
  // provider hands the selection to the NEAREST one left. Changes arrive
  // through SetProviderSelectionHandler; "" means nothing is selected.
  std::string SelectedProviderClientId();
  void SetSelectedProviderClientId(const std::string& clientId);
  void StepProviderSelection(int steps);

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
  // Returns false when the setting did NOT fully apply. Callers must read
  // CurrentKillSwitch() back and show the real state: a toggle left On over a
  // setting that did not take is the one failure mode here that costs privacy.
  bool SetKillSwitch(bool on);
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

  // ---- reliability / developer surface -------------------------------------
  // All three take mutex_ and issue synchronous RPCs to the service, so they
  // BLOCK. Never call them from the UI thread; the developer page runs them on
  // a worker and marshals the result back. Taking mutex_ is also what
  // serialises them against each other, which the read-modify-write below
  // depends on (iOS gets the same property from its serial bridgeQueue).
  ReliabilitySnapshot ReadReliability();

  // Read-modify-write of the WHOLE ReliabilitySettings struct from a FRESH
  // read — never from a cached snapshot, because every field the caller does
  // not touch is written back verbatim and a stale snapshot would revert
  // whatever changed underneath it.
  //
  // A NULLOPT fresh read is a NO-OP, not a write of a zeroed struct: see
  // ReliabilitySnapshot::settings. Returns the settings the device actually
  // applied (read back after the write), or nullopt if there was nothing to
  // write to.
  std::optional<urnet::ReliabilitySettings> UpdateReliabilitySettings(
      const std::function<void(urnet::ReliabilitySettings&)>& mutate);

  // `issued` is false when there is no session or the rpc threw. A void version
  // of this let the developer screen render "Requested: sync" on a screen that
  // simultaneously said there was no session.
  //
  // For MigrateExit and ProbeAllExits the result also carries the SDK's own
  // count (hasCount), so those two can report what they DID rather than what
  // was asked. For the other four "issued" is still the ceiling.
  ReliabilityActionResult RunReliabilityAction(ReliabilityAction action,
                                               const std::string& exitClientId = {});

  // ---- D6: fault injection + the probe suite --------------------------------
  //
  // The stale comment these replaced said dropExit/stallExit/shuffleExits and
  // the probe suite were DeviceLocal-only with no DeviceRemote equivalent. They
  // have all seven been on DeviceRemote since S1 (urnetwork_sdk.hpp:10114-10150,
  // exported at urnetwork_sdk.def:334-370), which is why this section exists.
  //
  // These are FAULT INJECTION: they deliberately degrade a live connection.
  // Two rules follow from that and neither is optional:
  //
  //   1. IMMEDIATE-OR-NOTHING. Never queue one into sync state and never retry
  //      client-side. A dropped exit replayed after an RPC reconnect drops a
  //      DIFFERENT, healthy exit minutes later, which is the bug S1 fixed and
  //      which the SDK now pins with TestDeviceRemoteAdvancedModeActionsAreNever
  //      Queued. Every method here is one call under the lock, and a throw is
  //      reported, not retried.
  //   2. The log must NAME the exit. These are destructive by design and
  //      Advanced Mode does not gate them behind a modal, so the log is the only
  //      record of what was done to which exit.
  //
  // Same threading contract as the three above: they take mutex_, they issue a
  // synchronous rpc, they BLOCK. Never call from the UI thread.

  // Force the exit out of the window. Returns false when there was no session,
  // the rpc threw, or the SDK declined (no such exit / no multi client).
  bool DropExit(const std::string& exitClientId);
  // Mark the exit stalled (or clear it). urnet::Exit carries no stalled flag, so
  // the client cannot render current state — the caller offers both directions.
  bool StallExit(const std::string& exitClientId, bool stalled);
  // Reshuffle the whole exit window. Device-wide, not per-exit.
  void ShuffleExits();

  // Start the probe suite. A nullopt config asks the SDK for its own default
  // (urnet::getDefaultProbeSuiteConfig) rather than a zeroed struct, which would
  // be a suite with zero concurrency and zero timeout.
  bool StartProbeSuite(const std::optional<urnet::ProbeSuiteConfig>& config = std::nullopt);
  void StopProbeSuite();
  bool ProbeSuiteRunning();
  // ReadSdkList-guarded: Go marshals a nil slice as the 4-byte document `null`
  // and the unwrap throws type_error.302. Empty, never an exception.
  std::vector<urnet::ProbeResult> GetProbeResults();

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
      // The service is unreachable/too old/refused, or a constructed session
      // has not completed its authenticated control channel by the bounded
      // deadline. The user remains SIGNED IN, and the pending case is
      // non-destructive: its tunnel is left running while observation continues.
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
  // Guarded by its OWN small lock, not mutex_, for two reasons that pull in
  // opposite directions:
  //
  //   - It cannot be unguarded. The bootstrap thread calls PublishSessionFailure
  //     OUTSIDE mutex_ (SdkHost::Initialize), so it reads and invokes this
  //     std::function while the view is assigning it from the UI thread. That is
  //     a concurrent write and copy of a std::function: undefined behaviour, and
  //     the trigger is the default dev-box state — signed in, service not
  //     running, so the bootstrap fails fast — plus a tray click at startup.
  //   - It must not be mutex_. The bootstrap thread holds mutex_ across the
  //     WHOLE of BootstrapSession, which is several synchronous service rpcs; a
  //     setter that waited on it would block the UI thread inside MainWindow's
  //     constructor and the first tray click would produce a frozen window.
  //
  // noticeMutex_ is therefore held only for the assignment and for copying the
  // handler out before it is invoked — never across the invocation, and never
  // together with mutex_ in the other order.
  void SetModeNoticeHandler(ModeNoticeHandler h);
  // A SECOND, independent subscriber to the same notices, for the same reason
  // SetLocationsObserver exists (see the R4 note there).
  //
  // THIS CHANNEL HAD TWO CONSUMERS AND ONE SLOT, and which of them won was a
  // THREAD RACE. MainWindow's constructor binds the window-level snackbar
  // (MainWindow.xaml.cpp) and DeveloperPage's constructor binds the Developer
  // InfoBar; the page is constructed FIRST, so the window's binding replaces it
  // — except that the page also asks for a replay on its own bridge thread, and
  // whichever of the two got there first is the one that rendered. Observed
  // live: "developer: mode notice cleared" in the log from a run whose snackbar
  // never fired. A channel whose whole job is "never fail silently" cannot have
  // a delivery that depends on which thread woke up first.
  //
  // Both slots are invoked, observer first, on the publishing thread. Same
  // threading contract as SetModeNoticeHandler: marshal and return.
  void SetModeNoticeObserver(ModeNoticeHandler h);
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

  // ---- Advanced Mode (D5) ---------------------------------------------------
  //
  // The persisted app-wide toggle: Normal assumes the VPN just works and hides
  // everything that operating it would need; Advanced reveals raw values, ids,
  // the tuning surface and the Developer destination on EVERY page. Not a page —
  // a reading that every page has two of.
  //
  // IT IS STANDING STATE, NOT AN EVENT, and that is the whole design of this
  // block. The value is loaded from disk in Initialize(), which runs at startup;
  // the main window is not built until the first tray click, which on this
  // machine has been observed 25 SECONDS later. A pure "advanced mode changed"
  // notification produced at load time has no handler to receive it, is dropped,
  // and the window then builds its NORMAL reading over a preference the user set
  // days ago — with no second event ever coming to correct it, because nothing
  // changed. That is precisely the failure sessionFailure_ / PublishModeNotice
  // exist to prevent (read the field comment on sessionFailure_ for the
  // timestamps), and this project has now been bitten by that shape twice.
  //
  // So the contract is the mode notice's contract:
  //   CurrentAdvancedMode()      valid at ANY time, including before any view
  //                              exists. This is the authority.
  //   SetAdvancedModeHandler()   an optimisation for changes AFTER a view binds.
  //   RefreshAdvancedMode()      replays the standing value to a view that was
  //                              built later. A new surface binds, then refreshes
  //                              — the same two-line pair MainWindow already uses
  //                              with SetModeNoticeHandler/RefreshModeNotice.
  //
  // THREADING: advancedMode_ is atomic, so the read costs nothing and needs no
  // lock; the handler has its own small lock (advancedMutex_) which is NEVER
  // held across the invocation and NEVER taken together with mutex_, for the
  // reasons spelled out on SetModeNoticeHandler. Handlers are invoked on the
  // caller's thread and must marshal.
  bool CurrentAdvancedMode() const {
    return advancedMode_.load(std::memory_order_acquire);
  }
  // Persist and publish. Writing the same value still publishes: a caller that
  // has just built a surface may be using this as its seed.
  void SetAdvancedMode(bool on);
  using AdvancedModeHandler = std::function<void(bool)>;
  void SetAdvancedModeHandler(AdvancedModeHandler h);
  void RefreshAdvancedMode();

 private:
  urnet::NetworkSpace BuildNetworkSpace();

  // ---- the session worker (one bootstrap at a time) -------------------------
  //
  // What a caller wants done once there is a session. `kind` is deliberately
  // separate from `location`: "best available" is also a nullopt location, so a
  // bare optional could not tell the two apart.
  enum class ConnectKind { None, BestAvailable, Location, Disconnect };
  struct SessionRequest {
    ConnectKind kind = ConnectKind::None;
    std::optional<urnet::ConnectLocation> location;
    // Static string, for the log. Never user-facing.
    const char* reason = "";
    // Row clicks only (ConnectFromRow / ConnectBestAvailableFromRow): the
    // worker must not consume this request before `notBefore`, and every
    // replacement carries its own deadline — so a click burst keeps pushing
    // the one pending intent forward and only the last click ever fires.
    // Immediate requests leave it at the epoch, which is always in the past.
    std::chrono::steady_clock::time_point notBefore{};
    // Eligible for CancelPendingRowConnect: only a settling row click may be
    // silently discarded. An explicit press or a Disconnect never is.
    bool coalesced = false;
    // Watchdog retries retain the already-published standing failure instead
    // of re-publishing it on every backoff attempt.
    bool automaticRecovery = false;
  };
  // The queued intent in the vocabulary the pure decision table speaks
  // (Common/ConnectAction.h). Location covers both the immediate "connect to
  // this one" and the coalesced row click: they reach the same worker and want
  // the same plan, and the table pins them as separate rows so that a future
  // divergence has to be a deliberate one.
  static constexpr gesture::Gesture GestureOf(ConnectKind kind) {
    switch (kind) {
      case ConnectKind::BestAvailable: return gesture::Gesture::Connect;
      case ConnectKind::Location:      return gesture::Gesture::ConnectRow;
      case ConnectKind::Disconnect:    return gesture::Gesture::Disconnect;
      case ConnectKind::None:          break;
    }
    return gesture::Gesture::EnsureSession;
  }
  // Record the request and make sure a worker is running. Takes ONLY
  // pendingMutex_ — never mutex_ — so it is safe from the UI thread while a
  // bootstrap is in flight.
  void RequestSession(SessionRequest request);
  // Drain and service requests until there are none left, then exit. The
  // "worker is alive" flag is cleared under pendingMutex_, the same lock a
  // producer holds while it tests it, so a request that lands as the worker is
  // finishing cannot be dropped.
  void SessionWorkerLoop();
  // Apply the request's connect intent. Caller holds mutex_.
  void ConnectLocked(const SessionRequest& request);
  // Row-click support (see ConnectFromRow). Whether the session is already
  // driving at the clicked target: `matches` is handed the SDK's own selected
  // location, and the answer is AND-ed with the connection status being
  // active (connecting or connected) — a selection the user has since
  // disconnected from must reconnect, not no-op. Deliberately lock-free, the
  // same pattern as ReadStats: it runs on the UI thread inside a click, and
  // mutex_ can be held across a whole bootstrap.
  bool RowClickIsCurrent(
      const std::function<bool(const std::optional<urnet::ConnectLocation>&)>& matches);
  // Discard a pending, still-settling row intent (and only that kind). Called
  // when a re-click of the current location makes the pending intent moot.
  void CancelPendingRowConnect(const char* why);

  std::mutex pendingMutex_;
  // Signalled on every RequestSession and on CancelPendingRowConnect, so a
  // worker sleeping out a row click's settle deadline re-reads the slot the
  // moment a replacement (possibly an IMMEDIATE one, e.g. Disconnect) lands.
  std::condition_variable pendingCv_;
  SessionRequest pending_;
  bool pendingRequested_ = false;
  bool sessionWorkerAlive_ = false;

  // ---- the service-reconnect watchdog ---------------------------------------
  //
  // The control channel dropping is the app's only notice that the service (and
  // with it the tun, its routes and the whole WFP session) is gone, and NOTHING
  // used to schedule a retry: PipeClient never reconnects by design, and the
  // only thing that called ServiceClient::Connect() was BootstrapSession, which
  // only ran at launch and at sign-in. So a service restarted under a running
  // app was invisible to it forever.
  //
  // This waits for the pipe to come back and asks the session worker for a
  // quiet recovery attempt. It also survives a listening pipe whose hello is
  // temporarily unusable (notably an installer replacing a protocol-v2
  // service with v3). Attempts use ServiceRecoveryPolicy's capped exponential
  // schedule, and never re-publish the standing failure on each attempt.
  void ScheduleServiceRetry();
  void ServiceWatchdogLoop();
  void StopServiceWatchdog();  // called from the destructor; joins the thread

  std::thread watchdog_;
  std::mutex watchdogMutex_;
  std::condition_variable watchdogCv_;
  bool watchdogStop_ = false;
  bool watchdogRunning_ = false;
  std::atomic<bool> serviceRecoveryNeeded_{false};

  // ---- the presentation worker (D4) -----------------------------------------
  //
  // Applies SetPresentationActive off the XAML thread. One desired value, last
  // write wins; the worker drains it and exits, and a write that lands while it
  // is finishing either sets `dirty` before the exit check (same lock) or finds
  // `running` false and starts a fresh worker — never dropped, never two.
  void PresentationWorkerLoop();
  void StopPresentationWorker();  // called from the destructor; joins the thread

  std::thread presentationWorker_;
  std::mutex presentationMutex_;
  bool presentationStop_ = false;
  bool presentationWorkerRunning_ = false;
  bool presentationDesired_ = false;
  bool presentationDirty_ = false;

  // ---- the rpc-sync watchdog (does the session we built actually PAIR?) -----
  //
  // WHY THIS EXISTS AT ALL, and why it matters more than the pairing fix it
  // ships beside. Every C++ call in BootstrapSession succeeds OFFLINE: the
  // DeviceRemote constructor, setRpcServer, every addXListener and every getter
  // return without ever needing the service to answer. So "session bootstrapped"
  // was logged, the app rendered, and NOTHING in this process could tell that
  // DeviceLocalRpc.Sync was refusing all of it — the owner's app sat reading
  // Disconnected over a live tunnel for a whole session while the SDK logged
  // "device instance mismatch" 59 times where no app surface would ever see it.
  // A refusal is permanent by construction (the remote retries the same rejected
  // pairing every 500ms forever), so there is no amount of waiting that fixes it.
  //
  // The design is therefore: inspect immediately, then keep a small bounded
  // poll alive for the lifetime of the generation. Pending sync is warned once
  // after the settle interval; a refusal is terminal and handled immediately;
  // a healthy session is checked less frequently so a later transport refusal
  // cannot become invisible.
  //
  // Generation, not a pointer: a session torn down and rebuilt while a check was
  // pending could hand back a DeviceRemote at the same address, and acting on
  // the WRONG session here means stopping a tunnel that is working.
  static constexpr std::chrono::milliseconds kSyncSettleDeadline{5000};
  static constexpr std::chrono::milliseconds kSyncFailureDeadline{20000};
  static constexpr std::chrono::milliseconds kSyncPendingPoll{500};
  static constexpr std::chrono::milliseconds kSyncHealthyPoll{2000};
  enum class RpcSyncState { Stale, Healthy, Pending, Refused };
  void ArmSyncWatchdogLocked(std::uint64_t generation, bool reattached);
  void SyncWatchdogLoop();
  // One observation. Caller must not hold mutex_; this serializes with the
  // current bootstrap and may tear down only the matching generation.
  RpcSyncState CheckSessionSync(std::uint64_t generation, bool reattached);
  void StopSyncWatchdog();  // called from the destructor; joins the thread

  std::thread syncWatchdog_;
  std::mutex syncMutex_;
  std::condition_variable syncCv_;
  bool syncStop_ = false;
  bool syncRunning_ = false;
  std::uint64_t syncGeneration_ = 0;
  bool syncReattached_ = false;
  std::chrono::steady_clock::time_point syncDeadline_{};
  std::chrono::steady_clock::time_point syncPendingSince_{};
  bool syncPendingWarned_ = false;
  bool syncPendingFailurePublished_ = false;
  std::uint64_t syncPendingFailureGeneration_ = 0;
  void PublishPendingSyncFailure(std::uint64_t generation);
  // Bumped under mutex_ every time device_ is created or destroyed. A watchdog
  // holding a stale value has nothing to say about the session that is live now.
  std::uint64_t sessionGeneration_ = 0;
  // Lock-free guards for the SDK's remote-change callback. It may run while
  // BootstrapSession holds mutex_, so taking that lock merely to mark the
  // encrypted credential envelope confirmed would risk a callback deadlock.
  std::atomic<std::uint64_t> activeRpcPersistenceGeneration_{0};
  std::atomic<std::uint64_t> confirmedRpcPersistenceGeneration_{0};
  std::mutex rpcPersistenceMutex_;
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
  //
  // `stopTunnel` is the ORDERING FIX as well as a switch. When true the
  // stop_tunnel goes out FIRST, before device_->close() — the app-side mirror
  // of the rule StopBudget.h established in the service: the cheap, local,
  // safety-critical half (routes, dns, firewall — 133 ms, measured) must never
  // be sequenced behind the half that can block indefinitely. It used to be the
  // other way round here, so a wedged DeviceRemote::close() held the machine's
  // routes hostage on the logout path.
  //
  // The session worker passes FALSE, because it sequences the stop itself
  // (gesture::Plan::stopTunnel) and because Connect needs the opposite of a
  // stop: dropping a stale DeviceRemote must NOT lift the firewall policy, or
  // a reconnect with the kill switch on would open the machine for the length
  // of a bring-up. start_tunnel is the reconciler there.
  void TeardownSessionLocked(bool stopTunnel = true);
  void SetupWalletCallbacks();
  // The wallet signed the challenge: authLogin{wallet_auth}. `signature` is what
  // the chain's verifier expects (base64 for SOL, hex for TAO).
  void AuthLoginWithWallet(const std::string& address, const std::string& signature,
                           const std::string& message, WalletConnect::Provider provider);
  // The browser returned a Google id token: authLogin{auth_jwt_type:"google"}.
  // An identity with no network yet is retained in pendingAuthJwt_ and the UI
  // routes to the create-network step, exactly as the wallet path does.
  void AuthLoginWithGoogle(const std::string& idToken, std::function<void(AuthResult)> done);
  // Bring up the controlling DeviceRemote — by reattaching to a session the
  // service already holds (the saved-blob path), or by asking the service to
  // start one. `reason` is the gesture's static reason string; the one
  // start_tunnel site logs it, so no session start can appear in a log without
  // saying who asked (D8's missing space-switch line).
  //
  // `attachOnly` is the D8 owner decision as a parameter: true means "adopt a
  // running session if there is one, start NOTHING otherwise" — the resume
  // path, a network-server change and the service-reconnect watchdog, none of
  // which is a person asking for a VPN. A decline sets bootstrapDeclined_ and
  // returns false with bootstrapError_ empty; it is policy, not failure.
  bool BootstrapSession(const char* reason, bool attachOnly);
  // ASK THE SERVICE WHAT IS ACTUALLY INSTALLED. One get_state rpc, once per
  // gesture, and the ONLY admissible answer to "is there a tunnel right now" —
  // see the contract in Common/ConnectAction.h.
  //
  // `answered` is the whole point of the signature. On no channel or a failed
  // call this returns a DEFAULT-CONSTRUCTED status, which reads as "nothing is
  // installed on this machine" — a sentence nobody said. The decision must be
  // able to tell that apart from a service that really has nothing running, and
  // no field of the reply can carry that distinction: they all have legitimate
  // defaults. (The first version tried `service_version`, which is empty in
  // every build of this SDK, so `known` was false forever and Connect never
  // took the branch that starts a tunnel.) Caller holds mutex_.
  proto::TunnelStatus CurrentServiceStatusLocked(bool& answered);
  void SetAuthState(AuthState s, const std::string& error = {});
  void SubscribeStats();          // caller holds mutex_; opens presentation controllers
  LiveStats ReadStats();          // read the current snapshot from the SDK getters
  void PublishStats();            // ReadStats() -> onStats_
  // Drawer feeds: subscribe listeners (in BootstrapSession) and publish
  // snapshots, only on change (block actions storm per routing decision).
  void SubscribeDrawer();
  // EnsureLocations without the lock: the third presentation-scoped subscribe,
  // alongside SubscribeStats and SubscribeDrawer, so the three can be re-armed
  // together from BootstrapSession and SetPresentationActive. mutex_ is not
  // recursive, so the public EnsureLocations() cannot be called from either.
  void EnsureLocationsLocked();  // caller holds mutex_
  // The no-device half of the above: bring the api cache into line with
  // apiLocationsQuery_, kicking a fetch only if the cache does not already
  // answer that exact query and no fetch for it is already in flight. Caller
  // holds mutex_ and must NOT hold apiLocationsMutex_.
  void EnsureApiLocationsLocked();  // caller holds mutex_
  // Re-bucket the cached api result at the current query and push it to both
  // handler slots. Silent while the view-controller feed is open. Takes
  // apiLocationsMutex_ internally and RELEASES it before invoking the handlers.
  // Callable from any thread; must not be called holding apiLocationsMutex_.
  void PublishApiLocations();
  // urnet::getFilteredLocationsFromResult over the cache, ReadSdkList-guarded
  // (FilteredLocations is struct-shaped but all six fields are `*List`; same
  // rule as every sibling getter). nullopt when nothing has been fetched yet -
  // deliberately NOT an engaged all-nullopt value, which is exactly the shape
  // the LOCATIONS_LOADING push has and is indistinguishable from "loaded, zero
  // providers" without the state string. Caller holds apiLocationsMutex_.
  std::optional<urnet::FilteredLocations> FilteredApiLocationsLocked();
  void ClosePresentationLocked();
  void PublishThroughput();
  void PublishContractRows();
  void PublishBlockActions();
  void PublishBlockStats();
  void PublishSplitRules();
  void PublishProviderLocations();
  void PublishProviderIdentities();
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
  // the provider globe's selection + scroll wheel, shared across every app
  std::optional<urnet::ProviderLocationsViewController> providerLocationsVc_;
  // network-name availability at sign-up; api-scoped, so it survives logout
  std::optional<urnet::NetworkNameValidationViewController> networkNameVc_;
  std::vector<urnet::Sub> subs_;
  std::vector<urnet::Sub> presentationSubs_;
  bool presentationActive_ = false;

  // ---- provider locations with NO service session (class A) -----------------
  //
  // Guarded by their OWN mutex, not mutex_, for two reasons. The completion
  // fires on an SDK/Go thread and mutex_ is held across a whole BootstrapSession
  // (service connect, hello, start_tunnel - seconds); and the fetch is kicked
  // from EnsureLocationsLocked, i.e. WITH mutex_ already held, so an SDK that
  // ever completed inline would self-deadlock on a non-recursive std::mutex.
  // Lock order is mutex_ -> apiLocationsMutex_ and never the reverse.
  std::mutex apiLocationsMutex_;
  // The raw get/findProviderLocations document currently on screen.
  std::optional<urnet::FindLocationsResult> apiLocations_;
  // urnet::LocationsLoading / LocationsLoaded / LocationsError for the cache
  // above, so the pane can tell loading from failed from genuinely-zero. Empty
  // until the first fetch is kicked.
  std::string apiLocationsState_;
  // Three queries, and they are NOT interchangeable. `Query` is what the user
  // wants (the search box); `LoadedQuery` is what apiLocations_ actually answers
  // and is therefore the one the buckets must be computed with; `PendingQuery`
  // is what the in-flight fetch will answer, which is what stops a fetch already
  // running for this exact query from being kicked a second time.
  std::string apiLocationsQuery_;
  std::string apiLocationsLoadedQuery_;
  std::string apiLocationsPendingQuery_;
  bool apiLocationsInFlight_ = false;
  // Bumped on every kick; a completion whose generation no longer matches is a
  // superseded fetch and is dropped rather than allowed to overwrite a newer
  // answer.
  uint64_t apiLocationsGeneration_ = 0;
  // THE SINGLE-WRITER GATE. True for exactly as long as locationsVc_ is open.
  // Atomic because the api completion reads it from an SDK thread while the
  // view controller is opened and closed under mutex_ on another. While it is
  // set, the api path pushes NOTHING: the device feed is the better source (it
  // is live and it drives server-side search) and two writers into one UI state
  // is the bug this exists to prevent.
  std::atomic<bool> deviceFeedOpen_{false};

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
  // The value-compare baselines for the two signal-only provider feeds; see
  // CurrentProviderLocations() for why an identity compare is not enough.
  std::vector<ProviderLocationRow> lastProviderLocations_;
  std::vector<ProviderIdentityRow> lastProviderIdentities_;

  // The session status to report for "we have a device, and it is/isn't on a
  // location" — derived from sessionMode_ so an rpc-only session can never
  // surface TunnelState::Up. Every place that used to hand-build such a status
  // goes through here.
  proto::TunnelStatus SessionStatus(bool haveLocation) const;
  // Take the two facts only the SERVICE can observe out of ANY status it sent —
  // an event, or the reply to hello/get_state. It used to be done only for
  // events, so a reattach (app restarted over a live session) adopted neither:
  // hello carries both and they were read for `state`/`mode` and dropped, and
  // the first thing the reattached session then pushed was a synthesised
  // SessionStatus carrying the DEFAULTS — dns_applied=false, wfp_state=off —
  // which renders a healthy protected tunnel as degraded and unguarded until
  // some later start/stop event happens to correct it.
  void AdoptServiceFacts(const proto::TunnelStatus& st);

  // ---- R1 SELF-EXCLUSION FOR *THIS* PROCESS --------------------------------
  //
  // Pin this process's SDK sockets to the physical NIC while the service's
  // tunnel is up, and unpin when it is not.
  //
  // WHY THIS EXISTS AT ALL. The service solved R1 for itself at step 2/8 —
  // EgressMonitor -> urnet::setEgressInterfaceIndex, i.e. IP_UNICAST_IF on every
  // socket the SDK opens — so its own platform traffic never follows the routes
  // it is about to install into the tun. That setter is PROCESS-GLOBAL inside
  // one loaded copy of URnetworkSdk.dll, and this app loads its OWN copy with
  // its OWN Go runtime for its OWN Api/LocalState/DeviceRemote. So the service's
  // bind covers urnetworkd and cannot reach here, and both mechanisms that were
  // supposed to keep our platform traffic out of our own tunnel — the egress
  // bind and the WFP app-id permit — were designed around the service and never
  // covered the UI. Measured 2026-08-08: "[dtm]failed to refresh JWT: Timeout."
  // from URnetwork.exe (pid 2584) while a tunnel was up.
  //
  // THE INDEX COMES FROM THE SERVICE (TunnelStatus::egress_index4). It is not
  // recomputed here on purpose: the correct answer requires excluding the tun's
  // LUID, only the service knows it, and EgressMonitor already implements the
  // retain-last-good-rather-than-unbind rule that R1 depends on. Two
  // implementations of that rule would eventually disagree, and the failure mode
  // of disagreeing is a socket that silently follows the tun.
  //
  // IT IS HALF OF A PAIR. On its own it would make things WORSE, not better: the
  // baseline WFP floor blocks everything that is not urnetworkd, loopback, LAN
  // or the tun, so an app socket moved onto the physical NIC would be blocked
  // outright. The other half is the Connected-only app-id permit the service
  // installs (WfpConfig::app_image_path). Do not land one without the other.
  //
  // WHAT IT DOES NOT FIX, stated so nobody assumes otherwise: name resolution.
  // Go on Windows resolves through GetAddrInfoW, so the query leaves svchost.exe
  // and goes to whatever resolver the stack picks — which, while connected, is
  // the tunnel's, over the tun. A cold cache plus a tunnel with no working exit
  // still cannot resolve. This moves the SOCKETS, not the resolver.
  //
  // Idempotent and change-gated; safe from the pipe reader thread.
  void ApplySdkEgressBind(int64_t index4, int64_t index6, const char* why);

  // The control channel dropped. Runs on the pipe reader thread.
  void OnServiceDisconnected();
  // Build and push the persistent notice from the CURRENT session state.
  // Caller holds mutex_ (it reads device_).
  void PublishModeNotice();
  // Push a "there is no usable app control session, and here is why" notice.
  // The user stays signed in; see ModeNotice::Kind::SessionFailed.
  void PublishSessionFailure(const std::string& why);
  // Copies of the two handlers, taken under noticeMutex_. Invoke the COPIES, so
  // the lock is never held across the calls.
  ModeNoticeHandler ModeNoticeHandlerCopy() const;
  ModeNoticeHandler ModeNoticeObserverCopy() const;
  // Deliver `notice` to both slots, observer first.
  void DeliverModeNotice(const ModeNotice& notice) const;

  ServiceClient service_;
  // Set once in Initialize() from URNETWORK_RPC_ONLY; never changes after.
  proto::StartMode requestedMode_ = proto::StartMode::Tunnel;
  // Set from the service's reply/hello whenever a session is established, and
  // reset here when one is torn down. Defaults to RpcOnly — the mode that
  // CLAIMS LESS — matching the policy TunnelStatus::from_json states for an
  // unreadable mode. With no session there is certainly no tunnel, so a stray
  // read before or after one must not be able to render "connected".
  std::atomic<proto::StartMode> sessionMode_{proto::StartMode::RpcOnly};
  // "There is a session AND the service that holds it is still there." Written
  // wherever device_ is created or destroyed and cleared when the control
  // channel drops (the service owns the tunnel: its process dying takes the
  // session with it, whatever this side still holds). Atomic because the strip
  // reads it from the UI thread — see HasSession().
  std::atomic<bool> hasSession_{false};
  // The rpc endpoint of the LIVE session, so the statuses this process
  // SYNTHESISES (SessionStatus) carry it too. Without this the advanced strip's
  // RPC field read "none" for the whole life of a healthy session, because the
  // only statuses that ever carried a host:port were the ones the service sent.
  //
  // Guarded by wfpStateMutex_, NOT mutex_: SessionStatus() is called from SDK
  // listener callbacks that do not hold mutex_, and this is already the lock
  // over "the last facts we know about the session".
  std::string sessionRpcHostPort_;
  // The last values the SERVICE reported for the two facts only it can observe:
  // whether the tun's resolvers actually took, and whether the WFP
  // leak-prevention policy is in force. Cached so SessionStatus() — which this
  // process synthesises — reports what is true instead of the struct defaults.
  // Both default to the DEGRADED reading, so a status pushed before the service
  // has ever spoken understates protection rather than overstating it.
  std::atomic<bool> lastServiceDnsApplied_{false};
  // ...and the third, which used to be SYNTHESISED from the mode flag
  // (`mode == Tunnel && service_.IsConnected()`). That inference is the class
  // of belief the disconnect bug was made of: after a Disconnect the mode flag
  // and the pipe were both still exactly as they had been, so every synthesised
  // status went on asserting "routes are installed right now" — the field
  // Protocol.h nominates as THE answer to "is my traffic going through the
  // tunnel" — with no idea whether they were. Carried from the service like its
  // two neighbours instead; the service is the process that owns the routes.
  std::atomic<bool> lastServiceRoutesInstalled_{false};
  // "THE LAST THING THE USER ASKED FOR WAS OFF." Written by the session worker
  // from the gesture it just dequeued, read by gesture::Decide for EnsureSession
  // alone (see AppFacts::userDisconnected).
  //
  // Deliberately NOT persisted. It is about this run of the app: a launch is a
  // fresh start and its resume path should bring the session up as it always
  // has. What it protects is the window in between — the service dying and
  // coming back, or a network-space change, silently re-installing the capture
  // routes on a machine whose owner pressed Disconnect and walked away. Before
  // Disconnect tore the DeviceRemote down, `if (device_)` made that impossible
  // by accident; it is a rule now instead.
  std::atomic<bool> userDisconnected_{false};
  mutable std::mutex wfpStateMutex_;
  std::string lastServiceWfpState_ = "off";
  // ---- aggregate connection health (#27) ------------------------------------
  // The one Tracker (ConnectionHealth.h) behind its own SMALL lock, never
  // mutex_: ReadStats is deliberately lock-free against mutex_ (it runs on the
  // UI thread and on SDK callback threads while a bootstrap can hold mutex_
  // for seconds), and the tracker is folded in at the end of ReadStats. The
  // lock covers Update/ReevalAtMillis as one unit and NoteNewAttempt from the
  // session worker; it is held across nothing else.
  mutable std::mutex healthMutex_;
  health::Tracker healthTracker_;
  // The egress binding currently in force on THIS process's SDK instance, as a
  // packed (index4, index6) pair, and the lock that keeps the compare and the
  // SDK call together.
  //
  // -1 means "never applied", which is distinct from 0 ("applied, and it is the
  // unbound state"): without that distinction the first status carrying 0 would
  // be a no-op and a stale bind from a previous session could survive.
  //
  // ITS OWN LOCK, AND NOT AN ATOMIC. There are two writers — the pipe reader
  // thread (pushed status) and the bootstrap thread (hello, and the start_tunnel
  // reply) — and with a bare compare-and-swap they can interleave so that the
  // LAST call into the SDK carries the FIRST thread's value. The state that
  // leaves behind is a process pinned to a stale interface with nothing left to
  // correct it, which is the exact failure this whole mechanism exists to
  // prevent. Held only across the compare and the setter, never across a
  // handler, and never together with mutex_ — the bootstrap thread holds mutex_
  // when it gets here, so taking them in the other order anywhere would be a
  // deadlock.
  std::mutex egressMutex_;
  int64_t sdkEgressBound_ = -1;
  // The STANDING reason there is no session, in words a user can act on, or
  // empty when there is nothing to report. Distinct from bootstrapError_, which
  // is per-attempt scratch: this survives the attempt so that a view created
  // LATER can still be told.
  //
  // It has to. The bootstrap runs on a background thread from Initialize(),
  // which is well before the first tray click — and the main window, and
  // therefore the notice handler, does not exist until that click. Observed
  // live: bootstrap failed at 18:01:58 with the service unreachable,
  // PublishSessionFailure found no handler bound and dropped the message, and
  // the window created 25 seconds later published an EMPTY notice from
  // RefreshModeNotice() and showed the user nothing at all. That is exactly the
  // "the app can fail to reach the service and say nothing on screen" this
  // notice channel exists to prevent, reintroduced by the ordering.
  //
  // Guarded by mutex_. Cleared on a successful bootstrap and on teardown.
  std::string sessionFailure_;
  // Why the last BootstrapSession() returned false, in words a user can act on.
  // Set on every failure path and read by the session worker; guarded by
  // mutex_, which BootstrapSession's caller already holds.
  std::string bootstrapError_;
  // Per-attempt classification: a missing/restarting/incompatible service can
  // recover without user input and keeps the capped service watchdog alive.
  // Credential/identity refusals are not retried blindly.
  bool bootstrapServiceRetryable_ = false;
  // The last BootstrapSession() returned false because the click-only rule
  // (D8) declined a cold start on an attach-only request — not because
  // anything failed. The worker reads it to log the outcome at INFO and skip
  // the failure notice. Guarded by mutex_ like bootstrapError_.
  bool bootstrapDeclined_ = false;
  std::string appVersion_ = "0.0.1";

  // Answer and clear whichever wallet-bridge flow is outstanding. Called when a
  // new one starts: the bridge has a single pair of callbacks, so the new flow
  // takes them over and the old one has to be TOLD rather than abandoned - an
  // abandoned callback is a busy flag nothing will ever clear.
  void CancelPendingWalletFlows(const char* reason);

  WalletConnect wallet_;
  std::function<void(AuthResult)> walletAuthDone_;
  // A bare signature request (SignWithSolanaWallet) rather than a sign-in. Non-
  // null is what tells the shared bridge callbacks which flow they are in, so
  // both are cleared whenever the other starts.
  std::function<void(bool, std::string, std::string, std::string)> walletSignDone_;
  std::string walletSignMessage_;
  // the instant network's jwt, held between CreateInstantAccount and
  // ConfirmInstantAccount so the seedphrase is read before the session exists
  std::optional<std::string> pendingInstantJwt_;
  // a Google id token whose identity has no network yet, held for the
  // create-network step (the auth-jwt analogue of pendingWalletAuth_)
  std::optional<std::string> pendingAuthJwt_;
  GoogleSignIn google_;
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
  // See SetModeNoticeHandler: this one is written from the UI thread and read
  // from the bootstrap thread, so it has its own lock. Read it through
  // ModeNoticeHandlerCopy(), never directly.
  mutable std::mutex noticeMutex_;
  ModeNoticeHandler onModeNotice_;
  // The Developer page's copy of the same notices (SetModeNoticeObserver).
  ModeNoticeHandler onModeNoticeObserver_;
  // sessionFailure_ is declared above with the field it belongs to. P2 and P5
  // arrived at the same design independently -- a bootstrap failure is standing
  // state, not an event, because it happens before any window exists to receive
  // it -- and the merge briefly carried both declarations.
  // ---- Advanced Mode (D5) ----
  // THE STANDING VALUE. Loaded from app_prefs.json in Initialize(), which runs
  // long before the main window exists, and read by every surface that has two
  // readings. Atomic rather than mutex_-guarded on purpose: it is read on the UI
  // thread from inside layout passes, and a getter that could block behind a
  // BootstrapSession holding mutex_ across several synchronous rpcs would freeze
  // the window. It is one bool and it is never read together with anything else.
  std::atomic<bool> advancedMode_{false};
  // The handler's own lock, on the same reasoning as noticeMutex_: never held
  // across the invocation, and never taken together with mutex_ in either order.
  // Read the handler through AdvancedModeHandlerCopy(), never directly.
  mutable std::mutex advancedMutex_;
  AdvancedModeHandler onAdvancedMode_;
  AdvancedModeHandler AdvancedModeHandlerCopy() const;

  DnsSettingsHandler onDnsSettings_;
  BlockerEnabledHandler onBlockerEnabled_;
  LocationsHandler onLocations_;
  PeersHandler onPeers_;
  // R4: the Network destination's copy of the two feeds above (SetLocationsObserver)
  LocationsHandler onLocationsObserver_;
  PeersHandler onPeersObserver_;
  RemoteChangedHandler onRemoteChanged_;
  ProviderLocationsHandler onProviderLocations_;
  ProviderIdentitiesHandler onProviderIdentities_;
  ProviderSelectionHandler onProviderSelection_;
  AuthState authState_ = AuthState::LoggedOut;
  // "Is there a stored device session?" — cached so IsLoggedIn() does not have
  // to take mutex_, which on a resume meant waiting out the whole tunnel
  // bootstrap before the main window could be created. Written wherever the
  // stored client jwt changes; see IsLoggedIn().
  std::atomic<bool> loggedIn_{false};
};

}  // namespace urnw
