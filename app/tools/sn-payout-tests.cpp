// Executable spec for the SN payout line under the points figure on Earnings
// (App/SnPayoutPresentation.h): which line shows (no coldkey, nothing
// claimable, claimable), the current epoch's end, claim-open and expiry times
// formatted from the SDK's epoch schedule, and - with the Solana panel's
// decision (App/SolanaWalletPresentation.h) - that the final USDC payout line
// shows only while USDC is pending. And settlement v2 (APPS-SPEC.md, the UX
// rules): what the section beside the v1 tile shows for every state of the
// read, configured or not, pending as pending and unavailable never as a zero,
// in the store's words; and how the v2 claim dialog moves along with the SDK's
// claim events. Run against the same sources the app compiles, on any host
// with a C++20 compiler.
//
// The WinUI half (WalletPage's line and v2 section, their Claim and Set coldkey
// actions, the claim dialogs) cannot be built off Windows at all; what is
// verified here is every decision it makes before it touches a XAML object.
//
//   c++ -std=c++20 -I ../src/App sn-payout-tests.cpp ../src/App/SnPayoutPresentation.cpp ../src/App/SolanaWalletPresentation.cpp -o /tmp/sn-payout-tests && /tmp/sn-payout-tests
//
// With URNW_SN_PAYOUT_TESTS_SDK the SDK's v2 account and claim callback are
// the generated header's own (urnet::SnLedgerAccount, read from the json the
// device answers with through the header's conversions, and
// urnet::SnLedgerClaimCallback), which is what WalletPage instantiates the
// boundary templates with. The header needs nlohmann/json; both are system
// includes because the generated code does not build with -Wextra -Werror:
//
//   c++ -std=c++20 -Wall -Wextra -Werror -DURNW_SN_PAYOUT_TESTS_SDK -I ../src/App -isystem <dir of urnetwork_sdk.hpp> -isystem <dir of nlohmann/> sn-payout-tests.cpp ../src/App/SnPayoutPresentation.cpp ../src/App/SolanaWalletPresentation.cpp -o ...
//
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "SnPayoutPresentation.h"
#include "SolanaWalletPresentation.h"

#if defined(URNW_SN_PAYOUT_TESTS_SDK)
#include "urnetwork_sdk.hpp"
#endif

using namespace urnw::snpayout;

