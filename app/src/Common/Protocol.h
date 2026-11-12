// URnetwork control protocol — the contract between the tray app (URnetwork.exe,
// unprivileged, per user) and the service (urnetworkd.exe, LocalSystem).
//
// Transport: newline-delimited UTF-8 JSON over the named pipe
// \\.\pipe\urnetwork.control (see PipeName.h). Each request is one JSON object
// terminated by '\n'; each reply is one JSON object terminated by '\n'. The
// service also pushes unsolicited event objects on the same connection.
//
// This mirrors the macOS app<->extension boundary: `start_tunnel` carries the
// same fields the macOS app puts in NETunnelProviderProtocol.providerConfiguration,
// and device RPC (mTLS WebSocket on loopback) is established once the service
// is prepared. Provider selection/proof precedes capture and the later Up state.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ProvideLifecycle.h"

namespace urnw::proto {

// bump when the wire format changes incompatibly; hello negotiates it
//
// 2: StartTunnel::mode / TunnelStatus::mode + TunnelState::RpcOnly.
//    This bump is load-bearing, not bookkeeping. A version-1 service has no
//    `mode` handler in its from_json, so it SILENTLY DROPS the field: an
//    rpc-only request arrives as a plain start_tunnel, all eight steps run, and
//    the machine's routes and DNS are rewritten by a request that asked for
//    exactly the opposite. `mode` cannot be made safe by its own absence — the
//    only thing that distinguishes "this peer honours mode" from "this peer
//    ignores mode" is the version. Anything requesting RpcOnly MUST refuse to
//    proceed against a peer reporting < kFirstStartModeVersion.
//    See SdkHost::BootstrapSession.
//
//    NOT bumped for StartTunnel::kill_switch, TunnelStatus::dns_applied or
//    TunnelStatus::wfp_state, and the test is the same one that made `mode`
//    load-bearing: does silence mean the wrong thing? It does not. A peer that
//    drops kill_switch arms nothing, which is the permissive default the app
//    already claims when it has no state. A peer that drops dns_applied or
//    wfp_state is read as "DNS not applied" and "no firewall policy", i.e. the
//    DEGRADED reading — the app understates its protection rather than
//    overstating it. `mode` had to be versioned because its absence meant the
//    opposite: routes rewritten by a request that asked for none.
// 3: every live session reports the exact DeviceLocal instance and an opaque
//    RPC session generation. These fields are required for safe adoption; a
//    port match alone cannot prove that saved mTLS material belongs to the
//    listener currently bound there.
// 4: start_tunnel returns Preparing once RPC is usable. Capture is deferred
//    until the selected destination has a usable proven provider.
//
//    Not bumped for start_provider / stop_provider or the TunnelStatus
//    provider_* fields, by the same test: does silence mean the wrong thing? It
//    does not. A service that does not know the verbs answers "unknown request
//    type" and runs nothing, which is the behaviour before they existed, and a
//    peer that drops the provider_* fields reads as "no provider-only device",
//    which claims less than the truth rather than more.
//
//    Nor for get_provider_stats: a service that does not know it answers
//    "unknown request type", which the app reads as "no statistics" and
//    renders as it did before the verb existed (no client count and no
//    provider plots while disconnected). Nor for its later extender fields: a
//    service that does not send them reads as the role unsupported, which
//    hides the extender row and plot while disconnected, as before.
//
//    Nor for the network country (StartTunnel and StartProvider
//    network_country_*, set_network_country): a service that drops the fields
//    or does not know the verb sets no network country, which is the
//    behaviour before they existed, and an app too old to send them leaves a
//    new service with none.
//
//    Nor for StartTunnel::system_proxy, a diagnostic only: a service that drops
//    it writes no proxy line, and an app too old to send it gets "unknown".
//
//    Nor for upload_logs: a service that does not know it answers "unknown
//    request type", and the app falls back to what it did before the verb
//    existed (the DeviceRemote's UploadLogs while a session is bound). Its
//    Reply::log_upload_* and TunnelStatus::log_upload_* fields read as no
//    upload from a peer too old to send them.
//
//    Nor for set_provide_extender and ProviderStats::provide_extender_writable:
//    the app sends the verb only to a service whose get_provider_stats said it
//    takes it, and a service that does not say so reads as no writer, which
//    keeps the Connect page's Extender switch hidden while disconnected, as
//    before.
//
//    Nor for set_measurements_enabled and ProviderStats::measurements_enabled
//    and measurements_enabled_writable (lag.dev/PLAN.md C4): the same rule. The
//    app sends the verb only to a service whose get_provider_stats said it
//    takes it, and a service that does not say so keeps the lag.dev speed
//    tests switch hidden while disconnected; the setting itself still applies,
//    since every device reads it from its space.
//
//    Nor for set_provide_extender_port_mapping and
//    ProviderStats::provide_extender_port_mapping and
//    provide_extender_port_mapping_writable (connect EXTENDER.md S8): the same
//    rule. The app sends the verb only to a service whose get_provider_stats
//    said it takes it, and a service that does not say so keeps the router
//    port mapping switch hidden while disconnected; a peer that drops the
//    setting reads it as off, the apps' default, and the setting itself still
//    applies, since every device reads it from its space.
//
//    Nor for reset_extenders, Reply::reset and Reply::reset_busy: a service
//    that does not know the verb answers "unknown request type" and resets
//    nothing, which is the behaviour before it existed, and the app then leaves
//    the reset to the next import of the space, whose values carry it
//    (extender_reset_id). A peer that drops `reset` reads as nothing reset,
//    which claims less, and one that drops `reset_busy` as a refusal the app
//    does not send again, which the next import covers as well.
//
//    Nor for set_ingress_dns, Reply::ingress_dns_held and
//    Reply::ingress_dns_busy, by the same rule: a service that does not know
//    the verb answers "unknown request type" and writes nothing, which is the
//    behaviour before it existed, and the next import of the space carries the
//    region and the servers (its values hold them). A peer that drops
//    `ingress_dns_held` reads as a space not held, which claims less, and one
//    that drops `ingress_dns_busy` as a refusal the app does not send again,
//    which the next import covers as well.
inline constexpr int kProtocolVersion = 4;

// The first version that understands StartTunnel::mode. Below this, an absent
// `mode` on the wire means "ignored", not "defaulted".
inline constexpr int kFirstStartModeVersion = 2;
inline constexpr int kFirstRpcSessionIdentityVersion = 3;
inline constexpr int kFirstDeferredCaptureVersion = 4;

// ---- message type tags ----------------------------------------------------

namespace msg {
inline constexpr const char* kHello = "hello";                   // app -> service
inline constexpr const char* kStartTunnel = "start_tunnel";      // app -> service
inline constexpr const char* kStopTunnel = "stop_tunnel";        // app -> service
inline constexpr const char* kGetState = "get_state";            // app -> service
inline constexpr const char* kSetSplitTunnel = "set_split_tunnel"; // app -> service
inline constexpr const char* kSetKillSwitch = "set_kill_switch"; // app -> service
inline constexpr const char* kLogout = "logout";                 // app -> service
inline constexpr const char* kStartProvider = "start_provider";  // app -> service
inline constexpr const char* kStopProvider = "stop_provider";    // app -> service
inline constexpr const char* kGetProviderStats = "get_provider_stats";  // app -> service
inline constexpr const char* kSetNetworkCountry = "set_network_country";  // app -> service
inline constexpr const char* kUploadLogs = "upload_logs";        // app -> service
inline constexpr const char* kSetProvideExtender = "set_provide_extender";  // app -> service
// app -> service
inline constexpr const char* kSetMeasurementsEnabled = "set_measurements_enabled";
// app -> service
inline constexpr const char* kSetProvideExtenderPortMapping = "set_provide_extender_port_mapping";
inline constexpr const char* kResetExtenders = "reset_extenders";  // app -> service
inline constexpr const char* kSetIngressDns = "set_ingress_dns";  // app -> service
inline constexpr const char* kReply = "reply";                   // service -> app
inline constexpr const char* kEvent = "event";                   // service -> app (unsolicited)
}  // namespace msg

// What a `start_tunnel` is asking the service to bring up.
//
// RpcOnly exists so the app can be driven end to end before the tunnel itself
// is written: it runs steps 1-5 of TunnelController::StartLocked minus the
// wintun adapter (NetworkSpace, DeviceLocal, the mTLS RPC listener) and STOPS
// before step 6/8, which is the first call that rewrites the machine's routes
// and DNS. Nothing in an RpcOnly session touches the routing table, and because
// no adapter is created it needs no elevation at all.
enum class StartMode {
  Tunnel,   // the real thing: adapter, routes, DNS, packet pump
  RpcOnly,  // DeviceLocal + RPC listener only; the network is not touched
};

inline const char* ToString(StartMode m) {
  switch (m) {
    case StartMode::Tunnel: return "tunnel";
    case StartMode::RpcOnly: return "rpc_only";
  }
  return "tunnel";
}

// Absent parses as Tunnel — that is what every pre-mode client means. An
// unrecognized string returns nullopt rather than falling back: a request that
// meant "do not touch my network" and was misspelled must not be answered with
// a real tunnel. StartTunnel's from_json turns that nullopt into a throw, which
// the ControlServer answers as a failed reply.
inline std::optional<StartMode> StartModeFromString(const std::string& s) {
  if (s == "tunnel") return StartMode::Tunnel;
  if (s == "rpc_only") return StartMode::RpcOnly;
  return std::nullopt;
}

// tunnel lifecycle state, reported in replies and state-change events
enum class TunnelState {
  Stopped,
  Starting,
  Up,       // wintun adapter up, DeviceLocal running, RPC listener ready
  Stopping,
  Error,
  // An rpc-only session is serving: DeviceLocal + RPC listener are live and NO
  // routes exist. Deliberately NOT `Up` — every `state == Up` test in the app
  // means "connected", and this is the state in which the app must not say so.
  // An older peer that does not know this string parses it as `Stopped`, which
  // is the safe direction to be wrong in.
  RpcOnly,
  Preparing,  // RPC live; capture not committed; routes/DNS fields report transition facts
};

inline const char* ToString(TunnelState s) {
  switch (s) {
    case TunnelState::Stopped: return "stopped";
    case TunnelState::Starting: return "starting";
    case TunnelState::Up: return "up";
    case TunnelState::Stopping: return "stopping";
    case TunnelState::Error: return "error";
    case TunnelState::RpcOnly: return "rpc_only";
    case TunnelState::Preparing: return "preparing";
  }
  return "unknown";
}

inline TunnelState TunnelStateFromString(const std::string& s) {
  if (s == "starting") return TunnelState::Starting;
  if (s == "up") return TunnelState::Up;
  if (s == "preparing") return TunnelState::Preparing;
  if (s == "stopping") return TunnelState::Stopping;
  if (s == "error") return TunnelState::Error;
  if (s == "rpc_only") return TunnelState::RpcOnly;
  return TunnelState::Stopped;
}

// "The service has a live DeviceLocal and RPC listener the app can dial." True
// for both modes. Use this for session/reattach decisions.
//
// constexpr so Common/ConnectAction.h — which is pure, and whose whole table is
// evaluated at compile time in the selftest — can express "is there a session"
// with the SAME predicate the rest of the product uses rather than a second
// copy of it.
inline constexpr bool IsSessionLive(TunnelState s) {
  return s == TunnelState::Up || s == TunnelState::RpcOnly ||
         s == TunnelState::Preparing;
}

// "Traffic is actually being carried." Use this, never IsSessionLive, for
// anything the user reads as "connected".
inline constexpr bool IsTunnelUp(TunnelState s) { return s == TunnelState::Up; }

// ---- request payloads -----------------------------------------------------

// StartTunnel mirrors the macOS providerConfiguration. The service builds its
// own NetworkSpace from network_space_json and constructs the DeviceLocal with
// these credentials, then starts the RPC listener the app's DeviceRemote dials.
struct StartTunnel {
  std::string by_jwt;              // client JWT for this device
  std::string network_space_json;  // NetworkSpace.toJson() from the app
  std::string instance_id;         // stable instance UUID
  std::string device_description;  // e.g. "windows-desktop"
  std::string device_spec;         // hardware/model string
  std::string app_version;         // "<version>-<build>"
  // device RPC key material (per session, from GenerateDeviceRpcKeyMaterial):
  std::string rpc_server_pem;      // server key+cert the service presents
  std::string rpc_client_cert_pem; // client cert the service pins (mTLS)
  std::string rpc_listen_hostport; // e.g. "127.0.0.1:12042"
  std::string rpc_session_id;      // opaque per-session adoption generation
  // split tunnel: process image paths. In denylist mode (allowlist_mode=false)
  // these are BYPASS paths (they leave the tunnel; everyone else tunnels). In
  // allowlist mode they are the KEEP-ON-TUNNEL paths (only these tunnel; everyone
  // else bypasses) - mirrors Android's "inclusions take precedence".
  std::vector<std::string> excluded_app_paths;
  bool allowlist_mode = false;
  // Tunnel unless the app explicitly asks otherwise, so an absent field on the
  // wire keeps its pre-mode meaning. See StartMode.
  StartMode mode = StartMode::Tunnel;
  // The user's persisted kill-switch preference, carried at start so the
  // service knows it before it installs a single route. The APP still owns the
  // setting and its persistence (SdkHost::SetKillSwitch, LocalState); what
  // changed is what it drives — a WFP policy in the service rather than the
  // SDK's routeLocal flag, which only ever sees packets the OS already routed
  // INTO the tun and so cannot cover IPv6, LAN, another adapter's resolver, or
  // a dead service.
  //
  // Absent parses as false, the permissive default, matching
  // SdkHost::CurrentKillSwitch's "claim the permissive default, not the strict
  // one" when no state exists. Getting this backwards would have an old client
  // silently arm a kill switch nobody asked for.
  bool kill_switch = false;
  // The network country the app read (Common/NetworkCountry.h): the country of
  // the mobile broadband network carrying the default route, "" for none, and
  // the source token that says why. The service applies it to its own sdk
  // before it imports the network space (TunnelController::SetNetworkCountry),
  // so this session's first extender dials already front with that country's
  // list while the extender hint cannot be fetched. Absent parses as no
  // country, which is what a service did before the field existed.
  std::string network_country_code;
  std::string network_country_source;
  // The kind of the user's system proxy (Common/DiagnosticLines.h
  // UserProxyKind), which the service writes to the log feedback uploads
  // ([app][proxy]). Per user (WinINet), so the LocalSystem service cannot read
  // it, and only its kind: never a host, a port, a URL or the bypass list. The
  // service takes nothing but a kind UserProxyKind can produce. Absent, from
  // an older app, it is written as "unknown"; an older service ignores it.
  std::string system_proxy;
};

struct SetSplitTunnel {
  std::vector<std::string> excluded_app_paths;  // meaning depends on allowlist_mode (see StartTunnel)
  bool allowlist_mode = false;
};

// logout: the account signed out (Common/SignOut.h). The service ends any
// session and the provider-only device, deletes this machine's device identity,
// and logs out what its sdk stored in this network space for the account: the
// client credential and instance a device persists when it starts, among the
// rest. The space comes from the app because a restarted service has imported
// none. Absent, as an older app sends it, the identity alone is deleted, as
// before. No protocol bump: an older service ignores the field and deletes the
// identity, which is what it always did.
struct Logout {
  std::string network_space_json;  // NetworkSpace.toJson() from the app
};

struct SetKillSwitch {
  bool on = false;
};

// start_provider: keep providing while disconnected (ProvideLifecycle.h). The
// service runs a provider-only DeviceLocal built from the persisted device
// identity and these credentials, in this network space, with this provide
// mode — and nothing else: no wintun adapter, no route, no DNS entry, no
// firewall policy and no device RPC listener, so it carries no RPC material.
// Refused while a tunnel session exists or is starting, while the kill
// switch's armed floor is in force, and for a mode that does not provide while
// disconnected (provide::ProviderStartRefusal). The same request again keeps
// the running device and applies the mode and the provider transport policy in
// place (SameProviderDevice); any other request replaces it. stop_provider
// retires it. A Connect with the same identity takes it as the session's
// device and any other Connect retires it first, so a Connect never runs two
// devices under one identity; a disconnect may hand the session's device back
// (provide::KeepsSessionDevice).
struct StartProvider {
  std::string by_jwt;              // client JWT for this device
  std::string network_space_json;  // NetworkSpace.toJson() from the app
  std::string instance_id;         // stable instance UUID
  std::string device_description;
  std::string device_spec;
  std::string app_version;
  std::string provide_mode;        // the provide control mode: "always" | "network" | "auto"
  // The app's mirror of the provider transport policy (TransportSettings json,
  // SdkHost::ApplyTransportSettings). Empty when it was never edited here: the
  // policy the service's DeviceLocal persisted (or its default) stands, exactly
  // as BootstrapSession seeds a tunnel session's device only when one exists.
  std::string provider_transport_settings_json;
  // The network country, as StartTunnel carries it. Applied in place, process
  // wide, so it is not part of SameProviderDevice: a new country never
  // rebuilds the running device.
  std::string network_country_code;
  std::string network_country_source;
};

// set_network_country: the network country the app reads changed (it follows
// the default route, App/NetworkCountryWatch.h), or the app has just greeted a
// service whose running devices may hold an older one. The service applies it
// to its sdk, process-wide and in place, from each device's next extender
// dial; nothing a status reports moves. The fields are StartTunnel's.
struct SetNetworkCountry {
  std::string network_country_code;
  std::string network_country_source;
};

// upload_logs: "send feedback with logs" whether or not a tunnel runs (support
// inbox 2090). The logs support reads are the service's — the sdk's UploadLogs
// zips the glog files of the process it runs in — and the app's DeviceRemote
// reaches the service's DeviceLocal only while a session runs, so a report sent
// while disconnected, held by the kill switch or failing to connect carried
// none. The app now asks the service to upload its own logs for a feedback the
// server accepted, with the credentials start_provider carries.
//
// The service runs the sdk's UploadLogsWithFiles on the session's device, else
// on the provider-only device, else on a standalone device built from these
// credentials (Common/LogUpload.h carries the lifecycle). The upload itself is
// the sdk's: the zip of the service's glog files and of the app's (app_log_dir,
// below), POST /log/{feedback_id}/upload on the space's API with the device's
// client credentials, the server's 100 MB cap and its rate limit of one upload
// per network per 5 minutes. Nothing goes anywhere it did not go before.
//
// The reply comes once the upload is admitted (log_upload_id): the zip and the
// post run on a thread of their own (logupload::Flight), never under the
// session lock, which a zip of up to the upload's cap would hold for as long as
// it reads the disk. One upload at a time: a request while one is in flight is
// refused with log_upload_busy. The outcome reaches the app in the status the
// service pushes when the upload ends: log_upload_id names the upload and
// log_upload_state says how it ended (logupload::CompletionFor).
//
// One set of logs: the server keeps one file per feedback and admits one
// upload per network per 5 minutes, so the app's own glog files ride in the
// service's zip, under app/. The app names its log directory, and the service
// opens the glog files there as the app's pipe client, never with its own
// rights (Common/AppLogFiles.h).
struct UploadLogs {
  std::string feedback_id;         // the server-issued id the logs attach to
  // the same six as StartProvider, used only when no device runs
  std::string by_jwt;
  std::string network_space_json;
  std::string instance_id;
  std::string device_description;
  std::string device_spec;
  std::string app_version;
  // The app's glog directory (urnet::getLogDir in the app). Additive: absent
  // from an older app, and ignored by an older service.
  std::string app_log_dir;
};

inline void to_json(nlohmann::json& j, const UploadLogs& v) {
  j = {
      {"feedback_id", v.feedback_id},
      {"by_jwt", v.by_jwt},
      {"network_space_json", v.network_space_json},
      {"instance_id", v.instance_id},
      {"device_description", v.device_description},
      {"device_spec", v.device_spec},
      {"app_version", v.app_version},
  };
  if (!v.app_log_dir.empty()) j["app_log_dir"] = v.app_log_dir;
}

inline void from_json(const nlohmann::json& j, UploadLogs& v) {
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  get("feedback_id", v.feedback_id);
  get("by_jwt", v.by_jwt);
  get("network_space_json", v.network_space_json);
  get("instance_id", v.instance_id);
  get("device_description", v.device_description);
  get("device_spec", v.device_spec);
  get("app_version", v.app_version);
  get("app_log_dir", v.app_log_dir);
}

// The server's feedback ids are uuids, and this one becomes a path segment of
// the API url the service's device posts to (/log/<feedback_id>/upload).
// Anything else — a '/', a "..", a query — is refused before it reaches the
// SDK.
inline bool LooksLikeFeedbackId(std::string_view id) {
  if (id.size() != 36) return false;
  for (std::size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

// set_provide_extender: the Extender switch while disconnected (EXTENDER.md F3,
// N7). The setting belongs to the network space (`.provide_extender` in the
// service's storage), and every device reads it from its space when it starts.
// A session's DeviceRemote writes it over the device rpc; with no session the
// service writes it through the device it runs, or with none into the space
// the last device ran in (provide::ExtenderSettingTargetFor). So a Connect
// after the change reads it, and so does a provider-only device after a change
// made while connected. Sent only over a status whose
// ProviderStats::provide_extender_writable said the service takes it.
struct SetProvideExtender {
  bool provide_extender = true;
};

// set_measurements_enabled: the lag.dev speed tests switch while disconnected
// (lag.dev/PLAN.md C4, decision 18). Like the provide extender setting it
// belongs to the network space (`.measurements_enabled`), every device reads
// it from its space when it starts, and it is written through the same
// targets (provide::ExtenderSettingTargetFor). Sent only over a status whose
// ProviderStats::measurements_enabled_writable said the service takes it.
struct SetMeasurementsEnabled {
  bool measurements_enabled = true;
};

// set_provide_extender_port_mapping: the router port mapping switch while
// disconnected ("Open a port on my router", connect EXTENDER.md S8): whether
// the extender role asks the router, by UPnP, NAT-PMP or PCP, to forward its
// ports (tcp 443, udp 443 and udp 4053) to this device, which opens them on the
// router. Off until the user turns it on, on every app (owner, 2026-11-11; the
// miner turns it on). Like the provide extender setting it belongs to the
// network space (`.provide_extender_port_mapping`), every device reads it from
// its space when it starts, and it is written through the same targets
// (provide::ExtenderSettingTargetFor), with no device into the space the last
// device ran in. Sent only over a status whose
// ProviderStats::provide_extender_port_mapping_writable said the service takes
// it.
struct SetProvideExtenderPortMapping {
  bool provide_extender_port_mapping = false;
};

// reset_extenders: "Reset extenders" in the app's Account > Extenders (connect
// EXTENDER.md E7). The app has reset its own network space -- everything it
// learned about extenders is cleared and the extenders a user added are
// removed -- and hands the service the id that reset returned
// (NetworkSpace::resetExtenders) with the space's key. The service applies it
// to the space it holds under that key (NetworkSpace::applyExtenderReset),
// which the session's device and the provider-only device both run in, so one
// call covers both; a live extender path keeps running, and every new extender
// dial draws from the fresh directory. A reset the space has applied already,
// or an older one, changes nothing. A service that holds no such space resets
// nothing: the next import of the space (start_tunnel, start_provider) carries
// the id in its values and applies it there. Every field is required.
struct ResetExtenders {
  // the space's key
  std::string host_name;
  std::string env_name;
  // what NetworkSpace::resetExtenders returned in the app
  std::string extender_reset_id;
};

// The reset_extenders request for the space `key` names, after a reset that
// returned `resetId`: nullopt when the key or the id is missing, which the
// service would refuse, so the next import of the space carries the reset
// instead. A template over urnet::NetworkSpaceKey by the sdk's field names, so
// this header needs no sdk header.
template <class Key>
std::optional<ResetExtenders> ResetExtendersRequestFor(const std::optional<Key>& key,
                                                       const std::string& resetId) {
  if (!key || !key->host_name || !key->env_name) return std::nullopt;
  ResetExtenders request;
  request.host_name = *key->host_name;
  request.env_name = *key->env_name;
  request.extender_reset_id = resetId;
  if (request.host_name.empty() || request.env_name.empty() ||
      request.extender_reset_id.empty()) {
    return std::nullopt;
  }
  return request;
}

// The key of the space a reset_extenders request names, as the service's space
// manager looks it up (urnet::NetworkSpaceKey).
template <class Key>
Key SpaceKeyOf(const ResetExtenders& request) {
  Key key;
  key.host_name = request.host_name;
  key.env_name = request.env_name;
  return key;
}

// set_ingress_dns: the network space's ingress dns (sdk control_doh.go), the
// connection region (NetworkSpace::setIngressCountryCode) and the bootstrap
// DNS-over-HTTPS servers (NetworkSpace::setControlDohUrls), which pick the
// resolvers URnetwork's own names (api, connect, extender) are looked up
// through. The app saves them in its own space, where they apply in place, and
// hands the service the values its space then holds, with the space's key. The
// service writes them through the same setters into the space it holds under
// that key, which the session's device and the provider-only device both run
// in, so they apply in place there too: a connection attempt that cannot reach
// URnetwork resolves through a region chosen from the can't-reach prompt at its
// next lookup, as an in-process tunnel does, rather than only after a
// reconnect. The app also sends them when it reattaches to a running session,
// which imported the space when it started. A service that holds no such space
// writes nothing: the next import of the space (start_tunnel, start_provider)
// carries the values. Every field is required; an empty region is undisclosed,
// and no servers is the built-in servers alone.
struct SetIngressDns {
  // the space's key
  std::string host_name;
  std::string env_name;
  // what NetworkSpace::getIngressCountryCode answers in the app
  std::string ingress_country_code;
  // what NetworkSpace::getControlDohUrls answers in the app, v4 then v6
  std::vector<std::string> control_doh_urls;
};

// The set_ingress_dns request for the space `key` names, with the region and
// the servers that space holds: nullopt when the key is missing, which the
// service would refuse, so the next import of the space carries the values
// instead. A template over urnet::NetworkSpaceKey by the sdk's field names, so
// this header needs no sdk header.
template <class Key>
std::optional<SetIngressDns> SetIngressDnsRequestFor(const std::optional<Key>& key,
                                                     const std::string& ingressCountryCode,
                                                     const std::vector<std::string>& controlDohUrls) {
  if (!key || !key->host_name || !key->env_name) return std::nullopt;
  SetIngressDns request;
  request.host_name = *key->host_name;
  request.env_name = *key->env_name;
  if (request.host_name.empty() || request.env_name.empty()) return std::nullopt;
  request.ingress_country_code = ingressCountryCode;
  request.control_doh_urls = controlDohUrls;
  return request;
}

// The key of the space a set_ingress_dns request names, as the service's space
// manager looks it up (urnet::NetworkSpaceKey).
template <class Key>
Key SpaceKeyOf(const SetIngressDns& request) {
  Key key;
  key.host_name = request.host_name;
  key.env_name = request.env_name;
  return key;
}

// ---- reply / state payload ------------------------------------------------

struct TunnelStatus {
  TunnelState state = TunnelState::Stopped;
  std::string rpc_listen_hostport;  // echoed so the app can dial the DeviceRemote
  std::string instance_id;          // exact live DeviceLocal pairing identity
  std::string rpc_session_id;       // exact live RPC credential generation
  std::string error;                // set when state == Error
  std::string service_version;
  // Zero is the fail-closed default for an absent field. Initialising this to
  // the current version would let an old peer that omitted it pass feature
  // gates for fields it silently ignores. The service sets its real version
  // explicitly in every status it publishes.
  int protocol_version = 0;
  // best-effort counters (authoritative stats come over the device RPC)
  int64_t tunnel_local_up_millis = 0;
  // The mode of the session this status describes. Reported separately from
  // `state` so it survives Starting/Stopping/Error, where `state` says nothing
  // about which kind of session was asked for. In a live session the two agree
  // by construction: the controller derives both from one stored mode.
  //
  // Defaults to Tunnel, and that is right even for an absent field: a peer old
  // enough not to send `mode` is a peer that only ever built real tunnels, so
  // reading its silence as "Tunnel" is honest. What is NOT safe is asking such
  // a peer for RpcOnly — see kFirstStartModeVersion.
  StartMode mode = StartMode::Tunnel;
  // Controller-reported route-configuration ownership; false for rpc-only.
  // May be true during partial Apply. Not a fresh route-table query: false after
  // cleanup requests does not certify kernel removal.
  bool routes_installed = false;
  // Current owner's IPv4 DNS-apply result, cleared after cleanup requests.
  // IPv6 failures are logged separately. Capture commits only after this flag
  // is true; false on teardown does not independently verify resolver removal.
  // Defaults false for a peer too old to send it.
  bool dns_applied = false;
  // The firewall policy in force: "off" | "armed" | "connecting" | "connected".
  // Reported so the app can say whether leak prevention is actually running — on
  // an unelevated or otherwise failed install it is "off" while the tunnel is
  // up, which is a materially different state from a protected one.
  //
  // "connecting" arrived with the Armed/Connecting split (Service/WfpPolicy.h)
  // and needed NO protocol bump, by the same test the rest of this block uses:
  // a peer that does not know the string reads it as "not off", i.e. some policy
  // is in force — which is true, and understates rather than overstates
  // protection. What it does NOT carry is that DNS is open machine-wide while it
  // holds; that is disclosed in the kill-switch copy, not inferred from here.
  std::string wfp_state = "off";
  // THE PHYSICAL EGRESS INTERFACE THE SERVICE HAS ITS OWN SDK PINNED TO — the
  // ifIndex passed to setEgressInterfaceIndex at step 2/8, i.e. IP_UNICAST_IF.
  // 0 when nothing is pinned (no tunnel, or no physical default route).
  //
  // Reported because the app runs a SECOND SDK instance in a SECOND process, and
  // that instance's egress binding is process-global inside its own copy of the
  // DLL — the service's bind cannot reach it. Without this the app's platform
  // sockets follow the route table into the tun as soon as the tunnel is up, so
  // the UI's own account/auth/JWT traffic is carried by the tunnel it exists to
  // report on. Observed 2026-08-08: "[dtm]failed to refresh JWT: Timeout."
  // logged by URnetwork.exe while a tunnel was up with no working exit.
  //
  // The service is the right source rather than the app computing it itself:
  // DiscoverEgress has to EXCLUDE the tun LUID to get the right answer, only the
  // service knows that LUID, and EgressMonitor already re-validates the index and
  // deliberately retains the last good one rather than unbinding (see its
  // Refresh). Duplicating that logic in the app would be a second implementation
  // of an R1 rule that must not drift.
  //
  // NO PROTOCOL BUMP. A peer too old to send it leaves 0, which the app reads as
  // "do not bind" — exactly today's behaviour. Absent means unchanged, and the
  // safe direction is the default.
  int64_t egress_index4 = 0;
  int64_t egress_index6 = 0;
  // WHY THE LAST TEARDOWN HAPPENED. "" | "user" | "failsafe_no_exit" |
  // "failsafe_no_inbound" | "failsafe_sdk_unresponsive" (Service/DeadTunnelVerdict.h
  // owns the failsafe spellings; they are literals there, not built by hand). An
  // account out of balance is never stopped with the first two.
  //
  // Without this a failsafe teardown is INDISTINGUISHABLE FROM THE USER PRESSING
  // DISCONNECT, and that is the difference between "you turned it off" and "it
  // turned itself off to keep you online" — which is the only sentence that
  // makes the behaviour acceptable rather than alarming.
  //
  // NO PROTOCOL BUMP, by the same test the block above uses: a peer too old to
  // send it leaves "", which reads as "no reason given" and renders exactly as
  // today. Absent means unchanged, and unchanged is the safe direction.
  std::string stop_reason;
  // A dead-tunnel countdown is running RIGHT NOW and is close enough to matter
  // (Service/TunnelWatchdogSettings.h, DeadTunnelSettings::failsafeNoticeMillis).
  // The UI warns on this, so an automatic teardown is never a surprise. It stays
  // false out of balance, unless the sdk itself has stopped answering. Defaults
  // false — a peer that cannot say simply never warns, which is today's
  // behaviour.
  bool failsafe_armed = false;
  // The provider-only device (start_provider), which provides while there is
  // no tunnel session. Its running bit, the control mode it was asked for, its
  // live tier as the sdk applied it (0 none, 1 network, 3 public) and whether
  // it holds a Network-mode provide key, read off the device when it was built
  // or re-moded. All false/empty while a tunnel session runs: that session's
  // own device is the provider, and the app reads it over the device RPC.
  //
  // No protocol bump (see kProtocolVersion): a peer too old to send these reads
  // as "no provider-only device", which claims less than the truth.
  bool provider_running = false;
  std::string provider_control_mode;
  int64_t provider_mode = 0;
  bool provider_network_key = false;
  // The log upload (upload_logs): the last upload the app asked the service
  // for, as logupload::Flight reads it. Its id (the reply's log_upload_id; 0
  // before the first), where it is (logupload::ToString(FlightState):
  // "running", "uploaded", "refused", "failed") and the device that carries
  // it. The app reads the outcome of its upload here. No protocol bump: a peer
  // too old to send these reads as no upload.
  int64_t log_upload_id = 0;
  std::string log_upload_state;
  std::string log_upload_carrier;
};

// get_provider_stats: what the provider-only device carries, for the screens a
// tunnel session's DeviceRemote feeds while connected — the Connect page's
// client count and its network peers, the Earnings provider plots with their
// gate and their "no traffic yet" line, the Earnings extender row and extender
// plot, and the client id whose GET /network/provider-status row is this
// device's. The service reads them off the device's own ContractViewController
// (the controller the app opens on the DeviceRemote) and its network peers and
// extender status listeners, never with the session lock or a device call on
// the way (TunnelController::ProviderStats), so asking cannot wedge the
// control pipe. `available` is false while no provider-only device runs, or
// while its statistics could not be opened; every other field is then empty.
//
// The points, the distribution and the extender status are the sdk's own json
// (urnet::ThroughputPoint, urnet::TransportDistribution,
// urnet::ExtenderProvideStatus), carried as they are: the service writes them
// with the header's to_json and the app reads them back with its from_json
// (ProviderPointsOf, ProviderDistributionOf, ExtenderPointsOf,
// ExtenderProvideStatusOf), so this file needs no sdk header and the field
// names keep one owner.
//
// The extender fields came after the verb, with no protocol bump, by the test
// kProtocolVersion uses: a service that does not send them leaves no status,
// which the app reads as the role unsupported and hides the extender row and
// plot (EXTENDER.md N1), exactly as before they existed. So did
// provide_extender_writable: a service that does not send it reads as one that
// cannot take the switch's write, and the switch stays hidden over its status.
struct ProviderStats {
  bool available = false;
  std::string client_id;
  // the device's connected network peers (Device::getNetworkPeers' Connected),
  // which the Connect page shows as "Providing to N clients"
  int64_t client_count = 0;
  // Those peers themselves, as the sdk's NetworkPeerList json (NetworkPeersOf)
  // with each peer cut to what the rows show and connect with: client id,
  // provide enabled, device name and spec. The Connect page's network peers,
  // the chooser's and the Network destination's, which a session's
  // PeerViewController feeds while connected. Read with the count, in one
  // reading. A service from before the field never sends it, which the app
  // reads as no list: the count stands alone, as it did.
  nlohmann::json network_peers;
  int64_t window_seconds = 60;
  // the provider section's gate (ContractViewController::getProviderPacketStats)
  bool has_provider_stats = false;
  nlohmann::json provider_points = nlohmann::json::array();
  nlohmann::json provider_distribution;  // null when there is none
  // the extender series (ContractViewController::getExtenderThroughputPoints)
  nlohmann::json extender_points = nlohmann::json::array();
  // The provider extender role on the device (EXTENDER.md N2, N7): the status
  // its listener pushed last (Device::getExtenderProvideStatus before the
  // first), null when none was read, and the setting read beside it
  // (Device::getProvideExtender). Whole, so the router port mapping's state
  // and its two texts (PortMapping, PortMappingMethodName, PortMappingPorts,
  // connect EXTENDER.md S10) ride it as they do a session's status, and the
  // line under the port mapping switch reads the same either way; a status
  // from a service built before the texts reads them empty, which shows no
  // line. The app's view reads nothing else of the port mapping
  // (ExtenderProvideStatusViewOf).
  nlohmann::json extender_provide_status;
  bool provide_extender = false;
  // The service takes set_provide_extender for that setting, sent with the
  // role's reading: the Connect page's Extender switch may show over it. A
  // service from before the verb never sends it, which the app reads as no
  // writer and keeps the switch hidden while disconnected (N1), as before.
  bool provide_extender_writable = false;
  // The lag.dev measurements setting (Device::getMeasurementsEnabled), read
  // beside the provide extender setting: the lag.dev speed tests switch under
  // the Extender switch. On by default, as the sdk's.
  bool measurements_enabled = true;
  // The service takes set_measurements_enabled for that setting. A service
  // from before the verb never sends it, which the app reads as no writer and
  // keeps the speed tests switch hidden while disconnected (N1).
  bool measurements_enabled_writable = false;
  // The router port mapping setting (Device::getProvideExtenderPortMapping,
  // connect EXTENDER.md S8), read beside the provide extender setting: the
  // "Open a port on my router" switch under the speed tests switch. Off by
  // default, as the sdk's on every app, so a service that does not send it
  // reads as off.
  bool provide_extender_port_mapping = false;
  // The service takes set_provide_extender_port_mapping for that setting. A
  // service from before the verb never sends it, which the app reads as no
  // writer and keeps the router port mapping switch hidden while disconnected
  // (N1).
  bool provide_extender_port_mapping_writable = false;
};

struct Reply {
  bool ok = false;
  std::string error;             // set when !ok
  std::optional<TunnelStatus> status;
  std::string in_reply_to;       // request type tag this answers
  // get_provider_stats' answer; a service too old to know the verb sends none
  std::optional<ProviderStats> provider_stats;
  // upload_logs' answer: the device that carries the upload
  // (logupload::ToString), empty in every other reply
  std::string log_upload_carrier;
  // upload_logs' answer: the id status reports the upload under; 0 in every
  // other reply, and from a service too old to say
  int64_t log_upload_id = 0;
  // upload_logs refused because an upload is in flight already
  bool log_upload_busy = false;
  // reset_extenders' answer: the service held the space and the reset was new
  // to it. False in every other reply, and from a service too old to say.
  bool reset = false;
  // reset_extenders refused because a tunnel or provider operation held the
  // service's session lock: the app sends it again once that operation ends
  // (Common/ExtenderReset.h). False in every other reply.
  bool reset_busy = false;
  // set_ingress_dns' answer: the service held the space and wrote the values
  // into it. False in every other reply, and from a service too old to say.
  bool ingress_dns_held = false;
  // set_ingress_dns refused because a tunnel or provider operation held the
  // service's session lock: the app sends it again once that operation ends
  // (Common/IngressDnsSync.h). False in every other reply.
  bool ingress_dns_busy = false;
};

// ---- JSON (de)serialization ----------------------------------------------
// Uses nlohmann's find/get_to pattern so unknown/absent fields are tolerated,
// matching the SDK wrapper's forward-compatible convention.

inline void to_json(nlohmann::json& j, const StartTunnel& v) {
  j = {
      {"by_jwt", v.by_jwt},
      {"network_space_json", v.network_space_json},
      {"instance_id", v.instance_id},
      {"device_description", v.device_description},
      {"device_spec", v.device_spec},
      {"app_version", v.app_version},
      {"rpc_server_pem", v.rpc_server_pem},
      {"rpc_client_cert_pem", v.rpc_client_cert_pem},
      {"rpc_listen_hostport", v.rpc_listen_hostport},
      {"rpc_session_id", v.rpc_session_id},
      {"excluded_app_paths", v.excluded_app_paths},
      {"allowlist_mode", v.allowlist_mode},
      {"mode", ToString(v.mode)},
      {"kill_switch", v.kill_switch},
      {"network_country_code", v.network_country_code},
      {"network_country_source", v.network_country_source},
      {"system_proxy", v.system_proxy},
  };
}

inline void from_json(const nlohmann::json& j, StartTunnel& v) {
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  if (auto it = j.find("mode"); it != j.end() && it->is_string()) {
    const std::string raw = it->get<std::string>();
    auto mode = StartModeFromString(raw);
    // Deliberately fatal. Silently defaulting an unrecognized mode to Tunnel
    // would answer "do not touch my network" with a rewritten route table.
    if (!mode) throw std::runtime_error("unknown start mode: " + raw);
    v.mode = *mode;
  }
  get("by_jwt", v.by_jwt);
  get("network_space_json", v.network_space_json);
  get("instance_id", v.instance_id);
  get("device_description", v.device_description);
  get("device_spec", v.device_spec);
  get("app_version", v.app_version);
  get("rpc_server_pem", v.rpc_server_pem);
  get("rpc_client_cert_pem", v.rpc_client_cert_pem);
  get("rpc_listen_hostport", v.rpc_listen_hostport);
  get("rpc_session_id", v.rpc_session_id);
  get("excluded_app_paths", v.excluded_app_paths);
  get("allowlist_mode", v.allowlist_mode);
  get("kill_switch", v.kill_switch);
  get("network_country_code", v.network_country_code);
  get("network_country_source", v.network_country_source);
  get("system_proxy", v.system_proxy);
}

inline void to_json(nlohmann::json& j, const SetSplitTunnel& v) {
  j = {{"excluded_app_paths", v.excluded_app_paths},
       {"allowlist_mode", v.allowlist_mode}};
}
inline void from_json(const nlohmann::json& j, SetSplitTunnel& v) {
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  get("excluded_app_paths", v.excluded_app_paths);
  get("allowlist_mode", v.allowlist_mode);
}

inline void to_json(nlohmann::json& j, const Logout& v) {
  j = {{"network_space_json", v.network_space_json}};
}
inline void from_json(const nlohmann::json& j, Logout& v) {
  if (auto it = j.find("network_space_json"); it != j.end() && it->is_string())
    it->get_to(v.network_space_json);
}

inline void to_json(nlohmann::json& j, const SetKillSwitch& v) {
  j = {{"on", v.on}};
}
inline void from_json(const nlohmann::json& j, SetKillSwitch& v) {
  if (auto it = j.find("on"); it != j.end() && !it->is_null()) it->get_to(v.on);
}

inline void to_json(nlohmann::json& j, const StartProvider& v) {
  j = {
      {"by_jwt", v.by_jwt},
      {"network_space_json", v.network_space_json},
      {"instance_id", v.instance_id},
      {"device_description", v.device_description},
      {"device_spec", v.device_spec},
      {"app_version", v.app_version},
      {"provide_mode", v.provide_mode},
      {"provider_transport_settings_json", v.provider_transport_settings_json},
      {"network_country_code", v.network_country_code},
      {"network_country_source", v.network_country_source},
  };
}

inline void from_json(const nlohmann::json& j, StartProvider& v) {
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  get("by_jwt", v.by_jwt);
  get("network_space_json", v.network_space_json);
  get("instance_id", v.instance_id);
  get("device_description", v.device_description);
  get("device_spec", v.device_spec);
  get("app_version", v.app_version);
  get("provide_mode", v.provide_mode);
  get("provider_transport_settings_json", v.provider_transport_settings_json);
  get("network_country_code", v.network_country_code);
  get("network_country_source", v.network_country_source);
}

inline void to_json(nlohmann::json& j, const SetNetworkCountry& v) {
  j = {{"network_country_code", v.network_country_code},
       {"network_country_source", v.network_country_source}};
}
inline void from_json(const nlohmann::json& j, SetNetworkCountry& v) {
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  get("network_country_code", v.network_country_code);
  get("network_country_source", v.network_country_source);
}

inline void to_json(nlohmann::json& j, const SetProvideExtender& v) {
  j = {{"provide_extender", v.provide_extender}};
}

// Strict, unlike the reports: a write whose value did not arrive must not
// write a default the user never chose. The ControlServer answers the throw
// as a failed reply.
inline void from_json(const nlohmann::json& j, SetProvideExtender& v) {
  auto it = j.find("provide_extender");
  if (it == j.end() || !it->is_boolean()) {
    throw std::runtime_error("set_provide_extender requires provide_extender (a boolean)");
  }
  v.provide_extender = it->get<bool>();
}

inline void to_json(nlohmann::json& j, const SetMeasurementsEnabled& v) {
  j = {{"measurements_enabled", v.measurements_enabled}};
}

// Strict, as set_provide_extender's: a write whose value did not arrive must
// not write a default the user never chose.
inline void from_json(const nlohmann::json& j, SetMeasurementsEnabled& v) {
  auto it = j.find("measurements_enabled");
  if (it == j.end() || !it->is_boolean()) {
    throw std::runtime_error("set_measurements_enabled requires measurements_enabled (a boolean)");
  }
  v.measurements_enabled = it->get<bool>();
}

inline void to_json(nlohmann::json& j, const SetProvideExtenderPortMapping& v) {
  j = {{"provide_extender_port_mapping", v.provide_extender_port_mapping}};
}

// Strict, as set_provide_extender's: a write whose value did not arrive must
// not write a default the user never chose, and least of all open a port.
inline void from_json(const nlohmann::json& j, SetProvideExtenderPortMapping& v) {
  auto it = j.find("provide_extender_port_mapping");
  if (it == j.end() || !it->is_boolean()) {
    throw std::runtime_error(
        "set_provide_extender_port_mapping requires provide_extender_port_mapping (a boolean)");
  }
  v.provide_extender_port_mapping = it->get<bool>();
}

inline void to_json(nlohmann::json& j, const ResetExtenders& v) {
  j = {{"host_name", v.host_name},
       {"env_name", v.env_name},
       {"extender_reset_id", v.extender_reset_id}};
}

// Strict, like set_provide_extender's: a request that does not name its space
// exactly must not reset another, and an id that did not arrive is no reset.
// The ControlServer answers the throw as a failed reply.
inline void from_json(const nlohmann::json& j, ResetExtenders& v) {
  auto required = [&](const char* k, std::string& out) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
      throw std::runtime_error(std::string("reset_extenders requires ") + k +
                               " (a non-empty string)");
    }
    out = it->get<std::string>();
  };
  required("host_name", v.host_name);
  required("env_name", v.env_name);
  required("extender_reset_id", v.extender_reset_id);
}

inline void to_json(nlohmann::json& j, const SetIngressDns& v) {
  j = {{"host_name", v.host_name},
       {"env_name", v.env_name},
       {"ingress_country_code", v.ingress_country_code},
       {"control_doh_urls", v.control_doh_urls}};
}

// Strict, like reset_extenders': a request that does not name its space
// exactly must not write another's, and a value that did not arrive is not
// written as a default the user never chose (a missing region is not
// undisclosed, missing servers are not the built-in servers alone). The
// ControlServer answers the throw as a failed reply.
inline void from_json(const nlohmann::json& j, SetIngressDns& v) {
  auto requiredKey = [&](const char* k, std::string& out) {
    auto it = j.find(k);
    if (it == j.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
      throw std::runtime_error(std::string("set_ingress_dns requires ") + k +
                               " (a non-empty string)");
    }
    out = it->get<std::string>();
  };
  requiredKey("host_name", v.host_name);
  requiredKey("env_name", v.env_name);
  auto region = j.find("ingress_country_code");
  if (region == j.end() || !region->is_string()) {
    throw std::runtime_error("set_ingress_dns requires ingress_country_code (a string)");
  }
  v.ingress_country_code = region->get<std::string>();
  auto urls = j.find("control_doh_urls");
  if (urls == j.end() || !urls->is_array()) {
    throw std::runtime_error("set_ingress_dns requires control_doh_urls (an array of strings)");
  }
  std::vector<std::string> controlDohUrls;
  for (const auto& url : *urls) {
    if (!url.is_string()) {
      throw std::runtime_error("set_ingress_dns requires control_doh_urls (an array of strings)");
    }
    controlDohUrls.push_back(url.get<std::string>());
  }
  v.control_doh_urls = std::move(controlDohUrls);
}

namespace detail {
// RFC 4648 §5 base64url, padding optional, as a jwt segment carries it. False
// for any other character.
inline bool DecodeBase64Url(std::string_view in, std::string& out) {
  out.clear();
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const char c : in) {
    int value;
    if (c >= 'A' && c <= 'Z') value = c - 'A';
    else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
    else if (c >= '0' && c <= '9') value = c - '0' + 52;
    else if (c == '-') value = 62;
    else if (c == '_') value = 63;
    else if (c == '=') break;
    else return false;
    buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFFu));
    }
  }
  return true;
}
}  // namespace detail

