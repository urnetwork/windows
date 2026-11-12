// Everything the SN payout line decides before it touches a XAML object: which
// line shows under the points figure on Earnings, the current epoch's times it
// names and whether it carries Claim. And the same for settlement v2: what its
// section beside the v1 tile shows, how its claim dialog moves along, the claim
// sheet's pool rows, and capture the flag's won-slot lines.
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

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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
// account's proof verified against vault2's root before its claim is sent:
// alone to the pinned vault2, or with the others in one STClaimBatcher
// transaction that calls vault2 for each. Earnings shows them in a
// "Settlement v2" section beside the v1 tile, which stays as it was. The SDK's
// SnLedgerAccount is mirrored as a plain view; WalletPage.cpp copies the
// fields across.
//
// Capture the flag (sn CAPTURETHEFLAG.md §10): from epoch 8 the operator runs a
// second pool beside its provider pool, whose auction in each country pays its
// winners into the coldkey's account there, claimed from vault2 like any
// pool's. The ledger lists that account beside the provider pool's with its
// kind (SnLedgerAccount.Kind), and names a pool it cannot read just now in
// UnavailablePools rather than failing the whole answer. Each account is
// titled by its pool in the store's words ("Capture the flag", and "Provider
// pool" for the one operator pool beside it) and says its own status; an
// unavailable pool's row says so in place of its balances, never a zero, and
// never hides the others; and one action claims both pools, since the SDK's
// SnLedgerClaim claims every pool, two or more accounts in one batch
// transaction where the chain settings name a batcher and the gas key can pay
// for it, else each alone, the largest first. A server from before capture the
// flag sends no kind, which the SDK reads as a pool, and no unavailable pools,
// so the coldkey has one account there, titled and claimed as before.
//
// Below the claim minimum (owner, 2026-11-11, in every app): any account whose
// claimable is under the claim relay's transfer floor says so, alone or beside
// others, since no claim moves it until it clears the floor; it waits in
// vault2. The claim never counts it: Claim, its amount and Claim both are the
// accounts above the floor, so this device never asks for a claim the chain
// will refuse. The floor is the relay's estimate at its last price; the SDK's
// preflight refusing an account under the runtime's floor (not_claimable,
// AmountTooLow) says the same, never a failure.

// The pool an account is in (the SDK's SnLedgerPoolKind*).
enum class PoolKind {
  Pool,  // "pool": an operator's provider pool, and any kind this build does not know
  Ctf,   // "ctf": the operator's capture the flag pool
};

// The SDK's kind as the plain one: "ctf" is capture the flag, anything else a
// pool (the SDK reads an entry without a kind as one, as the server meant).
PoolKind PoolKindFrom(std::string_view kind);

// A capture the flag pool's noId is 2^32 plus the operator's (the SDK's
// SnLedgerCtfPoolIdBase): the kind of an account the claim reports that the
// page's read did not list, as the SDK reads an entry without a kind.
inline constexpr int64_t kCtfPoolIdBase = int64_t{1} << 32;
PoolKind PoolKindOfNoId(int64_t noId);

// The store key that titles an account: its block on the tile, its row in the
// claim dialog and in the claim sheet. sn_ctf_pool for capture the flag;
// sn_ledger_provider_pool for an operator pool while `providerPool` (the pools
// in view name it so: NamesProviderPool); else sn_ledger_pool ("Pool {}" with
// the pool's noId), as before capture the flag.
std::string PoolTitleKey(PoolKind kind, bool providerPool);

// One pool in view, read or unavailable, which names the others.
struct PoolInView {
  int64_t noId = 0;
  PoolKind kind = PoolKind::Pool;
};

// The operator's pool reads "Provider pool": capture the flag's pool is in
// view beside exactly one operator pool. With more than one operator pool, or
// none of capture the flag's, each keeps "Pool {}" (Android's
// snLedgerPoolName, and Linux's).
bool NamesProviderPool(const std::vector<PoolInView>& pools);

// Claim both (sn_ledger_claim_both): the accounts the claim moves now are
// exactly two, the operator pool's and capture the flag's, which the one claim
// sends together (Android's snLedgerClaimsBoth). `movable` are their pools.
bool ClaimsBothPools(const std::vector<PoolKind>& movable);

