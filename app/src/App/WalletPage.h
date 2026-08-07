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
#include "UrComponents.h"

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

  // --preview-ui only (Startup.h): raise the connect-wallet snackbar so the
  // component can be seen without an account. Same call, same store keys as
  // the real path; the ERROR severity, which is the one that must persist.
  void ShowPreviewSnackbar();
  // --preview-ui only: the leaderboard fetch is skipped with the other API
  // loads, so settle its panel on the empty state instead of leaving it on
  // "Loading..." — which is indistinguishable from a hang.
  void ShowPreviewLeaderboardState();

  void OnWalletAddressChanged(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnConnectWallet(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  // loading / empty / failed, in one place
  void ApplyLeaderboard(urnet::LeaderboardEarnersList const& earners, bool ok);
  void ApplyWallets(urnet::AccountWalletsList const& wallets);
  void ValidateWalletAddress();  // debounced; the server validates per chain
  void ApplyWalletValidation(std::string const& chain, uint32_t generation, bool valid);
  // `serverError` is the api's own (unlocalizable) message, empty when there is none
  void ApplyWalletConnectResult(bool ok, std::string const& serverError);

  winrt::URnetwork::implementation::MainWindow& w_;
  // "Wallet connected" / the server's refusal: a transient message, so it
  // dismisses itself (iOS UrSnackBar). The bar used to stay open forever.
  urnw::kit::Snackbar snackbar_;

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
