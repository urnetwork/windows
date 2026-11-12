// SPDX-License-Identifier: MPL-2.0
#include "SnPayoutPresentation.h"

#include <algorithm>
#include <format>
#include <limits>

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

std::string PoolTitleKey(PoolKind kind, bool besideCtf) {
  if (kind == PoolKind::Ctf) return "sn_ctf_pool";
  return besideCtf ? "sn_ledger_provider_pool" : "sn_ledger_pool";
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
  const bool besideCtf =
      std::any_of(view.accounts.begin(), view.accounts.end(),
                  [](const LedgerAccountView& block) { return block.kind == PoolKind::Ctf; });
  view.poolTitles = view.accounts.size() > 1 || besideCtf;
  // blocks that name their pools say their own status; a lone account's floor
  // is the claim relay's note under it, as before capture the flag
  if (view.poolTitles) {
    for (LedgerAccountView& block : view.accounts) {
      block.titleKey = PoolTitleKey(block.kind, besideCtf);
      block.underFloor = UnderTransferFloor(block.claimableRao, transferFloorRao);
    }
  }
  if (fetch == LedgerFetch::Failed) view.note = LedgerNote::Unavailable;
  if (fetch == LedgerFetch::NoDevice) view.note = LedgerNote::NeedsSession;
  if (fetch == LedgerFetch::Ready && view.accounts.empty()) view.note = LedgerNote::NoAccount;
  // any account with something to claim, never a sum of the ledger's amounts,
  // which an absurd answer could overflow
  const bool anyClaimable = std::any_of(
      view.accounts.begin(), view.accounts.end(),
      [](const LedgerAccountView& account) { return account.claimableRao > 0; });
  view.showClaim = vault2Configured;
  view.claimEnabled = vault2Configured && canClaim && anyClaimable;
  // Claim both: an account of each pool that the claim can move now
  auto claimableIn = [&view](PoolKind kind) {
    return std::any_of(view.accounts.begin(), view.accounts.end(),
                       [kind](const LedgerAccountView& account) {
                         return account.kind == kind && account.claimableRao > 0 &&
                                !account.underFloor;
                       });
  };
  view.claimBoth = vault2Configured && claimableIn(PoolKind::Pool) && claimableIn(PoolKind::Ctf);
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
  for (const LedgerAccount& account : accounts) besideCtf_ = besideCtf_ || account.kind == PoolKind::Ctf;
  for (const UnavailablePool& pool : unlisted) besideCtf_ = besideCtf_ || pool.kind == PoolKind::Ctf;
  // a row says its own floor where the tile's blocks do: beside another pool
  const bool namesPools = accounts.size() + unlisted.size() > 1 || besideCtf_;
  for (const LedgerAccount& account : accounts) {
    if (account.claimableRao <= 0) continue;
    LedgerClaimRow row;
    row.noId = account.noId;
    row.index = account.index;
    row.kind = account.kind;
    row.amountRao = account.claimableRao;
    row.underFloor = namesPools && UnderTransferFloor(account.claimableRao, transferFloorRao);
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

int64_t LedgerClaimProgress::ClaimableRao() const {
  // every row's amount is at least 0: the constructor takes the claimable
  // accounts, Confirmed keeps a positive amount, and an unlisted row starts at 0
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  int64_t total = 0;
  for (const LedgerClaimRow& row : rows_) {
    if (row.state == LedgerClaimState::Confirmed || row.unavailable) continue;
    total = row.amountRao > kMax - total ? kMax : total + row.amountRao;
  }
  return total;
}

bool LedgerClaimProgress::CanClaim(bool allowActions) const {
  return allowActions && !sending_ && !needsGas_ && ClaimableRao() > 0;
}

bool LedgerClaimProgress::ClaimsBoth() const {
  auto claimableIn = [this](PoolKind kind) {
    return std::any_of(rows_.begin(), rows_.end(), [kind](const LedgerClaimRow& row) {
      return row.kind == kind && row.state != LedgerClaimState::Confirmed && !row.unavailable &&
             !row.underFloor && row.amountRao > 0;
    });
  };
  return claimableIn(PoolKind::Pool) && claimableIn(PoolKind::Ctf);
}

std::string LedgerClaimProgress::TitleKey(const LedgerClaimRow& row) const {
  const bool besideCtf =
      besideCtf_ || std::any_of(rows_.begin(), rows_.end(), [](const LedgerClaimRow& other) {
        return other.kind == PoolKind::Ctf;
      });
  return PoolTitleKey(row.kind, besideCtf);
}

bool LedgerClaimProgress::Start() {
  if (sending_ || ClaimableRao() <= 0) return false;
  for (LedgerClaimRow& row : rows_) {
    if (row.state == LedgerClaimState::Confirmed || row.unavailable) continue;
    row.state = LedgerClaimState::Sending;
    row.message.clear();
  }
  sending_ = true;
  failure_.clear();
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
  LedgerClaimRow& row = RowFor(noId, index);
  row.state = LedgerClaimState::Sent;
  row.txHash = txHash;
  row.unavailable = false;
}

void LedgerClaimProgress::Confirmed(int64_t noId, int64_t index, const std::string& txHash,
                                    int64_t amountRao) {
  LedgerClaimRow& row = RowFor(noId, index);
  row.state = LedgerClaimState::Confirmed;
  row.txHash = txHash;
  row.unavailable = false;
  if (amountRao > 0) row.amountRao = amountRao;
}

void LedgerClaimProgress::Failed(int64_t noId, int64_t index, const std::string& message) {
  const std::string code = ClaimFailureCode(message);
  // a funding problem rather than an account's: the gas key's funding hint shows
  if (code == "needs_gas") needsGas_ = true;
  if (noId <= 0) {
    failure_ = message;
    return;
  }
  if (code == "ledger_unavailable") {
    // the claim's own read could not read the pool (index 0: its account is
    // unknown): each of its rows not confirmed is unavailable, never failed,
    // and a pool with no row gets one
    bool listed = false;
    for (LedgerClaimRow& row : rows_) {
      if (row.noId != noId) continue;
      listed = true;
      if (row.state == LedgerClaimState::Confirmed) continue;
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
  row.state = LedgerClaimState::Failed;
  row.message = message;
}

void LedgerClaimProgress::Done() {
  sending_ = false;
  for (LedgerClaimRow& row : rows_) {
    if (row.state == LedgerClaimState::Sending) row.state = LedgerClaimState::Idle;
  }
}

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
  for (const UnavailablePool& pool : unavailable) {
    auto same = [&pool](const RelayRow& row) { return row.noId == pool.noId; };
    if (std::any_of(rows.begin(), rows.end(), same)) continue;
    RelayRow row;
    row.noId = pool.noId;
    row.status = RelayRowStatus::Unavailable;
    rows.push_back(row);
    kinds.push_back(pool.kind);
  }
  // capture the flag's account, or a pool the status could not read, makes the
  // rows; the provider pool's accounts alone are the sheet's figure and its
  // notes, as before capture the flag
  const bool besideCtf = std::find(kinds.begin(), kinds.end(), PoolKind::Ctf) != kinds.end();
  const bool anyUnavailable = std::any_of(rows.begin(), rows.end(), [](const RelayRow& row) {
    return row.status == RelayRowStatus::Unavailable;
  });
  if (!besideCtf && !anyUnavailable) return {};
  for (size_t i = 0; i < rows.size(); ++i) {
    rows[i].titleKey = PoolTitleKey(kinds[i], besideCtf);
    // a network paid to one coldkey needs no coldkey on its rows
    if (coldkeys.size() < 2) rows[i].coldkeySs58.clear();
  }
  return rows;
}

std::string CtfWinKey(const CtfWin& win) {
  return win.role == CtfRole::Extender ? "sn_ctf_won_extender_slot" : "sn_ctf_won_provider_slot";
}

}  // namespace urnw::snpayout
