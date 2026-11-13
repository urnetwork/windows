// SPDX-License-Identifier: MPL-2.0
//
// No pch.h on purpose -- see SolanaWalletPresentation.h. App.vcxproj compiles
// this with PrecompiledHeader=NotUsing, like ExtenderPresentation.cpp.
#include "SolanaWalletPresentation.h"

namespace urnw::solana {

BridgeErrorText BridgeErrorTextFor(std::string_view code) {
  if (code == "extension_not_found") return {"bittensor_error_extension_not_found", true};
  if (code == "no_account") return {"bittensor_error_no_account", true};
  if (code == "session_not_found") return {"solana_wallet_error_session_not_found", false};
  if (code == "user_rejected") return {"bittensor_error_user_rejected", false};
  return {};
}

}  // namespace urnw::solana
