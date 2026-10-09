// Executable spec for what a network peer row connects to (App/PeerLocation.h):
// a tap on one of the user's own devices in the chooser sheet or on the
// Network page connects to the peer's client id with network_peer set, so the
// SDK reaches the device as a trusted same-network peer (the Network provide
// mode) and not as a public exit, as android and apple do; and a re-tap of a
// device the user picked before the flag was set is not swallowed as "already
// selected". Run against the same header the app compiles, on any host with a
// C++20 compiler. The rows and SdkHost cannot be built off Windows, so the
// wiring cases read their source.
//
// By default the templates are instantiated with stand-ins that carry the
// fields of urnet::NetworkPeer, urnet::ConnectLocationId and
// urnet::ConnectLocation under the generated wrapper's names and types:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -I ../src/App peer-location-tests.cpp -o /tmp/peer-location-tests && /tmp/peer-location-tests ..
//
// With URNW_PEER_LOCATION_TESTS_SDK it is built against the generated header
// itself, and the location is also checked as the json the SDK receives (the
// header needs nlohmann/json; both are system includes because the generated
// code does not build with -Wextra -Werror):
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_PEER_LOCATION_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> peer-location-tests.cpp -o /tmp/peer-location-tests && /tmp/peer-location-tests ..
//
// The argument is the app directory (default ".."): the wiring cases read
// src/App/LocationSheets.cpp and src/App/SdkHost.cpp.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "PeerLocation.h"

#if defined(URNW_PEER_LOCATION_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Case(const std::string& name) {
  ++gCases;
  gCurrentCase = name;
}

