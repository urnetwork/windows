// SPDX-License-Identifier: MPL-2.0
#include "TunnelController.h"

#include <chrono>
#include <fstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>   // AF_INET / AF_INET6
#include <windows.h>

#include "ConsoleArgs.h"  // ClampStopAfterStep — the flag's own bounds
#include "Heartbeat.h"    // the lock-free state mirror the heartbeat reads
#include "Ids.h"
#include "Log.h"
#include "LogUpload.h"   // the log upload's device and its refusals
#include "Paths.h"
#include "ProvideLifecycle.h"  // the provider-only device's refusals
#include "StopBudget.h"   // the shutdown budgets and the abandonable teardown
#include "Strings.h"
#include "ThreadGuard.h"
#include "Version.h"
#include "WintunError.h"

namespace urnw {
namespace {

constexpr DWORD kRingCapacity = 0x400000;  // 4 MiB (power of two, within wintun bounds)

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

// The log upload flight's clock: it bounds how long an upload may run, which a
// wall clock that jumps would misjudge.
int64_t SteadyMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// How long the service's teardown waits for a log upload's thread to come out
// of the sdk's call (the zip): past it the process exits around it.
constexpr std::chrono::milliseconds kLogUploadReturnBudget{2000};

// What the sdk's upload callback needs to end the upload. Owned by the call
// once the sdk took it, freed by the callback.
struct LogUploadReport {
  std::shared_ptr<logupload::Flight> flight;
  int64_t uploadId = 0;
  const char* carrierName = "";
  // tells the standalone device's waiter that the upload reported; empty for
  // the session's and the provider-only device
  std::function<void()> reported;
};

// The sdk's upload callback (urnet_upload_logs_cb), on an SDK thread: the
// server's answer or the post's error ends the upload in the flight, whose
// finish hook pushes the status.
void OnLogUploadReport(void* userData, const char* resultJson, const char* error) {
  std::unique_ptr<LogUploadReport> report(static_cast<LogUploadReport*>(userData));
  logupload::FlightState state = logupload::FlightState::Uploaded;
  if (error != nullptr) {
    state = logupload::FlightState::Failed;
    LogWarn("logs: the log upload ({} device) failed: {}", report->carrierName, error);
  } else if (resultJson != nullptr) {
    try {
      const auto result = nlohmann::json::parse(resultJson).get<urnet::UploadLogsResult>();
      if (result.error) {
        state = logupload::FlightState::Refused;
        LogWarn("logs: the log upload ({} device) was refused: {}", report->carrierName,
                result.error->message);
      }
    } catch (const std::exception& e) {
      state = logupload::FlightState::Failed;
      LogWarn("logs: the log upload's ({} device) answer did not parse: {}",
              report->carrierName, e.what());
    }
  }
  if (state == logupload::FlightState::Uploaded) {
    LogInfo("logs: the log upload ({} device) finished", report->carrierName);
  }
  if (report->reported) report->reported();
  report->flight->Finish(report->uploadId, state);
}

// The upload's own thread (logupload::Flight::Run): the sdk zips this process's
// log files, with the app's under app/, and starts the post to
// /log/{feedback_id}/upload. The call goes through the c abi by the device's
// handle, which the flight keeps valid until it returns, so that nothing of
// TunnelController is touched here. The sdk reads the app's files through its
// own duplicates of their handles before the call returns
// (DeviceLocal.UploadLogsWithFiles), so they are closed right after it.
void UploadLogsOnDevice(const std::shared_ptr<logupload::Flight>& flight, int64_t uploadId,
                        uint64_t deviceHandle, const std::string& feedbackId,
                        const char* carrierName, const std::function<void()>& reported,
                        AppLogHandles& appLogFiles) {
  urnet::UploadLogsFileList uploadLogsFiles;
  for (const AppLogHandles::File& file : appLogFiles.Files()) {
    urnet::UploadLogsFile uploadLogsFile;
    uploadLogsFile.Source = applogs::kAppLogFilesSource;
    uploadLogsFile.Name = file.name;
    uploadLogsFile.FileDescriptor = static_cast<int64_t>(reinterpret_cast<intptr_t>(file.handle));
    uploadLogsFiles.push_back(std::move(uploadLogsFile));
  }
  const std::string uploadLogsFilesJson = nlohmann::json(uploadLogsFiles).dump();
  auto report = std::make_unique<LogUploadReport>();
  report->flight = flight;
  report->uploadId = uploadId;
  report->carrierName = carrierName;
  report->reported = reported;
  char* error = nullptr;
  const bool started = urnet_device_local_upload_logs_with_files(
      deviceHandle, feedbackId.c_str(), uploadLogsFilesJson.c_str(), &OnLogUploadReport,
      report.get(), &error);
  appLogFiles.CloseAll();
  if (started) {
    // the callback owns it now
    report.release();
    return;
  }
  const std::string message = error != nullptr ? error : "the device is gone";
  if (error != nullptr) urnet_free_string(error);
  LogWarn("logs: the log upload ({} device) did not start: {}", carrierName, message);
  if (reported) reported();
  flight->Finish(uploadId, logupload::FlightState::Failed);
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

TunnelController::TunnelController()
    : logUploadFlight_(std::make_shared<logupload::Flight>(NowMillis())),
      sdkVersion_(urnet::version()) {
  storageDir_ = StorageRoot(/*isService=*/true);
  // An upload's end pushes the status that carries it, as a transition does.
  logUploadFlight_->SetOnFinished([this] { NotifyStateChanged(); });
}

TunnelController::~TunnelController() {
  // First: the hook reaches into this object, and no call of it may run once
  // it is going.
  logUploadFlight_->ClearOnFinished();
  Stop();
  // A log upload still zipping is given a moment to come out of the sdk's
  // call, and no more: it holds nothing of this object, and the service's exit
  // must not wait on a disk.
  if (!logUploadFlight_->WaitReturned(kLogUploadReturnBudget)) {
    LogWarn("logs: a log upload was still zipping when the service stopped");
  }
}

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

urnet::NetworkSpace TunnelController::ImportNetworkSpaceLocked(
    const std::string& networkSpaceJson) {
  if (!spaceManager_) {
    spaceManager_ =
        urnet::newNetworkSpaceManager(Narrow(SdkStorageDir(true).wstring()));
  }
  urnet::NetworkSpace space = spaceManager_->importNetworkSpaceFromJson(networkSpaceJson);
  // The client info this process's Api for the space carries: every device
  // built on it signs in and connects with that Api's (server
  // session/REVOKE-UI-FINAL.md §1.13), and the app's own Api, which the app
  // sets, is in another process. The service ships with the app, so this
  // build's version is the app's. Its own best effort: an sdk that refuses it
  // reports an unknown device, and must not cost the start.
  try {
    space.getApi().setClientInfo(urnet::newClientInfo("windows", version::kString));
  } catch (const std::exception& e) {
    LogWarn("tunnel: setting the client info failed: {}", e.what());
  }
  // Where set_provide_extender writes with no device running. Its own best
  // effort: a key that cannot be read costs that write, never this start.
  try {
    lastSpaceKey_ = space.getKey();
  } catch (const std::exception&) {
    lastSpaceKey_.reset();
  }
  return space;
}

urnet::DeviceLocal TunnelController::NewDeviceLocked(const urnet::NetworkSpace& space,
                                                     const std::string& byJwt,
                                                     const std::string& deviceDescription,
                                                     const std::string& deviceSpec,
                                                     const std::string& appVersion,
                                                     const std::string& instanceId,
                                                     const char* who) {
  auto km = LoadKeyMaterial();
  // The device target comes from the measured host's memory tier, and the
  // same cached measurement chose the process budget at startup, so the
  // target and the budget backing it are always one tier.
  const int64_t memoryTargetByteCount = DeviceMemoryTargetByteCount();
  LogInfo("{} constructing DeviceLocal ({} identity, {} MiB memory target)", who,
          km ? "persisted" : "new", memoryTargetByteCount / (1024 * 1024));
  if (km) {
    return urnet::newDeviceLocalWithMemoryTarget(space, byJwt, deviceDescription, deviceSpec,
                                                 appVersion, instanceId,
                                                 /*enable_rpc=*/false, *km,
                                                 memoryTargetByteCount);
  }
  // An empty key material (handle 0) is nil in the SDK: new identity.
  urnet::DeviceLocal device = urnet::newDeviceLocalWithMemoryTarget(
      space, byJwt, deviceDescription, deviceSpec, appVersion, instanceId,
      /*enable_rpc=*/false, urnet::DeviceLocalKeyMaterial{}, memoryTargetByteCount);
  PersistKeyMaterial(device.getKeyMaterial());
  return device;
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
          clamped >= 6 ? std::string("Capture waits for provider proof, with the "
                                     "pump and split routing prepared first.")
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
  cfg.block_ipv6_when_disconnected = true;
  cfg.service_image_path = ServiceImagePath();
  // Install-derived identity, used only in Connecting/Connected. Armed remains
  // independent of session state and never permits this UI image.
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
    // The second funnel for the status mirror (see SetStateLocked): a firewall
    // transition changes what the app is told about this machine WITHOUT
    // changing the tunnel state, and the app's disconnect rule reads wfp_state
    // directly — "the machine is captured" is routes OR a policy in force.
    PublishStatusLocked();
    return true;
  }
  WfpConfig cfg = BaseWfpConfig();
  cfg.tun_luid = (state == WfpState::Connected && adapter_)
                     ? adapter_->Luid().Value
                     : 0;
  cfg.tunnel_resolvers_v4 =
      state == WfpState::Connected ? appliedResolvers_ : std::vector<std::string>{};
  // The v6 half follows what NetworkConfig actually installed, so the firewall
  // and the route table describe the same tunnel: a v6 floor with the tun and
  // the v6 resolvers lifted through it only when the tun really carries v6.
  cfg.tunnel_ipv6 =
      state == WfpState::Connected && netConfig_ != nullptr && netConfig_->AppliedIpv6();
  cfg.tunnel_resolvers_v6 =
      cfg.tunnel_ipv6 ? appliedResolversV6_ : std::vector<std::string>{};
  // CONNECTING ONLY. There is no tunnel resolver yet. The bound SDK normally
  // resolves in-process and matches the exact service-image permit, but a
  // Windows/system fallback lookup before that bind is active leaves through
  // Dnscache in svchost.exe. This address-scoped path keeps that transition
  // recoverable without widening the idle Armed policy.
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
      LogInfo("wfp: connecting-state compatibility DNS path = the host's own "
              "resolvers [{}] (read fresh; Windows fallback lookups leave "
              "Dnscache in svchost and cannot be permitted by our app id)",
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
  if (AttemptsConnection(state)) {
    if (cfg.app_image_path.empty()) {
      LogWarn("wfp: URnetwork.exe was not found next to this executable, so the "
              "UI process gets NO firewall permit. Its platform traffic stays "
              "inside the tunnel while connected — if the tunnel has no working "
              "exit, the UI cannot reach the platform to say so.");
    } else {
      LogInfo("wfp: permitting the UI process ({}) on the physical NIC for the "
              "connection attempt/session. It pairs with the app binding "
              "its own sdk egress off the tun; neither half works alone. This "
              "permit exists while connecting or connected; armed permits urnetworkd "
              "and nothing else.",
              Narrow(cfg.app_image_path));
    }
  }
  const bool ok = wfp_.Apply(state, cfg);
  // Whether it took or not: the mirror must carry what wfp_ actually holds, and
  // a FAILED apply is exactly the state the app must not be told is protection.
  PublishStatusLocked();
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
    // The app is told what the firewall actually holds, from the one path that
    // changes it without mutex_. Routes are untouched here, so only the policy
    // is patched.
    RepublishMachineFactsLockFree(/*routesReverted=*/false);
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
  // ...and the OTHER lock-free reader, for the same reason: Status() serves the
  // app's connect decision without taking mutex_, so a transition that does not
  // republish is a transition the app cannot see. This is the funnel that covers
  // most of the rule in the header; the sites that change a status-visible fact
  // WITHOUT a transition publish for themselves and say so.
  //
  // It composes a status, which reads EgressMonitor under its own small lock.
  // That is bounded (a struct read behind an IP Helper refresh) and it is the
  // same work the RPC thread used to do on every get_state — unlike the glog
  // flush this function deliberately does not do, it cannot re-enter the SDK.
  PublishStatusLocked();
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

  // Read controller ownership and the DNS-apply result, not the step number.
  // These are not fresh OS route, resolver, adapter or filter observations.
  const bool hadAdapter = adapter_ != nullptr;
  const bool hadRoutes = netConfig_ != nullptr;
  const bool hadDns = netConfig_ != nullptr && netConfig_->DnsApplied();
  const WfpState wfpAtStop = wfp_.State();

  LogWarn("tunnel: ======== STAGED BRING-UP: STOPPING AFTER STEP {}/8 ========",
          step);
  LogWarn("tunnel: --stop-after={} was passed, so the sequence stops here. The "
          "last thing that ran was step {}/8, which left {}. {}",
          stopAfter, step, reached,
          step >= 6 ? std::string("The pump and split routing were prepared "
                                  "before these capture checkpoints.")
                      : std::format("Steps {}/8 to 8/8 did NOT run.", step + 1));
  LogWarn("tunnel: state AT THE STOP POINT: wintun adapter={}, address/mtu/"
          "routes={}, tunnel dns={}, firewall policy={}. facts=ownership "
          "os_state=unverified",
          hadAdapter ? "OWNED" : "not owned",
          hadRoutes ? "OWNED" : "not owned",
          hadDns ? "ACCEPTED" : "not accepted", ToString(wfpAtStop));
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

  // Re-read controller owners, the marker and policy state after the unwind.
  // Empty owners do not verify kernel cleanup, including an abandoned worker.
  const bool routesLeft = netConfig_ != nullptr;
  const bool adapterLeft = adapter_ != nullptr;
  const bool markerLeft = PeekActiveMarker();
  const WfpState wfpAfter = wfp_.State();
  const bool clean = !routesLeft && !adapterLeft && !markerLeft &&
                     wfpAfter == WfpState::Off;
  if (clean) {
    LogWarn("tunnel: ======== STOPPED AFTER STEP {}/8. CONTROLLER OWNERS EMPTY "
            "======== routes=false dns=false adapter=false active_marker=false "
            "firewall=off facts=ownership os_state=unverified. {}",
            step,
            hadRoutes ? "Route/DNS cleanup was attempted; compare OS state "
                        "with the baseline."
                      : "No route/DNS configuration owner was present.");
  } else {
    LogError("tunnel: ======== STOPPED AFTER STEP {}/8, BUT CONTROLLER CLEANUP "
             "IS INCOMPLETE ======== routes={} adapter={} active_marker={} "
             "firewall={} facts=ownership os_state=unverified. Stop this process "
             "(the adapter and the filter policy both die with it), then run "
             "`urnetworkd revert` from an elevated prompt.",
             step, routesLeft ? "OWNED" : "not owned",
             adapterLeft ? "OWNED" : "not owned",
             markerLeft ? "STILL SET" : "clear", ToString(wfpAfter));
  }
  return true;
}

proto::TunnelStatus TunnelController::StartLocked(const proto::StartTunnel& config) {
  const uint64_t stopGeneration = stopGeneration_.load();
  // Adopt the caller's kill-switch preference BEFORE the teardown below, so a
  // reconnect keeps the policy in force across the gap rather than dropping it
  // and re-arming.
  killSwitch_.store(config.kill_switch);
  StopLocked(/*finalDisarm=*/false);  // idempotent restart
  // On the watchdog's capture timing, which its sampler and evaluator read too.
  auto readiness = std::make_shared<CaptureReadiness>(deadTunnelWatchdog_.settings().capture);
  std::atomic_store(&captureReadiness_, readiness);
  if (stopGeneration_.load() != stopGeneration) readiness->Cancel();
  // A NEW ATTEMPT CLEARS THE LAST TEARDOWN'S REASON. Left set, a failsafe stop
  // would keep explaining itself over the top of the connection that replaced
  // it — the app renders "URnetwork disconnected you" beside a live tunnel.
  lastStopReason_.store(kStopReasonNone);

  // REFUSE TO START ON TOP OF A DEVICE THAT IS STILL HELD. The teardown above is
  // bounded, so it can return having LEFT the previous session's DeviceLocal
  // and wintun adapter alive on a detached thread (see TearDownSessionLocked).
  // Building a second session over that would ask wintun to create a second
  // adapter carrying the same pinned GUID while the first still holds it, with
  // two DeviceLocals and two packet pumps behind it — a worse machine state
  // than the failure that got us here, and one no revert path understands.
  //
  // THE QUESTION IS ASKED ABOUT NOW, NOT ABOUT THE PAST. This used to read
  // `if (TeardownAbandoned())` — a process-global one-way bool with no clear
  // path — and on 2026-08-11 that cost two testers their VPN: a teardown ran
  // 2013 ms against a 2000 ms budget, was abandoned by thirteen milliseconds,
  // FINISHED 392 ms later having released everything, and every Connect for the
  // rest of the process's life was refused with "its device is still held". It
  // had been free for 2.2 seconds. The gate each abandoned worker signals
  // through is retained now (StopBudget.h), so the same worker that caused the
  // refusal can withdraw it, and this is where that is re-read.
  // AND ONCE THIS PROCESS HAS DECIDED TO LEAVE, IT LEAVES. Asked before the
  // sweep and answered without it, because the sweep can legitimately say "clear"
  // during the second kSelfRestartGrace leaves on the clock for the reply to
  // reach the app: the abandoned worker finishes at, say, 300ms, a Connect
  // arrives at 500ms, and it would be allowed to build a wintun adapter, rewrite
  // this machine's routes and DNS, arm the firewall — and then be terminated at
  // 1000ms by a countdown that was armed before it started. That is recoverable
  // only by the floor, and to the user it looks like precisely the bug this whole
  // change is about: press Connect, watch it die, no reason given. A start we
  // will not live to keep is not a start.
  if (SelfRestartPending()) {
    SetStateLocked(proto::TunnelState::Error);
    error_ =
        "this service is already restarting itself — reconnecting in a few "
        "seconds";
    LogWarn("tunnel: REFUSING to start — this process has ALREADY committed to "
            "ending itself so the scm restarts it clean, and is inside the "
            "{}ms grace it leaves for that reply to reach the app. Even if the "
            "wedged teardown has finished in the meantime, a session started "
            "now would be killed mid-bring-up by a countdown it cannot see. The "
            "start after the restart is the one that works.",
            kSelfRestartGrace.count());
    return StatusLocked();
  }

  const AbandonedTeardownSweep abandoned = SweepAbandonedTeardowns();
  // SAY SO. A refusal that quietly stops happening is indistinguishable in a log
  // from a refusal that never happened, and this is the line that tells the next
  // reader the budget was too tight rather than the machine broken.
  if (abandoned.completed_late > 0)
    LogWarn("tunnel: {} previously ABANDONED sdk teardown(s) have since "
            "FINISHED and released the device, adapter and pump they were "
            "holding. The refusal they caused is withdrawn and this start "
            "proceeds normally — they overran their {}ms budget, they were not "
            "wedged.",
            abandoned.completed_late, kSdkTeardownBudget.count());
  if (abandoned.outstanding > 0) {
    SetStateLocked(proto::TunnelState::Error);
    // RECOVERY IS PERFORMED, NOT PRESCRIBED. Telling a user to run sc.exe is a
    // dead end wearing an error message's clothes. The installed service has
    // SC_ACTION_RESTART (InstallService in main.cpp), so ending this process IS
    // the fix: the adapter the worker is holding dies with it as a PnP surprise
    // removal, the SCM starts a clean one within seconds, and the app reattaches
    // by itself (SdkHost::OnServiceDisconnected -> ScheduleServiceRetry).
    //
    // StopLocked has already requested machine cleanup before SDK teardown.
    // Process exit remains the adapter backstop; owner release is not a fresh
    // OS-state verification.
    const bool restarting = RequestSelfRestart(
        "a previous sdk teardown is still holding this session's device");
    error_ = restarting
                 ? "a previous teardown is still holding its device, so this "
                   "service is restarting itself now — reconnecting in a few "
                   "seconds"
                 : "a previous teardown is still holding its device; this "
                   "service process must be restarted before another tunnel "
                   "can start";
    LogError("tunnel: REFUSING to start — {} abandoned sdk teardown(s) have "
             "NOT finished and still hold a device, adapter and pump. A second "
             "session would ask wintun for a second adapter on the same pinned "
             "guid. {}",
             abandoned.outstanding,
             restarting
                 ? "This process is ending itself so the scm restarts it clean; "
                   "prior route/DNS cleanup=attempted os_state=unverified."
                 : "Nothing can restart this process for you here — stop it and "
                   "run it again.");
    return StatusLocked();
  }
  activeInstanceId_ = config.instance_id;
  rpcSessionId_ = config.rpc_session_id;
  excludedPaths_ = config.excluded_app_paths;
  allowlist_ = config.allowlist_mode;
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
  // startMode_ and error_ both moved without a state transition (SetStateLocked
  // ran above them), and mode is one of the facts the app decides on — an
  // rpc-only session is one that never has routes, in both directions. Publish.
  PublishStatusLocked();

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
        return StatusLocked();
    } else {
      const std::filesystem::path dll = ExeDir() / L"wintun.dll";
      LogInfo("tunnel: [1/8] loading wintun from {}", dll.string());
      // The Windows error code goes into the message the user sees: it is
      // what tells a missing DLL, a non-elevated process and a blocked driver
      // apart (WintunError.h).
      DWORD wintunError = 0;
      wintun_ = Wintun::Load(dll, &wintunError);
      if (!wintun_)
        throw std::runtime_error(wintun_error::LoadFailure(wintunError));
      adapter_ = WintunAdapter::Create(*wintun_, ids::kTunAdapterName,
                                       ids::kTunAdapterGuid, kRingCapacity,
                                       &wintunError);
      if (!adapter_)
        throw std::runtime_error(wintun_error::AdapterFailure(wintunError));
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
        return StatusLocked();
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
    // above fires when the binding moves, which a cable coming out does not do
    // (the index is deliberately retained) — so it cannot be the hook that tells
    // the SDK the network moved. This one fires whenever an observation moves
    // the network the SDK's sockets ride, the cable out included
    // (EgressChangeFilter), and never for churn that moves nothing: each one
    // kicks every transport in this process.
    //
    // It must do almost nothing: it runs on a system worker thread that
    // EgressMonitor::Stop() waits for, so it records the event and returns. The
    // SDK calls happen on the watchdog's own thread, coalesced.
    egress_->SetOnNetworkEvent(CaptureNetworkEventHandler(readiness,
        [this](const auto& session, int64_t eventMillis) {
          return deadTunnelWatchdog_.NoteNetworkEvent(session, eventMillis);
        }));
    egress_->SetOnNetworkQualityEvent(
        [this] { deadTunnelWatchdog_.NoteNetworkQualityEvent(); });
    egress_->Start();  // logs the chosen interface; keeps it current on change
    // The binding Status() reports, from the monitor that owns it (see
    // liveEgressIndex4_). Start() has reported it already when there is one;
    // this also clears whatever an earlier session left when there is not.
    {
      const EgressInterfaces bound = egress_->Current();
      liveEgressIndex4_.store(static_cast<int64_t>(bound.index4));
      liveEgressIndex6_.store(static_cast<int64_t>(bound.index6));
    }
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
      return StatusLocked();

    // --- 3/8 NetworkSpace (own storage; import the app's space json) ---
    step = "3/8 network space";
    // The network country the app read, in force before the space and its
    // device exist, so their first extender dials already have it.
    SetNetworkCountry(config.network_country_code, config.network_country_source);
    LogInfo("tunnel: [3/8] opening the network space in {}",
            SdkStorageDir(true).string());
    networkSpace_ = ImportNetworkSpaceLocked(config.network_space_json);
    if (HaltAfterStepLocked(
            3, "an open network space under the service's own storage root — "
               "files, and nothing else"))
      return StatusLocked();

    // --- 4/8 DeviceLocal (stable provider identity via persisted key material) ---
    step = "4/8 device";
    // The same construction the provider-only device uses (NewDeviceLocked):
    // one copy of the identity rules for both.
    device_ = NewDeviceLocked(*networkSpace_, config.by_jwt, config.device_description,
                              config.device_spec, config.app_version, config.instance_id,
                              "tunnel: [4/8]");
    LogInfo("tunnel: [4/8] device client_id={}", device_->getClientId());

    // Per-flow app attribution — "which program owns this connection" — fed to
    // the SDK's FlowOwnerLookup seam. THE FIRST CALLER OF setFlowOwnerLookup IN
    // THIS REPO: the seam has existed with a nil default (no attribution, one
    // branch cost on the SDK's egress path) since the smart-routing design.
    //
    // Installed HERE, right after DeviceLocal exists and BEFORE anything past
    // this point can carry a packet — steps 5-8 still have to run (the rpc
    // listener, the firewall widen, network settings, the pump) before this
    // session is up. The lambda below is the ONLY thing that touches
    // flowOwner_ from the SDK's side, and it does a cache-only read and
    // returns immediately: it is invoked from the SDK on the packet path
    // through a C -> Go -> C callback chain, so a synchronous table
    // enumeration here would stall the tun drain for every flow it is asked
    // about — see FlowOwner.h for why that is a hard requirement and not a
    // tuning choice.
    //
    // flowOwner_ is a TunnelController member that OUTLIVES this session
    // (like wfp_); Start() is idempotent, so a reconnect re-installs the
    // lookup on the new DeviceLocal without spinning up a second worker.
    flowOwner_.Start();
    device_->setFlowOwnerLookup(
        [this](int64_t version, int64_t protocol, std::string sourceIp,
               int64_t sourcePort, std::string destIp, int64_t destPort) {
          return flowOwner_.Lookup(version, protocol, sourceIp, sourcePort,
                                   destIp, destPort);
        });

    if (HaltAfterStepLocked(
            4, "a live DeviceLocal and its persisted identity — sockets to the "
               "platform, over the PHYSICAL nic; still no interface, route or "
               "dns entry of ours on this machine"))
      return StatusLocked();

    // --- 5/8 mTLS RPC listener the app's DeviceRemote dials ---
    step = "5/8 rpc";
    LogInfo("tunnel: [5/8] starting the device rpc listener on {}",
            config.rpc_listen_hostport);
    device_->setRpcServer(config.rpc_server_pem, config.rpc_client_cert_pem,
                          config.rpc_listen_hostport);
    rpcHostPort_ = config.rpc_listen_hostport;
    // The endpoint the app's DeviceRemote dials, and steps 6-8 can take seconds
    // — long enough for a get_state to be served in between. Publish it now
    // rather than at the next transition.
    PublishStatusLocked();

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
      return StatusLocked();

    if (readiness->Cancelled()) {
      StopLocked(/*finalDisarm=*/true);
      return StatusLocked();
    }

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
      PublishStatusLocked();  // the uptime clock, set after the transition
      // The device hears about network changes the way a tunnel session's does:
      // from the watchdog's sampler, the one thread that tells it. Without one
      // the monitor's events found no listener and an rpc-only device was never
      // told, so it re-dialled a switch only on its transports' timeouts. The
      // failsafe has nothing to judge here — it judges only a tunnel that is up —
      // so it gets no handler, and with no capture to wait for there is no
      // readiness callback either.
      deadTunnelWatchdog_.Start(&*device_, nullptr, {}, readiness);
      EgressInterfaces bound = egress_->Current();
      LogInfo("tunnel: RPC-ONLY UP in {}ms (rpc={} egress_v4_ifindex={}). Steps "
              "6/8 (routes+dns), 7/8 (split tunnel) and 8/8 (packet pump) were "
              "NOT run: no route, no dns entry and no address were written, and "
              "no active marker was set. The machine's network is untouched and "
              "no traffic is carried.",
              upSinceMillis_ - startedAtMillis, rpcHostPort_, bound.index4);
      return StatusLocked();
    }

    // Return the RPC listener before waiting for providers. The app cannot
    // select a destination until BootstrapSession receives this reply.
    SetStateLocked(proto::TunnelState::Preparing);
    WatchForCaptureLocked();
    LogInfo("tunnel: stage=bootstrap outcome=rpc-ready capture=pending "
            "routes=false dns=false firewall={} kill_switch={} elapsed_ms={}",
            ToString(wfp_.State()), killSwitch_.load(),
            NowMillis() - startedAtMillis);
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

  return StatusLocked();
}

// Capture is an asynchronous transaction after RPC and provider preparation.
// Caller holds mutex_; all SDK preparation precedes machine-wide changes.
CaptureResult TunnelController::BringUpTunnelLocked(CaptureReadiness& readiness,
                                                    CaptureTicket ticket,
                                                    const char*& step) {
  if (startMode_ != proto::StartMode::Tunnel || !adapter_ || !device_)
    throw std::runtime_error("capture requires a prepared tunnel session");

  TunnelNetworkSettings settings;
  return ApplyCapture(
      readiness, ticket,
      [&](CaptureStage stage) {
        switch (stage) {
          case CaptureStage::Prepare: {
            step = "adapter settings";
            // Complete SDK calls before installing any machine-wide policy.
            settings.local_address_v4 = device_->tunnelLocalAddress();
            if (settings.local_address_v4.empty())
              settings.local_address_v4 = "169.254.2.1";
            settings.prefix_v4 = 24;
            settings.mtu = kTunnelMtu;
            if (auto dns = device_->tunnelDnsAddressesIpv4(); dns && !dns->empty())
              settings.dns_servers_v4 = *dns;
            else
              settings.dns_servers_v4 = {urnet::getDefaultTunnelDnsAddressIpv4()};
            settings.local_address_v6 = device_->tunnelLocalAddressIpv6();
            if (settings.HasIpv6()) {
              const int64_t prefix = urnet::getTunnelLocalPrefixLengthIpv6();
              settings.prefix_v6 =
                  (0 < prefix && prefix <= 128) ? static_cast<uint8_t>(prefix) : 64;
              if (auto dns = device_->tunnelDnsAddressesIpv6(); dns && !dns->empty())
                settings.dns_servers_v6 = *dns;
              else
                settings.dns_servers_v6 = {urnet::getDefaultTunnelDnsAddressIpv6()};
            }
            step = "packet pump and split tunnel";
            // Settings may change during provider discovery. Use the latest
            // app rules, and prepare both directions before capture routes exist.
            splitTunnel_.Open();
            PushExcludedToDriver(excludedPaths_, allowlist_);
            if (!pump_) {
              pump_ = std::make_unique<PacketPump>(*adapter_, *device_);
              if (!pump_->Start())
                throw std::runtime_error("packet pump failed to start");
            }
            LogInfo("tunnel: stage=packet-pump outcome=ready capture=false");
            return true;
          }
          case CaptureStage::Firewall: {
            step = "capture firewall";
            // Read actual defaults, not the monitor's retained binding: a lost
            // uplink keeps its old binding deliberately to prevent routing loops.
            const auto egress = NetworkConfig::DiscoverEgress(adapter_->Luid());
            if (egress.index4 == 0 && egress.index6 == 0)
              throw std::runtime_error("no physical default route");
            if (!ApplyWfpLocked(WfpState::Connecting)) {
              if (killSwitch_.load())
                throw std::runtime_error("kill switch policy unavailable");
              LogWarn("tunnel: stage=capture-firewall outcome=unavailable "
                      "kill_switch=false");
            }
            return true;
          }
          case CaptureStage::Network: {
            step = "capture routes and DNS";
            netConfig_ = std::make_unique<NetworkConfig>(adapter_->Luid());
            SetActiveMarker(true);
            if (!netConfig_->Apply(settings))
              throw std::runtime_error("network configuration failed");
            appliedResolvers_ = settings.dns_servers_v4;
            appliedResolversV6_ = netConfig_->AppliedIpv6()
                                      ? settings.dns_servers_v6
                                      : std::vector<std::string>{};
            PublishStatusLocked();
            if (!netConfig_->DnsApplied())
              throw std::runtime_error("tunnel DNS configuration failed");
            return true;
          }
          case CaptureStage::Connected: {
            step = "connected firewall";
            if (wfp_.State() != WfpState::Off &&
                !ApplyWfpLocked(WfpState::Connected))
              throw std::runtime_error("connected policy unavailable");
            NetworkConfig::FlushResolverCache();
            // The historical debug labels are retained. Preparation now always
            // precedes capture; no debug mode may install routes into a stopped pump.
            if (HaltAfterStepLocked(6, "capture routes and DNS applied after "
                                      "provider proof, with the pump already ready") ||
                HaltAfterStepLocked(7, "capture with current split-tunnel rules") ||
                HaltAfterStepLocked(8, "the complete proven tunnel"))
              return false;
            return true;
          }
        }
        return false;
      },
      [&] {
        // Idempotent even if a debug stop already unwound the session. A stale
        // proof may retry with the same pump; a hard failure uses StopLocked.
        RevertMachineStateLocked(/*finalDisarm=*/false, netConfig_ != nullptr);
        if (!readiness.Cancelled() && killSwitch_.load() &&
            wfp_.State() != WfpState::Off)
          ApplyWfpLocked(WfpState::Connecting);
        LogInfo("tunnel: stage=capture outcome=rolled-back cleanup=attempted "
                "routes=false dns=false facts=ownership os_state=unverified "
                "firewall={}", ToString(wfp_.State()));
      });
}

void TunnelController::WatchForCaptureLocked() {
  const auto readiness = std::atomic_load(&captureReadiness_);
  deadTunnelWatchdog_.Start(&*device_, nullptr,
      [this](DeadTunnelReason reason) { FailsafeStop(reason); }, readiness,
      [this, readiness](CaptureTicket ticket) { ActivateCapture(readiness, ticket); });
}

void TunnelController::CancelCapture() {
  if (const auto readiness = std::atomic_load(&captureReadiness_)) readiness->Cancel();
}

void TunnelController::ActivateCapture(std::shared_ptr<CaptureReadiness> readiness,
                                       CaptureTicket ticket) {
  // Stop publishes cancellation before waiting for this mutex. Short timed
  // acquisitions let a terminal callback waiting behind another control request
  // leave inside the watchdog's join budget, without ever joining itself.
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  while (!readiness->Cancelled()) {
    if (lock.try_lock_for(std::chrono::milliseconds(10))) break;
  }
  if (!lock.owns_lock() || readiness->Cancelled() ||
      readiness != std::atomic_load(&captureReadiness_) ||
      state_ != proto::TunnelState::Preparing) return;
  const char* step = "readiness";
  try {
    LogInfo("tunnel: stage=provider-proof generation={} outcome=ready", ticket.generation);
    const auto result = BringUpTunnelLocked(*readiness, ticket, step);
    if (result == CaptureResult::Halted || readiness->Cancelled()) return;
    if (result == CaptureResult::Waiting) {
      LogInfo("tunnel: stage=capture outcome=superseded routes=false dns=false "
              "facts=ownership os_state=unverified");
      WatchForCaptureLocked();
    } else {
      upSinceMillis_ = NowMillis();
      SetStateLocked(proto::TunnelState::Up);
      LogInfo("tunnel: stage=capture outcome=active generation={} routes=true dns={} "
              "firewall={}", ticket.generation, netConfig_->DnsApplied(),
              ToString(wfp_.State()));
      deadTunnelWatchdog_.Start(&*device_, pump_->Counters(),
          [this](DeadTunnelReason reason) { FailsafeStop(reason); }, readiness);
    }
  } catch (const std::exception&) {
    // The stage identifies the failure without copying SDK messages containing
    // network endpoints, credentials, or device identifiers into diagnostics.
    error_ = std::string("tunnel activation failed at ") + step;
    LogError("tunnel: stage=capture outcome=failed component={}", step);
    StopLocked(/*finalDisarm=*/false);
    SetStateLocked(proto::TunnelState::Error);
  }
  lock.unlock();
  NotifyStateChanged();
}

void TunnelController::Stop() {
  stopGeneration_.fetch_add(1);
  CancelCapture();
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
  // Route cleanup was requested, but no Stopped transition is
  // coming — mutex_ is wedged, so nothing will publish a composed status ever
  // again on this process. Patch what actually changed, or every later
  // get_state reports a machine that is still captured.
  RepublishMachineFactsLockFree(/*routesReverted=*/true);
  // THE EXIT LATCH ONLY, and deliberately not a start refusal. Nothing was
  // handed to a worker here — the session objects are still owned by this
  // controller, behind a mutex_ that a wedged operation is holding. A later
  // start can only run if that lock came back, i.e. if the wedge is over, and
  // its first act is StopLocked, which tears the session down through the
  // bounded path and registers a real device-holding gate if THAT is abandoned.
  // What survives from here is only "this process may no longer unwind", which
  // is true and permanent because a thread is still somewhere inside the SDK.
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
          "route_cleanup=requested os_state=unverified. With the kill switch "
          "off, firewall removal is the documented fail-open action; this does "
          "not verify physical-network connectivity.",
          who);
  wfp_.Revert();
}

// --- the dead-tunnel failsafe ----------------------------------------------
//
// See the contract in the header. This runs on TunnelWatchdog's evaluator
// thread, which has already logged WHY; what is added here is what is being
// done about it and what the machine is left in.
void TunnelController::FailsafeStop(DeadTunnelReason reason) {
  stopGeneration_.fetch_add(1);
  CancelCapture();
  // Recorded BEFORE the teardown, so every status pushed during it — including
  // the Stopping transition — already carries the reason. An app that learns
  // "stopped" first and "why" second renders the alarming half alone.
  lastStopReason_.store(StopReasonOf(reason));
  const bool killSwitchOn = killSwitch_.load();
  const bool finalDisarm = FailsafeFinalDisarm(killSwitchOn);

  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (lock.try_lock_for(kStopLockBudget)) {
    // Request machine cleanup before bounded SDK teardown through the same
    // ordinary path. Its ownership flags do not independently verify OS state.
    StopLocked(finalDisarm);
    lock.unlock();
    LogWarn("tunnel: the failsafe teardown cleanup=attempted facts=ownership "
            "os_state=unverified. {} No reconnection is "
            "attempted and none will be: the next attempt is the user's, which "
            "is what makes this impossible to thrash.",
            killSwitchOn
                ? "The kill switch is on; Armed was requested. Check wfp_state "
                  "and policy errors for the applied result."
                : "The kill switch is off; firewall removal was requested. "
                  "Physical-network connectivity has not been verified.");
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
  // Same reason as Stop()'s escape: no composed status will ever be published
  // again on this process, and the app decides whether to offer a disconnect
  // from routes_installed and wfp_state. The push below would otherwise carry
  // stale ownership flags after this function's best-effort cleanup requests.
  RepublishMachineFactsLockFree(/*routesReverted=*/true);
  // Exit latch only, for the reason spelled out on the identical call in Stop():
  // no worker was ever handed this session, so there is no device for a later
  // start to collide with that StopLocked will not deal with itself.
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
  // Outside both locks by construction: the handler writes to the control pipe,
  // which can block on a client that is not reading. (Status() itself no longer
  // takes mutex_ — it serves the snapshot published at the last write — so this
  // is now about the pipe alone, which is reason enough.)
  if (handler) handler();
}

void TunnelController::StopLocked(bool finalDisarm) {
  CancelCapture();
  const proto::TunnelState priorState = state_;
  const bool wasRunning = priorState != proto::TunnelState::Stopped;
  // A configuration owner can hold partial Apply state. Its presence requests
  // cleanup; it does not prove which OS changes succeeded.
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
  // gives the SDK sampler its join budget (TunnelWatchdogSettings::
  // joinBudgetMillis) and abandons it after that (TunnelWatchdog.h spells out
  // why abandoning is safe). It also recognises
  // being called from its own evaluator thread — which it always is when the
  // failsafe is what got us here — and detaches instead of joining itself.
  deadTunnelWatchdog_.Stop();

  // --- THE ORDER OF THE UNWIND, WHICH IS THE FIX ----------------------------
  //
  // Phase 1 requests route, DNS, resolver-cache, marker and firewall cleanup
  // before touching the SDK. Per-operation failures are logged; this path does
  // not re-read kernel state to certify that every request succeeded.
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

  // The provider-only device, in every teardown, and so at the head of every
  // bring-up: StartLocked opens with StopLocked, so a Connect retires the
  // provider before the new session's DeviceLocal — the same persisted
  // identity — the adapter or a single route exists, and the two devices never
  // run together. It is phase 2 work (closing a device can block on the SDK),
  // so it comes after the machine is given back, and it is a no-op whenever a
  // tunnel session was running, because the two never coexist.
  RetireProviderDeviceLocked();
  // The standalone device a log upload ran on, for the same reason. Its upload
  // goes on: the POST runs on the network space's API, not on the device.
  RetireLogUploadDeviceLocked();

  const bool tornDown = TearDownSessionLocked();

  rpcHostPort_.clear();
  activeInstanceId_.clear();
  rpcSessionId_.clear();
  upSinceMillis_ = 0;
  SetStateLocked(proto::TunnelState::Stopped);
  // Distinguish attempted cleanup from the absence of a configuration owner.
  // Neither result is an independent check of the machine's network state.
  if (wasRunning)
    LogInfo("tunnel: stopped, route_dns_cleanup={} routes=false dns=false "
            "facts=ownership os_state=unverified{}",
            hadRoutes ? "attempted" : "not-owned",
            tornDown ? "" : " (SDK TEARDOWN ABANDONED — see above)");
}

void TunnelController::RevertMachineStateLocked(bool finalDisarm, bool hadRoutes) {
  if (netConfig_) { netConfig_->Revert(); netConfig_.reset(); }
  appliedResolvers_.clear();
  appliedResolversV6_.clear();
  // Publish released ownership before bounded SDK teardown, which may be
  // abandoned. These flags do not certify route/DNS removal from the OS.
  PublishStatusLocked();
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
  // Gate on configuration ownership, including partial Apply. An rpc-only
  // session has no configuration owner and must not flush the machine's cache.
  if (hadRoutes) NetworkConfig::FlushResolverCache();
  // Route cleanup was requested; clear the marker before the remaining unwind.
  SetActiveMarker(false);

  // --- the firewall policy, and what the kill switch actually decides -------
  //
  // Route cleanup was requested. Choose the intended post-cleanup policy;
  // the request does not prove either route removal or packet enforcement.
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
    // bootstrap DNS and UI permits; everything shared by both policies remains.
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
    LogInfo("tunnel: machine cleanup requested routes=false dns=false "
            "facts=ownership os_state=unverified resolver_cache_flush=attempted "
            "firewall={}; continuing with bounded session teardown",
            ToString(wfp_.State()));
}

// --- phase 2: our own objects, on a budget ---------------------------------
//
// Phase 1 has requested machine cleanup. This phase transfers the remaining
// session owners to a bounded worker; empty controller owners do not prove the
// worker finished or the adapter disappeared from the OS.
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
    egress_->SetOnNetworkQualityEvent(nullptr);
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
  // Its handlers are cleared above, so nothing writes these again for this
  // session; the next session's monitor sets them when it starts.
  liveEgressIndex4_.store(0);
  liveEgressIndex6_.store(0);

  // Clear the flow-owner lookup BEFORE the move below, for the same reason
  // egress_'s handlers are cleared above: the lambda StartLocked installed on
  // device_ at step 4/8 (FlowOwner.h) captures `this`, and the worker device
  // is about to be handed to may be ABANDONED rather than joined if
  // device->close() overruns kSdkTeardownBudget (StopBudget.h) — a real path,
  // not a hypothetical one, and exactly what the teardown-latch work exists
  // for. An abandoned worker's thread can outlive this TunnelController, and
  // a live DeviceLocal still holding a callback into a controller that may be
  // gone is a use-after-free waiting for the SDK to call it.
  //
  // setFlowOwnerLookup(nullptr) round-trips through an empty std::function,
  // which the generated wrapper turns into a null C callback + null user_data
  // (urnetwork_sdk.hpp: DeviceLocal::setFlowOwnerLookup) rather than a call
  // into a lambda pointing at us — the same nullptr-safe shape every other
  // optional SDK callback in this codebase relies on. So the device the
  // worker inherits, whether or not it is ever joined, can only ever answer
  // "no lookup installed"; it can never call back into this object again.
  if (device_) device_->setFlowOwnerLookup(nullptr);

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
       space = std::move(space), flight = logUploadFlight_]() mutable {
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
          // A log upload's call may still be on it: the flight then keeps it
          // until the call returns, and releases it.
          flight->KeepUntilReturned(device->handle(),
                                    std::make_shared<urnet::DeviceLocal>(std::move(*device)));
        }
        device.reset();
        adapter.reset();
        wintun.reset();
        space.reset();
        LogInfo("tunnel: sdk teardown complete");
      },
      // THE ONE ABANDONMENT IN THIS PROCESS THAT MAY REFUSE A LATER START. Every
      // per-session object was moved into the lambda above, so if this worker is
      // abandoned it — and only it — is what "the device is still held" means.
      // The gate RunBounded retains for it is what lets StartLocked ask again
      // later and get a different answer once it finishes.
      AbandonHazard::HoldsSessionDevice);

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
      "removal) and the filter policy with the dynamic session. NOT NECESSARILY "
      "FATAL TO CONNECT: this worker is watched, not written off — if it "
      "finishes later, the next start re-reads its gate, says so, and proceeds. "
      "Only a start attempted while it is STILL running is refused.",
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
  // The binding Status() serves, live: the mirror holds the one composed at the
  // last publish, and nothing publishes when the egress moves (this thread may
  // not take mutex_), so get_state went on handing the app the interface it had
  // already left. The app pins its own sdk to what it is told
  // (SdkHost::ApplySdkEgressBind), so it stayed pinned to that interface.
  const int64_t index4 = static_cast<int64_t>(egress.index4);
  const int64_t index6 = static_cast<int64_t>(egress.index6);
  const int64_t before4 = liveEgressIndex4_.exchange(index4);
  const int64_t before6 = liveEgressIndex6_.exchange(index6);
  const bool moved = before4 != index4 || before6 != index6;
  // And told, when the interface moved while the app is following a binding at
  // all (Status() reports one only while routes are installed). Pushed from a
  // worker: this is a system thread EgressMonitor::Stop() waits for, and a push
  // writes to the control pipe, which blocks while the app is not reading.
  bool reported = false;
  {
    std::scoped_lock lock(statusMirrorMutex_);
    reported = statusMirror_.routes_installed;
  }
  if (moved && reported) egressPush_.Request();

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
               "off-tunnel IPv6 and other adapters' resolvers are open, and "
               "reported to the app as wfp_state=off. It is retried at the next "
               "drop.",
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

