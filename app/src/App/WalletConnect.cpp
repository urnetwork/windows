// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "WalletConnect.h"

#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Config.h"

namespace urnw {
namespace {

constexpr const char* kWebBridge = "https://ur.io/wallet-connect";
constexpr const char* kAppUrl = "https://ur.io";
constexpr const char* kCluster = "mainnet-beta";

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// Percent-encode everything except RFC 3986 unreserved characters.
std::string Esc(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

std::string Unesc(const std::string& s) {
  auto hexv = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hexv(s[i + 1]), lo = hexv(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i] == '+' ? ' ' : s[i]);
  }
  return out;
}

std::string Base64(const uint8_t* data, size_t len) {
  DWORD n = 0;
  CryptBinaryToStringA(data, static_cast<DWORD>(len), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       nullptr, &n);
  std::string s(n, '\0');
  if (!CryptBinaryToStringA(data, static_cast<DWORD>(len),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &n)) {
    return "";
  }
  s.resize(n);
  return s;
}

void SplitUrl(const std::string& url, std::string& host, std::string& query) {
  auto scheme = url.find("://");
  size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;
  auto q = url.find('?', start);
  auto slash = url.find('/', start);
  size_t hostEnd = (std::min)(q == std::string::npos ? url.size() : q,
                              slash == std::string::npos ? url.size() : slash);
  host = url.substr(start, hostEnd - start);
  query = (q == std::string::npos) ? std::string() : url.substr(q + 1);
}

std::map<std::string, std::string> ParseQuery(const std::string& query) {
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < query.size()) {
    auto amp = query.find('&', i);
    std::string pair =
        query.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
    auto eq = pair.find('=');
    if (eq != std::string::npos) out[pair.substr(0, eq)] = Unesc(pair.substr(eq + 1));
    if (amp == std::string::npos) break;
    i = amp + 1;
  }
  return out;
}

}  // namespace

const char* WalletConnect::Host(Provider p) {
  switch (p) {
    case Provider::Solflare: return "solflare";
    case Provider::Bittensor: return "bittensor";
    default: return "phantom";
  }
}

std::optional<WalletConnect::Provider> WalletConnect::ProviderForHost(const std::string& host) {
  if (host == "phantom-connect" || host == "phantom-sign-message") return Provider::Phantom;
  if (host == "solflare-connect" || host == "solflare-sign-message") return Provider::Solflare;
  if (host == "bittensor-connect" || host == "bittensor-sign-message") return Provider::Bittensor;
  return std::nullopt;
}

bool WalletConnect::NewKeyPair() {
  dappKeyPair_ = urnet::generateWalletKeyPair();
  return dappKeyPair_.has_value();
}

std::optional<std::string> WalletConnect::SharedSecretBase58() const {
  if (!dappKeyPair_ || !walletEncryptionPublicKey_) return std::nullopt;
  auto priv = urnet::decodeBase58(dappKeyPair_->PrivateKeyBase58);
  auto pub = urnet::decodeBase58(*walletEncryptionPublicKey_);
  if (!priv || !pub) return std::nullopt;
  auto shared = urnet::generateSharedSecret(*priv, *pub);
  if (shared.empty()) return std::nullopt;
  return urnet::encodeBase58(shared.data(), static_cast<int32_t>(shared.size()));
}

void WalletConnect::OpenUrl(const std::string& url) {
  std::wstring w = Widen(url);
  HINSTANCE h = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(h) <= 32 && on_error) on_error("failed to open browser");
}

void WalletConnect::Connect(Provider p) {
  connectedPublicKey_.reset();
  walletEncryptionPublicKey_.reset();
  session_.reset();
  currentProvider_ = p;
  if (!NewKeyPair()) {
    if (on_error) on_error("failed to generate wallet keypair");
    return;
  }
  const std::string redirect = std::string("urnetwork://") + Host(p) + "-connect";
  std::string url = std::string(kWebBridge) +
                    "?dapp_encryption_public_key=" + Esc(dappKeyPair_->PublicKeyBase58) +
                    "&cluster=" + kCluster + "&app_url=" + Esc(kAppUrl) +
                    "&redirect_link=" + Esc(redirect) + "&method=connect&provider=" + Host(p);
  OpenUrl(url);
}

