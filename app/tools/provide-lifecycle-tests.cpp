// Executable spec for providing while disconnected (Common/ProvideLifecycle.h):
// the sdk's provide tier for every control mode connected and disconnected
// (Always provides publicly and Auto serves the user's own devices while
// disconnected, Never and unknown modes not at all), the service's refusals
// for the provider-only device (a tunnel session, the kill switch's armed
// floor, a device still held by an abandoned teardown, a restart in flight, a
// mode that does not provide), the step the app takes for every service state
// while it holds no session, and where the Extender switch's write goes while
// disconnected. Run against the same header the service and the app compile,
// on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/Common provide-lifecycle-tests.cpp -o /tmp/provide-lifecycle-tests && /tmp/provide-lifecycle-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <iostream>
#include <iterator>
#include <string>

#include "ProvideLifecycle.h"

using namespace urnw::provide;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

const char* Name(DisconnectedStep step) {
  switch (step) {
    case DisconnectedStep::None: return "none";
    case DisconnectedStep::Start: return "start";
    case DisconnectedStep::Stop: return "stop";
  }
  return "?";
}

const char* Name(ProviderRefusal refusal) {
  switch (refusal) {
    case ProviderRefusal::None: return "none";
    case ProviderRefusal::ModeDoesNotProvide: return "mode";
    case ProviderRefusal::TunnelSession: return "tunnel session";
    case ProviderRefusal::KillSwitchArmed: return "kill switch armed";
    case ProviderRefusal::DeviceStillHeld: return "device still held";
    case ProviderRefusal::RestartPending: return "restart pending";
  }
  return "?";
}

void CheckStep(const char* mode, const ServiceProviderFacts& facts, DisconnectedStep want,
               const std::string& what) {
  const DisconnectedStep got = DisconnectedProviderStep(mode, facts);
  Check(got == want, what + " (mode " + mode + "): got " + Name(got) + ", want " + Name(want));
}

void CheckRefusal(ControlMode mode, const ServiceProviderState& state, ProviderRefusal want,
                  const std::string& what) {
  const ProviderRefusal got = ProviderStartRefusal(mode, state);
  Check(got == want, what + ": got " + Name(got) + ", want " + Name(want));
}

// (a) the strings the app stores and the service receives
void TestControlModeStrings() {
  Check(ControlModeFrom("never") == ControlMode::Never, "modes: never parses");
  Check(ControlModeFrom("always") == ControlMode::Always, "modes: always parses");
  Check(ControlModeFrom("network") == ControlMode::Network, "modes: network parses");
  Check(ControlModeFrom("auto") == ControlMode::Auto, "modes: auto parses");
  // the sdk's manual mode defers to a provide mode that lives in the device,
  // and anything misspelled must not provide
  for (const char* other : {"manual", "", "Always", "ALWAYS", " always", "public", "wifi"}) {
    Check(ControlModeFrom(other) == ControlMode::Unknown,
          std::string("modes: '") + other + "' is unknown");
  }
  for (ControlMode mode : {ControlMode::Never, ControlMode::Always, ControlMode::Network,
                           ControlMode::Auto}) {
    Check(ControlModeFrom(ToString(mode)) == mode,
          std::string("modes: ") + ToString(mode) + " round-trips");
  }
  Check(std::string(ToString(ControlMode::Unknown)) == "unknown",
        "modes: an unknown mode logs as unknown, never as the raw string");
}

// (b) the sdk's applyProvideControlModeWithLock, connected and disconnected
void TestSdkTiers() {
  struct Row {
    ControlMode mode;
    Tier connected;
    Tier disconnected;
  };
  const Row rows[] = {
      {ControlMode::Never, Tier::None, Tier::None},
      {ControlMode::Always, Tier::Public, Tier::Public},
      {ControlMode::Network, Tier::Network, Tier::Network},
      {ControlMode::Auto, Tier::Public, Tier::Network},
      {ControlMode::Unknown, Tier::None, Tier::None},
  };
  for (const Row& row : rows) {
    const std::string name = ToString(row.mode);
    Check(SdkTierFor(row.mode, true) == row.connected, "tiers: " + name + " connected");
    Check(SdkTierFor(row.mode, false) == row.disconnected, "tiers: " + name + " disconnected");
    Check(ProviderRuns(row.mode, true) == (row.connected != Tier::None),
          "tiers: " + name + " provides connected");
  }
  // the sdk's protocol values, so a tier and a status field compare directly
  Check(static_cast<int64_t>(Tier::None) == 0 && static_cast<int64_t>(Tier::Network) == 1 &&
            static_cast<int64_t>(Tier::Public) == 3,
        "tiers: the sdk's ProvideMode values");
}

