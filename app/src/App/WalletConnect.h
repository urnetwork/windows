// Solana wallet connect (Phantom / Solflare) via the ur.io/wallet-connect browser
// bridge — the Windows peer of the Linux WalletConnect and the macOS
// ConnectWalletProviderViewModel. Desktop wallets are browser extensions, so we
// open https://ur.io/wallet-connect?... in the default browser; it drives the
// extension and returns via the urnetwork:// scheme carrying the same NaCl-box
// envelope the SDK decodes. Crypto is the SDK's (generateWalletKeyPair /
// generateSharedSecret / encrypt|decryptData / base58), so it is wire-compatible
// with the Apple CryptoKit path and the Linux app. First-principles: no macOS
// mobile-deeplink baggage — the desktop path is the only path here.
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
  enum class Provider { Phantom, Solflare };

  // Open the browser to connect the wallet. on_public_key fires on the
  // urnetwork://<provider>-connect callback (routed in via HandleDeepLink).
  void Connect(Provider p);

  // After a successful Connect, ask the wallet to sign `message`. on_signature
  // fires (base64) on the urnetwork://<provider>-sign-message callback.
  void SignMessage(const std::string& message);

  // Route a urnetwork:// callback here. Returns true if it was a wallet callback.
  bool HandleDeepLink(const std::string& url);

  bool connected() const { return connectedPublicKey_.has_value(); }

  std::function<void(std::string publicKey, Provider)> on_public_key;
  std::function<void(std::string base64Signature)> on_signature;
  std::function<void(std::string error)> on_error;

 private:
  static const char* Host(Provider p);
  static std::optional<Provider> ProviderForHost(const std::string& host);
  bool NewKeyPair();
  std::optional<std::string> SharedSecretBase58() const;
  void OpenUrl(const std::string& url);
  void HandleConnect(Provider p, const std::string& query);
  void HandleSignMessage(Provider p, const std::string& query);

  std::optional<urnet::WalletKeyPair> dappKeyPair_;
  std::optional<std::string> connectedPublicKey_;
  std::optional<std::string> walletEncryptionPublicKey_;
  std::optional<std::string> session_;
  Provider currentProvider_ = Provider::Phantom;
};

}  // namespace urnw