bool TunnelController::Logout(const std::string& networkSpaceJson) {
  stopGeneration_.fetch_add(1);
  CancelCapture();
  // Timed, as Stop() takes it: a connect attempt wedged inside the sdk holds
  // mutex_ for as long as the process lives, and a logout that waited behind it
  // held the control pipe with it, so every later request queued behind a lock
  // that never came back. The app sends stop_tunnel first, whose own escape
  // gives the machine back, and keeps this logout owed until it succeeds.
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_for(kStopLockBudget)) {
    LogWarn("tunnel: logout could not take the session lock within {}ms; the device "
            "identity and the account's sdk state were not cleared",
            kStopLockBudget.count());
    return false;
  }
  // As deliberate as a disconnect, so it is reported as one. See Stop().
  lastStopReason_.store(kStopReasonUser);
  // finalDisarm: signing out is as deliberate as disconnecting, and there is no
  // session left to protect. Leaving a signed-out machine blocked would be
  // unexplainable from any surface the user still has. It retires the
  // provider-only device too: a signed-out machine provides nothing.
  StopLocked(/*finalDisarm=*/true);
  // Clear persisted device identity so the next login starts clean (mirrors the
  // macOS logout provider message clearing LocalState).
  std::error_code ec;
  std::filesystem::remove(storageDir_ / L"client_key_seed.bin", ec);
  std::filesystem::remove(storageDir_ / L"provide_cert.pem", ec);
  std::filesystem::remove(storageDir_ / L"provide_key.pem", ec);
  // ...and what the sdk stored in the account's space: a DeviceLocal persists
  // its client credential and instance there when it starts, with its peer pins
  // and transport policy. Imported here rather than looked up, because a
  // service restarted since the account's last device has imported no space.
  bool cleared = true;
  if (!networkSpaceJson.empty()) {
    try {
      const urnet::NetworkSpace space = ImportNetworkSpaceLocked(networkSpaceJson);
      const urnet::AsyncLocalState asyncLocalState = space.getAsyncLocalState();
      const urnet::LocalState localState =
          asyncLocalState ? asyncLocalState.getLocalState() : urnet::LocalState{};
      if (!localState) throw std::runtime_error("the network space has no local state");
      localState.logout();
    } catch (const std::exception& e) {
      LogError("tunnel: logout could not clear the account's sdk state: {}", e.what());
      cleared = false;
    }
  }
  LogInfo("tunnel: logged out (cleared device identity{})",
          networkSpaceJson.empty() ? "; no network space named, so no sdk state"
                                   : (cleared ? " and the account's sdk state" : ""));
  return cleared;
}

