// SPDX-License-Identifier: MPL-2.0
#include "SnPayoutPresentation.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <limits>
#include <memory>

namespace urnw::snpayout {

PayoutLineView PayoutLineFor(bool walletKnown, bool hasColdkey, int64_t totalClaimableRao,
                             const std::optional<EpochSchedule>& schedule, int64_t nowMillis,
                             const std::function<std::string(int64_t)>& formatTime) {
  PayoutLineView view;
  if (!walletKnown) return view;
  if (!hasColdkey) {
    view.kind = LineKind::SetColdkey;
    return view;
  }
  view.kind = LineKind::Schedule;
  if (schedule && nowMillis < schedule->endMillis) {
    view.showTimes = true;
    view.epochEnd = formatTime(schedule->endMillis);
    view.claimOpen = formatTime(schedule->claimOpenMillis);
    view.expiry = formatTime(schedule->expiryMillis);
  }
  view.showClaim = totalClaimableRao > 0;
  return view;
}

std::string FormatScheduleTime(int64_t millis, int32_t utcOffsetMinutes) {
  constexpr int64_t kDayMillis = 86'400'000;
  const int64_t local = millis + static_cast<int64_t>(utcOffsetMinutes) * 60'000;
  int64_t days = local / kDayMillis;
  int64_t rest = local % kDayMillis;
  if (rest < 0) {
    rest += kDayMillis;
    --days;
  }
  // civil-from-days (Howard Hinnant)
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = yoe + era * 400 + (month <= 2 ? 1 : 0);
  const int64_t minutes = rest / 60'000;
  // std::format sizes its own output: GCC cannot bound these fields from the
  // arithmetic above, so an snprintf into a fixed buffer fails the Linux
  // harness build with -Werror=format-truncation
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}", year, month, day, minutes / 60,
                     minutes % 60);
}

PoolKind PoolKindFrom(std::string_view kind) {
  return kind == "ctf" ? PoolKind::Ctf : PoolKind::Pool;
}

PoolKind PoolKindOfNoId(int64_t noId) {
  return noId >= kCtfPoolIdBase ? PoolKind::Ctf : PoolKind::Pool;
}

std::string PoolTitleKey(PoolKind kind, bool providerPool) {
  if (kind == PoolKind::Ctf) return "sn_ctf_pool";
  return providerPool ? "sn_ledger_provider_pool" : "sn_ledger_pool";
}

bool NamesProviderPool(const std::vector<PoolInView>& pools) {
  bool ctf = false;
  std::vector<int64_t> operatorPools;
  for (const PoolInView& pool : pools) {
    if (pool.kind == PoolKind::Ctf) {
      ctf = true;
    } else if (std::find(operatorPools.begin(), operatorPools.end(), pool.noId) == operatorPools.end()) {
      operatorPools.push_back(pool.noId);
    }
  }
  return ctf && operatorPools.size() == 1;
}

bool ClaimsBothPools(const std::vector<PoolKind>& movable) {
  return movable.size() == 2 && std::count(movable.begin(), movable.end(), PoolKind::Ctf) == 1;
}

bool UnderTransferFloor(int64_t claimableRao, int64_t transferFloorRao) {
  return 0 < transferFloorRao && 0 < claimableRao && claimableRao < transferFloorRao;
}

