// Typed wrapper over the control PipeClient: the app's view of the service.
// Drives the tunnel lifecycle and surfaces state-change events.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>

#include "PipeClient.h"
#include "Protocol.h"

namespace urnw {

class ServiceClient {
 public:
  using StateHandler = std::function<void(const proto::TunnelStatus&)>;
  // The control channel dropped without us asking. See PipeClient.h: the
  // service owns the tunnel, so this is the app's only notice that there is no
  // longer one. Runs on the pipe reader thread and must not call Connect().
  using DisconnectHandler = std::function<void()>;

  bool Connect();
  bool IsConnected() const { return pipe_.IsConnected(); }
  void SetStateHandler(StateHandler h) { onState_ = std::move(h); }
  void SetDisconnectHandler(DisconnectHandler h) { onDisconnect_ = std::move(h); }

  proto::TunnelStatus Hello();
  proto::TunnelStatus StartTunnel(const proto::StartTunnel& config);
  proto::TunnelStatus StopTunnel();
  // `answered` is DID THE SERVICE ACTUALLY REPLY, and it has to be an explicit
  // out-param rather than something inferred from the returned struct. A failed
  // call yields a default-constructed TunnelStatus — "nothing running, no
  // routes" — which is indistinguishable by inspection from a service that
  // genuinely has nothing running, and the app's whole connect decision now
  // turns on telling those apart (Common/ConnectAction.h, ServiceFacts::known).
  //
  // The first version of that decision used `!service_version.empty()` as the
  // marker. It is ALWAYS empty: service_version is urnet::version(), and this
  // SDK build reports no version at all — the service's own startup line logs
  // `sdk=` with nothing after it. So every gesture read "the service did not
  // answer", took the unknown-fallback, and Connect never sent start_tunnel
  // over a stale session, which is the exact bug that fallback was written to
  // avoid being. Nothing derived from a payload field can be this marker; only
  // the transport knows.
  proto::TunnelStatus GetState(bool* answered = nullptr);
  bool SetSplitTunnel(const std::vector<std::string>& excludedPaths, bool allowlist = false);
  // Tell the service the kill switch changed. The app still owns the setting
  // and its persistence; the service owns the WFP policy the setting now drives.
  bool SetKillSwitch(bool on);
  bool Logout();

 private:
  proto::TunnelStatus CallStatus(const nlohmann::json& request,
                                 bool* answered = nullptr);

  PipeClient pipe_;
  StateHandler onState_;
  DisconnectHandler onDisconnect_;
};

}  // namespace urnw
