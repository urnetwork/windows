// Executable spec for the provider-only device on the control pipe
// (Common/Protocol.h): the start_provider and stop_provider tags, the
// StartProvider request on the wire, the TunnelStatus provider_* fields and
// their reading from a service too old to send them, the request comparison
// that keeps a running device (SameProviderDevice), and what one status tells
// the app's reconcile (ProviderFactsFrom). And get_provider_stats: the verb,
// the ProviderStats payload in a reply and an older service's silence, its
// extender fields and a service too old to send them, and the readers that
// turn its sdk documents back into the app's types (ProviderPointsOf,
// ProviderDistributionOf, ExtenderPointsOf, ExtenderProvideStatusOf,
// NetworkPeersOf for the device's connected network peers). And the
// Extender switch while disconnected: set_provide_extender, its strict
// request, and provide_extender_writable with an older service's silence. And
// the lag.dev speed tests switch the same way: set_measurements_enabled, its
// strict request, and measurements_enabled with measurements_enabled_writable
// and an older service's silence. And the router port mapping switch the same
// way, off by default: set_provide_extender_port_mapping, its strict request,
// and provide_extender_port_mapping with provide_extender_port_mapping_writable
// and an older service's silence. Run
// against the same header the service and the app compile; it needs
// nlohmann/json, like the app.
//
//   c++ -std=c++20 -I ../src/Common -I <dir with nlohmann/json.hpp> provide-protocol-tests.cpp -o /tmp/provide-protocol-tests && /tmp/provide-protocol-tests
//
// With URNW_PROVIDE_PROTOCOL_TESTS_SDK the readers run on the generated
// header's own urnet::ThroughputPoint, urnet::TransportDistribution and
// urnet::ExtenderProvideStatus, written the way the service writes them (their
// to_json), so the wire is proved to carry the sdk's documents back unchanged.
// The header is a system include, like nlohmann/json: the generated code does
// not build with -Wextra -Werror.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Protocol.h"

#if defined(URNW_PROVIDE_PROTOCOL_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

using namespace urnw;
using namespace urnw::proto;

#if !defined(URNW_PROVIDE_PROTOCOL_TESTS_SDK)
// The fields of the sdk's documents the readers are checked on, by the
// generated wrapper's names, with json conversions of the same shape.
namespace sample {
struct ThroughputSample {
  int64_t EgressByteCount = 0;
  int64_t IngressByteCount = 0;
};
struct ThroughputPoint {
  int64_t Time = 0;
  // the extender chart's route
  std::optional<ThroughputSample> Remote;
  std::optional<ThroughputSample> Local;
};
struct TransportDistribution {
  int64_t ByteCount = 0;
  bool Active = false;
};
inline void to_json(nlohmann::json& j, const ThroughputSample& v) {
  j = {{"EgressByteCount", v.EgressByteCount}, {"IngressByteCount", v.IngressByteCount}};
}
inline void from_json(const nlohmann::json& j, ThroughputSample& v) {
  j.at("EgressByteCount").get_to(v.EgressByteCount);
  j.at("IngressByteCount").get_to(v.IngressByteCount);
}
inline void to_json(nlohmann::json& j, const ThroughputPoint& v) {
  j = {{"Time", v.Time}};
  if (v.Remote) j["Remote"] = *v.Remote;
  if (v.Local) j["Local"] = *v.Local;
}
inline void from_json(const nlohmann::json& j, ThroughputPoint& v) {
  j.at("Time").get_to(v.Time);
  if (auto it = j.find("Remote"); it != j.end() && !it->is_null()) v.Remote = it->get<ThroughputSample>();
  if (auto it = j.find("Local"); it != j.end() && !it->is_null()) v.Local = it->get<ThroughputSample>();
}
inline void to_json(nlohmann::json& j, const TransportDistribution& v) {
  j = {{"ByteCount", v.ByteCount}, {"Active", v.Active}};
}
inline void from_json(const nlohmann::json& j, TransportDistribution& v) {
  j.at("ByteCount").get_to(v.ByteCount);
  j.at("Active").get_to(v.Active);
}
// the fields of urnet::ExtenderProvideStatus the app's view reads
struct ExtenderProvideStatus {
  bool Supported = false;
  std::string State;
  std::string Reason;
  bool Enabled = false;
  bool ActivatedV4 = false;
};
inline void to_json(nlohmann::json& j, const ExtenderProvideStatus& v) {
  j = {{"Supported", v.Supported},
       {"State", v.State},
       {"Reason", v.Reason},
       {"Enabled", v.Enabled},
       {"ActivatedV4", v.ActivatedV4}};
}
inline void from_json(const nlohmann::json& j, ExtenderProvideStatus& v) {
  j.at("Supported").get_to(v.Supported);
  j.at("State").get_to(v.State);
  j.at("Reason").get_to(v.Reason);
  j.at("Enabled").get_to(v.Enabled);
  j.at("ActivatedV4").get_to(v.ActivatedV4);
}
// the fields of urnet::NetworkPeer the peer rows read, every one optional on
// the wire as in the generated wrapper's from_json
struct NetworkPeer {
  std::optional<std::string> ClientId;
  bool ProvideEnabled = false;
  std::string Principal;
  std::string DeviceSpec;
  std::string DeviceName;
};
inline void to_json(nlohmann::json& j, const NetworkPeer& v) {
  j = {{"ProvideEnabled", v.ProvideEnabled},
       {"Principal", v.Principal},
       {"DeviceSpec", v.DeviceSpec},
       {"DeviceName", v.DeviceName}};
  if (v.ClientId) j["ClientId"] = *v.ClientId;
}
inline void from_json(const nlohmann::json& j, NetworkPeer& v) {
  if (!j.is_object()) return;
  if (auto it = j.find("ClientId"); it != j.end() && !it->is_null()) v.ClientId = it->get<std::string>();
  if (auto it = j.find("ProvideEnabled"); it != j.end() && !it->is_null()) it->get_to(v.ProvideEnabled);
  if (auto it = j.find("Principal"); it != j.end() && !it->is_null()) it->get_to(v.Principal);
  if (auto it = j.find("DeviceSpec"); it != j.end() && !it->is_null()) it->get_to(v.DeviceSpec);
  if (auto it = j.find("DeviceName"); it != j.end() && !it->is_null()) it->get_to(v.DeviceName);
}
}  // namespace sample
namespace doc = sample;
#else
namespace doc = urnet;
#endif

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