void WalletConnect::SignMessage(const std::string& message) {
  if (!dappKeyPair_ || !session_ || !walletEncryptionPublicKey_) {
    if (on_error) on_error("wallet not connected");
    return;
  }
  const std::string messageB58 = urnet::encodeBase58(
      reinterpret_cast<const uint8_t*>(message.data()), static_cast<int32_t>(message.size()));
  const std::string payload =
      nlohmann::json{{"message", messageB58}, {"session", *session_}, {"display", "utf8"}}.dump();
  auto sharedB58 = SharedSecretBase58();
  if (!sharedB58) {
    if (on_error) on_error("failed to derive shared secret");
    return;
  }
  const std::string nonce = urnet::generateNonce();
  const std::string enc =
      urnet::encryptData(reinterpret_cast<const uint8_t*>(payload.data()),
                         static_cast<int32_t>(payload.size()), nonce, *sharedB58);
  if (enc.empty()) {
    if (on_error) on_error("failed to encrypt sign-message payload");
    return;
  }
  const std::string redirect =
      std::string("urnetwork://") + Host(currentProvider_) + "-sign-message";
  std::string url = std::string(kWebBridge) +
                    "?dapp_encryption_public_key=" + Esc(dappKeyPair_->PublicKeyBase58) +
                    "&cluster=" + kCluster + "&nonce=" + Esc(nonce) +
                    "&redirect_link=" + Esc(redirect) + "&payload=" + Esc(enc) +
                    "&method=signMessage&provider=" + Host(currentProvider_);
  OpenUrl(url);
}

void WalletConnect::SignMessageBittensor(const std::string& message) {
  // No connect handshake and no encryption envelope: the bridge drives an
  // injected substrate wallet (Bittensor Wallet, SubWallet, Talisman,
  // polkadot-js) and returns the ss58 address with the sr25519 signature.
  connectedPublicKey_.reset();
  walletEncryptionPublicKey_.reset();
  session_.reset();
  currentProvider_ = Provider::Bittensor;
  const std::string redirect =
      std::string("urnetwork://") + Host(Provider::Bittensor) + "-sign-message";
  std::string url = std::string(kWebBridge) + "?provider=" + Host(Provider::Bittensor) +
                    "&method=signMessage&message=" + Esc(message) +
                    "&redirect_link=" + Esc(redirect);
  // The WalletConnect Cloud project id lets the bridge pair with a wallet app;
  // without one the bridge falls back to injected (extension) wallets only.
  const std::string projectId = config::kWalletConnectProjectId;
  if (!projectId.empty()) url += "&wc_project_id=" + Esc(projectId);
  OpenUrl(url);
}

bool WalletConnect::HandleDeepLink(const std::string& url) {
  std::string host, query;
  SplitUrl(url, host, query);
  auto provider = ProviderForHost(host);
  if (!provider) return false;
  if (*provider == Provider::Bittensor)
    HandleBittensor(host, query);
  else if (host.find("-connect") != std::string::npos)
    HandleConnect(*provider, query);
  else
    HandleSignMessage(*provider, query);
  return true;
}

