// Whether this device provides while there is no tunnel session, for each
// provide control mode: the one lifecycle decision the app and the service
// both apply to the provider-only device (support inbox 1521: a provider that
// earns nothing and is told nothing).
//
// The defect this answers. The provider is the service's DeviceLocal, and that
// device existed only inside a tunnel session. Disconnect sends stop_tunnel
// (it has to: the capture routes and the firewall policy would otherwise stay
// on the machine, ConnectAction.h bug A), TunnelController::StopLocked
// destroys the DeviceLocal with the tunnel, and the launch path never starts a
// session without a Connect gesture (SdkHost::BootstrapSession, D8). So on
// Windows the provider stopped in every mode the moment the user disconnected
// and never started after a launch that did not connect. A user who picked
// Always ("provides to everyone whenever the app is running") earned only
// while connected, and Auto never served the user's own devices while idle.
//
// What the other platforms do. Android and Apple keep their packet tunnel, and
// with it the DeviceLocal, running whenever providing is enabled, and the sdk
// decides the tier. Windows keeps the DeviceLocal without the tunnel instead:
// while there is no tunnel session the service runs a provider-only device
// with no wintun adapter, no route, no DNS entry, no firewall (WFP) policy and
// no device RPC listener, so providing never changes how this machine's own
// traffic is routed, and no client can drive the device.
//
// The SDK semantics, mirrored and never reinterpreted
// (DeviceLocal.applyProvideControlModeWithLock, sdk/device_local.go):
//   never   -> no providing
//   always  -> public
//   network -> network (the user's own devices), connected or not
//   auto    -> public while connected, network while not
//   anything else -> no providing (the sdk's conservative default)
// "manual" defers to an explicitly set provide mode that lives in the device,
// which does not exist while disconnected. This app never offers it (the
// Connect page and onboarding offer auto, always, network and never), so it is
// treated like any other unknown mode and gets no provider-only device.
//
// Pure and header-only, constexpr, no Windows headers and no allocation, like
// ConnectAction.h: tools/provide-lifecycle-tests.cpp runs it on any host,
// TunnelController::StartProvider refuses with ProviderStartRefusal,
// TunnelController::SetProvideExtender writes where ExtenderSettingTargetFor
// says, and SdkHost::ReconcileProviderLocked asks DisconnectedProviderStep
// what to send.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace urnw::provide {

enum class ControlMode { Never, Always, Network, Auto, Unknown };

constexpr ControlMode ControlModeFrom(std::string_view mode) {
  if (mode == "never") return ControlMode::Never;
  if (mode == "always") return ControlMode::Always;
  if (mode == "network") return ControlMode::Network;
  if (mode == "auto") return ControlMode::Auto;
  return ControlMode::Unknown;
}

// For logs: the mode's own name, never the raw string a request carried.
constexpr const char* ToString(ControlMode mode) {
  switch (mode) {
    case ControlMode::Never: return "never";
    case ControlMode::Always: return "always";
    case ControlMode::Network: return "network";
    case ControlMode::Auto: return "auto";
    case ControlMode::Unknown: break;
  }
  return "unknown";
}

// The live provide tier, with the sdk's values (urnet::ProvideModeNone,
// ProvideModeNetwork, ProvideModePublic), so a tier and a status field compare
// directly.
enum class Tier : int64_t { None = 0, Network = 1, Public = 3 };

// The tier the sdk applies for `mode`. `connected` is the sdk's own test: a
// destination is set on a tunnel session's device.
constexpr Tier SdkTierFor(ControlMode mode, bool connected) {
  switch (mode) {
    case ControlMode::Always:
      return Tier::Public;
    case ControlMode::Network:
      return Tier::Network;
    case ControlMode::Auto:
      return connected ? Tier::Public : Tier::Network;
    case ControlMode::Never:
    case ControlMode::Unknown:
      break;
  }
  return Tier::None;
}

// The lifecycle decision: does this device provide? Connected, the tunnel
// session's DeviceLocal is the provider. Disconnected, a provider-only device
// runs exactly when the answer is true.
constexpr bool ProviderRuns(ControlMode mode, bool connected) {
  return SdkTierFor(mode, connected) != Tier::None;
}

// ---- the service's half: may a provider-only device start now? -------------

// What TunnelController holds at the moment a start_provider is served.
struct ServiceProviderState {
  // A tunnel or rpc-only session exists, is starting or is stopping. Its own
  // DeviceLocal is the provider, under the same identity.
  bool tunnelSession = false;
  // A firewall policy is in force with no tunnel session: the kill switch's
  // armed floor after an unexpected drop. Armed permits this service's own
  // image so a reconnect can resolve and dial, and a provider would leave
  // through that permit; the floor stays the only thing in force until the
  // user reconnects or lifts it.
  bool firewallInForce = false;
  // An abandoned teardown still holds a DeviceLocal under this identity
  // (StopBudget.h SweepAbandonedTeardowns).
  bool deviceStillHeld = false;
  // This process has committed to ending itself so the SCM restarts it.
  bool restartPending = false;
};