// The client a credential names: its network_id and client_id claims, read
// without verifying the signature, as the sdk reads them (parseByJwtClientId)
// and as the service already takes the token from the app over its own pipe.
// A refreshed token names the same client. Anything that is not a jwt naming a
// client is its own subject, compared whole, as every credential was before.
inline std::string JwtSubject(std::string_view jwt) {
  const std::size_t first = jwt.find('.');
  const std::size_t second =
      first == std::string_view::npos ? first : jwt.find('.', first + 1);
  std::string payload;
  if (second == std::string_view::npos ||
      !detail::DecodeBase64Url(jwt.substr(first + 1, second - first - 1), payload))
    return std::string(jwt);
  const nlohmann::json claims = nlohmann::json::parse(payload, nullptr, false);
  if (!claims.is_object()) return std::string(jwt);
  const auto client = claims.find("client_id");
  if (client == claims.end() || !client->is_string() ||
      client->get_ref<const std::string&>().empty())
    return std::string(jwt);
  const auto network = claims.find("network_id");
  const std::string networkId = network != claims.end() && network->is_string()
                                    ? network->get<std::string>()
                                    : std::string();
  return "client:" + networkId + "/" + client->get<std::string>();
}

// The identity a DeviceLocal is built from (provide::DeviceIdentity), as each
// request carries it: the client its credential names, and views into the
// request for the rest, valid while the request lives.
inline provide::DeviceIdentity IdentityOf(const StartProvider& r) {
  return {JwtSubject(r.by_jwt), r.network_space_json, r.instance_id, r.device_description,
          r.device_spec, r.app_version};
}
inline provide::DeviceIdentity IdentityOf(const StartTunnel& r) {
  return {JwtSubject(r.by_jwt), r.network_space_json, r.instance_id, r.device_description,
          r.device_spec, r.app_version};
}