// --- the provider-only device (start_provider) ------------------------------
//
// See the contract in the header and Common/ProvideLifecycle.h. This is steps 3
// and 4 of a bring-up and nothing after them: no wintun adapter (step 1), no
// egress binding (step 2 — with no tun there is nothing to loop into, so the
// device's sockets follow the route table like any other process's), no rpc
// listener (step 5), no firewall policy, no route, no DNS entry, no active
// marker, no packet pump, no split tunnel and no flow-owner lookup.

bool TunnelController::StartProvider(const proto::StartProvider& request,
                                     std::string& error) {
  // Timed, like Stop(): a connect attempt wedged inside the SDK holds mutex_ for
  // as long as the process lives, and a start_provider that waited behind it
  // would hold the control pipe with it. There is nothing to provide beside a
  // bring-up in any case — its own device will.
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_for(kStopLockBudget)) {
    error = "a tunnel operation is in progress";
    LogWarn("provide: start_provider refused: the session lock was not free "
            "within {}ms",
            kStopLockBudget.count());
    return false;
  }
  const provide::ControlMode mode = provide::ControlModeFrom(request.provide_mode);
  provide::ServiceProviderState state;
  // Any trace of a tunnel session counts, not only the reported state: a
  // session's DeviceLocal runs under the same identity.
  state.tunnelSession = state_ == proto::TunnelState::Starting ||
                        proto::IsSessionLive(state_) ||
                        state_ == proto::TunnelState::Stopping || device_.has_value() ||
                        adapter_ != nullptr || netConfig_ != nullptr || pump_ != nullptr ||
                        egress_ != nullptr;
  state.firewallInForce = wfp_.State() != WfpState::Off;
  const AbandonedTeardownSweep abandoned = SweepAbandonedTeardowns();
  if (abandoned.completed_late > 0)
    LogWarn("provide: {} previously ABANDONED sdk teardown(s) have since FINISHED "
            "and released the device they were holding",
            abandoned.completed_late);
  state.deviceStillHeld = abandoned.outstanding > 0;
  state.restartPending = SelfRestartPending();
  if (const provide::ProviderRefusal refusal = provide::ProviderStartRefusal(mode, state);
      refusal != provide::ProviderRefusal::None) {
    error = provide::RefusalReason(refusal);
    LogInfo("provide: start_provider refused (mode={}): {}", provide::ToString(mode),
            error);
    return false;
  }

  // The network country the app read: in place for a device that keeps running,
  // and in force before a new one is built.
  SetNetworkCountry(request.network_country_code, request.network_country_source);

  // The same request again — a relaunched app adopting the provider an earlier
  // run left, or a reconcile after a mode change: keep the device and apply the
  // mode in place.
  if (providerDevice_ && proto::SameProviderDevice(providerRequest_, request)) {
    try {
      providerDevice_->setProvideControlMode(request.provide_mode);
    } catch (const std::exception&) {
      error = "the provide mode could not be applied";
      LogError("provide: applying mode {} to the running provider-only device failed",
               provide::ToString(mode));
      return false;
    }
    providerRequest_.provide_mode = request.provide_mode;
    ReadProviderFactsLocked();
    PublishStatusLocked();
    LogInfo("provide: the provider-only device keeps running (mode={} tier={})",
            provide::ToString(mode), providerTier_);
    return true;
  }

  RetireProviderDeviceLocked();
  // A standalone log upload device runs under the same identity; its upload
  // goes on without it (StopLocked says why).
  RetireLogUploadDeviceLocked();
  const char* step = "network space";
  try {
    providerSpace_ = ImportNetworkSpaceLocked(request.network_space_json);
    step = "device";
    providerDevice_ = std::make_unique<urnet::DeviceLocal>(
        NewDeviceLocked(*providerSpace_, request.by_jwt, request.device_description,
                        request.device_spec, request.app_version, request.instance_id,
                        "provide:"));
    step = "provider transport policy";
    if (!request.provider_transport_settings_json.empty()) {
      providerDevice_->setProviderTransportSettings(
          nlohmann::json::parse(request.provider_transport_settings_json)
              .get<urnet::TransportSettings>());
    }
    step = "provide mode";
    providerDevice_->setProvideControlMode(request.provide_mode);
    providerRequest_ = request;
    ReadProviderFactsLocked();
    // What the app shows for it while disconnected, and the network changes it
    // is told about: both best effort, neither a reason to stop providing.
    OpenProviderStatsLocked();
    WatchProviderNetworkLocked();
    PublishStatusLocked();
    LogInfo("provide: PROVIDING WITHOUT A TUNNEL (mode={} tier={} network_key={} "
            "client_id={}). No wintun adapter, route, dns entry, firewall policy or "
            "device rpc listener exists for it: this machine's own traffic is routed "
            "exactly as it would be without URnetwork.",
            provide::ToString(mode), providerTier_, providerNetworkKey_,
            providerDevice_->getClientId());
    return true;
  } catch (const std::exception&) {
    // The stage names the failure, as ActivateCapture's does, without copying an
    // SDK message that can carry endpoints or identifiers into the reply.
    error = std::string("the provider could not be started at the ") + step + " step";
    LogError("provide: stage=provider-only outcome=failed component={}", step);
    RetireProviderDeviceLocked();
    return false;
  }
}

