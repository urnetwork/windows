// The words for the Solana wallet bridge page's own failure codes. Signing in
// with Phantom or Solflare, and adding either as a sign-in method, go through
// the ur.io wallet bridge (WalletConnect.cpp), which hands a failure back with
// a stable code (the SDK's urnet::SolanaWalletBridgeError*) next to its
// English text, on the connect or the sign step. The codes it shares with the
// Bittensor bridge page mean the same, so they read in the same strings, with
// Phantom or Solflare as the wallet's name.
//
// It is pure, for the reason ExtenderPresentation.h gives: the windows
// solution has no test project and a WinUI 3 app cannot be built off Windows,
// so tools/solana-wallet-tests.cpp verifies it on any host with a C++20
// compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace urnw::solana {

// What this app says for a code: the store key, and whether it takes the
// wallet's product name ("{}"). An empty key for any other code (the page's
// invalid_request and wallet_error, a code this app does not know, the -1 of
// pages before the codes): the page's own text is shown then.
struct BridgeErrorText {
  std::string key;
  bool takesWalletName = false;
};

BridgeErrorText BridgeErrorTextFor(std::string_view code);

}  // namespace urnw::solana
