// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "WalletPage.h"

#include <chrono>
#include <cstdio>

#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;

namespace urnw {

// winrt::implements makes IInspectable a member typedef of every C++/WinRT
// implementation type, which is why MainWindow could name it unqualified. A
// plain class outside that hierarchy has to bring it in.
using winrt::Windows::Foundation::IInspectable;

namespace {
// Display name of an account wallet's blockchain (macOS/android parity). Chain
// names are product names: the store carries them untranslated.
std::wstring ChainName(std::string const& blockchain) {
  if (blockchain == urnet::SOL) return urnw::Localized("solana");
  if (blockchain == urnet::TAO) return urnw::Localized("bittensor");
  if (blockchain == urnet::MATIC) return urnw::Localized("polygon");
  return urnw::Widen(blockchain);  // an unknown chain: its raw id
}
}  // namespace

WalletPage::WalletPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window), snackbar_(window.WalletInfo(), window.DispatcherQueue()) {}

WalletPage::~WalletPage() {
  if (walletValidateTimer_) walletValidateTimer_.Stop();
}

void WalletPage::Initialize() {
  // debounce the connect-wallet address validation while typing (apple parity):
  // each keystroke restarts the window, and only the pause validates.
  walletValidateTimer_ = w_.DispatcherQueue().CreateTimer();
  walletValidateTimer_.Interval(std::chrono::milliseconds(300));
  walletValidateTimer_.IsRepeating(false);
  walletValidateTimer_.Tick([weak = w_.get_weak()](auto const&, auto const&) {
    if (auto self = weak.get()) self->wallet().ValidateWalletAddress();
  });
}

void WalletPage::ApplyStrings() {
  // wallet
  w_.PlanHeading().Text(Loc("plan"));
  w_.UpgradeButton().Content(LocBox("upgrade_with_stripe"));
  w_.PayoutWalletsHeading().Text(Loc("payout_wallets"));
  w_.ConnectWalletHeading().Text(Loc("connect_a_wallet"));
  // supported chains, then the bittensor caveat: two store sentences rather than a
  // third near-duplicate of both
  w_.ConnectWalletChainsText().Text(
      hstring{urnw::Localized("connect_external_wallet_supported_chains") + L" " +
              urnw::Localized("bittensor_wallet_future_use")});
  w_.WalletAddressBox().PlaceholderText(Loc("enter_wallet_address"));
  w_.ConnectWalletButton().Content(LocBox("connect"));

  // leaderboard (its title comes from the NavigationView header)
  w_.LeaderboardDescription().Text(Loc("leaderboard_description"));
  w_.LeaderboardStatusText().Text(Loc("loading"));
}

// ---- wallet --------------------------------------------------------------

void WalletPage::LoadWallet() {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getAccountWallets(
      [queue, weak](std::optional<urnet::GetAccountWalletsResult> result,
                    std::optional<std::string>) {
        if (!result || !result->wallets) return;
        urnet::AccountWalletsList wallets = *result->wallets;
        queue.TryEnqueue([weak, wallets] {
          if (auto self = weak.get()) self->wallet().ApplyWallets(wallets);
        });
      });
}

void WalletPage::ApplyWallets(urnet::AccountWalletsList const& wallets) {
  w_.WalletsList().Items().Clear();
  for (auto const& wallet : wallets) {
    std::wstring label = ChainName(wallet.blockchain) + L"  " +
                         urnw::Widen(wallet.wallet_address);
    // TAO (bittensor) wallets are recorded for future use only: the server
    // refuses to make one the payout wallet and skips the auto-default on
    // creation, so the row says so (macOS/android parity)
    if (wallet.blockchain == urnet::TAO) {
      label += L"  -  " + urnw::Localized("stored_for_future_use");
    }
    w_.WalletsList().Items().Append(winrt::box_value(hstring{label}));
  }
}

// ---- connect wallet (external wallet, by address) -------------------------
// Paste an address; the server validates it per chain and the first chain that
// accepts it wins. Bittensor connects by address only (no signature), matching
// apple/android — the signed bridge flow is sign-in, not wallet connect.

void WalletPage::OnWalletAddressChanged(IInspectable const&, TextChangedEventArgs const&) {
  walletValidation_ = {};
  walletChain_.clear();
  ++walletValidateGeneration_;  // drop any validation still in flight
  w_.ConnectWalletButton().IsEnabled(false);
  w_.WalletChainText().Text(L"");
  if (walletValidateTimer_) {
    walletValidateTimer_.Stop();  // restart the debounce window on every keystroke
    walletValidateTimer_.Start();
  }
}

void WalletPage::ValidateWalletAddress() {
  const std::string address = urnw::Narrow(w_.WalletAddressBox().Text().c_str());
  // the shortest supported address (solana base58) is 32 characters
  if (address.size() < 32 || !Sdk().apiReady()) return;
  const uint32_t generation = ++walletValidateGeneration_;

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  const std::string chains[] = {urnet::SOL, urnet::MATIC, urnet::TAO};
  for (std::string const& chain : chains) {
    urnet::WalletValidateAddressArgs args;
    args.address = address;
    args.chain = chain;
    Sdk().api().walletValidateAddress(
        args, [queue, weak, chain, generation](
                  std::optional<urnet::WalletValidateAddressResult> result,
                  std::optional<std::string>) {
          const bool valid = result && result->valid && *result->valid;
          queue.TryEnqueue([weak, chain, generation, valid] {
            if (auto self = weak.get())
              self->wallet().ApplyWalletValidation(chain, generation, valid);
          });
        });
  }
}