// "A device built from `a` can keep running for `b`." The identity it was
// built from must match — the client its credential names, identity and
// space; a refreshed credential for the same client is the same device. The provide mode,
// the provider transport policy and the network country may differ: the
// running device takes a new mode and a new policy in place (the build applies
// both the same way, right after construction), and the service applies the
// country in place for every device. Shared by the service, which keeps its
// device for such a request, and the tests.
inline bool SameProviderDevice(const StartProvider& a, const StartProvider& b) {
  return provide::SameIdentity(IdentityOf(a), IdentityOf(b));
}

inline void to_json(nlohmann::json& j, const TunnelStatus& v) {
  j = {
      {"state", ToString(v.state)},
      {"rpc_listen_hostport", v.rpc_listen_hostport},
      {"instance_id", v.instance_id},
      {"rpc_session_id", v.rpc_session_id},
      {"error", v.error},
      {"service_version", v.service_version},
      {"protocol_version", v.protocol_version},
      {"tunnel_local_up_millis", v.tunnel_local_up_millis},
      {"mode", ToString(v.mode)},
      {"routes_installed", v.routes_installed},
      {"dns_applied", v.dns_applied},
      {"wfp_state", v.wfp_state},
      {"egress_index4", v.egress_index4},
      {"egress_index6", v.egress_index6},
      {"stop_reason", v.stop_reason},
      {"failsafe_armed", v.failsafe_armed},
      {"provider_running", v.provider_running},
      {"provider_control_mode", v.provider_control_mode},
      {"provider_mode", v.provider_mode},
      {"provider_network_key", v.provider_network_key},
      {"log_upload_id", v.log_upload_id},
      {"log_upload_state", v.log_upload_state},
      {"log_upload_carrier", v.log_upload_carrier},
  };
}

