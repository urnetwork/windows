// Post Quantum Identity data + logic shared by the SdkHost identity feed and
// the provider-locations badge (port of the android PostQuantumIdentityViewModel
// row + apple ProviderIdentityRow / linux IdentityRow). Kept WinRT-free, like
// ProviderLocations.h, so SdkHost.h can carry the row type and the pure logic
// stays unit-testable off a Windows host. The identicon rasterization (which
// needs WinUI image types) lives in IdenticonImage.h.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Sdk.h"

namespace urnw {

// the provider-locations trailing badge size (android BADGE_IDENTICON_SIZE);
// rasters render at 2x this, like every other identicon size
constexpr int kBadgeIdenticonSize = 16;

// One provider with an established, identity-verified e2e session: the egress
// client id (the join key against the provider-locations rows), the canonical
// key hash, and the raw public identity key the identicon renders from.
struct ProviderIdentityRow {
  std::string clientId;
  std::string hash;
  std::vector<uint8_t> key;
};

// value equality on (client id, hash) -- the identicon derives from the key,
// which the hash captures (apple ProviderIdentityRow ==)
inline bool operator==(const ProviderIdentityRow& a, const ProviderIdentityRow& b) {
  return a.clientId == b.clientId && a.hash == b.hash;
}
inline bool operator!=(const ProviderIdentityRow& a, const ProviderIdentityRow& b) {
  return !(a == b);
}

// Decode the JSON-crossing ProviderIdentityList (PublicKey crosses as base64)
// into rows, computing the canonical hash through the SDK rule
// (urnet::publicIdentityKeyHash), like apple's identity.getPublicKeyHash().
// Entries with no client id or key are skipped.
std::vector<ProviderIdentityRow> ReadProviderIdentityRows(
    const std::optional<urnet::ProviderIdentityList>& list);

// value equality across the row set (skips a rebuild when nothing changed)
bool SameProviderIdentityRows(const std::vector<ProviderIdentityRow>& a,
                              const std::vector<ProviderIdentityRow>& b);

}  // namespace urnw