// (c) the defect: on Windows every mode stopped providing on Disconnect
void TestProvidesWhileDisconnected() {
  Check(ProviderRuns(ControlMode::Always, false), "always provides while disconnected");
  Check(SdkTierFor(ControlMode::Always, false) == Tier::Public,
        "always provides PUBLICLY while disconnected (Android and Apple parity)");
  Check(ProviderRuns(ControlMode::Auto, false),
        "auto serves the user's own devices while disconnected");
  Check(ProviderRuns(ControlMode::Network, false),
        "network serves the user's own devices while disconnected");
  Check(!ProviderRuns(ControlMode::Never, false), "never does not provide while disconnected");
  Check(!ProviderRuns(ControlMode::Unknown, false),
        "an unknown mode does not provide while disconnected");
}

// (d) the service refuses before anything is built
void TestServiceRefusals() {
  const ServiceProviderState clear;
  CheckRefusal(ControlMode::Always, clear, ProviderRefusal::None, "refusal: always, nothing else");
  CheckRefusal(ControlMode::Auto, clear, ProviderRefusal::None, "refusal: auto, nothing else");
  CheckRefusal(ControlMode::Network, clear, ProviderRefusal::None,
               "refusal: network, nothing else");
  CheckRefusal(ControlMode::Never, clear, ProviderRefusal::ModeDoesNotProvide,
               "refusal: never does not provide while disconnected");
  CheckRefusal(ControlMode::Unknown, clear, ProviderRefusal::ModeDoesNotProvide,
               "refusal: an unknown mode does not provide while disconnected");

  ServiceProviderState session;
  session.tunnelSession = true;
  CheckRefusal(ControlMode::Always, session, ProviderRefusal::TunnelSession,
               "refusal: a tunnel session exists or is starting (two devices, one identity)");

  ServiceProviderState armed;
  armed.firewallInForce = true;
  CheckRefusal(ControlMode::Always, armed, ProviderRefusal::KillSwitchArmed,
               "refusal: the kill switch is armed after an unexpected drop");

  ServiceProviderState held;
  held.deviceStillHeld = true;
  CheckRefusal(ControlMode::Always, held, ProviderRefusal::DeviceStillHeld,
               "refusal: an abandoned teardown still holds this identity");

  ServiceProviderState restarting;
  restarting.restartPending = true;
  CheckRefusal(ControlMode::Always, restarting, ProviderRefusal::RestartPending,
               "refusal: the service is restarting itself");

  // the first reason in the documented order wins
  ServiceProviderState everything;
  everything.tunnelSession = true;
  everything.firewallInForce = true;
  everything.deviceStillHeld = true;
  everything.restartPending = true;
  CheckRefusal(ControlMode::Never, everything, ProviderRefusal::ModeDoesNotProvide,
               "refusal: the mode is judged first");
  CheckRefusal(ControlMode::Always, everything, ProviderRefusal::TunnelSession,
               "refusal: a tunnel session before the armed floor");
  everything.tunnelSession = false;
  CheckRefusal(ControlMode::Always, everything, ProviderRefusal::KillSwitchArmed,
               "refusal: the armed floor before a held device");

  for (ProviderRefusal refusal :
       {ProviderRefusal::ModeDoesNotProvide, ProviderRefusal::TunnelSession,
        ProviderRefusal::KillSwitchArmed, ProviderRefusal::DeviceStillHeld,
        ProviderRefusal::RestartPending}) {
    Check(std::string(RefusalReason(refusal)).size() > 10,
          std::string("refusal: '") + Name(refusal) + "' has a reason for the reply");
  }
  Check(std::string(RefusalReason(ProviderRefusal::None)).empty(),
        "refusal: none has no reason");
}

