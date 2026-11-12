// The Earnings destination's claim dialogs, plus the presentation helpers the
// destination and the dialogs share.
//
//   ClaimAlphaSheet   the unclaimed SN25a of the claimable epochs since the
//                     coldkey was attached, the gas key that pays for the claim
//                     transaction, and the claim itself: sent by the SDK on this
//                     device straight to the settlement vault contract. No
//                     URnetwork API is in that path.
//   ClaimLedgerSheet  the same for settlement v2: the coldkey's claimable v2
//                     accounts, each titled by its pool (capture the flag's
//                     beside the provider pool's), the same gas key and funding
//                     hint, and the claim of every pool, sent by the SDK on this
//                     device for the pinned vault2, each proof verified against
//                     vault2's root first: two or more accounts in one batch
//                     transaction through the chain settings' batcher where the
//                     gas key can pay for it, else one claim each to vault2.
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

#include "SnPayoutPresentation.h"

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
// "not-finalized" is, despite its name, an epoch the vault finalized with
// nothing for this coldkey to claim (no payout root was published for it, or
// no share in its payout list), so it reads "No payout published".
winrt::hstring ClaimStatusText(std::string const& status);

// A v2 account's title in the store's words, from its key
// (snpayout::PoolTitleKey): "Capture the flag", "Provider pool", or "Pool 1"
// with the pool's noId. The tile, the claim dialog and the claim sheet say it.
winrt::hstring PoolTitleText(std::string const& titleKey, int64_t noId);

// Capture the flag's won-slot line (snpayout::CtfWinKey) with the country's
// name in the reader's language, or its code where Windows has none: "You won
// one of the Iceland provider slots".
winrt::hstring CtfWinText(snpayout::CtfWin const& win);

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
// thread the binding delivers them on. The page marshals them to the UI thread
// (WalletPage::StartV1Claim), so an adapter may call these from anywhere.
struct ClaimEvents {
  std::function<void(int64_t epoch, std::string txHash)> sent;
  std::function<void(int64_t epoch, std::string txHash, int64_t amountRao)> confirmed;
  std::function<void(int64_t epoch, std::string message)> failed;
  std::function<void()> done;
};

// The v1 dialog's epochs and the state of their claim, which the dialog draws.
// The claim is the signed-in session's, not the dialog's (owner, 2026-11-12):
// the page holds this while a claim is in flight (snpayout::ClaimInFlight) and
// moves it with the claim's events whether or not a dialog is up, so a dialog
// dismissed during the claim drops none of them, one opened again shows the
// claim with Claim held until Done, and no second claim starts while it is in
// flight. UI thread only.
class ClaimAlphaProgress {
 public:
  // the dialog's view of one epoch, moved along by the claim events
  struct Row {
    EpochClaim claim;
    enum class State { Idle, Sending, Sent, Confirmed, Failed } state = State::Idle;
    std::string message;  // the failure, when State::Failed
  };

  // `claims` is every epoch the SDK reported since the wallet was attached;
  // the dialog claims the "claimable" ones and lists the rest with their
  // state, newest first. `gasTao` is the gas key's balance when known: a known
  // empty key needs gas before anything can be sent.
  ClaimAlphaProgress(std::vector<EpochClaim> claims, std::optional<double> gasTao);

  const std::vector<Row>& rows() const { return rows_; }
  bool sending() const { return sending_; }
  // set from a "needs gas" failure as well as a known empty gas key
  bool needsGas() const { return needsGas_; }
  // the epochs the last press claims (Start)
  const std::vector<int64_t>& epochs() const { return epochs_; }

  int64_t TotalClaimableRao() const;
  size_t ClaimableCount() const;
  // The dialog's figure, which Claim names too: what is claimable, and during
  // a claim what the claim moves, the epochs it sends as Start found them,
  // fixed until Done (owner, 2026-11-12, as on Android, Linux and Apple), so
  // it never falls to 0 as the epochs confirm.
  int64_t FigureRao() const;

