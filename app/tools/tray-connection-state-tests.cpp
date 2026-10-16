// Executable spec for what the tray says about the connection (App/
// TrayConnectionState.h): Connected, in words and in the icon, only with the
// tunnel up and the health connected; a connect in progress reads as the
// connect page's connecting line and a failed start as its "Couldn't connect";
// the tunnel-down lines keep the machine's own state first; and the menu offers
// Connect or Disconnect only with a status to act on. Run against the header the
// app compiles, on any host with a C++20 compiler.
//
//   c++ -std=c++20 -I ../src/App -I ../src/Common tray-connection-state-tests.cpp -o /tmp/tray-tests && /tmp/tray-tests
//
// tests/tray_connection_state_test.go runs it, with negative controls that
// rewrite a copy of the header, and pins AppController's and TrayIcon's use of
// it.
//
// SPDX-License-Identifier: MPL-2.0

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "TrayConnectionState.h"

namespace tray = urnw::tray;
using Health = urnw::health::State;
using tray::Action;
using tray::Status;
using tray::Tunnel;

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

const char* Name(Status status) {
  switch (status) {
    case Status::None: return "none";
    case Status::Connected: return "connected";
    case Status::FindingProviders: return "finding providers";
    case Status::Degraded: return "degraded";
    case Status::Connecting: return "connecting";
    case Status::Failed: return "failed";
    case Status::Disconnecting: return "disconnecting";
    case Status::BlockedKillSwitch: return "blocked by the kill switch";
    case Status::BlockedNoTunnel: return "blocked with no tunnel";
    case Status::FailsafeStopped: return "failsafe stopped";
  }
  return "?";
}

const char* Name(Tunnel tunnel) {
  switch (tunnel) {
    case Tunnel::Down: return "down";
    case Tunnel::Starting: return "starting";
    case Tunnel::Up: return "up";
    case Tunnel::Stopping: return "stopping";
    case Tunnel::Failed: return "failed";
  }
  return "?";
}

std::string Name(std::optional<Health> health) {
  return health ? urnw::health::ToString(*health) : "no health";
}

// Signed in, with a status: the tunnel and the health as given, nothing else
// installed.
tray::Facts Known(Tunnel tunnel, std::optional<Health> health = std::nullopt) {
  tray::Facts facts;
  facts.signedIn = true;
  facts.statusKnown = true;
  facts.tunnel = tunnel;
  facts.health = health;
  return facts;
}

const std::vector<std::optional<Health>> kEveryHealth = {
    std::nullopt,       Health::NoService, Health::Disconnected, Health::Connecting,
    Health::Evaluating, Health::Connected, Health::Degraded,     Health::Failed,
};

const std::vector<std::string_view> kEveryWfpState = {"off", "", "armed", "connecting",
                                                      "connected"};

// Connected, and the connected icon, only with the tunnel up and the health
// connected, or the tunnel's own claim with no stats since it came up.
void TestConnectedNeedsTheTunnelUp() {
  for (Tunnel tunnel : {Tunnel::Down, Tunnel::Starting, Tunnel::Up, Tunnel::Stopping,
                        Tunnel::Failed}) {
    for (const auto& health : kEveryHealth) {
      for (std::string_view wfp : kEveryWfpState) {
        for (bool routes : {false, true}) {
          tray::Facts facts = Known(tunnel, health);
          facts.wfpState = wfp;
          facts.routesInstalled = routes;
          const tray::View view = tray::Decide(facts);
          const bool connected = tunnel == Tunnel::Up &&
                                 (!health || *health == Health::Connected);
          const std::string what = std::string("connected only with the tunnel up: tunnel ") +
                                   Name(tunnel) + ", " + Name(health) + ", wfp \"" +
                                   std::string(wfp) + "\"" + (routes ? ", routes" : "");
          Check((view.status == Status::Connected) == connected, what + " reads " +
                                                                    Name(view.status));
          Check(view.connectedIcon == connected, what + (view.connectedIcon
                                                             ? " shows the connected icon"
                                                             : " hides the connected icon"));
        }
      }
    }
  }
}