StartProvider SampleRequest() {
  StartProvider r;
  r.by_jwt = "client.jwt.value";
  r.network_space_json = R"({"key":{"host_name":"network.example","env_name":"main"}})";
  r.instance_id = "0193e520-0000-7000-8000-000000000001";
  r.device_description = "DESKTOP-1";
  r.device_spec = "windows amd64";
  r.app_version = "2026.10.5-1";
  r.provide_mode = "always";
  r.provider_transport_settings_json = R"({"mode":"auto"})";
  return r;
}

// (a) the verbs
void TestTags() {
  Check(std::string(msg::kStartProvider) == "start_provider", "tags: start_provider");
  Check(std::string(msg::kStopProvider) == "stop_provider", "tags: stop_provider");
  const nlohmann::json request = Request(msg::kStartProvider, SampleRequest());
  Check(TypeOf(request) == "start_provider", "tags: the request carries its type");
}

// (b) the request survives the wire, and an absent field is empty, not a throw
void TestStartProviderJson() {
  const StartProvider sent = SampleRequest();
  const nlohmann::json wire = nlohmann::json::parse(DumpForWire(nlohmann::json(sent)));
  const StartProvider got = wire.get<StartProvider>();
  Check(got.by_jwt == sent.by_jwt, "request: by_jwt");
  Check(got.network_space_json == sent.network_space_json, "request: network_space_json");
  Check(got.instance_id == sent.instance_id, "request: instance_id");
  Check(got.device_description == sent.device_description, "request: device_description");
  Check(got.device_spec == sent.device_spec, "request: device_spec");
  Check(got.app_version == sent.app_version, "request: app_version");
  Check(got.provide_mode == sent.provide_mode, "request: provide_mode");
  Check(got.provider_transport_settings_json == sent.provider_transport_settings_json,
        "request: provider_transport_settings_json");
  // no RPC material: the provider-only device has no listener to pin
  for (const char* rpc : {"rpc_server_pem", "rpc_client_cert_pem", "rpc_listen_hostport",
                          "rpc_session_id"}) {
    Check(!wire.contains(rpc), std::string("request: carries no ") + rpc);
  }
  const StartProvider sparse =
      nlohmann::json::parse(R"({"provide_mode":"auto","by_jwt":null})").get<StartProvider>();
  Check(sparse.provide_mode == "auto" && sparse.by_jwt.empty() &&
            sparse.provider_transport_settings_json.empty(),
        "request: absent and null fields read as empty");
}

// (c) the status fields, and a service too old to send them
void TestStatusFields() {
  TunnelStatus status;
  status.provider_running = true;
  status.provider_control_mode = "auto";
  status.provider_mode = 1;
  status.provider_network_key = true;
  const nlohmann::json wire = nlohmann::json::parse(DumpForWire(nlohmann::json(status)));
  for (const char* key : {"provider_running", "provider_control_mode", "provider_mode",
                          "provider_network_key"}) {
    Check(wire.contains(key), std::string("status: writes ") + key);
  }
  const TunnelStatus got = wire.get<TunnelStatus>();
  Check(got.provider_running, "status: provider_running round-trips");
  Check(got.provider_control_mode == "auto", "status: provider_control_mode round-trips");
  Check(got.provider_mode == 1, "status: provider_mode round-trips");
  Check(got.provider_network_key, "status: provider_network_key round-trips");

  // an older service: no provider fields at all reads as "no provider-only
  // device", which claims less than the truth
  const TunnelStatus old = nlohmann::json::parse(
                               R"({"state":"stopped","wfp_state":"off","protocol_version":4})")
                               .get<TunnelStatus>();
  Check(!old.provider_running && old.provider_control_mode.empty() && old.provider_mode == 0 &&
            !old.provider_network_key,
        "status: an older service reads as no provider-only device");
  Check(kProtocolVersion == 4, "status: no protocol bump for the provider verbs and fields");

  // a refused start_provider still carries the status beside its reason
  Reply refused;
  refused.ok = false;
  refused.error = provide::RefusalReason(provide::ProviderRefusal::KillSwitchArmed);
  refused.in_reply_to = msg::kStartProvider;
  TunnelStatus armed;
  armed.wfp_state = "armed";
  refused.status = armed;
  const Reply back = nlohmann::json::parse(DumpForWire(nlohmann::json(refused))).get<Reply>();
  Check(!back.ok && back.error == refused.error && back.status &&
            back.status->wfp_state == "armed" && !back.status->provider_running,
        "status: a refusal carries its reason and the status");
}