inline void from_json(const nlohmann::json& j, TunnelStatus& v) {
  if (auto it = j.find("state"); it != j.end() && it->is_string())
    v.state = TunnelStateFromString(it->get<std::string>());
  // A status is a report, so an unreadable mode degrades rather than throws —
  // but it degrades to RpcOnly, the mode that claims LESS. (The service always
  // writes a valid one; this is for a peer we do not control.)
  if (auto it = j.find("mode"); it != j.end() && it->is_string())
    v.mode = StartModeFromString(it->get<std::string>()).value_or(StartMode::RpcOnly);
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  get("rpc_listen_hostport", v.rpc_listen_hostport);
  get("instance_id", v.instance_id);
  get("rpc_session_id", v.rpc_session_id);
  get("error", v.error);
  get("service_version", v.service_version);
  get("protocol_version", v.protocol_version);
  get("tunnel_local_up_millis", v.tunnel_local_up_millis);
  get("routes_installed", v.routes_installed);
  get("dns_applied", v.dns_applied);
  get("wfp_state", v.wfp_state);
  get("egress_index4", v.egress_index4);
  get("egress_index6", v.egress_index6);
  get("stop_reason", v.stop_reason);
  get("failsafe_armed", v.failsafe_armed);
  get("provider_running", v.provider_running);
  get("provider_control_mode", v.provider_control_mode);
  get("provider_mode", v.provider_mode);
  get("provider_network_key", v.provider_network_key);
  get("log_upload_id", v.log_upload_id);
  get("log_upload_state", v.log_upload_state);
  get("log_upload_carrier", v.log_upload_carrier);
}