// One v2 account (the SDK's SnLedgerAccount). Amounts are rao.
struct LedgerAccount {
  int64_t noId = 0;          // the operator pool
  PoolKind kind = PoolKind::Pool;
  int64_t index = 0;         // the account's position in the pool's tree
  int64_t finalizedRao = 0;  // the cumulative in the latest finalized root
  int64_t claimableRao = 0;  // finalized less paid: what a claim transfers now
  int64_t paidRao = 0;       // vault2's claimed watermark, already transferred
  bool pending = false;      // a committed root that is not finalized holds the account
  int64_t pendingRao = 0;    // its cumulative in that root

  bool operator==(const LedgerAccount&) const = default;
};

// A pool the ledger could not read just now (the SDK's SnLedgerUnavailablePool):
// whether the coldkey has an account there is unknown.
struct UnavailablePool {
  int64_t noId = 0;
  PoolKind kind = PoolKind::Pool;

  bool operator==(const UnavailablePool&) const = default;
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
// root that is not finalized yet raises the account. An unavailable pool's
// block is its title and sn_ledger_pool_unavailable, with no figure.
struct LedgerAccountView {
  int64_t noId = 0;
  PoolKind kind = PoolKind::Pool;
  // the block's title (PoolTitleKey) while the section titles its blocks, else ""
  std::string titleKey;
  // the pool could not be read: sn_ledger_pool_unavailable in place of the figures
  bool unavailable = false;
  int64_t claimableRao = 0;
  // the claimable is under the transfer floor, under which the chain refuses a
  // claim: sn_ledger_pool_under_floor under the figure, on a lone account as on
  // one beside others, and the claim does not count it
  bool underFloor = false;
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
  // each block names its pool (its titleKey): more than one block, or capture
  // the flag's, which is never a lone figure that reads as the provider pool's
  bool poolTitles = false;
  std::vector<LedgerAccountView> accounts;
  LedgerNote note = LedgerNote::None;
  bool showClaim = false;  // Claim, which opens the v2 claim dialog
  bool claimEnabled = false;
  // Claim reads sn_ledger_claim_both: the accounts with something to claim
  // above the transfer floor are exactly the operator pool's and capture the
  // flag's (ClaimsBothPools)
  bool claimBoth = false;