// (e) what the app sends while it holds no session, for every service state
void TestDisconnectedStep() {
  ServiceProviderFacts idle;
  idle.answered = true;

  // a mode that provides while disconnected starts the provider-only device,
  // and keeps asking with the same verb (idempotent: a mode change, a new
  // policy, an adoption after an app restart)
  for (const char* mode : {"always", "auto", "network"}) {
    CheckStep(mode, idle, DisconnectedStep::Start, "step: nothing runs");
    ServiceProviderFacts running = idle;
    running.providerRunning = true;
    CheckStep(mode, running, DisconnectedStep::Start, "step: the provider runs (re-applied)");
  }

  // a mode that does not provide stops a running provider and asks nothing else
  for (const char* mode : {"never", "manual", ""}) {
    CheckStep(mode, idle, DisconnectedStep::None, "step: nothing runs and nothing should");
    ServiceProviderFacts running = idle;
    running.providerRunning = true;
    CheckStep(mode, running, DisconnectedStep::Stop, "step: a running provider is stopped");
  }

  // a failed read is not evidence: nothing changes either way
  ServiceProviderFacts unanswered;
  unanswered.providerRunning = true;
  CheckStep("always", unanswered, DisconnectedStep::None, "step: the service did not answer");
  CheckStep("never", unanswered, DisconnectedStep::None, "step: the service did not answer");

  // a tunnel session (starting, live or stopping) is the provider: never a
  // second device beside it
  ServiceProviderFacts session = idle;
  session.tunnelSession = true;
  CheckStep("always", session, DisconnectedStep::None, "step: a tunnel session exists");
  session.providerRunning = true;
  CheckStep("never", session, DisconnectedStep::None,
            "step: a tunnel session exists, whatever the provider bit says");

  // the armed floor holds: nothing starts under it, and a provider found under
  // it is stopped rather than left to leave through the service's own permit
  ServiceProviderFacts armed = idle;
  armed.killSwitchArmed = true;
  CheckStep("always", armed, DisconnectedStep::None, "step: the kill switch is armed");
  armed.providerRunning = true;
  CheckStep("always", armed, DisconnectedStep::Stop,
            "step: a provider running under the armed floor");
}

// (f) where set_provide_extender writes the setting, for every state the
// service can be in when it arrives
void TestExtenderSettingTarget() {
  struct Row {
    bool providerDevice, sessionDevice, lastSpace;
    ExtenderSettingTarget want;
    const char* what;
  };
  const Row rows[] = {
      {true, false, true, ExtenderSettingTarget::ProviderDevice,
       "the provider-only device takes the write, applied at once"},
      {true, false, false, ExtenderSettingTarget::ProviderDevice,
       "the provider-only device takes it whatever was imported before"},
      {false, true, true, ExtenderSettingTarget::SessionDevice,
       "a session's device that came up meanwhile takes it"},
      {false, true, false, ExtenderSettingTarget::SessionDevice,
       "a session's device takes it whatever was imported before"},
      {false, false, true, ExtenderSettingTarget::NetworkSpace,
       "with no device it goes into the space the next start reads"},
      {false, false, false, ExtenderSettingTarget::None,
       "with no device and no space it is refused, never dropped as written"},
      // never both at once in the service; the provider-only device wins the tie
      {true, true, true, ExtenderSettingTarget::ProviderDevice,
       "the provider-only device first"},
  };
  for (const Row& row : rows) {
    const ExtenderSettingTarget got =
        ExtenderSettingTargetFor(row.providerDevice, row.sessionDevice, row.lastSpace);
    Check(got == row.want, std::string("extender setting: ") + row.what + ": got " +
                               ToString(got) + ", want " + ToString(row.want));
  }
  for (const ExtenderSettingTarget target :
       {ExtenderSettingTarget::ProviderDevice, ExtenderSettingTarget::SessionDevice,
        ExtenderSettingTarget::NetworkSpace, ExtenderSettingTarget::None}) {
    Check(std::string(ToString(target)).size() > 5,
          "extender setting: every target names itself for the log");
  }
}