bool TunnelController::StopProvider() {
  // A wedged lock means a bring-up is holding it, and every bring-up opens by
  // retiring the provider-only device itself.
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_for(kStopLockBudget)) {
    LogWarn("provide: stop_provider could not take the session lock within {}ms",
            kStopLockBudget.count());
    return false;
  }
  RetireProviderDeviceLocked();
  return true;
}

bool TunnelController::SetProvideExtender(bool on, std::string& error) {
  // Timed, for StartProvider's reason: a wedged bring-up must not hold the
  // control pipe. The app then shows the setting as it stands.
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_for(kStopLockBudget)) {
    error = "a tunnel operation is in progress";
    LogWarn("provide: set_provide_extender refused: the session lock was not free "
            "within {}ms",
            kStopLockBudget.count());
    return false;
  }
  const provide::ExtenderSettingTarget target = provide::ExtenderSettingTargetFor(
      providerDevice_ != nullptr, device_.has_value(), spaceManager_ && lastSpaceKey_);
  try {
    switch (target) {
      case provide::ExtenderSettingTarget::ProviderDevice:
        // Persisted in its space and applied at once: the role starts or stops.
        providerDevice_->setProvideExtender(on);
        RefreshProviderExtenderLocked();
        break;
      case provide::ExtenderSettingTarget::SessionDevice:
        // The session's DeviceRemote hears it through its status listener.
        device_->setProvideExtender(on);
        break;
      case provide::ExtenderSettingTarget::NetworkSpace: {
        // The space spaceManager_ keeps for the last device's key: the next
        // import of that key reuses it, or replaces it with one that reads the
        // file this writes.
        urnet::NetworkSpace space = spaceManager_->getNetworkSpace(lastSpaceKey_);
        if (!space) {
          error = "the last device's network space is gone";
          LogWarn("provide: set_provide_extender refused: {}", error);
          return false;
        }
        space.getAsyncLocalState().getLocalState().setProvideExtender(on);
        break;
      }
      case provide::ExtenderSettingTarget::None:
        error = "no device has run in this service yet, so there is no network space to keep the "
                "setting in";
        LogWarn("provide: set_provide_extender refused: {}", error);
        return false;
    }
  } catch (const std::exception&) {
    // Named by its target, as StartProvider names its step, without copying an
    // SDK message into the reply.
    error = std::string("the provide extender setting could not be written to ") +
            provide::ToString(target);
    LogError("provide: stage=set-provide-extender outcome=failed target={}",
             provide::ToString(target));
    return false;
  }
  LogInfo("provide: provide extender {} ({})", on ? "on" : "off", provide::ToString(target));
  return true;
}