  bool operator==(const LedgerSectionView&) const = default;
};

// The section, from whether a coldkey is set, whether vault2 is configured
// (the chain settings' Vault2Address), the read and whether this device can
// send a claim (WalletPage::CanClaim). Hidden without a coldkey: accounts are a
// coldkey's. The accounts show while a read is Ready or Loading, never after a
// failed one. With vault2 configured the section always shows, with its Claim:
// the accounts, loading, unavailable, the session note or no account yet; Claim
// is enabled while an account on screen has something to claim above the
// transfer floor and the device can send it. Before vault2 is configured
// (before v2's activation) there is no Claim, and the section shows only when
// the read returned accounts, so a ledger that is not served yet says nothing.
//
// `unavailable` are the read's unavailable pools, each a block of its own in
// its place by noId (the ledger lists accounts by ascending noId), shown like
// the accounts; whether the coldkey has an account there is unknown, so no
// account yet is not said beside one. `transferFloorRao` is the claim relay's
// transfer floor in rao of α, 0 when unknown: any account with something to
// claim under it, lone or beside others, is under the floor.
LedgerSectionView LedgerSectionFor(bool hasColdkey, bool vault2Configured, LedgerFetch fetch,
                                   const std::vector<LedgerAccount>& accounts, bool canClaim,
                                   const std::vector<UnavailablePool>& unavailable = {},
                                   int64_t transferFloorRao = 0);

// The claimable is under a known transfer floor (`transferFloorRao`, 0 when
// unknown), under which the chain refuses a claim: no claim moves it until it
// rises.
bool UnderTransferFloor(int64_t claimableRao, int64_t transferFloorRao);

// ---- the v2 claim dialog
//
// The dialog lists the accounts with something to claim and moves each along
// with the SDK's SnLedgerClaimCallback: Sent with the transaction (the explorer
// link), Confirmed with the amount of vault2's Claimed event, Failed with what
// the failure says. needs_gas opens the gas key's funding hint, the v1 dialog's
// top-up; already_claimed, a third party's claim for the account (vault2 pays
// the leaf's coldkey whoever sends it), is the account's success. The page
// reads the accounts again after Done.
//
// Its one Claim claims every pool: the SDK sends two or more accounts in one
// batch transaction where it can, reported per account with the same hash (an
// account the batch did not get to, or every account without a batcher or a
// gas key that can pay for the batch, is then sent alone, the largest first),
// and the button says Claim both while the accounts it moves are exactly the
// operator pool's and capture the flag's. An unavailable pool is a row of its
// own, and the claim's ledger_unavailable for a pool (noId, index 0) makes
// that pool's unsent rows unavailable, never a failure or a zero. An account
// under the transfer floor reads Below the claim minimum and is not in the
// claim's amount, and the SDK's preflight refusing it (not_claimable:
// AmountTooLow) reads the same. An account the gas key could not pay for once
// the largest went (needs_gas for the account: nothing was sent for it) is
// still claimable, with the gas key's funding hint. The SDK tries every
// account with something to claim, one the dialog lists under the floor too,
// and the dialog shows what it answers, as Android's, Linux's and Apple's do:
// Below the claim minimum only for AmountTooLow, claimable under the funding
// hint for needs_gas, and Failed with the SDK's message for any other failure.
// No press takes such an account, whatever the SDK said of it, and a press
// lists it Below the claim minimum again until the SDK reports on it (Linux's
// sheet), so the dialog never offers a claim the chain refuses.

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
  PoolKind kind = PoolKind::Pool;
  int64_t amountRao = 0;  // claimable; the Claimed event's amount once confirmed
  LedgerClaimState state = LedgerClaimState::Idle;
  std::string txHash;   // the claim transaction once sent, "" before
  std::string message;  // the SDK's failure, when Failed
  // the pool could not be read, by the page or by the claim: at rest the row
  // says sn_ledger_pool_unavailable in place of its amount, and Claim leaves it out
  bool unavailable = false;
  // the row says sn_ledger_pool_under_floor at rest: the page's estimate put it
  // under the transfer floor and the SDK has not said otherwise since, or the
  // claim's preflight refused it as under the runtime's. Claim, its amount and
  // Claim both leave it out, and they leave out a row under the dialog's floor
  // by its amount whatever it says (needs_gas, Failed)
  bool underFloor = false;

  bool operator==(const LedgerClaimRow&) const = default;
};

// The SnErrorCode a Failed message starts with ("needs_gas: detail" -> "needs_gas").
std::string ClaimFailureCode(const std::string& message);

// The store key a failure shows, for a code the store has a sentence for, or ""
// to show the SDK's message as it came.
std::string ClaimFailureKey(const std::string& message);

class LedgerClaimProgress {
 public:
  // `accounts` as the page last read them: the claimable ones are the rows,
  // and each of the read's `unavailable` pools is a row of its own (its noId,
  // index 0, no amount). `gasTao` is the gas key's balance when known: a known
  // empty key needs gas before anything can be sent. `transferFloorRao` is the
  // claim relay's transfer floor in rao of α, 0 when unknown; any row under it
  // is marked, lone or beside others, as the tile marks it.
  LedgerClaimProgress(const std::vector<LedgerAccount>& accounts, std::optional<double> gasTao,
                      const std::vector<UnavailablePool>& unavailable = {},
                      int64_t transferFloorRao = 0);

  const std::vector<LedgerClaimRow>& rows() const { return rows_; }
  bool sending() const { return sending_; }
  bool needsGas() const { return needsGas_; }
  // a failure the SDK reported for no pool (pool 0): the claim as a whole
  const std::string& failure() const { return failure_; }

