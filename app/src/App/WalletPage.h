// The Wallet destination — the payout wallets, the account's points, the
// network-reliability window, the payouts ledger and the earning multiplier —
// and the Leaderboard destination, which the phase plan groups with it (payouts
// / points / leaderboard are one Class-A surface).
//
// Everything here is an in-process `Api` call against the NetworkSpace SdkHost
// already owns. The markup carries only the static structure (MainWindow.xaml,
// the Wallet and Leaderboard ScrollViewers); every list, table and card below is
// built into a named empty panel by this unit.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "SdkHost.h"
#include "UrComponents.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class WalletDetailSheet;
class PayoutDetailSheet;

// The points a payment (or the whole account) earned, split by the four
// account-point events the server emits. iOS AccountPointsStore parity;
// `point_value` is nano-points, so every field here is already converted.
struct PointsBreakdown {
  double net = 0;
  double payout = 0;
  double referral = 0;     // payout_linked_account
  double multiplier = 0;   // payout_multiplier
  double reliability = 0;  // payout_reliability
};

class WalletPage {
 public:
  explicit WalletPage(winrt::URnetwork::implementation::MainWindow& window);
  ~WalletPage();

  void Initialize();  // the address-validation debounce timer
  void ApplyStrings();

  // Every wallet-destination fetch: wallets, payout wallet, transfer stats,
  // wallet balance, referrals, points, reliability and payments. Each settles
  // its own panel independently, so one failing endpoint does not blank the
  // others.
  void LoadWallet();
  void LoadLeaderboard();

  // --preview-ui only (Startup.h): raise the connect-wallet snackbar so the
  // component can be seen without an account. Same call, same store keys as
  // the real path; the ERROR severity, which is the one that must persist.
  void ShowPreviewSnackbar();
  // --preview-ui only: the API loads are skipped with no session, so settle
  // every panel on its empty state instead of leaving them on "Loading...",
  // which is indistinguishable from a hang.
  void ShowPreviewWalletState();
  void ShowPreviewLeaderboardState();

  void OnWalletAddressChanged(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
  void OnConnectWallet(winrt::Windows::Foundation::IInspectable const&,
                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  // A coroutine: the wallet picker is a ContentDialog, and the browser-bridge
  // signature that follows it is asynchronous. MainWindow's forwarder ignores
  // the fire_and_forget, which is what a XAML Click handler needs.
  winrt::fire_and_forget OnVerifySeeker(
      winrt::Windows::Foundation::IInspectable const&,
      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnLeaderboardPublicToggled(winrt::Windows::Foundation::IInspectable const&,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

  // Called by the wallet-detail sheet once the payout wallet or the wallet list
  // changed under it.
  void RefreshAfterWalletChange();

 private:
  // A panel's fetch state. Every list on this destination renders exactly one
  // of these, because "nothing on screen" must never be able to mean three
  // different things at once (loading / empty / the request failed).
  enum class Fetch { Loading, Ready, Failed };

  // ---- wallets / payout wallet ----
  void ApplyWallets(std::vector<urnet::AccountWallet> const& wallets, Fetch state);
  void ApplyPayoutWalletId(std::string const& walletId);
  void RebuildWalletCards();
  winrt::Microsoft::UI::Xaml::UIElement BuildWalletCard(urnet::AccountWallet const& wallet);
  winrt::fire_and_forget ShowWalletDetail(urnet::AccountWallet wallet);

  // ---- header stats ----
  void ApplyTransferStats(int64_t unpaidBytes, bool ok);
  void ApplyWalletBalance(int64_t balanceUsdcNanoCents, bool ok);
  void ApplyReferrals(int64_t totalReferrals, bool ok);

  // ---- points ----
  void ApplyPoints(std::vector<urnet::AccountPoint> const& points, Fetch state);
  void RebuildPointsCard();

  // ---- payouts ----
  void ApplyPayments(std::vector<urnet::AccountPayment> const& payments, Fetch state);
  void RebuildPayouts();
  winrt::fire_and_forget ShowPayoutDetail(urnet::AccountPayment payment);

  // ---- reliability ----
  void ApplyReliability(std::optional<urnet::ReliabilityWindow> window, Fetch state);

  // ---- earning multiplier (Seeker) ----
  void ApplySeekerState();
  void ApplySeekerResult(bool ok, std::string const& serverError);

  // ---- connect wallet (external, by address) ----
  void ValidateWalletAddress();  // debounced; the server validates per chain
  void ApplyWalletValidation(std::string const& chain, uint32_t generation, bool valid);
  // `serverError` is the api's own (unlocalizable) message, empty when there is none
  void ApplyWalletConnectResult(bool ok, std::string const& serverError);

  // ---- leaderboard ----
  void ApplyLeaderboard(urnet::LeaderboardEarnersList const& earners, Fetch state);
  void ApplyRanking(urnet::NetworkRanking const& ranking, bool ok);
  void ApplyRankingPublicResult(bool ok, bool requested, std::string const& serverError);
  // Write the switch without the Toggled handler firing a request back at the
  // server — the handler cannot tell a user flip from a programmatic one.
  void SetRankingToggle(bool isPublic);

  // The points a single payment earned, from the already-loaded account points.
  PointsBreakdown BreakdownForPayment(std::string const& paymentId) const;
  // Total completed USDC paid into one wallet (iOS totalPaymentsByWalletId).
  double TotalPaidToWallet(std::string const& walletId) const;

  winrt::URnetwork::implementation::MainWindow& w_;
  // "Wallet connected" / the server's refusal: a transient message, so it
  // dismisses itself (iOS UrSnackBar). The bar used to stay open forever.
  urnw::kit::Snackbar snackbar_;

  // ---- loaded state (UI thread only) ----
  std::vector<urnet::AccountWallet> wallets_;
  std::string payoutWalletId_;
  std::vector<urnet::AccountPayment> payments_;
  std::vector<urnet::AccountPoint> points_;
  PointsBreakdown accountPoints_;
  std::optional<urnet::ReliabilityWindow> reliability_;
  bool seekerHolder_ = false;   // any account wallet carries the Seeker token
  bool verifyingSeeker_ = false;
  // the signed-in network, for highlighting our own leaderboard row
  std::string ownNetworkId_;

  // leaderboard ranking + its switch
  int64_t leaderboardRank_ = 0;
  bool rankingPublic_ = false;
  bool applyingRankingToggle_ = false;  // a programmatic write, not a user flip
  bool settingRankingPublic_ = false;

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

  // the open detail sheet, held for the life of its ShowAsync
  std::shared_ptr<WalletDetailSheet> walletSheet_;
  std::shared_ptr<PayoutDetailSheet> payoutSheet_;
};

}  // namespace urnw
