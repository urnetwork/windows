// SPDX-License-Identifier: MPL-2.0
#include "PostQuantumIdentity.h"

// WinRT-free translation unit (App.vcxproj PrecompiledHeader=NotUsing), so the
// Win32 crypto header is included directly rather than via the WinUI pch.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>  // CryptStringToBinaryA (crypt32.lib, already linked)

#include <utility>

namespace urnw {
namespace {

// Standard base64 decode (the SDK marshals ProviderIdentity.PublicKey with Go's
// encoding/json, i.e. StdEncoding: +/ and padding). The two-call idiom sizes the
// buffer first. Returns empty on any failure -- the caller skips such an entry.
std::vector<uint8_t> DecodeBase64(const std::string& base64) {
  std::vector<uint8_t> out;
  if (base64.empty()) return out;
  DWORD len = 0;
  if (!CryptStringToBinaryA(base64.c_str(), static_cast<DWORD>(base64.size()),
                            CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) {
    return out;
  }
  out.resize(len);
  if (!CryptStringToBinaryA(base64.c_str(), static_cast<DWORD>(base64.size()),
                            CRYPT_STRING_BASE64, out.data(), &len, nullptr, nullptr)) {
    out.clear();
    return out;
  }
  out.resize(len);
  return out;
}

}  // namespace

std::vector<ProviderIdentityRow> ReadProviderIdentityRows(
    const std::optional<urnet::ProviderIdentityList>& list) {
  std::vector<ProviderIdentityRow> rows;
  if (!list) return rows;
  rows.reserve(list->size());
  for (const auto& identity : *list) {
    if (!identity.ClientId || identity.ClientId->empty()) continue;
    std::vector<uint8_t> key = DecodeBase64(identity.PublicKey);
    if (key.empty()) continue;
    ProviderIdentityRow row;
    row.clientId = *identity.ClientId;
    row.hash = urnet::publicIdentityKeyHash(key.data(), static_cast<int32_t>(key.size()));
    row.key = std::move(key);
    rows.push_back(std::move(row));
  }
  return rows;
}

bool SameProviderIdentityRows(const std::vector<ProviderIdentityRow>& a,
                              const std::vector<ProviderIdentityRow>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

}  // namespace urnw