  // the rows not confirmed yet, but for an unavailable pool's and one under
  // the transfer floor: what Claim moves. Saturates rather than overflowing on
  // an absurd ledger answer (the endpoint is untrusted).
  int64_t ClaimableRao() const;
  // ClaimableRao is a total to show: a row not claimed yet is one Claim moves,
  // or every row is claimed (or there is none), and the total is a true zero.
  // While the rows left are all under the floor or unavailable, or of an
  // amount not known yet, they say why rather than a zero (Android's dialog).
  bool TotalKnown() const;
  // Claim's button: actions allowed (a device session), something to claim
  // above the transfer floor, gas, and no claim out
  bool CanClaim(bool allowActions) const;
  // Claim's button says Claim both (sn_ledger_claim_both): the rows Claim
  // moves are exactly the operator pool's and capture the flag's
  // (ClaimsBothPools), which the one claim sends together
  bool ClaimsBoth() const;
  // A row's title (PoolTitleKey): every row names its pool, and the one
  // operator pool beside capture the flag's (an account or a pool of the
  // page's read, or a row the claim reported) is the provider pool, as on the
  // tile.
  std::string TitleKey(const LedgerClaimRow& row) const;

  // Claim pressed: every row Claim moves is Sending; an unavailable pool's,
  // which the claim's own read reports on again, and one the preflight refused
  // stay as they are, and one under the dialog's floor by its amount that the
  // last press left claimable or failed reads Below the claim minimum again
  // until the SDK reports on it. False, changing nothing, while a claim is out
  // or with nothing to claim.
  bool Start();
  // The callback's events. The SDK claims every account it verified, which can
  // be one the page's read did not list: that account gets a row, or takes the
  // row of its pool when the page could not read that pool.
  void Sent(int64_t noId, int64_t index, const std::string& txHash);
  void Confirmed(int64_t noId, int64_t index, const std::string& txHash, int64_t amountRao);
  void Failed(int64_t noId, int64_t index, const std::string& message);
  // The claim is over: a row the SDK never reported on is not sending any
  // more. Once the claim's own read answered (an event for an account or a
  // pool, or nothing to claim), a pool the page could not read that the claim
  // said nothing of was read and held nothing to claim, and its row goes
  // (Android's snLedgerClaimSettled).
  void Done();

 private:
  LedgerClaimRow& RowFor(int64_t noId, int64_t index);
  // a row Claim moves (ClaimableRao, TotalKnown, ClaimsBoth, Start)
  bool Movable(const LedgerClaimRow& row) const;

  std::vector<LedgerClaimRow> rows_;
  // the pools of the page's read, every account claimable or not and each
  // pool it could not read, which name the rows
  std::vector<PoolInView> poolsInView_;
  // the claim relay's transfer floor when the dialog opened, 0 without one:
  // no press takes a row under it
  int64_t transferFloorRao_ = 0;
  bool sending_ = false;
  bool needsGas_ = false;
  std::string failure_;
  // this claim's own read of the ledger answered
  bool ledgerRead_ = false;
  // the pools this claim's own read reported unavailable (ledger_unavailable)
  std::vector<int64_t> reportedUnavailable_;
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

// ---- the claim sheet's pool rows
//
// URnetwork's claim relay claims every pool of a coldkey with one Claim now,
// and the SDK's claim status lists each (coldkey, pool) it sees
// (SnClaimStatusResult.Accounts) and the pools it could not read. The claim
// sheet (BittensorKeySheets.h ClaimRelaySheet) draws a row per account under
// its figure, as Android's does and the SDK's interface asks: its title, what
// it holds to claim or what URnetwork's claim paid, and its status. The tile
// of a network with no wallet on this device lists the primary coldkey's
// accounts under the relay's sum (Android's snClaimRelayTileRows).

// One account as the relay sees it (the SDK's SnClaimRelayAccount).
struct RelayAccount {
  std::string coldkeySs58;
  int64_t noId = 0;
  PoolKind kind = PoolKind::Pool;
  int64_t claimableRao = 0;
  // the account's latest claim when URnetwork sent it: "" and 0 for one anyone
  // else sent, or none; the explorer's page of it, "" without one
  std::string claimTxHash;
  std::string claimTxUrl;
  int64_t claimAmountRao = 0;

