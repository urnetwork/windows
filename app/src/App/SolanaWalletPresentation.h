// The words for a failed Solana wallet sign-in. Signing in with Phantom or
// Solflare, and adding either as a sign-in method, go through the ur.io wallet
// bridge (WalletConnect.cpp). The sdk reads each of their failures, a code the
// bridge page hands back or one the app finds itself, as one of its failures
// (urnet::solanaWalletFailureFor) and names the store key of the message every
// app shows for it, with the wallet's name when the message takes one. This is
// the one table that shows those messages, so a failure reads the same here as
// on every other app, and the page's own English is never shown: it is only
// logged, as one bounded line.
//
// It is pure, for the reason ExtenderPresentation.h gives: the windows
// solution has no test project and a WinUI 3 app cannot be built off Windows,
// so tools/solana-wallet-tests.cpp verifies it on any host with a C++20
// compiler.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace urnw::solana {

// The wallet a message names when the sdk named none: the sdk's own word for
// a wallet it does not know.
inline constexpr std::string_view kUnknownWalletName = "Solana";

// The most of the bridge page's own English that its log line keeps, in bytes.
// The page's messages are a sentence; the cap is for a link written to fill
// the log.
inline constexpr std::size_t kPageTextLogBytes = 200;

// A message to show: its store key, and the wallet's name for its "{}", set
// exactly when the message takes one.
struct FailureText {
  std::string key;
  std::optional<std::string> walletName;
};

// The message for an sdk failure's `messageKey` (urnet::SolanaWalletFailure
// message_key, one of the sdk's seven), with its `walletName` (wallet_name, ""
// when unset). A key this build does not know, and "" for no failure at all,
// is the generic wallet failure, bittensor_error_wallet_failed. A message that
// takes the wallet's name and got none names kUnknownWalletName; one that takes
// none drops a name it got.
FailureText FailureTextFor(std::string_view messageKey, std::string_view walletName);

// The bridge page's own English as one log line: each byte outside printable
// ASCII reads as '?', so no line break or control character reaches the log,
// and text past kPageTextLogBytes is cut, ending in "...". It came off a link
// any program can fire at the app.
std::string PageTextLogLine(std::string_view pageText);

}  // namespace urnw::solana