void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    Fail("cannot read " + path);
    return {};
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// The body of the definition that starts at `signature`, through the closing
// brace in its first column; empty (and a failure) when it is gone.
std::string DefinitionBody(const std::string& source, const std::string& signature) {
  const size_t start = source.find(signature);
  if (start == std::string::npos) {
    Fail("no definition " + signature);
    return {};
  }
  const size_t end = source.find("\n}\n", start);
  return source.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

size_t Count(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

#if defined(URNW_PEER_LOCATION_TESTS_SDK)
using Peer = urnet::NetworkPeer;
using Location = urnet::ConnectLocation;
#else
// The fields of urnet::NetworkPeer, urnet::ConnectLocationId and
// urnet::ConnectLocation, by the generated wrapper's names and types.
struct Peer {
  std::optional<std::string> ClientId;
  bool ProvideEnabled{};
  std::string Principal{};
  std::string DeviceSpec{};
  std::string DeviceName{};
};

struct LocationId {
  std::optional<std::string> client_id;
  std::optional<std::string> location_id;
  std::optional<std::string> location_group_id;
  std::optional<bool> best_available;
};

struct Location {
  std::optional<LocationId> connect_location_id;
  std::optional<std::string> name;
  std::optional<int32_t> provider_count;
  std::optional<std::string> location_type;
  std::optional<std::string> country_code;
  bool stable{};
  bool strong_privacy{};
  std::optional<bool> network_peer;
};
#endif

constexpr const char* kPeerClientId = "018f2c3e-7a10-7b44-9c1d-5e2a3f4b5c6d";

Peer MakePeer(const std::string& name, const std::string& spec) {
  Peer peer;
  peer.ClientId = kPeerClientId;
  peer.ProvideEnabled = true;
  peer.DeviceName = name;
  peer.DeviceSpec = spec;
  return peer;
}

// A location the chooser lists from the SDK (a country), which never carries
// the network peer flag.
Location MakeCountry(const std::string& id) {
  Location location;
  decltype(location.connect_location_id)::value_type locationId;
  locationId.location_id = id;
  location.connect_location_id = locationId;
  location.name = "Germany";
  location.location_type = "country";
  location.country_code = "de";
  return location;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string appDir = 1 < argc ? argv[1] : "..";

  // ---- the location a peer row connects to -----------------------------------

  {
    Case("a peer row connects to the peer's client id as a network peer");
    const Location location =
        urnw::PeerConnectLocation<Location>(MakePeer("Office laptop", "Windows 11"));
    Check(location.network_peer == std::optional<bool>(true),
          "network_peer is not set: the SDK would reach the device as a public exit");
    Check(location.connect_location_id.has_value(), "no connect_location_id");
    if (location.connect_location_id) {
      const auto& id = *location.connect_location_id;
      Check(id.client_id == std::optional<std::string>(kPeerClientId),
            "the location id is not the peer's client id");
      Check(!id.location_id && !id.location_group_id,
            "a peer location carries no location or group id");
      Check(!id.best_available.value_or(false), "a peer location is not best available");
    }
    Check(location.name == std::optional<std::string>("Office laptop"),
          "the location is not named after the peer");
  }
  {
    Case("the peer's name is its device name, else its device spec, else its client id");
    Check(urnw::PeerDisplayName(MakePeer("Office laptop", "Windows 11")) == "Office laptop",
          "the device name comes first");
    Check(urnw::PeerDisplayName(MakePeer("", "Windows 11")) == "Windows 11",
          "the device spec stands in for a missing name");
    Check(urnw::PeerDisplayName(MakePeer("", "")) == kPeerClientId,
          "the client id stands in for both");
    Peer anonymous = MakePeer("", "");
    anonymous.ClientId.reset();
    Check(urnw::PeerDisplayName(anonymous).empty(), "no name at all is empty");
    Check(urnw::PeerConnectLocation<Location>(MakePeer("", "Windows 11")).name ==
              std::optional<std::string>("Windows 11"),
          "the location takes the same name the row shows");
  }

  // ---- the row coalescer's "already there" -----------------------------------

  {
    Case("a peer row is current only while the selection is the same peer as a peer");
    const Location peer = urnw::PeerConnectLocation<Location>(MakePeer("Office laptop", ""));
    Check(urnw::SameNetworkPeer(std::optional<Location>(peer), peer),
          "the selected peer, tapped again, is not current");

    // what a peer row selected before the flag was set: same client id, public
    Location before = peer;
    before.network_peer.reset();
    Check(!urnw::SameNetworkPeer(std::optional<Location>(before), peer),
          "a peer selected as a public exit swallows the tap that would make it a peer");
    before.network_peer = false;
    Check(!urnw::SameNetworkPeer(std::optional<Location>(before), peer),
          "a peer selected with network_peer false swallows the tap");

    Check(!urnw::SameNetworkPeer(std::optional<Location>(), peer),
          "nothing selected is not current");
  }
  {
    Case("every other row is current as it was: no location carries the flag");
    const Location country = MakeCountry("country-de");
    Check(urnw::SameNetworkPeer(std::optional<Location>(country), country),
          "an unflagged location, tapped again, is no longer current");
    Location flaggedFalse = country;
    flaggedFalse.network_peer = false;
    Check(urnw::SameNetworkPeer(std::optional<Location>(flaggedFalse), country),
          "network_peer false and unset are told apart");
    Check(urnw::SameNetworkPeer(std::optional<Location>(country), flaggedFalse),
          "unset and network_peer false are told apart");
  }

#if defined(URNW_PEER_LOCATION_TESTS_SDK)
  {
    Case("the SDK receives the peer location with network_peer true");
    const nlohmann::json json =
        urnw::PeerConnectLocation<Location>(MakePeer("Office laptop", "Windows 11"));
    Check(json.contains("network_peer") && json["network_peer"] == true,
          "the json has no network_peer true: " + json.dump());
    Check(json["connect_location_id"]["client_id"] == kPeerClientId,
          "the json's location id is not the peer's client id: " + json.dump());
    const Location back = json.get<Location>();
    Check(back.network_peer == std::optional<bool>(true), "the flag is lost on the way back");
  }
#endif

  // ---- wiring ------------------------------------------------------------------

  const std::string sheetsPath = appDir + "/src/App/LocationSheets.cpp";
  {
    Case("the chooser sheet's peer row connects through PeerConnectLocation");
    const std::string body =
        DefinitionBody(ReadFile(sheetsPath), "Grid LocationChooserSheet::MakePeerRow(");
    Check(body.find("self->sdk_.ConnectFromRow(PeerConnectLocation<urnet::ConnectLocation>("
                    "peerCopy));") != std::string::npos,
          "MakePeerRow does not connect to PeerConnectLocation(peerCopy)");
  }
  {
    // The list rows hold a key, not a location: a click resolves the key against
    // the cached feeds (NetworkPage::ConnectFromListKey), so a row rewritten in
    // place connects to what it shows now, and the peer it finds is the live
    // entry rather than a copy captured when the row was built.
    Case("the Network page's peer rows connect through PeerConnectLocation");
    const std::string body =
        DefinitionBody(ReadFile(sheetsPath), "void NetworkPage::ConnectFromListKey(");
    Check(body.find("Sdk().ConnectFromRow(PeerConnectLocation<urnet::ConnectLocation>(peer));") !=
              std::string::npos,
          "NetworkPage::ConnectFromListKey does not connect its peer rows to "
          "PeerConnectLocation(peer)");
  }
  {
    Case("no peer row builds its location by hand");
    // a hand-built client id location is how the rows lost the flag
    const std::string sheets = ReadFile(sheetsPath);
    Check(Count(sheets, "client_id =") == 0,
          "LocationSheets.cpp assigns a client id itself; build peer locations with "
          "PeerConnectLocation");
    Check(Count(sheets, "PeerConnectLocation<urnet::ConnectLocation>(") == 2,
          "expected the two peer rows (sheet, Network page) to use PeerConnectLocation");
  }
  {
    Case("SdkHost's re-select no-op compares the peer flag");
    const std::string host = ReadFile(appDir + "/src/App/SdkHost.cpp");
    const std::string body = DefinitionBody(host, "void SdkHost::ConnectFromRow(");
    Check(body.find("IsLocationSelected(sel, location) && SameNetworkPeer(sel, location)") !=
              std::string::npos,
          "ConnectFromRow treats a peer selected as a public exit as already current");
  }

  std::cout << (gFailures == 0 ? "PASS" : "FAIL") << " peer location"
#if defined(URNW_PEER_LOCATION_TESTS_SDK)
            << " (against urnetwork_sdk.hpp)"
#endif
            << ": " << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