LedgerSectionView LedgerSectionFor(bool hasColdkey, bool vault2Configured, LedgerFetch fetch,
                                   const std::vector<LedgerAccount>& accounts, bool canClaim,
                                   const std::vector<UnavailablePool>& unavailable,
                                   int64_t transferFloorRao) {
  LedgerSectionView view;
  if (!hasColdkey) return view;
  bool anyAccount = false;
  if (fetch == LedgerFetch::Ready || fetch == LedgerFetch::Loading) {
    for (const LedgerAccount& account : accounts) {
      LedgerAccountView block;
      block.noId = account.noId;
      block.kind = account.kind;
      block.claimableRao = account.claimableRao;
      block.paidRao = account.paidRao;
      block.earnedRao = account.finalizedRao;
      block.showPending = account.pending && account.pendingRao > account.finalizedRao;
      if (block.showPending) block.pendingRao = account.pendingRao - account.finalizedRao;
      view.accounts.push_back(block);
    }
    anyAccount = !view.accounts.empty();
    // each unavailable pool in its place by noId; a pool the read also listed
    // (the SDK refuses such an answer) shows its account
    for (const UnavailablePool& pool : unavailable) {
      auto same = [&pool](const LedgerAccountView& block) { return block.noId == pool.noId; };
      if (std::any_of(view.accounts.begin(), view.accounts.end(), same)) continue;
      LedgerAccountView block;
      block.noId = pool.noId;
      block.kind = pool.kind;
      block.unavailable = true;
      const auto at = std::find_if(
          view.accounts.begin(), view.accounts.end(),
          [&pool](const LedgerAccountView& other) { return other.noId > pool.noId; });
      view.accounts.insert(at, block);
    }
  }
  // before vault2 a ledger that is not served yet says nothing, so a read
  // that returned no account, whatever pools it could not read, shows nothing
  if (!vault2Configured && !anyAccount) return LedgerSectionView{};
  view.shown = true;
  view.loading = fetch == LedgerFetch::Loading;
  std::vector<PoolInView> pools;
  bool anyCtf = false;
  for (const LedgerAccountView& block : view.accounts) {
    pools.push_back(PoolInView{block.noId, block.kind});
    anyCtf = anyCtf || block.kind == PoolKind::Ctf;
  }
  view.poolTitles = view.accounts.size() > 1 || anyCtf;
  const bool providerPool = NamesProviderPool(pools);
  // every account says its own floor, a lone one as one beside others
  // (owner, 2026-11-11); a lone pool's block still names no pool
  for (LedgerAccountView& block : view.accounts) {
    if (view.poolTitles) block.titleKey = PoolTitleKey(block.kind, providerPool);
    block.underFloor = UnderTransferFloor(block.claimableRao, transferFloorRao);
  }
  if (fetch == LedgerFetch::Failed) view.note = LedgerNote::Unavailable;
  if (fetch == LedgerFetch::NoDevice) view.note = LedgerNote::NeedsSession;
  if (fetch == LedgerFetch::Ready && view.accounts.empty()) view.note = LedgerNote::NoAccount;
  // the accounts a claim moves now: something to claim above the floor (never
  // a sum of the ledger's amounts, which an absurd answer could overflow)
  std::vector<PoolKind> movable;
  for (const LedgerAccountView& block : view.accounts) {
    if (!block.unavailable && block.claimableRao > 0 && !block.underFloor) movable.push_back(block.kind);
  }
  view.showClaim = vault2Configured;
  view.claimEnabled = vault2Configured && canClaim && !movable.empty();
  view.claimBoth = vault2Configured && ClaimsBothPools(movable);
  return view;
}

std::string LedgerNoteKey(LedgerNote note) {
  switch (note) {
    case LedgerNote::Unavailable:
      return "sn_ledger_unavailable";
    case LedgerNote::NeedsSession:
      return "earnings_claims_need_session";
    case LedgerNote::NoAccount:
      return "no_claims_yet_linux";
    case LedgerNote::None:
      break;
  }
  return {};
}

std::string ExplorerTxLink(const std::string& explorerTxUrl, const std::string& txHash) {
  if (explorerTxUrl.empty() || txHash.empty()) return {};
  std::string url = explorerTxUrl;
  const size_t at = url.find("%s");
  if (at != std::string::npos) return url.replace(at, 2, txHash);
  if (url.back() != '/') url += '/';
  return url + txHash;
}

std::string ClaimFailureCode(const std::string& message) {
  std::string code = message.substr(0, message.find(':'));
  while (!code.empty() && (code.back() == ' ' || code.back() == '\t')) code.pop_back();
  return code;
}

std::string ClaimFailureKey(const std::string& message) {
  const std::string code = ClaimFailureCode(message);
  if (code == "needs_gas") return "add_tao_for_gas";
  if (code == "chain_rpc_unreachable" || code == "chain_rpc_error") return "chain_rpc_unreachable";
  if (code == "connect_wallet_first") return "connect_wallet_first";
  if (message.empty() || message == "claim_failed") return "claim_failed";
  return {};
}