// With the tunnel up the health says how far the connection got, in the
// connect page's words.
void TestTheTunnelUpReadsItsHealth() {
  const struct {
    std::optional<Health> health;
    Status status;
  } cases[] = {
      {std::nullopt, Status::Connected},           {Health::Connected, Status::Connected},
      {Health::Evaluating, Status::FindingProviders}, {Health::Degraded, Status::Degraded},
      {Health::Connecting, Status::Connecting},    {Health::Failed, Status::Failed},
      {Health::Disconnected, Status::None},        {Health::NoService, Status::None},
  };
  for (const auto& c : cases) {
    const Status status = tray::Decide(Known(Tunnel::Up, c.health)).status;
    Check(status == c.status, std::string("the tunnel up reads its health: ") +
                                  Name(c.health) + " reads " + Name(status) + ", not " +
                                  Name(c.status));
  }
}

// A connect in progress reads as the connect page's connecting line, whatever
// the firewall and the routes say while it starts. It read as the app name
// alone (the kill switch off) or as a blocked or disconnecting line.
void TestAConnectInProgressReadsConnecting() {
  for (const auto& health : kEveryHealth) {
    for (std::string_view wfp : kEveryWfpState) {
      for (bool routes : {false, true}) {
        for (bool failsafe : {false, true}) {
          tray::Facts facts = Known(Tunnel::Starting, health);
          facts.wfpState = wfp;
          facts.routesInstalled = routes;
          facts.failsafeStop = failsafe;
          const Status status = tray::Decide(facts).status;
          // the stats clamp keeps a finding or failed window while preparing
          const Status want = health == Health::Evaluating ? Status::FindingProviders
                              : health == Health::Failed   ? Status::Failed
                                                           : Status::Connecting;
          Check(status == want, std::string("a connect in progress reads connecting: ") +
                                    Name(health) + ", wfp \"" + std::string(wfp) + "\"" +
                                    (routes ? ", routes" : "") +
                                    (failsafe ? ", after a failsafe stop" : "") + " reads " +
                                    Name(status) + ", not " + Name(want));
        }
      }
    }
  }
}

// A start the service could not finish says "Couldn't connect", as the connect
// page does, unless the machine's own state is the more urgent line.
void TestAFailedStartSaysCouldNotConnect() {
  Check(tray::Decide(Known(Tunnel::Failed)).status == Status::Failed,
        "a failed start says couldn't connect");
  Check(tray::Decide(Known(Tunnel::Failed, Health::Failed)).status == Status::Failed,
        "a failed start says couldn't connect with the failed health");
  tray::Facts armed = Known(Tunnel::Failed);
  armed.wfpState = "armed";
  Check(tray::Decide(armed).status == Status::BlockedKillSwitch,
        "a failed start under the armed kill switch says the kill switch holds the machine");
  tray::Facts routes = Known(Tunnel::Failed);
  routes.routesInstalled = true;
  Check(tray::Decide(routes).status == Status::Disconnecting,
        "a failed start with routes installed says the traffic still goes to the tunnel");
  Check(!tray::Decide(Known(Tunnel::Failed, Health::Connected)).connectedIcon,
        "a failed start never shows the connected icon");
}

// With the tunnel down, the machine's own state first (#41), in its order:
// routes, the armed kill switch, another policy, the failsafe stop.
void TestTheTunnelDownLinesKeepTheirOrder() {
  for (Tunnel tunnel : {Tunnel::Down, Tunnel::Stopping}) {
    const std::string where = std::string("the tunnel ") + Name(tunnel) + ": ";
    Check(tray::Decide(Known(tunnel)).status == Status::None,
          where + "nothing installed reads as the app name alone");
    Check(tray::Decide(Known(tunnel, Health::Connected)).status == Status::None,
          where + "a stale connected health is not a connection");

    tray::Facts routes = Known(tunnel);
    routes.routesInstalled = true;
    routes.wfpState = "armed";
    routes.failsafeStop = true;
    Check(tray::Decide(routes).status == Status::Disconnecting,
          where + "routes still installed come first");

    tray::Facts armed = Known(tunnel);
    armed.wfpState = "armed";
    armed.failsafeStop = true;
    Check(tray::Decide(armed).status == Status::BlockedKillSwitch,
          where + "the armed kill switch comes before the failsafe stop");

    for (std::string_view wfp : {"connecting", "connected"}) {
      tray::Facts policy = Known(tunnel);
      policy.wfpState = wfp;
      policy.failsafeStop = true;
      Check(tray::Decide(policy).status == Status::BlockedNoTunnel,
            where + "a \"" + std::string(wfp) + "\" policy with no tunnel reads as blocked");
    }

    for (std::string_view wfp : {"off", ""}) {
      tray::Facts failsafe = Known(tunnel);
      failsafe.wfpState = wfp;
      failsafe.failsafeStop = true;
      Check(tray::Decide(failsafe).status == Status::FailsafeStopped,
            where + "the failsafe stop with the policy \"" + std::string(wfp) +
                "\" says the tunnel failed");
    }
  }
}

