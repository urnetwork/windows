// The Wallet destination — payout wallets, connecting an external wallet by
// address — and the Leaderboard destination, which the phase plan groups with
// it (payouts / points / leaderboard are one Class-A surface).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "SdkHost.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class WalletPage {
 public:
  explicit WalletPage(winrt::URnetwork::implementation::MainWindow& window);
  ~WalletPage();

  void Initialize();  // the address-validation debounce timer
  void ApplyStrings();

  void LoadWallet();
  void LoadLeaderboard();

  void OnWalletAddressChanged(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnConnectWallet(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  void ApplyWallets(urnet::AccountWalletsList const& wallets);
  void ValidateWalletAddress();  // debounced; the server validates per chain
  void ApplyWalletValidation(std::string const& chain, uint32_t generation, bool valid);
  // `serverError` is the api's own (unlocalizable) message, empty when there is none
  void ApplyWalletConnectResult(bool ok, std::string const& serverError);

  winrt::URnetwork::implementation::MainWindow& w_;

  // connect-wallet state (UI thread only). The address is validated against each
  // supported chain; the generation drops results from a superseded edit.
  struct WalletValidation {
    bool sol = false;
    bool matic = false;
    bool tao = false;
  };
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer walletValidateTimer_{nullptr};
  WalletValidation walletValidation_;
  uint32_t walletValidateGeneration_ = 0;
  std::string walletChain_;          // the chain that accepted the address ("" = none)
  bool connectingWallet_ = false;
};

}  // namespace urnw