// (d) the comparison that keeps a running device
void TestSameProviderDevice() {
  const StartProvider base = SampleRequest();
  StartProvider remoded = base;
  remoded.provide_mode = "network";
  Check(SameProviderDevice(base, remoded), "same device: only the mode differs");
  Check(SameProviderDevice(base, base), "same device: identical");
  // the provider transport policy is applied to the running device in place,
  // as the build applies it right after construction
  StartProvider repoliced = base;
  repoliced.provider_transport_settings_json = "";
  Check(SameProviderDevice(base, repoliced),
        "same device: only the provider transport policy differs");
  // a tunnel start carries the same identity fields
  StartTunnel start;
  start.by_jwt = base.by_jwt;
  start.network_space_json = base.network_space_json;
  start.instance_id = base.instance_id;
  start.device_description = base.device_description;
  start.device_spec = base.device_spec;
  start.app_version = base.app_version;
  Check(urnw::provide::SameIdentity(IdentityOf(base), IdentityOf(start)),
        "same device: a tunnel start with the provider's identity is the same device");
  start.instance_id = "0193e520-0000-7000-8000-000000000002";
  Check(!urnw::provide::SameIdentity(IdentityOf(base), IdentityOf(start)),
        "same device: a tunnel start with another identity is another device");

  struct Change {
    const char* field;
    void (*apply)(StartProvider&);
  };
  const Change changes[] = {
      {"by_jwt", [](StartProvider& r) { r.by_jwt = "another.jwt.value"; }},
      {"network_space_json", [](StartProvider& r) { r.network_space_json = "{}"; }},
      {"instance_id",
       [](StartProvider& r) { r.instance_id = "0193e520-0000-7000-8000-000000000002"; }},
      {"device_description", [](StartProvider& r) { r.device_description = "DESKTOP-2"; }},
      {"device_spec", [](StartProvider& r) { r.device_spec = "windows arm64"; }},
      {"app_version", [](StartProvider& r) { r.app_version = "2026.10.6-1"; }},
  };
  for (const Change& change : changes) {
    StartProvider other = base;
    change.apply(other);
    Check(!SameProviderDevice(base, other),
          std::string("same device: a different ") + change.field + " builds a new device");
  }
}

// A jwt-shaped token: base64url header and claims, then an opaque signature.
// The subject reads the claims only, so the signature never has to verify.
std::string Base64Url(std::string_view in) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (const unsigned char c : in) {
    buffer = (buffer << 8) | c;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out.push_back(kAlphabet[(buffer >> bits) & 0x3Fu]);
    }
  }
  if (bits > 0) out.push_back(kAlphabet[(buffer << (6 - bits)) & 0x3Fu]);
  return out;
}

std::string Jwt(const std::string& claims, const std::string& signature) {
  return Base64Url(R"({"alg":"ES256","typ":"JWT"})") + "." + Base64Url(claims) + "." +
         signature;
}

// (d2) the credential counts by the client it names. The app refreshes its
// token on every connect, in its own process, and each device refreshes its
// own as it starts: compared token by token, every disconnect that kept the
// session's device rebuilt it 25ms later (measured).
void TestSameDeviceAcrossJwtRefresh() {
  const std::string network = "0193e520-0000-7000-8000-0000000000aa";
  const std::string client = "0193e520-0000-7000-8000-0000000000bb";
  const auto claims = [](const std::string& networkId, const std::string& clientId,
                         const std::string& created) {
    return R"({"network_id":")" + networkId +
           R"(","network_name":"example","user_id":"0193e520-0000-7000-8000-0000000000cc",)" +
           R"("client_id":")" + clientId + R"(","create_time":")" + created + R"("})";
  };
  StartProvider kept = SampleRequest();
  kept.by_jwt = Jwt(claims(network, client, "2026-11-03T09:00:00Z"), "signature1");
  StartProvider refreshed = kept;
  refreshed.by_jwt = Jwt(claims(network, client, "2026-11-03T19:33:57Z"), "signature2");
  Check(SameProviderDevice(kept, refreshed), "same device: a refreshed jwt for the same client");
  StartTunnel start;
  start.by_jwt = refreshed.by_jwt;
  start.network_space_json = kept.network_space_json;
  start.instance_id = kept.instance_id;
  start.device_description = kept.device_description;
  start.device_spec = kept.device_spec;
  start.app_version = kept.app_version;
  Check(urnw::provide::SameIdentity(IdentityOf(kept), IdentityOf(start)),
        "same device: a connect with a refreshed jwt for the same client keeps the device");

  StartProvider otherClient = kept;
  otherClient.by_jwt =
      Jwt(claims(network, "0193e520-0000-7000-8000-0000000000dd", "2026-11-03T19:33:57Z"),
          "signature2");
  Check(!SameProviderDevice(kept, otherClient),
        "same device: a jwt for another client builds a new device");
  StartProvider otherNetwork = kept;
  otherNetwork.by_jwt =
      Jwt(claims("0193e520-0000-7000-8000-0000000000ee", client, "2026-11-03T19:33:57Z"),
          "signature2");
  Check(!SameProviderDevice(kept, otherNetwork),
        "same device: a jwt for another network's client builds a new device");

  // a token that names no client is compared whole, as every token was
  StartProvider networkOnly = kept;
  networkOnly.by_jwt = Jwt(R"({"network_id":")" + network + R"("})", "signature1");
  StartProvider networkOnlyRefreshed = networkOnly;
  networkOnlyRefreshed.by_jwt = Jwt(R"({"network_id":")" + network + R"("})", "signature2");
  Check(SameProviderDevice(networkOnly, networkOnly), "same device: a network jwt is its own");
  Check(!SameProviderDevice(networkOnly, networkOnlyRefreshed),
        "same device: a jwt that names no client is compared whole");

  Check(JwtSubject(kept.by_jwt) == "client:" + network + "/" + client,
        "jwt subject: the network and the client the claims name");
  Check(JwtSubject(kept.by_jwt).find("signature1") == std::string::npos,
        "jwt subject: never the token itself");
  Check(JwtSubject("client.jwt.value") == "client.jwt.value",
        "jwt subject: a token that is not a jwt is its own subject");
  Check(JwtSubject("a.%%%.c") == "a.%%%.c",
        "jwt subject: claims that are not base64url leave the token its own subject");
  Check(JwtSubject(Jwt(R"({"client_id":7})", "s")) == Jwt(R"({"client_id":7})", "s"),
        "jwt subject: a client_id that is not a string leaves the token its own subject");
}