// What one status says about the provider-only device, as the app's reconcile
// reads it (provide::DisconnectedProviderStep). `answered` is the transport's
// word that the service replied (ServiceClient::GetState); nothing in the
// payload can carry it.
inline provide::ServiceProviderFacts ProviderFactsFrom(const TunnelStatus& s, bool answered) {
  provide::ServiceProviderFacts facts;
  facts.answered = answered;
  facts.tunnelSession = IsSessionLive(s.state) || s.state == TunnelState::Starting ||
                        s.state == TunnelState::Stopping;
  // Empty reads as off, as ConnectAction.h's WfpInForce does.
  facts.killSwitchArmed = !s.wfp_state.empty() && s.wfp_state != "off";
  facts.providerRunning = s.provider_running;
  return facts;
}

inline void to_json(nlohmann::json& j, const ProviderStats& v) {
  j = {
      {"available", v.available},
      {"client_id", v.client_id},
      {"client_count", v.client_count},
      {"network_peers", v.network_peers},
      {"window_seconds", v.window_seconds},
      {"has_provider_stats", v.has_provider_stats},
      {"provider_points", v.provider_points},
      {"provider_distribution", v.provider_distribution},
      {"extender_points", v.extender_points},
      {"extender_provide_status", v.extender_provide_status},
      {"provide_extender", v.provide_extender},
      {"provide_extender_writable", v.provide_extender_writable},
      {"measurements_enabled", v.measurements_enabled},
      {"measurements_enabled_writable", v.measurements_enabled_writable},
      {"provide_extender_port_mapping", v.provide_extender_port_mapping},
      {"provide_extender_port_mapping_writable", v.provide_extender_port_mapping_writable},
  };
}