  bool operator==(const RelayAccount&) const = default;
};

enum class RelayRowStatus {
  Pending,      // pending_data_key: URnetwork claims it
  UnderFloor,   // sn_ledger_pool_under_floor: no claim moves it until it clears the floor
  Claimed,      // claim_confirmed: URnetwork's claim paid it, nothing claimable since
  Unavailable,  // sn_ledger_pool_unavailable, in place of an amount
};

struct RelayRow {
  int64_t noId = 0;
  std::string titleKey;     // PoolTitleKey
  std::string coldkeySs58;  // named after the title while the rows hold more than one coldkey, else ""
  RelayRowStatus status = RelayRowStatus::Pending;
  int64_t amountRao = 0;    // what it holds to claim, or what URnetwork's claim paid; 0 shows none
  std::string txUrl;        // Claimed: its transaction's page, "" without one

  bool operator==(const RelayRow&) const = default;
};

// The claim sheet's rows, in the SDK's order (Android's snClaimRelayRows):
// each account with something to claim, Pending or, under a known transfer
// floor (`transferFloorRao`, rao of α, 0 when unknown), UnderFloor; each
// account URnetwork claimed with nothing claimable since, Claimed; then each
// unavailable pool. A lone account is a row too, so its status shows. Titles
// name the pools of every account of the answer and every unavailable pool
// (Android's snClaimRelayPools).
std::vector<RelayRow> RelayRowsFor(const std::vector<RelayAccount>& accounts,
                                   const std::vector<UnavailablePool>& unavailable,
                                   int64_t transferFloorRao);

// The relay's claimable is a total to show: it adds up the primary coldkey's
// (`coldkeySs58`) accounts, leaving out the pools it could not read, so with
// a pool unavailable and none of the coldkey's accounts read there is nothing
// to add up, and the unavailable rows say so rather than a zero (Android's
// snClaimRelayTotalKnown).
bool RelayTotalKnown(const std::vector<RelayAccount>& accounts, const std::string& coldkeySs58,
                     const std::vector<UnavailablePool>& unavailable);

// The Settlement v2 tile of a network with no wallet on this device: the
// relay's sum of the primary coldkey's accounts and, under it, those accounts.
struct RelayTileView {
  bool showTotal = true;  // RelayTotalKnown: the figure, never a zero for nothing read
  // the primary coldkey's accounts with something to claim, Pending or
  // UnderFloor, once there are two or one is capture the flag's, or beside a
  // pool the relay could not read; then each unavailable pool (Android's
  // snClaimRelayTileRows). The tile leaves Pending to the relay's reason under it.
  std::vector<RelayRow> rows;
  // no rows, and the figure is one account's under the transfer floor:
  // sn_ledger_pool_under_floor under the figure, as a lone block says it
  bool underFloor = false;

  bool operator==(const RelayTileView&) const = default;
};

RelayTileView RelayTileFor(const std::vector<RelayAccount>& accounts, const std::string& coldkeySs58,
                           const std::vector<UnavailablePool>& unavailable, int64_t transferFloorRao);

// ---- the won slots (GET /sn/ctf/client)
//
// The SDK reads capture the flag's wins (SnCtfClient): the network's, with the
// session's network credential as ur.io reads them, else this client's. Its
// UnclaimedWins is every win whose award is not claimed yet, newest first, a
// provider's before an extender's within an epoch. Earnings words each in a
// line of its own with the capture the flag account (owner, 2026-11-11: the
// line shows for every unclaimed win): the network won one of that country's
// slots in that role. A win is a slot, never the flag, which a country's ten
// winners hold together. An answer with an Error says nothing, never "no
// wins". A role a later server adds has no line: the store has no sentence
// for a win in a role it does not name.

enum class CtfRole { Provider, Extender };

struct CtfWin {
  CtfRole role = CtfRole::Provider;
  std::string country;  // lowercase ISO 3166-1 alpha-2, named in the reader's language by the page