void WalletConnect::HandleConnect(Provider p, const std::string& query) {
  auto params = ParseQuery(query);
  if (params.count("errorCode")) {
    if (on_error) on_error(params.count("errorMessage") ? params["errorMessage"] : "wallet connect error");
    return;
  }
  const std::string keyParam = std::string(Host(p)) + "_encryption_public_key";
  if (!params.count(keyParam) || !params.count("nonce") || !params.count("data") || !dappKeyPair_) {
    if (on_error) on_error("missing wallet connect parameters");
    return;
  }
  walletEncryptionPublicKey_ = params[keyParam];
  auto sharedB58 = SharedSecretBase58();
  if (!sharedB58) {
    if (on_error) on_error("failed to derive shared secret");
    return;
  }
  auto decrypted = urnet::decryptData(params["data"], params["nonce"], *sharedB58);
  if (decrypted.empty()) {
    if (on_error) on_error("failed to decrypt wallet connection");
    return;
  }
  try {
    auto j = nlohmann::json::parse(std::string(decrypted.begin(), decrypted.end()));
    connectedPublicKey_ = j.at("public_key").get<std::string>();
    session_ = j.at("session").get<std::string>();
    currentProvider_ = p;
    if (on_public_key) on_public_key(*connectedPublicKey_, p);
  } catch (const std::exception& e) {
    if (on_error) on_error(std::string("bad connect response: ") + e.what());
  }
}

void WalletConnect::HandleSignMessage(Provider p, const std::string& query) {
  auto params = ParseQuery(query);
  if (params.count("errorCode")) {
    if (on_error) on_error(params.count("errorMessage") ? params["errorMessage"] : "wallet signing error");
    return;
  }
  if (!params.count("nonce") || !params.count("data") || !dappKeyPair_ ||
      !walletEncryptionPublicKey_ || !connectedPublicKey_) {
    if (on_error) on_error("missing wallet signature parameters");
    return;
  }
  auto sharedB58 = SharedSecretBase58();
  if (!sharedB58) {
    if (on_error) on_error("failed to derive shared secret");
    return;
  }
  auto decrypted = urnet::decryptData(params["data"], params["nonce"], *sharedB58);
  if (decrypted.empty()) {
    if (on_error) on_error("failed to decrypt wallet signature");
    return;
  }
  try {
    auto j = nlohmann::json::parse(std::string(decrypted.begin(), decrypted.end()));
    const std::string signatureB58 = j.at("signature").get<std::string>();
    auto sigBytes = urnet::decodeBase58(signatureB58);
    if (!sigBytes) {
      if (on_error) on_error("failed to decode wallet signature");
      return;
    }
    // The backend expects a base64 signature for Solana (macOS parity).
    if (on_signature)
      on_signature(*connectedPublicKey_, Base64(sigBytes->data(), sigBytes->size()), p);
  } catch (const std::exception& e) {
    if (on_error) on_error(std::string("bad signature response: ") + e.what());
  }
}

// Bittensor returns plain query params from the bridge — sr25519 signatures are
// public, so there is no envelope to decrypt (apple/android parity):
//   urnetwork://bittensor-sign-message?address=<ss58>&signature=<0xhex>
//   urnetwork://bittensor-connect?address=<ss58>
//   urnetwork://bittensor-*?errorCode=-1&errorMessage=<text>
void WalletConnect::HandleBittensor(const std::string& host, const std::string& query) {
  auto params = ParseQuery(query);
  if (params.count("errorCode")) {
    if (on_error) on_error(params.count("errorMessage") ? params["errorMessage"] : "wallet signing error");
    return;
  }
  const std::string address = params.count("address") ? params["address"] : std::string();
  if (address.empty()) {
    if (on_error) on_error("missing wallet address parameter");
    return;
  }
  connectedPublicKey_ = address;
  currentProvider_ = Provider::Bittensor;
  if (host == "bittensor-connect") {
    if (on_public_key) on_public_key(address, Provider::Bittensor);
    return;
  }
  const std::string signature = params.count("signature") ? params["signature"] : std::string();
  if (signature.empty()) {
    if (on_error) on_error("missing wallet signature parameters");
    return;
  }
  // The server verifies the hex sr25519 signature as returned (no re-encoding).
  if (on_signature) on_signature(address, signature, Provider::Bittensor);
}

}  // namespace urnw
