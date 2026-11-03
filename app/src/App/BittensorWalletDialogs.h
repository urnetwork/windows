// The Bittensor wallet dialogs: the chooser (Talisman / TAO.com /
// WalletConnect) in front of every Bittensor flow, and the coldkey the payout
// consent names. The protocol behind them is the SDK session helper, run by
// SdkHost; these only collect the user's answers. All run on the UI thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <string>

#include <winrt/Windows.Foundation.h>

#include "SdkHost.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw::pages {

// Change wallet's offer of a key on this device, above the wallets (the
// one-tap key's mocks, 3b): what deleting the old wallet's place means, the
// one-tap button and the sdk's disclaimer for it. Empty (no createKey) where
// the sheet offers wallets only, as Use a wallet I already have does.
struct WalletKeyOffer {
  // the sdk's disclaimer for the offer (FundsDisclaimerAt("change_wallet"))
  std::string disclaimer;
  // Create a key on this device: runs once the sheet has closed, so its own
  // warning can show
  std::function<void()> createKey;
};

// `next` gets "talisman", "taocom", "walletconnect", or "" when the user
// cancelled or chose the key offer's Create a key on this device, which runs
// keyOffer.createKey instead. Does nothing (and calls nothing) while another
// ContentDialog is open.
winrt::fire_and_forget ChooseBittensorWallet(
    winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window,
    std::function<void(std::string walletId)> next, WalletKeyOffer keyOffer = {});

// The coldkey the payout consent will name, typed by the user and never
// prefilled: the payout coldkey is a deliberate choice, and may be another
// account than the one that signs in. Continue takes it only once the SDK's
// ss58 rule passes (snconsent::ColdkeyFrom with urnet::validateSs58), before
// any request, and stays open on an invalid one. `next` gets the coldkey, or ""
// when the user cancelled. Does nothing (and calls nothing) while another
// ContentDialog is open.
winrt::fire_and_forget EnterConsentColdkey(
    winrt::com_ptr<winrt::URnetwork::implementation::MainWindow> window,
    std::function<void(std::string coldkeySs58)> next);

}  // namespace urnw::pages
