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
// and the device RPC (mTLS WebSocket on loopback) is established separately by
// the SDK once the tunnel is up — this channel only carries lifecycle + config.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
inline constexpr int kProtocolVersion = 2;

// The first version that understands StartTunnel::mode. Below this, an absent
// `mode` on the wire means "ignored", not "defaulted".
inline constexpr int kFirstStartModeVersion = 2;

// ---- message type tags ----------------------------------------------------

namespace msg {
inline constexpr const char* kHello = "hello";                   // app -> service
inline constexpr const char* kStartTunnel = "start_tunnel";      // app -> service
inline constexpr const char* kStopTunnel = "stop_tunnel";        // app -> service
inline constexpr const char* kGetState = "get_state";            // app -> service
inline constexpr const char* kSetSplitTunnel = "set_split_tunnel"; // app -> service
inline constexpr const char* kSetKillSwitch = "set_kill_switch"; // app -> service
inline constexpr const char* kLogout = "logout";                 // app -> service
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
};

inline const char* ToString(TunnelState s) {
  switch (s) {
    case TunnelState::Stopped: return "stopped";
    case TunnelState::Starting: return "starting";
    case TunnelState::Up: return "up";
    case TunnelState::Stopping: return "stopping";
    case TunnelState::Error: return "error";
    case TunnelState::RpcOnly: return "rpc_only";
  }
  return "unknown";
}

inline TunnelState TunnelStateFromString(const std::string& s) {
  if (s == "starting") return TunnelState::Starting;
  if (s == "up") return TunnelState::Up;
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
  return s == TunnelState::Up || s == TunnelState::RpcOnly;
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
};

struct SetSplitTunnel {
  std::vector<std::string> excluded_app_paths;  // meaning depends on allowlist_mode (see StartTunnel)
  bool allowlist_mode = false;
};

struct SetKillSwitch {
  bool on = false;
};

// ---- reply / state payload ------------------------------------------------

struct TunnelStatus {
  TunnelState state = TunnelState::Stopped;
  std::string rpc_listen_hostport;  // echoed so the app can dial the DeviceRemote
  std::string error;                // set when state == Error
  std::string service_version;
  int protocol_version = kProtocolVersion;
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
  // True only when routes and DNS are actually installed right now. This is the
  // field to trust for "is my traffic going through the tunnel"; it is false for
  // the whole life of an rpc-only session.
  bool routes_installed = false;
  // True only when the tunnel's resolvers were actually accepted by the stack.
  //
  // routes_installed and dns_applied are separate facts and used to be
  // conflated. Applying the network settings deliberately SUCCEEDS when the DNS
  // half fails — tearing a working tunnel down over its resolvers trades a DNS
  // problem for a connectivity one — so before this field a DNS failure left
  // state=up, routes_installed=true and one warning in the service log, while
  // every surface said Connected and every query went out in the clear.
  //
  // Defaults FALSE, including for a peer too old to send it. That is the safe
  // direction: an unknown DNS state renders as degraded, not as clean.
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
  // "failsafe_no_inbound" | "failsafe_sdk_unresponsive" (Service/TunnelWatchdog.h
  // owns the failsafe spellings; they are literals there, not built by hand).
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
  // (Service/TunnelWatchdog.h, kFailsafeNoticeMillis). The UI warns on this, so
  // an automatic teardown is never a surprise. Defaults false — a peer that
  // cannot say simply never warns, which is today's behaviour.
  bool failsafe_armed = false;
};

struct Reply {
  bool ok = false;
  std::string error;             // set when !ok
  std::optional<TunnelStatus> status;
  std::string in_reply_to;       // request type tag this answers
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
      {"excluded_app_paths", v.excluded_app_paths},
      {"allowlist_mode", v.allowlist_mode},
      {"mode", ToString(v.mode)},
      {"kill_switch", v.kill_switch},
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
  get("excluded_app_paths", v.excluded_app_paths);
  get("allowlist_mode", v.allowlist_mode);
  get("kill_switch", v.kill_switch);
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

inline void to_json(nlohmann::json& j, const SetKillSwitch& v) {
  j = {{"on", v.on}};
}
inline void from_json(const nlohmann::json& j, SetKillSwitch& v) {
  if (auto it = j.find("on"); it != j.end() && !it->is_null()) it->get_to(v.on);
}

inline void to_json(nlohmann::json& j, const TunnelStatus& v) {
  j = {
      {"state", ToString(v.state)},
      {"rpc_listen_hostport", v.rpc_listen_hostport},
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
