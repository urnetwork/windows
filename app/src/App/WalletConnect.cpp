// SPDX-License-Identifier: MPL-2.0
// the project compiles with /Yu"pch.h" (App.vcxproj), so every translation unit
// must include it first
#include "pch.h"

#include "WalletConnect.h"

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincrypt.h>

#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "BridgeTrampoline.h"
#include "Config.h"
#include "Localization.h"
#include "Log.h"
#include "SolanaWalletPresentation.h"
#include "Strings.h"
#include "UrlQuery.h"

// AssocQueryStringW: the program Windows opens https links with
#pragma comment(lib, "shlwapi.lib")

namespace urnw {
namespace {

constexpr const char* kWebBridge = "https://ur.io/wallet-connect";
// Sign in with Apple (OpenAppleOAuth): Apple's web flow, the api's callback
constexpr const char* kAppleAuthorize = "https://appleid.apple.com/auth/authorize";
constexpr const char* kAppleServicesId = "network.ur.service";  // the web client id
constexpr const char* kAppleCallbackPath = "/auth/apple/callback";
constexpr const char* kOAuthReturnHost = "oauth";   // urnetwork://oauth/<provider>
constexpr const char* kAppleReturnPath = "/apple";
// Sign in with Google (OpenGoogleOAuth): Google's web flow (authorization
// code), the api's callback exchanges the code and returns the identity token
constexpr const char* kGoogleAuthorize = "https://accounts.google.com/o/oauth2/v2/auth";
// the ur.io web sign-in client (SsoBridge.jsx); the api's callback holds its secret
constexpr const char* kGoogleClientId =
    "338638865390-cg4m0t700mq9073smhn9do81mr640ig1.apps.googleusercontent.com";
constexpr const char* kGoogleCallbackPath = "/auth/google/callback";
constexpr const char* kGoogleReturnPath = "/google";
constexpr const char* kPlatform = "windows";
constexpr const char* kAppUrl = "https://ur.io";
constexpr const char* kCluster = "mainnet-beta";
// A bridge page's DACL (BridgeTrampoline.h): protected, with full control for
// its owner, the signed-in user who wrote it, and nothing for anyone else.
constexpr wchar_t kOwnerOnlyFileSddl[] = L"D:P(A;;FA;;;OW)";

std::wstring Widen(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
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

// The path of a url (between the host and the query), "" when there is none.
std::string UrlPath(const std::string& url) {
  auto scheme = url.find("://");
  size_t start = (scheme == std::string::npos) ? 0 : scheme + 3;
  auto q = url.find('?', start);
  auto slash = url.find('/', start);
  if (slash == std::string::npos || (q != std::string::npos && q < slash)) return std::string();
  return url.substr(slash, q == std::string::npos ? std::string::npos : q - slash);
}

// base64url without padding, for the Apple attempt state.
std::string Base64Url(const std::string& s) {
  std::string out = Base64(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  for (auto& c : out) {
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
  }
  while (!out.empty() && out.back() == '=') out.pop_back();
  return out;
}

// The words for a failure the bridge page handed back: this app's own for a
// code it knows (solana::BridgeErrorTextFor), with the wallet's name where the
// string takes it, else the page's text.
std::string LocalizedBridgeError(WalletConnect::Provider p, const std::string& code,
                                 const std::string& pageText) {
  const solana::BridgeErrorText text = solana::BridgeErrorTextFor(code);
  if (text.key.empty()) return pageText;
  if (!text.takesWalletName) return Narrow(Localized(text.key));
  const std::wstring walletName =
      Localized(p == WalletConnect::Provider::Solflare ? "solflare" : "phantom");
  return Narrow(Format(text.key, walletName));
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

// urnet::decryptData throws (urnet::Error) for a nonce or data of the wrong
// length and for a failed decryption, and both arrive on a link that any program
// can fire at the app: a throw must not leave the handler, so a failure is an
// empty answer here.
std::optional<std::vector<uint8_t>> TryDecrypt(const std::string& dataBase58,
                                               const std::string& nonceBase58,
                                               const std::string& sharedSecretBase58) {
  try {
    return urnet::decryptData(dataBase58, nonceBase58, sharedSecretBase58);
  } catch (const std::exception&) {
    return std::nullopt;
  }
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
  // the wallet's key came off a link: generateSharedSecret throws for a key of
  // the wrong length, and a throw must not leave the handler
  try {
    auto shared = urnet::generateSharedSecret(*priv, *pub);
    if (shared.empty()) return std::nullopt;
    return urnet::encodeBase58(shared.data(), static_cast<int32_t>(shared.size()));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void WalletConnect::OpenUrl(const std::string& url) {
  if (bridge::NeedsTrampoline(url)) {
    OpenThroughTrampoline(url);
    return;
  }
  std::wstring w = Widen(url);
  HINSTANCE h = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(h) <= 32 && on_error) on_error("failed to open browser");
}

// A url longer than ShellExecuteW passes on (the payout consent's bridge url):
// a page in the user's own temp directory that only its owner may open, which
// replaces itself with the url. The browser Windows opens https links with is
// started on the page's short path, and the page is deleted once the browser
// has had time to read it; a page an earlier flow left (superseded, or the app
// quit first) goes with the next.
void WalletConnect::OpenThroughTrampoline(const std::string& url) {
  const auto fail = [this](const char* what) {
    LogError("wallet bridge: {}", what);
    if (on_error) on_error("failed to open browser");
  };
  const std::string page = bridge::TrampolinePage(url);
  if (page.empty()) return fail("not opening a long bridge url that is not a plain https url");
  wchar_t temp[MAX_PATH + 1]{};
  const DWORD length = ::GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
  if (length == 0 || length >= std::size(temp)) {
    return fail("no temp directory for the bridge page");
  }
  const std::filesystem::path directory(temp);
  WIN32_FIND_DATAW found{};
  HANDLE search = ::FindFirstFileW((directory / bridge::kTrampolinePattern).c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      ::DeleteFileW((directory / found.cFileName).c_str());
    } while (::FindNextFileW(search, &found));
    ::FindClose(search);
  }
  // 64 random bits name it: created new, never an existing file of that name
  std::random_device random;
  wchar_t name[17]{};
  std::swprintf(name, std::size(name), L"%08x%08x", random(), random());
  const std::filesystem::path path =
      directory / (std::wstring(bridge::kTrampolinePrefix) + name + L".html");
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kOwnerOnlyFileSddl, SDDL_REVISION_1, &security.lpSecurityDescriptor, nullptr)) {
    return fail("no security descriptor for the bridge page");
  }
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, &security, CREATE_NEW,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
  ::LocalFree(security.lpSecurityDescriptor);
  if (file == INVALID_HANDLE_VALUE) return fail("could not create the bridge page");
  DWORD written = 0;
  const bool wrote = ::WriteFile(file, page.data(), static_cast<DWORD>(page.size()), &written,
                                 nullptr) &&
                     written == page.size();
  ::CloseHandle(file);
  if (!wrote) {
    ::DeleteFileW(path.c_str());
    return fail("could not write the bridge page");
  }
  // In the browser Windows opens https links with, the one that holds the
  // wallet's extension: what opens .html is often another (Edge, an editor),
  // and there the bridge page finds no Talisman. The page's own association
  // only when Windows names no https program, or it does not start.
  wchar_t browser[MAX_PATH + 1]{};
  DWORD browserSize = static_cast<DWORD>(std::size(browser));
  if (FAILED(::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L"https", L"open", browser,
                                 &browserSize))) {
    browser[0] = L'\0';
  }
  bool opened = false;
  for (const bridge::TrampolineLaunch& launch : bridge::LaunchesFor(browser, path.wstring())) {
    HINSTANCE h = ShellExecuteW(nullptr, L"open", launch.file.c_str(),
                                launch.parameters.empty() ? nullptr : launch.parameters.c_str(),
                                nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) > 32) {
      opened = true;
      break;
    }
  }
  if (!opened) {
    ::DeleteFileW(path.c_str());
    return fail("could not open the bridge page");
  }
  std::thread([path] {
    std::this_thread::sleep_for(bridge::kTrampolineLifetime);
    ::DeleteFileW(path.c_str());
  }).detach();
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
                    "?dapp_encryption_public_key=" + PercentEncode(dappKeyPair_->PublicKeyBase58) +
                    "&cluster=" + kCluster + "&app_url=" + PercentEncode(kAppUrl) +
                    "&redirect_link=" + PercentEncode(redirect) +
                    "&method=connect&provider=" + Host(p);
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
                    "?dapp_encryption_public_key=" + PercentEncode(dappKeyPair_->PublicKeyBase58) +
                    "&cluster=" + kCluster + "&nonce=" + PercentEncode(nonce) +
                    "&redirect_link=" + PercentEncode(redirect) + "&payload=" + PercentEncode(enc) +
                    "&method=signMessage&provider=" + Host(currentProvider_);
  OpenUrl(url);
}

void WalletConnect::OpenBittensorBridge(const std::string& bridgeUrl) {
  // No connect handshake and no encryption envelope: sr25519 signatures are
  // public, and the session already holds everything the url carries.
  connectedPublicKey_.reset();
  walletEncryptionPublicKey_.reset();
  session_.reset();
  currentProvider_ = Provider::Bittensor;
  OpenUrl(bridgeUrl);
}

std::string WalletConnect::OAuthState(const std::string& token) {
  const nlohmann::json claims = {{"platform", kPlatform}, {"token", token}};
  return Base64Url(claims.dump());
}

std::string WalletConnect::AppleOAuthState(const std::string& token) { return OAuthState(token); }

void WalletConnect::OpenGoogleOAuth(const std::string& apiUrl, const std::string& state,
                                    const std::string& nonce) {
  if (apiUrl.empty()) {
    if (on_error) on_error("no api url for the Google sign-in callback");
    return;
  }
  std::string origin = apiUrl;
  while (!origin.empty() && origin.back() == '/') origin.pop_back();
  // the code flow: google only hands the identity token to a server, so the
  // api's callback exchanges the code and redirects it back to this app
  std::string url = std::string(kGoogleAuthorize) +
                    "?client_id=" + PercentEncode(kGoogleClientId) +
                    "&redirect_uri=" + PercentEncode(origin + kGoogleCallbackPath) +
                    "&response_type=code" + "&scope=" + PercentEncode("openid email profile") +
                    "&state=" + PercentEncode(state) + "&nonce=" + PercentEncode(nonce) +
                    "&prompt=select_account";
  OpenUrl(url);
}

void WalletConnect::OpenAppleOAuth(const std::string& apiUrl, const std::string& state,
                                   const std::string& nonce) {
  if (apiUrl.empty()) {
    if (on_error) on_error("no api url for the Apple sign-in callback");
    return;
  }
  std::string origin = apiUrl;
  while (!origin.empty() && origin.back() == '/') origin.pop_back();
  std::string url = std::string(kAppleAuthorize) +
                    "?client_id=" + PercentEncode(kAppleServicesId) +
                    "&redirect_uri=" + PercentEncode(origin + kAppleCallbackPath) +
                    "&response_type=" + PercentEncode("code id_token") +
                    "&response_mode=form_post" + "&scope=" + PercentEncode("name email") +
                    "&state=" + PercentEncode(state) + "&nonce=" + PercentEncode(nonce);
  OpenUrl(url);
}

void WalletConnect::HandleOAuthReturn(const std::string& url) {
  // urnetwork://oauth/apple?state=…&id_token=…  (or &error=…), and the same
  // shape on urnetwork://oauth/google: the path names the provider
  const std::string path = UrlPath(url);
  std::string provider;
  if (path == kAppleReturnPath) {
    provider = "apple";
  } else if (path == kGoogleReturnPath) {
    provider = "google";
  } else {
    if (on_error) on_error("unknown oauth callback");
    return;
  }
  auto q = url.find('?');
  auto params = ParseQueryString(q == std::string::npos ? std::string() : url.substr(q + 1));
  const std::string state = params.count("state") ? params["state"] : std::string();
  const std::string idToken = params.count("id_token") ? params["id_token"] : std::string();
  std::string error = params.count("error") ? params["error"] : std::string();
  if (error.empty() && idToken.empty()) error = "sign-in returned no identity token";
  if (on_sso) on_sso(provider, idToken, state, error);
}

bool WalletConnect::HandleDeepLink(const std::string& url) {
  std::string host, query;
  SplitUrl(url, host, query);
  if (host == kOAuthReturnHost) {
    HandleOAuthReturn(url);
    return true;
  }
  auto provider = ProviderForHost(host);
  if (!provider) return false;
  if (*provider == Provider::Bittensor) {
    // the session checks it (urnet::BittensorWalletSession::handleBridgeReturn)
    if (on_bittensor_return) on_bittensor_return(url);
  } else if (host.find("-connect") != std::string::npos)
    HandleConnect(*provider, query);
  else
    HandleSignMessage(*provider, query);
  return true;
}

void WalletConnect::HandleConnect(Provider p, const std::string& query) {
  auto params = ParseQueryString(query);
  if (params.count("errorCode")) {
    if (on_error) {
      const std::string pageText =
          params.count("errorMessage") ? params["errorMessage"] : "wallet connect error";
      on_error(LocalizedBridgeError(p, params["errorCode"], pageText));
    }
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
  const auto decrypted = TryDecrypt(params["data"], params["nonce"], *sharedB58);
  if (!decrypted || decrypted->empty()) {
    if (on_error) on_error("failed to decrypt wallet connection");
    return;
  }
  try {
    auto j = nlohmann::json::parse(std::string(decrypted->begin(), decrypted->end()));
    connectedPublicKey_ = j.at("public_key").get<std::string>();
    session_ = j.at("session").get<std::string>();
    currentProvider_ = p;
    if (on_public_key) on_public_key(*connectedPublicKey_, p);
  } catch (const std::exception& e) {
    if (on_error) on_error(std::string("bad connect response: ") + e.what());
  }
}

void WalletConnect::HandleSignMessage(Provider p, const std::string& query) {
  auto params = ParseQueryString(query);
  if (params.count("errorCode")) {
    if (on_error) {
      const std::string pageText =
          params.count("errorMessage") ? params["errorMessage"] : "wallet signing error";
      on_error(LocalizedBridgeError(p, params["errorCode"], pageText));
    }
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
  const auto decrypted = TryDecrypt(params["data"], params["nonce"], *sharedB58);
  if (!decrypted || decrypted->empty()) {
    if (on_error) on_error("failed to decrypt wallet signature");
    return;
  }
  try {
    auto j = nlohmann::json::parse(std::string(decrypted->begin(), decrypted->end()));
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

}  // namespace urnw
