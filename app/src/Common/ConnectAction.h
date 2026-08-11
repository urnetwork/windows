// WHAT A CONNECT/DISCONNECT GESTURE ACTUALLY HAS TO DO — the one place that
// decides, for every button, tray item and row click in this app.
//
// WHY THIS EXISTS. There are TWO things behind the one word "connected": the
// SDK's connect session (a DeviceRemote and its connect controller, owned by
// the app) and the SERVICE's tunnel (a wintun adapter, 31 capture routes, the
// tun's DNS and a WFP policy, owned by urnetworkd). Every control used to drive
// exactly one of them, and the app decided which by looking at its own
// `device_` pointer. Both halves of that were wrong, and the owner hit both:
//
//   A. Disconnect drove only the SDK. The routes, the DNS and the firewall
//      policy stayed exactly as they were, so after pressing Disconnect the
//      machine had no internet at all — every path captured by a tunnel with
//      nothing carrying it. The tray tooltip then called that "Blocked (kill
//      switch on)" with the kill switch off, which is how it got reported as a
//      kill switch. There was no kill switch involved.
//
//   B. Connect drove only the SDK too, whenever `device_` happened to be
//      non-null. The tray's "turn the tunnel off" escape hatch stops the
//      SERVICE — which destroys the service's DeviceLocal and its mTLS
//      listener — and left `device_` alive, so the next Connect re-issued
//      connectBestAvailable() against a handle to a listener that no longer
//      existed and NEVER SENT start_tunnel. The only control that worked broke
//      the other one.
//
// THE RULE THAT REPLACES `if (device_)`: ask the SERVICE what is actually
// installed on this machine, and read the answer as the PAIR
// (IsSessionLive(state), routes_installed). `routes_installed` is the field
// Protocol.h nominates as the answer to "is my traffic going through the
// tunnel" and it is read off the object that owns the routes, in the process
// that holds them. IsSessionLive ALONE would be a fresh version of the same
// bug: it is true for an rpc-only session, which is defined by having no
// routes.
//
// It cannot race in a way that hurts. The app is the sole issuer of
// start_tunnel/stop_tunnel and serialises them through ONE worker with
// last-request-wins, and `start_tunnel` is itself the reconciler — StartLocked
// opens with an idempotent StopLocked that never drops the firewall policy to
// Off in the gap. So the worst case of a stale read is ONE EXTRA TUNNEL
// RESTART, never a blocked machine. That asymmetry is what makes a single
// get_state safe here.
//
// The derivation is PURE and lives in Common for the ConnectionHealth.h reason:
// the service selftest links this header and pins the whole table, so "which
// gesture does what to which half" is a tested claim rather than a hope. Same
// constraints as ConnectionHealth.h — constexpr, no Windows headers, no
// allocation, no clock, string_view only.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string_view>

#include "ConnectionHealth.h"
#include "Protocol.h"

