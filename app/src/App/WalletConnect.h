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
//   - Bittensor (any substrate wallet): a single signMessage step. sr25519
//     signatures are public, so the bridge returns plain query params (the ss58
//     address + the hex signature) with no encryption envelope, and there is no
//     connect handshake to keep state for.
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

  // Bittensor: no connect handshake — the bridge signs `message` with an
  // injected substrate wallet (or a WalletConnect pairing, when a project id is
  // configured) and returns the address and signature together on the
  // urnetwork://bittensor-sign-message callback.
  void SignMessageBittensor(const std::string& message);

  // Route a urnetwork:// callback here. Returns true if it was a wallet callback.
  bool HandleDeepLink(const std::string& url);

  bool connected() const { return connectedPublicKey_.has_value(); }

  std::function<void(std::string publicKey, Provider)> on_public_key;
  // publicKey is the wallet address (base58 for Solana, ss58 for Bittensor) and
  // signature is what the server verifies for that chain: base64 ed25519 for
  // Solana, hex sr25519 for Bittensor.
  std::function<void(std::string publicKey, std::string signature, Provider)> on_signature;
  std::function<void(std::string error)> on_error;

 private:
  static const char* Host(Provider p);
  static std::optional<Provider> ProviderForHost(const std::string& host);
  bool NewKeyPair();
  std::optional<std::string> SharedSecretBase58() const;
  void OpenUrl(const std::string& url);
  void HandleConnect(Provider p, const std::string& query);
  void HandleSignMessage(Provider p, const std::string& query);
  void HandleBittensor(const std::string& host, const std::string& query);

  std::optional<urnet::WalletKeyPair> dappKeyPair_;
  std::optional<std::string> connectedPublicKey_;
  std::optional<std::string> walletEncryptionPublicKey_;
  std::optional<std::string> session_;
  Provider currentProvider_ = Provider::Phantom;
};

}  // namespace urnw