bool TunnelController::SetMeasurementsEnabled(bool on, std::string& error) {
  // Timed, as SetProvideExtender's: a wedged bring-up must not hold the control
  // pipe. The app then shows the setting as it stands.
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_for(kStopLockBudget)) {
    error = "a tunnel operation is in progress";
    LogWarn("provide: set_measurements_enabled refused: the session lock was not free "
            "within {}ms",
            kStopLockBudget.count());
    return false;
  }
  // the provide extender setting's rule (Common/ProvideLifecycle.h): the same
  // space holds both, and the same devices read them
  const provide::ExtenderSettingTarget target = provide::ExtenderSettingTargetFor(
      providerDevice_ != nullptr, device_.has_value(), spaceManager_ && lastSpaceKey_);
  try {
    switch (target) {
      case provide::ExtenderSettingTarget::ProviderDevice:
        // persisted in its space and applied at once: the measurement API starts
        // or stops, and the operator withdraws or restores the name
        providerDevice_->setMeasurementsEnabled(on);
        RefreshProviderMeasurementsLocked();
        break;
      case provide::ExtenderSettingTarget::SessionDevice:
        device_->setMeasurementsEnabled(on);
        break;
      case provide::ExtenderSettingTarget::NetworkSpace: {
        urnet::NetworkSpace space = spaceManager_->getNetworkSpace(lastSpaceKey_);
        if (!space) {
          error = "the last device's network space is gone";
          LogWarn("provide: set_measurements_enabled refused: {}", error);
          return false;
        }
        space.getAsyncLocalState().getLocalState().setMeasurementsEnabled(on);
        break;
      }
      case provide::ExtenderSettingTarget::None:
        error = "no device has run in this service yet, so there is no network space to keep the "
                "setting in";
        LogWarn("provide: set_measurements_enabled refused: {}", error);
        return false;
    }
  } catch (const std::exception&) {
    error = std::string("the measurements setting could not be written to ") +
            provide::ToString(target);
    LogError("provide: stage=set-measurements-enabled outcome=failed target={}",
             provide::ToString(target));
    return false;
  }
  LogInfo("provide: lag.dev measurements {} ({})", on ? "on" : "off", provide::ToString(target));
  return true;
}

