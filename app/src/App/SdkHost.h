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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "AddSignIn.h"
#include "BalanceGate.h"
#include "ClientEvents.h"
#include "ConnectAction.h"
#include "ConnectionHealth.h"
#include "ExtenderPresentation.h"
#include "ExtenderReset.h"
#include "MobileBroadband.h"
#include "FeedbackLogUpload.h"
#include "NetworkCountryWatch.h"
#include "PostQuantumIdentity.h"
#include "ProvideLifecycle.h"
#include "ProviderLocations.h"
#include "ProviderStatusPresentation.h"
#include "Sdk.h"
#include "ServiceClient.h"
#include "ServiceRecoveryPolicy.h"
#include "SignOut.h"
#include "VerifySendNotice.h"
#include "Version.h"
#include "WalletBridgeRoute.h"
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
  // With verification_required: whether the server sent the code (send_error).
  VerifySendNotice verify_send;
};

// The text for a refused wallet sign-in, network create or added sign-in
// method: a pasted signature from another account than the entered address
// (the server's signature_mismatch after a manual Bittensor wallet,
// bittensor::ConnectErrorKey) says so with the wallet's name; anything else is
// the server's `message`. `bittensorWalletId` is "" for a Solana wallet.
std::string WalletProofRefusalText(const std::string& code, const std::string& message,
                                   const std::string& bittensorWalletId);

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
  // provideClients is unknown, not zero, and nothing may render it as a count.
  // Only while the service's provider-only device provides (no session, so no
  // DeviceRemote: SdkHost::ReconcileProviderLocked): its status says its tier
  // and key, and its peers come only from get_provider_stats, which an older
  // service does not answer and which is read while the window presents
  // (SdkHost::ProviderOnlyStatsLoop).
  bool provideClientsUnknown = false;
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
  // what decided the action (sdk BlockAction.Reason, a BlockActionReason value;
  // "" for ordinary provider-routed traffic). See BlockActionReason.h.
  std::string reason;
};

