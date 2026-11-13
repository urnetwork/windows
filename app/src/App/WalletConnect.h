// Wallet connect via the ur.io/wallet-connect browser bridge — the Windows peer
// of the Linux WalletConnect and the macOS ConnectWalletProviderViewModel.
// Desktop wallets are browser extensions, so we open
// https://ur.io/wallet-connect?... in the default browser; it drives the wallet
// and returns via the urnetwork:// scheme.
//
// Two providers, two envelopes:
//   - Solana (Phantom / Solflare): connect -> signMessage, carrying the same
//     NaCl-box envelope the SDK decodes. Crypto is the SDK's
//     (generateWalletKeyPair / generateSharedSecret / encrypt|decryptData /
//     base58), so it is wire-compatible with the Apple CryptoKit path and the
//     Linux app.
//   - Bittensor (Talisman, TAO.com, WalletConnect): a single signMessage step
//     whose url and hand-back are the SDK's (urnet::BittensorWalletSession).
//     This class only opens the url and hands the raw
//     urnetwork://bittensor-sign-message link back to the session; it parses
//     nothing.
//
// First-principles: no macOS mobile-deeplink baggage — the desktop path is the
// only path here.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <optional>
#include <string>

#include "Sdk.h"

namespace urnw {

class WalletConnect {
 public:
  enum class Provider { Phantom, Solflare, Bittensor };

  // Open the browser to connect a Solana wallet. on_public_key fires on the
  // urnetwork://<provider>-connect callback (routed in via HandleDeepLink).
  void Connect(Provider p);

  // After a successful Connect, ask the Solana wallet to sign `message`.
  // on_signature fires on the urnetwork://<provider>-sign-message callback.
  void SignMessage(const std::string& message);

  // Bittensor: open the session's bridge url (BittensorWalletSession::
  // bridgeUrl). The page drives the wallet and returns on
  // urnetwork://bittensor-sign-message, which on_bittensor_return receives
  // whole for the session to check.
  void OpenBittensorBridge(const std::string& bridgeUrl);

  // Sign in with Apple straight against Apple: Apple has no desktop SDK, so
  // the browser opens Apple's authorize page (client_id = the Apple Services
  // ID, redirect_uri = <api>/auth/apple/callback), Apple posts the result to
  // the api, and the api redirects to urnetwork://oauth/apple?state=…
  // &id_token=… (or &error=…), which HandleDeepLink routes to on_sso with
  // provider "apple"; the host checks the echoed state and the token's nonce.
  // `state` is echoed back untouched and `nonce` is handed to the provider so
  // the token carries it. The api picks the urnetwork:// scheme from the
  // `platform` claim inside `state` (AppleOAuthState); the state is otherwise
  // opaque.
  void OpenAppleOAuth(const std::string& apiUrl, const std::string& state,
                      const std::string& nonce);
  // Sign in with Google the same way: the browser opens
  // Google's authorize page (client_id = the ur.io web sign-in client,
  // redirect_uri = <api>/auth/google/callback, response_type=code), Google
  // redirects to the api with an authorization code, the api exchanges it for
  // the identity token and redirects to urnetwork://oauth/google?state=…
  // &id_token=… (or &error=…), which HandleDeepLink routes to on_sso with
  // provider "google". The state carries the same platform claim.
  void OpenGoogleOAuth(const std::string& apiUrl, const std::string& state,
                       const std::string& nonce);
  // The state of one Apple or Google attempt: base64url of
  // {"platform":"windows","token":…}. The api's callbacks read the platform
  // claim to pick the return scheme; the token is what makes it unique.
  static std::string AppleOAuthState(const std::string& token);
  static std::string OAuthState(const std::string& token);

  // Route a urnetwork:// callback here. Returns true if it was a wallet or an
  // oauth sign-in callback.
  bool HandleDeepLink(const std::string& url);

  bool connected() const { return connectedPublicKey_.has_value(); }

  std::function<void(std::string publicKey, Provider)> on_public_key;
  // publicKey is the wallet address (base58 for Solana, ss58 for Bittensor) and
  // signature is what the server verifies for that chain: base64 ed25519 for
  // Solana, hex sr25519 for Bittensor.
  std::function<void(std::string publicKey, std::string signature, Provider)> on_signature;
  std::function<void(std::string error)> on_error;
  // the raw urnetwork://bittensor-* hand-back link
  std::function<void(std::string url)> on_bittensor_return;
  // urnetwork://oauth/<provider>?state=<state>&id_token=<token>, or
  // ?state=<state>&error=<message>. `error` is non-empty when the api's
  // callback reported one or returned no token.
  std::function<void(std::string provider, std::string authJwt, std::string state,
                     std::string error)>
      on_sso;

 private:
  static const char* Host(Provider p);
  static std::optional<Provider> ProviderForHost(const std::string& host);
  bool NewKeyPair();
  std::optional<std::string> SharedSecretBase58() const;
  // in the browser: as it is, or through a page of our own when it is longer
  // than ShellExecuteW passes on (BridgeTrampoline.h)
  void OpenUrl(const std::string& url);
  void OpenThroughTrampoline(const std::string& url);
  void HandleConnect(Provider p, const std::string& query);
  void HandleSignMessage(Provider p, const std::string& query);
  // urnetwork://oauth/<apple|google>?state=…&id_token=… (or &error=…)
  void HandleOAuthReturn(const std::string& url);

  std::optional<urnet::WalletKeyPair> dappKeyPair_;
  std::optional<std::string> connectedPublicKey_;
  std::optional<std::string> walletEncryptionPublicKey_;
  std::optional<std::string> session_;
  Provider currentProvider_ = Provider::Phantom;
};

}  // namespace urnw