// (e) what one status tells the app's reconcile
void TestProviderFacts() {
  TunnelStatus idle;
  idle.state = TunnelState::Stopped;
  idle.wfp_state = "off";
  const provide::ServiceProviderFacts answered = ProviderFactsFrom(idle, true);
  Check(answered.answered && !answered.tunnelSession && !answered.killSwitchArmed &&
            !answered.providerRunning,
        "facts: an idle service");
  Check(!ProviderFactsFrom(idle, false).answered, "facts: answered is the transport's word");

  for (TunnelState state : {TunnelState::Starting, TunnelState::Preparing, TunnelState::Up,
                            TunnelState::RpcOnly, TunnelState::Stopping}) {
    TunnelStatus session = idle;
    session.state = state;
    Check(ProviderFactsFrom(session, true).tunnelSession,
          std::string("facts: ") + ToString(state) + " is a tunnel session");
  }
  for (TunnelState state : {TunnelState::Stopped, TunnelState::Error}) {
    TunnelStatus none = idle;
    none.state = state;
    Check(!ProviderFactsFrom(none, true).tunnelSession,
          std::string("facts: ") + ToString(state) + " is no tunnel session");
  }

  for (const char* wfp : {"armed", "connecting", "connected"}) {
    TunnelStatus policy = idle;
    policy.wfp_state = wfp;
    Check(ProviderFactsFrom(policy, true).killSwitchArmed,
          std::string("facts: wfp ") + wfp + " is a policy in force");
  }
  for (const char* wfp : {"off", ""}) {
    TunnelStatus open = idle;
    open.wfp_state = wfp;
    Check(!ProviderFactsFrom(open, true).killSwitchArmed,
          std::string("facts: wfp '") + wfp + "' is no policy");
  }

  TunnelStatus providing = idle;
  providing.provider_running = true;
  providing.provider_mode = 3;
  Check(ProviderFactsFrom(providing, true).providerRunning, "facts: the provider runs");
  // and the whole chain: an Always user who just disconnected starts providing
  Check(provide::DisconnectedProviderStep("always", ProviderFactsFrom(idle, true)) ==
            provide::DisconnectedStep::Start,
        "facts: Always after a Disconnect starts the provider-only device");
  TunnelStatus armedAfterDrop = idle;
  armedAfterDrop.wfp_state = "armed";
  Check(provide::DisconnectedProviderStep("always", ProviderFactsFrom(armedAfterDrop, true)) ==
            provide::DisconnectedStep::None,
        "facts: nothing starts under the armed floor");
}

// (f) get_provider_stats: the verb, the payload and an older service's silence
void TestProviderStatsReply() {
  Check(std::string(msg::kGetProviderStats) == "get_provider_stats", "stats: the tag");
  Check(TypeOf(Request(msg::kGetProviderStats)) == "get_provider_stats",
        "stats: the request carries its type");

  ProviderStats sent;
  sent.available = true;
  sent.client_id = "018f2b1e-0000-7000-8000-00000000c0de";
  sent.client_count = 3;
  sent.window_seconds = 60;
  sent.has_provider_stats = true;
  sent.provider_points = nlohmann::json::parse(R"([{"Time":1000,"Local":{"EgressByteCount":5,"IngressByteCount":7}}])");
  sent.provider_distribution = nlohmann::json::parse(R"({"ByteCount":12,"Active":true})");
  Reply reply;
  reply.ok = true;
  reply.in_reply_to = msg::kGetProviderStats;
  reply.provider_stats = sent;
  const Reply back = nlohmann::json::parse(DumpForWire(nlohmann::json(reply))).get<Reply>();
  Check(back.ok && back.provider_stats.has_value(), "reply: provider_stats round-trips");
  if (back.provider_stats) {
    const ProviderStats& got = *back.provider_stats;
    Check(got.available, "provider stats: available round-trips");
    Check(got.client_id == sent.client_id, "provider stats: client_id round-trips");
    Check(got.client_count == 3, "provider stats: client_count round-trips");
    Check(got.window_seconds == 60, "provider stats: window_seconds round-trips");
    Check(got.has_provider_stats, "provider stats: has_provider_stats round-trips");
    Check(got.provider_points == sent.provider_points, "provider stats: the points arrive unchanged");
    Check(got.provider_distribution == sent.provider_distribution,
          "provider stats: the distribution arrives unchanged");
  }
  Check(!back.status, "reply: get_provider_stats carries no tunnel status");

  // an older service: "unknown request type", and nothing that reads as stats
  const Reply old = nlohmann::json::parse(
      R"({"type":"reply","ok":false,"error":"unknown request type: get_provider_stats","in_reply_to":"get_provider_stats"})")
                        .get<Reply>();
  Check(!old.ok && !old.provider_stats, "stats: an older service's reply carries no statistics");
  // nothing runs: unavailable, and every field empty
  const ProviderStats none = nlohmann::json::parse(DumpForWire(nlohmann::json(ProviderStats{}))).get<ProviderStats>();
  Check(!none.available && none.client_id.empty() && none.client_count == 0 &&
            !none.has_provider_stats && none.provider_points.empty() &&
            none.provider_distribution.is_null(),
        "stats: no provider-only device reads as unavailable and empty");
  // a document of the wrong shape reads as none, never as a throw that loses the rest
  const ProviderStats odd = nlohmann::json::parse(
      R"({"available":true,"client_count":2,"provider_points":{"Time":1},"provider_distribution":[1,2]})")
                                .get<ProviderStats>();
  Check(odd.available && odd.client_count == 2 && odd.provider_points.empty() &&
            odd.provider_distribution.is_null(),
        "stats: wrong-shaped sdk documents read as none");
}