LedgerClaimProgress::LedgerClaimProgress(const std::vector<LedgerAccount>& accounts,
                                         std::optional<double> gasTao,
                                         const std::vector<UnavailablePool>& unavailable,
                                         int64_t transferFloorRao)
    // A known-empty gas key cannot pay for anything; an unknown balance (the
    // chain did not answer) does not block the attempt, and the SDK's
    // preflight says needs_gas if it comes to that.
    : needsGas_(gasTao.has_value() && *gasTao <= 0.0) {
  // the pools of the page's read, as the tile has them: every account,
  // claimable or not, and each pool it could not read and did not list (the
  // SDK refuses an answer with a pool in both)
  std::vector<UnavailablePool> unlisted;
  for (const UnavailablePool& pool : unavailable) {
    auto same = [&pool](const LedgerAccount& account) { return account.noId == pool.noId; };
    if (!std::any_of(accounts.begin(), accounts.end(), same)) unlisted.push_back(pool);
  }
  for (const LedgerAccount& account : accounts) poolsInView_.push_back(PoolInView{account.noId, account.kind});
  for (const UnavailablePool& pool : unlisted) poolsInView_.push_back(PoolInView{pool.noId, pool.kind});
  // a row says its own floor, lone or beside others, as the tile's block does
  for (const LedgerAccount& account : accounts) {
    if (account.claimableRao <= 0) continue;
    LedgerClaimRow row;
    row.noId = account.noId;
    row.index = account.index;
    row.kind = account.kind;
    row.amountRao = account.claimableRao;
    row.underFloor = UnderTransferFloor(account.claimableRao, transferFloorRao);
    rows_.push_back(row);
  }
  // a pool the page could not read: its noId and index 0, as the claim
  // reports it (SnLedgerClaimCallback, ledger_unavailable)
  for (const UnavailablePool& pool : unlisted) {
    LedgerClaimRow row;
    row.noId = pool.noId;
    row.kind = pool.kind;
    row.unavailable = true;
    rows_.push_back(row);
  }
}

namespace {

// a row Claim moves: not confirmed, its pool read, above the floor, with an
// amount the dialog knows
bool Movable(const LedgerClaimRow& row) {
  return row.state != LedgerClaimState::Confirmed && !row.unavailable && !row.underFloor && row.amountRao > 0;
}

}  // namespace

int64_t LedgerClaimProgress::ClaimableRao() const {
  // every row's amount is at least 0: the constructor takes the claimable
  // accounts, Confirmed keeps a positive amount, and an unlisted row starts at 0
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  int64_t total = 0;
  for (const LedgerClaimRow& row : rows_) {
    if (!Movable(row)) continue;
    total = row.amountRao > kMax - total ? kMax : total + row.amountRao;
  }
  return total;
}

bool LedgerClaimProgress::TotalKnown() const {
  bool left = false;
  for (const LedgerClaimRow& row : rows_) {
    if (row.state == LedgerClaimState::Confirmed) continue;
    if (Movable(row)) return true;
    left = true;
  }
  return !left;
}

bool LedgerClaimProgress::CanClaim(bool allowActions) const {
  return allowActions && !sending_ && !needsGas_ && ClaimableRao() > 0;
}

bool LedgerClaimProgress::ClaimsBoth() const {
  std::vector<PoolKind> movable;
  for (const LedgerClaimRow& row : rows_) {
    if (Movable(row)) movable.push_back(row.kind);
  }
  return ClaimsBothPools(movable);
}

std::string LedgerClaimProgress::TitleKey(const LedgerClaimRow& row) const {
  std::vector<PoolInView> pools = poolsInView_;
  for (const LedgerClaimRow& other : rows_) pools.push_back(PoolInView{other.noId, other.kind});
  return PoolTitleKey(row.kind, NamesProviderPool(pools));
}