enum class ProviderRefusal {
  None,
  ModeDoesNotProvide,
  TunnelSession,
  KillSwitchArmed,
  DeviceStillHeld,
  RestartPending,
};

// Why the service refuses a provider-only device, or None. Every refusal is
// decided before anything is built.
constexpr ProviderRefusal ProviderStartRefusal(ControlMode mode,
                                               const ServiceProviderState& state) {
  if (!ProviderRuns(mode, /*connected=*/false)) return ProviderRefusal::ModeDoesNotProvide;
  if (state.tunnelSession) return ProviderRefusal::TunnelSession;
  if (state.firewallInForce) return ProviderRefusal::KillSwitchArmed;
  if (state.deviceStillHeld) return ProviderRefusal::DeviceStillHeld;
  if (state.restartPending) return ProviderRefusal::RestartPending;
  return ProviderRefusal::None;
}

// The refusal as the control reply's error. English, for the log and the
// developer surfaces; the app acts on the reply's `ok`, never on this text.
constexpr const char* RefusalReason(ProviderRefusal refusal) {
  switch (refusal) {
    case ProviderRefusal::ModeDoesNotProvide:
      return "the provide mode does not provide while disconnected";
    case ProviderRefusal::TunnelSession:
      return "a tunnel session exists, and its device is already the provider";
    case ProviderRefusal::KillSwitchArmed:
      return "the kill switch is holding this machine blocked; providing resumes "
             "after you reconnect or turn the kill switch off";
    case ProviderRefusal::DeviceStillHeld:
      return "a previous teardown is still holding this device's identity";
    case ProviderRefusal::RestartPending:
      return "this service is restarting itself";
    case ProviderRefusal::None:
      break;
  }
  return "";
}

// ---- the service's half: where the extender switch's write goes -------------

// set_provide_extender writes the provider extender setting, which belongs to
// the network space (`.provide_extender`): every device reads it from its
// space, a session's when it starts as much as the provider-only device's. So
// the write goes through the device that runs, which persists it and applies
// it at once, and with none into the space the last device ran in, which the
// next start of either imports. The two devices never run side by side.
// set_measurements_enabled writes the lag.dev measurements setting
// (`.measurements_enabled`, lag.dev/PLAN.md C4) by the same rule: it belongs to
// the same space and rides on the same role.
// set_provide_extender_port_mapping writes the router port mapping setting
// (`.provide_extender_port_mapping`, connect EXTENDER.md S8) by the same rule
// too, into the space through the sdk's LocalState when no device runs, so a
// toggle that lands between two devices is the next one's setting.
enum class ExtenderSettingTarget {
  ProviderDevice,  // the provider-only device: the switch's own case
  SessionDevice,   // a session's device that came up after the app asked
  NetworkSpace,    // no device: the space the last device ran in
  None,            // no device has run in this process: refused
};

constexpr ExtenderSettingTarget ExtenderSettingTargetFor(bool providerDevice, bool sessionDevice,
                                                         bool lastSpace) {
  if (providerDevice) return ExtenderSettingTarget::ProviderDevice;
  if (sessionDevice) return ExtenderSettingTarget::SessionDevice;
  if (lastSpace) return ExtenderSettingTarget::NetworkSpace;
  return ExtenderSettingTarget::None;
}

// For logs.
constexpr const char* ToString(ExtenderSettingTarget target) {
  switch (target) {
    case ExtenderSettingTarget::ProviderDevice: return "the provider-only device";
    case ExtenderSettingTarget::SessionDevice: return "the session's device";
    case ExtenderSettingTarget::NetworkSpace: return "the last device's network space";
    case ExtenderSettingTarget::None: break;
  }
  return "nothing";
}

// ---- the service's half: one device across connect and disconnect ----------
//
// The provider-only device and a tunnel session's device carry one identity:
// the persisted key material and the request's jwt, space and description. A
// Connect used to retire the provider-only device and build the session a new
// DeviceLocal, and a Disconnect closed that one for a new provider-only device
// to be built. Every switch closed at once everything the old device held:
// its platform transports, its network peers, and every contract, each close
// posted to the api by itself. On a busy provider that is thousands of posts
// in a few seconds, drawing 429s whose retries dial again, and thousands of
// blocking socket closes, each holding an os thread. So the device now moves
// between the two roles whenever the identity allows, and only what belongs to
// a session (the adapter, the routes, the rpc listener, the destination) comes
// and goes with it.

// The request fields a DeviceLocal is built from, as StartProvider and
// StartTunnel both carry them. The credential counts by the client it names,
// never by the token (proto::JwtSubject): tokens are refreshed all the time.
// The app refreshes its token on every connect, in its own process, and every
// device refreshes its own as it starts, so two requests for one device rarely
// carry the same token. Measured: compared token by token, every disconnect
// that kept the session's device rebuilt it 25ms later.
struct DeviceIdentity {
  std::string jwtSubject;
  std::string_view networkSpaceJson;
  std::string_view instanceId;
  std::string_view deviceDescription;
  std::string_view deviceSpec;
  std::string_view appVersion;
};