// Nothing known yet: the app name alone, and no connected icon.
void TestNoStatusClaimsNothing() {
  for (Tunnel tunnel : {Tunnel::Down, Tunnel::Starting, Tunnel::Up, Tunnel::Stopping,
                        Tunnel::Failed}) {
    for (const auto& health : kEveryHealth) {
      tray::Facts facts = Known(tunnel, health);
      facts.statusKnown = false;
      facts.routesInstalled = true;
      facts.wfpState = "armed";
      const tray::View view = tray::Decide(facts);
      Check(view.status == Status::None && !view.connectedIcon,
            std::string("no status claims nothing: tunnel ") + Name(tunnel) + ", " +
                Name(health) + " reads " + Name(view.status));
    }
  }
}

// Connect or Disconnect only with a status to act on: signed in, and once the
// service has said what its tunnel is doing. Then the shared rule decides which.
void TestNoActionWithoutAStatus() {
  for (bool signedIn : {false, true}) {
    for (bool statusKnown : {false, true}) {
      for (bool disconnect : {false, true}) {
        tray::Facts facts = Known(Tunnel::Up, Health::Connected);
        facts.signedIn = signedIn;
        facts.statusKnown = statusKnown;
        facts.actionIsDisconnect = disconnect;
        const Action want = !signedIn || !statusKnown ? Action::None
                            : disconnect               ? Action::Disconnect
                                                       : Action::Connect;
        const Action action = tray::ActionFor(facts);
        const char* names[] = {"none", "connect", "disconnect"};
        Check(action == want, std::string("no action without a status: ") +
                                  (signedIn ? "signed in" : "signed out") + ", " +
                                  (statusKnown ? "a status" : "no status") + ", the rule says " +
                                  (disconnect ? "disconnect" : "connect") + ": offers " +
                                  names[static_cast<int>(action)] + ", not " +
                                  names[static_cast<int>(want)]);
      }
    }
  }
}

// proto::TunnelState's enumerators, by name (tests/tray_connection_state_test.go
// holds this list to Protocol.h's).
enum class StandInTunnelState { Stopped, Starting, Up, Stopping, Error, RpcOnly, Preparing };

void TestTheServiceStatesFold() {
  const struct {
    StandInTunnelState state;
    Tunnel tunnel;
    const char* name;
  } cases[] = {
      {StandInTunnelState::Stopped, Tunnel::Down, "stopped"},
      {StandInTunnelState::Starting, Tunnel::Starting, "starting"},
      {StandInTunnelState::Up, Tunnel::Up, "up"},
      {StandInTunnelState::Stopping, Tunnel::Stopping, "stopping"},
      {StandInTunnelState::Error, Tunnel::Failed, "error"},
      {StandInTunnelState::RpcOnly, Tunnel::Down, "rpc only"},
      {StandInTunnelState::Preparing, Tunnel::Starting, "preparing"},
  };
  for (const auto& c : cases) {
    const Tunnel tunnel = tray::TunnelOf(c.state);
    Check(tunnel == c.tunnel, std::string("the service state ") + c.name + " folds to " +
                                  Name(tunnel) + ", not " + Name(c.tunnel));
  }
}

}  // namespace

int main() {
  const struct {
    const char* name;
    void (*run)();
  } tests[] = {
      {"connected needs the tunnel up", TestConnectedNeedsTheTunnelUp},
      {"the tunnel up reads its health", TestTheTunnelUpReadsItsHealth},
      {"a connect in progress reads connecting", TestAConnectInProgressReadsConnecting},
      {"a failed start says couldn't connect", TestAFailedStartSaysCouldNotConnect},
      {"the tunnel-down lines keep their order", TestTheTunnelDownLinesKeepTheirOrder},
      {"no status claims nothing", TestNoStatusClaimsNothing},
      {"no action without a status", TestNoActionWithoutAStatus},
      {"the service states fold", TestTheServiceStatesFold},
  };
  for (const auto& test : tests) {
    const int before = gFailures;
    test.run();
    std::cout << (gFailures == before ? "ok   " : "FAIL ") << test.name << "\n";
  }
  std::cout << gCases << " checks, " << gFailures << " failed\n";
  return gFailures == 0 ? 0 : 1;
}
