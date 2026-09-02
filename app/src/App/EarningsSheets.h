// The Earnings destination's claim dialog, plus the presentation helpers the
// destination and the dialog share.
//
//   ClaimAlphaSheet   the unclaimed SN25a across the finalized epochs since the
//                     coldkey was attached, the gas key that pays for the claim
//                     transaction, and the claim itself: sent by the SDK on this
//                     device straight to the settlement vault contract. No
//                     URnetwork API is in that path.
//
// Plain C++ helpers like StatsSheets/BalanceSheets: no runtime classes; every
// method runs on the UI thread. Control handlers capture the sheet WEAKLY: the
// page holds the shared_ptr for the life of ShowAsync, so lock() always succeeds
// during interaction and a late callback after dismissal finds nothing.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace urnw {

// ---- shared presentation -----------------------------------------------------

// The account's points, summed per event kind (iOS AccountPointsBreakdown).
struct PointsBreakdown {
  double net = 0;
  double payout = 0;       // providing
  double referral = 0;
  double multiplier = 0;   // the Seeker 2x, points only
  double reliability = 0;
};

// A points value with thousands separators, dropping a zero fraction
// (iOS `.number.grouping(.automatic)`).
std::wstring FormatPointsValue(double points);

// The date part of an SDK timestamp ("2026-08-06T12:00:00Z" -> "2026-08-06").
// Deliberately ISO rather than a localized "Jan 2": the store has no month-name
// resources, and a table of dates is read by comparison, not by prose.
std::wstring ShortDate(std::string const& timestamp);

// The same ISO date from epoch milliseconds (the epoch bounds the server sends).
std::wstring DateFromMillis(int64_t millis);

// "5F3s...kQ9v": the first four and last four characters of an address, which
// is how every surface here shows a coldkey (the SDK's ShortSs58 shape, kept
// local so the preview build renders without a device).
std::wstring ShortAddress(std::string const& address);

// "3.2410 SN25a" from rao (1 alpha = 1e9 rao), four decimals, the store's
// symbol (sn_alpha_symbol).
std::wstring FormatAlphaRao(int64_t rao);

// "0.0123" - a TAO amount with four decimals.
std::wstring FormatTao(double tao);

// "0.71%" from basis points.
std::wstring FormatShareBpsValue(int64_t shareBps);

// The store's word for an epoch's claim status ("open", "claimable",
// "claimed", "expired", "not-finalized"); the raw status for one this build
// does not know, so a new server state is visible rather than blank.
winrt::hstring ClaimStatusText(std::string const& status);

// ---- the claim dialog ----------------------------------------------------------

// One finalized epoch's claim, as the page and the dialog see it (the SDK's
// SnEpochClaim, copied so the dialog holds no SDK handle).
struct EpochClaim {
  int64_t epoch = 0;
  int64_t shareBps = 0;
  int64_t amountRao = 0;
  std::string status;  // "open" | "claimable" | "claimed" | "expired" | "not-finalized"
  int64_t claimOpenBlock = 0;
  int64_t expiryBlock = 0;
  std::string txHash;   // the last known claim tx for this epoch, "" if none
  std::string message;  // the SDK's reason an epoch is not claimable, "" if none
};

// The SDK-held gas key: the EVM address that signs claims, the ss58 mirror the
// user funds with TAO, and its balance when the chain answered.
struct GasKeyInfo {
  std::string address;
  std::string mirrorSs58;
  std::optional<double> tao;  // nullopt: the balance fetch failed or is pending
};

// The SDK's SnClaimCallback, as four functions the page adapts to whatever
// thread the binding delivers them on. The dialog marshals to the UI thread
// itself, so an adapter may call these from anywhere.
struct ClaimEvents {
  std::function<void(int64_t epoch, std::string txHash)> sent;
  std::function<void(int64_t epoch, std::string txHash, int64_t amountRao)> confirmed;
  std::function<void(int64_t epoch, std::string message)> failed;
  std::function<void()> done;
};
// Starts the claim for `epochs` (Device.snClaim); the page owns the SDK call.
using Claimer = std::function<void(std::vector<int64_t> epochs, ClaimEvents events)>;

class ClaimAlphaSheet : public std::enable_shared_from_this<ClaimAlphaSheet> {
 public:
  // `claims` is every epoch the SDK reported since the wallet was attached;
  // the dialog claims the "claimable" ones and lists the rest with their state.
  // `explorerTxUrl` is the chain's "https://.../tx/%s" (empty: no links).
  // `allowActions` is WalletPage::CanClaim(): false when there is no device
  // session to send with, in which case the dialog still opens and still
  // READS but its one action is disabled. `onClaimed` fires on the UI thread
  // after at least one epoch confirmed, so the page can refresh the tile.
  static std::shared_ptr<ClaimAlphaSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root,
      winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
      std::vector<EpochClaim> claims, GasKeyInfo gas, std::string explorerTxUrl,
      bool allowActions, Claimer claimer, std::function<void()> onClaimed);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  // the dialog's view of one epoch, moved along by the claim events
  struct Row {
    EpochClaim claim;
    enum class State { Idle, Sending, Sent, Confirmed, Failed } state = State::Idle;
    std::string message;  // the failure, when State::Failed
  };

  ClaimAlphaSheet(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
                  std::vector<EpochClaim> claims, GasKeyInfo gas, std::string explorerTxUrl,
                  bool allowActions, Claimer claimer, std::function<void()> onClaimed);

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void StartClaim();
  void CopyMirrorAddress();
  // Re-renders the rows, the gas block, the progress line and the button from
  // the current state. Called on every event.
  void ApplyState();
  winrt::Microsoft::UI::Xaml::UIElement BuildRow(Row const& row);
  void OpenTx(std::string const& txHash);

  int64_t TotalClaimableRao() const;
  size_t ClaimableCount() const;
  bool NeedsGas() const;

  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_;
  std::vector<Row> rows_;
  GasKeyInfo gas_;
  std::string explorerTxUrl_;
  bool allowActions_;
  Claimer claimer_;
  std::function<void()> onClaimed_;

  bool sending_ = false;
  bool needsGas_ = false;  // set from a "needs gas" failure as well as a zero balance
  bool anyConfirmed_ = false;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock totalValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock acrossText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock gasBalanceValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel needsGasPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel rowsPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel progressRow_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button claimButton_{nullptr};
};

}  // namespace urnw