// (g) the readers, on the sdk's own types when built against the header: the
// service writes them with to_json, the app reads them back with from_json
void TestProviderStatsReaders() {
  std::vector<doc::ThroughputPoint> points(2);
  points[0].Time = 1000;
  points[1].Time = 2000;
  doc::ThroughputSample local{};
  local.EgressByteCount = 5;
  local.IngressByteCount = 7;
  points[1].Local = local;
  doc::TransportDistribution distribution{};
  distribution.ByteCount = 12;
  distribution.Active = true;

  ProviderStats stats;
  stats.available = true;
  stats.provider_points = points;              // the service's write
  stats.provider_distribution = distribution;
  const ProviderStats wire = nlohmann::json::parse(DumpForWire(nlohmann::json(stats))).get<ProviderStats>();

  const std::vector<doc::ThroughputPoint> read = ProviderPointsOf<doc::ThroughputPoint>(wire);
  Check(read.size() == 2 && read[0].Time == 1000 && read[1].Time == 2000,
        "readers: the points come back, oldest first");
  Check(read.size() == 2 && !read[0].Local && read[1].Local &&
            read[1].Local->EgressByteCount == 5 && read[1].Local->IngressByteCount == 7,
        "readers: a point's samples come back");
  const std::optional<doc::TransportDistribution> back =
      ProviderDistributionOf<doc::TransportDistribution>(wire);
  Check(back && back->ByteCount == 12 && back->Active, "readers: the distribution comes back");

  ProviderStats empty;
  Check(ProviderPointsOf<doc::ThroughputPoint>(empty).empty(), "readers: no points read as none");
  Check(!ProviderDistributionOf<doc::TransportDistribution>(empty),
        "readers: no distribution reads as none");
  ProviderStats unreadable;
  unreadable.provider_points = nlohmann::json::parse(R"([{"Time":"soon"}])");
  unreadable.provider_distribution = nlohmann::json::parse(R"({"ByteCount":"many"})");
  Check(ProviderPointsOf<doc::ThroughputPoint>(unreadable).empty(),
        "readers: points the type cannot read are none, never a partial window");
  Check(!ProviderDistributionOf<doc::TransportDistribution>(unreadable),
        "readers: a distribution the type cannot read is none");
}

// (g2) the provider-only device's network peers: the list the app shows while
// disconnected, read back on the sdk's own NetworkPeer when built against the
// header, with the count from the same reading; what an older service sends,
// and shapes that are not a list of peers
void TestProviderStatsPeers() {
  std::vector<doc::NetworkPeer> connected(2);
  connected[0].ClientId = std::string("018f2b1e-0000-7000-8000-0000000000a1");
  connected[0].ProvideEnabled = true;
  connected[0].DeviceName = "vps provider";
  connected[0].DeviceSpec = "linux amd64";
  connected[1].ClientId = std::string("018f2b1e-0000-7000-8000-0000000000b2");
  connected[1].ProvideEnabled = false;
  connected[1].DeviceName = "office laptop";
  connected[1].DeviceSpec = "windows amd64";

  ProviderStats stats;
  stats.available = true;
  stats.client_count = static_cast<int64_t>(connected.size());
  stats.network_peers = connected;  // the service's write
  const ProviderStats wire = nlohmann::json::parse(DumpForWire(nlohmann::json(stats))).get<ProviderStats>();
  Check(wire.client_count == 2, "peers: the count comes with the list");
  const std::optional<std::vector<doc::NetworkPeer>> read =
      NetworkPeersOf<std::vector<doc::NetworkPeer>>(wire);
  Check(read && read->size() == 2, "peers: the connected peers come back");
  if (read && read->size() == 2) {
    Check((*read)[0].ClientId == connected[0].ClientId && (*read)[0].ProvideEnabled &&
              (*read)[0].DeviceName == "vps provider" && (*read)[0].DeviceSpec == "linux amd64",
          "peers: a providing peer comes back unchanged, in order");
    Check((*read)[1].ClientId == connected[1].ClientId && !(*read)[1].ProvideEnabled &&
              (*read)[1].DeviceName == "office laptop",
          "peers: a peer that does not provide comes back as such");
  }

  // no peers connected is a reading, not an absence
  ProviderStats none;
  none.available = true;
  none.network_peers = std::vector<doc::NetworkPeer>{};
  const ProviderStats noneWire = nlohmann::json::parse(DumpForWire(nlohmann::json(none))).get<ProviderStats>();
  const auto noneRead = NetworkPeersOf<std::vector<doc::NetworkPeer>>(noneWire);
  Check(noneRead && noneRead->empty(), "peers: an empty list reads as no peers connected");

  // an older service sends the count alone, and the app has no list to show
  const ProviderStats older = nlohmann::json::parse(
      R"({"available":true,"client_id":"c","client_count":3,"window_seconds":60,"has_provider_stats":true,"provider_points":[],"provider_distribution":null})")
                                  .get<ProviderStats>();
  Check(older.client_count == 3 && older.network_peers.is_null(),
        "peers: an older service's answer carries the count and no list");
  Check(!NetworkPeersOf<std::vector<doc::NetworkPeer>>(older),
        "peers: no list reads as none, not as no peers");
  // nothing runs: no list
  const ProviderStats idle = nlohmann::json::parse(DumpForWire(nlohmann::json(ProviderStats{}))).get<ProviderStats>();
  Check(!NetworkPeersOf<std::vector<doc::NetworkPeer>>(idle),
        "peers: no provider-only device has no list");
  // a shape that is not a list of peers is none, never a throw that loses the rest
  const ProviderStats odd = nlohmann::json::parse(
      R"({"available":true,"client_count":2,"network_peers":{"ClientId":"x"}})")
                                .get<ProviderStats>();
  Check(odd.available && odd.client_count == 2 && odd.network_peers.is_null() &&
            !NetworkPeersOf<std::vector<doc::NetworkPeer>>(odd),
        "peers: a list of the wrong shape reads as none");
  ProviderStats unreadable;
  unreadable.network_peers = nlohmann::json::parse(R"([{"ClientId":"x","ProvideEnabled":"yes"}])");
  Check(!NetworkPeersOf<std::vector<doc::NetworkPeer>>(unreadable),
        "peers: peers the type cannot read are none, never a partial list");
}

