// Build-time app configuration. Identity constants (service name, tray GUID,
// uri scheme) live in Common/Ids.h and must stay stable across releases; this is
// for values that vary by build or deployment.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

namespace urnw::config {

// WalletConnect Cloud project id — one project id shared by every URnetwork
// client (see apple/NEXTSTEPS2.md). It is passed to the ur.io/wallet-connect
// bridge as `wc_project_id` so the bridge can pair with a wallet app (QR /
// mobile deep link).
//
// Empty is fine and is the common desktop case: the bridge then drives injected
// (browser-extension) wallets only — Bittensor Wallet, SubWallet, Talisman,
// polkadot-js for Bittensor; Phantom/Solflare for Solana. Nothing crashes and no
// button dies; only mobile wallet pairing is lost.
//
// Define URN_WALLETCONNECT_PROJECT_ID on the build (CI / build machine, the way
// android takes it from local.properties) to inject the value without
// committing it:
//   msbuild ... /p:UrnWalletConnectProjectId=<project id>
#if defined(URN_WALLETCONNECT_PROJECT_ID)
inline constexpr const char* kWalletConnectProjectId = URN_WALLETCONNECT_PROJECT_ID;
#else
inline constexpr const char* kWalletConnectProjectId = "";
#endif

}  // namespace urnw::config
