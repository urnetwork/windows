// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose -- see SolanaWalletPresentation.h. App.vcxproj compiles
// this with PrecompiledHeader=NotUsing, like ExtenderPresentation.cpp.
#include "SolanaWalletPresentation.h"

#include <iterator>

namespace urnw::solana {

FailureText FailureTextFor(std::string_view messageKey, std::string_view walletName) {
  struct Message {
    std::string_view key;
    bool takesWalletName = false;
  };
  // The sdk's seven messages (urnet::SolanaWalletFailureMessage*), each with
  // whether its string takes the wallet's name. The generic wallet failure is
  // last: it is also the message for a key this build does not know.
  static constexpr Message kMessages[] = {
      {.key = "bittensor_error_user_rejected", .takesWalletName = false},
      {.key = "no_wallets_found_alert_content", .takesWalletName = false},
      {.key = "couldnt_open_wallet", .takesWalletName = false},
      {.key = "bittensor_error_extension_not_found", .takesWalletName = true},
      {.key = "bittensor_error_no_account", .takesWalletName = true},
      {.key = "solana_wallet_error_session_not_found", .takesWalletName = false},
      {.key = "bittensor_error_wallet_failed", .takesWalletName = true},
  };
  Message message = kMessages[std::size(kMessages) - 1];
  for (const Message& known : kMessages) {
    if (known.key == messageKey) message = known;
  }
  FailureText text{.key = std::string(message.key), .walletName = std::nullopt};
  if (message.takesWalletName) {
    text.walletName = std::string(walletName.empty() ? kUnknownWalletName : walletName);
  }
  return text;
}

std::string PageTextLogLine(std::string_view pageText) {
  std::string line;
  for (const char c : pageText.substr(0, kPageTextLogBytes)) {
    line.push_back(0x20 <= c && c <= 0x7e ? c : '?');
  }
  if (pageText.size() > kPageTextLogBytes) line += "...";
  return line;
}

}  // namespace urnw::solana