// (h) the extender role on the wire: its series, its status and the setting,
// what a service too old to send them answers, and shapes that are not theirs
void TestProviderStatsExtender() {
  ProviderStats sent;
  sent.available = true;
  sent.extender_points = nlohmann::json::parse(
      R"([{"Time":3000,"Remote":{"EgressByteCount":11,"IngressByteCount":13}}])");
  sent.extender_provide_status = nlohmann::json::parse(
      R"({"Supported":true,"State":"active","Reason":"","Enabled":true,"ActivatedV4":true})");
  sent.provide_extender = true;
  Reply reply;
  reply.ok = true;
  reply.in_reply_to = msg::kGetProviderStats;
  reply.provider_stats = sent;
  const Reply back = nlohmann::json::parse(DumpForWire(nlohmann::json(reply))).get<Reply>();
  Check(back.ok && back.provider_stats.has_value(), "extender: the reply carries the statistics");
  if (back.provider_stats) {
    const ProviderStats& got = *back.provider_stats;
    Check(got.extender_points == sent.extender_points,
          "provider stats: the extender points arrive unchanged");
    Check(got.extender_provide_status == sent.extender_provide_status,
          "provider stats: extender_provide_status round-trips");
    Check(got.provide_extender, "provider stats: provide_extender round-trips");
    Check(got.provider_points.empty(), "provider stats: the extender series is not the provider's");
  }

  // a service that knows get_provider_stats but not the extender fields sends
  // exactly this: no status, no series and the setting off, which the app
  // reads as the role unsupported (both extender surfaces hidden, as before)
  const ProviderStats older = nlohmann::json::parse(
      R"({"available":true,"client_id":"c","client_count":1,"window_seconds":60,"has_provider_stats":true,"provider_points":[],"provider_distribution":null})")
                                  .get<ProviderStats>();
  Check(older.available && older.client_count == 1 && older.extender_provide_status.is_null() &&
            older.extender_points.empty() && !older.provide_extender,
        "extender: a service without the extender fields reads as no status and no series");
  Check(!ExtenderProvideStatusOf<doc::ExtenderProvideStatus>(older),
        "extender: an older service's answer has no status for the app to read");
  // nothing runs: no role to report
  const ProviderStats none =
      nlohmann::json::parse(DumpForWire(nlohmann::json(ProviderStats{}))).get<ProviderStats>();
  Check(none.extender_provide_status.is_null() && none.extender_points.empty() &&
            !none.provide_extender,
        "extender: no provider-only device reports no role");
  // a document of the wrong shape reads as none, never as a throw that loses the rest
  const ProviderStats odd = nlohmann::json::parse(
      R"({"available":true,"client_count":2,"provide_extender":true,"extender_points":{"Time":1},"extender_provide_status":[1,2]})")
                                .get<ProviderStats>();
  Check(odd.available && odd.client_count == 2 && odd.provide_extender &&
            odd.extender_points.empty() && odd.extender_provide_status.is_null(),
        "extender: wrong-shaped sdk documents read as none");
}

// (i) the extender readers, on the sdk's own types when built against the
// header: the service writes them with to_json, the app reads them back with
// from_json
void TestExtenderReaders() {
  std::vector<doc::ThroughputPoint> providerPoints(1);
  providerPoints[0].Time = 1000;
  std::vector<doc::ThroughputPoint> extenderPoints(2);
  extenderPoints[0].Time = 2000;
  extenderPoints[1].Time = 3000;
  doc::ThroughputSample relayed{};
  relayed.EgressByteCount = 11;
  relayed.IngressByteCount = 13;
  extenderPoints[1].Remote = relayed;
  doc::ExtenderProvideStatus status{};
  status.Supported = true;
  status.State = "active";
  status.Reason = "dial tcp6 [2001:db8::1]:443: i/o timeout";
  status.Enabled = true;
  status.ActivatedV4 = true;

  ProviderStats stats;
  stats.available = true;
  stats.provider_points = providerPoints;  // the service's writes
  stats.extender_points = extenderPoints;
  stats.extender_provide_status = status;
  stats.provide_extender = true;
  const ProviderStats wire = nlohmann::json::parse(DumpForWire(nlohmann::json(stats))).get<ProviderStats>();

  const std::vector<doc::ThroughputPoint> read = ExtenderPointsOf<doc::ThroughputPoint>(wire);
  Check(read.size() == 2 && read[0].Time == 2000 && read[1].Time == 3000,
        "readers: the extender points come back, oldest first");
  Check(read.size() == 2 && !read[0].Remote && read[1].Remote &&
            read[1].Remote->EgressByteCount == 11 && read[1].Remote->IngressByteCount == 13,
        "readers: an extender point's samples come back");
  const std::vector<doc::ThroughputPoint> provider = ProviderPointsOf<doc::ThroughputPoint>(wire);
  Check(provider.size() == 1 && provider[0].Time == 1000,
        "readers: the provider series stays its own beside the extender's");
  const std::optional<doc::ExtenderProvideStatus> back =
      ExtenderProvideStatusOf<doc::ExtenderProvideStatus>(wire);
  Check(back && back->Supported && back->State == "active" && back->Enabled &&
            back->ActivatedV4 && back->Reason == status.Reason,
        "readers: the extender status comes back");
  Check(wire.provide_extender, "readers: the setting comes back beside it");

  ProviderStats empty;
  Check(ExtenderPointsOf<doc::ThroughputPoint>(empty).empty(),
        "readers: no extender points read as none");
  Check(!ExtenderProvideStatusOf<doc::ExtenderProvideStatus>(empty),
        "readers: no extender status reads as none");
  ProviderStats unreadable;
  unreadable.extender_points = nlohmann::json::parse(R"([{"Time":"soon"}])");
  unreadable.extender_provide_status = nlohmann::json::parse(R"({"Supported":"yes"})");
  Check(ExtenderPointsOf<doc::ThroughputPoint>(unreadable).empty(),
        "readers: extender points the type cannot read are none, never a partial window");
  Check(!ExtenderProvideStatusOf<doc::ExtenderProvideStatus>(unreadable),
        "readers: an extender status the type cannot read is none");
}