void WalletPage::ApplyWalletValidation(std::string const& chain, uint32_t generation,
                                       bool valid) {
  if (generation != walletValidateGeneration_) return;  // a later edit superseded this
  if (chain == urnet::SOL) walletValidation_.sol = valid;
  else if (chain == urnet::MATIC) walletValidation_.matic = valid;
  else if (chain == urnet::TAO) walletValidation_.tao = valid;

  if (walletValidation_.sol) walletChain_ = urnet::SOL;
  else if (walletValidation_.matic) walletChain_ = urnet::MATIC;
  else if (walletValidation_.tao) walletChain_ = urnet::TAO;
  else walletChain_.clear();

  w_.ConnectWalletButton().IsEnabled(!walletChain_.empty() && !connectingWallet_);
  if (walletChain_ == urnet::TAO) {
    w_.WalletChainText().Text(Loc("bittensor_wallet_future_use"));
  } else if (!walletChain_.empty()) {
    w_.WalletChainText().Text(
        hstring{urnw::Format("wallet_provider_lower", ChainName(walletChain_))});
  } else {
    w_.WalletChainText().Text(L"");
  }
}

void WalletPage::OnConnectWallet(IInspectable const&, RoutedEventArgs const&) {
  const std::string address = urnw::Narrow(w_.WalletAddressBox().Text().c_str());
  if (address.empty() || walletChain_.empty() || connectingWallet_) return;
  connectingWallet_ = true;
  w_.ConnectWalletButton().IsEnabled(false);

  // WalletViewController::addExternalWallet parity: the account wallet is created
  // on the chain the server validated, with the USDC token type.
  urnet::CreateAccountWalletArgs args;
  args.blockchain = walletChain_;
  args.wallet_address = address;
  args.default_token_type = "USDC";

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().createAccountWallet(
      args, [queue, weak](std::optional<urnet::CreateAccountWalletResult> result,
                          std::optional<std::string> err) {
        const bool ok = result && result->wallet_id && !result->wallet_id->empty();
        // this runs on an sdk thread; hand the raw outcome to the ui thread and
        // let it do the lookup (the store is read from the ui thread throughout)
        const std::string error = ok ? std::string() : (err ? *err : std::string());
        queue.TryEnqueue([weak, ok, error] {
          if (auto self = weak.get())
            self->wallet().ApplyWalletConnectResult(ok, error);
        });
      });
}

void WalletPage::ApplyWalletConnectResult(bool ok, std::string const& serverError) {
  connectingWallet_ = false;
  // a server error is not localizable; show it when there is one
  snackbar_.Show(ok ? Loc("wallet_connected")
                    : (serverError.empty() ? Loc("wallet_connect_failed") : H(serverError)),
                 ok ? InfoBarSeverity::Success : InfoBarSeverity::Error);
  if (!ok) {
    w_.ConnectWalletButton().IsEnabled(!walletChain_.empty());
    return;
  }
  w_.WalletAddressBox().Text(L"");  // clears the verdict through OnWalletAddressChanged
  LoadWallet();                     // refresh the list with the new wallet
}

void WalletPage::ShowPreviewSnackbar() {
  snackbar_.Show(Loc("wallet_connect_failed"), InfoBarSeverity::Error);
}

// ---- leaderboard ---------------------------------------------------------

void WalletPage::ShowPreviewLeaderboardState() {
  ApplyLeaderboard({}, /*ok=*/true);
}

// The panel used to draw one heading and nothing else whether the fetch was in
// flight, had come back empty, or had failed - and a failure was silent in the
// log too, so there was no way at all to tell "this screen is broken" from
// "there is no data". All three now say which they are.
void WalletPage::ApplyLeaderboard(urnet::LeaderboardEarnersList const& earners, bool ok) {
  w_.LeaderboardList().Items().Clear();
  if (!ok) {
    w_.LeaderboardStatusText().Text(Loc("something_went_wrong"));
    w_.LeaderboardStatusText().Visibility(Visibility::Visible);
    return;
  }
  if (earners.empty()) {
    w_.LeaderboardStatusText().Text(Loc("none"));
    w_.LeaderboardStatusText().Visibility(Visibility::Visible);
    return;
  }
  w_.LeaderboardStatusText().Visibility(Visibility::Collapsed);
  int rank = 1;
  for (auto const& e : earners) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d.  %s  —  %.1f MiB", rank++,
                  e.network_name.c_str(), e.net_mib_count);
    w_.LeaderboardList().Items().Append(winrt::box_value(winrt::to_hstring(buf)));
  }
}

void WalletPage::LoadLeaderboard() {
  w_.LeaderboardStatusText().Text(Loc("loading"));
  w_.LeaderboardStatusText().Visibility(Visibility::Visible);

  urnet::GetLeaderboardArgs args;
  // [queue, weak], like every other SDK callback in this file. This one used to
  // capture a raw `this` and call w_.DispatcherQueue() from the SDK thread -
  // the only two places in the split that did (the other is OnSendFeedback).
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getLeaderboard(
      args, [queue, weak](std::optional<urnet::LeaderboardResult> result,
                          std::optional<std::string> err) {
        const bool ok = result && result->earners && !err;
        urnet::LeaderboardEarnersList earners;
        if (ok) earners = *result->earners;
        if (!ok) {
          urnw::LogError("leaderboard: fetch failed{}",
                         err ? (": " + *err) : std::string());
        }
        queue.TryEnqueue([weak, earners = std::move(earners), ok] {
          if (auto self = weak.get()) self->wallet().ApplyLeaderboard(earners, ok);
        });
      });
}

}  // namespace urnw
