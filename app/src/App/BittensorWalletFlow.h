// The app-side decisions of a Bittensor wallet proof. The protocol itself is
// the SDK's session helper (urnet::BittensorWalletSession, sdk
// bittensor_wallet.go): what is signed, the bridge url, and whether a
// hand-back is acceptable. What stays here is what only the app can decide:
// which wallet the chooser picked, what the user does next (switch to the
// browser), which hand-backs this flow should ignore, and the localized text for
// each refusal code.
//
// The supported wallets are Talisman, TAO.com and WalletConnect (UPGRADE.md
// 4.6), all browser_bridge on Windows: the system browser opens the ur.io page,
// which drives the wallet's extension (Talisman, TAO.com) or pairs a
// WalletConnect wallet (Nova, Nightly, ...: a QR to scan) and returns on the
// existing urnetwork://bittensor-sign-message protocol handler (installer
// Package.wxs). Signing by hand, with btcli or a wallet's own tool, happens on
// the same page ("Enter address manually"): the page checks the paste and
// returns it like any other proof. The app has no form of its own for it.
//
// Pure and header-only, so app/tools/bittensor-wallet-tests.cpp runs it
// without WinRT or the SDK. The string constants mirror the SDK's
// (urnet::BittensorWallet*); tests/bittensor_wallet_test.go reads the
// SdkHost sources to keep the two tied together.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>

namespace urnw::bittensor {

inline constexpr std::string_view kPlatform = "windows";
// the hand-back link the installer registers (urnetwork://) and the bridge
// already returned on; owned by this app, not by the sdk
inline constexpr std::string_view kRedirectLink = "urnetwork://bittensor-sign-message";

inline constexpr std::string_view kWalletTalisman = "talisman";
inline constexpr std::string_view kWalletTaoCom = "taocom";
inline constexpr std::string_view kWalletWalletConnect = "walletconnect";

// The chooser's entries, in order (the sdk's BittensorWalletIdList).
inline constexpr std::string_view kChooserWallets[] = {kWalletTalisman, kWalletTaoCom,
                                                       kWalletWalletConnect};
inline constexpr int kChooserWalletCount = 3;

inline constexpr std::string_view kTransportBrowserBridge = "browser_bridge";

inline constexpr std::string_view kPurposeLogin = "login";
inline constexpr std::string_view kPurposeCreate = "create";
inline constexpr std::string_view kPurposeConnect = "connect";
// adding the wallet as a sign-in method (Settings' add sheet); a login
// session refuses its hand-back, so it can never sign in
inline constexpr std::string_view kPurposeAdd = "add";
// the network payout consent (Earnings, SnConsentPresentation.h): the session
// takes the server's consent message instead of fetching a challenge
// (setNetworkConsentChallenge), and every other purpose refuses its hand-back
inline constexpr std::string_view kPurposeConsent = "consent";

// The chooser lists kChooserWallets as buttons; the picked index (or -1 for
// cancel) maps to a wallet id ("" = cancelled).
inline std::string WalletForChoice(int index) {
  if (index < 0 || index >= kChooserWalletCount) return std::string();
  return std::string(kChooserWallets[index]);
}

// The localization key of the line under a chooser entry ("" = none):
// WalletConnect says which wallets it reaches.
inline std::string ChooserHintKey(std::string_view walletId) {
  if (walletId == kWalletWalletConnect) return "bittensor_walletconnect_hint";
  return std::string();
}

// The status line while the browser is open: Talisman and TAO.com ask for the
// extension's approval (bittensor_continue_in_browser, with the wallet name),
// WalletConnect for a scan or the wallet app (bittensor_walletconnect_continue).
// "" for a wallet this app does not know.
inline std::string BrowserHintKey(std::string_view walletId) {
  if (walletId == kWalletTalisman || walletId == kWalletTaoCom) return "bittensor_continue_in_browser";
  if (walletId == kWalletWalletConnect) return "bittensor_walletconnect_continue";
  return std::string();
}

// Only the WalletConnect page needs the app's WalletConnect project id.
inline bool NeedsWalletConnectProjectId(std::string_view walletId) {
  return walletId == kWalletWalletConnect;
}

// What the app does once the challenge is in hand.
enum class NextStep { Unsupported, OpenBrowser };

inline NextStep NextStepFor(std::string_view transport) {
  if (transport == kTransportBrowserBridge) return NextStep::OpenBrowser;
  // "extension" is the web's, and "manual" a phone's or ur.io's: a desktop app
  // neither drives an extension itself nor takes a pasted signature. The proof
  // is answered with a failure.
  return NextStep::Unsupported;
}

// A refusal that is not this flow's answer: a hand-back for another purpose
// (another flow's tab), a link that is not a Bittensor hand-back, or one that
// arrives when nothing waits (the bridge page's "Return to URnetwork" after
// its automatic redirect, a replay). The flow keeps waiting.
inline bool IsForeignReturn(std::string_view errorCode) {
  return errorCode == "purpose_mismatch" || errorCode == "not_bittensor_return" ||
         errorCode == "not_awaiting_wallet";
}

// The localization key for a refusal code. Empty for "wallet_error": the
// failure the bridge page handed back, shown in this app's words for the
// page's code (BridgeErrorTextFor), or as the page's own message, falling
// back to `fallbackKey` when it is empty.
inline std::string ErrorKey(std::string_view errorCode, std::string_view fallbackKey) {
  if (errorCode == "invalid_signature") return "bittensor_error_invalid_signature";
  if (errorCode == "challenge_expired") return "bittensor_error_challenge_expired";
  if (errorCode == "message_mismatch") return "bittensor_error_message_mismatch";
  if (errorCode == "address_mismatch") return "earnings_wallet_mismatch";
  if (errorCode == "invalid_ss58_address") return "invalid_ss58_address";
  if (errorCode == "wallet_error") return std::string();
  return std::string(fallbackKey);
}

// What this app says for the bridge page's own code for a "wallet_error"
// (the sdk's BittensorWalletResult::BridgeErrorCode, one of
// urnet::BittensorWalletBridgeError*): a store key, and whether it takes the
// wallet's product name ("{}"). An empty key for a code this app does not know
// (or none, from a page before the codes): the page's own text is shown then.
struct BridgeErrorText {
  std::string key;
  bool takesWalletName = false;
};

inline BridgeErrorText BridgeErrorTextFor(std::string_view bridgeCode) {
  if (bridgeCode == "address_not_in_wallet") return {"bittensor_error_address_not_in_wallet", true};
  if (bridgeCode == "address_mismatch") return {"earnings_wallet_mismatch", false};
  if (bridgeCode == "extension_not_found") return {"bittensor_error_extension_not_found", true};
  if (bridgeCode == "no_account") return {"bittensor_error_no_account", true};
  if (bridgeCode == "extension_not_authorized") return {"bittensor_error_extension_not_authorized", true};
  if (bridgeCode == "user_rejected") return {"bittensor_error_user_rejected", false};
  if (bridgeCode == "walletconnect_expired") return {"bittensor_error_walletconnect_expired", false};
  if (bridgeCode == "walletconnect_unavailable") return {"bittensor_error_walletconnect_unavailable", false};
  return {};
}

}  // namespace urnw::bittensor