inline bool SameIdentity(const DeviceIdentity& a, const DeviceIdentity& b) {
  return a.jwtSubject == b.jwtSubject && a.networkSpaceJson == b.networkSpaceJson &&
         a.instanceId == b.instanceId && a.deviceDescription == b.deviceDescription &&
         a.deviceSpec == b.deviceSpec && a.appVersion == b.appVersion;
}

// The fields that tell two identities apart, by request field name only: the
// values are credentials. Empty exactly when SameIdentity holds. The service
// logs it when a start_provider rebuilds the running provider-only device,
// which closes every transport, peer and contract that device holds.
inline std::string DifferingIdentityFields(const DeviceIdentity& a, const DeviceIdentity& b) {
  std::string names;
  const auto note = [&names](bool differs, std::string_view name) {
    if (!differs) return;
    if (!names.empty()) names += ", ";
    names += name;
  };
  note(a.jwtSubject != b.jwtSubject, "by_jwt");
  note(a.networkSpaceJson != b.networkSpaceJson, "network_space_json");
  note(a.instanceId != b.instanceId, "instance_id");
  note(a.deviceDescription != b.deviceDescription, "device_description");
  note(a.deviceSpec != b.deviceSpec, "device_spec");
  note(a.appVersion != b.appVersion, "app_version");
  return names;
}

// Connect: the running provider-only device becomes the session's device when
// it was built for the identity the start carries. Otherwise it is retired and
// the session builds its own, as before.
inline bool PromotesProviderDevice(bool providerDevice, const DeviceIdentity& provider,
                                   const DeviceIdentity& start) {
  return providerDevice && SameIdentity(provider, start);
}

// Disconnect: what the service knows when a session ends.
struct SessionStopFacts {
  // The session holds a DeviceLocal.
  bool sessionDevice = false;
  // The user's disconnect (stop_tunnel). Not the failsafe, not a failed or
  // replaced start, not a logout and not the service stopping: each of those
  // closes the device, as before.
  bool disconnect = false;
  // The device's own provide control mode.
  ControlMode mode = ControlMode::Unknown;
  // After the machine is given back. The kill switch's armed floor outlasts a
  // failsafe stop only, but the rule is ProviderStartRefusal's either way.
  bool firewallInForce = false;
  // Another DeviceLocal under this identity is still held by an abandoned
  // teardown.
  bool deviceStillHeld = false;
  bool restartPending = false;
};

// The session's device stays, as the provider-only device, exactly when a
// provider-only device could start in its place now: the same rule, so the two
// paths cannot disagree about when this machine provides.
constexpr bool KeepsSessionDevice(const SessionStopFacts& facts) {
  if (!facts.sessionDevice || !facts.disconnect) return false;
  ServiceProviderState state;
  state.firewallInForce = facts.firewallInForce;
  state.deviceStillHeld = facts.deviceStillHeld;
  state.restartPending = facts.restartPending;
  return ProviderStartRefusal(facts.mode, state) == ProviderRefusal::None;
}

// ---- the app's half: what to ask the service while there is no session -----

// What one service status says, as the app's reconcile reads it
// (proto::ProviderFactsFrom fills it from a TunnelStatus).
struct ServiceProviderFacts {
  // A status was answered at all. Without one nothing changes: a failed read is
  // not evidence that nothing runs.
  bool answered = false;
  // A tunnel or rpc-only session is starting, live or stopping.
  bool tunnelSession = false;
  // Some firewall policy is in force (wfp_state not "off").
  bool killSwitchArmed = false;
  bool providerRunning = false;
};

enum class DisconnectedStep {
  None,
  // start_provider. Idempotent by design: the service keeps the device it runs
  // for an identical request and only applies the mode, so this one verb starts
  // a provider, follows a mode change, picks up a new provider transport policy
  // or network space (a different request builds a new device), and adopts a
  // provider an earlier run of this app left running.
  Start,
  // stop_provider: retire the provider-only device, and nothing else.
  Stop,
};

// The step the app takes while it holds no session. `controlMode` is the
// stored provide control mode ("never" for a signed-out app).
constexpr DisconnectedStep DisconnectedProviderStep(std::string_view controlMode,
                                                    const ServiceProviderFacts& facts) {
  if (!facts.answered || facts.tunnelSession) return DisconnectedStep::None;
  if (!ProviderRuns(ControlModeFrom(controlMode), /*connected=*/false)) {
    return facts.providerRunning ? DisconnectedStep::Stop : DisconnectedStep::None;
  }
  // The armed floor stays the only thing in force until the user reconnects or
  // lifts it (the service refuses a start there as well). A provider found
  // running under it is stopped rather than left to leave through the
  // service's own permit.
  if (facts.killSwitchArmed) {
    return facts.providerRunning ? DisconnectedStep::Stop : DisconnectedStep::None;
  }
  return DisconnectedStep::Start;
}

}  // namespace urnw::provide
