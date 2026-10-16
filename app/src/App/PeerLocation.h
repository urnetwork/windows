// What a network peer row connects to: the location the chooser sheet's pinned
// peer section and the Network page's peers group build when one of the user's
// own devices is tapped (LocationSheets.cpp), and the name those rows and the
// connect drawer show for a peer.
//
// A peer row is one of the user's own devices (PeerViewController: connected
// and provide-enabled), so its location carries network_peer = true. The SDK
// then reaches the device as a trusted same-network peer and the connection
// egresses under the Network provide mode (sdk device_local.go). Without the
// flag the SDK takes the client id for a public exit and the connection runs
// under the Public provide mode. android
// (NetworkPeersViewModel.connectLocationForPeer) and apple
// (NetworkPeerItem.toConnectLocation) set it the same way. "Stay on this exit"
// (ProviderLocationsSheet.cpp) also connects to a client id, but to a public
// exit, and leaves it false.
//
// Header-only, WinRT-free and SDK-free: the functions are templates over the
// shapes of urnet::NetworkPeer and urnet::ConnectLocation (the generated
// urnetwork_sdk.hpp's names and types), so tools/peer-location-tests.cpp runs
// them on any host with a C++20 compiler; LocationSheets.cpp and SdkHost.cpp
// instantiate them with the real types.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <optional>
#include <string>

namespace urnw {

// A network peer's display name: DeviceName, else DeviceSpec, else the client
// id. Shared with the connect drawer's selected-location label (req4).
template <class Peer>
std::string PeerDisplayName(const Peer& peer) {
  if (!peer.DeviceName.empty()) return peer.DeviceName;
  if (!peer.DeviceSpec.empty()) return peer.DeviceSpec;
  return peer.ClientId.value_or(std::string());
}

// The selected-location label the connect drawer and the status strip show
// (req4): the location's own `name`, unless `selected` names a connected,
// provide-enabled network peer, whose display name it then is.
// `connectedPeers` is the device's connected peers (getNetworkPeers), which
// PeerViewController.getPeers filters by ProvideEnabled the same way.
// SdkHost::ReadStats resolves it off the UI thread.
template <class Location, class PeerList>
std::string SelectedLocationName(std::string name, const std::optional<Location>& selected,
                                 const std::optional<PeerList>& connectedPeers) {
  if (!selected || !selected->connect_location_id || !connectedPeers) return name;
  const auto& clientId = selected->connect_location_id->client_id;
  if (!clientId || clientId->empty()) return name;
  for (const auto& peer : *connectedPeers) {
    if (peer.ProvideEnabled && peer.ClientId && *peer.ClientId == *clientId) {
      return PeerDisplayName(peer);
    }
  }
  return name;
}

// The location a tap on a peer row connects to: the peer's client id as the
// location id, its display name, and network_peer set.
template <class Location, class Peer>
Location PeerConnectLocation(const Peer& peer) {
  Location location;
  typename decltype(location.connect_location_id)::value_type id;
  id.client_id = peer.ClientId;
  location.connect_location_id = id;
  location.name = PeerDisplayName(peer);
  location.network_peer = true;
  return location;
}

// Whether `selected` reaches its destination the way `location` would, as a
// network peer or as a public exit. The SDK installs a different transport for
// each (connectLocationTransportEqual), so SdkHost::ConnectFromRow's re-select
// no-op asks this on top of IsLocationSelected: a device picked from the peer
// list before peer rows set the flag is the same location by id, and tapping
// it again must still reconnect it as a network peer.
template <class Location>
bool SameNetworkPeer(const std::optional<Location>& selected, const Location& location) {
  return selected &&
         selected->network_peer.value_or(false) == location.network_peer.value_or(false);
}

}  // namespace urnw