TunnelController::ExtenderResetResult TunnelController::ResetExtenders(
    const proto::ResetExtenders& request) {
  ExtenderResetResult result;
  const auto key = proto::SpaceKeyOf<urnet::NetworkSpaceKey>(request);
  urnet::NetworkSpace space;
  {
    // Timed, for StartProvider's reason: a wedged bring-up must not hold the
    // control pipe. Busy, so the app sends the reset again once the operation
    // holding the lock ends; the next import of the space carries it anyway.
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    if (!lock.try_lock_for(kStopLockBudget)) {
      result.busy = true;
      result.error = "a tunnel operation is in progress";
      LogWarn("tunnel: reset_extenders refused: the session lock was not free within {}ms",
              kStopLockBudget.count());
      return result;
    }
    // A handle of 0 is the sdk's nil: the manager holds no space under the key.
    try {
      if (spaceManager_) space = spaceManager_->getNetworkSpace(key);
    } catch (const std::exception&) {
      result.error = "the network space could not be read";
      LogError("tunnel: stage=reset-extenders outcome=failed step=lookup");
      return result;
    }
  }
  if (space) {
    // With the session lock released: the reset stops and joins the space's
    // extender network client and node before it starts their replacements.
    try {
      result.reset = space.applyExtenderReset(request.extender_reset_id);
    } catch (const std::exception&) {
      result.error = "the extender reset could not be applied";
      LogError("tunnel: stage=reset-extenders outcome=failed step=apply");
      return result;
    }
  }
  LogInfo("tunnel: extenders reset space={}/{} outcome={}", request.host_name,
          request.env_name,
          !space ? "not-held" : (result.reset ? "reset" : "already-applied"));
  result.ok = true;
  return result;
}

TunnelController::IngressDnsResult TunnelController::SetIngressDns(
    const proto::SetIngressDns& request) {
  IngressDnsResult result;
  const auto key = proto::SpaceKeyOf<urnet::NetworkSpaceKey>(request);
  urnet::NetworkSpace space;
  {
    // Timed, for StartProvider's reason: a wedged bring-up must not hold the
    // control pipe. Busy, so the app sends the values again once the operation
    // holding the lock ends; the next import of the space carries them anyway.
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    if (!lock.try_lock_for(kStopLockBudget)) {
      result.busy = true;
      result.error = "a tunnel operation is in progress";
      LogWarn("tunnel: set_ingress_dns refused: the session lock was not free within {}ms",
              kStopLockBudget.count());
      return result;
    }
    // A handle of 0 is the sdk's nil: the manager holds no space under the key.
    try {
      if (spaceManager_) space = spaceManager_->getNetworkSpace(key);
    } catch (const std::exception&) {
      result.error = "the network space could not be read";
      LogError("tunnel: stage=ingress-dns outcome=failed step=lookup");
      return result;
    }
  }
  if (space) {
    // With the session lock released: each setter swaps the space's DoH cache
    // in place, which joins the lookups in flight on the one it replaces. The
    // setters are the app's own (SdkHost::SetIngressCountryCode,
    // SetControlDohUrls), so the space normalizes and validates as it did
    // there, and a value it already holds changes nothing.
    try {
      (void)space.setIngressCountryCode(request.ingress_country_code);
      const std::string errorId =
          space.setControlDohUrls(urnet::StringList(request.control_doh_urls));
      if (!errorId.empty()) {
        result.error = "the bootstrap DNS-over-HTTPS servers were refused: " + errorId;
        LogError("tunnel: stage=ingress-dns outcome=failed step=doh-servers error_id={}",
                 errorId);
        return result;
      }
    } catch (const std::exception&) {
      result.error = "the ingress dns could not be applied";
      LogError("tunnel: stage=ingress-dns outcome=failed step=apply");
      return result;
    }
    result.held = true;
  }
  LogInfo("tunnel: ingress dns space={}/{} outcome={}", request.host_name, request.env_name,
          space ? "applied" : "not-held");
  result.ok = true;
  return result;
}

void TunnelController::RetireProviderDeviceLocked() {
  if (!providerDevice_ && !providerSpace_) return;
  // The statistics first, under their own lock, so a get_provider_stats served
  // from here on says that nothing runs.
  std::optional<urnet::ContractViewController> statsVc;
  urnet::Sub peersSub;
  urnet::Sub extenderSub;
  {
    std::scoped_lock lock(providerStatsMutex_);
    statsVc = std::move(providerStatsVc_);
    providerStatsVc_.reset();
    peersSub = std::move(providerPeersSub_);
    extenderSub = std::move(providerExtenderSub_);
    providerPeers_.reset();
    providerClientId_.clear();
    providerExtender_.reset();
    providerExtenderSetting_ = false;
    providerMeasurementsSetting_ = true;
  }
  // The network watch's handlers are dropped on this thread, as
  // TearDownSessionLocked drops the session monitor's, so the monitor the
  // worker inherits can only unregister itself.
  if (providerEgress_) {
    providerEgress_->SetOnNetworkEvent(nullptr);
    providerEgress_->SetOnNetworkQualityEvent(nullptr);
  }
  // Moved into locals and then reset, for the reason TearDownSessionLocked
  // spells out: a moved-from engaged optional still tests true.
  auto egress = std::move(providerEgress_);
  providerEgress_.reset();
  auto network = std::move(providerNetwork_);
  providerNetwork_.reset();
  auto device = std::move(providerDevice_);
  providerDevice_.reset();
  auto space = std::move(providerSpace_);
  providerSpace_.reset();
  const provide::ControlMode mode = provide::ControlModeFrom(providerRequest_.provide_mode);
  providerRequest_ = proto::StartProvider{};
  providerTier_ = 0;
  providerNetworkKey_ = false;
  // Published before the bounded close, as RevertMachineStateLocked publishes
  // released ownership: the app must not keep showing a provider that is going.
  PublishStatusLocked();
  LogInfo("provide: retiring the provider-only device (mode={})", provide::ToString(mode));
  const auto started = std::chrono::steady_clock::now();
  const bool finished = RunBounded(
      kSdkTeardownBudget,
      [egress = std::move(egress), network = std::move(network), peersSub = std::move(peersSub),
       extenderSub = std::move(extenderSub), statsVc = std::move(statsVc),
       device = std::move(device), space = std::move(space), flight = logUploadFlight_]() mutable {
        // After this returns no further OS observation reaches the notifier.
        if (egress) egress->Stop();
        egress.reset();
        // Ends the notifier's thread, waiting out a call into the device that is
        // already running: the device has to outlive it, so it goes first.
        network.reset();
        // Assigned, never reset(): Sub::reset() releases the handle without
        // unsubscribing (PacketPump.cpp).
        peersSub = urnet::Sub{};
        extenderSub = urnet::Sub{};
        // The typed close, which releases the controller from the device.
        if (device && statsVc) device->closeContractViewController(*statsVc);
        statsVc.reset();
        if (device) device->close();
        // A log upload's call may still be on it: the flight then keeps it
        // until the call returns, and releases it.
        if (device) {
          const uint64_t deviceHandle = device->handle();
          flight->KeepUntilReturned(deviceHandle,
                                    std::shared_ptr<urnet::DeviceLocal>(std::move(device)));
        }
        device.reset();
        space.reset();
      },
      // The worker owns a DeviceLocal under this device's identity. While it is
      // outstanding a second device — a Connect's — would run beside it, so its
      // abandonment refuses a start exactly as the session teardown's does, and
      // the next start restarts the service clean instead.
      AbandonHazard::HoldsSessionDevice);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();
  if (!finished) {
    LogError("provide: the provider-only device did not close inside its {}ms "
             "budget and is LEFT closing on its own thread; a start is refused "
             "until it finishes",
             kSdkTeardownBudget.count());
  } else if (ms > 500) {
    LogWarn("provide: closing the provider-only device took {}ms", ms);
  }
}

void TunnelController::ReadProviderFactsLocked() {
  providerTier_ = 0;
  providerNetworkKey_ = false;
  if (!providerDevice_) return;
  try {
    providerTier_ = providerDevice_->getProvideMode();
    if (auto keys = providerDevice_->getProvideSecretKeys()) {
      for (const auto& key : *keys) {
        if (key.provide_mode == urnet::ProvideModeNetwork) {
          providerNetworkKey_ = true;
          break;
        }
      }
    }
  } catch (const std::exception&) {
    // A status must still be publishable; it then claims less (tier 0).
    LogWarn("provide: reading the provider-only device's tier failed");
  }
}

