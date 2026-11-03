// Everything the SN payout line decides before it touches a XAML object: which
// line shows under the points figure on Earnings, the current epoch's times it
// names and whether it carries Claim. And the same for settlement v2: what its
// section beside the v1 tile shows, and how its claim dialog moves along.
//
// Why it exists: provider payouts moved from weekly USDC to the UR subnet, and
// nothing on Earnings said how or when a provider is paid ("used it a month, no
// crypto"). Per the sn whitepaper (§5.2, §8.3), earnings settle every epoch and
// are paid in SN25a to the provider's Bittensor coldkey when the provider
// claims them; the app never claims by itself. The times come from the epoch
// schedule the SDK reads with the claims (the coordinator's claim-open offset
// and claim TTL), so the copy never fixes a duration.
//
// It is all here, and all pure, for the reason ExtenderPresentation.h gives:
// the windows solution has no test project and a WinUI 3 app cannot be built
// off Windows, so every decision expressed on plain values is verified by
// tools/sn-payout-tests.cpp on any host with a C++20 compiler. The SDK's
// SnEpochSchedule is mirrored as a plain view; WalletPage.cpp copies the fields
// across at its boundary.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace urnw::snpayout {

// The current epoch's settlement times (the SDK's SnEpochSchedule): when it
// ends, when its share can be claimed and when an unclaimed share expires.
struct EpochSchedule {
  int64_t epoch = 0;
  int64_t endMillis = 0;
  int64_t claimOpenMillis = 0;
  int64_t expiryMillis = 0;
  // the policy's epoch length in blocks, which the one-tap key's reminder reads
  int64_t epochBlocks = 0;
};

enum class LineKind {
  Hidden,      // whether a coldkey is set is not known yet
  SetColdkey,  // set_coldkey_to_get_paid, with set_coldkey opening the coldkey flow
  Schedule,    // sn_payout_schedule, then sn_payout_schedule_times when known
};

// What the line under the points figure shows.
struct PayoutLineView {
  LineKind kind = LineKind::Hidden;
  bool showTimes = false;  // sn_payout_schedule_times, with the three times below
  std::string epochEnd;
  std::string claimOpen;
  std::string expiry;
  bool showClaim = false;  // Claim, which opens the claim dialog

  bool operator==(const PayoutLineView&) const = default;
};

// The line for the pane. Hidden while it is not known whether a coldkey is set
// (the wallet is loading, or its read failed with none cached); SetColdkey with
// none, since alpha goes only to a coldkey; otherwise the schedule, with the
// times when the schedule is known and its epoch has not already ended (an
// ended epoch waits for the next read), and Claim while something is claimable.
PayoutLineView PayoutLineFor(bool walletKnown, bool hasColdkey, int64_t totalClaimableRao,
                             const std::optional<EpochSchedule>& schedule, int64_t nowMillis,
                             const std::function<std::string(int64_t)>& formatTime);

// "2026-10-13 02:00": a schedule time in the reader's local time, given the
// zone's offset from UTC at that instant (the page asks Windows per instant, so
// a daylight change before the expiry is honored). Civil-from-days like the
// history's dates (EarningsSheets DateFromMillis), so no locale.
std::string FormatScheduleTime(int64_t millis, int32_t utcOffsetMinutes);

// ---- settlement v2 ------------------------------------------------------------
//
// From its switch epoch an operator pool settles in STSettlementVault2: the
// payout coldkey has one account per pool, a cumulative balance that never
// expires. The SDK on this device reads the coldkey's accounts from the
// operator's ledger (SnLedgerAccounts) and claims them (SnLedgerClaim), each
// account's proof verified against vault2's root before one claim per account
// goes to the pinned vault2. Earnings shows them in a "Settlement v2" section
// beside the v1 tile, which stays as it was. The SDK's SnLedgerAccount is
// mirrored as a plain view; WalletPage.cpp copies the fields across.

// One v2 account (the SDK's SnLedgerAccount). Amounts are rao.
struct LedgerAccount {
  int64_t noId = 0;          // the operator pool
  int64_t index = 0;         // the account's position in the pool's tree
  int64_t finalizedRao = 0;  // the cumulative in the latest finalized root
  int64_t claimableRao = 0;  // finalized less paid: what a claim transfers now
  int64_t paidRao = 0;       // vault2's claimed watermark, already transferred
  bool pending = false;      // a committed root that is not finalized holds the account
  int64_t pendingRao = 0;    // its cumulative in that root

  bool operator==(const LedgerAccount&) const = default;
};

