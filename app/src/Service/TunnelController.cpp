// SPDX-License-Identifier: MPL-2.0
#include "TunnelController.h"

#include <chrono>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>   // AF_INET / AF_INET6
#include <windows.h>

#include "ConsoleArgs.h"  // ClampStopAfterStep — the flag's own bounds
#include "Heartbeat.h"    // the lock-free state mirror the heartbeat reads
#include "Ids.h"
#include "Log.h"
#include "Paths.h"
#include "StopBudget.h"   // the shutdown budgets and the abandonable teardown
#include "Strings.h"
#include "ThreadGuard.h"

namespace urnw {
namespace {

constexpr DWORD kRingCapacity = 0x400000;  // 4 MiB (power of two, within wintun bounds)
constexpr uint32_t kTunnelMtu = 1440;      // matches macOS

std::filesystem::path ExeDir() {
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

void WriteFileBytes(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (f) f.write(reinterpret_cast<const char*>(b.data()), b.size());
}

// Registry-style GUID text. Hand-rolled rather than StringFromGUID2, which
// lives behind combaseapi.h — the service has no other reason to pull COM in.
std::string GuidText(const GUID& g) {
  return std::format(
      "{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
      g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
      g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

std::string Join(const std::vector<std::string>& parts) {
  std::string out;
  for (const auto& p : parts) {
    if (!out.empty()) out += ",";
    out += p;
  }
  return out;
}

}  // namespace

TunnelController::TunnelController() {
  storageDir_ = StorageRoot(/*isService=*/true);
}

TunnelController::~TunnelController() { Stop(); }

std::optional<urnet::DeviceLocalKeyMaterial> TunnelController::LoadKeyMaterial() {
  auto seed = ReadFileBytes(storageDir_ / L"client_key_seed.bin");
  auto cert = ReadFileBytes(storageDir_ / L"provide_cert.pem");
  auto key = ReadFileBytes(storageDir_ / L"provide_key.pem");
  if (seed.empty() || cert.empty() || key.empty()) return std::nullopt;
  return urnet::newDeviceLocalKeyMaterial(
      seed.data(), static_cast<int32_t>(seed.size()), cert.data(),
      static_cast<int32_t>(cert.size()), key.data(),
      static_cast<int32_t>(key.size()));
}

void TunnelController::PersistKeyMaterial(const urnet::DeviceLocalKeyMaterial& km) {
  WriteFileBytes(storageDir_ / L"client_key_seed.bin", km.getClientKeySeed());
  WriteFileBytes(storageDir_ / L"provide_cert.pem", km.getProvideTlsCertificatePem());
  WriteFileBytes(storageDir_ / L"provide_key.pem", km.getProvideTlsPrivateKeyPem());
}

void TunnelController::ClampToRpcOnly() {
  rpcOnlyClamp_.store(true);
  LogWarn("tunnel: CLAMPED TO RPC-ONLY for the life of this process. Every "
          "start_tunnel will be served as rpc-only whatever it requests: no "
          "wintun adapter, and this process will not write a route or a dns "
          "entry no matter what any client asks for.");
}

void TunnelController::SetStopAfterStep(int step) {
  const int clamped = ClampStopAfterStep(step);
  if (clamped != step) {
    // Unreachable from the parser, which rejects anything outside 1..8 rather
    // than normalising it. Loud anyway: the direction of the clamp is a safety
    // property, and a silent correction here would hide a caller that computed
    // the step wrongly — including one that computed 0 and meant "no stop".
    LogError("tunnel: --stop-after was given the out-of-range step {}; clamping "
             "to {} (towards the EARLIER stop). Out of range is never read as "
             "'run all eight steps'.",
             step, clamped);
  }
  stopAfterStep_.store(clamped);
  LogWarn("tunnel: STAGED BRING-UP: every start_tunnel served by this process "
          "will stop after step {}/8 and unwind through the ordinary teardown. "
          "{} This flag only ever stops the sequence EARLIER — it enables "
          "nothing, and it does not lift the rpc-only clamp if one is in force.",
          clamped,
          clamped >= 8 ? std::string("All eight steps run and are then torn "
                                     "straight back down.")
          : clamped == 7
              ? std::string("Step 8/8 will not run.")
              : std::format("Steps {}/8 to 8/8 will not run.", clamped + 1));
}

std::wstring TunnelController::ServiceImagePath() {
  wchar_t buf[MAX_PATH];
  DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return std::wstring(buf, n);
}

std::wstring TunnelController::AppImagePath() {
  std::error_code ec;
  const std::filesystem::path app = ExeDir() / L"URnetwork.exe";
  if (!std::filesystem::exists(app, ec)) return {};
  return app.wstring();
}

WfpConfig TunnelController::BaseWfpConfig() {
  WfpConfig cfg;
  // Must track the route set: the routes send these ranges out the physical
  // NIC, so the firewall has to permit them or the LAN silently dies. Both are
  // built from net::kLocalBypassV4 (NetPolicy.h).
  cfg.allow_lan = true;
  cfg.block_ipv6 = true;
  cfg.service_image_path = ServiceImagePath();
  // Populated for EVERY state and read by exactly one (Connected). That is
  // deliberate and it is the same shape host_resolvers_v4 has in reverse:
  // BuildFilterSet decides which states a field turns into a filter, so this
  // function stays a pure "what is true of this machine" answer and cannot be
  // the place a policy quietly widens. BaseWfpConfig is also what the connecting
  // watchdog rebuilds Armed from off-thread, and Armed's filter set must not
  // depend on session state — this field is a constant of the install, so it
  // does not make it one.
  cfg.app_image_path = AppImagePath();
  return cfg;
}

bool TunnelController::ApplyWfpLocked(WfpState state) {
  // FIRST, BEFORE ANYTHING IS INSTALLED. A watchdog from a previous window must
  // be fully stopped — and any Apply it had already entered must have finished —
  // before this transition installs anything. Cancelling AFTER the apply would
  // leave a race in which a stale watchdog's "narrow to Armed" lands just after
  // our "widen to Connecting" and silently closes the window we are opening.
  // Cancel joins, so on the far side of this line no other thread is in
  // WfpPolicy at all.
  CancelConnectingWatchdogLocked();
  if (state == WfpState::Off) {
    wfp_.Revert();
    return true;
  }
  WfpConfig cfg = BaseWfpConfig();
  cfg.tun_luid = (state == WfpState::Connected && adapter_)
                     ? adapter_->Luid().Value
                     : 0;
  cfg.tunnel_resolvers_v4 =
      state == WfpState::Connected ? appliedResolvers_ : std::vector<std::string>{};
  // CONNECTING ONLY. There is no tunnel resolver yet, so without this the
  // port-53 block has nothing to permit and OUR OWN name resolution dies with
  // everyone else's — and ours does not come out of this process: the SDK is Go,
  // Go on Windows resolves through GetAddrInfoW, and the wire query is issued by
  // the DNS Client service in svchost.exe, so the app-id permit cannot match it.
  //
  // Deliberately NOT read for Armed. The permit it produces is address-scoped
  // and therefore machine-wide, and Armed is the idle state — nothing is
  // resolving, so there is nothing to strand, and installing it would open
  // plaintext DNS for every process on the box for as long as the kill switch is
  // on. See WfpPolicy.cpp filter 9b.
  //
  // Read fresh on entry to every attempt, so a roam cannot leave a stale permit
  // behind.
  if (state == WfpState::Connecting) {
    cfg.host_resolvers_v4 =
        NetworkConfig::HostResolversV4(adapter_ ? adapter_->Luid() : NET_LUID{});
    if (cfg.host_resolvers_v4.empty()) {
      LogWarn("wfp: this host has no usable IPv4 resolver on any adapter, so "
              "the connecting policy has no DNS path to permit for our own name "
              "resolution. The port-53 hard block stands down for the length of "
              "this attempt rather than leaving it unable to resolve — see the "
              "wfp warning that follows for exactly what that opens.");
    } else {
      LogInfo("wfp: connecting-state DNS path = the host's own resolvers [{}] "
              "(read fresh; our name resolution leaves svchost, not this "
              "process, so it cannot be permitted by app id)",
              Join(cfg.host_resolvers_v4));
    }
  }
  if (cfg.service_image_path.empty()) {
    // Rank-1 exemption. Without an app id we would install a policy that blocks
    // our own transport, and the machine would be armed with no way back except
    // an elevated revert. Refuse to install anything at all instead.
    LogError("wfp: cannot resolve this executable's path; REFUSING to install a "
             "policy that would block our own service");
    return false;
  }
  // The app permit, at both edges, and only where it applies. Not fatal when
  // absent: the app then behaves exactly as it did before this existed (its
  // sockets stay in the tun), which is worse but not broken.
  if (state == WfpState::Connected) {
    if (cfg.app_image_path.empty()) {
      LogWarn("wfp: URnetwork.exe was not found next to this executable, so the "
              "UI process gets NO firewall permit. Its platform traffic stays "
              "inside the tunnel while connected — if the tunnel has no working "
              "exit, the UI cannot reach the platform to say so.");
    } else {
      LogInfo("wfp: permitting the UI process ({}) on the physical NIC for the "
              "life of this connected session. It pairs with the app binding "
              "its own sdk egress off the tun; neither half works alone. This "
              "permit exists in CONNECTED ONLY — armed still permits urnetworkd "
              "and nothing else.",
              Narrow(cfg.app_image_path));
    }
  }
  const bool ok = wfp_.Apply(state, cfg);
  // Arm the watchdog on the way IN. Every other transition has already cancelled
  // it at the top, so this one branch is the whole lifecycle. Keyed off the
  // RESULT and not the argument, so a Connecting that failed to install does not
  // leave a timer waiting to narrow a policy that was never widened.
  if (ok && state == WfpState::Connecting) ArmConnectingWatchdogLocked();
  return ok;
}

// How long a connection attempt may hold the DNS window open.
//
// Chosen against what a LEGITIMATE slow attempt costs on this platform, because
// a timeout under that number turns a bad-network connect into an
// unreconnectable machine — the failure this whole file exists to avoid:
//
//   * The Windows DNS client's query schedule across a server list runs to
//     roughly 12-13s before it gives up (1s, 2s, 2s, 4s, 4s).
//   * A TCP connect to a black-holed address costs ~21s (SYN + 2 retransmits at
//     the Windows default), and the SDK dials after it resolves.
//
// So ~35s is a plausible honest attempt on a hostile network, and 60s is a
// little under 2x that: long enough that the watchdog never fires on a slow
// success, short enough that a WEDGED attempt cannot hold machine-wide plaintext
// DNS open for the life of the process. It is a backstop, not the normal path —
// the ordinary close is StartLocked reaching Connected or falling back through
// StopLocked to Armed, both of which happen in seconds.
constexpr std::chrono::seconds kConnectingWindow{60};

void TunnelController::ArmConnectingWatchdogLocked() {
  // Defensive: the only caller has already cancelled, so this is a no-op. It is
  // here so that a second caller cannot leak a thread.
  CancelConnectingWatchdogLocked();
  {
    std::scoped_lock lock(watchdogMutex_);
    watchdogCancelled_ = false;
  }
  watchdog_ = StartGuardedThread("connecting-watchdog", [this] {
    {
      std::unique_lock lock(watchdogMutex_);
      if (watchdogWake_.wait_for(lock, kConnectingWindow,
                                 [this] { return watchdogCancelled_; })) {
        return;  // a transition beat us to it; nothing to do
      }
    }
    // Deliberately does NOT take mutex_: a wedged connect attempt is holding it,
    // and that is the case this exists for. Nothing below reads session state —
    // the armed policy depends on none (WfpPolicy.h, host_resolvers_v4) — and
    // WfpPolicy serialises this against the connect thread's own Apply.
    LogWarn("tunnel: a connection attempt has held the DNS window open for {}s "
            "without finishing. Narrowing the firewall back to ARMED now: the "
            "machine-wide plaintext-DNS permit closes, and an attempt that is "
            "still running will no longer be able to resolve. This is a "
            "backstop — reaching it means the attempt is wedged, not slow.",
            static_cast<long long>(kConnectingWindow.count()));
    wfp_.Apply(WfpState::Armed, BaseWfpConfig());
  });
}

void TunnelController::CancelConnectingWatchdogLocked() {
  if (!watchdog_.joinable()) return;
  {
    std::scoped_lock lock(watchdogMutex_);
    watchdogCancelled_ = true;
  }
  watchdogWake_.notify_all();
  // Safe to join under mutex_: the watchdog thread never acquires it. If it is
  // mid-Apply this waits for that Apply, which is bounded by BFE.
  watchdog_.join();
}

// See the contract in the header: the lock-free mirror is why every write to
// state_ goes through one function instead of eight assignments.
//
// THE GLOG FLUSH IS DELIBERATELY NOT HERE, and the reason is the ordering rule
// this file already lives by. StopLocked calls this with Stopping BEFORE
// RevertMachineStateLocked, and flushGlog() is a cgo call into the very runtime
// that may be the thing wedged — so putting it here would sequence a call that
// can block on the SDK ahead of the route revert, which StopBudget.h calls the
// one thing that must never be sequenced behind anything. The flush happens at
// the RPC boundary instead (ControlServer::PushState, outside mutex_) and on
// the ~1 Hz heartbeat tick, which together bound how stale the SDK's log can be
// to about a second on every path including teardown.
void TunnelController::SetStateLocked(proto::TunnelState next) {
  state_ = next;
  PublishTunnelState(proto::ToString(next));
}

proto::TunnelStatus TunnelController::Start(const proto::StartTunnel& config) {
  std::scoped_lock lock(mutex_);
  return StartLocked(config);
}

// --- the staged bring-up stop point (--stop-after=<N>) ----------------------
//
// Called at each of the eight step boundaries. Inert — one atomic load and a
// compare — unless the flag was passed, which is what keeps every existing
// invocation byte-identical.
//
// `step >= stopAfter` rather than `step == stopAfter` ON PURPOSE. Step 1 is
// SKIPPED in rpc-only mode, so a boundary can be reached without its step having
// run; a `==` test would sail past a stop point that was never emitted and carry
// on to the next one. The comparison that cannot do that is the one that stops at
// the first boundary AT OR AFTER the requested step, which is also the reading
// that always errs towards stopping sooner.
bool TunnelController::HaltAfterStepLocked(int step, const char* reached) {
  const int stopAfter = stopAfterStep_.load();
  if (stopAfter == 0 || step < stopAfter) return false;

  // Read off the objects that OWN each fact, not inferred from the step number.
  // The whole point of this log is to tell the operator whether anything is
  // still applied to their machine, and a step number is a claim about what
  // should have happened, not a report of what did.
  const bool hadAdapter = adapter_ != nullptr;
  const bool hadRoutes = netConfig_ != nullptr;
  const bool hadDns = netConfig_ != nullptr && netConfig_->DnsApplied();
  const WfpState wfpAtStop = wfp_.State();

  LogWarn("tunnel: ======== STAGED BRING-UP: STOPPING AFTER STEP {}/8 ========",
          step);
  LogWarn("tunnel: --stop-after={} was passed, so the sequence stops here. The "
          "last thing that ran was step {}/8, which left {}. {}",
          stopAfter, step, reached,
          step >= 8 ? std::string("Nothing was skipped; the full bring-up is "
                                  "being torn straight back down.")
          : step == 7 ? std::string("Step 8/8 did NOT run.")
                      : std::format("Steps {}/8 to 8/8 did NOT run.", step + 1));
  LogWarn("tunnel: state AT THE STOP POINT: wintun adapter={}, address/mtu/"
          "routes={}, tunnel dns={}, firewall policy={}.",
          hadAdapter ? "CREATED" : "none",
          hadRoutes ? "APPLIED" : "not applied",
          hadDns ? "APPLIED" : "not applied", ToString(wfpAtStop));
  LogWarn("tunnel: unwinding through the ORDINARY teardown — the same StopLocked "
          "a user pressing disconnect runs, with the same revert, the same "
          "resolver-cache flush, the same active-marker handling and the same "
          "kill-switch rules. There is no second teardown path here.");

  // finalDisarm=true, and it is the same argument Stop() passes. This IS a
  // deliberate stop: the operator asked for the sequence to end at this step.
  // The alternative (false, the failed-start/reconnect path) would hold the
  // firewall ARMED across a gap that has no next session coming — i.e. it would
  // leave the operator's machine blocked, with no UI to explain it, at the end
  // of a run whose entire purpose was to prove the machine comes back.
  StopLocked(/*finalDisarm=*/true);

  // What is ACTUALLY left, read again after the unwind. If any of these is not
  // the benign value, the teardown did not finish and this line is the only
  // place the operator finds that out before looking at their route table.
  const bool routesLeft = netConfig_ != nullptr;
  const bool adapterLeft = adapter_ != nullptr;
  const bool markerLeft = PeekActiveMarker();
  const WfpState wfpAfter = wfp_.State();
  const bool clean = !routesLeft && !adapterLeft && !markerLeft &&
                     wfpAfter == WfpState::Off;
  if (clean) {
    LogWarn("tunnel: ======== STOPPED AFTER STEP {}/8. NOTHING IS LEFT APPLIED "
            "======== routes/dns reverted, firewall policy off, wintun adapter "
            "gone, no active marker. {}",
            step,
            hadRoutes ? "This machine's routes and DNS were rewritten by this "
                        "run and have been given back — diff them against your "
                        "baseline before you trust that sentence."
                      : "This run never wrote a route, a dns entry or an "
                        "address, so there was nothing to give back.");
  } else {
    LogError("tunnel: ======== STOPPED AFTER STEP {}/8, BUT SOMETHING IS STILL "
             "APPLIED ======== routes={} adapter={} active_marker={} "
             "firewall={}. The teardown did not fully unwind. Stop this process "
             "(the adapter and the filter policy both die with it), then run "
             "`urnetworkd revert` from an elevated prompt.",
             step, routesLeft ? "STILL INSTALLED" : "reverted",
             adapterLeft ? "STILL PRESENT" : "gone",
             markerLeft ? "STILL SET" : "clear", ToString(wfpAfter));
  }
  return true;
}

proto::TunnelStatus TunnelController::StartLocked(const proto::StartTunnel& config) {
  // Adopt the caller's kill-switch preference BEFORE the teardown below, so a
  // reconnect keeps the policy in force across the gap rather than dropping it
  // and re-arming.
  killSwitch_.store(config.kill_switch);
  StopLocked(/*finalDisarm=*/false);  // idempotent restart
  // A NEW ATTEMPT CLEARS THE LAST TEARDOWN'S REASON. Left set, a failsafe stop
  // would keep explaining itself over the top of the connection that replaced
  // it — the app renders "URnetwork disconnected you" beside a live tunnel.
  lastStopReason_.store(kStopReasonNone);

  // REFUSE TO START ON TOP OF AN ABANDONED TEARDOWN. The teardown above is
  // bounded, so it can return having LEFT the previous session's DeviceLocal
  // and wintun adapter alive on a detached thread (see TearDownSessionLocked).
  // Building a second session over that would ask wintun to create a second
  // adapter carrying the same pinned GUID while the first still holds it, with
  // two DeviceLocals and two packet pumps behind it — a worse machine state
  // than the failure that got us here, and one no revert path understands.
  //
  // The honest answer is that this process is finished. The installed service
  // has SC_ACTION_RESTART configured (InstallService in main.cpp) and its next
  // start runs the sweep, so "restart me" is a real recovery route rather than
  // a dead end.
  if (TeardownAbandoned()) {
    SetStateLocked(proto::TunnelState::Error);
    error_ =
        "a previous teardown could not be completed and its device is still "
        "held; this service process must be restarted before another tunnel "
        "can start";
    LogError("tunnel: REFUSING to start — {}. Restart urnetworkd (sc stop "
             "urnetworkd && sc start urnetworkd).",
             error_);
    return Status();
  }
  SetStateLocked(proto::TunnelState::Starting);
  // The clamp wins over the request, and it is applied HERE, once, before
  // anything reads the mode. Everything downstream — the fence, the step-6
  // precondition, the reported state and mode — reads startMode_, so a clamped
  // process cannot have a Tunnel-mode session by any route.
  if (rpcOnlyClamp_.load() && config.mode != proto::StartMode::RpcOnly) {
    LogWarn("tunnel: start requested mode={} but this process is clamped to "
            "rpc-only; serving it as rpc-only. The reply reports mode=rpc_only "
            "and state=rpc_only, so the caller can see it did not get a tunnel.",
            proto::ToString(config.mode));
    startMode_ = proto::StartMode::RpcOnly;
  } else {
    startMode_ = config.mode;
  }
  const bool rpcOnly = startMode_ == proto::StartMode::RpcOnly;
  error_.clear();

  // === THE RETRY PATH ======================================================
  //
  // OPEN THE DNS WINDOW HERE, BEFORE ANYTHING RESOLVES. This is the single line
  // that keeps the Armed/Connecting split from silently breaking reconnection,
  // and it is at the top rather than at step 6/8 because THE RESOLUTION HAPPENS
  // AT STEPS 3-5, before step 6 exists:
  //
  //   3/8 importNetworkSpaceFromJson
  //   4/8 newDeviceLocal* — the first thing that dials the platform BY NAME
  //   5/8 setRpcServer
  //   ... the fence ...
  //   6/8 the firewall policy   <- where the policy USED to be applied
  //
  // The state on entry is whatever StopLocked just left. With the kill switch on
  // that is ARMED — StopLocked(finalDisarm=false) narrows to Armed and holds it
  // across the gap, which is the entire point of a kill switch — so every
  // attempt AFTER the first (the user pressing connect again, the app
  // re-bootstrapping on resume, any future automatic retry or backoff) would run
  // steps 3-5 under a policy with NO DNS PATH. It would fail at 3/8 or 4/8,
  // return through StopLocked to Armed, and fail identically forever: armed ->
  // cannot resolve -> cannot connect -> still armed. Widening at step 6 would be
  // too late by three steps.
  //
  // Gated on the policy being installed at all: with the kill switch off nothing
  // is blocked, so there is no window to open. Gated on !rpcOnly because that
  // mode's promise is that it writes nothing to this machine, and a filter is
  // something.
  if (!rpcOnly && wfp_.State() != WfpState::Off) {
    LogInfo("tunnel: a connection attempt is starting while the firewall is {} — "
            "widening to CONNECTING before step 3/8 so our own name resolution "
            "has a path. Steps 3-5 resolve the platform host; step 6/8 is far too "
            "late to open it.",
            ToString(wfp_.State()));
    if (!ApplyWfpLocked(WfpState::Connecting)) {
      // Not fatal on its own: the attempt may still resolve from the DNS Client
      // cache, and failing the start here would turn a firewall hiccup into a
      // machine that cannot connect. It is loud because the likely next symptom
      // is a start that dies at 3/8 or 4/8 for no visible reason.
      LogError("tunnel: could not widen the firewall to CONNECTING ({}). The "
               "armed policy has no DNS path, so this attempt can only resolve "
               "from the OS cache and will probably fail at step 3/8 or 4/8.",
               wfp_.LastError());
    }
  }

  // Named so a failure says which step threw. Nothing here has run before, so
  // "it stopped after step 3" is the whole diagnosis on the first real start.
  const char* step = "init";
  const int64_t startedAtMillis = NowMillis();
  LogInfo("tunnel: starting mode={} (rpc={} device=\"{}\" spec=\"{}\" app={} "
          "jwt={}B split={} paths={})",
          proto::ToString(startMode_), config.rpc_listen_hostport,
          config.device_description, config.device_spec, config.app_version,
          config.by_jwt.size(), config.allowlist_mode ? "allowlist" : "denylist",
          config.excluded_app_paths.size());
  if (rpcOnly) {
    // Loud, and at the top, because every line after this one has to be read in
    // this light: no adapter is created, no address, no route and no DNS entry
    // is written, and the sequence RETURNS after step 5. Nothing below can
    // reach step 6.
    LogWarn("tunnel: RPC-ONLY MODE — no wintun adapter, and the machine's "
            "ROUTES AND DNS WILL NOT BE TOUCHED. Steps 1, 6, 7 and 8 are "
            "skipped; the session ends at the rpc listener (step 5) and reports "
            "state 'rpc_only', never 'up'. No traffic is carried.");
  }

  try {
    // --- 1/8 wintun adapter (installs the driver on first use; needs SYSTEM) ---
    // Created FIRST, before any SDK object: the adapter is what the egress
    // binding below has to exclude, and it carries no address or route yet so
    // it cannot attract traffic while we set the rest up.
    step = "1/8 wintun";
    if (rpcOnly) {
      // Step 1 is the ONLY one of steps 1-5 that needs elevation, and the only
      // thing steps 2-5 want from it is the LUID step 2 excludes — which a zero
      // LUID expresses exactly (see EgressMonitor's ctor). Skipping it is what
      // lets this mode run unelevated.
      LogInfo("tunnel: [1/8] SKIPPED (rpc-only): no wintun adapter is created, "
              "so this mode needs no elevation");
      // The stop point is checked in BOTH branches, not once after the if/else,
      // because what step 1 left behind is the whole content of the message and
      // it differs completely between them.
      if (HaltAfterStepLocked(
              1, "NOTHING — step 1 was skipped in rpc-only mode, so no wintun "
                 "adapter was created. If you wanted the adapter, drop "
                 "--rpc-only and run elevated"))
        return Status();
    } else {
      const std::filesystem::path dll = ExeDir() / L"wintun.dll";
      LogInfo("tunnel: [1/8] loading wintun from {}", dll.string());
      wintun_ = Wintun::Load(dll);
      if (!wintun_)
        throw std::runtime_error(
            "failed to load wintun.dll (is it next to urnetworkd.exe?)");
      adapter_ = WintunAdapter::Create(*wintun_, ids::kTunAdapterName,
                                       ids::kTunAdapterGuid, kRingCapacity);
      if (!adapter_)
        throw std::runtime_error(
            "failed to create the wintun adapter (needs LocalSystem/admin and a "
            "loadable wintun driver)");
      NET_IFINDEX tunIndex = 0;
      NET_LUID tunLuid = adapter_->Luid();
      ::ConvertInterfaceLuidToIndex(&tunLuid, &tunIndex);
      // Log the GUID and alias wintun ACTUALLY assigned, not the ones we asked
      // for. WintunCreateAdapter treats the GUID as a request, and those two
      // values are exactly what the startup orphan sweep matches on — if a
      // sweep ever fails to find a stranded adapter, this line is the answer.
      GUID assignedGuid{};
      const std::string requestedGuid = GuidText(ids::kTunAdapterGuid);
      std::string guidText = "?";
      if (::ConvertInterfaceLuidToGuid(&tunLuid, &assignedGuid) == NO_ERROR)
        guidText = GuidText(assignedGuid);
      LogInfo("tunnel: [1/8] adapter up: luid {:#x}, guid {} ({}), interface {}",
              tunLuid.Value, guidText,
              guidText == requestedGuid ? "as requested"
                                        : "NOT the requested " + requestedGuid,
              NetworkConfig::DescribeInterface(tunIndex));
      // GATE B's stop point: the adapter exists and carries nothing. This is the
      // one boundary --rpc-only structurally cannot reach.
      if (HaltAfterStepLocked(
              1, "a wintun adapter with NO address, NO route and NO dns server "
                 "— an interface exists on this machine, and nothing is routed "
                 "to it"))
        return Status();
    }

    // --- 2/8 R1: bind the SDK's egress to the physical interface. ---
    // Ordering is the whole mechanism, and it is load-bearing twice over:
    //   * BEFORE any SDK object exists, so no socket is ever created unbound
    //     (an unbound socket keeps whatever route it resolved and will follow
    //     the tun once step 6 installs the routes);
    //   * BEFORE step 6 installs those routes, so DiscoverEgress still sees a
    //     clean table and picks the physical default route.
    // Do not move this below the NetworkSpace/DeviceLocal construction.
    step = "2/8 egress (R1)";
    // No adapter in rpc-only mode, so there is no tun to exclude: a zero LUID
    // says so literally, and DiscoverEgress's `luid == tunLuid` test then
    // matches nothing. Nothing is faked.
    const NET_LUID egressExcludeLuid = adapter_ ? adapter_->Luid() : NET_LUID{};
    LogInfo("tunnel: [2/8] binding sdk egress to the physical interface ({})",
            rpcOnly ? "rpc-only: no tun to exclude, so this is a preference, "
                      "not R1 protection"
                    : "R1");
    egress_ = std::make_unique<EgressMonitor>(egressExcludeLuid);
    // Set before Start(), which refreshes synchronously. The handler takes only
    // splitMutex_ — see the note on it in the header; it must not take mutex_,
    // which this thread is holding right now and StopLocked holds while waiting
    // for the monitor's callbacks to drain.
    egress_->SetOnChange([this](EgressInterfaces e) { OnEgressChanged(e); });
    // THE SECOND CALLBACK, AND THE ONE THAT COVERS THE OWNER'S CASE. SetOnChange
    // above fires only when the bound INDEX MOVES, which is false for a cable
    // coming out (the index is deliberately retained) — so it cannot be the hook
    // that tells the SDK the network moved. This one fires on every observation.
    //
    // It must do almost nothing: it runs on a system worker thread that
    // EgressMonitor::Stop() waits for, so it records the event and returns. The
    // SDK calls happen on the watchdog's own thread, coalesced.
    egress_->SetOnNetworkEvent([this] { deadTunnelWatchdog_.NoteNetworkEvent(); });
    egress_->Start();  // logs the chosen interface; keeps it current on change
    if (egress_->Current().index4 == 0) {
      // Not fatal — there may genuinely be no network yet, and the monitor will
      // bind as soon as one appears — but it is the R1 hazard, so it is loud.
      // With no tun there is no loop to fall into, so it is only a note.
      if (rpcOnly) {
        LogWarn("tunnel: [2/8] no physical ipv4 egress interface (rpc-only: no "
                "tun exists, so there is nothing to loop into)");
      } else {
        LogError("tunnel: [2/8] no physical ipv4 egress interface; R1 protection "
                 "is NOT in force yet");
      }
    }
    // GATE C's stop point: the egress binding is decided and logged, and no
    // route has been written, so `Get-NetTCPConnection -OwningProcess <pid>` can
    // be read against a still-clean route table.
    if (HaltAfterStepLocked(
            2, "the sdk's egress bound to the physical nic — a setting inside "
               "this process only; no interface, route or dns entry on this "
               "machine was changed by it"))
      return Status();

    // --- 3/8 NetworkSpace (own storage; import the app's space json) ---
    step = "3/8 network space";
    LogInfo("tunnel: [3/8] opening the network space in {}",
            SdkStorageDir(true).string());
    if (!spaceManager_) {
      spaceManager_ =
          urnet::newNetworkSpaceManager(Narrow(SdkStorageDir(true).wstring()));
    }
    networkSpace_ = spaceManager_->importNetworkSpaceFromJson(config.network_space_json);
    if (HaltAfterStepLocked(
            3, "an open network space under the service's own storage root — "
               "files, and nothing else"))
      return Status();

    // --- 4/8 DeviceLocal (stable provider identity via persisted key material) ---
    step = "4/8 device";
    auto km = LoadKeyMaterial();
    LogInfo("tunnel: [4/8] constructing DeviceLocal ({} identity)",
            km ? "persisted" : "new");
    if (km) {
      device_ = urnet::newDeviceLocalWithKeyMaterial(
          *networkSpace_, config.by_jwt, config.device_description,
          config.device_spec, config.app_version, config.instance_id,
          /*enable_rpc=*/false, *km);
    } else {
      device_ = urnet::newDeviceLocalWithDefaults(
          *networkSpace_, config.by_jwt, config.device_description,
          config.device_spec, config.app_version, config.instance_id,
          /*enable_rpc=*/false);
      PersistKeyMaterial(device_->getKeyMaterial());
    }
    LogInfo("tunnel: [4/8] device client_id={}", device_->getClientId());
    if (HaltAfterStepLocked(
            4, "a live DeviceLocal and its persisted identity — sockets to the "
               "platform, over the PHYSICAL nic; still no interface, route or "
               "dns entry of ours on this machine"))
      return Status();

    // --- 5/8 mTLS RPC listener the app's DeviceRemote dials ---
    step = "5/8 rpc";
    LogInfo("tunnel: [5/8] starting the device rpc listener on {}",
            config.rpc_listen_hostport);
    device_->setRpcServer(config.rpc_server_pem, config.rpc_client_cert_pem,
                          config.rpc_listen_hostport);
    rpcHostPort_ = config.rpc_listen_hostport;

    // --- the window trace (URNETWORK_SDK_TRACE; off unless set) --------------
    //
    // Started HERE, at the end of step 5, and deliberately BEFORE the fence:
    //
    //   * the device exists, so there is something to sample;
    //   * it is before step 6, so the trace covers the whole destructive half as
    //     well as the session, and the first window's formation — which is the
    //     thing being measured — happens after the app connects, i.e. long after
    //     this point;
    //   * it is reached in rpc-only mode too. That is on purpose: window
    //     formation needs no tun, no routes and no elevation, so the trace can
    //     be exercised unelevated with `--rpc-only` before it is trusted in an
    //     elevated run.
    //
    // Read from the environment per session rather than cached, so a service
    // left running can be re-traced by restarting the session.
    if (const WindowTraceConfig traceCfg = WindowTraceFromEnvironment();
        !traceCfg.error.empty()) {
      // Not fatal. A malformed diagnostic flag must not stop a tunnel, and it
      // must not silently become an enabled one either — so it is off, and it
      // says so at WARN where the operator who typed it will see it.
      LogWarn("tunnel: [5/8] {} The window trace is OFF for this session.",
              traceCfg.error);
    } else if (traceCfg.enabled) {
      trace_.Start(&*device_, traceCfg);
    }

    // The step-5 stop point sits BEFORE the fence, so that when --stop-after=5
    // and --rpc-only are both given the one that does LESS wins. Both end the
    // sequence at the same step; only this one also gives the rpc listener and
    // the DeviceLocal back. "Stops things earlier, never widens them" decides
    // the tie, and the rpc-only clamp is untouched either way — it is still in
    // force for every subsequent start, and the fence below is still the thing
    // that stops a clamped session reaching step 6.
    if (HaltAfterStepLocked(
            5, "the mTLS rpc listener the app dials — a loopback socket in this "
               "process; the machine's routes and dns are still exactly as they "
               "were"))
      return Status();

    // === THE FENCE ==========================================================
    // Everything above this line is inert with respect to the machine's
    // network. Step 6, below, is the first call that rewrites routes and DNS.
    //
    // In rpc-only mode the sequence ends HERE, by returning out of the
    // function — not by skipping a block, not by a conditional wrapped around
    // the destructive steps, and not by a flag consulted further down. There is
    // no control path from this point to step 6 in this mode.
    if (rpcOnly) {
      SetStateLocked(proto::TunnelState::RpcOnly);
      upSinceMillis_ = NowMillis();
      EgressInterfaces bound = egress_->Current();
      LogInfo("tunnel: RPC-ONLY UP in {}ms (rpc={} egress_v4_ifindex={}). Steps "
              "6/8 (routes+dns), 7/8 (split tunnel) and 8/8 (packet pump) were "
              "NOT run: no route, no dns entry and no address were written, and "
              "no active marker was set. The machine's network is untouched and "
              "no traffic is carried.",
              upSinceMillis_ - startedAtMillis, rpcHostPort_, bound.index4);
      return Status();
    }

    // False means --stop-after halted inside steps 6-8 and the teardown has
    // already run. Returning here rather than falling through is what keeps a
    // halted session from being reported Up.
    if (!BringUpTunnelLocked(config, step)) return Status();

    SetStateLocked(proto::TunnelState::Up);
    upSinceMillis_ = NowMillis();
    EgressInterfaces bound = egress_->Current();
    LogInfo("tunnel: UP in {}ms (rpc={} egress_v4_ifindex={} split_tunnel={})",
            upSinceMillis_ - startedAtMillis, rpcHostPort_, bound.index4,
            splitTunnel_.IsAvailable() ? "driver" : "none");

    // --- the tunnel is now the machine's only path: start watching it --------
    //
    // HERE, and not one step earlier. Everything before this line is a bring-up
    // that has its own failure handling; from this line on there is no caller
    // waiting on anything, no timer, and — until this — nothing in the whole
    // service that would ever look at the tunnel again. That absence is the
    // bug: 31 capture routes and a firewall that blocks every other path, held
    // by a session nobody re-examines.
    //
    // The watchdog is given a SHARE of the pump's counters rather than the pump
    // (which a bounded teardown may abandon) and a raw device pointer with the
    // same contract WindowTrace has: stopped at the top of StopLocked, before
    // anything touches device_.
    deadTunnelWatchdog_.Start(&*device_, pump_ ? pump_->Counters() : nullptr,
                    [this](DeadTunnelReason reason) { FailsafeStop(reason); });
  } catch (const std::exception& e) {
    error_ = e.what();
    SetStateLocked(proto::TunnelState::Error);
    LogError("tunnel: start FAILED at step {} (mode={}): {}", step,
             proto::ToString(startMode_), error_);
    // finalDisarm=false: a failed start is not a user disconnect. With the kill
    // switch on, the policy stays Armed so a start that died halfway does not
    // hand the machine back to the clear.
    StopLocked(/*finalDisarm=*/false);
    SetStateLocked(proto::TunnelState::Error);  // StopLocked resets to Stopped
  }

  return Status();
}

// Steps 6-8 — the destructive half. Called from exactly one place, immediately
// after the fence in StartLocked. Caller holds mutex_.
//
// Returns false when --stop-after ended the sequence at one of these three
// steps; the teardown has already run by then, so the caller must return rather
// than report the session up.
bool TunnelController::BringUpTunnelLocked(const proto::StartTunnel& config,
                                           const char*& step) {
  // The second, independent gate. StartLocked already returned before reaching
  // this call in rpc-only mode; this checks the STORED mode rather than the
  // caller's argument, so a future caller that reaches here with the wrong mode
  // throws instead of rewriting the route table. It is not reachable today, and
  // that is the point: this function must be impossible to misuse, not merely
  // unused incorrectly.
  if (startMode_ != proto::StartMode::Tunnel) {
    throw std::runtime_error(
        "refusing to apply network settings: session mode is '" +
        std::string(proto::ToString(startMode_)) + "', not 'tunnel'");
  }
  // Steps 6 and 8 dereference the adapter. It only exists if step 1 ran, which
  // only happens in tunnel mode — belt to the braces above, and it makes the
  // dependency explicit rather than a latent null deref.
  if (!adapter_)
    throw std::runtime_error(
        "refusing to apply network settings: no wintun adapter (step 1 did not "
        "run)");

  // --- IPv6-only refusal, BEFORE anything is written ------------------------
  //
  // Our tunnel is IPv4-only, so protecting the user means blocking IPv6 at the
  // two v6 ALE layers. On an IPv6-only access network (NAT64/DNS64, or a
  // Windows 11 CLAT synthesising v4 over v6) that does not degrade the user, it
  // DISCONNECTS them — including from our own providers, so the client cannot
  // even recover. Detect and refuse with a message that names the cause, rather
  // than block into a dead end and let it look like our bug.
  //
  // EgressInterfaces::index4 is the signal and it is already computed: step 2/8
  // logs when it is 0. Here it is load-bearing rather than advisory.
  {
    const EgressInterfaces egress = egress_ ? egress_->Current()
                                            : NetworkConfig::DiscoverEgress(adapter_->Luid());
    if (egress.index4 == 0) {
      if (egress.index6 != 0) {
        throw std::runtime_error(
            "this network is IPv6-only (no IPv4 default route, but an IPv6 one "
            "exists). The tunnel carries IPv4 only, so connecting would mean "
            "blocking IPv6 — which on this network would cut the machine off "
            "entirely, including from our own providers. Refusing to connect "
            "rather than leave you with no network and no cause.");
      }
      throw std::runtime_error(
          "no usable network: there is no IPv4 default route and no IPv6 one "
          "either. Nothing to tunnel over.");
    }
  }

  // --- arm the leak-prevention layer BEFORE the first route -----------------
  //
  // Ordering is the point. Once step 6 installs routes the host's traffic is
  // being redirected, and if the firewall went up afterwards there would be a
  // window in which the tun is authoritative but IPv6 and other adapters' DNS
  // are still wide open. Arming here closes it.
  //
  // Deliberately NOT the FIRST installation: on a start that begins with the
  // policy Off (kill switch off, or a first connect after a deliberate stop),
  // steps 1-5 bring the SDK up and establish the platform connection, and
  // installing anything before them would block the APP's own account/auth
  // traffic for the whole of a slow connect. The window that leaves open is
  // "connecting, before any route exists", where the machine is exactly as
  // exposed as it was a second earlier — no worse.
  //
  // A start that begins ARMED is the other case, and it is not this one: there
  // the policy is already installed, and StartLocked has already widened it to
  // Connecting at the top so steps 3-5 can resolve. See the block there.
  //
  // A failure here is FATAL when the kill switch is on (the user asked for a
  // guarantee we cannot give) and non-fatal when it is off (leak prevention is
  // still worth having, but it is not what they asked for and a hard failure
  // would just mean no VPN at all).
  //
  // CONNECTING, not Armed. The attempt is still in flight here — the routes are
  // not installed, the tun carries nothing, and the SDK is still resolving and
  // dialling — so this is the state that has to carry the DNS path. Applying
  // Armed here would slam the window shut in the middle of the attempt that
  // opened it, and on a first connect (kill switch off, policy Off until now) it
  // would be the only state the attempt ever ran under. For a reconnect this is
  // usually a no-op: StartLocked already widened to Connecting at the top.
  step = "6/8 firewall policy";
  if (!ApplyWfpLocked(WfpState::Connecting)) {
    const std::string why = wfp_.LastError();
    if (killSwitch_.load()) {
      throw std::runtime_error(
          "the kill switch is on but the leak-prevention firewall could not be "
          "installed (" + why + "); refusing to connect rather than report a "
          "protection that is not in force");
    }
    LogError("tunnel: [6/8] leak-prevention firewall NOT installed ({}). The "
             "tunnel will still come up, but IPv6 and other adapters' resolvers "
             "are NOT blocked — R6 and R7 are open for this session. Reported "
             "to the app as wfp_state=off.",
             why);
  }

  // --- 6/8 network settings (address/MTU/routes/DNS), from the device ---
  step = "6/8 network config";
  TunnelNetworkSettings settings;
  settings.local_address_v4 = device_->tunnelLocalAddress();
  if (settings.local_address_v4.empty()) settings.local_address_v4 = "169.254.2.1";
  settings.prefix_v4 = 24;
  settings.mtu = kTunnelMtu;
  // dns from the device: the dns settings' unencrypted local servers when set,
  // otherwise the distinct plain-DNS UpgradeMux mask. always plain :53, never OS-level
  // encrypted DNS: the mux performs the unencrypted-DNS -> DoH upgrade in-tunnel.
  // the tunnel is ipv4-only, so only the ipv4 resolvers apply
  if (auto dns = device_->tunnelDnsAddressesIpv4(); dns && !dns->empty()) {
    settings.dns_servers = *dns;
  } else {
    // Keep the exceptional fallback coupled to the SDK's separately tested
    // URnetwork-owned UpgradeMux identity.
    settings.dns_servers = {urnet::getDefaultTunnelDnsAddressIpv4()};
  }
  LogInfo("tunnel: [6/8] applying network settings addr={}/{} mtu={} dns=[{}]",
          settings.local_address_v4, settings.prefix_v4, settings.mtu,
          Join(settings.dns_servers));
  netConfig_ = std::make_unique<NetworkConfig>(adapter_->Luid());
  // Mark the machine as "routes installed" BEFORE installing them. The next
  // start reads this to tell an orderly shutdown from a crash; a marker left
  // by a run that died between the two is exactly the case we want reported.
  SetActiveMarker(true);
  if (!netConfig_->Apply(settings)) throw std::runtime_error("network config failed");
  appliedResolvers_ = settings.dns_servers;

  // Routes are in. Widen the policy from Armed to Connected: the tun's LUID and
  // the tunnel's own resolvers become permitted. Doing this AFTER Apply is
  // deliberate — permitting the tun before it has an address permits nothing,
  // and permitting a resolver we then failed to set would be a claim we cannot
  // back.
  //
  // If the DNS half failed, say so here too. With the port-53 block in force
  // the consequence is not a leak but a total DNS outage, which is the safer
  // failure and still one the user has to be told about (TunnelStatus::
  // dns_applied carries it to the app).
  if (wfp_.State() != WfpState::Off && !ApplyWfpLocked(WfpState::Connected)) {
    LogError("tunnel: [6/8] could not widen the firewall policy to connected "
             "({}). The armed policy is still in force, which means the tun "
             "itself is blocked — stopping rather than serving a tunnel that "
             "cannot carry traffic.",
             wfp_.LastError());
    throw std::runtime_error("firewall policy could not follow the tunnel up: " +
                             wfp_.LastError());
  }

  // --- THE Connecting -> Connected EDGE: flush the OS resolver cache ---------
  //
  // Everything the machine resolved during the connecting window went out
  // through the HOST's resolvers, over the physical NIC, in plaintext — that is
  // what filter 9b permits, and it is machine-wide because the query leaves
  // svchost.exe rather than this process. Those ANSWERS sit in the one
  // machine-wide Dnscache and are served to every process for the rest of their
  // TTL. Setting the tun's resolvers a few lines above changed where the next
  // QUERY goes and touched none of them.
  //
  // So without this line "DNS is pinned to the tunnel's resolvers while
  // connected" is true of queries and false of answers, and the gap is exactly
  // as long as the longest TTL the connecting window happened to pick up. Flush
  // it here, at the instant the policy becomes Connected, for the same reason
  // WireGuard and Mullvad both flush.
  //
  // Failure is logged inside and IGNORED here: a stale cache is not worth
  // failing a tunnel that is otherwise up and correct.
  NetworkConfig::FlushResolverCache();

  if (!netConfig_->DnsApplied()) {
    LogError("tunnel: [6/8] the tunnel is up but its DNS was NOT applied. "
             "{} Reporting dns_applied=false.",
             wfp_.State() == WfpState::Off
                 ? "The firewall is not in force either, so queries go to the "
                   "physical adapter's resolver IN THE CLEAR (R6)."
                 : "The firewall blocks every resolver except the tunnel's, and "
                   "no adapter points at one, so name resolution will fail "
                   "closed.");
  }

  // THE STOP POINT THAT MATTERS. Everything labelled 6/8 has now run — the
  // firewall went to Connecting, the address, MTU, 31 routes and DNS were
  // applied, the policy widened to Connected, and the resolver cache was
  // flushed. This machine is redirected. Stopping here and unwinding is Gate D:
  // "routes, then immediate revert", without ever starting the pump.
  if (HaltAfterStepLocked(
          6, "THIS MACHINE'S ROUTES AND DNS REWRITTEN — the tun has its address "
             "and mtu, the 31 capture routes point at it, its resolvers are set, "
             "and the leak-prevention policy is CONNECTED. This is the first "
             "step that changed anything outside this process"))
    return false;

  // --- 7/8 split tunneling (driver optional) ---
  step = "7/8 split tunnel";
  LogInfo("tunnel: [7/8] split tunnel: {} path(s), {} mode",
          config.excluded_app_paths.size(),
          config.allowlist_mode ? "allowlist" : "denylist");
  splitTunnel_.Open();
  excludedPaths_ = config.excluded_app_paths;
  allowlist_ = config.allowlist_mode;
  PushExcludedToDriver(excludedPaths_, allowlist_);
  if (HaltAfterStepLocked(
          7, "the routes and dns of step 6 PLUS the split-tunnel driver's app "
             "rules, if the driver is present. No packet has moved: the pump has "
             "not started, so the tun is authoritative and silent"))
    return false;

  // --- 8/8 packet pump ---
  step = "8/8 pump";
  LogInfo("tunnel: [8/8] starting the packet pump");
  pump_ = std::make_unique<PacketPump>(*adapter_, *device_);
  if (!pump_->Start()) throw std::runtime_error("packet pump failed to start");
  if (HaltAfterStepLocked(
          8, "a COMPLETE tunnel — routes, dns, split tunnel and a running packet "
             "pump. --stop-after=8 is the smoke test: it brings the whole thing "
             "up and takes it straight back down"))
    return false;

  return true;
}

void TunnelController::Stop() {
  // Whoever called the public Stop() is a person or their agent: the app's
  // Disconnect, the SCM, the console handler, Logout. None of them is the
  // failsafe, which has its own entry point — so the app can tell "you turned
  // it off" from "it turned itself off", which is the only framing that makes
  // an automatic teardown acceptable rather than alarming.
  lastStopReason_.store(kStopReasonUser);
  // TIMED, not blocking. The class's own connecting-watchdog note (see the
  // header) already establishes that a connect attempt wedged inside the SDK
  // holds mutex_ for as long as the process lives. Stop() used to take that lock
  // unconditionally, which meant the one operation the operator cannot be denied
  // — turning the VPN off — was gated behind the one hang the design already
  // admits it cannot prevent. It would not have logged, and it would not have
  // reverted a single route.
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (lock.try_lock_for(kStopLockBudget)) {
    StopLocked(/*finalDisarm=*/true);
    return;
  }

  LogError("tunnel: could not take the session lock within {}ms — another "
           "operation (almost certainly a connect attempt) is wedged inside the "
           "sdk and is never going to give it back. Reverting this machine's "
           "ROUTES WITHOUT THE LOCK and abandoning the orderly teardown: the "
           "operator asked for the tunnel off, and a stop that waits forever for "
           "a lock is a machine with no network and no explanation.",
           kStopLockBudget.count());
  // CrashRevert is the right tool and the ONLY one available here. It takes no
  // lock of this class, allocates nothing, and issues route ioctls only — see
  // NetworkConfig.h for why it deliberately skips the DNS clear (an RPC to
  // dnscache that can block, which is exactly what must not happen on a path
  // taken because something else already blocked).
  //
  // What is NOT done here, and why it is still safe: the tun's DNS settings are
  // left in place. They die with the process — wintun never calls
  // SwDeviceSetLifetime, so process exit is a PnP surprise removal that takes
  // the adapter and every route pointed at it — and NoteTeardownAbandoned()
  // below makes that process exit happen by TerminateProcess rather than by
  // hoping.
  NetworkConfig::CrashRevert();
  SetActiveMarker(false);
  DropFirewallOnEscape("stop");
  NoteTeardownAbandoned();
}

// The lock-free half of "give this machine back", shared by Stop()'s timeout
// path and FailsafeStop()'s.
//
// CrashRevert deliberately has no WFP path (NetworkConfig.h) — it issues route
// ioctls only, because it is also the console handler's floor and must not call
// anything that can block. But the routes are only half the block: with the
// policy still Connected, every non-tun path stays blocked and the machine has
// no internet even though nothing is routed to the tun any more. Waiting for
// process death to drop the filters is good enough for a crash and NOT good
// enough for an operation whose entire purpose is to give the internet back.
//
// Legal from here, and already proven in this file: WfpPolicy is internally
// synchronised precisely so an off-thread caller can narrow while a connect is
// wedged, which is exactly what the connecting watchdog does.
//
// Gated on the kill switch, and that gate is the whole product decision. With
// it ON the user asked to stay blocked when the tunnel is not up, and a
// timeout is not permission to change that answer for them.
void TunnelController::DropFirewallOnEscape(const char* who) {
  if (killSwitch_.load()) {
    LogWarn("tunnel: [{}] the KILL SWITCH IS ON, so the firewall policy is "
            "deliberately LEFT IN FORCE on this lock-free path: this machine "
            "stays blocked, which is what the setting promises. Turn the kill "
            "switch off (or stop urnetworkd — the policy dies with the process) "
            "to lift it.",
            who);
    return;
  }
  if (wfp_.State() == WfpState::Off) return;
  LogWarn("tunnel: [{}] dropping the leak-prevention firewall WITHOUT THE LOCK. "
          "The routes are already reverted, so leaving the policy installed "
          "would leave this machine blocked with nothing to blame it on — and "
          "the kill switch is off, which means fail OPEN is the documented "
          "default. Traffic falls back to the physical adapter in the clear.",
          who);
  wfp_.Revert();
}

// --- the dead-tunnel failsafe ----------------------------------------------
//
// See the contract in the header. This runs on TunnelWatchdog's evaluator
// thread, which has already logged WHY; what is added here is what is being
// done about it and what the machine is left in.
void TunnelController::FailsafeStop(DeadTunnelReason reason) {
  // Recorded BEFORE the teardown, so every status pushed during it — including
  // the Stopping transition — already carries the reason. An app that learns
  // "stopped" first and "why" second renders the alarming half alone.
  lastStopReason_.store(StopReasonOf(reason));
  const bool killSwitchOn = killSwitch_.load();
  const bool finalDisarm = FailsafeFinalDisarm(killSwitchOn);

  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (lock.try_lock_for(kStopLockBudget)) {
    // THE ORDINARY TEARDOWN. Phase 1 hands the machine back — routes, tun DNS,
    // the resolver cache, the marker, the policy — before phase 2 touches
    // anything that can block on a network that has already failed. That
    // two-phase order is not repeated here; it is inherited, which is the point
    // of there being no second teardown path.
    StopLocked(finalDisarm);
    lock.unlock();
    LogWarn("tunnel: the failsafe teardown is complete. {} No reconnection is "
            "attempted and none will be: the next attempt is the user's, which "
            "is what makes this impossible to thrash.",
            killSwitchOn
                ? "The KILL SWITCH IS ON, so the firewall narrowed to ARMED and "
                  "this machine is still blocked — deliberately, and nothing is "
                  "leaking. Turning the kill switch off lifts it immediately."
                : "The kill switch is off, so the firewall was lifted and this "
                  "machine's internet is back, unprotected, exactly as it is "
                  "after a user disconnect.");
    NotifyStateChanged();
    return;
  }

  // THE LOCK-FREE ESCAPE. A connect attempt wedged inside the SDK holds mutex_
  // for the life of the process, and "this tunnel is blocking the machine" is
  // the one verdict that cannot be made conditional on acquiring it.
  LogError("tunnel: the failsafe could not take the session lock within {}ms — "
           "something (almost certainly a connect attempt) is wedged inside the "
           "sdk and will never give it back. Reverting this machine's ROUTES "
           "WITHOUT THE LOCK and abandoning the orderly teardown.",
           kStopLockBudget.count());
  deadTunnelWatchdog_.Cancel();  // non-blocking: there is no teardown to hang this off
  NetworkConfig::CrashRevert();
  SetActiveMarker(false);
  DropFirewallOnEscape("failsafe");
  NoteTeardownAbandoned();
  NotifyStateChanged();
}

void TunnelController::SetOnStateChanged(std::function<void()> handler) {
  std::scoped_lock lock(stateChangedMutex_);
  onStateChanged_ = std::move(handler);
}

void TunnelController::NotifyStateChanged() {
  std::function<void()> handler;
  {
    std::scoped_lock lock(stateChangedMutex_);
    handler = onStateChanged_;
  }
  // Outside both locks by construction: the handler reads Status(), which takes
  // mutex_, and it writes to the control pipe, which can block on a client that
  // is not reading.
  if (handler) handler();
}

void TunnelController::StopLocked(bool finalDisarm) {
  const proto::TunnelState priorState = state_;
  const bool wasRunning = priorState != proto::TunnelState::Stopped;
  // Whether THIS teardown has routes to give back. netConfig_ exists only if
  // step 6 ran, which only happens in tunnel mode — so it is also the honest
  // answer to "was the machine's network touched", and it is read from state
  // rather than from the mode flag.
  const bool hadRoutes = netConfig_ != nullptr;
  if (proto::IsSessionLive(state_) || state_ == proto::TunnelState::Starting)
    SetStateLocked(proto::TunnelState::Stopping);
  if (wasRunning) LogInfo("tunnel: stopping (was {})", proto::ToString(priorState));

  // BEFORE EITHER PHASE, AND BEFORE ANYTHING TOUCHES device_. The trace thread
  // holds a raw DeviceLocal* and calls into it every tick; phase 2 closes and
  // destroys that object. Stop() joins, so on the far side of this line no other
  // thread is inside the SDK on our behalf. It is a no-op when nothing was
  // started, which is the normal case.
  trace_.Stop();
  // Same contract, same reason, and BOUNDED — unlike the trace, the watchdog is
  // always on, so it may not become a new way for a stop to hang. Its Stop()
  // gives the SDK sampler kWatchdogJoinBudgetMillis and abandons it after that
  // (TunnelWatchdog.h spells out why abandoning is safe). It also recognises
  // being called from its own evaluator thread — which it always is when the
  // failsafe is what got us here — and detaches instead of joining itself.
  deadTunnelWatchdog_.Stop();

  // --- THE ORDER OF THE UNWIND, WHICH IS THE FIX ----------------------------
  //
  // PHASE 1 gives the MACHINE back: routes, DNS, resolver cache, marker,
  // firewall policy. All local, all cheap (133 ms for the whole thing, measured
  // four times over), none of it able to block on a network that has already
  // failed.
  //
  // PHASE 2 tears down the SDK and the native plumbing. Any of it can block
  // indefinitely — DeviceLocal::close() unwinding wedged transports is the
  // documented suspect, and a pump send stuck in the SDK is the proven one.
  //
  // These used to run in the opposite order, and that is precisely how a
  // machine ends up with thirty-one capture routes pointed at a dead tunnel for
  // eighty seconds while its owner presses Ctrl+C sixteen times. The old code
  // even carried a comment saying the routes were "the thing to give back
  // soonest" — and then sequenced them behind the packet pump, which is the
  // component most likely to be stuck when the tunnel is broken.
  //
  // Reverting first costs nothing in correctness. For the short window before
  // phase 2 finishes, the pump may still move a packet across a tun that no
  // route points at: the host stack has nothing to send it and drops what
  // arrives. The exposure is identical to the moment after Revert() in the old
  // order — it is simply reached sooner, which is the entire point.
  RevertMachineStateLocked(finalDisarm, hadRoutes);

  const bool tornDown = TearDownSessionLocked();

  rpcHostPort_.clear();
  upSinceMillis_ = 0;
  SetStateLocked(proto::TunnelState::Stopped);
  // Do NOT say "network restored" when nothing was ever changed: an rpc-only
  // session that claims to have restored the network is a claim the reader
  // would use to rule out a network problem this service did not cause.
  if (wasRunning)
    LogInfo("tunnel: stopped, {}{}",
            hadRoutes ? "network restored"
                      : "no network state to restore (nothing was applied)",
            tornDown ? "" : " (SDK TEARDOWN ABANDONED — see above)");
}

void TunnelController::RevertMachineStateLocked(bool finalDisarm, bool hadRoutes) {
  if (netConfig_) { netConfig_->Revert(); netConfig_.reset(); }
  appliedResolvers_.clear();
  // --- THE OTHER EDGE: Connected -> Armed/Off --------------------------------
  //
  // The mirror of the flush at the Connected edge, and it is not symmetry for
  // its own sake. Every answer in the machine-wide cache right now was resolved
  // THROUGH THE TUNNEL, by the exit's resolvers. Left there, those answers are
  // served to every process on this box after the tunnel is gone: a
  // geo-steered or split-horizon record now points the user's traffic somewhere
  // it was only ever meant to go from the exit, and an address that is only
  // reachable through the tunnel simply fails in a way that looks like a
  // network fault.
  //
  // Gated on hadRoutes rather than on the mode flag, for StopLocked's own
  // reason: it is the honest answer to "did this session point the machine at
  // the tun". An rpc-only session never sets a resolver, so it has nothing to
  // give back and must flush nothing — that mode's promise is that it does not
  // touch this machine, and the DNS cache is this machine's.
  if (hadRoutes) NetworkConfig::FlushResolverCache();
  // Routes are gone; the marker's job is done whether or not the rest unwinds.
  SetActiveMarker(false);

  // --- the firewall policy, and what the kill switch actually decides -------
  //
  // The routes have just gone back. THIS is the moment the machine falls to the
  // clear, and it is the only moment the kill switch has an opinion about.
  //
  //   * finalDisarm (user disconnect, service shutdown): policy Off. The user
  //     asked to stop; leaving them blocked with no UI to explain it is the
  //     "machine with no network and no obvious cause" failure this whole
  //     design exists to avoid.
  //   * otherwise (reconnect, failed start): with the kill switch ON the policy
  //     narrows from Connected back to Armed and STAYS THERE across the gap —
  //     or, if there was no policy in force to narrow, is INSTALLED as Armed
  //     here for the first time (see that branch; it is why "kill switch on,
  //     policy off" stopped being an unprotected drop nothing ever retried).
  //     That gap — routes gone, next session not yet up — is the case a kill
  //     switch exists for, and it is the case setRouteLocal structurally cannot
  //     cover because there is no tun for it to drop packets from.
  //
  // Deliberately NOT implemented: Mullvad's "lockdown"/Proton's "advanced kill
  // switch", where a DELIBERATE disconnect also stays blocked. The research
  // note lists that as an arming trigger, but it conflates two different
  // products: a kill switch (block on an unexpected drop) and lockdown (block
  // whenever not connected). Lockdown needs its own separately-worded toggle
  // and its own UI, neither of which exists yet, and it cannot be verified on
  // an unelevated machine. Shipping it silently behind the existing toggle
  // would surprise a user who turned on something described as a kill switch.
  if (finalDisarm) {
    if (wfp_.State() != WfpState::Off)
      LogInfo("tunnel: lifting the leak-prevention firewall (session ended{})",
              killSwitch_.load()
                  ? ", kill switch on but this was a deliberate stop"
                  : "");
    ApplyWfpLocked(WfpState::Off);
  } else if (killSwitch_.load() && wfp_.State() != WfpState::Off) {
    // ALSO THE CLOSE OF THE CONNECTING WINDOW. This runs on the failed-start and
    // reconnect paths, so it is where an attempt that opened the machine-wide
    // DNS permit gives it back. Narrowing Connecting -> Armed only ever removes
    // filter 9b, so nothing that was permitted in both states is interrupted.
    LogWarn("tunnel: the tunnel is down and the KILL SWITCH IS ON — holding the "
            "firewall in the armed state, so nothing leaves this machine except "
            "our own service, loopback, the LAN, DHCP and NDP until the tunnel "
            "is back. NAME RESOLUTION IS PART OF 'nothing': armed carries no DNS "
            "permit, so no process on this machine resolves until a connection "
            "attempt reopens the window. `sc stop urnetworkd` lifts it (the "
            "policy dies with the process).");
    ApplyWfpLocked(WfpState::Armed);
  } else if (killSwitch_.load() && hadRoutes) {
    // THE SECOND WAY INTO ARMED, and the one that was missing.
    //
    // Every branch above requires a policy to ALREADY be installed
    // (wfp_.State() != Off), so "kill switch on, policy off, tunnel dropping"
    // fell through all of them and the drop went unprotected — with nothing that
    // would ever retry, because the next start would find the policy off too.
    //
    // That state is reachable and not exotic. Step 6/8's install is deliberately
    // NON-FATAL when the switch is off (a firewall hiccup should not mean no VPN
    // at all), so a session can be up and carrying traffic with the policy off;
    // the user then turns the kill switch on mid-session. SetKillSwitch retries
    // the install immediately for exactly that case, but the retry can fail too
    // — unelevated, it always does — and this is the backstop for when it did.
    //
    // STILL SOFT. hadRoutes is what keeps it soft: it arms only when THIS
    // teardown is giving a real tunnel's routes back, i.e. only on a drop,
    // failed start or reconnect of a session that was actually carrying the
    // machine's traffic. A deliberate Stop takes the finalDisarm branch above
    // and lifts everything; a machine that has never connected installs nothing;
    // an rpc-only session has no routes and so arms nothing. This is not
    // lockdown, and it must not become it.
    LogWarn("tunnel: the tunnel is down, the KILL SWITCH IS ON, and no firewall "
            "policy was in force to narrow — the install failed earlier this "
            "session, or the switch was turned on after it did. Installing the "
            "ARMED policy now rather than handing the machine to the clear: this "
            "drop is the exact case the switch exists for. If this cannot be "
            "installed either (it needs LocalSystem or elevation) the machine "
            "fails OPEN and the app is told wfp_state=off.");
    ApplyWfpLocked(WfpState::Armed);
  } else if (wfp_.State() != WfpState::Off) {
    LogInfo("tunnel: the tunnel is down and the kill switch is off — lifting "
            "the firewall; traffic falls back to the physical adapter in the "
            "clear, which is the documented fail-open default");
    ApplyWfpLocked(WfpState::Off);
  }
  if (hadRoutes)
    LogInfo("tunnel: this machine's network is BACK — routes reverted, tun dns "
            "cleared, resolver cache flushed, firewall policy {}. Everything "
            "below this line is releasing our own objects and cannot cost the "
            "operator their network, however long it takes.",
            ToString(wfp_.State()));
}

// --- phase 2: our own objects, on a budget ---------------------------------
//
// Everything unwound here is OURS. None of it is state on the operator's
// machine; phase 1 already gave all of that back. That is what makes it
// acceptable to give this phase a deadline and walk away from it.
bool TunnelController::TearDownSessionLocked() {
  // Nothing to do — and, importantly, no thread to spawn — for the common case
  // of a Stop with no session (idempotent restart, a second Stop, ~TunnelController
  // after a clean stop).
  if (!pump_ && !egress_ && !device_ && !adapter_ && !wintun_ && !networkSpace_ &&
      !splitTunnel_.IsAvailable())
    return true;

  // Drop the egress change handler FIRST and on THIS thread. It is a lambda that
  // captures `this`; the monitor is about to be owned by a worker that may
  // outlive the call, and a change notification firing into a destroyed
  // controller is a use-after-free in a LocalSystem service. Clearing it here
  // means the monitor a worker inherits can only ever unregister itself.
  if (egress_) {
    egress_->SetOnChange(nullptr);
    egress_->SetOnNetworkEvent(nullptr);
  }

  // Take splitMutex_ only to MOVE the client out, never across the close. The
  // existing hazard note still applies — the egress callback wants this lock and
  // egress_->Stop() waits for that callback — and moving rather than closing
  // under the lock makes it structurally impossible to get wrong: after this
  // scope the member is empty, so a late callback finds an unavailable driver
  // and no-ops.
  SplitTunnelClient split;
  {
    std::scoped_lock lock(splitMutex_);
    split = std::move(splitTunnel_);
  }

  // Hand EVERY per-session object to the worker by move. This is the ownership
  // contract from StopBudget.h, and it is the reason abandoning the worker is
  // safe rather than a crash waiting to happen: after these moves this object
  // holds no pointer to anything the worker touches, and the worker holds no
  // pointer to anything this object owns. Nothing to race, nothing to free
  // twice, nothing to free too early.
  //
  // Moved into locals and then RESET, rather than moved straight into the
  // capture list, because two of these are std::optional. Moving an engaged
  // optional leaves the SOURCE ENGAGED holding a moved-from value — so
  // `device_` would still test true afterwards, and StartLocked's `device_ =
  // ...` and every `if (device_)` in this file would be reasoning about a
  // handle that is really 0. The unique_ptrs null themselves; the resets are
  // written for all of them so the rule is uniform and nobody has to remember
  // which is which.
  auto pump = std::move(pump_);
  pump_.reset();
  auto egress = std::move(egress_);
  egress_.reset();
  auto device = std::move(device_);
  device_.reset();
  auto adapter = std::move(adapter_);
  adapter_.reset();
  auto wintun = std::move(wintun_);
  wintun_.reset();
  auto space = std::move(networkSpace_);  // spaceManager_ persists across sessions
  networkSpace_.reset();

  // The order INSIDE the lambda is the old reverse-dependency order, unchanged,
  // because the dependencies are unchanged:
  //   pump before adapter/device  — the outbound thread holds references to both
  //   split before egress         — a late change callback must not find a
  //                                 closed driver handle
  //   device before adapter       — the receive path can still target the ring
  //   adapter before wintun       — ~WintunAdapter calls back into the loaded DLL
  const auto started = std::chrono::steady_clock::now();
  const bool finished = RunBounded(
      kSdkTeardownBudget,
      [pump = std::move(pump), split = std::move(split),
       egress = std::move(egress), device = std::move(device),
       adapter = std::move(adapter), wintun = std::move(wintun),
       space = std::move(space)]() mutable {
        if (pump) pump->Stop();
        pump.reset();
        split.Close();
        // After this returns no further change callbacks can run.
        if (egress) egress->Stop();
        egress.reset();
        // Reset the egress binding so a later non-tunnel run isn't pinned to a
        // stale nic.
        urnet::setEgressInterfaceIndex(0, 0);
        if (device) {
          LogInfo("tunnel: closing the device (this is the call that can block "
                  "on wedged transports)");
          device->close();
        }
        device.reset();
        adapter.reset();
        wintun.reset();
        space.reset();
        LogInfo("tunnel: sdk teardown complete");
      });

  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();
  if (finished) {
    if (ms > 500)
      LogWarn("tunnel: the sdk teardown took {}ms (budget {}ms)", ms,
              kSdkTeardownBudget.count());
    return true;
  }

  LogError(
      "tunnel: ======== SDK TEARDOWN ABANDONED after {}ms ======== it did not "
      "finish inside its {}ms budget, so it has been LEFT RUNNING on its own "
      "thread rather than waited on. This is the wedge case: a packet send or "
      "DeviceLocal::close() that the sdk will not complete because every "
      "transport is down. THIS MACHINE'S NETWORK IS ALREADY BACK — routes, dns "
      "and the firewall policy were reverted before this phase started, and "
      "nothing left running here can undo that. What is still held is ours "
      "alone: the device, the wintun adapter and the packet pump, on a thread "
      "that owns them outright. This process will now exit by TerminateProcess "
      "instead of unwinding, which takes the adapter with it (a pnp surprise "
      "removal) and the filter policy with the dynamic session.",
      ms, kSdkTeardownBudget.count());
  return false;
}

// --- crash/orderly-exit bookkeeping ---------------------------------------
//
// This marker does NOT restore anything. The restore path is the wintun adapter
// dying with the process (see NetworkConfig.h). The marker exists so the next
// start can SAY that the last one ended badly, instead of the owner having to
// infer it — and so the startup sweep has a reason to shout.

std::filesystem::path TunnelController::ActiveMarkerPath() {
  return StorageRoot(/*isService=*/true) / L"tunnel_active";
}

void TunnelController::SetActiveMarker(bool active) {
  std::error_code ec;
  if (active) {
    std::ofstream f(ActiveMarkerPath(), std::ios::trunc);
    if (f) f << ::GetCurrentProcessId() << "\n";
  } else {
    std::filesystem::remove(ActiveMarkerPath(), ec);
  }
}

bool TunnelController::TakeActiveMarker() {
  std::error_code ec;
  if (!std::filesystem::exists(ActiveMarkerPath(), ec)) return false;
  std::filesystem::remove(ActiveMarkerPath(), ec);
  return true;
}

bool TunnelController::PeekActiveMarker() {
  std::error_code ec;
  return std::filesystem::exists(ActiveMarkerPath(), ec);
}

void TunnelController::PushPhysicalAddressesLocked(const EgressInterfaces& egress) {
  uint8_t addr4[4] = {0};
  uint8_t addr6[16] = {0};
  bool has4 = egress.index4 != 0 &&
              NetworkConfig::InterfaceSourceAddress(egress.index4, AF_INET, addr4);
  bool has6 = egress.index6 != 0 &&
              NetworkConfig::InterfaceSourceAddress(egress.index6, AF_INET6, addr6);
  splitTunnel_.SetPhysicalAddresses(has4 ? egress.index4 : 0, has4 ? addr4 : nullptr,
                                    has6 ? egress.index6 : 0, has6 ? addr6 : nullptr);
}

// The egress interface moved (Wi-Fi -> ethernet, DHCP renew, resume). The SDK
// followed it in EgressMonitor::Refresh; the driver has to follow it here, or
// every excluded app keeps being rebound to the source address of the adapter
// we just left — which stops working the moment that address is reclaimed.
void TunnelController::OnEgressChanged(EgressInterfaces egress) {
  std::scoped_lock lock(splitMutex_);
  if (!splitTunnel_.IsAvailable()) return;
  LogInfo("split-tunnel: following the egress change, re-binding excluded apps "
          "to v4={} src={} v6={}",
          egress.index4, NetworkConfig::DescribeInterface(egress.index4),
          egress.index6);
  PushPhysicalAddressesLocked(egress);
}

void TunnelController::PushExcludedToDriver(const std::vector<std::string>& paths, bool allowlist) {
  std::scoped_lock lock(splitMutex_);
  if (!splitTunnel_.IsAvailable()) return;
  // The driver rebinds excluded sockets to the physical interface's source
  // address, so resolve the current physical interface + its preferred source.
  // Take it from the monitor rather than rediscovering: the monitor holds the
  // last known good interface across a momentary loss of the default route, and
  // the driver and the sdk must agree on which nic is "physical".
  // A zero LUID when there is no adapter — the rpc-only case, where there is no
  // tun to exclude. Unreachable today (the driver is only ever open in tunnel
  // mode, and IsAvailable() above returns first), but the deref was latent.
  const NET_LUID excludeLuid = adapter_ ? adapter_->Luid() : NET_LUID{};
  EgressInterfaces egress =
      egress_ ? egress_->Current() : NetworkConfig::DiscoverEgress(excludeLuid);
  PushPhysicalAddressesLocked(egress);
  splitTunnel_.SetMode(allowlist);
  splitTunnel_.SetExcludedPaths(paths);
  // Enable whenever there is a rule set. In allowlist mode an empty keep-set would
  // route nothing through the tunnel, so the service only sends allowlist mode with
  // a non-empty set (see SdkHost); either way !empty is the right enable signal.
  splitTunnel_.SetEnabled(!paths.empty());
}

bool TunnelController::SetKillSwitch(bool on) {
  std::scoped_lock lock(mutex_);
  if (killSwitch_.load() == on) return true;
  killSwitch_.store(on);
  LogInfo("tunnel: kill switch {} (firewall policy currently {})",
          on ? "ON" : "off", ToString(wfp_.State()));
  // Two combinations take effect NOW; the rest are decided at the next
  // transition. Turning it OFF while armed-and-disconnected, because the user
  // asking for their network back is the one case where waiting is the wrong
  // answer. Turning it ON over a live tunnel with NO policy installed, because
  // waiting there means never — see the second branch below. While connected
  // WITH a policy the policy is already the full leak fix regardless, and
  // turning it ON while disconnected deliberately does not arm (see StopLocked:
  // that is the line between a kill switch and lockdown).
  // Connecting counts as armed here. It is only reachable from this function on
  // a path that left the window open without a session (an rpc-only fence return
  // with a policy already installed), and in that case the user turning the
  // switch off must get the whole policy lifted, not a narrowing to Armed that
  // leaves them blocked after asking not to be.
  if (!on && state_ != proto::TunnelState::Up &&
      (wfp_.State() == WfpState::Armed ||
       wfp_.State() == WfpState::Connecting)) {
    LogInfo("tunnel: kill switch turned off while armed — lifting the firewall "
            "immediately rather than at the next transition");
    ApplyWfpLocked(WfpState::Off);
    return true;
  }
  // Turning it ON over a LIVE tunnel that has NO policy installed. This is the
  // one combination the "decided at the next transition" rule got wrong, and it
  // got it wrong silently.
  //
  // Step 6/8's install is non-fatal when the switch is off, so a tunnel can be
  // up and carrying traffic with wfp_state=off. Storing the preference and
  // waiting for the next transition then means: nothing is armed now, and when
  // the tunnel DROPS every StopLocked branch that could arm requires a policy to
  // already be installed — so the drop is unprotected and never retried. The
  // user asked for a guarantee and got a boolean.
  //
  // Retry the install NOW instead. Connected, not Armed: the tunnel is up, so
  // Connected is the policy that matches the machine (the tun permitted, DNS
  // pinned to the tunnel's resolvers) and it is what step 6/8 would have
  // installed. StopLocked's new hadRoutes branch remains the backstop for when
  // this fails.
  //
  // A failure here does NOT tear the tunnel down — the user asked for
  // protection, not for a disconnection — but it is reported: this returns
  // false, ControlServer answers ok=false, and the status it pushes carries
  // wfp_state=off, so no surface can claim a guarantee that is not in force.
  if (on && state_ == proto::TunnelState::Up &&
      wfp_.State() == WfpState::Off) {
    LogWarn("tunnel: kill switch turned ON over a live tunnel with NO firewall "
            "policy installed (the step 6/8 install failed earlier this session, "
            "which is non-fatal while the switch is off). Retrying the install "
            "NOW: without it nothing is armed, and the next drop would fall "
            "straight to the clear with no branch able to arm it.");
    if (!ApplyWfpLocked(WfpState::Connected)) {
      LogError("tunnel: the leak-prevention firewall STILL could not be "
               "installed ({}). The tunnel is left up — the user asked for "
               "protection, not for a disconnect — but it is NOT protected: "
               "IPv6 and other adapters' resolvers are open, and reported to "
               "the app as wfp_state=off. It is retried at the next drop.",
               wfp_.LastError());
      return false;
    }
  }
  return true;
}

bool TunnelController::SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist) {
  std::scoped_lock lock(mutex_);
  excludedPaths_ = excludedPaths;
  allowlist_ = allowlist;
  // Only a real tunnel has a split-tunnel driver open; in every other state,
  // including rpc_only, the rules are stored and applied on the next Start.
  if (state_ != proto::TunnelState::Up) return true;
  PushExcludedToDriver(excludedPaths_, allowlist_);
  return true;
}

void TunnelController::Logout() {
  std::scoped_lock lock(mutex_);
  // As deliberate as a disconnect, so it is reported as one. See Stop().
  lastStopReason_.store(kStopReasonUser);
  // finalDisarm: signing out is as deliberate as disconnecting, and there is no
  // session left to protect. Leaving a signed-out machine blocked would be
  // unexplainable from any surface the user still has.
  StopLocked(/*finalDisarm=*/true);
  // Clear persisted device identity so the next login starts clean (mirrors the
  // macOS logout provider message clearing LocalState).
  std::error_code ec;
  std::filesystem::remove(storageDir_ / L"client_key_seed.bin", ec);
  std::filesystem::remove(storageDir_ / L"provide_cert.pem", ec);
  std::filesystem::remove(storageDir_ / L"provide_key.pem", ec);
  LogInfo("tunnel: logged out (cleared device identity)");
}

proto::TunnelStatus TunnelController::Status() {
  proto::TunnelStatus s;
  s.state = state_;
  s.mode = startMode_;
  // Reported from the object that OWNS the routes, not from the mode: it is
  // true exactly while an applied-and-not-yet-reverted NetworkConfig exists,
  // including the window where Apply partially succeeded. The app reads this
  // rather than inferring "connected" from the mode.
  s.routes_installed = netConfig_ != nullptr;
  // Reported from the object that OWNS the resolvers, for the same reason
  // routes_installed is. Two separate facts: the tunnel can carry traffic while
  // its DNS did not take, and that combination used to be invisible.
  s.dns_applied = netConfig_ != nullptr && netConfig_->DnsApplied();
  // "Is leak prevention actually running." Off while the tunnel is up is a
  // materially different state from a protected one — it is what an unelevated
  // or failed install looks like — so the app gets to see it rather than infer
  // protection from `state == up`.
  s.wfp_state = ToString(wfp_.State());
  s.rpc_listen_hostport = rpcHostPort_;
  s.error = error_;
  s.service_version = urnet::version();
  s.protocol_version = proto::kProtocolVersion;
  s.tunnel_local_up_millis = upSinceMillis_ ? (NowMillis() - upSinceMillis_) : 0;
  // Read from the object that OWNS the binding, like routes_installed and
  // dns_applied above, and only while there IS a tunnel. In rpc-only mode the
  // monitor exists and has an index, but nothing has been routed, so the app has
  // no tun to escape and must not pin itself to anything: reporting 0 there is
  // the honest answer, not a missing one.
  if (egress_ && netConfig_ != nullptr) {
    const EgressInterfaces bound = egress_->Current();
    s.egress_index4 = static_cast<int64_t>(bound.index4);
    s.egress_index6 = static_cast<int64_t>(bound.index6);
  }
  // Why the last teardown happened, and whether another one is coming. Both are
  // read from lock-free publishers rather than from session state, so they are
  // still correct on the one path where they matter most: a status served while
  // a failsafe teardown is in flight.
  s.stop_reason = lastStopReason_.load();
  s.failsafe_armed = deadTunnelWatchdog_.FailsafeArmed();
  return s;
}

}  // namespace urnw