namespace {
// The DeviceRemote path's peers (SdkHost::ReadStats counts them): the connected
// network peers, none when the device reports no peers at all. Each keeps only
// what the app's peer rows show and connect with (App/PeerLocation.h): the
// control pipe is open to every signed-in user of this machine, unlike a
// session's mTLS device rpc, and a peer's principal and roles are not for them.
urnet::NetworkPeerList ConnectedPeers(std::optional<urnet::NetworkPeers> peers) {
  urnet::NetworkPeerList rows;
  if (!peers || !peers->Connected) return rows;
  rows.reserve(peers->Connected->size());
  for (auto& peer : *peers->Connected) {
    urnet::NetworkPeer row;
    row.ClientId = std::move(peer.ClientId);
    row.ProvideEnabled = peer.ProvideEnabled;
    row.DeviceName = std::move(peer.DeviceName);
    row.DeviceSpec = std::move(peer.DeviceSpec);
    rows.push_back(std::move(row));
  }
  return rows;
}
}  // namespace

void TunnelController::OpenProviderStatsLocked() {
  if (!providerDevice_) return;
  std::string clientId;
  std::optional<urnet::ContractViewController> vc;
  urnet::Sub peersSub;
  auto peers = std::make_shared<LatestNetworkPeers>();
  try {
    clientId = providerDevice_->getClientId();
    vc.emplace(providerDevice_->openContractViewController());
    // Subscribed before the first read, so a change in between is either in
    // that read or delivered after it. The listener holds a share of the
    // peers, never this object: a callback already running when the retire
    // unsubscribes still finds it.
    peersSub = providerDevice_->addNetworkPeersChangeListener(
        [peers](std::optional<urnet::NetworkPeers> networkPeers) {
          peers->Store(ConnectedPeers(std::move(networkPeers)));
        });
    peers->Store(ConnectedPeers(providerDevice_->getNetworkPeers()));
  } catch (const std::exception&) {
    LogWarn("provide: the provider-only device's statistics could not be opened; the "
            "app shows no client count or provider plots while disconnected");
    peersSub = urnet::Sub{};
    try {
      if (vc) providerDevice_->closeContractViewController(*vc);
    } catch (const std::exception&) {
    }
    return;
  }
  // The provider extender role (EXTENDER.md N2, N7), as a session's
  // DeviceRemote reports it: a status listener, subscribed before the first
  // read for the peers listener's reason and holding a share of the reading,
  // never this object, and the setting read beside it. The setting is read
  // here and again after set_provide_extender writes it through this device
  // (RefreshProviderExtenderLocked); a session's device, the only other
  // writer, never runs beside this one. Its own best effort: a failure costs
  // the extender row, plot and switch, which then read as the role
  // unsupported, and nothing else.
  auto extender = std::make_shared<LatestExtenderProvideStatus>();
  urnet::Sub extenderSub;
  bool extenderSetting = false;
  // the lag.dev measurements setting rides on the role (lag.dev/PLAN.md C4):
  // read with it, and reported only beside it
  bool measurementsSetting = true;
  try {
    extenderSub = providerDevice_->addExtenderProvideStatusChangeListener(
        [extender](std::optional<urnet::ExtenderProvideStatus> status) {
          extender->Store(std::move(status));
        });
    extender->Store(providerDevice_->getExtenderProvideStatus());
    extenderSetting = providerDevice_->getProvideExtender();
    measurementsSetting = providerDevice_->getMeasurementsEnabled();
  } catch (const std::exception&) {
    LogWarn("provide: the provider-only device's extender status could not be read; the "
            "app shows no extender row or plot while disconnected");
    extenderSub = urnet::Sub{};
    extender.reset();
  }
  std::scoped_lock lock(providerStatsMutex_);
  providerStatsVc_ = std::move(vc);
  providerPeersSub_ = std::move(peersSub);
  providerPeers_ = std::move(peers);
  providerClientId_ = std::move(clientId);
  providerExtenderSub_ = std::move(extenderSub);
  providerExtender_ = std::move(extender);
  providerExtenderSetting_ = extenderSetting;
  providerMeasurementsSetting_ = measurementsSetting;
}

void TunnelController::RefreshProviderExtenderLocked() {
  // The device derives the status from the setting it now holds (off, or
  // setting up while it provides), so this reading already shows the write; a
  // listener push that was in flight is replaced by the next one within its
  // epoch.
  std::optional<urnet::ExtenderProvideStatus> status;
  bool setting = false;
  try {
    status = providerDevice_->getExtenderProvideStatus();
    setting = providerDevice_->getProvideExtender();
  } catch (const std::exception&) {
    LogWarn("provide: re-reading the provider-only device's extender role failed; its next "
            "status push reports it");
    return;
  }
  std::scoped_lock lock(providerStatsMutex_);
  // a role whose reading never opened is not reported, and stays so
  if (!providerExtender_) return;
  providerExtender_->Store(std::move(status));
  providerExtenderSetting_ = setting;
}

void TunnelController::RefreshProviderMeasurementsLocked() {
  bool setting = true;
  try {
    setting = providerDevice_->getMeasurementsEnabled();
  } catch (const std::exception&) {
    LogWarn("provide: re-reading the provider-only device's measurements setting failed; "
            "its next statistics open reports it");
    return;
  }
  std::scoped_lock lock(providerStatsMutex_);
  // reported only beside the role, as the extender setting is
  if (!providerExtender_) return;
  providerMeasurementsSetting_ = setting;
}

void TunnelController::WatchProviderNetworkLocked() {
  if (!providerDevice_) return;
  // Stable for the device's whole life (see providerDevice_), and the notifier
  // ends before the device is closed (RetireProviderDeviceLocked).
  urnet::DeviceLocal* device = providerDevice_.get();
  try {
    // networkChanged() alone: notifyNetworkChange() is the same seam in this
    // sdk (reliability_controls.go), and a device with no multi client — no
    // tunnel — gets the process-wide transport kick and the DoH recovery from
    // it, which is what a provider-only device needs.
    providerNetwork_ = std::make_unique<NetworkChangeNotifier>(
        [device] {
          LogInfo("provide: the os reported an ip/route change — telling the "
                  "provider-only device the network moved, so its transports re-dial "
                  "now instead of timing out against the old path");
          try {
            device->networkChanged();
          } catch (const std::exception& e) {
            LogWarn("provide: the sdk network-change notification failed: {}", e.what());
          }
        },
        [device] {
          try {
            device->networkQualityChanged();
          } catch (const std::exception& e) {
            LogWarn("provide: the sdk network-quality notification failed: {}", e.what());
          }
        });
    // A zero LUID: no tun exists to exclude, as in rpc-only mode.
    providerEgress_ =
        std::make_unique<EgressMonitor>(NET_LUID{}, EgressMonitor::Binding::ObserveOnly);
    providerEgress_->SetOnNetworkEvent(providerNetwork_->NetworkEventSink());
    providerEgress_->SetOnNetworkQualityEvent(providerNetwork_->NetworkQualitySink());
    providerEgress_->Start();
  } catch (const std::exception&) {
    // What did start is retired with the device.
    LogWarn("provide: network changes will not reach the provider-only device; it "
            "recovers through its transports' timeouts");
  }
}

// See the contract in the header. No session lock and no device call: the copy
// the build and the listeners left, and the controller's sampled state.
proto::ProviderStats TunnelController::ProviderStats() {
  proto::ProviderStats stats;
  std::scoped_lock lock(providerStatsMutex_);
  if (!providerStatsVc_) return stats;
  stats.available = true;
  stats.client_id = providerClientId_;
  // the count and the peers it counts, from one reading
  if (providerPeers_) {
    const urnet::NetworkPeerList connected = providerPeers_->Load();
    stats.client_count = static_cast<int64_t>(connected.size());
    stats.network_peers = connected;
  }
  // Each read on its own, as SdkHost's ReadSdkList reads them: a document one
  // getter cannot decode costs that field, never the reply. Logged once.
  static std::atomic<bool> logged{false};
  const auto read = [&](const char* what, const auto& get) {
    try {
      get();
    } catch (const std::exception& e) {
      if (!logged.exchange(true))
        LogWarn("provide: reading the provider-only device's {} failed: {}", what, e.what());
    }
  };
  read("window", [&] {
    if (const int64_t window = providerStatsVc_->getWindowDurationSeconds(); window > 0)
      stats.window_seconds = window;
  });
  read("throughput", [&] {
    if (auto points = providerStatsVc_->getProviderThroughputPoints())
      stats.provider_points = *points;
  });
  read("transport distribution", [&] {
    if (auto distribution = providerStatsVc_->getProviderTransportDistribution())
      stats.provider_distribution = *distribution;
  });
  read("packet stats", [&] {
    stats.has_provider_stats = providerStatsVc_->getProviderPacketStats().has_value();
  });
  read("extender throughput", [&] {
    if (auto points = providerStatsVc_->getExtenderThroughputPoints())
      stats.extender_points = *points;
  });
  // The extender role, as its listener last said: none when its reading could
  // not be opened, which the app reads as the role unsupported. Beside it, that
  // this service takes the switch's write (SetProvideExtender).
  if (providerExtender_) {
    if (auto status = providerExtender_->Load()) stats.extender_provide_status = *status;
    stats.provide_extender = providerExtenderSetting_;
    stats.provide_extender_writable = true;
    // and the lag.dev measurements setting, which this service writes too
    // (SetMeasurementsEnabled)
    stats.measurements_enabled = providerMeasurementsSetting_;
    stats.measurements_enabled_writable = true;
  }
  return stats;
}

// See the contract in the header. No session lock: the country is a fact about
// the network, not about a session, and its one sdk call is a store.
void TunnelController::SetNetworkCountry(const std::string& code, const std::string& source) {
  const netcountry::Reading reading = netcountry::Normalized(code, source);
  std::scoped_lock lock(networkCountryMutex_);
  if (networkCountry_ && *networkCountry_ == reading) return;
  networkCountry_ = reading;
  urnet::setNetworkCountryCode(reading.code);
  if (reading.code.empty()) {
    LogInfo("tunnel: no network country ({}): extender dials take the extender "
            "hint's country, or the global spoof list",
            reading.source);
  } else {
    LogInfo("tunnel: network country \"{}\" ({}): while the extender hint cannot be "
            "fetched, extender dials front with that country's spoof list",
            reading.code, reading.source);
  }
}

std::optional<netcountry::Reading> TunnelController::NetworkCountry() {
  std::scoped_lock lock(networkCountryMutex_);
  return networkCountry_;
}

// ---- the log upload (upload_logs) -------------------------------------------
//
// "Send feedback with logs" uploads this service's glog files, which is where
// everything support reads about the tunnel, the provider and the network is.
// It used to reach them only through the app's DeviceRemote, i.e. only while a
// session ran. Now the app asks here, connected or not.