inline void from_json(const nlohmann::json& j, ProviderStats& v) {
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  get("available", v.available);
  get("client_id", v.client_id);
  get("client_count", v.client_count);
  get("window_seconds", v.window_seconds);
  get("has_provider_stats", v.has_provider_stats);
  get("provide_extender", v.provide_extender);
  get("provide_extender_writable", v.provide_extender_writable);
  get("measurements_enabled", v.measurements_enabled);
  get("measurements_enabled_writable", v.measurements_enabled_writable);
  get("provide_extender_port_mapping", v.provide_extender_port_mapping);
  get("provide_extender_port_mapping_writable", v.provide_extender_port_mapping_writable);
  // The sdk's documents are kept only in the shape they have to have: anything
  // else reads as none, never as a throw that loses the counts above.
  if (auto it = j.find("network_peers"); it != j.end() && it->is_array())
    v.network_peers = *it;
  if (auto it = j.find("provider_points"); it != j.end() && it->is_array())
    v.provider_points = *it;
  if (auto it = j.find("provider_distribution"); it != j.end() && it->is_object())
    v.provider_distribution = *it;
  if (auto it = j.find("extender_points"); it != j.end() && it->is_array())
    v.extender_points = *it;
  if (auto it = j.find("extender_provide_status"); it != j.end() && it->is_object())
    v.extender_provide_status = *it;
}