// Where the page's read of the accounts stands.
enum class LedgerFetch {
  Loading,   // SnLedgerAccounts is out; the last read's accounts stay meanwhile
  Ready,     // it answered with the accounts (none: the ledger has no account for the coldkey)
  Failed,    // it answered with an error (ledger_unavailable, or any other)
  NoDevice,  // no device session to read with (SnLedgerAccounts is the device's)
};

// The line under the section's accounts.
enum class LedgerNote {
  None,
  Unavailable,   // the read failed, so no figure shows, never a zero
  NeedsSession,  // no device session to read with, as the v1 tile says it
  NoAccount,     // the ledger has no account for the coldkey yet
};

// The store key of the note's line, "" for None. The section's words are the
// store's sn_ledger_* keys, which the Android and Linux apps look up too; the
// empty state is the Linux tile's.
std::string LedgerNoteKey(LedgerNote note);

// One account's block: the claimable figure (sn_ledger_claimable), Paid and
// Earned (sn_ledger_paid, sn_ledger_earned), and the pending line while a
// root that is not finalized yet raises the account.
struct LedgerAccountView {
  int64_t noId = 0;
  int64_t claimableRao = 0;
  int64_t paidRao = 0;
  int64_t earnedRao = 0;     // the finalized cumulative
  bool showPending = false;  // sn_ledger_pending: "Pending: +X, awaiting finalization"
  int64_t pendingRao = 0;    // X, the pending cumulative less the finalized one

  bool operator==(const LedgerAccountView&) const = default;
};

// What the section shows, under its heading (sn_ledger_title).
struct LedgerSectionView {
  bool shown = false;
  bool loading = false;     // "Loading..." in the section's header
  bool poolTitles = false;  // more than one account: each block names its pool (sn_ledger_pool)
  std::vector<LedgerAccountView> accounts;
  LedgerNote note = LedgerNote::None;
  bool showClaim = false;  // Claim, which opens the v2 claim dialog
  bool claimEnabled = false;

  bool operator==(const LedgerSectionView&) const = default;
};

// The section, from whether a coldkey is set, whether vault2 is configured
// (the chain settings' Vault2Address), the read and whether this device can
// send a claim (WalletPage::CanClaim). Hidden without a coldkey: accounts are a
// coldkey's. The accounts show while a read is Ready or Loading, never after a
// failed one. With vault2 configured the section always shows, with its Claim:
// the accounts, loading, unavailable, the session note or no account yet; Claim
// is enabled while the accounts on screen have something to claim and the
// device can send it. Before vault2 is configured (before v2's activation)
// there is no Claim, and the section shows only when the read returned
// accounts, so a ledger that is not served yet says nothing.
LedgerSectionView LedgerSectionFor(bool hasColdkey, bool vault2Configured, LedgerFetch fetch,
                                   const std::vector<LedgerAccount>& accounts, bool canClaim);

// ---- the v2 claim dialog
//
// The dialog lists the accounts with something to claim and moves each along
// with the SDK's SnLedgerClaimCallback: Sent with the transaction (the explorer
// link), Confirmed with the amount of vault2's Claimed event, Failed with what
// the failure says. needs_gas opens the gas key's funding hint, the v1 dialog's
// top-up; already_claimed, a third party's claim for the account (vault2 pays
// the leaf's coldkey whoever sends it), is the account's success. The page
// reads the accounts again after Done.

// The TAO the funding hint asks for. The SDK sends a v2 claim only when the gas
// key holds its gas limit (eth_estimateGas, about 2.51M on runtime 475 for
// about 236k used, plus a quarter) times 1.1 x the gas price: about 0.017 TAO
// at the plan's 5 gwei. The v1 dialog asks for the same. Whether the key can
// pay stays the SDK's preflight (needs_gas), never this figure.
inline constexpr double kLedgerClaimGasTao = 0.02;

// The explorer page of a claim transaction: the chain settings' ExplorerTxUrl
// with the hash in place of its "%s", else appended as a path segment; "" when
// either is empty, and then no link shows.
std::string ExplorerTxLink(const std::string& explorerTxUrl, const std::string& txHash);

enum class LedgerClaimState { Idle, Sending, Sent, Confirmed, Failed };

struct LedgerClaimRow {
  int64_t noId = 0;
  int64_t index = 0;
  int64_t amountRao = 0;  // claimable; the Claimed event's amount once confirmed
  LedgerClaimState state = LedgerClaimState::Idle;
  std::string txHash;   // the claim transaction once sent, "" before
  std::string message;  // the SDK's failure, when Failed