inline bool operator==(const BlockActionItem& a, const BlockActionItem& b) {
  return a.id == b.id && a.timeMillis == b.timeMillis && a.hosts == b.hosts &&
         a.ips == b.ips && a.matchedHosts == b.matchedHosts &&
         a.matchedIps == b.matchedIps && a.block == b.block && a.local == b.local &&
         a.overrideId == b.overrideId && a.hasBlockOverride == b.hasBlockOverride &&
         a.hasRouteOverride == b.hasRouteOverride && a.packetCount == b.packetCount &&
         a.byteCount == b.byteCount && a.reason == b.reason;
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

// ---- transport distribution + transport settings (TRANSPORTSTATS) ----------
// One transport's slice of the stats window's REMOTE traffic, ready to render as
// a segment of the transport bar plus its legend entry. A mirror of the SDK
// ContractViewController's TransportShare: every render value -- the byte share,
// the cumulative boundary (the segment's right edge as a fraction of the bar,
// so drawing every segment from its neighbours' boundaries tiles exactly 100%
// at every tween frame), the whole percent (the used ones sum to exactly 100; a
// used sliver can be 0 -> "<1%"), used, enabled -- is computed by the SDK view
// controller so the math is shared and tested once for every platform. The app
// only maps names/colors and draws.
struct TransportShareRow {
  std::string transportType;  // sdk id: h3 | h1 | dns | dnspump | p2p | unknown
  bool h1PlusActive = false;  // live negotiated carrier; H1 retains its sdk id
  int64_t egressByteCount = 0;
  int64_t ingressByteCount = 0;
  double share = 0;     // fraction of the window's remote bytes, 0..1
  double boundary = 0;  // cumulative share through this transport, 0..1
  int64_t percent = 0;  // whole percent for the legend
  bool used = false;    // carried traffic in the window: segment + legend entry
  bool enabled = false; // enabled by the transport settings: unused footer entry when idle
};

inline bool operator==(const TransportShareRow& a, const TransportShareRow& b) {
  return a.transportType == b.transportType && a.egressByteCount == b.egressByteCount &&
         a.ingressByteCount == b.ingressByteCount && a.share == b.share &&
         a.boundary == b.boundary && a.percent == b.percent && a.used == b.used &&
         a.enabled == b.enabled && a.h1PlusActive == b.h1PlusActive;
}
inline bool operator!=(const TransportShareRow& a, const TransportShareRow& b) {
  return !(a == b);
}

// The window's remote traffic partitioned by the transport that carried it, in
// the SDK's stable order (h3, h1, dns, dnspump, p2p, unknown) with every
// transport present. Follows the same window as the throughput points, so it
// drains to inactive as traffic ages out. Empty shares = no feed.
struct TransportDistributionSnapshot {
  std::vector<TransportShareRow> shares;
  int64_t byteCount = 0;
  bool active = false;  // any transport carried traffic in the window
};

inline bool operator==(const TransportDistributionSnapshot& a,
                       const TransportDistributionSnapshot& b) {
  return a.shares == b.shares && a.byteCount == b.byteCount && a.active == b.active;
}
inline bool operator!=(const TransportDistributionSnapshot& a,
                       const TransportDistributionSnapshot& b) {
  return !(a == b);
}

// The Earnings page's statistics feed (EXTENDER.md O5, O8), read on the SAME
// throughput tick as the client points: the provider series (the Local and
// Blocked charts), the extender series (the extender chart, its Remote route),
// whether the device reports provider packet stats (the provider section's
// gate, with the provide mode) and the provider transport distribution (its
// bar). Whether the extender role runs is deliberately NOT here: it is the
// pushed status's `enabled` (ExtenderProvideStatusView), which changes with the
// role rather than on a tick the listener may never send.
struct ProviderThroughputSnapshot {
  std::vector<urnet::ThroughputPoint> providerPoints;
  std::vector<urnet::ThroughputPoint> extenderPoints;
  int64_t windowSeconds = 60;
  // Whether the device reports provider packet stats: the provider section's
  // gate with the provide mode (O8). Engaged when this publish carries a
  // reading: the controller's on a throughput tick, the device's own when a
  // presentation opens (a new controller reports none until it samples), and
  // false when the session ends. Nullopt when the window only hid, so the page
  // keeps the reading it has.
  std::optional<bool> hasProviderStats;
  // Engaged only when the distribution changed since the last publish, the
  // client bar's rule: an idle tick must not rebuild the legend.
  // CurrentProviderThroughput always engages it, for the seed.
  std::optional<TransportDistributionSnapshot> providerDistribution;
};

// The Earnings provider status of the service's provider-only device (no
// session): the readings a ProviderStatusViewController would publish, kept
// from GET /network/provider-status answers read on the api
// (SdkHost::SetProviderOnlyStatusWanted).
using ProviderOnlyStatus = providerstatus::Readings<urnet::ProviderStatus>;

// Which device transport policy a surface reads/edits: the CLIENT policy (the
// carrier this device uses to reach providers) or the PROVIDER policy (the
// carrier it uses when relaying for remote clients). Both are SDK
// TransportSettings; the SDK owns every rule (default priorities, the fixed
// preference order, the last-enabled-mode refusal, normalization).
enum class TransportSettingsKind { Client, Provider };

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
  // (e.g. the client was removed, or another device signed this session out):
  // the sdk has already cleared its local auth state. `cause` is the sdk's
  // reason, read in its listener (AuthLogoutCause.h): "session_revoked"
  // (urnet::AuthLogoutCauseSessionRevoked) or "". The Api's listener and,
  // with a session up, the device's report one rejection each. Runs on an sdk
  // callback thread and must only marshal -- the ui marshals onto its thread
  // and calls Logout() once for them (AuthLogoutNotice.h).
  using AuthInvalidHandler = std::function<void(std::string cause)>;
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
  // The transport bar's feed: the client (remote) distribution, published from
  // the SAME throughput tick as the points and only when it changed.
  using TransportDistributionHandler = std::function<void(TransportDistributionSnapshot)>;
  // The extender panel's feed (EXTENDER.md K4, K5). The SDK coalesces its own
  // change stream to one callback per second; this publishes only when the
  // mapped view actually changed, so an idle network costs the UI thread
  // nothing. A default-constructed view means "no session / nothing known",
  // which the panel draws as a red dot and 0 of 0.
  using ExtenderStatusHandler = std::function<void(ExtenderStatusView)>;
  // The provider extender rows' feed (EXTENDER.md N2, N7): the device's status
  // mapped to the plain view both rows draw, with the setting read beside it,
  // published only when the view changed. A default-constructed view is "no
  // session / unsupported", which hides both rows.
  using ExtenderProvideStatusHandler = std::function<void(ExtenderProvideStatusView)>;
  // The Earnings page's statistics feed (O5, O8), every throughput tick.
  using ProviderThroughputHandler = std::function<void(ProviderThroughputSnapshot)>;
  // The provider-only device's provider status (no session), every change of
  // the readings: a poll answered or failed, or the source gone.
  using ProviderOnlyStatusHandler = std::function<void(ProviderOnlyStatus)>;
  // The client / provider transport policy in force (device change listeners +
  // the initial read); nullopt = no device / no policy known.
  using TransportSettingsHandler =
      std::function<void(TransportSettingsKind, std::optional<urnet::TransportSettings>)>;
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

  // Account discovery: authLogin{user_auth} routes an email/phone to the
  // password, create or verify step (macOS LoginInitialViewModel parity).
  void StartLogin(const std::string& userAuth, std::function<void(LoginRouting)> done);

  // Sign-up: networkCreate with user_auth + password, or with the retained
  // wallet auth (params.useWalletAuth). verification_required in the result
  // routes the UI to the verify step; a jwt registers this device.
  void CreateNetwork(const CreateNetworkParams& params, std::function<void(AuthResult)> done);

  // Verify-code entry + resend (authVerify / authVerifySend). A successful
  // verify yields the network jwt and registers this device.
  void VerifyCode(const std::string& userAuth, const std::string& code,
                  std::function<void(AuthResult)> done);
  // The resend asks for result_errors, so a code the server did not send
  // arrives as AuthVerifySendResult.error rather than as a 200 that reads sent.
  void ResendVerifyCode(const std::string& userAuth,
                        std::function<void(VerifySendNotice)> done);

  // Password reset: emails a reset link to the user auth. Asks for
  // result_errors, so a link the server did not send arrives as
  // AuthPasswordResetResult.error rather than as a 200 that reads sent.
  void SendPasswordResetLink(const std::string& userAuth,
                             std::function<void(VerifySendNotice)> done);

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

  // ---- Google / Apple SSO (the provider's web flow, the api's callback) ------
  // Neither identity provider has a native desktop flow here, so both run in
  // the default browser against the provider itself: Google's authorize page
  // (code flow) or Apple's, with <api>/auth/<provider>/callback as the
  // redirect. The api hands the identity token back on
  // urnetwork://oauth/<provider>, which HandleDeepLink routes here (the same
  // client ids the ur.io login dialog uses, so the server accepts the token
  // exactly as it does for the web). The attempt's `state` must be echoed and
  // the token must carry the attempt's `nonce` claim before the token goes to
  // authLogin{auth_jwt_type:<provider>}; an identity with no network yet routes
  // to the create-network step the same way a wallet does. `provider` is
  // "google" or "apple".
  void SignInWithSso(const std::string& provider, std::function<void(AuthResult)> done);
  // An SSO identity authenticated but has no network: the id token is retained
  // for CreateNetwork (name + terms, no password), like a wallet.
  bool HasPendingAuthJwt();

  // ---- network server (iOS NetworkServerSheet parity) ----------------------
  // Which network API this client talks to. On a fork this is the difference
  // between the official bringyour.com and a self-hosted deployment.
  struct NetworkServer {
    std::string hostName;
    std::string apiUrl;      // live, derived or overridden
    std::string connectUrl;  // live platform (connect) url
    // the EXPLICIT overrides in force, or empty when the urls are derived
    std::string configuredApiUrl;
    std::string configuredConnectUrl;
    // What "the default network" means for THIS process: normally the
    // compiled-in ids::kNetworkSpaceHostName, but URNETWORK_NETWORK_HOST
    // when that is set. The sheet's "Use default network" hardcoded the
    // compiled-in host, so pressing it in a test-network session silently moved
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

  // Sign in with a Bittensor wallet (`walletId`: "talisman" or "taocom",
  // BittensorWalletFlow.h). The SDK session helper
  // (urnet::BittensorWalletSession) runs the proof: Talisman through the ur.io
  // bridge in the browser, TAO.com through the manual form (the manual
  // handler below). The proof goes to authLogin{wallet_auth{blockchain=TAO}};
  // a wallet with no network yet keeps its wallet for the create step.
  void SignInWithBittensor(const std::string& walletId, std::function<void(AuthResult)> done);

  // The manual form of a Bittensor proof (TAO.com documents no programmatic
  // interface): the UI shows `message` for the user to sign in the wallet,
  // takes the address (prefilled with `address` when the flow is bound to
  // one) and the signature, and answers with SubmitBittensorManual, or
  // CancelBittensorProof when the user closes it. The handler runs on an SDK
  // thread; the UI marshals.
  struct BittensorManualRequest {
    std::string walletId;
    std::string walletName;
    std::string message;
    std::string address;
    std::string purpose;
  };
  void SetBittensorManualHandler(std::function<void(BittensorManualRequest)> handler);
  // accepted=false keeps the form open when the error is correctable (a typo);
  // `error` is localized. A non-correctable refusal ends the proof (its flow
  // is answered) and closes the form (`closed`).
  struct BittensorManualAnswer {
    bool accepted = false;
    bool closed = false;
    std::string error;
  };
  BittensorManualAnswer SubmitBittensorManual(const std::string& address,
                                              const std::string& signature);
  void CancelBittensorProof();

  // Sign `message` with a Solana wallet through the same browser bridge and hand
  // the address and signature back without authenticating. Adding a Solana
  // sign-in method is the caller (SignSolanaForAdd): it hands the signed pair to
  // addAuth rather than logging in with it.
  //
  // Only one wallet flow can be in flight, because the bridge exposes a single
  // pair of callbacks: starting a sign-in supersedes a pending signature request
  // and vice versa. `done` runs on whichever thread delivered the deep link, so
  // a UI caller marshals.
  void SignWithSolanaWallet(WalletConnect::Provider provider, const std::string& message,
                            std::function<void(bool ok, std::string address,
                                               std::string signature, std::string error)> done);

  // ---- adding a sign-in method (Settings' add sheet, AddSignIn.h) -----------
  // Each produces a credential for addAuth on the CURRENT network and nothing
  // else: no SetAuthState, no authLogin, no retained pending auth, no jwt. They
  // share the browser bridge's single flow with the sign-ins (starting one
  // supersedes whatever is in flight, which is told), and `done` runs on an SDK
  // or deep-link thread, so a UI caller marshals. An error starting with
  // "superseded by " is a quiet cancel.
  //
  // The provider's identity token from its web flow, like SignInWithSso, but
  // the attempt is owned by the add sheet (add_sign_in::SsoPurpose::Add): its
  // urnetwork://oauth/<provider> return is answered here and never reaches
  // authLogin. `provider` is "google" or "apple".
  void SsoTokenForAdd(const std::string& provider,
                      std::function<void(std::string idToken, std::string error)> done);
  // A fresh SOL /auth/wallet-challenge signed by the Solana wallet through the
  // bridge, as a bare signature request (SignWithSolanaWallet).
  void SignSolanaForAdd(WalletConnect::Provider provider,
                        std::function<void(std::string address, std::string signature,
                                           std::string message, std::string error)> done);
  // A Bittensor proof under the session helper's add purpose
  // (bittensor::kPurposeAdd): Talisman and WalletConnect through the bridge,
  // TAO.com through `manualHandler` (the add sheet's own form, since a second
  // ContentDialog cannot open over it), answered with SubmitBittensorManual /
  // CancelBittensorProof as usual.
  void SignBittensorForAdd(const std::string& walletId,
                           std::function<void(BittensorManualRequest)> manualHandler,
                           std::function<void(std::string address, std::string signature,
                                              std::string message, std::string error)> done);
  // The add sheet closed or switched method: answer and drop its flow.
  void CancelAddSignIn();

  // Connect a Solana wallet through the same browser bridge and hand back its
  // public key WITHOUT signing anything and WITHOUT authenticating. The address
  // becomes an account wallet (createAccountWallet: the Earnings Solana payout
  // wallet), which takes no signature - android (MWA connect) and apple
  // (connectPhantomWallet) connected the same way. Same single-flow rule and
  // threading caveat as above: it supersedes any wallet flow in flight and is
  // superseded by the next one, and `done` runs on whichever thread delivered
  // the deep link.
  void ConnectSolanaWallet(WalletConnect::Provider provider,
                           std::function<void(bool ok, std::string address, std::string error)> done);

  // Sign a server-issued TAO challenge with a Bittensor wallet WITHOUT
  // authenticating: the signed triple attaches the coldkey to the provider
  // (Api.snSetWallet / Device.connectSnWallet, Earnings). The challenge is
  // fetched for `walletAddress` when the user pasted one (the session then
  // refuses any other signing account), or for whichever account the wallet
  // picks when it is empty. `purpose` is the session's ("connect"). `done`
  // gets the address, the hex sr25519 signature and the exact message that
  // was signed, or a localized error (a superseded flow's starts with
  // bridge::kSupersededPrefix); same threading caveat as above.
  void SignWithBittensorWallet(const std::string& walletId, const std::string& walletAddress,
                               const std::string& purpose,
                               std::function<void(bool ok, std::string address,
                                                  std::string signature, std::string message,
                                                  std::string error)> done);

  // Route a urnetwork:// deep link (wallet callback; later OAuth) into the host.
  // Called from the app's protocol-activation handler.
  void HandleDeepLink(const std::string& url);

  // Sign out of URnetwork (owner decisions 2026-10-05: the tunnel and the
  // provider stop as on Quit, the app keeps running, signed out; and each
  // network starts fresh). In order:
  //   0. the browser and wallet flows the account started are answered and
  //      forgotten (CancelPendingWalletFlows), so a late add-sign-in return
  //      cannot add a method to the next account signed in;
  //   1. the session-request slot is emptied: a connect, a settling row click
  //      or a reconcile queued for the signed-out account never runs;
  //   2. the app is signed out (loggedIn_) before the lock, so a pass that
  //      takes mutex_ first starts nothing for the account: BootstrapSession
  //      and ReconcileProviderLocked read it before the stored jwt;
  //   3. under mutex_, so after any pass in flight: the local credentials are
  //      logged out and the api's credential cleared (the next sign-in's calls
  //      would otherwise carry it), then the sign-out is recorded as owed
  //      (Common/SignOut.h) and delivered: the control channel is dialled when
  //      it is down, then Quit's stop_tunnel and stop_provider, in Quit's
  //      order, then the service's logout, which severs the device identity
  //      and clears what the service's sdk stored for the account;
  //   4. the DeviceRemote, its feeds and the saved rpc session go.
  // A delivery that does not complete (no service, a refused request) leaves
  // the sign-out owed in a marker that outlives the app: the service watchdog
  // retries it, every later pass delivers it first, and nothing is started
  // until it has been delivered. The sign-out completes in the app either way.
  // The kill switch is treated as Quit treats it: stop_tunnel lifts any
  // firewall policy, the armed floor included. UI thread; it blocks on the
  // lock and the pipe calls, as the stop_tunnel it replaces did.
  void Logout();

  // The tray's Quit, the service half (owner decision 2026-10-05,
  // AppLifetime.h): the service ends with no session and no provider-only
  // device, and nothing in this process starts either again. In order:
  //   1. the session-request slot is emptied and closed for good, so a pass
  //      already queued, the failsafe edge, a mode change or the watchdog's
  //      recovery cannot reach start_tunnel or start_provider afterwards;
  //   2. this object's own threads are stopped and joined: the service
  //      watchdog, the presentation worker, the rpc-sync watchdog and the
  //      provider-only statistics loop with its provider status poll;
  //   3. under mutex_, so after any pass in flight: stop_tunnel, which ends
  //      the session (tunnel or rpc-only) and lifts any firewall policy, the
  //      kill switch's armed floor included, as Disconnect does; then
  //      stop_provider; then the DeviceRemote and the saved rpc session go.
  // The control channel is dialled first when it is down, because a service
  // that is running still runs what it ran. The stored kill-switch setting, the
  // provide mode and the auth are untouched: quitting is not signing out, and
  // the next launch reconciles from them.
  //
  // Blocking (joins, the lock, two pipe calls), and it is the last thing the
  // app does: AppController::Shutdown calls it from the UI thread after the
  // window and the tray are gone. Not undone: the process is ending.
  void Quit();

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
  //
  // OUT OF BALANCE, NONE OF THEM STARTS. Every connect entry point (these, the
  // urnet::ConnectLocation overload and the two row variants below) first asks
  // the start-connect gate (SetStartConnectGate, BalanceGate.h): blocked, it
  // records nothing and shows the upgrade path instead. EnsureSession is a
  // reattach, not a connect, and Disconnect is always admitted, so a session
  // that runs out of balance is never dropped by this.
  void ConnectBestAvailable();
  void Connect(const std::string& connectLocationJson);
  void Disconnect();
  // The start-connect gate. facts gives urnw::balance::StartConnectFacts for
  // this instant; upgrade shows the upgrade path in place of a blocked
  // connect, and is handed the refused gesture (the same gesture, asked anew)
  // so the balance recovery can run it once data is back; fetchBalance fetches
  // the subscription balance and calls its argument once the fetch settles
  // (succeeded, failed or timed out). All are called on the caller's thread,
  // which for every connect entry point is the UI thread. Unset, every connect
  // is admitted.
  void SetStartConnectGate(std::function<urnw::balance::StartConnectFacts()> facts,
                           std::function<void(std::function<void()>)> upgrade,
                           std::function<void(std::function<void()>)> fetchBalance) {
    startConnectFacts_ = std::move(facts);
    startConnectUpgrade_ = std::move(upgrade);
    startConnectFetchBalance_ = std::move(fetchBalance);
  }
  // The user's connect gestures, for the balance recovery (BalanceGate.h,
  // BalanceRecovery): admitted runs when the gate lets a connect gesture
  // start (it replaces a connect still waiting on the balance), disconnected
  // when the user disconnects (nothing is reconnected by itself after it).
  // Called on the caller's thread, the UI thread.
  void SetConnectGestureObserver(std::function<void()> admitted,
                                 std::function<void()> disconnected) {
    connectAdmitted_ = std::move(admitted);
    onUserDisconnect_ = std::move(disconnected);
  }
  // Whether a connect gesture may start now; when not, shows the upgrade path,
  // or, on a stale balance, fetches it and runs `again` (the same gesture,
  // asked anew) once the fetch settles. `what` names the entry point in the log.
  bool AdmitStartConnect(const char* what, std::function<void()> again);
  // Runs a connect gesture the gate refused earlier (the one upgrade was
  // handed) past the gate, and not as a new gesture: the balance recovery
  // decided on a fresh balance that data is back. Also how it rebuilds a
  // connection held out of balance, which the latched gate would refuse. UI
  // thread.
  void RetryRefusedConnect(const std::function<void()>& connect);

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
  // lands on a signed-in space, the service-reconnect watchdog, and an
  // unexpected drop (the service's dead-tunnel failsafe), whose pass drops the
  // stale DeviceRemote and hands providing to the provider-only device.
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
  void SetExtenderStatusHandler(ExtenderStatusHandler h) {
    onExtenderStatus_ = std::move(h);
  }
  void SetExtenderProvideStatusHandler(ExtenderProvideStatusHandler h) {
    onExtenderProvideStatus_ = std::move(h);
  }
  void SetProviderThroughputHandler(ProviderThroughputHandler h) {
    onProviderThroughput_ = std::move(h);
  }
  void SetProviderOnlyStatusHandler(ProviderOnlyStatusHandler h) {
    onProviderOnlyStatus_ = std::move(h);
  }
  void SetTransportDistributionHandler(TransportDistributionHandler h) {
    onTransportDistribution_ = std::move(h);
  }
  void SetTransportSettingsHandler(TransportSettingsHandler h) {
    onTransportSettings_ = std::move(h);
  }
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
  // The last published transport distribution (TRANSPORTSTATS). A cache read
  // like CurrentSplitRules, no rpc: the value is refreshed by PublishThroughput
  // on every throughput tick.
  TransportDistributionSnapshot CurrentTransportDistribution();
  // The last published extender status (K4). A cache read like the
  // distribution above: the value is refreshed by the SDK's once-a-second
  // change listener, never by polling.
  ExtenderStatusView CurrentExtenderStatus();
  // The last published provider extender status (N7), the same kind of cache
  // read: refreshed by the device's change listener, never by polling. With no
  // session it is the provider-only device's, as the last get_provider_stats
  // answer said (providerOnly).
  ExtenderProvideStatusView CurrentExtenderProvideStatus();
  // The last published statistics feed (O8), with the distribution and the
  // provider-stats reading engaged: the seed when the Earnings page is built and
  // when it shows. A cache read under the drawer lock, no rpc.
  ProviderThroughputSnapshot CurrentProviderThroughput();
  // ---- the provider-only device's provider status (P008, no session) --------
  //
  // With no session the Earnings provider status cannot come from a
  // ProviderStatusViewController: the sdk opens one only on a device, and the
  // provider is the service's provider-only device. So this reads the same
  // GET /network/provider-status on the api, at the controller's cadence, and
  // keeps the row whose client id the service reports for its device
  // (get_provider_stats): providerstatus::Readings.
  //
  // Wanted while the Earnings destination shows with providing enabled, the
  // window presents and there is no session — the controller's started state.
  // A want polls at once and then about once a minute; an unwant drops the poll
  // in flight and keeps the snapshot, as the controller's stop() does. The
  // readings reset when the provider-only device stops being the provider (a
  // session, or no device), and say the status is unavailable while the
  // service reports no statistics (an older service, or no device running).
  // Both calls take only a light lock: safe on the UI thread.
  void SetProviderOnlyStatusWanted(bool wanted);
  ProviderOnlyStatus CurrentProviderOnlyStatus();
  // The SDK's ExtenderViewController for this session (K6, K7): the settings
  // form, the share payload and the import all go through it, so every app
  // applies one implementation of those rules. Null with no session -- the
  // controller is opened from the device, and the account section shows the
  // NoDevice state rather than an editable form backed by nothing.
  // A SHARED reference, not a raw pointer into the host's own storage.
  //
  // This is the one view controller a caller uses from a BACKGROUND thread: the
  // account section's settings read and save, and the share/import sheets, all
  // hop off the UI thread for what are DeviceRemote rpcs. Handing out a raw
  // pointer meant the teardown could run `urnet_release` on the handle between
  // the caller's null check and its call. Holding the shared_ptr for the
  // duration of the call keeps the C handle registered; ClosePresentationLocked
  // still close()s the controller, which is what stops the Go side, and the
  // last reference releases the handle when the call in flight returns.
  //
  // Null with no session -- the controller is opened from the device, and the
  // account section shows the NoDevice state rather than an editable form
  // backed by nothing.
  std::shared_ptr<urnet::ExtenderViewController> ExtenderController();
  // The LEGACY single private extender (K6: "stays as an advanced field with
  // its exclusive override"). It is a network-space VALUE, not one of the three
  // the view controller edits, so it is read and written here.
  //
  // The write is safe against a live session: the space manager applies a
  // change confined to the EXTENDER values -- and NetExtender is one of them --
  // in place, restarting the space's network client and node rather than
  // rebuilding the space, so the DeviceRemote bound to it and everything
  // derived from it stay valid. An empty ip clears the override.
  std::optional<urnet::NetExtender> CurrentNetExtender();
  bool SetNetExtender(const std::optional<urnet::NetExtender>& value);
  // "Reset extenders" (Account > Extenders; connect EXTENDER.md E7): this
  // installation's extender state back to a fresh install's. The app's own
  // space resets first (NetworkSpace::resetExtenders): everything it learned
  // about extenders is cleared, the extenders a user added go -- the manual
  // hosts, the private extender above, the dns name, gossip url and root keys
  // back to their defaults -- and the values are persisted with the reset's
  // id, which the service imports with the space at its next start. Then the
  // service is handed the id (reset_extenders), so the space its session's
  // device and its provider-only device run in resets at once. Like the
  // private extender it needs no session. A live extender path keeps running;
  // every new extender dial draws from the fresh directory.
  //
  // Synchronous: the space's reset joins its extender network client and the
  // pipe call can wait behind a start_tunnel, so callers run it off the UI
  // thread. False when the app's own space could not be reset (no space, or
  // the call threw). The service's answer is logged rather than returned: a
  // service busy with a tunnel operation is sent the reset once more when that
  // operation ends (Common/ExtenderReset.h, QueueExtenderResetResend), and the
  // next import of the space carries the reset to a service that did not take
  // it.
  bool ResetExtenders();
  // ---- VLESS (Settings > VLESS and the login screen's network sheet) -------
  // One VLESS server in the ACTIVE network space's values, which the space's
  // client strategy dials through while it is on and valid (sdk
  // vless_settings.go). Like the private extender above it needs no session:
  // the login screen edits it before sign-in. A save applies in place -- the
  // space, a DeviceRemote bound to it and everything derived from it stay
  // valid -- and the service imports the space at its next tunnel start
  // (StartTunnel.network_space_json), which is why the sheet says so.
  //
  // The space calls take mutex_, which a session bootstrap holds for its whole
  // length, and every call crosses the C ABI, so callers run them off the UI
  // thread (VlessSheet).
  //
  // The space's settings, or the sdk's new-form defaults when it has none;
  // nullopt with no space or when the read failed.
  std::optional<urnet::VlessSettings> CurrentVlessSettings();
  // Saves them to the space: "" when saved, a vless_error_* id when the sdk
  // refused enabled settings that do not validate (nothing is saved then),
  // nullopt when the call could not run (no space, or it threw).
  std::optional<std::string> SetVlessSettings(const urnet::VlessSettings& settings);
  // A vless:// share link read into settings (enabled), or its error id;
  // nullopt when the call failed.
  std::optional<urnet::VlessLinkResult> ParseVlessLink(const std::string& link);
  // The share link of the settings; "" when they do not validate or the call
  // failed (ValidateVlessSettings tells the two apart).
  std::string VlessSettingsLink(const urnet::VlessSettings& settings);
  // "" when the settings can be dialed (enabled or not), else the error id of
  // the first problem; nullopt when the call failed.
  std::optional<std::string> ValidateVlessSettings(const urnet::VlessSettings& settings);
  // ---- bootstrap DNS-over-HTTPS servers (Account > Extenders and the login
  //      screen's network sheet; sdk control_doh_ui.go) -----------------------
  // `https://<ip literal>/<path>` servers in the active network space's values,
  // tried ahead of the built-in DoH servers for the lookups of the space's own
  // names, for networks that block the built-in ones. Like VLESS above they
  // need no session -- a fresh install behind such a network cannot sign in
  // without them -- a save applies in place, and the service takes them with
  // the space at its next tunnel start. The same lock and C ABI rule: callers
  // run these off the UI thread (ControlDohBlock).
  //
  // The space's servers, v4 then v6, normalized; empty is the built-in servers
  // alone. nullopt with no space or when the read failed.
  std::optional<std::vector<std::string>> CurrentControlDohUrls();
  // Saves them to the space: "" when saved (an empty list clears them), a
  // control_doh_error_* id when a line does not validate or there are too many
  // (nothing is saved then), nullopt when the call could not run.
  std::optional<std::string> SetControlDohUrls(const std::vector<std::string>& urls);
  // The sdk's preset for a country (ExtenderPresentation.h
  // kControlDohChinaCountryCode), v4 first; empty when there is none or the
  // call failed. Touches no host state.
  std::vector<std::string> RegionalControlDohUrls(const std::string& countryCode);
  // The client / provider transport policy: the device's when there is a
  // session (offline the DeviceRemote answers with the pending or last known
  // policy), else the app LocalState mirror (see ApplyTransportSettings), else
  // nullopt (never edited -> the SDK default stands; the editor falls back to
  // urnet::defaultTransportSettings()).
  std::optional<urnet::TransportSettings> CurrentTransportSettings(TransportSettingsKind kind);
  // Runtime Auto eligibility reported by the service process that owns the
  // memory budget. There is no app-side fallback: without a live/last-known
  // device status the editor simply omits constraint indicators.
  std::optional<urnet::TransportStatus> CurrentTransportStatus(TransportSettingsKind kind);
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
  // The provider extender setting (EXTENDER.md N4): written through the device,
  // which persists it in its own space and applies it at once; while the device
  // process is out of contact the DeviceRemote queues it and replays it at the
  // next sync. With no session the switch shows over the provider-only
  // device's status only when the service takes its write, and the write goes
  // to the service (set_provide_extender), which persists it in the same space
  // a session's device reads, from ProviderOnlyStatsLoop's thread
  // (ExtenderProvideWriteRouteFor). There is no app-side mirror to keep,
  // unlike the provide mode: the service owns the setting either way. Callers
  // never write while the row is hidden (N1).
  void SetProvideExtender(bool on);
  void ApplyDnsSettings(const urnet::DnsResolverSettings& settings);
  // Apply a transport policy (client or provider) to the device AND mirror it
  // into the app LocalState. The service's DeviceLocal persists the policy in
  // ITS local state and restores it at creation, so a policy set while the
  // tunnel runs survives service restarts with no app involvement -- but the
  // app (%LOCALAPPDATA%\URnetwork\app) and the service (ProgramData) do not
  // share storage, and a DeviceRemote only holds pending state in memory. So an
  // edit made with no session would be lost on relaunch and offline reads would
  // show the default. Hence the mirror, and BootstrapSession seeds the device
  // from it (apple DeviceManager.initDevice parity). The applied policy comes
  // back through the change listener (the service's truth when attached, the
  // queued value when not).
  void ApplyTransportSettings(TransportSettingsKind kind,
                              const urnet::TransportSettings& settings);
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
  // The app-wide client event queue (ClientEvents.h): every product event the
  // onboarding optimization loop reads goes through this one facade. Valid
  // after Initialize().
  bool eventsReady() const { return events_ != nullptr; }
  ClientEventQueue& events() { return *events_; }
  // The sign-up pages' "Periodic product updates" box, read at submit: the
  // next network create carries product_updates=false when it is off (absent
  // = opted in), and signup.optout_changed records the opt-out.
  void SetProductUpdatesOptOut(bool optOut);
  // urnetwork://onboarding/<step> deep links (the campaign emails' buttons):
  // routed to the window, which owns the destinations. Fired on the caller's
  // thread (AppController marshals to the UI thread).
  void SetOnboardingLinkHandler(std::function<void(const std::string& url)> handler) {
    onOnboardingLink_ = std::move(handler);
  }
  bool hasDevice() { return device_.has_value(); }
  urnet::DeviceRemote& device() { return *device_; }
  // "Send feedback with logs" whether or not a tunnel runs (support inbox
  // 2090), after the server accepted the feedback with the box ticked. The logs
  // that matter are the service's, and the DeviceRemote reaches them only while
  // a session runs, so the app asks the service first (Protocol.h upload_logs)
  // with this session's client credentials as start_provider carries them, and
  // falls back to the DeviceRemote's UploadLogs only when the service did not
  // take it (logupload::AppStepAfterService). Returns at once: the request runs
  // on FeedbackLogUpload's thread, one at a time, and the outcome of an upload
  // the service took comes in its status (FollowServiceLogUpload). Failure is
  // logged, never shown: the feedback itself was accepted. UI thread.
  void UploadFeedbackLogs(const std::string& feedbackId);
  // Account page opens billing/upgrade in the browser at this host.
  std::string linkHostName() const { return "ur.io"; }
  // The version this app reports to the SDK/server: this build's own
  // (urnw::version::kString). Settings shows it because urnet::version() is
  // empty in this SDK build, so it is the only build identifier the version row
  // can actually display.
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
  // The values the space manager holds for `key`, read out of the space's own
  // json (the getters return EFFECTIVE values); nullopt when the manager has no
  // such space or its json could not be read. BuildNetworkSpace and
  // ApplyNetworkServer write a space's values whole, so they write their own
  // values over these (NetworkSpaceStartup.h BundledSpaceValuesOver,
  // ServerSpaceValuesOver). Needs mutex_, like everything else that touches
  // spaceManager_.
  std::optional<urnet::NetworkSpaceValues> StoredSpaceValuesLocked(
      const urnet::NetworkSpaceKey& key);

  // ---- the session worker (one bootstrap at a time) -------------------------
  //
  // What a caller wants done once there is a session. `kind` is deliberately
  // separate from `location`: "best available" is also a nullopt location, so a
  // bare optional could not tell the two apart.
  //
  // Provider is not a session request at all: "keep the provider-only device in
  // step" (ReconcileProviderLocked), with no gesture, no bootstrap and no attach.
  // Every pass of the worker that leaves no session ends with that reconcile, so
  // a Provider request is covered by whatever else is pending.
  enum class ConnectKind { None, BestAvailable, Location, Disconnect, Provider };
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
      case ConnectKind::Provider:      break;  // never reaches the table
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

  // ---- keep providing while disconnected (Common/ProvideLifecycle.h) --------
  //
  // With no session the service's provider-only device is the provider. This
  // keeps it in step with the stored provide mode and provider transport policy
  // — started, adopted or re-moded (start_provider is idempotent) or stopped,
  // as provide::DisconnectedProviderStep says from one get_state — and leaves a
  // session alone: its own device provides. A signed-out app counts as mode
  // "never". Asks the service nothing for a mode that does not provide once its
  // status is known to run nothing. Caller holds mutex_. The session worker runs
  // it at the end of every pass that leaves no session (a Disconnect, a launch
  // or a service recovery that found nothing to reattach to, a failed Connect)
  // and for every RequestProviderReconcile.
  void ReconcileProviderLocked(const char* reason);
  // Queue that reconcile on the session worker, off the calling thread — the
  // UI thread for a mode, policy or kill-switch change: start_provider builds a
  // DeviceLocal in the service, and a click must not wait on it.
  void RequestProviderReconcile(const char* reason);
  // The provide fields of a snapshot with no DeviceRemote: the provider-only
  // device as the service's last status said, and its client count as the
  // last get_provider_stats answer said. Lock-free, for ReadStats and the
  // control-pipe reader.
  void FillProviderOnlyStats(LiveStats& stats) const;

  // ---- the provider-only device's statistics (no session) -------------------
  //
  // get_provider_stats, asked every kProviderOnlyStatsInterval by one thread of
  // its own, and only while it can matter: no session, the service reports a
  // provider-only device, the control channel is up and the window presents.
  // An answer feeds what a session's DeviceRemote feeds — the Earnings provider
  // plots, their gate and the "no traffic yet" line (the drawer caches and
  // onProviderThroughput_), the Earnings extender row and extender plot and
  // the Connect page's Extender switch (the extender points and
  // onExtenderProvideStatus_, as the provider-only device's status, with
  // whether the service takes the switch's write), the Connect page's client
  // count (serviceProviderClients_) — and names the client whose provider
  // status the api read above keeps. It also sends that switch's write
  // (set_provide_extender), first in its pass, so the answer after it already
  // carries it. Off the UI thread because the pipe serializes calls: a poll
  // can wait behind a start_tunnel for as long as that takes. Started by
  // Initialize, joined by the destructor.
  static constexpr std::chrono::seconds kProviderOnlyStatsInterval{2};
  // the sdk's ProviderStatusViewController poll interval
  static constexpr std::chrono::seconds kProviderOnlyStatusInterval{60};
  void ProviderOnlyStatsLoop();
  void StopProviderOnlyStats();  // called from the destructor; joins the thread
  // Re-evaluate now rather than at the next tick: a presentation, a want or a
  // provider that started or stopped.
  void KickProviderOnlyStats();
  // Put one answer on screen; take what this loop showed off it again,
  // forgetting the count and the gate too when the provider is gone (a hide
  // keeps them, ClearDrawer's rule, and the extender status with them; the
  // loop forgets that status wherever the provider stops being the source).
  // Caller holds mutex_, with no session.
  void ShowProviderOnlyStatsLocked(const proto::ProviderStats& stats);
  void ClearProviderOnlyStatsLocked(bool providerGone);
  // One GET /network/provider-status on the api, applied to the readings when
  // it answers unless they moved on meanwhile. Caller holds mutex_ (api_).
  void FetchProviderOnlyStatusLocked(const std::string& clientId);
  // The readings changed hands: forget them (the source is gone) or mark them
  // unavailable (the service reports no statistics), publishing a change.
  void ResetProviderOnlyStatus();
  void ProviderOnlyStatusUnavailable();
  // The Extender switch's write with no session: queued by SetProvideExtender
  // (the UI thread) and sent by ProviderOnlyStatsLoop, outside mutex_. Written
  // or refused, the next published status replaces the switch's guess.
  void QueueProviderOnlyExtenderWrite(bool on);
  void WriteProviderOnlyExtender(bool on);
  // A reset_extenders the service refused as busy (Common/ExtenderReset.h):
  // owed by ResetExtenders, made due by the first pushed status that ends the
  // operation which held the service's lock (the state handler, on the pipe's
  // reader thread, which must stay free to read the answer), and sent once
  // more by ProviderOnlyStatsLoop, outside mutex_. Its answer is only logged,
  // and an app that exits first drops it: the next import carries the reset.
  void QueueExtenderResetResend(proto::ResetExtenders request);
  void ResendExtenderReset(const proto::ResetExtenders& request);

  std::thread providerOnlyThread_;
  std::mutex providerOnlyMutex_;
  std::condition_variable providerOnlyCv_;
  bool providerOnlyStop_ = false;
  bool providerOnlyKick_ = false;
  bool providerOnlyStatusWanted_ = false;
  // The switch's last write not yet sent; a newer flip replaces it.
  std::optional<bool> providerOnlyExtenderWrite_;
  // The owed reset made due and not yet sent again.
  std::optional<proto::ResetExtenders> extenderResetResend_;
  // The reset the service last refused as busy, until a pushed status ends
  // the operation that refused it. Its own lock, innermost.
  extenderreset::Owed owedExtenderReset_;
  // Bumped whenever the readings stop belonging to the polls in flight (an
  // unwant, a reset), so a late answer is dropped, the controller's rule.
  uint64_t providerOnlyStatusGeneration_ = 0;
  ProviderOnlyStatus providerOnlyStatus_;

  std::mutex pendingMutex_;
  // Signalled on every RequestSession and on CancelPendingRowConnect, so a
  // worker sleeping out a row click's settle deadline re-reads the slot the
  // moment a replacement (possibly an IMMEDIATE one, e.g. Disconnect) lands.
  std::condition_variable pendingCv_;
  SessionRequest pending_;
  bool pendingRequested_ = false;
  bool sessionWorkerAlive_ = false;
  // Set once by Quit, under pendingMutex_, and never cleared: from then on
  // RequestSession records nothing and ReconcileProviderLocked asks the service
  // nothing. Atomic because the reconcile reads it under mutex_ alone.
  std::atomic<bool> quitting_{false};

  // ---- the sign-out the service is owed (Common/SignOut.h) -------------------
  //
  // Recorded by Logout and delivered by it when it can be; otherwise by the
  // head of every session pass (SettleSignOutLocked), which the service
  // watchdog keeps asking for while it is owed, signed in or not. Neither
  // BootstrapSession nor ReconcileProviderLocked starts or adopts anything
  // while it is owed.
  //
  // The service as a delivery sees it: the channel dialled when it is down, and
  // each request on service_, its status adopted. Caller holds mutex_.
  signout::Service SignOutServiceLocked();
  // Deliver an owed sign-out at the head of a pass, and keep the watchdog
  // retrying while it stays owed. `reason` names the pass in the log. Caller
  // holds mutex_.
  void SettleSignOutLocked(const char* reason);
  // The marker file (Paths.h SignOutOwedFile).
  static signout::Marker SignOutMarker();
  // Loaded by Initialize. Owed() is read without mutex_ by the watchdog.
  signout::Obligation signOut_{SignOutMarker()};

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
  // The Api Initialize created or ApplyNetworkServer replaced: the client info
  // its requests and connect auths carry, "windows" and this build's version
  // (server session/REVOKE-UI-FINAL.md §1.13, the last use the Sessions page
  // lists), and its confirmed rejection of the account's credential routed,
  // with the rejection's cause, to the sign-out the device's rejection takes
  // (onAuthInvalid_). Caller holds mutex_.
  void BindApiLocked();
  void SetupWalletCallbacks();
  void RequestWalletChallenge(
      const std::string& blockchain, const std::string& walletAddress,
      std::function<void(std::optional<std::string> message, std::string error)> done);
  // `bittensorWalletId` is the Bittensor wallet that signed `walletAuth` ("" for
  // none), for the words of a refusal.
  void SubmitCreateNetwork(const CreateNetworkParams& params,
                           std::optional<urnet::WalletAuthArgs> walletAuth,
                           std::function<void(AuthResult)> done,
                           const std::string& bittensorWalletId = std::string());
  // The wallet signed the challenge: authLogin{wallet_auth}. `signature` is what
  // the chain's verifier expects (base64 for SOL, hex for TAO).
  // `bittensorWalletId` is the Bittensor wallet that signed ("" for Solana).
  void AuthLoginWithWallet(const std::string& address, const std::string& signature,
                           const std::string& message, WalletConnect::Provider provider,
                           const std::string& bittensorWalletId = std::string());
  // The bridge returned an identity token: authLogin{auth_jwt_type:provider}.
  // An identity with no network yet is retained in pendingAuthJwt_ (with its
  // provider in pendingAuthJwtType_) and the UI routes to the create-network
  // step, exactly as the wallet path does.
  void AuthLoginWithSso(const std::string& provider, const std::string& idToken,
                        std::function<void(AuthResult)> done);
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
  void ClampCaptureStats(LiveStats& stats) const;
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
  // `sessionEnding`: a teardown's close forgets the provider extender status and
  // the provider-stats reading; a hide's close keeps both, so a re-shown window
  // draws what it drew (EXTENDER.md O8).
  void ClosePresentationLocked(bool sessionEnding);
  // `deviceHasProviderStats`: the device's answer when the caller asked it
  // (SubscribeDrawer); otherwise the controller's (a throughput tick).
  void PublishThroughput(std::optional<bool> deviceHasProviderStats = std::nullopt);
  // Whether the device reports provider packet stats, asked of the device: one
  // rpc, caller holds mutex_ (EXTENDER.md O8).
  bool DeviceHasProviderStatsLocked();
  void PublishContractRows();
  void PublishBlockActions();
  void PublishBlockStats();
  void PublishSplitRules();
  void PublishProviderLocations();
  void PublishProviderIdentities();
  // The extender status listener's payload, mapped and pushed when it changed
  // (K4, K5). Called on an SDK callback thread.
  void PublishExtenderStatus(std::optional<urnet::ExtenderStatus> status);
  // The provider extender status listener's payload (N2, N7), with the setting
  // read beside it, mapped and pushed when it changed. Called on an SDK callback
  // thread.
  void PublishExtenderProvideStatus(std::optional<urnet::ExtenderProvideStatus> status);
  // Push one mapped view when it changed: a session's device's
  // (PublishExtenderProvideStatus) or, with no session, the provider-only
  // device's as get_provider_stats reported it (ShowProviderOnlyStatsLocked).
  // One dedup baseline for both, so a session that takes over with the same
  // reading still publishes (the view's providerOnly differs).
  void PublishExtenderProvideView(ExtenderProvideStatusView view);
  // Take the provider-only device's extender status off the screens when that
  // device stops being the source (a session, or no provider), publishing
  // the unsupported view. A session's own status is never touched.
  void ForgetProviderOnlyExtenderStatus();
  // Read getLocalOverrideAppIds(), compute {paths, allowlist} (Android inversion:
  // any include-in-tunnel app => allowlist with the tunnel set, else denylist with
  // the bypass set), and push to the service -> driver. Called from the override
  // change listener and the initial drawer snapshot.
  void PushLocalOverrideAppsToDriver();
  // logout or hide: reset caches and push empty snapshots. `sessionEnding` also
  // forgets what a hide keeps (see ClosePresentationLocked).
  void ClearDrawer(bool sessionEnding);
  static std::string RandomLoopbackHostPort();
  std::string DeviceSpec();
  std::string DeviceDescription();

  std::mutex mutex_;
  std::optional<urnet::NetworkSpaceManager> spaceManager_;
  std::optional<urnet::NetworkSpace> networkSpace_;
  std::optional<urnet::Api> api_;
  std::unique_ptr<ClientEventQueue> events_;
  std::function<void(const std::string& url)> onOnboardingLink_;
  // SetStartConnectGate
  std::function<urnw::balance::StartConnectFacts()> startConnectFacts_;
  std::function<void(std::function<void()>)> startConnectUpgrade_;
  std::function<void(std::function<void()>)> startConnectFetchBalance_;
  // SetConnectGestureObserver (not userDisconnected_: that name is the session
  // worker's own fact, below)
  std::function<void()> connectAdmitted_;
  std::function<void()> onUserDisconnect_;
  // RetryRefusedConnect is running the refused gesture: the gate admits it
  bool retryingRefusedConnect_ = false;
  bool productUpdatesOptOut_ = false;  // the next create's product_updates
  // the sign-up pages' opt-out onto a create's args (absent = opted in)
  void ApplySignupPreferences(urnet::NetworkCreateArgs& args) const;
  // POST /network/auth-client with the device's time zone (IANA) and locale
  // (BCP 47) on the args: the campaign engine schedules its emails in the
  // user's local time. Sent as extra json fields until the C ABI's
  // AuthNetworkClientArgs carries them.
  void AuthNetworkClientWithLocale(const urnet::AuthNetworkClientArgs& args,
                                   urnet::AuthNetworkClientCallback callback);
  std::optional<urnet::AsyncLocalState> asyncLocalState_;
  std::optional<urnet::LocalState> localState_;
  std::optional<urnet::DeviceRemote> device_;
  // whether a network-visible provide key exists, pushed by
  // addProvideSecretKeysListener (DeviceRemote has no secret-keys getter --
  // the controller subscribes and caches the derived bit)
  std::atomic<bool> provideHasNetworkKey_{false};
  // The service's provider-only device as its last adopted status said
  // (AdoptServiceFacts): whether any status has been adopted at all, and the
  // device's running bit, live tier and network-key bit. Atomic for the reason
  // provideHasNetworkKey_ is: ReadStats reads them lock-free.
  std::atomic<bool> serviceProviderKnown_{false};
  std::atomic<bool> serviceProviderRunning_{false};
  std::atomic<int64_t> serviceProviderMode_{0};
  std::atomic<bool> serviceProviderNetworkKey_{false};
  // Its connected network peers as the last get_provider_stats answer said
  // (ProviderOnlyStatsLoop); -1 while unknown (no answer yet, an older service,
  // or a provider that stopped), which hides the count.
  std::atomic<int64_t> serviceProviderClients_{-1};
  std::optional<urnet::ConnectViewController> connectVc_;
  std::optional<urnet::ContractViewController> contractVc_;  // live throughput feed
  // K6/K7's one implementation of the settings, share and import rules.
  // shared_ptr and guarded by drawerMutex_ rather than mutex_: see
  // ExtenderController(). drawerMutex_ is the light lock the UI thread already
  // takes for cache reads, and the ordering mutex_ -> drawerMutex_ is the one
  // every Publish* already uses, so this adds no new lock order.
  std::shared_ptr<urnet::ExtenderViewController> extenderVc_;
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
  // the dedup baseline for the transport bar feed (PublishThroughput)
  TransportDistributionSnapshot lastTransportDistribution_;
  ExtenderStatusView lastExtenderStatus_;
  ExtenderProvideStatusView lastExtenderProvideStatus_;
  // Set by SetProvideExtender, and for the provider-only device's status by
  // WriteProviderOnlyExtender once the service answered: the next status
  // publishes even when it equals the last one. The row painted a local guess
  // after the write (N7), and only a pushed status replaces it.
  bool extenderProvideRepublish_ = false;
  // the statistics feed's caches (O8), refreshed by PublishThroughput
  std::vector<urnet::ThroughputPoint> lastProviderPoints_;
  std::vector<urnet::ThroughputPoint> lastExtenderPoints_;
  bool lastHasProviderStats_ = false;
  TransportDistributionSnapshot lastProviderDistribution_;
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
  // Name resolution uses connect's Windows in-process resolver while this bind
  // is active. Its query socket is therefore also owned by URnetwork.exe and
  // pinned to this interface; the service's Connected policy repeats the exact
  // app-id exemption at UDP/TCP port 53 in the DNS sublayer for that reason.
  //
  // Idempotent and change-gated; safe from the pipe reader thread.
  void ApplySdkEgressBind(int64_t index4, int64_t index6, const char* why);

  // ---- the network country (Common/NetworkCountry.h; open bug P052) ---------
  //
  // The country of the mobile broadband network carrying the default route, ""
  // for none: what the sdk's extender dials fall back to while the extender
  // hint cannot be fetched. Like the egress binding above it is process-global
  // inside one copy of the sdk (urnet::setNetworkCountryCode), and two
  // processes dial: this one (sign-in, account, the api) and the service, whose
  // devices are the tunnel session's and the provider-only one. So this
  // process reads it, applies it to its own sdk before the network spaces are
  // built, and hands it to the service — in start_tunnel and start_provider,
  // which apply it before their device is built, and in set_network_country
  // whenever it changes and whenever this process greets a service
  // (BootstrapSession), whose running devices may hold an older one.

  // Start the watch (NetworkCountryWatch.h), subscribe it to the OS's route and
  // interface changes, and wait briefly for its first report. Once, from
  // Initialize, before the space manager exists.
  void StartNetworkCountryWatch();
  // The watch's report, on its thread: this process's sdk first, then the
  // service. Never takes mutex_: Initialize holds it while it waits for the
  // first report.
  void ApplyNetworkCountry(const netcountry::Reading& reading);
  // set_network_country with the current reading, read under the push lock so
  // the last push the service hears carries the last reading. A service too old
  // for the verb answers "unknown request type" and keeps no country, as before
  // it existed. Safe from any thread; takes no mutex_.
  void PushNetworkCountry(const char* why);
  // After a start_tunnel or start_provider that carried `sent`: the push above,
  // when the reading is no longer `sent`. A start carries the reading it was
  // built with, outside the push lock, so a change the watch pushed meanwhile
  // can reach the service first and the start then puts the older country
  // back; this puts the newer one back in turn. Takes no mutex_.
  void PushNetworkCountryIfMoved(const netcountry::Reading& sent, const char* why);
  // The reading last applied to this process's sdk; empty before the first.
  netcountry::Reading CurrentNetworkCountry() const;

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
  std::atomic<proto::TunnelState> lastServiceState_{proto::TunnelState::Stopped};
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
  // The network country last applied to this process's sdk, under its own
  // lock: written by the watch's thread, read by the bootstrap and the provider
  // reconcile for their requests.
  mutable std::mutex networkCountryMutex_;
  netcountry::Reading networkCountry_;
  // Serializes the pushes (PushNetworkCountry). mutex_ may be held when it is
  // taken, never the other way round.
  std::mutex networkCountryPushMutex_;
  // The watch's notifications are torn down before the watch (~SdkHost), so
  // nothing records into it once it is ending.
  std::unique_ptr<NetworkCountryWatch> networkCountryWatch_;
  std::unique_ptr<DefaultRouteChanges> networkCountryChanges_;

  // ---- send feedback with logs ----------------------------------------------
  // FeedbackLogUpload's host steps: ask the service (the pipe call, mutex_
  // held only to read the session), and read the DeviceRemote's handle for
  // the old path (under mutex_), returning its call, which touches nothing of
  // this object.
  logupload::ServiceAnswer AskServiceToUploadLogs(const std::string& feedbackId);
  std::function<void()> PrepareDeviceRemoteLogUpload(const std::string& feedbackId);
  // The outcome of the upload the service took, once its status names it
  // finished (logupload::CompletionFor): logged, and the wait ends. From
  // AdoptServiceFacts, on whatever thread brought the status.
  void FollowServiceLogUpload(const proto::TunnelStatus& st);
  // The service's id of the upload this process waits on, 0 for none.
  std::atomic<int64_t> pendingLogUploadId_{0};
  // The request's thread. Stopped first in ~SdkHost: its steps take mutex_
  // and the pipe, and its old path's call is left past the exit budget.
  std::unique_ptr<FeedbackLogUpload> feedbackLogUpload_ = std::make_unique<FeedbackLogUpload>(
      [this](const std::string& feedbackId) { return AskServiceToUploadLogs(feedbackId); },
      [this](const std::string& feedbackId) { return PrepareDeviceRemoteLogUpload(feedbackId); });
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
  // this build's version (Common/Version.h): what the sdk's client info, the
  // service's devices, the product events and the log uploads report. It was
  // a hard-coded "0.0.1", which every one of them sent for every build.
  std::string appVersion_ = urnw::version::kString;
  // The Api's logout listener (BindApiLocked), replaced with the Api.
  std::optional<urnet::Sub> apiLogoutSub_;

  // Answer and clear whichever wallet-bridge flow is outstanding. Called when a
  // new one starts: the bridge has a single pair of callbacks, so the new flow
  // takes them over and the old one has to be TOLD rather than abandoned - an
  // abandoned callback is a busy flag nothing will ever clear. Returns the new
  // flow's number (walletFlows_), which a challenge continuation checks before
  // it opens the bridge.
  uint64_t CancelPendingWalletFlows(const char* reason);

  WalletConnect wallet_;
  std::function<void(AuthResult)> walletAuthDone_;

  // ---- the Bittensor proof in flight (one at a time, like every bridge flow)
  struct BittensorProofOutcome {
    bool ok = false;
    urnet::BittensorWalletProof proof;
    std::string error;  // localized, or a superseded reason
  };
  // Start a proof for `flow` (walletFlows_): fetch the session's challenge,
  // then open the bridge (Talisman) or ask the manual handler (TAO.com).
  // `done` runs once: with the proof, a refusal, or a superseding flow's reason.
  // `manualHandler` replaces the window's manual form for this proof (the add
  // sheet shows its own); null = the window's (SetBittensorManualHandler).
  void BeginBittensorProof(uint64_t flow, const std::string& walletId, const std::string& purpose,
                           const std::string& expectedAddress,
                           std::function<void(BittensorProofOutcome)> done,
                           std::function<void(BittensorManualRequest)> manualHandler = nullptr);
  // the urnetwork://bittensor-sign-message hand-back (WalletConnect)
  void HandleBittensorReturn(const std::string& url);
  // Answer proof `serial` if it is still the one in flight.
  void FinishBittensorProof(uint64_t serial, BittensorProofOutcome outcome);
  // guards the four members below; never held while a callback runs
  std::mutex bittensorLock_;
  uint64_t bittensorSerial_ = 0;
  std::shared_ptr<urnet::BittensorWalletSession> bittensorSession_;
  std::function<void(BittensorProofOutcome)> bittensorDone_;
  std::function<void(BittensorManualRequest)> bittensorManualHandler_;
  // The wallet a Bittensor sign-in used: its network-create step (a fresh
  // challenge bound to the same address) goes through the same wallet.
  std::string pendingBittensorWalletId_;
  // A bare signature request (SignWithSolanaWallet) rather than a sign-in. Non-
  // null is what tells the shared bridge callbacks which flow they are in, so
  // both are cleared whenever the other starts.
  std::function<void(bool, std::string, std::string, std::string)> walletSignDone_;
  std::string walletSignMessage_;
  // A bare connect request (ConnectSolanaWallet): answered from on_public_key
  // with the wallet's address, before any challenge or signature. Like
  // walletSignDone_ and walletAuthDone_, CancelPendingWalletFlows answers and
  // clears it whenever another flow starts.
  std::function<void(bool, std::string, std::string)> walletConnectDone_;
  // The number of the wallet-bridge flow that started last. A flow superseded
  // while its challenge was being fetched must not open the bridge afterwards:
  // on Windows that would open a stray tab, overwrite walletSignMessage_ and
  // reset the session the current flow depends on.
  bridge::FlowSerial walletFlows_;
  // Exact single-use server challenge being signed by an authentication flow.
  // Kept separate from walletSignMessage_, which also serves signed-in utility
  // signature requests.
  std::string walletAuthMessage_;
  // the instant network's jwt, held between CreateInstantAccount and
  // ConfirmInstantAccount so the seedphrase is read before the session exists
  std::optional<std::string> pendingInstantJwt_;
  // an SSO identity token whose identity has no network yet, held for the
  // create-network step (the auth-jwt analogue of pendingWalletAuth_), with
  // the provider it came from ("google" | "apple") for auth_jwt_type
  std::optional<std::string> pendingAuthJwt_;
  std::string pendingAuthJwtType_;
  // The Google / Apple sign-in attempt in flight: its provider, the state the
  // api's callback must echo and the nonce the returned token must carry.
  // Cleared by its answer or
  // by a superseding flow (CancelPendingWalletFlows).
  struct SsoAttempt {
    std::string provider;
    std::string state;
    std::string nonce;
    // who started it: a login (authLogin) or the add sheet (addAuth)
    add_sign_in::SsoPurpose purpose = add_sign_in::SsoPurpose::SignIn;
  };
  std::optional<SsoAttempt> ssoAttempt_;
  // An add-owned sso attempt's answer (SsoTokenForAdd): the identity token or
  // an error. Like walletAuthDone_, CancelPendingWalletFlows answers and
  // clears it whenever another flow starts.
  std::function<void(std::string, std::string)> ssoAddDone_;
  // Open the provider's web flow for a fresh attempt (state + nonce).
  void OpenSsoAttempt(const std::string& provider, add_sign_in::SsoPurpose purpose);
  // Identity of a wallet that has no network yet. The discovery signature has
  // already been consumed; create-network always requests a fresh bound
  // challenge before this value can be submitted.
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
  TransportDistributionHandler onTransportDistribution_;
  ExtenderStatusHandler onExtenderStatus_;
  ExtenderProvideStatusHandler onExtenderProvideStatus_;
  ProviderThroughputHandler onProviderThroughput_;
  ProviderOnlyStatusHandler onProviderOnlyStatus_;
  TransportSettingsHandler onTransportSettings_;
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