bool LedgerClaimProgress::Start() {
  if (sending_ || ClaimableRao() <= 0) return false;
  for (LedgerClaimRow& row : rows_) {
    // a row the SDK added has no amount to count, and is claimed again too
    if (row.state == LedgerClaimState::Confirmed || row.unavailable || row.underFloor) continue;
    row.state = LedgerClaimState::Sending;
    row.message.clear();
  }
  sending_ = true;
  failure_.clear();
  ledgerRead_ = false;
  reportedUnavailable_.clear();
  return true;
}

LedgerClaimRow& LedgerClaimProgress::RowFor(int64_t noId, int64_t index) {
  for (LedgerClaimRow& row : rows_) {
    if (row.noId == noId && row.index == index) return row;
  }
  // the account of a pool the page could not read: it takes that pool's row,
  // which held no account of its own
  for (LedgerClaimRow& row : rows_) {
    if (row.noId == noId && row.unavailable && row.amountRao == 0 &&
        row.state == LedgerClaimState::Idle) {
      row.index = index;
      return row;
    }
  }
  LedgerClaimRow row;
  row.noId = noId;
  row.index = index;
  row.kind = PoolKindOfNoId(noId);
  rows_.push_back(row);
  return rows_.back();
}

void LedgerClaimProgress::Sent(int64_t noId, int64_t index, const std::string& txHash) {
  ledgerRead_ = true;
  LedgerClaimRow& row = RowFor(noId, index);
  row.state = LedgerClaimState::Sent;
  row.txHash = txHash;
  row.unavailable = false;
  // the page's estimate of the floor was off: the chain takes it
  row.underFloor = false;
}

void LedgerClaimProgress::Confirmed(int64_t noId, int64_t index, const std::string& txHash,
                                    int64_t amountRao) {
  ledgerRead_ = true;
  LedgerClaimRow& row = RowFor(noId, index);
  row.state = LedgerClaimState::Confirmed;
  row.txHash = txHash;
  row.unavailable = false;
  row.underFloor = false;
  if (amountRao > 0) row.amountRao = amountRao;
}

void LedgerClaimProgress::Failed(int64_t noId, int64_t index, const std::string& message) {
  const std::string code = ClaimFailureCode(message);
  // a funding problem rather than an account's: the gas key's funding hint shows
  if (code == "needs_gas") needsGas_ = true;
  if (noId <= 0) {
    // nothing to claim in any pool is the claim's own read answering, which it
    // never says while a pool is unavailable
    if (code == "not_claimable") ledgerRead_ = true;
    failure_ = message;
    return;
  }
  ledgerRead_ = true;
  if (code == "ledger_unavailable") {
    // the claim's own read could not read the pool (index 0: its account is
    // unknown): each of its rows this claim has not sent is unavailable, never
    // failed, while one sent, claimed or failed keeps its state (Android's and
    // Linux's rule), and a pool with no row gets one
    reportedUnavailable_.push_back(noId);
    bool listed = false;
    for (LedgerClaimRow& row : rows_) {
      if (row.noId != noId) continue;
      listed = true;
      if (row.state != LedgerClaimState::Idle && row.state != LedgerClaimState::Sending) continue;
      row.state = LedgerClaimState::Idle;
      row.unavailable = true;
      row.message.clear();
    }
    if (!listed) RowFor(noId, index).unavailable = true;
    return;
  }
  LedgerClaimRow& row = RowFor(noId, index);
  row.unavailable = false;
  if (code == "already_claimed") {
    row.state = LedgerClaimState::Confirmed;
    return;
  }
  if (code == "needs_gas") {
    // the gas key could not pay for this account once the largest went: the
    // SDK sent nothing for it, so it is still claimable, with the funding hint
    row.state = LedgerClaimState::Idle;
    row.message.clear();
    return;
  }
  if (code == "not_claimable" && message.find("AmountTooLow") != std::string::npos) {
    // the claim's preflight: under the runtime's transfer floor, so nothing
    // was sent and it waits in vault2, Below the claim minimum
    row.state = LedgerClaimState::Idle;
    row.underFloor = true;
    row.message.clear();
    return;
  }
  row.state = LedgerClaimState::Failed;
  row.message = message;
}