// One device across connect and disconnect: the identity a device is built
// from decides whether a Connect keeps the provider-only device, and a
// Disconnect keeps the session's device exactly when a provider-only device
// could start in its place.
void TestOneDevice() {
  const DeviceIdentity base{"jwt", "{\"space\":1}", "instance", "windows-desktop", "spec", "1.2-3"};
  Check(SameIdentity(base, base), "one device: an identity is its own");
  const std::pair<const char*, DeviceIdentity> changed[] = {
      {"jwt", {"jwt2", base.networkSpaceJson, base.instanceId, base.deviceDescription,
               base.deviceSpec, base.appVersion}},
      {"space", {base.jwtSubject, "{\"space\":2}", base.instanceId, base.deviceDescription,
                 base.deviceSpec, base.appVersion}},
      {"instance", {base.jwtSubject, base.networkSpaceJson, "instance2", base.deviceDescription,
                    base.deviceSpec, base.appVersion}},
      {"description", {base.jwtSubject, base.networkSpaceJson, base.instanceId, "other",
                       base.deviceSpec, base.appVersion}},
      {"spec", {base.jwtSubject, base.networkSpaceJson, base.instanceId, base.deviceDescription,
                "spec2", base.appVersion}},
      {"app version", {base.jwtSubject, base.networkSpaceJson, base.instanceId,
                       base.deviceDescription, base.deviceSpec, "1.2-4"}},
  };
  for (const auto& [field, identity] : changed) {
    Check(!SameIdentity(base, identity),
          std::string("one device: a changed ") + field + " is another identity");
    Check(!PromotesProviderDevice(true, base, identity),
          std::string("one device: a connect with a changed ") + field +
              " builds its own device");
  }
  Check(PromotesProviderDevice(true, base, base),
        "one device: a connect keeps the provider-only device built for its identity");
  Check(!PromotesProviderDevice(false, base, base),
        "one device: with no provider-only device a connect builds one");

  // A rebuild names what differs, by request field name, in the order above.
  const char* const fieldNames[] = {"by_jwt",      "network_space_json", "instance_id",
                                    "device_description", "device_spec", "app_version"};
  static_assert(std::size(fieldNames) == std::size(changed));
  for (std::size_t i = 0; i < std::size(changed); ++i) {
    Check(DifferingIdentityFields(base, changed[i].second) == fieldNames[i],
          std::string("one device: the differing fields name ") + fieldNames[i]);
  }
  Check(DifferingIdentityFields(base, base).empty(),
        "one device: an identity differs from itself in nothing");
  // Another client on another instance: both fields are named.
  const DeviceIdentity moved{"jwt2", base.networkSpaceJson, "instance2",
                             base.deviceDescription, base.deviceSpec, base.appVersion};
  Check(DifferingIdentityFields(base, moved) == "by_jwt, instance_id",
        "one device: every differing field is named");

  // The disconnect rule, for every mode.
  SessionStopFacts disconnect;
  disconnect.sessionDevice = true;
  disconnect.disconnect = true;
  for (const ControlMode mode : {ControlMode::Always, ControlMode::Network, ControlMode::Auto}) {
    SessionStopFacts facts = disconnect;
    facts.mode = mode;
    Check(KeepsSessionDevice(facts), std::string("one device: a disconnect in mode ") +
                                         ToString(mode) + " keeps the device providing");
  }
  for (const ControlMode mode : {ControlMode::Never, ControlMode::Unknown}) {
    SessionStopFacts facts = disconnect;
    facts.mode = mode;
    Check(!KeepsSessionDevice(facts), std::string("one device: a disconnect in mode ") +
                                          ToString(mode) + " closes the device");
  }
  // Every reason a provider-only device would be refused closes the device.
  SessionStopFacts facts = disconnect;
  facts.mode = ControlMode::Auto;
  {
    SessionStopFacts f = facts;
    f.sessionDevice = false;
    Check(!KeepsSessionDevice(f), "one device: a session without a device keeps nothing");
  }
  {
    SessionStopFacts f = facts;
    f.disconnect = false;
    Check(!KeepsSessionDevice(f),
          "one device: a stop that is not the user's disconnect (failsafe, failed or "
          "replaced start, logout, service stop) closes the device");
  }
  {
    SessionStopFacts f = facts;
    f.firewallInForce = true;
    Check(!KeepsSessionDevice(f),
          "one device: the kill switch's armed floor closes the device, as it refuses a "
          "provider");
  }
  {
    SessionStopFacts f = facts;
    f.deviceStillHeld = true;
    Check(!KeepsSessionDevice(f),
          "one device: another device still held under this identity closes this one");
  }
  {
    SessionStopFacts f = facts;
    f.restartPending = true;
    Check(!KeepsSessionDevice(f), "one device: a pending self-restart closes the device");
  }
  // Kept exactly when a provider-only device could start in its place.
  for (const ControlMode mode : {ControlMode::Never, ControlMode::Always, ControlMode::Network,
                                 ControlMode::Auto, ControlMode::Unknown}) {
    for (int bits = 0; bits < 8; ++bits) {
      SessionStopFacts f = disconnect;
      f.mode = mode;
      f.firewallInForce = (bits & 1) != 0;
      f.deviceStillHeld = (bits & 2) != 0;
      f.restartPending = (bits & 4) != 0;
      ServiceProviderState state;
      state.firewallInForce = f.firewallInForce;
      state.deviceStillHeld = f.deviceStillHeld;
      state.restartPending = f.restartPending;
      Check(KeepsSessionDevice(f) == (ProviderStartRefusal(mode, state) == ProviderRefusal::None),
            std::string("one device: the disconnect agrees with the provider start rule (mode ") +
                ToString(mode) + ", case " + std::to_string(bits) + ")");
    }
  }
}

}  // namespace

int main() {
  TestControlModeStrings();
  TestSdkTiers();
  TestProvidesWhileDisconnected();
  TestServiceRefusals();
  TestDisconnectedStep();
  TestExtenderSettingTarget();
  TestOneDevice();
  std::cout << (gCases - gFailures) << "/" << gCases << " provide lifecycle checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