  // Claim pressed: every claimable epoch not confirmed yet is Sending, and
  // epochs() are the press's. False, changing nothing, while a claim is out
  // or with no epoch to claim.
  bool Start();
  // The claim's events.
  void Sent(int64_t epoch, std::string const& txHash);
  void Confirmed(int64_t epoch, std::string const& txHash, int64_t amountRao);
  void Failed(int64_t epoch, std::string const& message);
  // The claim is over: a row the SDK never reported on is not sending any more.
  void Done();

 private:
  Row* Find(int64_t epoch);

  std::vector<Row> rows_;
  std::vector<int64_t> epochs_;
  // what the press in flight moves (FigureRao)
  int64_t pressRao_ = 0;
  bool sending_ = false;
  bool needsGas_ = false;
};

// Starts the claim of `progress`'s claimable epochs (Device.snClaim) as the
// session's claim in flight (WalletPage::StartV1Claim): false, starting
// nothing, while one is in flight or when the press takes no epoch. The page
// owns the SDK call and moves `progress` along with the claim's events.
using Claimer = std::function<bool(std::shared_ptr<ClaimAlphaProgress> progress)>;

class ClaimAlphaSheet : public std::enable_shared_from_this<ClaimAlphaSheet> {
 public:
  // `progress` is what the dialog draws, shared with the page: during a claim
  // the claim in flight's, whose events the page applies whether or not a
  // dialog is up, so a dialog dismissed and opened again shows the claim as
  // it went on, and its Claim waits for it; between claims the epochs the SDK
  // reported since the wallet was attached. `explorerTxUrl` is the chain's
  // "https://.../tx/%s" (empty: no links). `allowActions` is
  // WalletPage::CanClaim(): false when there is no device session to send
  // with, in which case the dialog still opens and still READS but its one
  // action is disabled. The page reads the claims again after the claim's
  // Done, whether or not the dialog is still up. `held`: a claim of the other
  // kind is in flight, which Claim waits for, as the gas key sends one claim
  // at a time (the SDK reads its pending nonce as it signs, with no lock
  // across the two kinds).
  static std::shared_ptr<ClaimAlphaSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root,
      std::shared_ptr<ClaimAlphaProgress> progress, GasKeyInfo gas, std::string explorerTxUrl,
      bool allowActions, Claimer claimer, std::function<bool()> held);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }
  // Draws the progress again: the page calls it on each event of the claim
  // in flight, and at the other kind's Done.
  void Redraw() { ApplyState(); }

 private:
  using Row = ClaimAlphaProgress::Row;

  ClaimAlphaSheet(std::shared_ptr<ClaimAlphaProgress> progress, GasKeyInfo gas,
                  std::string explorerTxUrl, bool allowActions, Claimer claimer,
                  std::function<bool()> held);
  // a claim of the other kind is in flight
  bool Held() const { return held_ && held_(); }

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void StartClaim();
  void CopyMirrorAddress();
  // Re-renders the rows, the gas block, the progress line and the button from
  // the current state. Called on every event.
  void ApplyState();
  winrt::Microsoft::UI::Xaml::UIElement BuildRow(Row const& row);
  void OpenTx(std::string const& txHash);

  std::shared_ptr<ClaimAlphaProgress> progress_;
  GasKeyInfo gas_;
  std::string explorerTxUrl_;
  bool allowActions_;
  Claimer claimer_;
  std::function<bool()> held_;

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

// ---- the settlement v2 claim dialog --------------------------------------------

// Starts the claim of every claimable v2 account (Device.snLedgerClaim) on
// the dialog's `progress`, as the session's claim in flight
// (WalletPage::StartV2Claim): false, starting nothing, while one is in flight
// already or when the press would take nothing. The page owns the SDK call and
// moves `progress` along with the claim's events.
using LedgerClaimer = std::function<bool(std::shared_ptr<snpayout::LedgerClaimProgress> progress)>;