// (j) the Extender switch while disconnected: the set_provide_extender verb and
// its request, and the provide_extender_writable field that tells the app the
// service takes it, with an older service's silence read as no writer
void TestSetProvideExtender() {
  Check(std::string(msg::kSetProvideExtender) == "set_provide_extender",
        "switch: the set_provide_extender tag");
  for (const bool on : {false, true}) {
    SetProvideExtender sent;
    sent.provide_extender = on;
    const nlohmann::json request = Request(msg::kSetProvideExtender, nlohmann::json(sent));
    const nlohmann::json wire = nlohmann::json::parse(DumpForWire(request));
    Check(TypeOf(wire) == "set_provide_extender", "switch: the request carries its type");
    Check(wire.get<SetProvideExtender>().provide_extender == on,
          std::string("switch: provide_extender round-trips ") + (on ? "on" : "off"));
  }
  // a write whose value did not arrive writes nothing, not a default
  for (const char* malformed : {R"({})", R"({"provide_extender":null})",
                                R"({"provide_extender":"false"})", R"({"provide_extender":0})"}) {
    bool threw = false;
    try {
      (void)nlohmann::json::parse(malformed).get<SetProvideExtender>();
    } catch (const std::exception&) {
      threw = true;
    }
    Check(threw, std::string("switch: a request without a boolean value is refused: ") + malformed);
  }

  ProviderStats sent;
  sent.available = true;
  sent.extender_provide_status = nlohmann::json::parse(
      R"({"Supported":true,"State":"off","Reason":"","Enabled":false,"ActivatedV4":false})");
  sent.provide_extender = false;
  sent.provide_extender_writable = true;
  Reply reply;
  reply.ok = true;
  reply.in_reply_to = msg::kGetProviderStats;
  reply.provider_stats = sent;
  const Reply back = nlohmann::json::parse(DumpForWire(nlohmann::json(reply))).get<Reply>();
  Check(back.provider_stats && back.provider_stats->provide_extender_writable,
        "provider stats: provide_extender_writable round-trips");
  Check(back.provider_stats && !back.provider_stats->provide_extender,
        "switch: the setting beside it stays its own");
  // a service from before the verb sends the role without the writer: the
  // switch must stay hidden over it
  const ProviderStats older = nlohmann::json::parse(
      R"({"available":true,"client_count":1,"provide_extender":true,"extender_provide_status":{"Supported":true,"State":"active","Reason":"","Enabled":true,"ActivatedV4":true}})")
                                  .get<ProviderStats>();
  Check(older.provide_extender && !older.provide_extender_writable,
        "switch: a service without the field reads as no writer");
  const ProviderStats none =
      nlohmann::json::parse(DumpForWire(nlohmann::json(ProviderStats{}))).get<ProviderStats>();
  Check(!none.provide_extender_writable, "switch: no provider-only device has no writer");
  Check(kProtocolVersion == 4, "switch: no protocol bump for the verb and its field");
}

// (k) the lag.dev speed tests switch while disconnected (lag.dev/PLAN.md C4):
// the set_measurements_enabled verb and its strict request, and the
// measurements_enabled setting with the measurements_enabled_writable field
// beside the role, an older service's silence read as no writer and the
// setting on
void TestSetMeasurementsEnabled() {
  Check(std::string(msg::kSetMeasurementsEnabled) == "set_measurements_enabled",
        "speed tests: the set_measurements_enabled tag");
  for (const bool on : {false, true}) {
    SetMeasurementsEnabled sent;
    sent.measurements_enabled = on;
    const nlohmann::json request = Request(msg::kSetMeasurementsEnabled, nlohmann::json(sent));
    const nlohmann::json wire = nlohmann::json::parse(DumpForWire(request));
    Check(TypeOf(wire) == "set_measurements_enabled",
          "speed tests: the request carries its type");
    Check(wire.get<SetMeasurementsEnabled>().measurements_enabled == on,
          std::string("speed tests: measurements_enabled round-trips ") + (on ? "on" : "off"));
  }
  // a write whose value did not arrive writes nothing, not a default
  for (const char* malformed :
       {R"({})", R"({"measurements_enabled":null})", R"({"measurements_enabled":"false"})",
        R"({"measurements_enabled":0})", R"({"provide_extender":false})"}) {
    bool threw = false;
    try {
      (void)nlohmann::json::parse(malformed).get<SetMeasurementsEnabled>();
    } catch (const std::exception&) {
      threw = true;
    }
    Check(threw,
          std::string("speed tests: a request without a boolean value is refused: ") + malformed);
  }

  ProviderStats sent;
  sent.available = true;
  sent.extender_provide_status = nlohmann::json::parse(
      R"({"Supported":true,"State":"active","Reason":"","Enabled":true,"ActivatedV4":true})");
  sent.provide_extender = true;
  sent.provide_extender_writable = true;
  sent.measurements_enabled = false;
  sent.measurements_enabled_writable = true;
  Reply reply;
  reply.ok = true;
  reply.in_reply_to = msg::kGetProviderStats;
  reply.provider_stats = sent;
  const Reply back = nlohmann::json::parse(DumpForWire(nlohmann::json(reply))).get<Reply>();
  Check(back.provider_stats && !back.provider_stats->measurements_enabled,
        "provider stats: measurements_enabled round-trips off");
  Check(back.provider_stats && back.provider_stats->measurements_enabled_writable,
        "provider stats: measurements_enabled_writable round-trips");
  Check(back.provider_stats && back.provider_stats->provide_extender,
        "speed tests: the provide extender setting beside it stays its own");
  // a service from before the verb: no writer, and the setting reads as the
  // sdk's default, on
  const ProviderStats older = nlohmann::json::parse(
      R"({"available":true,"client_count":1,"provide_extender":true,"provide_extender_writable":true,"extender_provide_status":{"Supported":true,"State":"active","Reason":"","Enabled":true,"ActivatedV4":true}})")
                                  .get<ProviderStats>();
  Check(older.provide_extender_writable && !older.measurements_enabled_writable,
        "speed tests: a service without the field reads as no writer");
  Check(older.measurements_enabled, "speed tests: and its setting as the default, on");
  const ProviderStats none =
      nlohmann::json::parse(DumpForWire(nlohmann::json(ProviderStats{}))).get<ProviderStats>();
  Check(!none.measurements_enabled_writable && none.measurements_enabled,
        "speed tests: no provider-only device has no writer, and the setting on");
  Check(kProtocolVersion == 4, "speed tests: no protocol bump for the verb and its fields");
}