TunnelController::LogUploadResult TunnelController::UploadLogs(
    const proto::UploadLogs& request, const std::function<void(std::string_view)>& noteCarrier,
    AppLogHandles appLogFiles) {
  LogUploadResult result;
  // Timed, like StartProvider: a connect attempt wedged inside the SDK holds
  // mutex_, and an upload that waited behind it would hold the control pipe.
  std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
  if (!lock.try_lock_for(kStopLockBudget)) {
    result.error = "a tunnel operation is in progress";
    LogWarn("logs: upload_logs refused: the session lock was not free within {}ms",
            kStopLockBudget.count());
    return result;
  }
  const logupload::Carrier which =
      logupload::CarrierFor(device_.has_value(), providerDevice_ != nullptr);
  const char* carrierName = logupload::ToString(which);
  // Admitted before anything is built: one upload at a time.
  const int64_t uploadId = logUploadFlight_->Begin(which, SteadyMillis());
  if (uploadId == 0) {
    result.busy = true;
    result.error = "a log upload is in flight already";
    LogInfo("logs: upload_logs refused: a log upload is in flight already");
    return result;
  }
  std::shared_ptr<LogUploadDevice> slot;
  uint64_t deviceHandle = 0;
  const char* step = "upload";
  try {
    if (which == logupload::Carrier::Tunnel) {
      deviceHandle = device_->handle();
    } else if (which == logupload::Carrier::Provider) {
      deviceHandle = providerDevice_->handle();
    } else {
      const AbandonedTeardownSweep abandoned = SweepAbandonedTeardowns();
      const logupload::StandaloneRefusal refusal =
          logupload::StandaloneRefusalFor(abandoned.outstanding > 0, SelfRestartPending());
      if (refusal != logupload::StandaloneRefusal::None) {
        logUploadFlight_->Finish(uploadId, logupload::FlightState::Failed);
        result.error = logupload::RefusalReason(refusal);
        LogInfo("logs: upload_logs refused: {}", result.error);
        return result;
      }
      // Neither runs: a device for the upload alone, built as StartProvider
      // builds the provider-only device (the persisted identity, the request's
      // credentials and network space) and nothing after that: no adapter,
      // route, DNS entry, firewall policy, marker or rpc listener. It provides
      // to nobody. One at a time, under the one identity.
      RetireLogUploadDeviceLocked();
      slot = std::make_shared<LogUploadDevice>();
      slot->flight = logUploadFlight_;
      step = "network space";
      slot->space = ImportNetworkSpaceLocked(request.network_space_json);
      step = "device";
      slot->device = std::make_unique<urnet::DeviceLocal>(
          NewDeviceLocked(*slot->space, request.by_jwt, request.device_description,
                          request.device_spec, request.app_version, request.instance_id,
                          "logs:"));
      step = "provide mode";
      slot->device->setProvideControlMode("never");
      logUpload_ = slot;
      deviceHandle = slot->device->handle();
      step = "upload";
    }
  } catch (const std::exception&) {
    // The stage names the failure, as StartProvider's does, without copying an
    // SDK message that can carry endpoints or identifiers into the reply.
    result.error = std::string("the logs could not be uploaded at the ") + step + " step";
    LogError("logs: stage=upload outcome=failed component={}", step);
    logUploadFlight_->Finish(uploadId, logupload::FlightState::Failed);
    // a standalone device built before the failure is closed, not just released
    if (slot) {
      CloseLogUploadDevice(slot);
      logUpload_.reset();
    }
    return result;
  }
  // Into the files being uploaded, before the upload's thread zips them: which
  // device carried the upload tells support whether a tunnel was up when it
  // was sent.
  if (noteCarrier) noteCarrier(carrierName);
  // The standalone device's waiter learns that the upload reported from the
  // upload's callback, and retires the device then.
  std::function<void()> reported;
  if (slot) {
    reported = [slot] {
      {
        std::scoped_lock slotLock(slot->mutex);
        slot->uploadReported = true;
      }
      slot->reported.notify_all();
    };
  }
  // The zip and the post, on the upload's own thread. It holds the flight, the
  // device's handle, the waiter's signal and the app's log files, and nothing
  // else of this object; the flight keeps the device alive until the call
  // returns (each teardown hands it over), and the call's callback ends the
  // upload in it, whose finish hook pushes the status.
  const size_t appLogFileCount = appLogFiles.Size();
  logUploadFlight_->Run(uploadId, deviceHandle,
                        [flight = logUploadFlight_, uploadId, deviceHandle,
                         feedbackId = request.feedback_id, carrierName, reported,
                         appLogFiles = std::make_shared<AppLogHandles>(std::move(appLogFiles))] {
                          UploadLogsOnDevice(flight, uploadId, deviceHandle, feedbackId,
                                             carrierName, reported, *appLogFiles);
                        });
  if (slot) {
    // The waiter retires the standalone device once its upload reports, or at
    // the bound if it never does. It owns a share of the slot and nothing else,
    // so it may outlive this controller; whoever takes the device first closes
    // it (CloseLogUploadDevice).
    std::thread([slot] {
      RunGuarded("log-upload-retire", [&] {
        {
          std::unique_lock<std::mutex> slotLock(slot->mutex);
          slot->reported.wait_for(slotLock, logupload::kStandaloneDeviceMaxLifetime,
                                  [&] { return slot->uploadReported || !slot->device; });
        }
        CloseLogUploadDevice(slot);
      });
    }).detach();
  }
  result.ok = true;
  result.carrier = carrierName;
  result.uploadId = uploadId;
  LogInfo("logs: uploading this service's logs for a feedback ({} device), with {} of the "
          "app's log files",
          carrierName, appLogFileCount);
  return result;
}

void TunnelController::CloseLogUploadDevice(const std::shared_ptr<LogUploadDevice>& slot) {
  std::unique_ptr<urnet::DeviceLocal> device;
  std::optional<urnet::NetworkSpace> space;
  {
    std::scoped_lock slotLock(slot->mutex);
    device = std::move(slot->device);
    slot->device.reset();
    space = std::move(slot->space);
    slot->space.reset();
  }
  // a waiter still waiting finds the slot empty and leaves
  slot->reported.notify_all();
  if (!device) return;
  LogInfo("logs: retiring the device that carried a log upload");
  const bool finished = RunBounded(
      kSdkTeardownBudget,
      [device = std::move(device), space = std::move(space), flight = slot->flight]() mutable {
        device->close();
        // The upload's call may still be on it: the flight then keeps it until
        // the call returns, and releases it.
        if (flight) {
          const uint64_t deviceHandle = device->handle();
          flight->KeepUntilReturned(deviceHandle,
                                    std::shared_ptr<urnet::DeviceLocal>(std::move(device)));
        }
        device.reset();
        space.reset();
      },
      // A DeviceLocal under this device's identity, like the provider-only
      // device's retire: abandoned, it refuses a start until it finishes.
      AbandonHazard::HoldsSessionDevice);
  if (!finished) {
    LogError("logs: the log upload device did not close inside its {}ms budget and is "
             "left closing on its own thread; a start is refused until it finishes",
             kSdkTeardownBudget.count());
  }
}

void TunnelController::RetireLogUploadDeviceLocked() {
  if (!logUpload_) return;
  CloseLogUploadDevice(logUpload_);
  logUpload_.reset();
}

// See the contract in the header. NO SESSION LOCK: a copy of the snapshot that
// was published at the last write, plus the two publishers that are lock-free by
// design and are therefore read live. The app's whole connect/disconnect
// decision now rides on this call, so it has to be answerable while a connect is
// wedged holding mutex_ — the same reason Stop() is bounded rather than
// blocking.
proto::TunnelStatus TunnelController::Status() {
  proto::TunnelStatus s;
  {
    std::scoped_lock lock(statusMirrorMutex_);
    s = statusMirror_;
  }
  // Invariants, filled here rather than trusted to the mirror so that a status
  // served before the first publish still identifies this service. From the
  // cached copy: urnet::version() is a cgo call, and this function's contract
  // is that it answers while the SDK is wedged. (It is also EMPTY in this SDK
  // build, which is why nothing may use it to decide anything — see
  // ServiceClient::GetState.)
  s.service_version = sdkVersion_;
  s.protocol_version = proto::kProtocolVersion;
  // Live, from the lock-free publishers: still correct for a status served
  // WHILE a failsafe teardown is in flight, which is the case they exist for.
  s.stop_reason = lastStopReason_.load();
  s.failsafe_armed = deadTunnelWatchdog_.FailsafeArmed();
  // The binding as the monitor holds it now, under the gate ComposeStatusLocked
  // reports one by: the mirror's copy is the one bound at the last publish, and
  // the egress moving publishes nothing (OnEgressChanged).
  if (s.routes_installed) {
    s.egress_index4 = liveEgressIndex4_.load();
    s.egress_index6 = liveEgressIndex6_.load();
  }
  const int64_t upSince = upSinceMirror_.load();
  s.tunnel_local_up_millis = upSince ? (NowMillis() - upSince) : 0;
  // The log upload in flight, off the flight's own lock (never the session's).
  const logupload::Flight::Reading upload = logUploadFlight_->Read(SteadyMillis());
  s.log_upload_id = upload.id;
  s.log_upload_state = logupload::ToString(upload.state);
  s.log_upload_carrier = upload.id == 0 ? "" : logupload::ToString(upload.carrier);
  return s;
}

proto::TunnelStatus TunnelController::StatusLocked() {
  proto::TunnelStatus s = ComposeStatusLocked();
  upSinceMirror_.store(upSinceMillis_);
  {
    std::scoped_lock lock(statusMirrorMutex_);
    statusMirror_ = s;
  }
  return s;
}

void TunnelController::PublishStatusLocked() { (void)StatusLocked(); }

void TunnelController::RepublishMachineFactsLockFree(bool routesReverted) {
  // Read the firewall FIRST: WfpPolicy has its own lock and an Apply can hold
  // it across a BFE call, and statusMirrorMutex_'s whole guarantee is that it is
  // never held across anything that can block.
  const std::string wfp = ToString(wfp_.State());
  std::scoped_lock lock(statusMirrorMutex_);
  statusMirror_.wfp_state = wfp;
  if (routesReverted) {
    statusMirror_.routes_installed = false;
    statusMirror_.dns_applied = false;
    // Relinquish capture ownership after the best-effort route-only escape.
    // The app must unbind; neither these flags nor the index reset verify DNS
    // removal, which that escape deliberately leaves to adapter teardown.
    statusMirror_.egress_index4 = 0;
    statusMirror_.egress_index6 = 0;
  }
}

proto::TunnelStatus TunnelController::ComposeStatusLocked() {
  proto::TunnelStatus s;
  s.state = state_;
  s.mode = startMode_;
  // Report configuration ownership, including a partial Apply, not a fresh OS
  // route-table query. Releasing the owner does not certify kernel cleanup.
  s.routes_installed = netConfig_ != nullptr;
  // Report that owner's DNS-apply result, not a resolver readback. False after
  // cleanup means the flag was cleared, not that removal was independently read.
  s.dns_applied = netConfig_ != nullptr && netConfig_->DnsApplied();
  // "Is leak prevention actually running." Off while the tunnel is up is a
  // materially different state from a protected one — it is what an unelevated
  // or failed install looks like — so the app gets to see it rather than infer
  // protection from `state == up`.
  s.wfp_state = ToString(wfp_.State());
  s.rpc_listen_hostport = rpcHostPort_;
  s.instance_id = activeInstanceId_;
  s.rpc_session_id = rpcSessionId_;
  s.error = error_;
  // The CACHED version, not a fresh urnet::version(). This composition now runs
  // inside SetStateLocked, which is on the teardown path AHEAD of the route
  // revert and may not block; a cgo call into a Go runtime that a wedged
  // connect is sitting on is exactly the thing that can.
  s.service_version = sdkVersion_;
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
  // The provider-only device, from the members that own it: the request it was
  // built from and the tier and key ReadProviderFactsLocked read off it. No SDK
  // call here, for the reason service_version above is the cached copy.
  s.provider_running = providerDevice_ != nullptr;
  if (providerDevice_) {
    s.provider_control_mode = providerRequest_.provide_mode;
    s.provider_mode = providerTier_;
    s.provider_network_key = providerNetworkKey_;
  }
  // stop_reason and failsafe_armed are deliberately NOT composed here: Status()
  // overlays them from their own lock-free publishers, so a status served
  // between a failsafe teardown starting and its publish still carries the
  // reason. Same for tunnel_local_up_millis, which is aged against a live clock.
  return s;
}

}  // namespace urnw