namespace urnw::gesture {

// Every control that can change the connection, named by what the USER did.
// Deliberately not named by what it does — that is the output of Decide, and
// the whole bug was controls whose names promised one half of the work.
enum class Gesture {
  // The connect button, the tray's connect item resolved to connect, and an
  // explicit "connect to this location".
  Connect,
  // A location row click. Same plan as Connect; kept separate so the table
  // pins the coalesced path too (it reaches the same worker after the 1.2 s
  // settle, and it is the path that silently did nothing after a tray stop).
  ConnectRow,
  // The connect button's other label, and the tray's connect item resolved to
  // disconnect. THIS is bug A.
  Disconnect,
  // The tray's single connect/disconnect item, which has to decide which of
  // the two it is. Resolved here rather than in the tray so the tray and the
  // window cannot read different truths (they used to: the tray tested
  // `state == Up`, the window tested the SDK's connect status).
  TrayToggle,
  // "Have a session; connect to nothing." The resume path, a network-server
  // change, and the service-reconnect watchdog.
  EnsureSession,
  // The tray escape hatch. Deliberately still exists after the fix: it is the
  // only control that (i) exists when the window does not and (ii) bypasses
  // the session worker, which matters exactly when the worker is the thing
  // that is wedged.
  ForceStopTunnel,
  // The tray's "turn off the kill switch" item.
  LiftKillSwitch,
};

// WHAT THE SERVICE SAYS, filled straight from one proto::TunnelStatus. The
// string_views point INTO that status, so it must outlive the facts.
struct ServiceFacts {
  // A control channel to the service exists at all.
  bool pipeUp = false;
  // ...AND a get_state actually answered on it. Distinct from pipeUp on
  // purpose: a call that failed produces a default-constructed TunnelStatus,
  // and reading "no routes, nothing running" out of a FAILED READ would tear a
  // healthy session down over a dropped reply. When this is false the decision
  // falls back to "keep what we have", which is the direction that cannot
  // destroy anything.
  bool known = false;
  proto::TunnelState state = proto::TunnelState::Stopped;
  proto::StartMode mode = proto::StartMode::Tunnel;
  // THE FIELD. True only while routes and DNS are installed right now, read off
  // the object that owns them.
  bool routesInstalled = false;
  // "off" | "armed" | "connecting" | "connected". Empty is read as "off" — an
  // absent field must not be able to claim a policy is in force.
  std::string_view wfpState = "off";
  // "" | "user" | "failsafe_*" — why the last teardown happened.
  std::string_view stopReason = "";
};

// WHAT THIS APP KNOWS ABOUT ITSELF.
struct AppFacts {
  // A DeviceRemote is constructed on this side. Note what this is NOT allowed
  // to mean any more: "there is a session". A DeviceRemote whose service-side
  // listener was destroyed still exists and still answers its cached getters.
  bool haveDevice = false;
  // The user's persisted kill-switch preference. Decide DELIBERATELY NEVER
  // READS THIS — whether a teardown leaves the firewall armed or lifts it is
  // the SERVICE's decision (RevertMachineStateLocked's finalDisarm branch), and
  // it has always been right about it: a deliberate stop lifts the policy, kill
  // switch or not, because a kill switch blocks on an UNEXPECTED drop and
  // lockdown blocks whenever not connected. The field is here so the table can
  // pin that the plan carries no arming intent either way.
  bool killSwitch = false;
  // This app asked the service for a real tunnel (i.e. URNETWORK_RPC_ONLY is
  // not set). False means the rpc-only developer mode, whose promise is that it
  // never touches this machine's network — no gesture may start a tunnel there.
  bool wantsTunnel = true;
  // The aggregate the user is being shown right now (ConnectionHealth.h). Read
  // only by TrayToggle, to decide which gesture the one tray item is.
  health::State health = health::State::NoService;
};

// WHAT THE WORKER MUST DO. No side effects here: Decide returns the plan and
// the caller performs it, in the order the caller documents — which is the
// ordering rule that made the service's teardown safe and that the app then
// had to learn too (machine state back FIRST, SDK teardown second).
struct Plan {
  // Send stop_tunnel. The service reverts routes, DNS and the resolver cache
  // and lifts the firewall policy, through its ONE two-phase StopLocked. There
  // is deliberately no second unwind anywhere in this product.
  bool stopTunnel = false;
  // Drop this side's DeviceRemote. stop_tunnel destroys the service's
  // DeviceLocal and its RPC listener, so a DeviceRemote kept across one is a
  // handle to something that is gone — which is the state bug B lives in.
  bool tearDownDevice = false;
  // Run the bootstrap: reattach if the service still holds a live session of
  // the mode we want (the saved-instance-id path), otherwise send start_tunnel.
  bool startTunnel = false;
  // Drive the SDK's connect controller at the request's target.
  bool sdkConnect = false;
  // Drive the SDK's connect controller to disconnect.
  bool sdkDisconnect = false;
  // Send set_kill_switch(false).
  bool liftKillSwitch = false;
  // One sentence, for the log and for the UI. NEVER empty, including for a
  // plan that does nothing: a gesture that lands in silence is the failure this
  // whole header exists to remove.
  const char* why = "";
};

// "Some firewall policy is in force." Empty reads as off — see wfpState.
inline constexpr bool WfpInForce(const ServiceFacts& f) {
  return !f.wfpState.empty() && f.wfpState != "off";
}

// "This machine's network is captured right now" — routes pointed into a tun,
// or a policy blocking the paths that are not it. Either one is something a
// disconnect has work to do about, whatever the SDK thinks it is doing.
inline constexpr bool MachineIsCaptured(const ServiceFacts& f) {
  return f.routesInstalled || WfpInForce(f);
}

// "The service can actually lift the policy if asked." EXACTLY the branch
// TunnelController::SetKillSwitch's lift arm can act on: not Up, and a policy
// in one of the two idle-ish states. It is NOT `wfp_state != "off"` — that
// version offered "unblock this machine" over a Connected policy, where the
// lift arm does nothing and returns true, i.e. a control that exists and
// provably cannot do what its label says.
inline constexpr bool KillSwitchIsLiftable(const ServiceFacts& f) {
  return f.pipeUp && f.state != proto::TunnelState::Up &&
         (f.wfpState == "armed" || f.wfpState == "connecting");
}

// "This machine is blocked because the KILL SWITCH is doing its job." Routes
// are gone, and the only thing still in force is the armed policy — the state
// StopLocked leaves behind on a drop nobody asked for. It is the one capture
// that is not a bug and not a leftover, and it has its own named controls (the
// tray's lift item, the Settings toggle), so the connect button does not have
// to be one of them.
inline constexpr bool BlockedByKillSwitch(const ServiceFacts& f) {
  return !f.routesInstalled && f.wfpState == "armed";
}

// THE BUTTON-LABEL RULE, shared by the connect page and the tray. The action is
// Disconnect whenever there is anything for a disconnect to DO: the SDK is
// driving something, or this machine is captured by something a disconnect
// would clear. That single rule kills the cell the owner was stuck in — the
// button reading "Connect" while the machine had no internet because a tunnel
// was still installed, so the only control on screen was the wrong one.
//
// The armed kill switch is deliberately excluded. There, the machine is blocked
// ON PURPOSE, reconnecting is what the user almost always wants, and the copy
// that explains the block already names the two controls that lift it. Offering
// "Disconnect" to someone who is already disconnected would be the same class
// of nonsense in the other direction.
inline constexpr bool ActionIsDisconnect(const ServiceFacts& f, health::State h) {
  const bool sdkActive =
      h != health::State::Disconnected && h != health::State::NoService;
  return sdkActive || (MachineIsCaptured(f) && !BlockedByKillSwitch(f));
}

// "The service is currently serving an rpc-only session." Reported by the
// service, not inferred: an app that asked for a tunnel and got this was
// CLAMPED (an unelevated service), and pressing Connect again cannot change
// that. Restarting the session on every press would churn a working rpc-only
// session for nothing; the standing mode notice is the surface that explains
// it.
inline constexpr bool ServingRpcOnly(const ServiceFacts& f) {
  return proto::IsSessionLive(f.state) && f.mode == proto::StartMode::RpcOnly;
}

// The one decision. Pure; the caller performs the plan.
inline constexpr Plan Decide(Gesture g, const ServiceFacts& f, const AppFacts& a) {
  Plan p;

  // The tray's one item is whichever of the two it is offering to be, decided
  // by the SAME predicate the label was drawn with.
  if (g == Gesture::TrayToggle)
    g = ActionIsDisconnect(f, a.health) ? Gesture::Disconnect : Gesture::Connect;

  switch (g) {
    case Gesture::LiftKillSwitch:
      if (!f.pipeUp) {
        p.why =
            "there is no service to ask — a firewall policy is held on a "
            "dynamic session and dies with the process that owns it";
        return p;
      }
      if (!KillSwitchIsLiftable(f)) {
        p.why =
            "the policy in force is not one the kill switch can lift; a "
            "disconnect is what takes a connected policy down";
        return p;
      }
      p.liftKillSwitch = true;
      p.why = "the kill switch is holding this machine blocked with no tunnel up";
      return p;

    case Gesture::ForceStopTunnel:
      if (!f.pipeUp) {
        p.why =
            "there is no control channel; nothing here can revert a tunnel this "
            "process does not own, and the service's death already took it";
        return p;
      }
      p.stopTunnel = true;
      // Without this the hatch fixes the machine and breaks the app: the next
      // Connect would drive a DeviceRemote whose listener this very stop
      // destroyed. That is the whole of bug B.
      //
      // SdkHost::StopServiceTunnel performs this plan in two pieces — the stop
      // inline (the hatch must never wait on the session worker's lock) and the
      // teardown through the Disconnect it queues, which reaches the row above
      // and sees a service with no session left. Same plan, and the split is
      // what keeps an escape hatch an escape hatch.
      p.tearDownDevice = a.haveDevice;
      p.why =
          "the escape hatch: take the whole tunnel down, and drop the session "
          "that pointed at it so the next Connect starts a real one";
      return p;

    case Gesture::Disconnect: {
      if (!f.pipeUp) {
        p.sdkDisconnect = a.haveDevice;
        p.why =
            "there is no service, so there is no tunnel to stop — the adapter "
            "and the firewall policy died with its process";
        return p;
      }
      if (!f.known) {
        // THE OPPOSITE FALLBACK TO CONNECT'S, and the asymmetry is the whole
        // reason both are written down. For a Connect, doing more on a failed
        // read is destructive (it tears down a session that may be fine). For a
        // Disconnect, doing more is the SAFE direction: stop_tunnel is
        // idempotent, its worst case is stopping nothing, and the cost of
        // guessing wrong is one bootstrap — against a user who pressed
        // Disconnect and may be looking at a machine with no internet.
        p.stopTunnel = true;
        p.tearDownDevice = a.haveDevice;
        p.sdkDisconnect = a.haveDevice;
        p.why =
            "the service could not be asked for its state, so the disconnect "
            "is honoured in full rather than half — stopping nothing is a "
            "cheaper mistake than leaving this machine captured";
        return p;
      }
      // Is there anything installed for the service to give back? Routes are
      // the headline, but a policy left in force is a blocked machine too, and
      // stop_tunnel is idempotent and lifts it (StopLocked(finalDisarm=true)).
      const bool serviceHasSomething =
          f.routesInstalled ||
          (proto::IsSessionLive(f.state) && f.mode == proto::StartMode::Tunnel) ||
          WfpInForce(f);
      p.stopTunnel = serviceHasSomething;
      p.sdkDisconnect = a.haveDevice;
      // Tear the session down TOO, and it is not a compromise — the middle
      // option ("stop the tunnel, keep the session") is precisely the state bug
      // B lives in. The cost is ~1 s of bootstrap on the next Connect, which
      // the app already pays on every cold start and every service restart.
      //
      // The second half of the test is what makes the tray escape hatch's
      // follow-up Disconnect do its job: after a force-stop there is nothing
      // left to stop, but this side is still holding a DeviceRemote whose
      // listener that very stop destroyed. A live rpc-only session is NOT
      // stale — it has no routes by design — so it survives a disconnect.
      p.tearDownDevice =
          a.haveDevice && (serviceHasSomething || !proto::IsSessionLive(f.state));
      p.why = serviceHasSomething
                  ? "a deliberate disconnect: this machine's routes, dns and "
                    "firewall policy come back FIRST, then the session goes"
              : p.tearDownDevice
                  ? "the service has no session left, so the DeviceRemote we "
                    "hold is a handle to a listener that is gone: drop it"
                  : "nothing of ours is installed on this machine; only the "
                    "connect controller has anything to stop";
      return p;
    }

    case Gesture::Connect:
    case Gesture::ConnectRow:
    case Gesture::EnsureSession: {
      p.sdkConnect = g != Gesture::EnsureSession;
      if (!f.pipeUp || !f.known) {
        // NOT a no-op. The bootstrap is the thing that DIALS the pipe, reports
        // why it could not, and arms the service-reconnect watchdog; making
        // this silent would take the app's only recovery from "the service is
        // not running yet" with it. And with a session already in hand we keep
        // it: a failed read is not evidence of anything.
        p.startTunnel = !a.haveDevice;
        p.why = a.haveDevice
                    ? "the service could not be asked for its state, so keep "
                      "the session we have — a dropped reply is not evidence "
                      "that the tunnel is gone"
                    : "no session and no answer from the service: the bootstrap "
                      "is what dials the pipe, and what says why if it cannot";
        return p;
      }
      // Does this gesture need ROUTES to be satisfied, or only a live session?
      // rpc-only never has routes by design, in both directions: the app asked
      // for it, or the service clamped and said so.
      const bool needRoutes = a.wantsTunnel && !ServingRpcOnly(f);
      const bool serviceHasWhatWeNeed =
          proto::IsSessionLive(f.state) && (!needRoutes || f.routesInstalled);
      if (a.haveDevice && serviceHasWhatWeNeed) {
        p.why =
            "the session and its tunnel are both live; drive the connect "
            "controller and touch nothing else";
        return p;
      }
      // THE ROW BUG B GETS WRONG. `if (device_) { ok = true; }` answered this
      // question from a pointer, so a Connect after ANY stop re-issued
      // connectBestAvailable() into a dead listener and never sent start_tunnel.
      p.tearDownDevice = a.haveDevice;
      p.startTunnel = true;
      // Deliberately NOT stopTunnel: start_tunnel is itself the reconciler, and
      // its internal StopLocked(finalDisarm=false) holds the firewall Armed
      // across the gap. Sending a deliberate stop first would drop the policy
      // to Off for the length of a bring-up — the exact gap a kill switch is
      // for.
      p.why = a.haveDevice
                  ? "the service has no tunnel for this session, so the "
                    "DeviceRemote we hold points at a listener that is gone: "
                    "drop it and start a real one"
                  : "no session yet: bootstrap one (a reattach if the service "
                    "still holds a live one, otherwise start_tunnel)";
      return p;
    }

    case Gesture::TrayToggle:
      break;  // resolved above; unreachable
  }
  p.why = "unrecognised gesture";
  return p;
}

}  // namespace urnw::gesture