class ClaimLedgerSheet : public std::enable_shared_from_this<ClaimLedgerSheet> {
 public:
  // `progress` is what the dialog draws, shared with the page: during a claim
  // the claim in flight's (snpayout::LedgerClaimInFlight), whose events the
  // page applies whether or not a dialog is up, so a dialog dismissed and
  // opened again shows the claim as it went on, and its Claim waits for it;
  // between claims the claimable accounts and the unavailable pools of the
  // page's last read, at the claim relay's transfer floor
  // (snpayout::LedgerClaimProgress). `explorerTxUrl`, `allowActions` and
  // `held` (a v1 claim in flight) as for ClaimAlphaSheet. The page reads the
  // accounts again after the claim's Done, whether or not the dialog is still
  // up.
  static std::shared_ptr<ClaimLedgerSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root,
      std::shared_ptr<snpayout::LedgerClaimProgress> progress, GasKeyInfo gas,
      std::string explorerTxUrl, bool allowActions, LedgerClaimer claimer,
      std::function<bool()> held);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }
  // Draws the progress again: the page calls it on each event of the claim
  // in flight, and at the other kind's Done.
  void Redraw() { ApplyState(); }

 private:
  ClaimLedgerSheet(std::shared_ptr<snpayout::LedgerClaimProgress> progress, GasKeyInfo gas,
                   std::string explorerTxUrl, bool allowActions, LedgerClaimer claimer,
                   std::function<bool()> held);
  // a v1 claim is in flight
  bool Held() const { return held_ && held_(); }

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void StartClaim();
  void CopyMirrorAddress();
  // Re-renders the figure, the gas block, the rows, the progress line, the
  // failure and the button from progress_. Called on every event.
  void ApplyState();
  winrt::Microsoft::UI::Xaml::UIElement BuildRow(snpayout::LedgerClaimRow const& row);
  // opens a claim transaction's page (snpayout::ExplorerTxLink)
  static void OpenTx(std::string const& url);

  std::shared_ptr<snpayout::LedgerClaimProgress> progress_;
  GasKeyInfo gas_;
  std::string explorerTxUrl_;
  bool allowActions_;
  LedgerClaimer claimer_;
  std::function<bool()> held_;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock totalValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock gasBalanceValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel needsGasPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel rowsPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel progressRow_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button claimButton_{nullptr};
};

// ---- EmojiTagSheet ----------------------------------------------------------
//
// The points leaderboard's emoji tag editor (android/POINTSLEADERBOARD.md and
// its amendment). The tag is composed on an EMOJI-ONLY keyboard: the draft is
// a read-only line the keys below append to, a backspace removes one emoji at
// a time (one grapheme, never half a sequence), and there is no text box, so
// nothing but emoji can ever be typed or pasted. A network with no tag starts
// from the SDK's random 1-3 emoji suggestion and the shuffle key re-rolls it;
// a suggestion is only a draft until Save. Every change is validated by the
// SDK exactly the way the server validates it (validateEmojiTag), so the
// counter and the Save button always say what the server would say.
//
// `saver(tag, done)` stores the tag (an empty tag clears it) and calls
// done(error) on the UI thread; an empty error closes the sheet, anything
// else is shown under the draft and the sheet stays open.
class EmojiTagSheet : public std::enable_shared_from_this<EmojiTagSheet> {
 public:
  using Saver = std::function<void(std::string tag, std::function<void(std::string)> done)>;

  static std::shared_ptr<EmojiTagSheet> Create(winrt::Microsoft::UI::Xaml::XamlRoot const& root,
                                               std::string currentTag, Saver saver);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  EmojiTagSheet(std::string currentTag, Saver saver);

  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  winrt::Microsoft::UI::Xaml::UIElement BuildDraftRow();
  winrt::Microsoft::UI::Xaml::UIElement BuildKeyboard();
  void ShowGroup(size_t index);
  void Append(std::string const& emoji);
  void DropLast();
  void Shuffle();
  // re-validates the draft through the SDK and renders the line, the counter
  // or the error, the keys and the buttons from the verdict
  void ApplyDraft();
  void Submit(std::string const& tag);
  void ApplyResult(std::string const& error);

  std::string currentTag_;
  std::vector<std::string> draft_;  // one element per emoji
  Saver saver_;
  bool saving_ = false;
  bool full_ = false;  // the draft holds the maximum: keys are disabled
  std::string normalized_;  // the SDK's form of a valid draft
  size_t group_ = 0;

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock draftText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock supportText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button backspaceButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button shuffleButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel groupStrip_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel keysPanel_{nullptr};
  std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> groupButtons_;
  std::vector<winrt::Microsoft::UI::Xaml::Controls::Button> keys_;
};

}  // namespace urnw