  bool operator==(const CtfWin&) const = default;
};

// The line's store key: sn_ctf_won_provider_slot or sn_ctf_won_extender_slot,
// each with the country's name as its one placeholder.
std::string CtfWinKey(const CtfWin& win);

// The lines after a read of the wins (every app, 2026-11-12): a read that
// answered (`read`, CtfWinsReadFrom) replaces them, and one that failed or
// could not be read (std::nullopt) leaves the last good read's lines as they
// were: it never blanks them. Only such a good read that no longer lists a
// win, or a sign-out (a change of device or network), takes a line away, and
// the first read after a sign-in shows none until it answers. True when the
// lines were replaced.
bool ApplyCtfWinsRead(std::vector<CtfWin>& lines, std::optional<std::vector<CtfWin>> read);

// ---- the wins after a claim
//
// The SDK's Done comes once the claim is in a block, but the server says a win
// is claimed only once vault2's claimed at the finalized head covers the
// claim, so the read at Done can still list a win the claim paid. After a
// claim that paid an account (Confirmed, or already_claimed: another sender's
// claim, which vault2 paid to the same coldkey) the page reads the wins at
// Done, as after any claim, and again 30 s, 60 s and 120 s after Done (every
// app's schedule), unless the page is gone or the user signed out. Each read
// replaces the lines.

// When those reads come, after the claim's Done.
inline constexpr std::array<std::chrono::milliseconds, 3> kCtfWinsReadsAfterClaim{
    std::chrono::seconds(30), std::chrono::seconds(60), std::chrono::seconds(120)};

// The claim's events as they came, with `afterDone` called once Done has been
// handed on, told whether the claim paid an account. The SDK may call the
// events from any of its threads, in order, Done last.
LedgerClaimEvents WithClaimPaid(LedgerClaimEvents events, std::function<void(bool paid)> afterDone);

// The reads on that schedule. The page arms a one-shot timer with each wait
// this gives, and its tick hands the wait's token back to Tick.
class CtfWinsReadsAfterClaim {
 public:
  // A wait for the page's timer, and the token its tick hands back.
  struct Wait {
    std::chrono::milliseconds wait{0};
    uint32_t token = 0;

    bool operator==(const Wait&) const = default;
  };

  // The session a claim starts in, for its Done.
  uint32_t Session() const { return session_; }

  // The Done of a claim started in `session`. One that paid an account starts
  // the reads over from this Done and gives the first wait; one that paid
  // nothing gives none and leaves an earlier claim's reads as they are; and so
  // does one that started before a sign-out, whose account has gone.
  std::optional<Wait> Done(bool paid, uint32_t session);

  struct Step {
    bool read = false;          // read the wins now
    std::optional<Wait> next;   // then arm the timer with this

    bool operator==(const Step&) const = default;
  };
  // The timer armed with `token` fired: the read due, and the wait until the
  // one after it. No read once the reads are over or stopped, nor for the tick
  // of a timer since replaced (a later paying claim's Done, a sign-out),
  // though it was on its way already.
  Step Tick(uint32_t token);

  // The user signed out, or the page goes: no read is left, and a claim from
  // before starts none.
  void Stop();