void LedgerClaimProgress::Done() {
  sending_ = false;
  for (LedgerClaimRow& row : rows_) {
    if (row.state == LedgerClaimState::Sending) row.state = LedgerClaimState::Idle;
  }
  if (!ledgerRead_) return;
  // a pool the page could not read that the claim's own read read after all,
  // and said nothing of: it held nothing to claim, so its row goes
  std::erase_if(rows_, [this](const LedgerClaimRow& row) {
    return row.unavailable && row.state == LedgerClaimState::Idle && row.amountRao == 0 &&
           row.txHash.empty() &&
           std::find(reportedUnavailable_.begin(), reportedUnavailable_.end(), row.noId) ==
               reportedUnavailable_.end();
  });
}

namespace {

// Every pool of the relay's answer, its accounts' and the ones it could not
// read, which name its rows (Android's snClaimRelayPools).
std::vector<PoolInView> RelayPools(const std::vector<RelayAccount>& accounts,
                                   const std::vector<UnavailablePool>& unavailable) {
  std::vector<PoolInView> pools;
  for (const RelayAccount& account : accounts) pools.push_back(PoolInView{account.noId, account.kind});
  for (const UnavailablePool& pool : unavailable) pools.push_back(PoolInView{pool.noId, pool.kind});
  return pools;
}

// Each unavailable pool's row, after the rows, but for a pool that already
// has one (an answer listing a pool as both read and unavailable shows its
// account, as the SDK refuses such a ledger answer).
void AppendUnavailableRows(std::vector<RelayRow>& rows, std::vector<PoolKind>& kinds,
                           const std::vector<UnavailablePool>& unavailable) {
  for (const UnavailablePool& pool : unavailable) {
    auto same = [&pool](const RelayRow& row) { return row.noId == pool.noId; };
    if (std::any_of(rows.begin(), rows.end(), same)) continue;
    RelayRow row;
    row.noId = pool.noId;
    row.status = RelayRowStatus::Unavailable;
    rows.push_back(row);
    kinds.push_back(pool.kind);
  }
}

}  // namespace

std::vector<RelayRow> RelayRowsFor(const std::vector<RelayAccount>& accounts,
                                   const std::vector<UnavailablePool>& unavailable,
                                   int64_t transferFloorRao) {
  std::vector<RelayRow> rows;
  std::vector<PoolKind> kinds;  // each row's pool, for its title
  std::vector<std::string> coldkeys;
  for (const RelayAccount& account : accounts) {
    // URnetwork's claim is the account's latest and nothing is claimable since
    const bool claimed = !account.claimTxHash.empty() && account.claimableRao <= 0;
    if (account.claimableRao <= 0 && !claimed) continue;
    RelayRow row;
    row.noId = account.noId;
    row.coldkeySs58 = account.coldkeySs58;
    if (claimed) {
      row.status = RelayRowStatus::Claimed;
      row.amountRao = std::max<int64_t>(account.claimAmountRao, 0);
      row.txUrl = account.claimTxUrl;
    } else {
      row.status = UnderTransferFloor(account.claimableRao, transferFloorRao)
                       ? RelayRowStatus::UnderFloor
                       : RelayRowStatus::Pending;
      row.amountRao = account.claimableRao;
    }
    rows.push_back(row);
    kinds.push_back(account.kind);
    if (!account.coldkeySs58.empty() &&
        std::find(coldkeys.begin(), coldkeys.end(), account.coldkeySs58) == coldkeys.end()) {
      coldkeys.push_back(account.coldkeySs58);
    }
  }
  AppendUnavailableRows(rows, kinds, unavailable);
  const bool providerPool = NamesProviderPool(RelayPools(accounts, unavailable));
  for (size_t i = 0; i < rows.size(); ++i) {
    rows[i].titleKey = PoolTitleKey(kinds[i], providerPool);
    // a network paid to one coldkey needs no coldkey on its rows
    if (coldkeys.size() < 2) rows[i].coldkeySs58.clear();
  }
  return rows;
}