namespace detail {
// One series of the sdk's points as the reader's type, oldest first; empty for
// none, or for something the type cannot read.
template <typename Point>
std::vector<Point> PointsOf(const nlohmann::json& points) {
  if (!points.is_array()) return {};
  try {
    return points.get<std::vector<Point>>();
  } catch (const std::exception&) {
    return {};
  }
}
}  // namespace detail

// The provider points as the reader's type (urnet::ThroughputPoint in the app),
// oldest first. Empty for a reply that carries none, or carries something the
// type cannot read: the plots then draw an empty window, never a partial one.
template <typename Point>
std::vector<Point> ProviderPointsOf(const ProviderStats& stats) {
  return detail::PointsOf<Point>(stats.provider_points);
}

// The extender points, by the same rule: the extender chart's series.
template <typename Point>
std::vector<Point> ExtenderPointsOf(const ProviderStats& stats) {
  return detail::PointsOf<Point>(stats.extender_points);
}

// The provider extender status as the reader's type
// (urnet::ExtenderProvideStatus); nullopt for none, from an older service too,
// or for one the type cannot read. The app reads nullopt as the role
// unsupported, which hides the extender row and plot (EXTENDER.md N1).
template <typename Status>
std::optional<Status> ExtenderProvideStatusOf(const ProviderStats& stats) {
  if (!stats.extender_provide_status.is_object()) return std::nullopt;
  try {
    return stats.extender_provide_status.get<Status>();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// The provider-only device's connected network peers as the reader's type
// (urnet::NetworkPeerList). nullopt for none, which is what an older service
// sends, or for a list the type cannot read: the app then leaves its peer
// lists as they are. An empty list is a reading: no peers are connected.
template <typename PeerList>
std::optional<PeerList> NetworkPeersOf(const ProviderStats& stats) {
  if (!stats.network_peers.is_array()) return std::nullopt;
  try {
    return stats.network_peers.get<PeerList>();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// The provider transport distribution as the reader's type
// (urnet::TransportDistribution); nullopt for none or an unreadable one.
template <typename Distribution>
std::optional<Distribution> ProviderDistributionOf(const ProviderStats& stats) {
  if (!stats.provider_distribution.is_object()) return std::nullopt;
  try {
    return stats.provider_distribution.get<Distribution>();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// "The service stopped this tunnel BY ITSELF because it could not carry
// traffic." One predicate, in the header both sides already share, so the tray,
// the connect page and the status strip cannot disagree about what counts as a
// failsafe stop — and so a reason string added later is recognised by all three
// without touching any of them.
inline bool IsFailsafeStop(const std::string& stop_reason) {
  return stop_reason.rfind("failsafe_", 0) == 0;
}

inline void to_json(nlohmann::json& j, const Reply& v) {
  j = {{"type", msg::kReply}, {"ok", v.ok}, {"error", v.error},
       {"in_reply_to", v.in_reply_to}};
  if (v.status) j["status"] = *v.status;
  if (v.provider_stats) j["provider_stats"] = *v.provider_stats;
  if (!v.log_upload_carrier.empty()) j["log_upload_carrier"] = v.log_upload_carrier;
  if (v.log_upload_id != 0) j["log_upload_id"] = v.log_upload_id;
  if (v.log_upload_busy) j["log_upload_busy"] = true;
  if (v.reset) j["reset"] = true;
  if (v.reset_busy) j["reset_busy"] = true;
  if (v.ingress_dns_held) j["ingress_dns_held"] = true;
  if (v.ingress_dns_busy) j["ingress_dns_busy"] = true;
}

inline void from_json(const nlohmann::json& j, Reply& v) {
  auto get = [&](const char* k, auto& out) {
    if (auto it = j.find(k); it != j.end() && !it->is_null()) it->get_to(out);
  };
  get("ok", v.ok);
  get("error", v.error);
  get("in_reply_to", v.in_reply_to);
  if (auto it = j.find("status"); it != j.end() && it->is_object()) {
    TunnelStatus s;
    it->get_to(s);
    v.status = s;
  }
  if (auto it = j.find("provider_stats"); it != j.end() && it->is_object()) {
    ProviderStats s;
    it->get_to(s);
    v.provider_stats = std::move(s);
  }
  get("log_upload_carrier", v.log_upload_carrier);
  get("log_upload_id", v.log_upload_id);
  get("log_upload_busy", v.log_upload_busy);
  get("reset", v.reset);
  get("reset_busy", v.reset_busy);
  get("ingress_dns_held", v.ingress_dns_held);
  get("ingress_dns_busy", v.ingress_dns_busy);
}

// Envelope helpers: every message on the wire has a top-level "type" tag.
inline std::string TypeOf(const nlohmann::json& j) {
  if (auto it = j.find("type"); it != j.end() && it->is_string())
    return it->get<std::string>();
  return {};
}

inline nlohmann::json Request(const char* type, nlohmann::json body = nlohmann::json::object()) {
  body["type"] = type;
  return body;
}

// THE ONE WAY A MESSAGE BECOMES BYTES ON THIS PIPE.
//
// nlohmann::json::dump() throws type_error.316 ("invalid UTF-8 byte at index
// N") on a string it cannot encode, and this protocol carries strings this
// process did not author: ControlServer copies the SDK's error text into
// Reply::error verbatim, and the SDK's errors are whatever a remote peer, a
// resolver or a Go library produced. One stray byte in one of those is enough.
//
// error_handler_t::replace, so a bad byte degrades to U+FFFD instead of
// throwing. That is the right trade HERE and the reasoning is worth stating,
// because "never silently corrupt data" would normally point the other way:
//
//   * the field at risk is a human-readable diagnostic, and the app acts on the
//     `ok` flag and the status, not on the exact bytes of `error`. A message
//     with one replacement character in it still says everything the app needs;
//   * the alternative is not "a correct message" but NO message. dump() throws
//     from PipeServer's write path, which is a worker thread whose escape ends
//     in std::terminate — so a single malformed diagnostic string would take
//     down a LocalSystem service holding the machine's default routes;
//   * and the corruption is bounded and visible: U+FFFD in a log line reads as
//     "a byte was not valid UTF-8 here", which is true and is itself the
//     diagnosis.
//
// Callers still wrap this in a try/catch. `replace` closes the invalid-UTF-8
// door, not every door — dump() can still fail on allocation.
inline std::string DumpForWire(const nlohmann::json& j) {
  return j.dump(/*indent=*/-1, /*indent_char=*/' ', /*ensure_ascii=*/false,
                nlohmann::json::error_handler_t::replace);
}

// What to send when even DumpForWire could not produce bytes. Deliberately a
// literal — building it from the data that just failed to serialize is how a
// fallback fails the same way the thing it is replacing did.
inline constexpr const char* kUnserializableReplyJson =
    R"({"type":"reply","ok":false,"error":"the service could not serialize its reply"})";

}  // namespace urnw::proto
