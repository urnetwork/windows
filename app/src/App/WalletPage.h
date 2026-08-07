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
#include <functional>
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
  // MAY THIS SURFACE TALK TO THE SERVER AT ALL?
  //
  // Guarding the LOAD paths is not enough and never was. MainWindow's
  // navigation relay skips the per-destination loads under --preview-ui, and
  // that is where the guarding stopped — so every ACTION this destination
  // offers went straight out to the api with no session: a sample wallet card
  // opened a real sheet whose Remove called removeWallet, the leaderboard
  // switch called setNetworkLeaderboardPublic on the first click with no env
  // var at all, and typing 32 characters into the address field fired three
  // walletValidateAddress calls. Reproduced by watching this process's own
  // sockets: an ESTABLISHED TLS connection to the api and a 401 in the log,
  // from a build with no account.
  //
  // Worse than a wasted 401 now: a machine that HAS a stored session (this
  // client finally restores it, see SdkHost::Initialize) would have run those
  // writes AUTHENTICATED, against the real account, from a preview switch.
  //
  // So every write, and every question asked of the server, goes through here:
  // not previewing, the api exists, and there is a session behind it.
  bool CanCallApi() const;
  // The refusal a guarded action shows. Never silent: an affordance that does
  // nothing and says nothing is indistinguishable from a hang.
  void RefuseNoSession();

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
  void ApplySeekerResult(uint32_t generation, bool ok, std::string const& serverError);

  // ---- connect wallet (external, by address) ----
  void ValidateWalletAddress();  // debounced; the server validates per chain
  void ApplyWalletValidation(std::string const& chain, uint32_t generation, bool valid);
  // `serverError` is the api's own (unlocalizable) message, empty when there is none
  void ApplyWalletConnectResult(uint32_t generation, bool ok, std::string const& serverError);

  // ---- leaderboard ----
  void ApplyLeaderboard(urnet::LeaderboardEarnersList const& earners, Fetch state);
  void ApplyRanking(urnet::NetworkRanking const& ranking, bool ok);
  void ApplyRankingPublicResult(uint32_t generation, bool ok, bool requested,
                                std::string const& serverError);
  // Write the switch without the Toggled handler firing a request back at the
  // server — the handler cannot tell a user flip from a programmatic one.
  void SetRankingToggle(bool isPublic);

  // The points a single payment earned, from the already-loaded account points.
  PointsBreakdown BreakdownForPayment(std::string const& paymentId) const;
  // Total completed USDC paid into one wallet (iOS totalPaymentsByWalletId).
  double TotalPaidToWallet(std::string const& walletId) const;

  // A request that can never answer must not be able to leave a control dead.
  //
  // The three flows below reach the ur.io browser bridge or the api and had no
  // timeout at all. WalletConnect only reports an error when the deep link
  // comes BACK carrying one — a closed browser tab produces nothing, ever — so
  // Verify Seeker greyed itself out and stayed that way until the app was
  // restarted, with no message and no sign it was waiting. Each flow now takes
  // a generation on the way out: the answer is dropped unless it is still the
  // current one, so a watchdog that has already given up cannot be overruled by
  // a late reply, and a superseded flow cannot resurrect its own busy flag.
  struct Flow {
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
    uint32_t generation = 0;
  };
  // Arms `flow`'s timer and returns the generation this attempt owns.
  uint32_t BeginFlow(Flow& flow, int timeoutMs, std::function<void()> onTimeout);
  // False when `generation` belongs to a flow that has already been given up on
  // or superseded — the caller must then do nothing at all.
  bool SettleFlow(Flow& flow, uint32_t generation);

  // An api call is answered or it is not; 20s is the sheet's watchdog too.
  static constexpr int kApiTimeoutMs = 20000;
  // The bridge flows leave the app entirely: the user opens a browser, picks a
  // wallet, approves a signature. Minutes, legitimately - so this is long
  // enough not to interrupt a real attempt and short enough to be a bound.
  static constexpr int kBridgeTimeoutMs = 180000;

  winrt::URnetwork::implementation::MainWindow& w_;
  // "Wallet connected" / the server's refusal: a transient message, so it
  // dismisses itself (iOS UrSnackBar). The bar used to stay open forever.
  //
  // TWO bars, one per destination, because each lives INSIDE its own
  // destination's ScrollViewer and the other one is Collapsed. A 401 from the
  // leaderboard switch went to the wallet's bar while the user was looking at
  // the leaderboard: the switch snapped back and the screen said nothing at
  // all - the message was sitting on a panel three clicks away, and turned up
  // later when the user happened to open Wallet (screenshotted). Notify() picks
  // the one the user can actually see.
  urnw::kit::Snackbar snackbar_;
  urnw::kit::Snackbar leaderboardSnackbar_;
  void Notify(winrt::hstring const& message,
              winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity severity);

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

  Flow seekerFlow_;

  // leaderboard ranking + its switch
  int64_t leaderboardRank_ = 0;
  bool rankingPublic_ = false;
  bool applyingRankingToggle_ = false;  // a programmatic write, not a user flip
  bool settingRankingPublic_ = false;
  Flow rankingFlow_;

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
  Flow connectFlow_;

  // the open detail sheet, held for the life of its ShowAsync
  std::shared_ptr<WalletDetailSheet> walletSheet_;
  std::shared_ptr<PayoutDetailSheet> payoutSheet_;
};

}  // namespace urnw