// (l) the router port mapping switch while disconnected (connect EXTENDER.md
// S8): the set_provide_extender_port_mapping verb and its strict request, and
// the provide_extender_port_mapping setting with the
// provide_extender_port_mapping_writable field beside the role, an older
// service's silence read as no writer and the setting off, the apps' default
void TestSetProvideExtenderPortMapping() {
  Check(std::string(msg::kSetProvideExtenderPortMapping) == "set_provide_extender_port_mapping",
        "port mapping: the set_provide_extender_port_mapping tag");
  Check(!SetProvideExtenderPortMapping{}.provide_extender_port_mapping,
        "port mapping: the request starts off");
  for (const bool on : {false, true}) {
    SetProvideExtenderPortMapping sent;
    sent.provide_extender_port_mapping = on;
    const nlohmann::json request =
        Request(msg::kSetProvideExtenderPortMapping, nlohmann::json(sent));
    const nlohmann::json wire = nlohmann::json::parse(DumpForWire(request));
    Check(TypeOf(wire) == "set_provide_extender_port_mapping",
          "port mapping: the request carries its type");
    Check(wire.get<SetProvideExtenderPortMapping>().provide_extender_port_mapping == on,
          std::string("port mapping: provide_extender_port_mapping round-trips ") +
              (on ? "on" : "off"));
  }
  // a write whose value did not arrive writes nothing, not a default, and
  // least of all opens a port
  for (const char* malformed :
       {R"({})", R"({"provide_extender_port_mapping":null})",
        R"({"provide_extender_port_mapping":"true"})", R"({"provide_extender_port_mapping":1})",
        R"({"provide_extender":true})", R"({"port_mapping":true})"}) {
    bool threw = false;
    try {
      (void)nlohmann::json::parse(malformed).get<SetProvideExtenderPortMapping>();
    } catch (const std::exception&) {
      threw = true;
    }
    Check(threw,
          std::string("port mapping: a request without a boolean value is refused: ") + malformed);
  }

  ProviderStats sent;
  sent.available = true;
  sent.extender_provide_status = nlohmann::json::parse(
      R"({"Supported":true,"State":"active","Reason":"","Enabled":true,"ActivatedV4":true})");
  sent.provide_extender = true;
  sent.provide_extender_writable = true;
  sent.measurements_enabled = false;
  sent.measurements_enabled_writable = true;
  sent.provide_extender_port_mapping = true;
  sent.provide_extender_port_mapping_writable = true;
  Reply reply;
  reply.ok = true;
  reply.in_reply_to = msg::kGetProviderStats;
  reply.provider_stats = sent;
  const Reply back = nlohmann::json::parse(DumpForWire(nlohmann::json(reply))).get<Reply>();
  Check(back.provider_stats && back.provider_stats->provide_extender_port_mapping,
        "provider stats: provide_extender_port_mapping round-trips on");
  Check(back.provider_stats && back.provider_stats->provide_extender_port_mapping_writable,
        "provider stats: provide_extender_port_mapping_writable round-trips");
  Check(back.provider_stats && back.provider_stats->provide_extender &&
            !back.provider_stats->measurements_enabled,
        "port mapping: the settings beside it stay their own");
  // a service from before the verb: no writer, and the setting reads as the
  // apps' default, off
  const ProviderStats older = nlohmann::json::parse(
      R"({"available":true,"client_count":1,"provide_extender":true,"provide_extender_writable":true,"measurements_enabled":true,"measurements_enabled_writable":true,"extender_provide_status":{"Supported":true,"State":"active","Reason":"","Enabled":true,"ActivatedV4":true}})")
                                  .get<ProviderStats>();
  Check(older.measurements_enabled_writable && !older.provide_extender_port_mapping_writable,
        "port mapping: a service without the field reads as no writer");
  Check(!older.provide_extender_port_mapping, "port mapping: and its setting as the default, off");
  const ProviderStats none =
      nlohmann::json::parse(DumpForWire(nlohmann::json(ProviderStats{}))).get<ProviderStats>();
  Check(!none.provide_extender_port_mapping_writable && !none.provide_extender_port_mapping,
        "port mapping: no provider-only device has no writer, and the setting off");
  Check(kProtocolVersion == 4, "port mapping: no protocol bump for the verb and its fields");
}

}  // namespace

int main() {
  TestTags();
  TestStartProviderJson();
  TestStatusFields();
  TestSameProviderDevice();
  TestSameDeviceAcrossJwtRefresh();
  TestProviderFacts();
  TestProviderStatsReply();
  TestProviderStatsReaders();
  TestProviderStatsPeers();
  TestProviderStatsExtender();
  TestExtenderReaders();
  TestSetProvideExtender();
  TestSetMeasurementsEnabled();
  TestSetProvideExtenderPortMapping();
  std::cout << (gCases - gFailures) << "/" << gCases << " provide protocol checks passed"
#if defined(URNW_PROVIDE_PROTOCOL_TESTS_SDK)
            << " (against urnetwork_sdk.hpp)"
#endif
            << "\n";
  return gFailures == 0 ? 0 : 1;
}