 private:
  // the read the timer waits for, by its place in kCtfWinsReadsAfterClaim; its
  // size while none is
  std::size_t next_ = kCtfWinsReadsAfterClaim.size();
  // the newest wait's token, and the session (one per sign-out)
  uint32_t token_ = 0;
  uint32_t session_ = 0;
};

// ---- at the SDK's boundary (WalletPage.cpp)
//
// Templates over the generated wrapper's types (urnetwork_sdk.hpp's
// urnet::SnLedgerAccount, urnet::SnLedgerUnavailablePool,
// urnet::SnLedgerClaimCallback, urnet::SnClaimRelayAccount,
// urnet::SnClaimStatusResult and urnet::SnCtfClientResult): the spec builds
// them against stand-ins with the wrapper's names and types and, when a header
// is at hand, against the header itself; WalletPage and the claim sheet
// instantiate them with the SDK's own.

// The SDK's SnCtfRole* values.
inline constexpr std::string_view kCtfRoleProvider = "provider";
inline constexpr std::string_view kCtfRoleExtender = "extender";

// The SDK's SnLedgerAccount as the plain view.
template <typename SdkLedgerAccount>
LedgerAccount LedgerAccountFrom(const SdkLedgerAccount& account) {
  LedgerAccount out;
  out.noId = account.no_id;
  out.kind = PoolKindFrom(account.kind);
  out.index = account.index;
  out.finalizedRao = account.finalized_rao;
  out.claimableRao = account.claimable_rao;
  out.paidRao = account.paid_rao;
  out.pending = account.pending;
  out.pendingRao = account.pending_rao;
  return out;
}

// The SDK's SnLedgerUnavailablePool as the plain view.
template <typename SdkUnavailablePool>
UnavailablePool UnavailablePoolFrom(const SdkUnavailablePool& pool) {
  UnavailablePool out;
  out.noId = pool.no_id;
  out.kind = PoolKindFrom(pool.kind);
  return out;
}

// The SDK's unavailable pools of a read (SnLedgerAccountsResult and
// SnClaimStatusResult's UnavailablePools, never nil from the SDK; a list it
// leaves out is none).
template <typename SdkUnavailablePoolList>
std::vector<UnavailablePool> UnavailablePoolsFrom(const std::optional<SdkUnavailablePoolList>& pools) {
  std::vector<UnavailablePool> out;
  if (!pools) return out;
  for (const auto& pool : *pools) out.push_back(UnavailablePoolFrom(pool));
  return out;
}

// The SDK's SnClaimRelayAccount as the plain view.
template <typename SdkRelayAccount>
RelayAccount RelayAccountFrom(const SdkRelayAccount& account) {
  RelayAccount out;
  out.coldkeySs58 = account.coldkey_ss58;
  out.noId = account.no_id;
  out.kind = PoolKindFrom(account.kind);
  out.claimableRao = account.claimable_rao;
  out.claimTxHash = account.claim_tx_hash.value_or(std::string());
  out.claimTxUrl = account.claim_tx_url.value_or(std::string());
  out.claimAmountRao = account.claim_amount_rao.value_or(0);
  return out;
}

// The SDK's SnClaimStatusResult's accounts as the plain views (a list it
// leaves out is none).
template <typename SdkClaimStatus>
std::vector<RelayAccount> RelayAccountsFrom(const SdkClaimStatus& status) {
  std::vector<RelayAccount> out;
  if (!status.accounts) return out;
  for (const auto& account : *status.accounts) out.push_back(RelayAccountFrom(account));
  return out;
}

// One SnCtfWin as the line's win: in a role this build knows and with a
// country to name, else none, and the line never guesses.
template <typename SdkCtfWin>
std::optional<CtfWin> CtfWinOf(const SdkCtfWin& sdk) {
  if (sdk.country.empty()) return std::nullopt;
  CtfWin win;
  if (sdk.role == kCtfRoleProvider) {
    win.role = CtfRole::Provider;
  } else if (sdk.role == kCtfRoleExtender) {
    win.role = CtfRole::Extender;
  } else {
    return std::nullopt;
  }
  win.country = sdk.country;
  return win;
}

// The lines' wins from the SDK's SnCtfClientResult: each of its UnclaimedWins
// in the SDK's order, newest first (CtfWinOf). None for an answer with an
// Error, which says nothing, and none where the answer has no list.
template <typename SdkCtfClientResult>
std::vector<CtfWin> CtfWinsFrom(const SdkCtfClientResult& result) {
  std::vector<CtfWin> wins;
  if (result.error || !result.unclaimed_wins) return wins;
  for (const auto& sdk : *result.unclaimed_wins) {
    if (auto win = CtfWinOf(sdk)) wins.push_back(*win);
  }
  return wins;
}

// A read's wins for the lines (ApplyCtfWinsRead): CtfWinsFrom for an answer
// the SDK could read, none at all (std::nullopt) for one with an Error or
// without the list, which the SDK always sends: a read that failed, which
// keeps the lines.
template <typename SdkCtfClientResult>
std::optional<std::vector<CtfWin>> CtfWinsReadFrom(const SdkCtfClientResult& result) {
  if (result.error || !result.unclaimed_wins) return std::nullopt;
  return CtfWinsFrom(result);
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