  bool operator==(const LedgerClaimRow&) const = default;
};

// The SnErrorCode a Failed message starts with ("needs_gas: detail" -> "needs_gas").
std::string ClaimFailureCode(const std::string& message);

// The store key a failure shows, for a code the store has a sentence for, or ""
// to show the SDK's message as it came.
std::string ClaimFailureKey(const std::string& message);

class LedgerClaimProgress {
 public:
  // `accounts` as the page last read them: the claimable ones are the rows.
  // `gasTao` is the gas key's balance when known: a known empty key needs gas
  // before anything can be sent.
  LedgerClaimProgress(const std::vector<LedgerAccount>& accounts, std::optional<double> gasTao);

  const std::vector<LedgerClaimRow>& rows() const { return rows_; }
  bool sending() const { return sending_; }
  bool needsGas() const { return needsGas_; }
  // a failure the SDK reported for no pool (pool 0): the claim as a whole
  const std::string& failure() const { return failure_; }

  // the rows not confirmed yet: what Claim sends. Saturates rather than
  // overflowing on an absurd ledger answer (the endpoint is untrusted).
  int64_t ClaimableRao() const;
  // Claim's button: actions allowed (a device session), something to claim,
  // gas, and no claim out
  bool CanClaim(bool allowActions) const;

  // Claim pressed: every row not confirmed is Sending. False, changing nothing,
  // while a claim is out or with nothing to claim.
  bool Start();
  // The callback's events. The SDK claims every account it verified, which can
  // be one the page's read did not list: that account gets a row.
  void Sent(int64_t noId, int64_t index, const std::string& txHash);
  void Confirmed(int64_t noId, int64_t index, const std::string& txHash, int64_t amountRao);
  void Failed(int64_t noId, int64_t index, const std::string& message);
  // the claim is over: a row the SDK never reported on is not sending any more
  void Done();

 private:
  LedgerClaimRow& RowFor(int64_t noId, int64_t index);

  std::vector<LedgerClaimRow> rows_;
  bool sending_ = false;
  bool needsGas_ = false;
  std::string failure_;
};

// The SDK's SnLedgerClaimCallback as four plain functions, which the dialog
// marshals to the UI thread itself (EarningsSheets.h LedgerClaimer). A failure
// with no pool (noId 0) is the claim's as a whole.
struct LedgerClaimEvents {
  std::function<void(int64_t noId, int64_t index, std::string txHash)> sent;
  std::function<void(int64_t noId, int64_t index, std::string txHash, int64_t amountRao)> confirmed;
  std::function<void(int64_t noId, int64_t index, std::string message)> failed;
  std::function<void()> done;
};

// ---- at the SDK's boundary (WalletPage.cpp)
//
// Templates over the generated wrapper's types (urnetwork_sdk.hpp's
// urnet::SnLedgerAccount and urnet::SnLedgerClaimCallback): the spec builds
// them against stand-ins with the wrapper's names and types and, when a header
// is at hand, against the header itself; WalletPage instantiates them with the
// SDK's own.

// The SDK's SnLedgerAccount as the plain view.
template <typename SdkLedgerAccount>
LedgerAccount LedgerAccountFrom(const SdkLedgerAccount& account) {
  LedgerAccount out;
  out.noId = account.no_id;
  out.index = account.index;
  out.finalizedRao = account.finalized_rao;
  out.claimableRao = account.claimable_rao;
  out.paidRao = account.paid_rao;
  out.pending = account.pending;
  out.pendingRao = account.pending_rao;
  return out;
}

// An SnLedgerClaimCallback that hands each event to `events`.
template <typename SdkLedgerClaimCallback>
SdkLedgerClaimCallback LedgerClaimCallbackFor(const LedgerClaimEvents& events) {
  SdkLedgerClaimCallback callback;
  callback.sent = [events](int64_t noId, int64_t index, const std::string& txHash) {
    if (events.sent) events.sent(noId, index, txHash);
  };
  callback.confirmed = [events](int64_t noId, int64_t index, const std::string& txHash,
                                int64_t amountRao) {
    if (events.confirmed) events.confirmed(noId, index, txHash, amountRao);
  };
  callback.failed = [events](int64_t noId, int64_t index, const std::string& message) {
    if (events.failed) events.failed(noId, index, message);
  };
  callback.done = [events]() {
    if (events.done) events.done();
  };
  return callback;
}

}  // namespace urnw::snpayout