namespace {

int gFailures = 0;
int gCases = 0;
std::string gCurrentCase;

void Fail(const std::string& message) {
  ++gFailures;
  std::cout << "  FAIL [" << gCurrentCase << "] " << message << "\n";
}

void Check(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

void CheckEq(const std::string& expected, const std::string& actual, const std::string& what) {
  if (expected != actual) {
    Fail(what + ": expected \"" + expected + "\", got \"" + actual + "\"");
  }
}

struct Case {
  explicit Case(const char* name) {
    gCurrentCase = name;
    ++gCases;
  }
};
#define TEST_CASE(name) Case case_##__LINE__(name)

// The SDK's schedule for an epoch closing 2026-10-13 00:00 UTC on the mainnet
// policy: claims open 14,400 blocks (two days) later, and the share expires at
// the end of epoch e+9 (453,600 blocks after the close, less one block).
constexpr int64_t kEnd = 1'791'849'600'000;
constexpr int64_t kClaimOpen = kEnd + 14'400LL * 12'000;
constexpr int64_t kExpiry = kEnd + (453'600LL - 1) * 12'000;
// 2026-10-06 00:00 UTC, a week before the close
constexpr int64_t kNow = 1'791'244'800'000;

EpochSchedule Schedule() {
  EpochSchedule schedule;
  schedule.epoch = 9;
  schedule.endMillis = kEnd;
  schedule.claimOpenMillis = kClaimOpen;
  schedule.expiryMillis = kExpiry;
  return schedule;
}

// names each time by the millis it was formatted from
std::string Tag(int64_t millis) { return "t" + std::to_string(millis); }

// A v2 account in pool 1: 3.741 alpha finalized, 0.5 of it paid, 3.241 claimable.
LedgerAccount Account() {
  LedgerAccount account;
  account.noId = 1;
  account.index = 42;
  account.finalizedRao = 3'741'000'000;
  account.claimableRao = 3'241'000'000;
  account.paidRao = 500'000'000;
  return account;
}

LedgerAccountView Block(int64_t noId, int64_t claimableRao, int64_t paidRao, int64_t earnedRao,
                        bool showPending = false, int64_t pendingRao = 0) {
  LedgerAccountView block;
  block.noId = noId;
  block.claimableRao = claimableRao;
  block.paidRao = paidRao;
  block.earnedRao = earnedRao;
  block.showPending = showPending;
  block.pendingRao = pendingRao;
  return block;
}

// the section with a coldkey set
LedgerSectionView Section(bool vault2Configured, LedgerFetch fetch,
                          const std::vector<LedgerAccount>& accounts, bool canClaim = true) {
  return LedgerSectionFor(true, vault2Configured, fetch, accounts, canClaim);
}

constexpr LedgerFetch kFetches[] = {LedgerFetch::Loading, LedgerFetch::Ready, LedgerFetch::Failed,
                                    LedgerFetch::NoDevice};

std::string FetchName(LedgerFetch fetch) {
  switch (fetch) {
    case LedgerFetch::Loading:
      return "Loading";
    case LedgerFetch::Ready:
      return "Ready";
    case LedgerFetch::Failed:
      return "Failed";
    case LedgerFetch::NoDevice:
      return "NoDevice";
  }
  return "?";
}

#if defined(URNW_SN_PAYOUT_TESTS_SDK)
using SdkLedgerAccount = urnet::SnLedgerAccount;
using SdkUnavailablePool = urnet::SnLedgerUnavailablePool;
using SdkUnavailablePoolList = urnet::SnLedgerUnavailablePoolList;
using SdkLedgerClaimCallback = urnet::SnLedgerClaimCallback;
using SdkRelayAccount = urnet::SnClaimRelayAccount;
using SdkClaimStatus = urnet::SnClaimStatusResult;
using SdkCtfWin = urnet::SnCtfWin;
using SdkCtfClientResult = urnet::SnCtfClientResult;
using SdkError = urnet::SnError;
#else
// The SDK's v2 account, unavailable pool, claim callback, relay account, claim
// status and capture the flag status, by the generated wrapper's names and
// types.
struct SdkLedgerAccount {
  std::string vault{};
  int64_t no_id{};
  std::string kind{};
  std::string label{};
  int64_t index{};
  int64_t finalized_rao{};
  int64_t claimable_rao{};
  int64_t paid_rao{};
  bool pending{};
  int64_t pending_rao{};
};
struct SdkUnavailablePool {
  std::string vault{};
  int64_t no_id{};
  std::string kind{};
  std::string label{};
};
using SdkUnavailablePoolList = std::vector<SdkUnavailablePool>;
struct SdkLedgerClaimCallback {
  std::function<void(int64_t no_id, int64_t index, std::string tx_hash, int64_t amount_rao)>
      confirmed;
  std::function<void()> done;
  std::function<void(int64_t no_id, int64_t index, std::string message)> failed;
  std::function<void(int64_t no_id, int64_t index, std::string tx_hash)> sent;
};
struct SdkRelayAccount {
  std::string coldkey_ss58{};
  int64_t no_id{};
  std::string kind{};
  std::string label{};
  int64_t index{};
  int64_t claimable_rao{};
  int64_t value_tao_rao{};
  int64_t pending_rao{};
  std::string reason{};
  bool claim_now_available{};
  std::optional<std::string> claim_now_reason;
  std::optional<int64_t> claim_now_next_at_millis;
  std::optional<std::string> claim_tx_hash;
  std::optional<int64_t> claim_amount_rao;
  std::optional<bool> connected;
  std::optional<std::string> action;
  std::optional<std::string> claim_tx_url;
};
struct SdkClaimStatus {
  std::optional<std::string> coldkey_ss58;
  int64_t claimable_rao{};
  int64_t transfer_floor_rao{};
  std::optional<std::vector<SdkRelayAccount>> accounts;
  std::optional<std::vector<SdkUnavailablePool>> unavailable_pools;
};
struct SdkCtfWin {
  int64_t epoch{};
  std::string role{};
  std::string country{};
  int64_t rank{};
  int64_t slot_rao{};
  int64_t pot_share_rao{};
  int64_t amount_rao{};
  bool pending{};
  std::string root_status{};
  std::string coldkey_ss58{};
  std::string client_id{};
  std::string claim_state{};
};
struct SdkError {
  std::string message{};
};
struct SdkCtfClientResult {
  std::string client_id{};
  int64_t as_of_millis{};
  std::optional<std::vector<SdkCtfWin>> wins;
  std::optional<SdkCtfWin> latest_win;
  std::optional<SdkError> error;
  std::optional<std::vector<SdkCtfWin>> unclaimed_wins;
};
#endif

// capture the flag's pool for UR's operator, NO 1 (2^32 + 1)
constexpr int64_t kCtfNoId = 4'294'967'297;

// The coldkey's capture the flag account, beside Account() in UR's provider
// pool: 0.05 alpha finalized, all of it claimable.
LedgerAccount CtfAccount() {
  LedgerAccount account;
  account.noId = kCtfNoId;
  account.kind = PoolKind::Ctf;
  account.index = 9;
  account.finalizedRao = 50'000'000;
  account.claimableRao = 50'000'000;
  return account;
}

UnavailablePool Unavailable(int64_t noId, PoolKind kind) {
  UnavailablePool pool;
  pool.noId = noId;
  pool.kind = kind;
  return pool;
}

// a block of the section with its title and kind
LedgerAccountView Titled(LedgerAccountView block, PoolKind kind, const std::string& titleKey) {
  block.kind = kind;
  block.titleKey = titleKey;
  return block;
}

// an unavailable pool's block
LedgerAccountView UnavailableBlock(int64_t noId, PoolKind kind, const std::string& titleKey) {
  LedgerAccountView block;
  block.noId = noId;
  block.kind = kind;
  block.titleKey = titleKey;
  block.unavailable = true;
  return block;
}

RelayAccount Relay(int64_t noId, PoolKind kind, int64_t claimableRao,
                   const std::string& coldkey = "5COLD") {
  RelayAccount account;
  account.coldkeySs58 = coldkey;
  account.noId = noId;
  account.kind = kind;
  account.claimableRao = claimableRao;
  return account;
}

RelayRow Row(int64_t noId, const std::string& titleKey, RelayRowStatus status, int64_t amountRao,
             const std::string& coldkey = "", const std::string& txUrl = "") {
  RelayRow row;
  row.noId = noId;
  row.titleKey = titleKey;
  row.coldkeySs58 = coldkey;
  row.status = status;
  row.amountRao = amountRao;
  row.txUrl = txUrl;
  return row;
}

// A win of the SDK's (SnCtfWin) in `epoch`, rank 1, not claimed.
SdkCtfWin SdkWin(int64_t epoch, const std::string& role, const std::string& country) {
  SdkCtfWin win;
  win.epoch = epoch;
  win.role = role;
  win.country = country;
  win.rank = 1;
  win.slot_rao = 48'025'660;
  win.pot_share_rao = 28'815'396;
  win.amount_rao = 76'841'056;
  win.root_status = "finalized";
  win.coldkey_ss58 = "5COLD";
  win.client_id = "3f6c2a1e-8d4b-4c7a-9e21-5b0d7f3a9c64";
  win.claim_state = "unclaimed";
  return win;
}

// a claim dialog over `accounts` with half a TAO of gas and the floor
LedgerClaimProgress Dialog(const std::vector<LedgerAccount>& accounts, int64_t transferFloorRao = 0,
                           const std::vector<UnavailablePool>& unavailable = {}) {
  return LedgerClaimProgress(accounts, 0.5, unavailable, transferFloorRao);
}

void CheckRow(const LedgerClaimRow& row, int64_t noId, int64_t index, int64_t amountRao,
              LedgerClaimState state, const std::string& txHash, const std::string& message,
              const std::string& what) {
  Check(row.noId == noId && row.index == index, what + ": the account");
  Check(row.amountRao == amountRao,
        what + ": amount " + std::to_string(row.amountRao) + ", want " + std::to_string(amountRao));
  Check(row.state == state, what + ": state");
  CheckEq(txHash, row.txHash, what + ": tx hash");
  CheckEq(message, row.message, what + ": message");
}

PayoutLineView Line(bool walletKnown, bool hasColdkey, int64_t claimableRao,
                    std::optional<EpochSchedule> schedule, int64_t now = kNow) {
  return PayoutLineFor(walletKnown, hasColdkey, claimableRao, schedule, now, Tag);
}

void CheckTimes(const PayoutLineView& view) {
  Check(view.showTimes, "the times show");
  CheckEq(Tag(kEnd), view.epochEnd, "epoch end");
  CheckEq(Tag(kClaimOpen), view.claimOpen, "claim open");
  CheckEq(Tag(kExpiry), view.expiry, "expiry");
}

// WalletPage's part in the wins after a claim, as it wires them: the claimer's
// Done (WithClaimPaid) reads the wins (LoadCtfClient) and hands `paid` to the
// schedule (ReadCtfWinsAfterClaim), whose one-shot timer waits each wait it
// gives and calls Tick (OnCtfWinsTimer); each answer goes to the lines through
// CtfWinsLines (ApplyCtfClient); a sign-out stops the reads and drops the
// lines (ResetForSignOut). The server is a fake whose next reads answer
// `answers`, one each, then `settled`; std::nullopt is a read that failed.
// Each answer is the network's unless `from` says whose it is, one each.
struct WinsPage {
  CtfWinsReadsAfterClaim schedule;
  std::optional<std::chrono::milliseconds> timer;  // armed with this wait
  uint32_t timerToken = 0;                         // and the token its tick hands back
  std::vector<std::chrono::milliseconds> waits;    // every wait armed
  CtfWinsLines wins;
  std::vector<std::optional<std::vector<CtfWin>>> answers;
  std::vector<CtfAnswer> from;
  std::optional<std::vector<CtfWin>> settled = std::vector<CtfWin>{};
  int reads = 0;
  std::vector<std::string> heard;  // the dialog's events and the page's reads, in order

  const std::vector<CtfWin>& lines() const { return wins.lines(); }
  // the lines an earlier good read gave
  void Seed(std::vector<CtfWin> earlier) { wins.Apply(wins.Start(), std::move(earlier), CtfAnswer::Network); }
  void Read() {
    ++reads;
    heard.push_back("read");
    std::optional<std::vector<CtfWin>> answer = settled;
    if (!answers.empty()) {
      answer = answers.front();
      answers.erase(answers.begin());
    }
    CtfAnswer whose = CtfAnswer::Network;
    if (!from.empty()) {
      whose = from.front();
      from.erase(from.begin());
    }
    wins.Apply(wins.Start(), std::move(answer), whose);
  }
  void Arm(CtfWinsReadsAfterClaim::Wait wait) {
    timer = wait.wait;
    timerToken = wait.token;
    waits.push_back(wait.wait);
  }
  // the claimer, around the dialog's events, in the session the claim starts in
  LedgerClaimEvents Claimer(LedgerClaimEvents dialog = {}) {
    const uint32_t session = schedule.Session();
    return WithClaimPaid(std::move(dialog), [this, session](bool paid) {
      Read();
      if (const auto wait = schedule.Done(paid, session)) Arm(*wait);
    });
  }
  // a tick with `token`: the armed timer's, or one on its way from a timer
  // since replaced
  void Tick(uint32_t token) {
    const CtfWinsReadsAfterClaim::Step step = schedule.Tick(token);
    if (!step.read) return;
    Read();
    if (step.next) Arm(*step.next);
  }
  // the armed timer's wait ran out
  void Fire() {
    timer.reset();
    Tick(timerToken);
  }
  void SignOut() {
    schedule.Stop();
    timer.reset();
    wins.Reset();
  }
};

// WalletPage's v2 claim, as it wires it: the claim in flight is the page's
// (LedgerClaimInFlight), never a dialog's. A dialog (OnClaimV2) draws the
// claim in flight's progress while there is one, else a new one over the
// page's read; its Claim starts the claim on its progress (StartV2Claim). The
// SDK's events come through its callback (LedgerClaimCallbackFor) and
// WithClaimPaid, each posted to the UI thread (`posted`, which Drain runs),
// where it moves the claim's own progress while the page or a dialog holds it
// (OnV2ClaimEvent), and Done ends the claim in flight (OnV2ClaimDone), then the
// page reads again. A sign-out drops it (ResetForSignOut), and a dialog up from
// before keeps its progress. The section's Claim is RebuildV2Section's.
struct ClaimPage {
  std::vector<LedgerAccount> accounts;  // the page's last read
  std::vector<UnavailablePool> unavailable;
  int64_t floor = 0;
  LedgerClaimInFlight claim;
  // the open dialog's progress, null while no dialog is up
  std::shared_ptr<LedgerClaimProgress> dialog;
  std::vector<std::function<void()>> posted;  // the UI thread's queue
  std::vector<SdkLedgerClaimCallback> sdk;    // each claim the SDK was asked for
  int redraws = 0;                            // an open dialog drew the claim again
  std::vector<bool> reads;                    // each Done's reads, told whether it paid
  bool v1InFlight = false;                    // the page's v1 claim is in flight

  // OnClaimV2
  void Open() {
    dialog = claim.Progress();
    if (!dialog) dialog = std::make_shared<LedgerClaimProgress>(accounts, 0.5, unavailable, floor);
  }
  void Close() { dialog.reset(); }
  // the dialog's Claim, held while a v1 claim is in flight (Held), which
  // StartV2Claim refuses too: the gas key sends one claim at a time
  bool ClaimEnabled() const { return dialog && dialog->CanClaim(true) && !v1InFlight; }
  // the dialog's Claim: ClaimLedgerSheet::StartClaim, then StartV2Claim
  bool Press() {
    if (!dialog || dialog->sending()) return false;
    if (v1InFlight) return false;
    const std::optional<uint64_t> started = claim.Start(dialog);
    if (!started) return false;
    const uint64_t ticket = *started;
    const std::weak_ptr<LedgerClaimProgress> rows = dialog;
    LedgerClaimEvents events;
    events.sent = [this, rows](int64_t noId, int64_t index, std::string txHash) {
      posted.push_back([this, rows, noId, index, txHash] {
        Event(rows, [&](LedgerClaimProgress& progress) { progress.Sent(noId, index, txHash); });
      });
    };
    events.confirmed = [this, rows](int64_t noId, int64_t index, std::string txHash, int64_t amountRao) {
      posted.push_back([this, rows, noId, index, txHash, amountRao] {
        Event(rows, [&](LedgerClaimProgress& progress) { progress.Confirmed(noId, index, txHash, amountRao); });
      });
    };
    events.failed = [this, rows](int64_t noId, int64_t index, std::string message) {
      posted.push_back([this, rows, noId, index, message] {
        Event(rows, [&](LedgerClaimProgress& progress) { progress.Failed(noId, index, message); });
      });
    };
    events.done = [this, rows, ticket] { posted.push_back([this, rows, ticket] { Done(rows, ticket); }); };
    events = WithClaimPaid(std::move(events), [this](bool paid) { posted.push_back([this, paid] { reads.push_back(paid); }); });
    sdk.push_back(LedgerClaimCallbackFor<SdkLedgerClaimCallback>(events));
    return true;
  }
  // OnV2ClaimEvent and OnV2ClaimDone
  void Event(const std::weak_ptr<LedgerClaimProgress>& rows, const std::function<void(LedgerClaimProgress&)>& step) {
    const auto progress = rows.lock();
    if (!progress) return;
    step(*progress);
    if (dialog) ++redraws;
  }
  void Done(const std::weak_ptr<LedgerClaimProgress>& rows, uint64_t ticket) {
    if (const auto progress = rows.lock()) progress->Done();
    claim.End(ticket);
    if (dialog) ++redraws;
  }
  // the UI thread runs what was posted, in order
  void Drain() {
    while (!posted.empty()) {
      const std::function<void()> step = posted.front();
      posted.erase(posted.begin());
      step();
    }
  }
  void SignOut() { claim.Drop(); }
  LedgerSectionView Section() const {
    return LedgerSectionFor(true, true, LedgerFetch::Ready, accounts, true, unavailable, floor, claim.InFlight());
  }
};

}  // namespace

int main() {
  {
    TEST_CASE("no coldkey asks for one");
    for (int64_t rao : {int64_t{0}, int64_t{3'241'000'000}}) {
      const PayoutLineView view = Line(true, false, rao, Schedule());
      Check(view.kind == LineKind::SetColdkey, "SetColdkey");
      Check(!view.showTimes && !view.showClaim, "no times and no Claim without a coldkey");
    }
    Check(Line(true, false, 0, std::nullopt).kind == LineKind::SetColdkey, "SetColdkey without a schedule");
  }
  {
    TEST_CASE("nothing shows while the coldkey is unknown");
    Check(Line(false, false, 0, Schedule()) == PayoutLineView{}, "hidden");
    Check(Line(false, true, 3'241'000'000, Schedule()) == PayoutLineView{}, "hidden with a wallet read pending");
  }
  {
    TEST_CASE("nothing claimable explains the schedule without Claim");
    const PayoutLineView view = Line(true, true, 0, Schedule());
    Check(view.kind == LineKind::Schedule, "Schedule");
    CheckTimes(view);
    Check(!view.showClaim, "no Claim");
  }
  {
    TEST_CASE("something claimable adds Claim");
    const PayoutLineView view = Line(true, true, 3'241'000'000, Schedule());
    Check(view.kind == LineKind::Schedule, "Schedule");
    CheckTimes(view);
    Check(view.showClaim, "Claim");
  }
  {
    TEST_CASE("without the schedule the explanation stands alone");
    const PayoutLineView view = Line(true, true, 1, std::nullopt);
    Check(view.kind == LineKind::Schedule && !view.showTimes, "no times");
    Check(view.epochEnd.empty() && view.claimOpen.empty() && view.expiry.empty(), "no time texts");
    Check(view.showClaim, "Claim still shows");
  }
  {
    TEST_CASE("an ended epoch waits for the next read");
    Check(!Line(true, true, 0, Schedule(), kEnd).showTimes, "no times at the close");
    CheckTimes(Line(true, true, 0, Schedule(), kEnd - 1));
  }
  {
    TEST_CASE("the times are local dates and times from the epoch data");
    CheckEq("2026-10-13 00:00", FormatScheduleTime(kEnd, 0), "end, UTC");
    CheckEq("2026-10-15 00:00", FormatScheduleTime(kClaimOpen, 0), "claim open, UTC");
    CheckEq("2026-12-14 23:59", FormatScheduleTime(kExpiry, 0), "expiry, UTC");
    // Berlin in summer time, then in winter time by the expiry
    CheckEq("2026-10-13 02:00", FormatScheduleTime(kEnd, 120), "end, Berlin");
    CheckEq("2026-12-15 00:59", FormatScheduleTime(kExpiry, 60), "expiry, Berlin");
    // west of UTC the close is the evening before
    CheckEq("2026-10-12 17:00", FormatScheduleTime(kEnd, -420), "end, Los Angeles");
    CheckEq("1969-12-31 23:59", FormatScheduleTime(-60'000, 0), "before the epoch");

    const PayoutLineView view = PayoutLineFor(
        true, true, 0, Schedule(), kNow, [](int64_t millis) { return FormatScheduleTime(millis, 120); });
    CheckEq("2026-10-13 02:00", view.epochEnd, "line end");
    CheckEq("2026-10-15 02:00", view.claimOpen, "line claim open");
    CheckEq("2026-12-15 01:59", view.expiry, "line expiry (the page's offset is per instant)");
  }
  {
    TEST_CASE("the final USDC payout line shows only while USDC is pending");
    using namespace urnw::solana;
    LegacyReads reads;
    reads.wallets = true;
    reads.payout = true;
    reads.payments = true;
    const SolanaPanelView pending = SolanaPanelFor(LegacyState::Ready, reads, std::nullopt, 3'870'000'000);
    Check(pending.showWaitingLine && !pending.showCard, "the line, with no payout wallet");
    CheckEq("3.87", pending.pendingUsd, "the amount");
    Check(!SolanaPanelFor(LegacyState::Ready, reads, std::nullopt, 0).showWaitingLine, "nothing pending");
    Check(!SolanaPanelFor(LegacyState::Ready, reads, std::nullopt, 4'000'000).showWaitingLine, "a sub-cent remainder");
    Check(!SolanaPanelFor(LegacyState::Loading, reads, std::nullopt, 3'870'000'000).showWaitingLine, "not before the reads");
  }

  // ---- settlement v2 (APPS-SPEC.md, UX rules) --------------------------------
  {
    TEST_CASE("v2: no section without a coldkey");
    for (const LedgerFetch fetch : kFetches) {
      for (const bool configured : {false, true}) {
        Check(LedgerSectionFor(false, configured, fetch, {Account()}, true) == LedgerSectionView{},
              "hidden, " + FetchName(fetch) + (configured ? ", configured" : ""));
      }
    }
  }
  {
    TEST_CASE("v2: an account shows Claimable, Paid and Earned, with Claim");
    const LedgerSectionView view = Section(true, LedgerFetch::Ready, {Account()});
    Check(view.shown && !view.loading && !view.poolTitles, "shown, one block, not loading");
    Check(view.note == LedgerNote::None, "no note");
    Check(view.accounts == std::vector<LedgerAccountView>{Block(1, 3'241'000'000, 500'000'000,
                                                                3'741'000'000)},
          "claimable 3.241, paid 0.5, earned the finalized 3.741, nothing pending");
    Check(view.showClaim && view.claimEnabled, "Claim, enabled");
  }
  {
    TEST_CASE("v2: pending shows as pending, awaiting finalization");
    LedgerAccount account = Account();
    account.pending = true;
    account.pendingRao = 3'861'000'000;
    Check(Section(true, LedgerFetch::Ready, {account}).accounts ==
              std::vector<LedgerAccountView>{
                  Block(1, 3'241'000'000, 500'000'000, 3'741'000'000, true, 120'000'000)},
          "+0.12 pending over the finalized 3.741, which stays the earned figure");
    account.pendingRao = account.finalizedRao;
    Check(!Section(true, LedgerFetch::Ready, {account}).accounts[0].showPending,
          "a pending root that does not raise the account");
    account.pendingRao = account.finalizedRao - 1;
    Check(!Section(true, LedgerFetch::Ready, {account}).accounts[0].showPending,
          "a pending cumulative below the finalized one");
    account.pending = false;
    account.pendingRao = account.finalizedRao + 1;
    Check(!Section(true, LedgerFetch::Ready, {account}).accounts[0].showPending,
          "no pending root");

    // only in the pending root so far: zeros, which are real, and the pending line
    LedgerAccount fresh;
    fresh.noId = 1;
    fresh.index = 7;
    fresh.pending = true;
    fresh.pendingRao = 50'000'000;
    const LedgerSectionView view = Section(true, LedgerFetch::Ready, {fresh});
    Check(view.accounts == std::vector<LedgerAccountView>{Block(1, 0, 0, 0, true, 50'000'000)},
          "pending only");
    Check(view.showClaim && !view.claimEnabled, "nothing to claim yet");
  }
  {
    TEST_CASE("v2: an unavailable ledger says so, never a zero");
    const LedgerSectionView view = Section(true, LedgerFetch::Failed, {Account()});
    Check(view.shown && view.note == LedgerNote::Unavailable, "unavailable");
    Check(view.accounts.empty(), "no figure, not even the last read's");
    Check(view.showClaim && !view.claimEnabled, "Claim stays disabled");
    Check(!view.loading, "not loading");
  }
  {
    TEST_CASE("v2: loading keeps the last read's accounts");
    const LedgerSectionView first = Section(true, LedgerFetch::Loading, {});
    Check(first.shown && first.loading && first.accounts.empty(), "the first read");
    Check(first.note == LedgerNote::None, "no note while loading");
    Check(first.showClaim && !first.claimEnabled, "Claim waits for the accounts");
    const LedgerSectionView again = Section(true, LedgerFetch::Loading, {Account()});
    Check(again.loading && again.accounts.size() == 1, "a reload keeps the accounts on screen");
    Check(again.claimEnabled, "and their Claim");
  }
  {
    TEST_CASE("v2: no account yet, and no device session");
    const LedgerSectionView none = Section(true, LedgerFetch::Ready, {});
    Check(none.shown && none.note == LedgerNote::NoAccount && none.accounts.empty(), "no account");
    Check(none.showClaim && !none.claimEnabled, "nothing to claim");
    const LedgerSectionView noDevice = Section(true, LedgerFetch::NoDevice, {Account()}, false);
    Check(noDevice.shown && noDevice.note == LedgerNote::NeedsSession, "the session note");
    Check(noDevice.accounts.empty(), "no figure without a read");
    Check(noDevice.showClaim && !noDevice.claimEnabled, "Claim disabled");
  }
  {
    TEST_CASE("v2: each note says it in the store's words");
    // the words the Android and Linux apps use: unavailable is the shared
    // sn_ledger_unavailable, the empty state the Linux tile's, and the missing
    // session the v1 tile's
    CheckEq("sn_ledger_unavailable", LedgerNoteKey(LedgerNote::Unavailable), "unavailable");
    CheckEq("earnings_claims_need_session", LedgerNoteKey(LedgerNote::NeedsSession), "no session");
    CheckEq("no_claims_yet_linux", LedgerNoteKey(LedgerNote::NoAccount), "no account yet");
    CheckEq("", LedgerNoteKey(LedgerNote::None), "no note: no line");
    CheckEq("sn_ledger_unavailable", LedgerNoteKey(Section(true, LedgerFetch::Failed, {}).note),
            "a failed read's line");
  }
  {
    TEST_CASE("v2: before vault2 is configured, balances only and no Claim");
    const LedgerSectionView view = Section(false, LedgerFetch::Ready, {Account()});
    Check(view.shown && view.accounts.size() == 1, "the balances the ledger returned");
    Check(!view.showClaim && !view.claimEnabled, "no Claim");
    Check(Section(false, LedgerFetch::Loading, {Account()}).shown, "kept while reloading");
    Check(Section(false, LedgerFetch::Ready, {}) == LedgerSectionView{}, "no account: nothing");
    Check(Section(false, LedgerFetch::Loading, {}) == LedgerSectionView{}, "loading: nothing");
    Check(Section(false, LedgerFetch::Failed, {Account()}) == LedgerSectionView{},
          "a ledger not served yet: nothing");
    Check(Section(false, LedgerFetch::NoDevice, {Account()}) == LedgerSectionView{},
          "no device: nothing");
  }
  {
    TEST_CASE("v2: Claim needs something claimable and a device");
    Check(!Section(true, LedgerFetch::Ready, {Account()}, false).claimEnabled, "no device");
    LedgerAccount paid = Account();
    paid.claimableRao = 0;
    paid.paidRao = paid.finalizedRao;
    Check(!Section(true, LedgerFetch::Ready, {paid}).claimEnabled, "all paid");
    LedgerAccount other = Account();
    other.noId = 2;
    other.index = 3;
    other.finalizedRao = 900'000'000;
    other.claimableRao = 900'000'000;
    other.paidRao = 0;
    const LedgerSectionView two = Section(true, LedgerFetch::Ready, {paid, other});
    Check(two.poolTitles, "two pools: each block names its pool");
    Check(two.accounts ==
              std::vector<LedgerAccountView>{
                  Titled(Block(1, 0, 3'741'000'000, 3'741'000'000), PoolKind::Pool, "sn_ledger_pool"),
                  Titled(Block(2, 900'000'000, 0, 900'000'000), PoolKind::Pool, "sn_ledger_pool")},
          "one block per account, in the ledger's order, each Pool {}");
    Check(two.claimEnabled, "one claimable account is enough");
    Check(!two.claimBoth, "two provider pools are not both pools");
  }
  {
    TEST_CASE("v2: an absurd ledger answer overflows neither the section nor the dialog");
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    LedgerAccount huge = Account();
    huge.finalizedRao = kMax;
    huge.claimableRao = kMax;
    huge.paidRao = 0;
    LedgerAccount other = huge;
    other.noId = 2;
    Check(Section(true, LedgerFetch::Ready, {huge, other}).claimEnabled,
          "Claim, without summing the accounts");
    LedgerClaimProgress progress({huge, other}, 0.5);
    Check(progress.ClaimableRao() == kMax, "the dialog's total saturates");
    Check(progress.CanClaim(true) && progress.Start(), "and Claim still sends");
    progress.Confirmed(1, 42, "0xabc", kMax);
    Check(progress.ClaimableRao() == kMax, "the other account is left");
  }
  {
    TEST_CASE("v2 claim: the dialog lists the claimable accounts");
    LedgerAccount paid = Account();
    paid.noId = 3;
    paid.claimableRao = 0;
    LedgerAccount other = Account();
    other.noId = 2;
    other.index = 3;
    other.claimableRao = 900'000'000;
    const LedgerClaimProgress progress({Account(), paid, other}, 0.5);
    Check(progress.rows().size() == 2, "two rows");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Idle, "", "", "pool 1");
    CheckRow(progress.rows()[1], 2, 3, 900'000'000, LedgerClaimState::Idle, "", "", "pool 2");
    Check(progress.ClaimableRao() == 4'141'000'000, "the total");
    Check(!progress.sending() && !progress.needsGas() && progress.failure().empty(), "at rest");
    Check(progress.CanClaim(true), "Claim enabled");
    Check(!progress.CanClaim(false), "not without a device session");
    Check(!LedgerClaimProgress({paid}, 0.5).CanClaim(true), "nothing to claim");
  }
  {
    TEST_CASE("v2 claim: Sent with the transaction, Confirmed with the Claimed amount, then Done");
    LedgerAccount other = Account();
    other.noId = 2;
    other.index = 3;
    other.claimableRao = 900'000'000;
    LedgerClaimProgress progress({Account(), other}, std::nullopt);
    Check(progress.Start(), "Claim starts");
    Check(progress.sending() && !progress.CanClaim(true), "one claim at a time");
    Check(!progress.Start(), "a second press sends nothing");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Sending, "", "",
             "sending");
    progress.Sent(1, 42, "0xabc");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Sent, "0xabc", "", "sent");
    progress.Confirmed(1, 42, "0xabc", 3'241'000'001);
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'001, LedgerClaimState::Confirmed, "0xabc", "",
             "confirmed with the event's amount");
    Check(progress.ClaimableRao() == 900'000'000, "a confirmed account leaves the total");
    progress.Done();
    Check(!progress.sending(), "done");
    CheckRow(progress.rows()[1], 2, 3, 900'000'000, LedgerClaimState::Idle, "", "",
             "never reported on");
    Check(progress.CanClaim(true), "the rest can be claimed again");
    Check(progress.Start(), "again");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'001, LedgerClaimState::Confirmed, "0xabc", "",
             "a confirmed account is not sent again");
    progress.Confirmed(2, 3, "0xdef", 0);
    CheckRow(progress.rows()[1], 2, 3, 900'000'000, LedgerClaimState::Confirmed, "0xdef", "",
             "an event without an amount keeps the claimable");
    progress.Done();
    Check(progress.ClaimableRao() == 0 && !progress.CanClaim(true) && !progress.Start(),
          "nothing left");
  }
  {
    TEST_CASE("v2 claim: needs_gas opens the gas top-up, and the account stays claimable");
    // the SDK's refusal before it sends (snNeedsGasMessage): nothing was sent
    // for the account
    const std::string needsGas =
        "needs_gas: this claim needs up to 0.0173 TAO of gas, the gas key 0x00aa has 0.0050 TAO; "
        "send TAO to 5GAS";
    LedgerClaimProgress progress({Account()}, 0.005);
    Check(!progress.needsGas() && progress.CanClaim(true),
          "a key with some TAO tries: the SDK's preflight decides");
    Check(progress.Start(), "Claim starts");
    progress.Failed(1, 42, needsGas);
    Check(progress.needsGas(), "the funding hint shows");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Idle, "", "",
             "still claimable, never failed");
    Check(progress.ClaimableRao() == 3'241'000'000, "still in the claim's amount");
    CheckEq("add_tao_for_gas", ClaimFailureKey(needsGas), "the hint's words");
    progress.Done();
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Idle, "", "",
             "claimable after the claim");
    Check(!progress.CanClaim(true), "Claim waits for gas");
    Check(LedgerClaimProgress({Account()}, 0.0).needsGas(), "a known empty gas key");
    Check(!LedgerClaimProgress({Account()}, 0.0).CanClaim(true), "cannot claim");
    Check(!LedgerClaimProgress({Account()}, std::nullopt).needsGas(),
          "an unknown balance does not block");
  }
  {
    TEST_CASE("v2 claim: already_claimed is the account's success");
    LedgerClaimProgress progress({Account()}, 0.5);
    Check(progress.Start(), "Claim starts");
    progress.Failed(1, 42, "already_claimed: NothingToClaim after a third party's claim");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Confirmed, "", "",
             "claimed");
    Check(!progress.needsGas() && progress.failure().empty(), "no failure");
    Check(progress.ClaimableRao() == 0, "nothing left to claim");
  }
  {
    TEST_CASE("v2 claim: any other failure shows its message, and a retry clears it");
    LedgerClaimProgress progress({Account()}, 0.5);
    Check(progress.Start(), "Claim starts");
    progress.Sent(1, 42, "0xabc");
    progress.Failed(1, 42, "claim_failed: TooManyStakingHotkeys");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Failed, "0xabc",
             "claim_failed: TooManyStakingHotkeys", "failed");
    CheckEq("", ClaimFailureKey(progress.rows()[0].message), "the message as it came");
    Check(!progress.needsGas(), "no gas hint");
    progress.Done();
    Check(progress.CanClaim(true), "still claimable");
    Check(progress.Start(), "retry");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Sending, "0xabc", "",
             "sending again");
  }
  {
    TEST_CASE("v2 claim: a failure for no pool is the dialog's");
    LedgerClaimProgress progress({Account()}, 0.5);
    Check(progress.Start(), "Claim starts");
    progress.Failed(0, 0, "ledger_unavailable: the ledger answered 503");
    CheckEq("ledger_unavailable: the ledger answered 503", progress.failure(), "the dialog's line");
    Check(progress.rows().size() == 1, "no row for it");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Sending, "", "",
             "untouched");
    progress.Done();
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Idle, "", "", "at rest");
    Check(progress.Start() && progress.failure().empty(), "a new claim clears it");
  }
  {
    TEST_CASE("v2 claim: an account the page did not list gets a row");
    LedgerClaimProgress progress({Account()}, 0.5);
    Check(progress.Start(), "Claim starts");
    progress.Sent(2, 7, "0xdef");
    Check(progress.rows().size() == 2, "a second row");
    CheckRow(progress.rows()[1], 2, 7, 0, LedgerClaimState::Sent, "0xdef", "", "sent");
    progress.Confirmed(2, 7, "0xdef", 900'000'000);
    CheckRow(progress.rows()[1], 2, 7, 900'000'000, LedgerClaimState::Confirmed, "0xdef", "",
             "confirmed");
    progress.Failed(3, 1, "proof_mismatch: the root does not match");
    CheckRow(progress.rows()[2], 3, 1, 0, LedgerClaimState::Failed, "",
             "proof_mismatch: the root does not match", "failed");
  }
  {
    TEST_CASE("v2 claim: the failure words");
    CheckEq("needs_gas", ClaimFailureCode("needs_gas: detail"), "code");
    CheckEq("claim_failed", ClaimFailureCode("claim_failed"), "a bare code");
    CheckEq("proof_mismatch", ClaimFailureCode("proof_mismatch : detail"), "trailing space");
    CheckEq("add_tao_for_gas", ClaimFailureKey("needs_gas: detail"), "needs_gas");
    CheckEq("chain_rpc_unreachable", ClaimFailureKey("chain_rpc_unreachable: dial tcp"),
            "rpc unreachable");
    CheckEq("chain_rpc_unreachable", ClaimFailureKey("chain_rpc_error: execution reverted"),
            "rpc error");
    CheckEq("connect_wallet_first", ClaimFailureKey("connect_wallet_first: no coldkey"),
            "no coldkey");
    CheckEq("claim_failed", ClaimFailureKey(""), "no message");
    CheckEq("claim_failed", ClaimFailureKey("claim_failed"), "no detail");
    for (const char* message :
         {"claim_failed: TooManyStakingHotkeys", "not_claimable: below the transfer floor",
          "proof_mismatch: the root does not match", "ledger_unavailable: 503"}) {
      CheckEq("", ClaimFailureKey(message), std::string("as it came: ") + message);
    }
  }
  {
    TEST_CASE("v2 claim: Sent links the transaction on the explorer");
    CheckEq("https://evm.taostats.io/tx/0xabc",
            ExplorerTxLink("https://evm.taostats.io/tx/%s", "0xabc"), "the hash in place of %s");
    CheckEq("https://explorer.example/tx/0xabc",
            ExplorerTxLink("https://explorer.example/tx", "0xabc"), "appended as a path segment");
    CheckEq("https://explorer.example/tx/0xabc",
            ExplorerTxLink("https://explorer.example/tx/", "0xabc"), "after a trailing slash");
    CheckEq("", ExplorerTxLink("", "0xabc"), "no explorer configured: no link");
    CheckEq("", ExplorerTxLink("https://evm.taostats.io/tx/%s", ""), "nothing sent yet: no link");
  }
  {
    TEST_CASE("v2 claim: the top-up asks for what a v2 claim's preflight needs");
    // the SDK's gas limit is eth_estimateGas (about 2.51M for a claim on runtime
    // 475) plus a quarter, priced at 1.1 x the gas price (legacy, the mainnet
    // default); at the plan's 5 gwei:
    const double preflightTao = 2'510'000.0 * 1.25 * 1.1 * 5e9 / 1e18;
    Check(preflightTao <= kLedgerClaimGasTao,
          "covers one claim's " + std::to_string(preflightTao) + " TAO");
    Check(kLedgerClaimGasTao == 0.02, "0.02 TAO, which the v1 dialog asks for too");
  }

  // ---- capture the flag (CAPTURETHEFLAG.md §10) ------------------------------
  {
    TEST_CASE("ctf: the kinds and the titles");
    Check(PoolKindFrom("ctf") == PoolKind::Ctf, "ctf is capture the flag");
    for (const char* kind : {"pool", "", "CTF", "provider", "extenders"}) {
      Check(PoolKindFrom(kind) == PoolKind::Pool, std::string("a pool: \"") + kind + "\"");
    }
    Check(PoolKindOfNoId(kCtfNoId) == PoolKind::Ctf, "2^32 + 1 is capture the flag's");
    Check(PoolKindOfNoId(kCtfPoolIdBase) == PoolKind::Ctf, "from 2^32");
    Check(PoolKindOfNoId(kCtfPoolIdBase - 1) == PoolKind::Pool, "under 2^32 a pool's");
    Check(PoolKindOfNoId(1) == PoolKind::Pool, "pool 1");
    CheckEq("sn_ctf_pool", PoolTitleKey(PoolKind::Ctf, true), "capture the flag");
    CheckEq("sn_ctf_pool", PoolTitleKey(PoolKind::Ctf, false), "capture the flag alone");
    CheckEq("sn_ledger_provider_pool", PoolTitleKey(PoolKind::Pool, true),
            "a pool beside capture the flag");
    CheckEq("sn_ledger_pool", PoolTitleKey(PoolKind::Pool, false), "Pool {}, as before");
  }
  {
    TEST_CASE("ctf: an older server's entries are pools, titled and claimed as before");
    // an entry without a kind reads as a pool (LedgerAccountFrom of kind ""),
    // and the relay's floor is known today too
    constexpr int64_t kFloor = 100'000'000;
    for (const int64_t floor : {int64_t{0}, kFloor}) {
      const std::string with = floor ? ", with the relay's floor" : "";
      const LedgerSectionView one =
          LedgerSectionFor(true, true, LedgerFetch::Ready, {Account()}, true, {}, floor);
      Check(one == Section(true, LedgerFetch::Ready, {Account()}), "the section as before" + with);
      Check(!one.poolTitles && one.accounts.size() == 1 && one.accounts[0].titleKey.empty(),
            "a lone account names no pool" + with);
      Check(one.showClaim && one.claimEnabled && !one.claimBoth, "Claim" + with);
      LedgerClaimProgress progress({Account()}, 0.5, {}, floor);
      CheckEq("sn_ledger_pool", progress.TitleKey(progress.rows()[0]), "Pool {} in the dialog" + with);
      Check(!progress.ClaimsBoth() && progress.ClaimableRao() == 3'241'000'000, "Claim with its amount" + with);
      // the claim sheet has a row per account, as Android's always had
      Check(RelayRowsFor({Relay(1, PoolKind::Pool, 841'000'000)}, {}, floor) ==
                std::vector<RelayRow>{Row(1, "sn_ledger_pool", RelayRowStatus::Pending, 841'000'000)},
            "the claim sheet's row, Pool {}" + with);
      // a network paid to two coldkeys (one of them a wallet it replaced)
      Check(RelayRowsFor({Relay(1, PoolKind::Pool, 841'000'000, "5AAA"),
                          Relay(1, PoolKind::Pool, 120'000'000, "5BBB")},
                         {}, floor) ==
                std::vector<RelayRow>{Row(1, "sn_ledger_pool", RelayRowStatus::Pending, 841'000'000, "5AAA"),
                                      Row(1, "sn_ledger_pool", RelayRowStatus::Pending, 120'000'000, "5BBB")},
            "two coldkeys' accounts, each named" + with);
      // the tile of a network with no wallet here: the figure alone
      Check(RelayTileFor({Relay(1, PoolKind::Pool, 841'000'000)}, "5COLD", {}, floor) == RelayTileView{},
            "the no-wallet tile's figure alone" + with);
    }
    // below the claim minimum on a lone account too (owner, 2026-11-11), once
    // the relay's floor is known
    LedgerAccount small = Account();
    small.claimableRao = 50'000'000;
    Check(!LedgerSectionFor(true, true, LedgerFetch::Ready, {small}, true, {}, 0).accounts[0].underFloor,
          "no floor known: nothing is under it");
    const LedgerSectionView under = LedgerSectionFor(true, true, LedgerFetch::Ready, {small}, true, {}, kFloor);
    Check(under.accounts[0].underFloor && under.accounts[0].titleKey.empty(), "a lone account under the floor");
    Check(under.showClaim && !under.claimEnabled, "no claim the chain will refuse");
    Check(LedgerClaimProgress({small}, 0.5, {}, kFloor).rows()[0].underFloor, "its dialog row");
    Check(RelayRowsFor({Relay(1, PoolKind::Pool, 50'000'000)}, {}, kFloor) ==
              std::vector<RelayRow>{Row(1, "sn_ledger_pool", RelayRowStatus::UnderFloor, 50'000'000)},
          "its claim sheet row");
    RelayTileView loneUnder;
    loneUnder.underFloor = true;
    Check(RelayTileFor({Relay(1, PoolKind::Pool, 50'000'000)}, "5COLD", {}, kFloor) == loneUnder,
          "under the no-wallet tile's figure");
  }
  {
    TEST_CASE("ctf: the capture the flag account is a block of its own beside the provider pool's");
    const LedgerSectionView view = Section(true, LedgerFetch::Ready, {Account(), CtfAccount()});
    Check(view.shown && view.poolTitles && view.note == LedgerNote::None, "titled blocks");
    Check(view.accounts ==
              std::vector<LedgerAccountView>{
                  Titled(Block(1, 3'241'000'000, 500'000'000, 3'741'000'000), PoolKind::Pool,
                         "sn_ledger_provider_pool"),
                  Titled(Block(kCtfNoId, 50'000'000, 0, 50'000'000), PoolKind::Ctf, "sn_ctf_pool")},
          "Provider pool, then Capture the flag");
    Check(view.showClaim && view.claimEnabled && view.claimBoth, "Claim both");
    // a lone capture the flag account still names its pool: its figure is not
    // the provider pool's
    const LedgerSectionView lone = Section(true, LedgerFetch::Ready, {CtfAccount()});
    Check(lone.poolTitles && lone.accounts.size() == 1 && lone.accounts[0].titleKey == "sn_ctf_pool",
          "Capture the flag alone");
    Check(lone.claimEnabled && !lone.claimBoth, "Claim, of the one pool");
    // Claim both needs something to claim in each pool
    LedgerAccount paid = Account();
    paid.claimableRao = 0;
    paid.paidRao = paid.finalizedRao;
    Check(!Section(true, LedgerFetch::Ready, {paid, CtfAccount()}).claimBoth,
          "the provider pool's is all paid");
    LedgerAccount ctfPaid = CtfAccount();
    ctfPaid.claimableRao = 0;
    ctfPaid.paidRao = ctfPaid.finalizedRao;
    Check(!Section(true, LedgerFetch::Ready, {Account(), ctfPaid}).claimBoth,
          "capture the flag's is all paid");
    Check(!Section(false, LedgerFetch::Ready, {Account(), CtfAccount()}).claimBoth,
          "no Claim before vault2");
    Check(Section(true, LedgerFetch::Ready, {Account(), CtfAccount()}, false).claimBoth &&
              !Section(true, LedgerFetch::Ready, {Account(), CtfAccount()}, false).claimEnabled,
          "without a device the button says it, disabled");
  }
  {
    TEST_CASE("ctf: an unavailable pool says so, never a zero, and hides no other");
    const LedgerSectionView view = LedgerSectionFor(true, true, LedgerFetch::Ready, {Account()},
                                                    true, {Unavailable(kCtfNoId, PoolKind::Ctf)});
    Check(view.shown && view.poolTitles && view.note == LedgerNote::None, "the accounts' section");
    Check(view.accounts ==
              std::vector<LedgerAccountView>{
                  Titled(Block(1, 3'241'000'000, 500'000'000, 3'741'000'000), PoolKind::Pool,
                         "sn_ledger_provider_pool"),
                  UnavailableBlock(kCtfNoId, PoolKind::Ctf, "sn_ctf_pool")},
          "the provider pool's figures, then capture the flag unavailable");
    Check(view.claimEnabled && !view.claimBoth, "Claim, of the provider pool");
    // the provider pool unavailable keeps its place before capture the flag's
    const LedgerSectionView first = LedgerSectionFor(true, true, LedgerFetch::Ready, {CtfAccount()},
                                                     true, {Unavailable(1, PoolKind::Pool)});
    Check(first.accounts ==
              std::vector<LedgerAccountView>{
                  UnavailableBlock(1, PoolKind::Pool, "sn_ledger_provider_pool"),
                  Titled(Block(kCtfNoId, 50'000'000, 0, 50'000'000), PoolKind::Ctf, "sn_ctf_pool")},
          "in noId order");
    // no account and a pool unavailable: whether there is one is unknown
    const LedgerSectionView none = LedgerSectionFor(true, true, LedgerFetch::Ready, {}, true,
                                                    {Unavailable(1, PoolKind::Pool)});
    Check(none.shown && none.note == LedgerNote::None, "never no account yet");
    Check(none.accounts == std::vector<LedgerAccountView>{UnavailableBlock(1, PoolKind::Pool, "")},
          "one block, which names no pool");
    Check(none.showClaim && !none.claimEnabled, "nothing to claim");
    // kept while reloading, as the accounts are; gone with a failed read
    Check(LedgerSectionFor(true, true, LedgerFetch::Loading, {Account()}, true,
                           {Unavailable(kCtfNoId, PoolKind::Ctf)})
                  .accounts.size() == 2,
          "kept while reloading");
    const LedgerSectionView failed = LedgerSectionFor(true, true, LedgerFetch::Failed, {Account()},
                                                      true, {Unavailable(kCtfNoId, PoolKind::Ctf)});
    Check(failed.accounts.empty() && failed.note == LedgerNote::Unavailable,
          "a failed read: unavailable as a whole");
    // before vault2 a ledger that is not served yet says nothing
    Check(LedgerSectionFor(true, false, LedgerFetch::Ready, {}, true,
                           {Unavailable(1, PoolKind::Pool)}) == LedgerSectionView{},
          "an unavailable pool alone, before vault2");
    Check(LedgerSectionFor(true, false, LedgerFetch::Ready, {Account()}, true,
                           {Unavailable(kCtfNoId, PoolKind::Ctf)})
                  .accounts.size() == 2,
          "beside an account it shows");
    // a pool both read and unavailable (the SDK refuses such an answer) shows its account
    Check(LedgerSectionFor(true, true, LedgerFetch::Ready, {Account(), CtfAccount()}, true,
                           {Unavailable(kCtfNoId, PoolKind::Ctf)})
                  .accounts ==
              Section(true, LedgerFetch::Ready, {Account(), CtfAccount()}).accounts,
          "the account, once");
  }
  {
    TEST_CASE("ctf: an account under the transfer floor");
    constexpr int64_t kFloor = 100'000'000;  // the relay's floor, 0.1 alpha at its price
    Check(UnderTransferFloor(kFloor - 1, kFloor), "under it");
    Check(!UnderTransferFloor(kFloor, kFloor), "at it");
    Check(!UnderTransferFloor(0, kFloor), "nothing to claim is not under it");
    Check(!UnderTransferFloor(1, 0), "no floor known");
    const LedgerSectionView view = LedgerSectionFor(true, true, LedgerFetch::Ready,
                                                    {Account(), CtfAccount()}, true, {}, kFloor);
    Check(!view.accounts[0].underFloor && view.accounts[1].underFloor,
          "capture the flag's 0.05 is under 0.1");
    Check(view.claimEnabled && !view.claimBoth, "Claim, not Claim both: no claim moves 0.05 now");
    Check(!Section(true, LedgerFetch::Ready, {Account(), CtfAccount()}).accounts[1].underFloor,
          "without the relay's floor nothing is under it");
    // a lone capture the flag account names its pool, so it says its floor
    Check(LedgerSectionFor(true, true, LedgerFetch::Ready, {CtfAccount()}, true, {}, kFloor)
              .accounts[0]
              .underFloor,
          "capture the flag's alone");
    // beside a pool the read could not read
    LedgerAccount smallPool = Account();
    smallPool.claimableRao = 50'000'000;
    const LedgerSectionView beside = LedgerSectionFor(true, true, LedgerFetch::Ready, {smallPool}, true,
                                                      {Unavailable(kCtfNoId, PoolKind::Ctf)}, kFloor);
    Check(beside.accounts[0].underFloor && !beside.accounts[1].underFloor,
          "the provider pool's under it, beside capture the flag unavailable");
    // every account under the floor: nothing the claim can move, so no Claim,
    // never a claim the chain will refuse
    LedgerAccount small = Account();
    small.claimableRao = 1;
    LedgerAccount smallCtf = CtfAccount();
    smallCtf.claimableRao = 1;
    Check(!LedgerSectionFor(true, true, LedgerFetch::Ready, {small}, true, {}, kFloor).claimEnabled,
          "a lone account under it: no Claim");
    const LedgerSectionView both =
        LedgerSectionFor(true, true, LedgerFetch::Ready, {small, smallCtf}, true, {}, kFloor);
    Check(both.accounts[0].underFloor && both.accounts[1].underFloor && both.showClaim &&
              !both.claimEnabled && !both.claimBoth,
          "both under it: no Claim");
    Check(LedgerSectionFor(true, true, LedgerFetch::Ready, {small}, true, {}, 0).claimEnabled,
          "no floor known: Claim, and the SDK's preflight decides");
    // the dialog marks a row where the tile marks its block
    const LedgerClaimProgress dialog({Account(), CtfAccount()}, 0.5, {}, kFloor);
    Check(!dialog.rows()[0].underFloor && dialog.rows()[1].underFloor, "the dialog's rows");
    Check(LedgerClaimProgress({smallPool}, 0.5, {Unavailable(kCtfNoId, PoolKind::Ctf)}, kFloor)
              .rows()[0]
              .underFloor,
          "beside an unavailable pool");
    LedgerClaimProgress alone = Dialog({small}, kFloor);
    Check(alone.rows().size() == 1 && alone.rows()[0].underFloor && alone.rows()[0].state == LedgerClaimState::Idle,
          "a lone row under it");
    Check(alone.ClaimableRao() == 0 && !alone.CanClaim(true) && !alone.Start(), "nothing for Claim to send");
  }
  {
    TEST_CASE("ctf claim: one Claim claims both pools, in one batch transaction");
    LedgerClaimProgress progress({Account(), CtfAccount()}, 0.5);
    Check(progress.rows().size() == 2, "a row for each pool");
    CheckEq("sn_ledger_provider_pool", progress.TitleKey(progress.rows()[0]), "Provider pool");
    CheckEq("sn_ctf_pool", progress.TitleKey(progress.rows()[1]), "Capture the flag");
    Check(progress.ClaimsBoth(), "Claim both");
    Check(progress.ClaimableRao() == 3'291'000'000, "the total");
    Check(progress.Start(), "one Claim");
    // the SDK's batch: Sent for each account with the one hash, Confirmed for each
    progress.Sent(1, 42, "0xbatch");
    progress.Sent(kCtfNoId, 9, "0xbatch");
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Sent, "0xbatch", "",
             "the provider pool's, sent");
    CheckRow(progress.rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Sent, "0xbatch", "",
             "capture the flag's, in the same transaction");
    progress.Confirmed(1, 42, "0xbatch", 3'241'000'000);
    progress.Confirmed(kCtfNoId, 9, "0xbatch", 50'000'000);
    progress.Done();
    Check(progress.ClaimableRao() == 0 && !progress.ClaimsBoth() && !progress.CanClaim(true),
          "both claimed");
    // one of them left: Claim with its amount
    LedgerClaimProgress partly({Account(), CtfAccount()}, 0.5);
    Check(partly.Start(), "Claim both");
    partly.Confirmed(1, 42, "0xbatch", 3'241'000'000);
    partly.Failed(kCtfNoId, 9, "claim_failed: reverted");
    partly.Done();
    Check(!partly.ClaimsBoth() && partly.CanClaim(true), "capture the flag's alone, again");
    // an account the batch did not attempt is then claimed alone, with its own hash
    LedgerClaimProgress alone({Account(), CtfAccount()}, 0.5);
    Check(alone.Start(), "Claim both");
    alone.Sent(1, 42, "0xbatch");
    alone.Sent(kCtfNoId, 9, "0xbatch");
    alone.Confirmed(1, 42, "0xbatch", 3'241'000'000);
    alone.Sent(kCtfNoId, 9, "0xalone");
    alone.Confirmed(kCtfNoId, 9, "0xalone", 50'000'000);
    Check(alone.rows().size() == 2, "the same row");
    CheckRow(alone.rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Confirmed, "0xalone", "",
             "its own transaction");
    // capture the flag's account with nothing to claim has no row, and the
    // provider pool's row is named as the tile names its block
    LedgerAccount ctfPaid = CtfAccount();
    ctfPaid.claimableRao = 0;
    ctfPaid.paidRao = ctfPaid.finalizedRao;
    const LedgerClaimProgress paidCtf({Account(), ctfPaid}, 0.5);
    Check(paidCtf.rows().size() == 1, "one row");
    CheckEq("sn_ledger_provider_pool", paidCtf.TitleKey(paidCtf.rows()[0]), "the dialog's title");
    CheckEq("sn_ledger_provider_pool",
            Section(true, LedgerFetch::Ready, {Account(), ctfPaid}).accounts[0].titleKey,
            "the tile's title");
    Check(!paidCtf.ClaimsBoth(), "Claim with its amount");
    const LedgerClaimProgress unavailableCtf({Account()}, 0.5, {Unavailable(kCtfNoId, PoolKind::Ctf)});
    CheckEq("sn_ledger_provider_pool", unavailableCtf.TitleKey(unavailableCtf.rows()[0]),
            "beside capture the flag unavailable");
    // under the relay's floor, capture the flag's is not Claim both, and the
    // claim's amount leaves it out: Claim moves the provider pool's
    LedgerClaimProgress floor({Account(), CtfAccount()}, 0.5, {}, 100'000'000);
    Check(!floor.rows()[0].underFloor && floor.rows()[1].underFloor, "under the floor");
    Check(!floor.ClaimsBoth() && floor.CanClaim(true), "Claim with its amount");
    Check(floor.ClaimableRao() == 3'241'000'000, "the amount leaves it out");
    Check(floor.Start(), "Claim");
    Check(floor.rows()[0].state == LedgerClaimState::Sending && floor.rows()[1].state == LedgerClaimState::Idle,
          "only the provider pool's is sending");
    // the SDK claims what its own preflight passes: the page's estimate can be off
    floor.Sent(kCtfNoId, 9, "0xbatch");
    Check(floor.rows()[1].state == LedgerClaimState::Sent && !floor.rows()[1].underFloor, "the chain took it");
  }
  {
    TEST_CASE("ctf claim: an unavailable pool is a row of its own, never a zero");
    LedgerClaimProgress progress({Account()}, 0.5, {Unavailable(kCtfNoId, PoolKind::Ctf)});
    Check(progress.rows().size() == 2, "the account and the pool");
    const LedgerClaimRow pool = progress.rows()[1];
    Check(pool.noId == kCtfNoId && pool.index == 0 && pool.kind == PoolKind::Ctf &&
              pool.unavailable && pool.amountRao == 0 && pool.state == LedgerClaimState::Idle,
          "capture the flag, unavailable, at index 0 as the claim reports it");
    CheckEq("sn_ctf_pool", progress.TitleKey(pool), "its title");
    CheckEq("sn_ledger_provider_pool", progress.TitleKey(progress.rows()[0]),
            "the provider pool beside it");
    Check(progress.ClaimableRao() == 3'241'000'000 && !progress.ClaimsBoth(),
          "Claim with the provider pool's amount");
    Check(progress.Start(), "Claim");
    Check(progress.rows()[1].state == LedgerClaimState::Idle,
          "not sending: the claim's own read reports on it");
    // still unavailable to the claim's read: the SDK's noId and index 0
    progress.Failed(kCtfNoId, 0, "ledger_unavailable: the ledger answered 503");
    Check(progress.rows().size() == 2 && progress.rows()[1].unavailable &&
              progress.rows()[1].state == LedgerClaimState::Idle && progress.rows()[1].message.empty(),
          "unavailable, never failed");
    Check(progress.failure().empty(), "not the dialog's failure");
    progress.Confirmed(1, 42, "0xabc", 3'241'000'000);
    progress.Done();
    Check(!progress.CanClaim(true), "nothing left that the claim can send");
    // readable again by a later claim's read: its account takes the pool's row
    LedgerClaimProgress back({Account()}, 0.5, {Unavailable(kCtfNoId, PoolKind::Ctf)});
    Check(back.Start(), "Claim");
    back.Sent(1, 42, "0xbatch");
    back.Sent(kCtfNoId, 9, "0xbatch");
    Check(back.rows().size() == 2, "no third row");
    CheckRow(back.rows()[1], kCtfNoId, 9, 0, LedgerClaimState::Sent, "0xbatch", "",
             "the pool's row, with its account");
    Check(!back.rows()[1].unavailable && back.rows()[1].kind == PoolKind::Ctf, "available");
    back.Confirmed(kCtfNoId, 9, "0xbatch", 50'000'000);
    CheckRow(back.rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Confirmed, "0xbatch", "",
             "confirmed with the Claimed amount");
    // the dialog's total: what Claim moves, else nothing rather than a zero
    // while the rows left say why (Android's dialog)
    Check(Dialog({Account()}, 0, {Unavailable(kCtfNoId, PoolKind::Ctf)}).TotalKnown(),
          "an account beside the unavailable pool: its total");
    Check(!progress.TotalKnown(), "the provider pool's claimed, capture the flag's unread: no total");
    const LedgerClaimProgress unread =
        Dialog({}, 0, {Unavailable(1, PoolKind::Pool), Unavailable(kCtfNoId, PoolKind::Ctf)});
    Check(!unread.TotalKnown() && unread.rows().size() == 2 && !unread.CanClaim(true), "nothing read: no total");
    Check(Dialog({}).TotalKnown(), "nothing to claim and nothing unread: its zero");
    LedgerAccount small = Account();
    small.claimableRao = 1;
    Check(!Dialog({small}, 1'000).TotalKnown(), "every account under the floor: no total, the rows say why");
    Check(Dialog({Account(), CtfAccount()}, 100'000'000).TotalKnown(), "one above the floor: its total");
    // an account's pool unavailable to the claim's own read, and nothing else
    LedgerClaimProgress lost = Dialog({CtfAccount()});
    Check(lost.Start(), "Claim");
    lost.Failed(kCtfNoId, 0, "ledger_unavailable: 503");
    lost.Done();
    Check(!lost.TotalKnown() && lost.rows().size() == 1, "no total once its only pool is unread");
    // everything claimed: a true zero
    LedgerClaimProgress all = Dialog({Account(), CtfAccount()});
    Check(all.Start(), "Claim both");
    all.Confirmed(1, 42, "0xbatch", 3'241'000'000);
    all.Confirmed(kCtfNoId, 9, "0xbatch", 50'000'000);
    all.Done();
    Check(all.TotalKnown() && all.ClaimableRao() == 0, "both claimed: 0 left");
    // a row the SDK added, of an amount not known yet, is not a zero either
    LedgerClaimProgress added = Dialog({Account()});
    Check(added.Start(), "Claim");
    added.Confirmed(1, 42, "0xabc", 3'241'000'000);
    added.Failed(2, 7, "claim_failed: reverted");
    added.Done();
    Check(!added.TotalKnown(), "an unlisted account's failure: no total");
  }
  {
    TEST_CASE("ctf claim: the claim's own read cannot read a pool the page listed");
    LedgerClaimProgress progress({Account(), CtfAccount()}, 0.5);
    Check(progress.Start(), "Claim both");
    progress.Failed(kCtfNoId, 0, "ledger_unavailable: 503");
    Check(progress.rows().size() == 2, "no new row");
    Check(progress.rows()[1].unavailable && progress.rows()[1].state == LedgerClaimState::Idle &&
              progress.rows()[1].amountRao == 50'000'000,
          "its row is unavailable");
    Check(progress.ClaimableRao() == 3'241'000'000, "what Claim moves leaves it out");
    // the figure and Claim both are the press's until Done (every app,
    // 2026-11-12: fixed at the press); the row says the pool is unavailable
    Check(progress.FigureRao() == 3'291'000'000 && progress.ClaimsBoth(), "the claim's figure, fixed at the press");
    progress.Sent(1, 42, "0xabc");
    progress.Confirmed(1, 42, "0xabc", 3'241'000'000);
    Check(progress.FigureRao() == 3'291'000'000 && progress.TotalKnown(), "one confirmed: still its figure");
    progress.Done();
    Check(!progress.CanClaim(true), "the page reads the accounts again");
    Check(!progress.ClaimsBoth() && !progress.TotalKnown(), "after Done: no Claim both, and no figure");
    // a confirmed account's pool unavailable to a later claim stays claimed
    LedgerClaimProgress claimed({Account(), CtfAccount()}, 0.5);
    Check(claimed.Start(), "Claim both");
    claimed.Confirmed(kCtfNoId, 9, "0xbatch", 50'000'000);
    claimed.Failed(kCtfNoId, 0, "ledger_unavailable: 503");
    Check(claimed.rows().size() == 2 && !claimed.rows()[1].unavailable &&
              claimed.rows()[1].state == LedgerClaimState::Confirmed,
          "claimed, and no unavailable row beside it");
    // only rows this claim has not sent: one sent may still confirm, and one
    // the claim failed keeps its failure (Android's and Linux's rule)
    LedgerClaimProgress sent = Dialog({Account(), CtfAccount()});
    Check(sent.Start(), "Claim both");
    sent.Sent(kCtfNoId, 9, "0xbatch");
    sent.Failed(kCtfNoId, 0, "ledger_unavailable: 503");
    Check(sent.rows().size() == 2 && sent.rows()[1].state == LedgerClaimState::Sent && !sent.rows()[1].unavailable,
          "sent, never unavailable");
    LedgerClaimProgress refused = Dialog({Account(), CtfAccount()});
    Check(refused.Start(), "Claim both");
    refused.Failed(kCtfNoId, 9, "proof_mismatch: the root does not match");
    refused.Failed(kCtfNoId, 0, "ledger_unavailable: 503");
    Check(refused.rows()[1].state == LedgerClaimState::Failed && !refused.rows()[1].unavailable,
          "failed, never unavailable");
    // a pool, or an account, the page never listed: its kind by its noId
    LedgerClaimProgress unlisted({Account()}, 0.5);
    Check(unlisted.Start(), "Claim");
    unlisted.Failed(kCtfNoId, 0, "ledger_unavailable: 503");
    Check(unlisted.rows().size() == 2 && unlisted.rows()[1].kind == PoolKind::Ctf &&
              unlisted.rows()[1].unavailable,
          "capture the flag, unavailable");
    LedgerClaimProgress fresh({Account()}, 0.5);
    Check(fresh.Start(), "Claim");
    fresh.Sent(kCtfNoId, 3, "0xbatch");
    Check(fresh.rows().size() == 2 && fresh.rows()[1].kind == PoolKind::Ctf, "capture the flag's");
    CheckEq("sn_ledger_provider_pool", fresh.TitleKey(fresh.rows()[0]), "the provider pool beside it");
  }
  {
    TEST_CASE("ctf: the claim sheet's pool rows");
    Check(RelayRowsFor({}, {}, 0).empty(), "no accounts, no rows");
    Check(RelayRowsFor({Relay(1, PoolKind::Pool, 841'000'000)}, {}, 0) ==
              std::vector<RelayRow>{Row(1, "sn_ledger_pool", RelayRowStatus::Pending, 841'000'000)},
          "one pool's row alone, as Android's sheet has it");
    Check(RelayRowsFor({Relay(1, PoolKind::Pool, 841'000'000), Relay(kCtfNoId, PoolKind::Ctf, 50'000'000)},
                       {}, 0) ==
              std::vector<RelayRow>{
                  Row(1, "sn_ledger_provider_pool", RelayRowStatus::Pending, 841'000'000),
                  Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::Pending, 50'000'000)},
          "both pools, Pending");
    Check(RelayRowsFor({Relay(1, PoolKind::Pool, 841'000'000), Relay(kCtfNoId, PoolKind::Ctf, 50'000'000)},
                       {}, 100'000'000) ==
              std::vector<RelayRow>{
                  Row(1, "sn_ledger_provider_pool", RelayRowStatus::Pending, 841'000'000),
                  Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::UnderFloor, 50'000'000)},
          "capture the flag's under the floor");
    Check(RelayRowsFor({Relay(kCtfNoId, PoolKind::Ctf, 50'000'000)}, {}, 0) ==
              std::vector<RelayRow>{Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::Pending, 50'000'000)},
          "capture the flag's alone names its pool");
    // claimed by URnetwork, nothing claimable since; nothing at all is no row
    const std::string hash = "0x" + std::string(64, 'a');
    RelayAccount claimed = Relay(1, PoolKind::Pool, 0);
    claimed.claimTxHash = hash;
    claimed.claimTxUrl = "https://evm.taostats.io/tx/" + hash;
    claimed.claimAmountRao = 841'000'000;
    Check(RelayRowsFor({claimed, Relay(kCtfNoId, PoolKind::Ctf, 50'000'000)}, {}, 0) ==
              std::vector<RelayRow>{
                  Row(1, "sn_ledger_provider_pool", RelayRowStatus::Claimed, 841'000'000, "",
                      "https://evm.taostats.io/tx/" + hash),
                  Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::Pending, 50'000'000)},
          "Claimed with what it paid and its transaction");
    // a second operator pool in the answer, even one with nothing, names each
    // operator pool by its number (Android's snLedgerPoolName)
    Check(RelayRowsFor({claimed, Relay(kCtfNoId, PoolKind::Ctf, 50'000'000), Relay(2, PoolKind::Pool, 0)},
                       {}, 0) ==
              std::vector<RelayRow>{
                  Row(1, "sn_ledger_pool", RelayRowStatus::Claimed, 841'000'000, "",
                      "https://evm.taostats.io/tx/" + hash),
                  Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::Pending, 50'000'000)},
          "Pool {} beside a second operator pool");
    // an unavailable pool, after the accounts; and alone
    Check(RelayRowsFor({Relay(1, PoolKind::Pool, 841'000'000)},
                       {Unavailable(kCtfNoId, PoolKind::Ctf)}, 0) ==
              std::vector<RelayRow>{
                  Row(1, "sn_ledger_provider_pool", RelayRowStatus::Pending, 841'000'000),
                  Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::Unavailable, 0)},
          "capture the flag unavailable");
    Check(RelayRowsFor({}, {Unavailable(1, PoolKind::Pool)}, 0) ==
              std::vector<RelayRow>{Row(1, "sn_ledger_pool", RelayRowStatus::Unavailable, 0)},
          "the pool unavailable, alone");
    Check(RelayRowsFor({Relay(kCtfNoId, PoolKind::Ctf, 50'000'000)},
                       {Unavailable(kCtfNoId, PoolKind::Ctf)}, 0)
                  .size() == 1,
          "a pool both read and unavailable shows its account");
    // capture the flag's with nothing to claim and no claim of URnetwork's has
    // no row, and still names the provider pool's beside it
    Check(RelayRowsFor({Relay(1, PoolKind::Pool, 841'000'000), Relay(kCtfNoId, PoolKind::Ctf, 0)}, {}, 0) ==
              std::vector<RelayRow>{Row(1, "sn_ledger_provider_pool", RelayRowStatus::Pending, 841'000'000)},
          "capture the flag's with nothing: no row of its own");
    // a network paid to two coldkeys names each row's coldkey
    Check(RelayRowsFor({Relay(1, PoolKind::Pool, 841'000'000, "5AAA"),
                        Relay(kCtfNoId, PoolKind::Ctf, 50'000'000, "5AAA"),
                        Relay(1, PoolKind::Pool, 120'000'000, "5BBB")},
                       {}, 0) ==
              std::vector<RelayRow>{
                  Row(1, "sn_ledger_provider_pool", RelayRowStatus::Pending, 841'000'000, "5AAA"),
                  Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::Pending, 50'000'000, "5AAA"),
                  Row(1, "sn_ledger_provider_pool", RelayRowStatus::Pending, 120'000'000, "5BBB")},
          "each coldkey named");
  }
  {
    TEST_CASE("ctf: Provider pool names the one operator pool beside capture the flag");
    Check(!NamesProviderPool(std::vector<PoolInView>{}), "no pools");
    Check(!NamesProviderPool(std::vector<PoolInView>{{1, PoolKind::Pool}}), "a pool alone");
    Check(!NamesProviderPool(std::vector<PoolInView>{{kCtfNoId, PoolKind::Ctf}}), "capture the flag alone");
    Check(NamesProviderPool(std::vector<PoolInView>{{1, PoolKind::Pool}, {kCtfNoId, PoolKind::Ctf}}),
          "beside capture the flag");
    Check(NamesProviderPool(
              std::vector<PoolInView>{{1, PoolKind::Pool}, {1, PoolKind::Pool}, {kCtfNoId, PoolKind::Ctf}}),
          "one operator pool, two coldkeys' accounts in it");
    Check(!NamesProviderPool(
              std::vector<PoolInView>{{1, PoolKind::Pool}, {2, PoolKind::Pool}, {kCtfNoId, PoolKind::Ctf}}),
          "two operator pools: Pool {} each");
    // the tile, the dialog and the sheet name them alike
    LedgerAccount second = Account();
    second.noId = 2;
    second.index = 5;
    second.claimableRao = 900'000'000;
    const LedgerSectionView three = Section(true, LedgerFetch::Ready, {Account(), second, CtfAccount()});
    Check(three.accounts.size() == 3 && three.accounts[0].titleKey == "sn_ledger_pool" &&
              three.accounts[1].titleKey == "sn_ledger_pool" && three.accounts[2].titleKey == "sn_ctf_pool",
          "the tile: Pool 1, Pool 2, Capture the flag");
    const LedgerClaimProgress dialog = Dialog({Account(), second, CtfAccount()});
    CheckEq("sn_ledger_pool", dialog.TitleKey(dialog.rows()[0]), "the dialog: Pool 1");
    CheckEq("sn_ctf_pool", dialog.TitleKey(dialog.rows()[2]), "the dialog: Capture the flag");
    Check(RelayRowsFor({Relay(1, PoolKind::Pool, 841'000'000), Relay(2, PoolKind::Pool, 120'000'000),
                        Relay(kCtfNoId, PoolKind::Ctf, 50'000'000)},
                       {}, 0) ==
              std::vector<RelayRow>{Row(1, "sn_ledger_pool", RelayRowStatus::Pending, 841'000'000),
                                    Row(2, "sn_ledger_pool", RelayRowStatus::Pending, 120'000'000),
                                    Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::Pending, 50'000'000)},
          "the sheet: Pool 1, Pool 2, Capture the flag");
    // an account of a second pool that the claim reports names them by number
    LedgerClaimProgress reported = Dialog({Account(), CtfAccount()});
    CheckEq("sn_ledger_provider_pool", reported.TitleKey(reported.rows()[0]), "Provider pool before");
    Check(reported.Start(), "Claim both");
    reported.Sent(2, 5, "0xbatch");
    CheckEq("sn_ledger_pool", reported.TitleKey(reported.rows()[0]), "Pool 1 once pool 2 is in view");
  }
  {
    TEST_CASE("ctf: Claim both is exactly the operator pool's and capture the flag's");
    Check(ClaimsBothPools({PoolKind::Pool, PoolKind::Ctf}), "the two");
    Check(ClaimsBothPools({PoolKind::Ctf, PoolKind::Pool}), "in either order");
    Check(!ClaimsBothPools({}), "none");
    Check(!ClaimsBothPools({PoolKind::Pool}), "the pool's alone");
    Check(!ClaimsBothPools({PoolKind::Ctf}), "capture the flag's alone");
    Check(!ClaimsBothPools({PoolKind::Pool, PoolKind::Pool}), "two operator pools");
    Check(!ClaimsBothPools({PoolKind::Pool, PoolKind::Pool, PoolKind::Ctf}), "three accounts");
    LedgerAccount second = Account();
    second.noId = 2;
    second.index = 5;
    second.claimableRao = 900'000'000;
    const LedgerSectionView three = Section(true, LedgerFetch::Ready, {Account(), second, CtfAccount()});
    Check(three.claimEnabled && !three.claimBoth, "the tile: three accounts, Claim");
    const LedgerClaimProgress dialog = Dialog({Account(), second, CtfAccount()});
    Check(!dialog.ClaimsBoth() && dialog.CanClaim(true), "the dialog: three rows, Claim with the amount");
    // a third under the floor leaves the two the claim moves
    LedgerAccount tiny = second;
    tiny.claimableRao = 1;
    Check(LedgerSectionFor(true, true, LedgerFetch::Ready, {Account(), tiny, CtfAccount()}, true, {}, 1'000)
              .claimBoth,
          "the tile: the two above the floor");
    Check(Dialog({Account(), tiny, CtfAccount()}, 1'000).ClaimsBoth(), "the dialog: the two above the floor");
  }
  {
    TEST_CASE("ctf claim: the preflight's AmountTooLow reads below the claim minimum, never failed");
    // the SDK's words for the runtime's transfer floor (runtimeRefusalMessage)
    const std::string tooLow = "not_claimable: AmountTooLow: the claim is below the runtime's transfer floor";
    // the page's estimate had it above the floor (no price yet, or a price
    // that moved since): the claim's preflight refused it and sent nothing
    LedgerClaimProgress progress = Dialog({Account(), CtfAccount()});
    Check(progress.ClaimsBoth() && progress.Start(), "Claim both");
    progress.Failed(kCtfNoId, 9, tooLow);
    const LedgerClaimRow ctf = progress.rows()[1];
    Check(ctf.state == LedgerClaimState::Idle && ctf.underFloor && ctf.message.empty() && !ctf.unavailable,
          "below the claim minimum, never failed");
    Check(progress.failure().empty() && !progress.needsGas(), "no failure, no gas hint");
    progress.Sent(1, 42, "0xabc");
    progress.Confirmed(1, 42, "0xabc", 3'241'000'000);
    progress.Done();
    Check(progress.rows()[1].underFloor && progress.rows()[1].state == LedgerClaimState::Idle, "after the claim");
    Check(progress.ClaimableRao() == 0 && !progress.CanClaim(true) && !progress.ClaimsBoth(),
          "nothing the claim can move");
    // a lone account too
    LedgerClaimProgress lone = Dialog({Account()});
    Check(lone.Start(), "Claim");
    lone.Failed(1, 42, tooLow);
    lone.Done();
    Check(lone.rows()[0].underFloor && lone.rows()[0].state == LedgerClaimState::Idle && !lone.CanClaim(true),
          "a lone account under the runtime's floor");
    // the SDK tries it again with the next press, and its newest answer is a
    // failure: Failed, and above the dialog's floor (none here) a press takes
    // it again
    LedgerClaimProgress again = Dialog({Account(), CtfAccount()});
    Check(again.Start(), "Claim both");
    again.Failed(kCtfNoId, 9, tooLow);
    again.Failed(1, 42, "claim_failed: execution reverted");
    again.Done();
    Check(again.rows()[1].underFloor && again.ClaimableRao() == 3'241'000'000, "the provider pool's alone");
    Check(again.Start(), "Claim again");
    Check(again.rows()[1].state == LedgerClaimState::Idle && again.rows()[1].underFloor,
          "the preflight's refusal stands until the SDK answers again");
    again.Confirmed(1, 42, "0xabc", 3'241'000'000);
    again.Failed(kCtfNoId, 9, "claim_failed: execution reverted");
    again.Done();
    CheckRow(again.rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Failed, "",
             "claim_failed: execution reverted", "now Failed");
    Check(!again.rows()[1].underFloor && again.ClaimableRao() == 50'000'000, "a press takes it again");
    // any other not_claimable of an account is its failure, as before
    const std::string elsewhere =
        "not_claimable: the ledger lists this account in 0x01, not in the configured vault2 0x02";
    LedgerClaimProgress other = Dialog({Account()});
    Check(other.Start(), "Claim");
    other.Failed(1, 42, elsewhere);
    CheckRow(other.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Failed, "", elsewhere, "failed");
    Check(!other.rows()[0].underFloor, "not under the floor");
  }
  {
    TEST_CASE("ctf claim: the gas key pays for the largest alone, and the other stays claimable");
    // the SDK's fallback (sdk-ctf-v2 §2.1): the gas key cannot pay for the
    // batch, so each account goes alone, the largest first, and one it then
    // cannot pay for is reported needs_gas with nothing sent
    const std::string needsGas =
        "needs_gas: this claim needs up to 0.0173 TAO of gas, the gas key 0x00aa has 0.0050 TAO; "
        "send TAO to 5GAS";
    LedgerClaimProgress progress = Dialog({Account(), CtfAccount()});
    Check(progress.ClaimsBoth() && progress.Start(), "Claim both");
    progress.Sent(1, 42, "0xlargest");
    progress.Confirmed(1, 42, "0xlargest", 3'241'000'000);
    progress.Failed(kCtfNoId, 9, needsGas);
    progress.Done();
    CheckRow(progress.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Confirmed, "0xlargest", "",
             "the largest, claimed");
    CheckRow(progress.rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Idle, "", "",
             "capture the flag's, still claimable");
    Check(!progress.rows()[1].underFloor && !progress.rows()[1].unavailable, "neither under the floor nor unavailable");
    Check(progress.needsGas() && progress.failure().empty(), "the funding hint, and no failure");
    Check(progress.ClaimableRao() == 50'000'000, "the claim's amount is what is left");
    Check(!progress.CanClaim(true), "Claim again once the gas key has gas");
    // the gas key pays for both alone after all: both claimed, no hint
    LedgerClaimProgress paid = Dialog({Account(), CtfAccount()});
    Check(paid.Start(), "Claim both");
    paid.Sent(1, 42, "0xlargest");
    paid.Confirmed(1, 42, "0xlargest", 3'241'000'000);
    paid.Sent(kCtfNoId, 9, "0xnext");
    paid.Confirmed(kCtfNoId, 9, "0xnext", 50'000'000);
    paid.Done();
    Check(!paid.needsGas() && paid.ClaimableRao() == 0, "both claimed");
  }
  {
    TEST_CASE("ctf claim: a pool the page could not read leaves at Done once the claim read it");
    // the claim's own read answered and said nothing of capture the flag's
    // pool: it was read, and held nothing to claim
    LedgerClaimProgress read = Dialog({Account()}, 0, {Unavailable(kCtfNoId, PoolKind::Ctf)});
    Check(read.rows().size() == 2 && read.Start(), "Claim");
    read.Sent(1, 42, "0xabc");
    read.Confirmed(1, 42, "0xabc", 3'241'000'000);
    Check(read.rows().size() == 2, "kept while the claim is out");
    read.Done();
    Check(read.rows().size() == 1 && read.rows()[0].noId == 1, "the pool's row goes");
    CheckEq("sn_ledger_provider_pool", read.TitleKey(read.rows()[0]), "the provider pool, as the page read it");
    // still unavailable to the claim's read: it stays
    LedgerClaimProgress still = Dialog({Account()}, 0, {Unavailable(kCtfNoId, PoolKind::Ctf)});
    Check(still.Start(), "Claim");
    still.Failed(kCtfNoId, 0, "ledger_unavailable: 503");
    still.Sent(1, 42, "0xabc");
    still.Done();
    Check(still.rows().size() == 2 && still.rows()[1].unavailable, "unavailable, kept");
    // the claim stopped before its read: it stays
    LedgerClaimProgress stopped = Dialog({Account()}, 0, {Unavailable(kCtfNoId, PoolKind::Ctf)});
    Check(stopped.Start(), "Claim");
    stopped.Failed(0, 0, "chain_rpc_unreachable: dial tcp");
    stopped.Done();
    Check(stopped.rows().size() == 2 && stopped.rows()[1].unavailable, "kept: nothing was read");
    // nothing to claim in any pool is the read answering too
    LedgerClaimProgress nothing = Dialog({Account()}, 0, {Unavailable(kCtfNoId, PoolKind::Ctf)});
    Check(nothing.Start(), "Claim");
    nothing.Failed(0, 0, "not_claimable: no settlement v2 account has anything to claim");
    nothing.Done();
    Check(nothing.rows().size() == 1 && nothing.rows()[0].noId == 1, "the pool's row goes");
    // its account found after all takes the row, and stays
    LedgerClaimProgress found = Dialog({Account()}, 0, {Unavailable(kCtfNoId, PoolKind::Ctf)});
    Check(found.Start(), "Claim");
    found.Sent(1, 42, "0xbatch");
    found.Sent(kCtfNoId, 9, "0xbatch");
    found.Done();
    Check(found.rows().size() == 2 && !found.rows()[1].unavailable && found.rows()[1].index == 9 &&
              found.rows()[1].state == LedgerClaimState::Sent,
          "the account's row");
    // a second claim starts its own read: the pool reported unavailable by the
    // first, read by the second, goes
    LedgerClaimProgress again = Dialog({Account(), CtfAccount()});
    Check(again.Start(), "Claim both");
    again.Failed(kCtfNoId, 0, "ledger_unavailable: 503");
    again.Failed(1, 42, "claim_failed: reverted");
    again.Done();
    Check(again.rows().size() == 2 && again.rows()[1].unavailable, "unavailable to the first claim");
    Check(again.Start(), "Claim again");
    again.Sent(1, 42, "0xdef");
    again.Done();
    Check(again.rows().size() == 2, "a row with its amount stays: the page listed its account");
  }
  {
    TEST_CASE("ctf: the no-wallet tile's rows and its total");
    // the primary coldkey's accounts with something to claim, once there are
    // two or one is capture the flag's (Android's snClaimRelayTileRows)
    RelayTileView both;
    both.rows = {Row(1, "sn_ledger_provider_pool", RelayRowStatus::Pending, 841'000'000),
                 Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::UnderFloor, 50'000'000)};
    Check(RelayTileFor({Relay(1, PoolKind::Pool, 841'000'000), Relay(kCtfNoId, PoolKind::Ctf, 50'000'000)},
                       "5COLD", {}, 100'000'000) == both,
          "both pools, capture the flag's under the floor");
    RelayTileView ctfAlone;
    ctfAlone.rows = {Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::Pending, 50'000'000)};
    Check(RelayTileFor({Relay(1, PoolKind::Pool, 0), Relay(kCtfNoId, PoolKind::Ctf, 50'000'000)}, "5COLD", {}, 0) ==
              ctfAlone,
          "capture the flag's alone names its pool");
    RelayTileView two;
    two.rows = {Row(1, "sn_ledger_pool", RelayRowStatus::Pending, 841'000'000),
                Row(2, "sn_ledger_pool", RelayRowStatus::Pending, 120'000'000)};
    Check(RelayTileFor({Relay(1, PoolKind::Pool, 841'000'000), Relay(2, PoolKind::Pool, 120'000'000)}, "5COLD", {},
                       0) == two,
          "two operator pools: a row each");
    // another coldkey's account is not in the figure (a wallet the network replaced)
    Check(RelayTileFor({Relay(1, PoolKind::Pool, 841'000'000), Relay(kCtfNoId, PoolKind::Ctf, 50'000'000, "5OLD")},
                       "5COLD", {}, 0) == RelayTileView{},
          "the primary coldkey's one account: the figure alone");
    // a pool the relay could not read: the coldkey's accounts, then the pool
    RelayTileView partial;
    partial.rows = {Row(1, "sn_ledger_provider_pool", RelayRowStatus::Pending, 841'000'000),
                    Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::Unavailable, 0)};
    Check(RelayTileFor({Relay(1, PoolKind::Pool, 841'000'000)}, "5COLD", {Unavailable(kCtfNoId, PoolKind::Ctf)}, 0) ==
              partial,
          "capture the flag unavailable");
    // none of the coldkey's pools read: no total, never a zero
    RelayTileView unread;
    unread.showTotal = false;
    unread.rows = {Row(1, "sn_ledger_pool", RelayRowStatus::Unavailable, 0)};
    Check(RelayTileFor({}, "5COLD", {Unavailable(1, PoolKind::Pool)}, 0) == unread, "nothing read");
    Check(!RelayTotalKnown({Relay(1, PoolKind::Pool, 5, "5OLD")}, "5COLD", {Unavailable(1, PoolKind::Pool)}),
          "another coldkey's account adds nothing to this one's");
    Check(RelayTotalKnown({}, "5COLD", {}), "a whole answer with nothing: its zero is the total");
    Check(RelayTotalKnown({Relay(1, PoolKind::Pool, 0)}, "5COLD", {Unavailable(kCtfNoId, PoolKind::Ctf)}),
          "one of its pools read");
  }
  {
    TEST_CASE("ctf: a line for every unclaimed win, newest first");
    CheckEq("sn_ctf_won_provider_slot", CtfWinKey(CtfWin{CtfRole::Provider, "is"}), "a provider slot");
    CheckEq("sn_ctf_won_extender_slot", CtfWinKey(CtfWin{CtfRole::Extender, "la"}), "an extender slot");
    const CtfWin icelandProvider{CtfRole::Provider, "is"};
    const CtfWin icelandExtender{CtfRole::Extender, "is"};
    const CtfWin kenyaProvider{CtfRole::Provider, "ke"};
    SdkCtfClientResult result;
    Check(CtfWinsFrom(result).empty(), "an answer without the list: no line");
    result.unclaimed_wins = std::vector<SdkCtfWin>{};
    Check(CtfWinsFrom(result).empty(), "nothing unclaimed: no line");
    // the SDK's order, newest first and a provider's before an extender's
    // within an epoch, is the lines' order
    result.unclaimed_wins = std::vector<SdkCtfWin>{SdkWin(11, "provider", "is"),
                                                   SdkWin(11, "extender", "is"),
                                                   SdkWin(9, "provider", "ke")};
    Check(CtfWinsFrom(result) == std::vector<CtfWin>{icelandProvider, icelandExtender, kenyaProvider},
          "a line each, in the SDK's order");
    // the latest win is no longer the line: one claimed since says nothing, as
    // one the server cannot say of (unknown), and an older one not claimed yet
    // still does
    SdkCtfWin claimed = SdkWin(13, "provider", "fi");
    claimed.claim_state = "claimed";
    SdkCtfWin unknown = SdkWin(12, "provider", "se");
    unknown.claim_state = "unknown";
    result.latest_win = claimed;
    result.wins = std::vector<SdkCtfWin>{claimed, unknown, SdkWin(11, "provider", "is"),
                                         SdkWin(11, "extender", "is"), SdkWin(9, "provider", "ke")};
    Check(CtfWinsFrom(result) == std::vector<CtfWin>{icelandProvider, icelandExtender, kenyaProvider},
          "the unclaimed wins, not the latest");
    // the same slot won again in a later epoch is a line of its own, and so is
    // a win of another of the network's clients (a network's answer)
    SdkCtfWin otherClient = SdkWin(10, "provider", "is");
    otherClient.client_id = "9a1b2c3d-4e5f-4a6b-8c7d-0e1f2a3b4c5d";
    result.unclaimed_wins =
        std::vector<SdkCtfWin>{SdkWin(11, "provider", "is"), SdkWin(10, "provider", "is"), otherClient};
    Check(CtfWinsFrom(result) == std::vector<CtfWin>{icelandProvider, icelandProvider, icelandProvider},
          "one line per win");
    // a win in a role this build does not know, or with no country to name,
    // has no line, and the others keep theirs
    result.unclaimed_wins = std::vector<SdkCtfWin>{SdkWin(11, "validator", "is"), SdkWin(10, "provider", ""),
                                                   SdkWin(9, "provider", "ke")};
    Check(CtfWinsFrom(result) == std::vector<CtfWin>{kenyaProvider}, "only the wins it can name");
    result.unclaimed_wins = std::vector<SdkCtfWin>{SdkWin(11, "provider", "is")};
    result.error = SdkError{};
    Check(CtfWinsFrom(result).empty(), "an answer with an error says nothing, never no wins");
  }
  {
    TEST_CASE("ctf: after a claim that paid, the wins are read at Done and 30 s, 60 s and 120 s after it");
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    Check(kCtfWinsReadsAfterClaim == std::array<milliseconds, 3>{seconds(30), seconds(60), seconds(120)},
          "every app's schedule");
    const CtfWin iceland{CtfRole::Provider, "is"};
    WinsPage page;
    page.Seed({iceland});
    // the server reads the claim at the finalized head: it still says
    // unclaimed at Done and 30 s after it, and claimed from then on
    page.answers = {std::vector<CtfWin>{iceland}, std::vector<CtfWin>{iceland}};
    // the dialog keeps its own events, and its Done comes before the page's read
    LedgerClaimProgress dialog = Dialog({CtfAccount()});
    Check(dialog.Start(), "Claim");
    LedgerClaimEvents dialogEvents;
    dialogEvents.sent = [&](int64_t noId, int64_t index, std::string txHash) {
      page.heard.push_back("sent");
      dialog.Sent(noId, index, txHash);
    };
    dialogEvents.confirmed = [&](int64_t noId, int64_t index, std::string txHash, int64_t amountRao) {
      page.heard.push_back("confirmed");
      dialog.Confirmed(noId, index, txHash, amountRao);
    };
    dialogEvents.done = [&] {
      page.heard.push_back("done");
      dialog.Done();
    };
    // through the SDK's callback, as WalletPage hands it to snLedgerClaim
    const SdkLedgerClaimCallback callback =
        LedgerClaimCallbackFor<SdkLedgerClaimCallback>(page.Claimer(dialogEvents));
    callback.sent(kCtfNoId, 9, "0xclaim");
    callback.confirmed(kCtfNoId, 9, "0xclaim", 50'000'000);
    callback.done();
    Check(page.heard == std::vector<std::string>{"sent", "confirmed", "done", "read"},
          "the dialog's events, then the read at Done");
    CheckRow(dialog.rows()[0], kCtfNoId, 9, 50'000'000, LedgerClaimState::Confirmed, "0xclaim", "",
             "the dialog's row");
    Check(page.reads == 1 && page.lines() == std::vector<CtfWin>{iceland}, "at Done: still unclaimed");
    Check(page.timer == milliseconds(seconds(30)), "the next read 30 s after Done");
    page.Fire();
    Check(page.reads == 2 && page.lines() == std::vector<CtfWin>{iceland}, "30 s after Done: still unclaimed");
    Check(page.timer == milliseconds(seconds(30)), "the next 60 s after Done");
    page.Fire();
    Check(page.reads == 3 && page.lines().empty(), "60 s after Done: claimed, and the line goes");
    Check(page.timer == milliseconds(seconds(60)), "the next 120 s after Done");
    page.Fire();
    Check(page.reads == 4 && page.lines().empty(), "120 s after Done: the last read");
    Check(!page.timer, "nothing after it");
    page.Fire();
    Check(page.reads == 4, "a stray tick reads nothing");
    Check(page.waits == std::vector<milliseconds>{seconds(30), seconds(30), seconds(60)},
          "30 s, 60 s and 120 s after Done");
  }
  {
    TEST_CASE("ctf: the wins' reads after a claim: what pays, starting over, and a sign-out");
    using std::chrono::milliseconds;
    using std::chrono::seconds;
    // a claim that paid nothing: the read at Done, as after any claim, and none after
    WinsPage none;
    LedgerClaimEvents nothing = none.Claimer();
    nothing.failed(kCtfNoId, 9, "needs_gas: the gas key cannot pay for the claim");
    nothing.failed(0, 0, "chain_rpc_unreachable: dial tcp: i/o timeout");
    nothing.done();
    Check(none.reads == 1 && !none.timer, "paid nothing: no read after Done");
    // already_claimed for an account is paid, another sender's claim paying the
    // same coldkey; for no pool it is not
    WinsPage already;
    LedgerClaimEvents third = already.Claimer();
    third.failed(1, 42, "already_claimed: another sender claimed it");
    third.done();
    Check(already.reads == 1 && already.timer == milliseconds(seconds(30)), "already_claimed pays");
    WinsPage noPool;
    LedgerClaimEvents whole = noPool.Claimer();
    whole.failed(0, 0, "already_claimed: nothing left");
    whole.done();
    Check(!noPool.timer, "already_claimed for no pool pays nothing");

    WinsPage page;
    LedgerClaimEvents first = page.Claimer();
    first.confirmed(1, 42, "0xone", 3'241'000'000);
    first.done();
    Check(page.reads == 1 && page.timer == milliseconds(seconds(30)), "the first claim paid");
    // one that paid nothing leaves the first claim's reads be
    LedgerClaimEvents unpaid = page.Claimer();
    unpaid.failed(kCtfNoId, 9, "claim_failed: execution reverted");
    unpaid.done();
    Check(page.reads == 2 && page.timer == milliseconds(seconds(30)), "the first claim's wait stands");
    page.Fire();
    Check(page.reads == 3 && page.timer == milliseconds(seconds(30)), "the first claim's 30 s read");
    // the next paying claim's Done starts them over, and a tick of the wait it
    // replaced, already on its way, reads nothing
    const uint32_t replaced = page.timerToken;
    LedgerClaimEvents second = page.Claimer();
    second.confirmed(kCtfNoId, 9, "0xtwo", 50'000'000);
    second.done();
    Check(page.reads == 4 && page.timer == milliseconds(seconds(30)), "30 s from the second Done");
    page.Tick(replaced);
    Check(page.reads == 4 && page.timer == milliseconds(seconds(30)), "the replaced wait's tick reads nothing");
    page.Fire();
    page.Fire();
    Check(page.reads == 6 && page.timer == milliseconds(seconds(60)), "60 s from it, then 120 s");
    Check(page.waits == std::vector<milliseconds>{seconds(30), seconds(30), seconds(30), seconds(30),
                                                  seconds(60)},
          "the waits");
    // a claim that started before a sign-out and whose Done lands after it
    // starts no reads for the next account
    LedgerClaimEvents before = page.Claimer();
    // a sign-out: none of the reads left, even from a tick already on its way
    page.SignOut();
    page.Fire();
    Check(page.reads == 6 && !page.timer, "no read after a sign-out");
    before.confirmed(1, 42, "0xbefore", 3'241'000'000);
    before.done();
    Check(!page.timer, "a claim from before the sign-out starts no reads");
    // the next account's paying claim starts its own
    LedgerClaimEvents next = page.Claimer();
    next.confirmed(1, 42, "0xthree", 3'241'000'000);
    next.done();
    page.Fire();
    Check(page.reads == 9 && page.timer == milliseconds(seconds(30)), "the next account's reads");
  }
  {
    TEST_CASE("ctf: a read of the wins that failed keeps the last good read's lines");
    const CtfWin iceland{CtfRole::Provider, "is"};
    const CtfWin laos{CtfRole::Extender, "la"};
    // what a read answers: the wins of an answer the SDK could read, a good
    // read with none, and nothing at all for one with an Error or without the
    // list, which the SDK always sends
    SdkCtfClientResult read;
    read.unclaimed_wins = std::vector<SdkCtfWin>{SdkWin(11, "provider", "is")};
    Check(CtfWinsReadFrom(read) == std::optional<std::vector<CtfWin>>{std::vector<CtfWin>{iceland}},
          "a good read's wins");
    read.unclaimed_wins = std::vector<SdkCtfWin>{};
    Check(CtfWinsReadFrom(read) == std::optional<std::vector<CtfWin>>{std::vector<CtfWin>{}},
          "a good read with none");
    read.unclaimed_wins = std::vector<SdkCtfWin>{SdkWin(11, "provider", "is")};
    read.error = SdkError{};
    Check(!CtfWinsReadFrom(read), "an answer with an Error: a read that failed");
    Check(!CtfWinsReadFrom(SdkCtfClientResult{}), "an answer without the list: a read that failed");
    // a failure between two good reads leaves the lines as they were, and a
    // good read that no longer lists a win takes its line away
    CtfWinsLines lines;
    Check(lines.Apply(lines.Start(), std::vector<CtfWin>{iceland, laos}, CtfAnswer::Network) &&
              lines.lines() == std::vector<CtfWin>{iceland, laos},
          "a good read");
    Check(!lines.Apply(lines.Start(), std::nullopt, CtfAnswer::Network) && lines.lines() == std::vector<CtfWin>{iceland, laos},
          "a read that failed keeps the lines");
    Check(lines.Apply(lines.Start(), std::vector<CtfWin>{laos}, CtfAnswer::Network) && lines.lines() == std::vector<CtfWin>{laos},
          "a good read replaces them");
    // reads overlap, each started at its time, and answer in any order: the
    // lines show the newest read's good answer
    const uint32_t slow = lines.Start();
    const uint32_t quick = lines.Start();
    Check(lines.Apply(quick, std::vector<CtfWin>{iceland}, CtfAnswer::Network) && lines.lines() == std::vector<CtfWin>{iceland},
          "the newer read answers first");
    Check(!lines.Apply(slow, std::vector<CtfWin>{}, CtfAnswer::Network) && lines.lines() == std::vector<CtfWin>{iceland},
          "an older read's late answer is dropped");
    // a newer read that fails drops no older read's good answer still to land
    const uint32_t older = lines.Start();
    const uint32_t newer = lines.Start();
    Check(!lines.Apply(newer, std::nullopt, CtfAnswer::Network) && lines.lines() == std::vector<CtfWin>{iceland}, "the newer fails");
    Check(lines.Apply(older, std::vector<CtfWin>{}, CtfAnswer::Network) && lines.lines().empty(),
          "the older read's good answer still shows: the win claimed");
    // a sign-out: no lines, and no answer of a read from before shows
    lines.Apply(lines.Start(), std::vector<CtfWin>{iceland}, CtfAnswer::Network);
    const uint32_t before = lines.Start();
    lines.Reset();
    Check(lines.lines().empty(), "a sign-out drops the lines");
    Check(!lines.Apply(before, std::vector<CtfWin>{laos}, CtfAnswer::Network) && lines.lines().empty(),
          "a read from before the sign-out shows nothing");
    Check(lines.Apply(lines.Start(), std::vector<CtfWin>{laos}, CtfAnswer::Network) && lines.lines() == std::vector<CtfWin>{laos},
          "the next account's first good read");
    // through the page: the read at a claim's Done fails, then the 30 s read
    // still lists the win, then the 60 s read says it is claimed
    WinsPage page;
    page.Seed({iceland});
    page.answers = {std::nullopt, std::vector<CtfWin>{iceland}, std::nullopt};
    page.settled = std::vector<CtfWin>{};
    LedgerClaimEvents claim = page.Claimer();
    claim.confirmed(kCtfNoId, 9, "0xclaim", 50'000'000);
    claim.done();
    Check(page.reads == 1 && page.lines() == std::vector<CtfWin>{iceland}, "a failed read at Done keeps the line");
    page.Fire();
    Check(page.reads == 2 && page.lines() == std::vector<CtfWin>{iceland}, "30 s after Done: still unclaimed");
    page.Fire();
    Check(page.reads == 3 && page.lines() == std::vector<CtfWin>{iceland}, "60 s after Done: failed, kept");
    page.Fire();
    Check(page.reads == 4 && page.lines().empty(), "120 s after Done: claimed, and the line goes");
    // a sign-out drops the lines, and the next account's first read that fails shows none
    page.Seed({iceland});
    page.SignOut();
    Check(page.lines().empty(), "a sign-out drops them");
    page.answers = {std::nullopt};
    page.Read();
    Check(page.lines().empty(), "the first read after a sign-in shows none until one answers");
  }
  {
    TEST_CASE("ctf: a network answer beats a client answer");
    const CtfWin iceland{CtfRole::Provider, "is"};
    const CtfWin laos{CtfRole::Extender, "la"};
    const CtfWin kenya{CtfRole::Provider, "ke"};
    using Lines = std::vector<CtfWin>;
    // whose answer: the SDK's ClientId, "" for the network's, else the client's
    SdkCtfClientResult network;
    network.unclaimed_wins = std::vector<SdkCtfWin>{SdkWin(11, "provider", "is")};
    Check(CtfAnswerOf(network) == CtfAnswer::Network, "no client named: the network's answer");
    SdkCtfClientResult client = network;
    client.client_id = "3f6c2a1e-8d4b-4c7a-9e21-5b0d7f3a9c64";
    Check(CtfAnswerOf(client) == CtfAnswer::Client, "a client named: that client's alone");

    // a client answer after a network answer leaves the network's lines,
    // as a read that failed would, whatever it holds
    CtfWinsLines lines;
    Check(lines.Apply(lines.Start(), Lines{iceland, laos}, CtfAnswer::Network) &&
              lines.lines() == Lines{iceland, laos},
          "the network's lines");
    Check(!lines.Apply(lines.Start(), Lines{iceland}, CtfAnswer::Client) && lines.lines() == Lines{iceland, laos},
          "a newer client answer leaves them");
    Check(!lines.Apply(lines.Start(), Lines{}, CtfAnswer::Client) && lines.lines() == Lines{iceland, laos},
          "a client answer with no win too");
    // the next network answer replaces them as before, and an older one than
    // the one shown is dropped
    Check(lines.Apply(lines.Start(), Lines{laos}, CtfAnswer::Network) && lines.lines() == Lines{laos},
          "the next network answer: a win claimed");
    const uint32_t slow = lines.Start();
    Check(lines.Apply(lines.Start(), Lines{kenya}, CtfAnswer::Network) && lines.lines() == Lines{kenya},
          "a newer network answer");
    Check(!lines.Apply(slow, Lines{laos}, CtfAnswer::Network) && lines.lines() == Lines{kenya},
          "an older network answer's late answer is dropped");

    // a client answer alone, as from a server before the network answer,
    // whose every answer is a client's: its lines show, as any good read's
    CtfWinsLines old;
    Check(old.Apply(old.Start(), Lines{iceland, laos}, CtfAnswer::Client) && old.lines() == Lines{iceland, laos},
          "a client answer alone shows its lines");
    Check(!old.Apply(old.Start(), std::nullopt, CtfAnswer::Client) && old.lines() == Lines{iceland, laos},
          "a read that failed keeps them");
    const uint32_t older = old.Start();
    Check(old.Apply(old.Start(), Lines{laos}, CtfAnswer::Client) && old.lines() == Lines{laos},
          "the next client answer replaces them");
    Check(!old.Apply(older, Lines{iceland}, CtfAnswer::Client) && old.lines() == Lines{laos},
          "an older client answer's late answer is dropped");

    // the network's answer replaces a client answer's lines, even from a read
    // that started first: its network read was slow, and the next read's
    // failed and fell back to the client token
    CtfWinsLines mixed;
    const uint32_t first = mixed.Start();
    const uint32_t second = mixed.Start();
    Check(mixed.Apply(second, Lines{iceland}, CtfAnswer::Client) && mixed.lines() == Lines{iceland},
          "the client answer shows while no network answer has");
    Check(mixed.Apply(first, Lines{iceland, laos}, CtfAnswer::Network) && mixed.lines() == Lines{iceland, laos},
          "the network answer beats it");
    Check(!mixed.Apply(mixed.Start(), Lines{iceland}, CtfAnswer::Client) && mixed.lines() == Lines{iceland, laos},
          "and no client answer replaces its lines since");

    // a sign-out: no network answer has shown since, so a client answer shows
    // again, and no answer of a read from before, network or not, shows
    const uint32_t before = mixed.Start();
    mixed.Reset();
    Check(!mixed.Apply(before, Lines{kenya}, CtfAnswer::Network) && mixed.lines().empty(),
          "a network answer from before the sign-out");
    Check(mixed.Apply(mixed.Start(), Lines{kenya}, CtfAnswer::Client) && mixed.lines() == Lines{kenya},
          "the next account's client answer shows");

    // through the page: the read at a claim's Done fell back to the client
    // token, whose answer lists this client's win alone; the other client's
    // line stays, and the network's answer 30 s after Done says the win is
    // claimed
    WinsPage page;
    page.Seed({iceland, laos});
    page.answers = {Lines{iceland}, Lines{laos}};
    page.from = {CtfAnswer::Client, CtfAnswer::Network};
    LedgerClaimEvents paid = page.Claimer();
    paid.confirmed(kCtfNoId, 9, "0xclaim", 50'000'000);
    paid.done();
    Check(page.reads == 1 && page.lines() == Lines{iceland, laos}, "the client answer at Done leaves the lines");
    page.Fire();
    Check(page.reads == 2 && page.lines() == Lines{laos}, "the network's answer 30 s after Done");
  }
  {
    TEST_CASE("v2 claim: the claim is the session's, not its dialog's");
    LedgerAccount paidAccount = Account();
    paidAccount.claimableRao = 0;
    paidAccount.paidRao = paidAccount.finalizedRao;
    ClaimPage page;
    page.accounts = {Account(), CtfAccount()};
    Check(page.Section().claimEnabled, "Claim");
    page.Open();
    Check(page.Press() && page.sdk.size() == 1, "Claim both: one claim");
    Check(page.claim.InFlight() && page.dialog == page.claim.Progress(), "the dialog's progress is the claim in flight");
    const std::shared_ptr<LedgerClaimProgress> first = page.claim.Progress();

    // the dialog open: each event draws it again
    page.sdk[0].sent(1, 42, "0xbatch");
    page.Drain();
    Check(page.redraws == 1, "an event redraws the open dialog");

    // dismissed during the claim: its events still move the claim
    page.Close();
    page.sdk[0].sent(kCtfNoId, 9, "0xbatch");
    page.sdk[0].confirmed(1, 42, "0xbatch", 3'241'000'000);
    page.Drain();
    Check(page.redraws == 1, "nothing to redraw with no dialog up");
    // the page has read the accounts since (a visit to Earnings), and the
    // confirmed one is paid: Claim still opens the claim in flight
    page.accounts = {paidAccount, CtfAccount()};
    Check(page.Section().claimEnabled, "Claim opens the claim in flight whatever the read says since");

    // opened again: the claim as it went on, its Claim held
    page.Open();
    Check(page.dialog == first, "the claim in flight's progress, not a new one over the read");
    CheckRow(page.dialog->rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Confirmed, "0xbatch", "",
             "the account confirmed while no dialog was up");
    CheckRow(page.dialog->rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Sent, "0xbatch", "",
             "the account still sent");
    Check(page.dialog->sending() && !page.dialog->CanClaim(true), "Claim held");
    Check(!page.Press() && page.sdk.size() == 1, "no second claim from the dialog opened again");
    // nor from a dialog over the read
    auto overTheRead = std::make_shared<LedgerClaimProgress>(std::vector<LedgerAccount>{CtfAccount()}, 0.5);
    Check(!page.claim.Start(overTheRead) && !overTheRead->sending() && page.claim.Progress() == first,
          "no second claim while one is in flight");

    // its Done with the dialog closed again: it lands, the page reads again
    page.Close();
    page.sdk[0].confirmed(kCtfNoId, 9, "0xbatch", 50'000'000);
    page.sdk[0].done();
    page.Drain();
    Check(!page.claim.InFlight() && !first->sending(), "Done landed with no dialog up");
    Check(page.reads == std::vector<bool>{true}, "the page read again after it, and it paid");
    page.accounts = {paidAccount};
    Check(!page.Section().claimEnabled, "Claim follows the read again");

    // opened after Done: a dialog over the read, which claims anew
    page.accounts = {Account()};
    page.Open();
    Check(page.dialog != first && !page.dialog->sending() && page.dialog->CanClaim(true), "a dialog over the read");
    Check(page.Press() && page.sdk.size() == 2, "the next claim");

    // the dialog pressed again after its claim's Done claims on from its own
    // progress
    page.sdk[1].failed(1, 42, "claim_failed: execution reverted");
    page.sdk[1].done();
    page.Drain();
    const std::shared_ptr<LedgerClaimProgress> retry = page.dialog;
    Check(!page.claim.InFlight() && retry->CanClaim(true), "failed: Claim again");
    Check(page.Press() && page.claim.Progress() == retry && page.sdk.size() == 3,
          "the same dialog's progress claims again");
  }
  {
    TEST_CASE("v2 claim: a sign-out drops the claim in flight");
    ClaimPage page;
    page.accounts = {Account()};
    page.Open();
    Check(page.Press(), "Claim");
    const std::shared_ptr<LedgerClaimProgress> departed = page.claim.Progress();
    page.SignOut();
    Check(!page.claim.InFlight(), "the page holds it no more");
    Check(page.dialog == departed, "a dialog up from before keeps its progress");
    // the device's claim ends with its session: its events move only its own
    // progress, which the dialog shows to its end
    page.sdk[0].failed(0, 0, "chain_rpc_unreachable: context canceled");
    page.sdk[0].done();
    page.Drain();
    Check(!departed->sending() && departed->failure() == "chain_rpc_unreachable: context canceled",
          "the dialog up from before shows the claim's end");
    Check(!page.claim.InFlight(), "and its Done ends no claim");
    page.Close();
    // the next account's read: Claim follows it, and its claim starts
    LedgerAccount paidAccount = Account();
    paidAccount.claimableRao = 0;
    page.accounts = {paidAccount};
    Check(!page.Section().claimEnabled, "Claim follows the next account's read");
    page.accounts = {Account()};
    page.Open();
    Check(page.dialog != departed && page.Press() && page.sdk.size() == 2, "the next account's claim");

    // a departed claim's Done that lands after the next account's claim
    // started ends nothing of it
    ClaimPage late;
    late.accounts = {Account()};
    late.Open();
    Check(late.Press(), "Claim");
    late.Close();
    late.SignOut();
    late.Open();
    Check(late.Press() && late.sdk.size() == 2, "the next account's claim");
    late.sdk[0].done();
    late.Drain();
    Check(late.claim.InFlight() && late.claim.Progress() == late.dialog && late.dialog->sending(),
          "the departed claim's Done ends nothing of the next one's");
    Check(!late.Press() && late.sdk.size() == 2, "and its Claim stays held");
  }
  {
    TEST_CASE("v2 claim: none while a v1 claim is in flight");
    // the SDK reads the gas key's pending nonce as it signs, with no lock
    // across a v1 and a v2 claim: the page sends one at a time (Linux's too)
    ClaimPage page;
    page.accounts = {Account()};
    page.v1InFlight = true;
    page.Open();
    Check(!page.ClaimEnabled(), "Claim waits for the v1 claim");
    Check(!page.Press() && page.sdk.empty() && !page.claim.InFlight() && !page.dialog->sending(),
          "no v2 claim while it runs");
    page.v1InFlight = false;
    Check(page.ClaimEnabled() && page.Press() && page.sdk.size() == 1, "after its Done, Claim");
  }
  {
    TEST_CASE("v1 claim: the claim in flight is the session's too");
    // the v1 dialog's progress as the claim in flight needs it
    // (EarningsSheets.h ClaimAlphaProgress): Start takes the claimable epochs
    struct Epochs {
      int claimable = 2;
      bool sending = false;
      bool Start() {
        if (sending || claimable == 0) return false;
        sending = true;
        return true;
      }
    };
    ClaimInFlight<Epochs> v1;
    auto dialog = std::make_shared<Epochs>();
    const std::optional<uint64_t> ticket = v1.Start(dialog);
    Check(ticket && v1.InFlight() && v1.Progress() == dialog && dialog->sending, "Claim: the claim in flight");
    // the dialog dismissed and opened again: the claim in flight's progress,
    // and no second claim, from it or from a dialog over the read
    Check(v1.Progress() == dialog, "opened again on the claim in flight");
    Check(!v1.Start(dialog) && !v1.Start(std::make_shared<Epochs>()), "no second claim while one is in flight");
    // its Done ends it, once
    Check(v1.End(*ticket) && !v1.InFlight() && !v1.Progress(), "Done");
    Check(!v1.End(*ticket), "a Done twice ends nothing");
    // nothing to claim starts nothing
    auto none = std::make_shared<Epochs>();
    none->claimable = 0;
    Check(!v1.Start(none) && !v1.InFlight(), "no epoch to claim: no claim");
    // a sign-out drops the claim in flight, and its Done, landing after the
    // next account's claim started, ends nothing of it
    const std::optional<uint64_t> departed = v1.Start(std::make_shared<Epochs>());
    v1.Drop();
    Check(!v1.InFlight(), "a sign-out drops it");
    auto next = std::make_shared<Epochs>();
    const std::optional<uint64_t> nextTicket = v1.Start(next);
    Check(departed && nextTicket && !v1.End(*departed) && v1.Progress() == next,
          "the departed claim's Done ends nothing of the next one's");
  }
  {
    TEST_CASE("v2: a claim in flight keeps the section's Claim");
    LedgerAccount paidAccount = Account();
    paidAccount.claimableRao = 0;
    Check(!LedgerSectionFor(true, true, LedgerFetch::Ready, {paidAccount}, true).claimEnabled,
          "nothing claimable: no Claim");
    Check(LedgerSectionFor(true, true, LedgerFetch::Ready, {paidAccount}, true, {}, 0, true).claimEnabled,
          "a claim in flight: Claim opens it");
    Check(LedgerSectionFor(true, true, LedgerFetch::Failed, {}, true, {}, 0, true).claimEnabled,
          "a read that failed since: Claim still opens it");
    Check(LedgerSectionFor(true, true, LedgerFetch::Ready, {Account()}, false, {}, 0, true).claimEnabled,
          "with no device session it still opens the claim");
    const LedgerSectionView before = LedgerSectionFor(true, false, LedgerFetch::Ready, {Account()}, true, {}, 0, true);
    Check(!before.showClaim && !before.claimEnabled, "before vault2 there is no Claim");
    Check(!LedgerSectionFor(false, true, LedgerFetch::Ready, {Account()}, true, {}, 0, true).shown,
          "no section without a coldkey");
  }
  {
    TEST_CASE("v2 claim: the figure during a claim is what the claim moves, never 0");
    constexpr int64_t kFloor = 100'000'000;
    LedgerAccount second = Account();
    second.noId = 2;
    second.index = 7;
    second.claimableRao = 1'000'000'000;
    // capture the flag's 0.05 under the floor, and a pool that could not be read
    LedgerClaimProgress dialog = Dialog({Account(), second, CtfAccount()}, kFloor, {Unavailable(3, PoolKind::Pool)});
    Check(dialog.FigureRao() == 4'241'000'000 && dialog.TotalKnown(), "between claims: what Claim moves");
    Check(dialog.Start(), "Claim");
    Check(dialog.FigureRao() == 4'241'000'000 && dialog.TotalKnown(),
          "during the claim: the known accounts above the floor it sends");
    // the SDK tries the account under the floor too, and claims one the page
    // did not list: neither is in the figure
    dialog.Sent(kCtfNoId, 9, "0xbatch");
    dialog.Sent(5, 1, "0xbatch");
    Check(dialog.FigureRao() == 4'241'000'000, "neither an account under the floor nor one of an amount not known");
    // each account confirmed: the figure stays what the claim moves, never 0
    dialog.Confirmed(1, 42, "0xbatch", 3'241'000'000);
    Check(dialog.FigureRao() == 4'241'000'000, "one confirmed: the same figure");
    dialog.Confirmed(2, 7, "0xbatch", 1'000'000'000);
    Check(dialog.FigureRao() == 4'241'000'000 && dialog.TotalKnown(),
          "everything it moves confirmed before Done: still its figure, never 0");
    dialog.Done();
    Check(!dialog.TotalKnown(), "after Done the rows left say why: no figure, never 0");

    // Claim both says what the claim moves while it runs
    LedgerClaimProgress both = Dialog({Account(), CtfAccount()});
    Check(both.Start() && both.ClaimsBoth() && both.FigureRao() == 3'291'000'000, "Claim both");
    both.Confirmed(1, 42, "0xbatch", 3'241'000'000);
    Check(both.ClaimsBoth() && both.FigureRao() == 3'291'000'000, "one confirmed: still Claim both, its figure");
    // an account the gas key could not pay for: the figure and Claim both stay
    // the press's until Done (every app, 2026-11-12), and the account's own
    // line is claimable again, with the funding hint; after Done it is what
    // Claim moves
    both.Failed(kCtfNoId, 9, "needs_gas: the gas key cannot pay for the claim");
    Check(both.ClaimsBoth() && both.FigureRao() == 3'291'000'000 && both.TotalKnown(),
          "short of gas: the press's figure until Done");
    CheckRow(both.rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Idle, "", "", "its own line, claimable");
    Check(!both.rows()[1].underFloor && both.needsGas(), "with the funding hint");
    both.Done();
    Check(!both.ClaimsBoth() && both.FigureRao() == 50'000'000 && both.TotalKnown(),
          "after Done: what Claim moves, the account short of gas");

    // a claim that sends nothing it took: still the press's figure until Done,
    // never a zero
    LedgerClaimProgress none = Dialog({Account()});
    Check(none.Start(), "Claim");
    none.Failed(1, 42, "needs_gas: the gas key cannot pay for the claim");
    Check(none.sending() && none.FigureRao() == 3'241'000'000 && none.TotalKnown(), "the press's figure");

    // a claim that moved everything, after Done: a true 0 between claims
    LedgerClaimProgress all = Dialog({Account()});
    Check(all.Start(), "Claim");
    all.Confirmed(1, 42, "0xone", 3'241'000'000);
    Check(all.FigureRao() == 3'241'000'000 && all.TotalKnown(), "confirmed before Done: its figure");
    all.Done();
    Check(all.FigureRao() == 0 && all.TotalKnown(), "after Done nothing is left: a true 0");
  }
  {
    TEST_CASE("ctf claim: an account the dialog lists under the floor, which the SDK then tries");
    // the SDK tries every account with something to claim, capture the flag's
    // 0.05 under the floor too: the dialog shows what it answers, as Android's,
    // Linux's and Apple's do, and no press of this dialog ever takes it
    constexpr int64_t kFloor = 100'000'000;
    const std::string needsGas =
        "needs_gas: this claim needs up to 0.0173 TAO of gas, the gas key 0x00aa has 0.0050 TAO; "
        "send TAO to 5GAS";
    const std::string tooLow = "not_claimable: AmountTooLow: the claim is below the runtime's transfer floor";
    // needs_gas: claimable under the funding hint, never Below the claim minimum
    LedgerClaimProgress gas = Dialog({Account(), CtfAccount()}, kFloor);
    Check(gas.rows()[1].underFloor && gas.Start(), "listed under the floor; Claim the provider pool's");
    Check(gas.rows()[1].state == LedgerClaimState::Idle, "it is not sending");
    gas.Sent(1, 42, "0xone");
    gas.Confirmed(1, 42, "0xone", 3'241'000'000);
    gas.Failed(kCtfNoId, 9, needsGas);
    gas.Done();
    CheckRow(gas.rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Idle, "", "", "claimable");
    Check(!gas.rows()[1].underFloor && !gas.rows()[1].unavailable, "not below the claim minimum");
    Check(gas.needsGas(), "under the funding hint");
    Check(gas.ClaimableRao() == 0 && !gas.ClaimsBoth() && !gas.CanClaim(true) && !gas.TotalKnown(),
          "still no claim of it, and no total");
    // another failure, after the chain took it in the batch: Failed with its
    // message, and the next press leaves it out
    LedgerClaimProgress failed = Dialog({Account(), CtfAccount()}, kFloor);
    Check(failed.Start(), "Claim");
    failed.Sent(1, 42, "0xbatch");
    failed.Sent(kCtfNoId, 9, "0xbatch");
    failed.Failed(1, 42, "claim_failed: execution reverted");
    failed.Failed(kCtfNoId, 9, "claim_failed: execution reverted");
    failed.Done();
    CheckRow(failed.rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Failed, "0xbatch",
             "claim_failed: execution reverted", "Failed, with its message");
    Check(!failed.rows()[1].underFloor, "not below the claim minimum");
    Check(failed.ClaimableRao() == 3'241'000'000 && !failed.ClaimsBoth(),
          "the next press takes the provider pool's alone, never Claim both");
    // that press lists it Below the claim minimum again until the SDK reports
    Check(failed.Start(), "Claim again");
    CheckRow(failed.rows()[0], 1, 42, 3'241'000'000, LedgerClaimState::Sending, "0xbatch", "",
             "the provider pool's, sending");
    CheckRow(failed.rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Idle, "0xbatch", "",
             "capture the flag's, waiting");
    Check(failed.rows()[1].underFloor, "below the claim minimum again");
    // and AmountTooLow alone reads Below the claim minimum once the SDK tried it
    failed.Failed(kCtfNoId, 9, tooLow);
    failed.Done();
    Check(failed.rows()[1].underFloor && failed.rows()[1].state == LedgerClaimState::Idle &&
              failed.rows()[1].message.empty(),
          "AmountTooLow: below the claim minimum");
    // one still Sent when its press ended keeps that state at the next press
    // (Linux's), and is not taken
    LedgerClaimProgress inflight = Dialog({Account(), CtfAccount()}, kFloor);
    Check(inflight.Start(), "Claim");
    inflight.Sent(kCtfNoId, 9, "0xslow");
    inflight.Failed(1, 42, "claim_failed: execution reverted");
    inflight.Done();
    Check(inflight.ClaimableRao() == 3'241'000'000 && inflight.Start(), "Claim the provider pool's again");
    CheckRow(inflight.rows()[1], kCtfNoId, 9, 50'000'000, LedgerClaimState::Sent, "0xslow", "", "still Sent");
    // a lone account under the floor: never a Claim of it
    LedgerAccount small = Account();
    small.claimableRao = 1;
    LedgerClaimProgress lone = Dialog({small}, kFloor);
    Check(!lone.CanClaim(true) && !lone.Start() && !lone.TotalKnown(), "nothing for Claim to send, and no total");
  }

  {
    TEST_CASE("v2 at the SDK's boundary: an account's fields");
    SdkLedgerAccount sdk;
    sdk.vault = "0x00000000000000000000000000000000000000aa";
    sdk.no_id = 1;
    sdk.kind = "pool";
    sdk.label = "Pool 1";
    sdk.index = 42;
    sdk.finalized_rao = 3'741'000'000;
    sdk.claimable_rao = 3'241'000'000;
    sdk.paid_rao = 500'000'000;
    sdk.pending = true;
    sdk.pending_rao = 3'861'000'000;
    LedgerAccount want = Account();
    want.pending = true;
    want.pendingRao = 3'861'000'000;
    Check(LedgerAccountFrom(sdk) == want, "every field");
    sdk.kind = "";
    Check(LedgerAccountFrom(sdk) == want, "no kind, as an older server's entry: a pool");
    SdkLedgerAccount ctf;
    ctf.vault = sdk.vault;
    ctf.no_id = kCtfNoId;
    ctf.kind = "ctf";
    ctf.label = "Capture the flag";
    ctf.index = 9;
    ctf.finalized_rao = 50'000'000;
    ctf.claimable_rao = 50'000'000;
    Check(LedgerAccountFrom(ctf) == CtfAccount(), "a capture the flag account");
    SdkUnavailablePool unavailable;
    unavailable.vault = sdk.vault;
    unavailable.no_id = kCtfNoId;
    unavailable.kind = "ctf";
    unavailable.label = "Capture the flag";
    Check(UnavailablePoolFrom(unavailable) == Unavailable(kCtfNoId, PoolKind::Ctf),
          "an unavailable pool");
    Check(UnavailablePoolsFrom(std::optional<SdkUnavailablePoolList>(SdkUnavailablePoolList{unavailable})) ==
              std::vector<UnavailablePool>{Unavailable(kCtfNoId, PoolKind::Ctf)},
          "the read's unavailable pools");
    Check(UnavailablePoolsFrom(std::optional<SdkUnavailablePoolList>()).empty(), "a list left out is none");
#if defined(URNW_SN_PAYOUT_TESTS_SDK)
    // what the device's SnLedgerAccounts answers with, through the header
    const auto result =
        nlohmann::json::parse(R"({"accounts":[{"vault":"0x00000000000000000000000000000000000000aa",
            "no_id":1,"kind":"pool","label":"Pool 1","index":42,"finalized_rao":3741000000,
            "claimable_rao":3241000000,"paid_rao":500000000,"pending":true,"pending_rao":3861000000},
            {"vault":"0x00000000000000000000000000000000000000aa","no_id":4294967297,"kind":"ctf",
            "label":"Capture the flag","index":9,"finalized_rao":50000000,"claimable_rao":50000000,
            "paid_rao":0,"pending":false,"pending_rao":0}],"unavailable_pools":[],"partial":false,
            "coldkey_ss58":"5SAMPLE"})")
            .get<urnet::SnLedgerAccountsResult>();
    Check(result.accounts.has_value() && result.accounts->size() == 2, "the header's list");
    if (result.accounts && result.accounts->size() == 2) {
      Check(LedgerAccountFrom(result.accounts->front()) == want, "the header's account");
      Check(LedgerAccountFrom(result.accounts->back()) == CtfAccount(), "the header's capture the flag account");
    }
    Check(UnavailablePoolsFrom(result.unavailable_pools).empty() && !result.partial, "a whole answer");
    const auto partial =
        nlohmann::json::parse(R"({"accounts":[{"vault":"0x00000000000000000000000000000000000000aa",
            "no_id":1,"kind":"pool","label":"Pool 1","index":42,"finalized_rao":3741000000,
            "claimable_rao":3241000000,"paid_rao":500000000,"pending":true,"pending_rao":3861000000}],
            "unavailable_pools":[{"vault":"0x00000000000000000000000000000000000000aa",
            "no_id":4294967297,"kind":"ctf","label":"Capture the flag"}],"partial":true})")
            .get<urnet::SnLedgerAccountsResult>();
    Check(partial.partial && UnavailablePoolsFrom(partial.unavailable_pools) ==
                                 std::vector<UnavailablePool>{Unavailable(kCtfNoId, PoolKind::Ctf)},
          "the header's unavailable pools");
    const auto older =
        nlohmann::json::parse(R"({"accounts":[{"vault":"0x00000000000000000000000000000000000000aa",
            "no_id":1,"index":42,"finalized_rao":3741000000,"claimable_rao":3241000000,
            "paid_rao":500000000,"pending":true,"pending_rao":3861000000}]})")
            .get<urnet::SnLedgerAccountsResult>();
    Check(older.accounts && older.accounts->size() == 1 && LedgerAccountFrom(older.accounts->front()) == want &&
              UnavailablePoolsFrom(older.unavailable_pools).empty(),
          "an answer without the new fields reads as before");
    const auto failed =
        nlohmann::json::parse(
            R"({"accounts":[],"error":{"code":"ledger_unavailable","message":"503"}})")
            .get<urnet::SnLedgerAccountsResult>();
    Check(failed.error.has_value() &&
              failed.error->code == std::optional<std::string>("ledger_unavailable"),
          "the header's error code");
#endif
  }
  {
    TEST_CASE("ctf at the SDK's boundary: the relay's accounts and the unclaimed wins");
    SdkRelayAccount relay;
    relay.coldkey_ss58 = "5COLD";
    relay.no_id = kCtfNoId;
    relay.kind = "ctf";
    relay.label = "Capture the flag";
    relay.index = 9;
    relay.claimable_rao = 50'000'000;
    relay.reason = "under_threshold";
    Check(RelayAccountFrom(relay) == Relay(kCtfNoId, PoolKind::Ctf, 50'000'000), "a pending account");
    const std::string hash = "0x" + std::string(64, 'b');
    relay.claimable_rao = 0;
    relay.claim_tx_hash = hash;
    relay.claim_tx_url = "https://evm.taostats.io/tx/" + hash;
    relay.claim_amount_rao = 50'000'000;
    RelayAccount claimed = Relay(kCtfNoId, PoolKind::Ctf, 0);
    claimed.claimTxHash = hash;
    claimed.claimTxUrl = "https://evm.taostats.io/tx/" + hash;
    claimed.claimAmountRao = 50'000'000;
    Check(RelayAccountFrom(relay) == claimed, "URnetwork's claim of it");
    // a claim status's accounts, and one without its list
    SdkClaimStatus withAccounts;
    withAccounts.accounts = std::vector<SdkRelayAccount>{relay};
    Check(RelayAccountsFrom(withAccounts) == std::vector<RelayAccount>{claimed}, "the status's accounts");
    Check(RelayAccountsFrom(SdkClaimStatus{}).empty(), "a list left out is none");
    // one SnCtfWin as the line's win
    Check(CtfWinOf(SdkWin(9, "provider", "is")) == std::optional<CtfWin>(CtfWin{CtfRole::Provider, "is"}),
          "a provider's win");
    Check(CtfWinOf(SdkWin(9, "extender", "is")) == std::optional<CtfWin>(CtfWin{CtfRole::Extender, "is"}),
          "an extender's win");
    Check(!CtfWinOf(SdkWin(9, "validator", "is")) && !CtfWinOf(SdkWin(9, "provider", "")),
          "a role this build does not know, or no country: no line");
#if defined(URNW_SN_PAYOUT_TESTS_SDK)
    // what the device's SnClaimStatus and SnCtfClient answer with, through the header
    const auto status =
        nlohmann::json::parse(R"({"relay_state":"running","action":"claim_now","reason":"queued",
            "coldkey_ss58":"5COLD","claimable_rao":891000000,"transfer_floor_rao":100000000,
            "accounts":[{"coldkey_ss58":"5COLD","no_id":1,"kind":"pool",
            "label":"Pool 1","index":42,"claimable_rao":841000000,"value_tao_rao":0,"pending_rao":0,
            "reason":"queued","claim_now_available":true},{"coldkey_ss58":"5COLD","no_id":4294967297,
            "kind":"ctf","label":"Capture the flag","index":9,"claimable_rao":50000000,
            "value_tao_rao":0,"pending_rao":0,"reason":"under_threshold","claim_now_available":true}],
            "unavailable_pools":[],"partial":false})")
            .get<urnet::SnClaimStatusResult>();
    const std::vector<RelayAccount> accounts = RelayAccountsFrom(status);
    Check(RelayRowsFor(accounts, UnavailablePoolsFrom(status.unavailable_pools), status.transfer_floor_rao) ==
              std::vector<RelayRow>{
                  Row(1, "sn_ledger_provider_pool", RelayRowStatus::Pending, 841'000'000),
                  Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::UnderFloor, 50'000'000)},
          "the header's relay accounts as the sheet's rows");
    RelayTileView tile;
    tile.rows = {Row(1, "sn_ledger_provider_pool", RelayRowStatus::Pending, 841'000'000),
                 Row(kCtfNoId, "sn_ctf_pool", RelayRowStatus::UnderFloor, 50'000'000)};
    Check(RelayTileFor(accounts, status.coldkey_ss58.value_or(std::string()),
                       UnavailablePoolsFrom(status.unavailable_pools), status.transfer_floor_rao) == tile,
          "the header's status as the no-wallet tile");
    const auto client =
        nlohmann::json::parse(R"({"client_id":"3f6c2a1e-8d4b-4c7a-9e21-5b0d7f3a9c64","as_of_millis":1,
            "epochs":[],"wins":[{"epoch":10,"role":"provider","country":"fi","rank":2,"slot_rao":48025660,
            "pot_share_rao":0,"amount_rao":48025660,"pending":false,"root_status":"finalized",
            "coldkey_ss58":"5COLD","client_id":"3f6c2a1e-8d4b-4c7a-9e21-5b0d7f3a9c64","claim_state":"claimed"},
            {"epoch":9,"role":"provider","country":"is","rank":1,"slot_rao":48025660,
            "pot_share_rao":28815396,"amount_rao":76841056,"pending":false,"root_status":"finalized",
            "coldkey_ss58":"5COLD","client_id":"3f6c2a1e-8d4b-4c7a-9e21-5b0d7f3a9c64","claim_state":"unclaimed"},
            {"epoch":9,"role":"extender","country":"is","rank":3,"slot_rao":48025660,
            "pot_share_rao":0,"amount_rao":48025660,"pending":false,"root_status":"pending",
            "coldkey_ss58":"5COLD","client_id":"3f6c2a1e-8d4b-4c7a-9e21-5b0d7f3a9c64","claim_state":"unclaimed"}],
            "latest_win":{"epoch":10,"role":"provider","country":"fi","rank":2,"slot_rao":48025660,
            "pot_share_rao":0,"amount_rao":48025660,"pending":false,"root_status":"finalized",
            "coldkey_ss58":"5COLD","client_id":"3f6c2a1e-8d4b-4c7a-9e21-5b0d7f3a9c64","claim_state":"claimed"},
            "unclaimed_wins":[{"epoch":9,"role":"provider","country":"is","rank":1,"slot_rao":48025660,
            "pot_share_rao":28815396,"amount_rao":76841056,"pending":false,"root_status":"finalized",
            "coldkey_ss58":"5COLD","client_id":"3f6c2a1e-8d4b-4c7a-9e21-5b0d7f3a9c64","claim_state":"unclaimed"},
            {"epoch":9,"role":"extender","country":"is","rank":3,"slot_rao":48025660,
            "pot_share_rao":0,"amount_rao":48025660,"pending":false,"root_status":"pending",
            "coldkey_ss58":"5COLD","client_id":"3f6c2a1e-8d4b-4c7a-9e21-5b0d7f3a9c64","claim_state":"unclaimed"}]})")
            .get<urnet::SnCtfClientResult>();
    Check(CtfWinsFrom(client) ==
              std::vector<CtfWin>{CtfWin{CtfRole::Provider, "is"}, CtfWin{CtfRole::Extender, "is"}},
          "the header's unclaimed wins, not its latest, which is claimed");
    const auto none =
        nlohmann::json::parse(R"({"client_id":"","as_of_millis":1,"epochs":[],"wins":[],"unclaimed_wins":[]})")
            .get<urnet::SnCtfClientResult>();
    Check(CtfWinsFrom(none).empty(), "the header's no wins: no line");
    const auto unauthorized =
        nlohmann::json::parse(R"({"client_id":"","as_of_millis":0,"error":{"code":"ctf_unavailable","message":"503"}})")
            .get<urnet::SnCtfClientResult>();
    Check(CtfWinsFrom(unauthorized).empty(), "the header's error: no line");
    // the read the page applies: the header's no wins is a good read with
    // none, its error a read that failed, which keeps the lines
    Check(CtfWinsReadFrom(none) == std::optional<std::vector<CtfWin>>{std::vector<CtfWin>{}},
          "the header's no wins: a good read with none");
    Check(!CtfWinsReadFrom(unauthorized), "the header's error: a read that failed");
    // and whose answer it is: the client's when it names one, else the network's
    Check(CtfAnswerOf(client) == CtfAnswer::Client, "the header's answer for a client");
    Check(CtfAnswerOf(none) == CtfAnswer::Network, "the header's answer for the network");
#endif
  }
  {
    TEST_CASE("v2 at the SDK's boundary: the claim callback hands each event on");
    std::vector<std::string> heard;
    LedgerClaimEvents events;
    events.sent = [&heard](int64_t noId, int64_t index, std::string txHash) {
      heard.push_back("sent " + std::to_string(noId) + " " + std::to_string(index) + " " + txHash);
    };
    events.confirmed = [&heard](int64_t noId, int64_t index, std::string txHash,
                                int64_t amountRao) {
      heard.push_back("confirmed " + std::to_string(noId) + " " + std::to_string(index) + " " +
                      txHash + " " + std::to_string(amountRao));
    };
    events.failed = [&heard](int64_t noId, int64_t index, std::string message) {
      heard.push_back("failed " + std::to_string(noId) + " " + std::to_string(index) + " " +
                      message);
    };
    events.done = [&heard]() { heard.push_back("done"); };
    const SdkLedgerClaimCallback callback = LedgerClaimCallbackFor<SdkLedgerClaimCallback>(events);
    callback.sent(1, 42, "0xabc");
    callback.confirmed(1, 42, "0xabc", 3'241'000'000);
    callback.failed(0, 0, "not_claimable: nothing to claim");
    callback.done();
    Check(heard == std::vector<std::string>{"sent 1 42 0xabc", "confirmed 1 42 0xabc 3241000000",
                                            "failed 0 0 not_claimable: nothing to claim", "done"},
          "in order, with their values");
    const SdkLedgerClaimCallback quiet = LedgerClaimCallbackFor<SdkLedgerClaimCallback>({});
    quiet.sent(1, 42, "0xabc");
    quiet.confirmed(1, 42, "0xabc", 1);
    quiet.failed(1, 42, "claim_failed");
    quiet.done();
    Check(heard.size() == 4, "events without a listener go nowhere");
  }

  std::cout << gCases << " cases, " << gFailures << " failures\n";
  return gFailures == 0 ? 0 : 1;
}
