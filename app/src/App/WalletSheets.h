// The wallet destination's two detail sheets, plus the presentation helpers the
// destination and the sheets share.
//
//   WalletDetailSheet   one account wallet: its identity, the payout-wallet tag,
//                       "make default" / "remove", and the payouts that landed
//                       in it (iOS Main/Account/Wallet/WalletView.swift).
//   PayoutDetailSheet   one payment: its points breakdown, the amount, the
//                       destination wallet and a link to the transaction on the
//                       chain's explorer (iOS PayoutItemView.swift).
//
// Plain C++ helpers like StatsSheets/BalanceSheets — no runtime classes; every
// method runs on the UI thread. Control handlers capture the sheet WEAKLY: the
// page holds the shared_ptr for the life of ShowAsync, so lock() always succeeds
// during interaction and a late callback after dismissal finds nothing.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "SdkHost.h"
#include "WalletPage.h"

namespace urnw {

// ---- shared presentation -----------------------------------------------------

// The blockchain's product name ("Solana", "Bittensor", "Polygon"), or the raw
// chain id for one this build does not know.
std::wstring ChainDisplayName(std::string const& blockchain);

// "***abc123" — the last six characters, as every other client shows a wallet.
std::wstring MaskAddress(std::string const& address);

// "12.34" (two decimals, as USDC is quoted throughout the product).
std::wstring FormatUsdcAmount(double amount);

// A points value with thousands separators, dropping a zero fraction
// (iOS `.number.grouping(.automatic)`).
std::wstring FormatPointsValue(double points);

// The date part of an SDK timestamp ("2026-08-06T12:00:00Z" -> "2026-08-06").
// Deliberately ISO rather than a localized "Jan 2": the store has no month-name
// resources, and a table of dates is read by comparison, not by prose.
std::wstring ShortDate(std::string const& timestamp);

// The chain's block explorer for a transaction hash, or an empty string when
// there is no hash. Solana -> solscan.io, everything else -> polygonscan.com
// (iOS PayoutItemView).
std::string ExplorerTxUrl(std::string const& blockchain, std::string const& txHash);

// The 48px gradient disc that identifies a chain (iOS WalletIcon.swift): the
// chain's gradient with its ticker in the brand's bitmap face. iOS uses vector
// logos; this build ships no logo assets, and inventing one is worse than
// naming the chain.
winrt::Microsoft::UI::Xaml::FrameworkElement WalletIconElement(std::string const& blockchain,
                                                              double size = 44);

// The "DEFAULT" chip on the payout wallet (iOS PayoutWalletTag.swift). Returns a
// collapsed element when this is not the payout wallet, so the caller can place
// it unconditionally.
winrt::Microsoft::UI::Xaml::FrameworkElement PayoutWalletTag(bool isPayoutWallet);

// The points-breakdown card body (iOS AccountPointsBreakdown.swift): payout /
// referral / reliability columns, the Seeker multiplier row when the account
// holds the token, and the net total. Shared by the wallet destination's
// "Account points" card and the payout detail sheet, which show the same shape
// over different totals.
winrt::Microsoft::UI::Xaml::UIElement BuildPointsBreakdown(PointsBreakdown const& points,
                                                          bool seekerHolder);

// ---- one account wallet ------------------------------------------------------
class WalletDetailSheet : public std::enable_shared_from_this<WalletDetailSheet> {
 public:
  // `onChanged` fires after the payout wallet moved or this wallet was removed
  // (the page reloads); `onSuccess` raises the page's snackbar, and is only
  // reached on a path that also CLOSES this sheet.
  //
  // A failure never goes to the page: the sheet is modal and covers it, so a
  // snackbar raised behind it is a message the user cannot read. Screenshotted:
  // pressing Remove with an expired session left the sheet open, unchanged, with
  // "401 Unauthorized" drawn underneath the dialog. Failures render on the
  // sheet's own line instead.
  // `allowActions` is WalletPage::CanCallApi(): false when there is no session
  // to act with (--preview-ui, signed out). The sheet still opens and still
  // READS - that is what the preview is for - but its two buttons, which are
  // the two api WRITES on this destination, are disabled and refuse. Under
  // --preview-ui with sample data those buttons used to be fully live: two
  // clicks put removeWallet on the wire from a build with no account, and on a
  // machine WITH a stored session they would have run it for real.
  static std::shared_ptr<WalletDetailSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      urnet::AccountWallet const& wallet, bool isPayoutWallet,
      std::vector<urnet::AccountPayment> const& payments, bool allowActions,
      std::function<void()> onChanged,
      std::function<void(winrt::hstring message)> onSuccess);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  WalletDetailSheet(SdkHost& sdk, urnet::AccountWallet const& wallet, bool isPayoutWallet,
                    std::vector<urnet::AccountPayment> const& payments, bool allowActions,
                    std::function<void()> onChanged,
                    std::function<void(winrt::hstring)> onSuccess)
      : sdk_(sdk),
        wallet_(wallet),
        isPayoutWallet_(isPayoutWallet),
        payments_(payments),
        allowActions_(allowActions),
        onChanged_(std::move(onChanged)),
        onSuccess_(std::move(onSuccess)) {}

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void MakeDefault();
  void RemoveWallet();       // arms the confirmation; the second press commits
  void CommitRemoveWallet();
  // Disables the actions for the life of a request AND arms the watchdog below.
  // Returns the generation this request owns; pair it with SettleRequest().
  uint32_t SetBusy(bool busy);
  // False when this answer belongs to a request already given up on.
  bool SettleRequest(uint32_t generation);
  void ShowError(winrt::hstring const& message);

  // False when the caller has no session: the actions are drawn but inert.
  bool CanAct();

  SdkHost& sdk_;
  urnet::AccountWallet wallet_;
  bool isPayoutWallet_ = false;
  std::vector<urnet::AccountPayment> payments_;
  bool allowActions_ = false;
  std::function<void()> onChanged_;
  std::function<void(winrt::hstring)> onSuccess_;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button makeDefaultButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button removeButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock confirmText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  // A request that never answers must not be able to leave this sheet dead.
  //
  // Found by running: Api::setPayoutWallet DROPS the call and never invokes its
  // callback when wallet_id is not a UUID - no result, no error, nothing in the
  // log - so the sheet sat with both actions greyed out and no message, forever,
  // with no way back but Close. Every id the UI passes comes from
  // getAccountWallets and is well-formed, so this is a guard rather than a
  // routine path; but "the caller always passes a good id" is an invariant
  // nothing enforces, and a dead dialog is the worst possible way to find out.
  //
  // The watchdog also has to be able to have the LAST word. When it fired and
  // the real callback turned up afterwards carrying a success, the sheet hid
  // itself and fired onChanged_ - reloading the page - after the user had
  // already been told the request failed. Every request takes a generation on
  // the way out and the answer is dropped unless it is still the current one.
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer watchdog_{nullptr};
  static constexpr int kRequestTimeoutMs = 20000;
  uint32_t requestGeneration_ = 0;
  bool removeArmed_ = false;
  bool busy_ = false;
};

// ---- one payout --------------------------------------------------------------
// Read-only: it makes no request, so it has no failure of its own.
class PayoutDetailSheet : public std::enable_shared_from_this<PayoutDetailSheet> {
 public:
  static std::shared_ptr<PayoutDetailSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root,
      urnet::AccountPayment const& payment, PointsBreakdown const& points,
      bool seekerHolder);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
};

}  // namespace urnw
