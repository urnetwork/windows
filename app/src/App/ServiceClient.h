// Typed wrapper over the control PipeClient: the app's view of the service.
// Drives the tunnel lifecycle and surfaces state-change events.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>

#include "PipeClient.h"
#include "ExtenderReset.h"
#include "IngressDnsSync.h"
#include "LogUpload.h"
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
  // `answered` as for GetState below: whether the service replied with a
  // status at all, which a failed call's default status cannot say.
  proto::TunnelStatus StopTunnel(bool* answered = nullptr);
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
  // logout (Protocol.h Logout): the service severs this machine's device
  // identity and clears what its sdk stored for the account in this network
  // space. True when the service answered that it did.
  bool Logout(const proto::Logout& request);
  // Keep providing while there is no tunnel session (Protocol.h start_provider).
  // True when the service runs the provider-only device, or kept the one it was
  // running for an identical request. `status` receives the status the reply
  // carries, a refusal's included, and stays empty when no status came back;
  // `error` receives the refusal's reason. Unlike the tunnel calls this never
  // folds a refusal into status.state: a refused provider says nothing about
  // the tunnel.
  bool StartProvider(const proto::StartProvider& request,
                     std::optional<proto::TunnelStatus>* status = nullptr,
                     std::string* error = nullptr);
  // stop_provider: the service retires the provider-only device and touches
  // nothing else. Same out-params as StartProvider.
  bool StopProvider(std::optional<proto::TunnelStatus>* status = nullptr,
                    std::string* error = nullptr);
  // get_provider_stats (Protocol.h ProviderStats). True when the service
  // answered with them; false for a transport failure or for a service too old
  // to know the verb ("unknown request type"), which the app reads as "no
  // statistics" and renders as it did before they existed.
  bool GetProviderStats(proto::ProviderStats& stats);
  // set_network_country (Protocol.h SetNetworkCountry). True when the service
  // took it; false for a transport failure or for a service too old to know
  // the verb, which then holds no network country, as before the verb existed.
  bool SetNetworkCountry(const proto::SetNetworkCountry& country);
  // upload_logs (Protocol.h): the service uploads its own logs for a feedback
  // the server accepted, whether or not a tunnel runs. Accepted once the
  // service admitted the upload, with `carrier` naming the device and
  // `uploadId` the id its status reports the outcome under; Busy when one is
  // in flight already; NotTaken for a transport failure, a refusal or a
  // service too old to know the verb ("unknown request type"), with `error`
  // saying which. The app then falls back to its DeviceRemote.
  logupload::ServiceAnswer UploadLogs(const proto::UploadLogs& request,
                                      std::string* carrier = nullptr,
                                      int64_t* uploadId = nullptr,
                                      std::string* error = nullptr);
  // The Extender switch's write while there is no session
  // (set_provide_extender). True when the service wrote it; false with `error`
  // for a refusal or a transport failure. Sent only to a service whose
  // get_provider_stats said it takes it (provide_extender_writable).
  bool SetProvideExtender(bool on, std::string* error = nullptr);
  // The lag.dev speed tests switch's write while there is no session
  // (set_measurements_enabled). True when the service wrote it; false with
  // `error` for a refusal or a transport failure. Sent only to a service whose
  // get_provider_stats said it takes it (measurements_enabled_writable).
  bool SetMeasurementsEnabled(bool on, std::string* error = nullptr);
  // The router port mapping switch's write while there is no session
  // (set_provide_extender_port_mapping). True when the service wrote it; false
  // with `error` for a refusal or a transport failure. Sent only to a service
  // whose get_provider_stats said it takes it
  // (provide_extender_port_mapping_writable).
  bool SetProvideExtenderPortMapping(bool on, std::string* error = nullptr);
  // reset_extenders (Protocol.h ResetExtenders): the service applies a reset
  // the app made in its own space to the space it holds under the same key,
  // which its session's device and its provider-only device run in. Taken when
  // the service answered it, with `reset` saying whether it held that space
  // and the reset was new to it; Busy when a tunnel or provider operation held
  // its session lock (Reply::reset_busy), which the app answers by sending the
  // reset again once that operation ends (Common/ExtenderReset.h); NotTaken
  // for any other refusal, a transport failure or a service too old to know
  // the verb ("unknown request type"), with `error` saying which. The next
  // import of the space carries the reset to the service whatever it answers.
  extenderreset::ServiceAnswer ResetExtenders(const proto::ResetExtenders& request,
                                              bool* reset = nullptr,
                                              std::string* error = nullptr);
  // set_ingress_dns (Protocol.h SetIngressDns): the service writes the
  // connection region and the bootstrap DNS-over-HTTPS servers the app's space
  // holds into the space it holds under the same key, which its session's
  // device and its provider-only device run in, where they apply in place.
  // Taken when the service answered it, with `held` saying whether it held
  // that space; Busy when a tunnel or provider operation held its session lock
  // (Reply::ingress_dns_busy), which the app answers by sending the values
  // again once that operation ends (Common/IngressDnsSync.h); NotTaken for any
  // other refusal, a transport failure or a service too old to know the verb
  // ("unknown request type"), with `error` saying which. The next import of
  // the space carries the values to the service whatever it answers.
  ingressdns::ServiceAnswer SetIngressDns(const proto::SetIngressDns& request,
                                          bool* held = nullptr,
                                          std::string* error = nullptr);

 private:
  proto::TunnelStatus CallStatus(const nlohmann::json& request,
                                 bool* answered = nullptr);
  bool CallProvider(const nlohmann::json& request, std::optional<proto::TunnelStatus>* status,
                    std::string* error);

  PipeClient pipe_;
  StateHandler onState_;
  DisconnectHandler onDisconnect_;
};

}  // namespace urnw