bool RelayTotalKnown(const std::vector<RelayAccount>& accounts, const std::string& coldkeySs58,
                     const std::vector<UnavailablePool>& unavailable) {
  return unavailable.empty() ||
         std::any_of(accounts.begin(), accounts.end(),
                     [&coldkeySs58](const RelayAccount& account) { return account.coldkeySs58 == coldkeySs58; });
}

RelayTileView RelayTileFor(const std::vector<RelayAccount>& accounts, const std::string& coldkeySs58,
                           const std::vector<UnavailablePool>& unavailable, int64_t transferFloorRao) {
  RelayTileView view;
  view.showTotal = RelayTotalKnown(accounts, coldkeySs58, unavailable);
  std::vector<RelayRow> rows;
  std::vector<PoolKind> kinds;
  for (const RelayAccount& account : accounts) {
    if (account.coldkeySs58 != coldkeySs58 || account.claimableRao <= 0) continue;
    RelayRow row;
    row.noId = account.noId;
    row.status = UnderTransferFloor(account.claimableRao, transferFloorRao) ? RelayRowStatus::UnderFloor
                                                                            : RelayRowStatus::Pending;
    row.amountRao = account.claimableRao;
    rows.push_back(row);
    kinds.push_back(account.kind);
  }
  // the coldkey's one operator pool says it all in the figure, as before
  // capture the flag; a lone account under the floor says so under it
  const bool names = rows.size() > 1 || std::find(kinds.begin(), kinds.end(), PoolKind::Ctf) != kinds.end();
  if (!names && unavailable.empty()) {
    view.underFloor = rows.size() == 1 && rows[0].status == RelayRowStatus::UnderFloor;
    return view;
  }
  AppendUnavailableRows(rows, kinds, unavailable);
  const bool providerPool = NamesProviderPool(RelayPools(accounts, unavailable));
  for (size_t i = 0; i < rows.size(); ++i) rows[i].titleKey = PoolTitleKey(kinds[i], providerPool);
  view.rows = std::move(rows);
  return view;
}

std::string CtfWinKey(const CtfWin& win) {
  return win.role == CtfRole::Extender ? "sn_ctf_won_extender_slot" : "sn_ctf_won_provider_slot";
}

LedgerClaimEvents WithClaimPaid(LedgerClaimEvents events, std::function<void(bool paid)> afterDone) {
  // set on the SDK's thread, read at Done, which the SDK calls last
  auto paid = std::make_shared<std::atomic<bool>>(false);
  LedgerClaimEvents out;
  out.sent = std::move(events.sent);
  out.confirmed = [confirmed = std::move(events.confirmed), paid](int64_t noId, int64_t index,
                                                                   std::string txHash, int64_t amountRao) {
    paid->store(true);
    if (confirmed) confirmed(noId, index, std::move(txHash), amountRao);
  };
  out.failed = [failed = std::move(events.failed), paid](int64_t noId, int64_t index,
                                                         std::string message) {
    // already_claimed is an account's success: vault2 paid it to the same coldkey
    if (noId > 0 && ClaimFailureCode(message) == "already_claimed") paid->store(true);
    if (failed) failed(noId, index, std::move(message));
  };
  out.done = [done = std::move(events.done), paid, afterDone = std::move(afterDone)] {
    if (done) done();
    if (afterDone) afterDone(paid->load());
  };
  return out;
}

std::optional<std::chrono::milliseconds> CtfWinsReadsAfterClaim::Done(bool paid) {
  if (!paid) return std::nullopt;
  next_ = 0;
  return kCtfWinsReadsAfterClaim[0];
}

CtfWinsReadsAfterClaim::Step CtfWinsReadsAfterClaim::Tick() {
  Step step;
  if (next_ >= kCtfWinsReadsAfterClaim.size()) return step;
  step.read = true;
  const std::size_t at = next_++;
  if (next_ < kCtfWinsReadsAfterClaim.size()) {
    step.next = kCtfWinsReadsAfterClaim[next_] - kCtfWinsReadsAfterClaim[at];
  }
  return step;
}

void CtfWinsReadsAfterClaim::Stop() { next_ = kCtfWinsReadsAfterClaim.size(); }

}  // namespace urnw::snpayout
