// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "EarningsSheets.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "Localization.h"
#include "Log.h"
#include "PageContext.h"
#include "Sdk.h"
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace urnw::pages;

namespace urnw {
namespace {

// The gas a claim needs is a fraction of a cent of TAO on the subtensor EVM.
// The SDK reports "needs gas" rather than an estimate, so the funding hint
// names a round amount that covers many claims.
constexpr double kSuggestedGasTao = 0.01;

TextBlock MakeText(hstring const& text, double fontSize, Brush const& brush = nullptr,
                   bool wrap = false) {
  TextBlock tb;
  tb.Text(text);
  tb.FontSize(fontSize);
  if (brush) tb.Foreground(brush);
  if (wrap) tb.TextWrapping(TextWrapping::Wrap);
  return tb;
}

// UrLabel: the 12sp muted caption above a value.
TextBlock MakeLabel(hstring const& text) {
  return MakeText(text, 12, colors::MutedBrush());
}

// The ABC Gravity Extra Condensed figure a value is shown in throughout this
// destination (iOS titleCondensedFont).
TextBlock MakeValue(hstring const& text, double fontSize = 22,
                    Brush const& brush = nullptr) {
  TextBlock tb = MakeText(text, fontSize, brush ? brush : colors::TextBrush());
  tb.FontFamily(FontFamily(L"ms-appx:///Assets/Fonts/abcgravity_extra_condensed.otf#ABC "
                           L"Gravity Extra Condensed"));
  return tb;
}

ContentDialog MakeDialog(XamlRoot const& root, hstring const& title) {
  ContentDialog dialog;
  dialog.XamlRoot(root);
  if (!title.empty()) dialog.Title(winrt::box_value(title));
  dialog.CloseButtonText(Loc("close"));
  dialog.Background(colors::SheetBrush());
  return dialog;
}

Border HairLine() {
  Border line;
  line.Height(1);
  line.Background(colors::BorderBrush());
  line.Margin(Thickness{0, 8, 0, 8});
  return line;
}

ColumnDefinition StarColumn(double weight = 1) {
  ColumnDefinition col;
  col.Width(GridLengthHelper::FromValueAndType(weight, GridUnitType::Star));
  return col;
}

ColumnDefinition AutoColumn() {
  ColumnDefinition col;
  col.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Auto));
  return col;
}

// A failure message from the SDK is a short English reason ("needs gas",
// "expired", "rpc unreachable"); the store has a sentence for each of the ones
// a user can act on. Anything else is shown as it came.
bool MentionsGas(std::string const& message) {
  std::string lower = message;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find("gas") != std::string::npos || lower.find("funds") != std::string::npos;
}

bool MentionsExpiry(std::string const& message) {
  std::string lower = message;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find("expire") != std::string::npos;
}

bool MentionsRpc(std::string const& message) {
  std::string lower = message;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.find("rpc") != std::string::npos ||
         lower.find("unreachable") != std::string::npos ||
         lower.find("timeout") != std::string::npos;
}

// SnClaimCallback.Failed messages start with one of the SDK's stable codes
// (SnErrorCode*) followed by ": detail".
std::string FailureCode(std::string const& message) {
  const size_t colon = message.find(':');
  std::string code = colon == std::string::npos ? message : message.substr(0, colon);
  while (!code.empty() && std::isspace(static_cast<unsigned char>(code.back()))) code.pop_back();
  return code;
}

hstring FailureText(int64_t epoch, std::string const& message) {
  const std::string code = FailureCode(message);
  if (code == "claims_for_epoch_expired" || MentionsExpiry(message)) {
    return hstring{urnw::Format("claims_for_epoch_expired", epoch)};
  }
  if (code == "chain_rpc_unreachable" || code == "chain_rpc_error" || MentionsRpc(message)) {
    return Loc("chain_rpc_unreachable");
  }
  if (code == "needs_gas" || MentionsGas(message)) return Loc("add_tao_for_gas");
  if (code == "already_claimed") return Loc("claim_confirmed");
  if (code == "connect_wallet_first") return Loc("connect_wallet_first");
  if (message.empty() || (code == "claim_failed" && code == message)) return Loc("claim_failed");
  return H(message);
}

}  // namespace

// ---- shared presentation -----------------------------------------------------

std::wstring FormatPointsValue(double points) {
  wchar_t buf[64];
  // whole points read as counts, fractions keep two places (iOS grouping)
  if (std::fabs(points - std::llround(points)) < 0.005) {
    std::swprintf(buf, std::size(buf), L"%lld", static_cast<long long>(std::llround(points)));
  } else {
    std::swprintf(buf, std::size(buf), L"%.2f", points);
  }
  // thousands separators, inserted by hand: std::format's 'L' needs a locale
  // this process does not set, and the value is a bare number either way
  std::wstring s = buf;
  const size_t end = s.find(L'.') == std::wstring::npos ? s.size() : s.find(L'.');
  const size_t start = (!s.empty() && s[0] == L'-') ? 1 : 0;
  for (size_t i = end; i > start + 3;) {
    i -= 3;
    s.insert(i, L",");
  }
  return s;
}

std::wstring ShortDate(std::string const& timestamp) {
  // "2026-08-06T12:00:00Z" and "2026-08-06 12:00:00" both start with the date
  if (timestamp.size() >= 10 && timestamp[4] == '-' && timestamp[7] == '-') {
    return urnw::Widen(timestamp.substr(0, 10));
  }
  return urnw::Widen(timestamp);
}

std::wstring DateFromMillis(int64_t millis) {
  if (millis <= 0) return L"-";
  // civil-from-days (Howard Hinnant), so no locale and no time zone: an epoch
  // boundary is a UTC instant and the table is read by comparison
  int64_t days = millis / 86'400'000;
  if (millis < 0 && millis % 86'400'000 != 0) --days;
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t d = doy - (153 * mp + 2) / 5 + 1;
  const int64_t m = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = y + (m <= 2 ? 1 : 0);
  wchar_t buf[32];
  std::swprintf(buf, std::size(buf), L"%04lld-%02lld-%02lld", static_cast<long long>(year),
                static_cast<long long>(m), static_cast<long long>(d));
  return buf;
}

std::wstring ShortAddress(std::string const& address) {
  return urnw::Widen(urnet::shortSs58(address));  // "5F3s...kQ9v"
}

std::wstring FormatAlphaRao(int64_t rao) {
  // the SDK's own rendering ("3.2410 SN25a" with the alpha symbol), so every
  // platform shows the same figure for the same rao
  return urnw::Widen(urnet::formatAlpha(rao));
}

std::wstring FormatTao(double tao) {
  wchar_t buf[64];
  std::swprintf(buf, std::size(buf), L"%.4f", tao);
  return buf;
}

std::wstring FormatShareBpsValue(int64_t shareBps) {
  return urnw::Widen(urnet::formatShareBps(shareBps));
}

hstring ClaimStatusText(std::string const& status) {
  if (status == "claimable") return Loc("sn_status_claimable");
  if (status == "claimed") return Loc("claim_confirmed");
  if (status == "expired") return Loc("claim_expired");
  if (status == "open") return Loc("sn_status_open");
  if (status == "not-finalized") return Loc("sn_status_not_finalized");
  return H(status);
}

// ---- ClaimAlphaSheet ---------------------------------------------------------

std::shared_ptr<ClaimAlphaSheet> ClaimAlphaSheet::Create(
    XamlRoot const& root, winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
    std::vector<EpochClaim> claims, GasKeyInfo gas, std::string explorerTxUrl,
    bool allowActions, Claimer claimer, std::function<void()> onClaimed) {
  auto sheet = std::shared_ptr<ClaimAlphaSheet>(
      new ClaimAlphaSheet(queue, std::move(claims), std::move(gas), std::move(explorerTxUrl),
                          allowActions, std::move(claimer), std::move(onClaimed)));
  sheet->Build(root);
  return sheet;
}

ClaimAlphaSheet::ClaimAlphaSheet(winrt::Microsoft::UI::Dispatching::DispatcherQueue const& queue,
                                 std::vector<EpochClaim> claims, GasKeyInfo gas,
                                 std::string explorerTxUrl, bool allowActions, Claimer claimer,
                                 std::function<void()> onClaimed)
    : queue_(queue),
      gas_(std::move(gas)),
      explorerTxUrl_(std::move(explorerTxUrl)),
      allowActions_(allowActions),
      claimer_(std::move(claimer)),
      onClaimed_(std::move(onClaimed)) {
  // newest epoch first, as the history table orders them
  std::sort(claims.begin(), claims.end(),
            [](EpochClaim const& a, EpochClaim const& b) { return a.epoch > b.epoch; });
  for (auto& claim : claims) {
    Row row;
    row.claim = std::move(claim);
    rows_.push_back(std::move(row));
  }
  needsGas_ = NeedsGas();
}

int64_t ClaimAlphaSheet::TotalClaimableRao() const {
  int64_t total = 0;
  for (auto const& row : rows_) {
    if (row.claim.status == "claimable" && row.state != Row::State::Confirmed) {
      total += row.claim.amountRao;
    }
  }
  return total;
}

size_t ClaimAlphaSheet::ClaimableCount() const {
  size_t count = 0;
  for (auto const& row : rows_) {
    if (row.claim.status == "claimable" && row.state != Row::State::Confirmed) ++count;
  }
  return count;
}

bool ClaimAlphaSheet::NeedsGas() const {
  // A known-empty gas key cannot pay for anything; an unknown balance (the
  // chain did not answer) does not block the attempt, the SDK reports the
  // real reason if there is one.
  return gas_.tao.has_value() && *gas_.tao <= 0.0;
}

void ClaimAlphaSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, Loc("claim_alpha_title"));

  StackPanel content;
  content.Spacing(6);
  content.MinWidth(460);

  // the figure: what is claimable now, over how many epochs
  totalValue_ = MakeValue(hstring{FormatAlphaRao(TotalClaimableRao())}, 36);
  content.Children().Append(totalValue_);
  acrossText_ = MakeText(
      hstring{urnw::Format("claim_across_epochs", static_cast<int64_t>(ClaimableCount()))}, 12,
      colors::MutedBrush(), true);
  content.Children().Append(acrossText_);

  // what a claim is, and where it runs: on this device, against the vault
  content.Children().Append(
      MakeText(Loc("claim_sends_from_device"), 12, colors::MutedBrush(), true));
  content.Children().Append(
      MakeText(Loc("claims_open_after_finalization"), 12, colors::MutedBrush(), true));

  content.Children().Append(HairLine());

  // the gas key: its address, its balance, and the funding hint when it is empty
  content.Children().Append(MakeLabel(Loc("gas_key")));
  auto gasAddress = MakeText(hstring{urnw::Widen(gas_.address.empty() ? std::string("-")
                                                                        : gas_.address)},
                             13, colors::TextBrush(), true);
  gasAddress.IsTextSelectionEnabled(true);
  content.Children().Append(gasAddress);

  Grid balanceRow;
  balanceRow.ColumnSpacing(12);
  balanceRow.ColumnDefinitions().Append(StarColumn());
  balanceRow.ColumnDefinitions().Append(AutoColumn());
  auto balanceLabel = MakeText(Loc("earnings_gas_balance"), 13, colors::MutedBrush());
  Grid::SetColumn(balanceLabel, 0);
  balanceRow.Children().Append(balanceLabel);
  gasBalanceValue_ = MakeText(hstring{}, 13, colors::TextBrush());
  Grid::SetColumn(gasBalanceValue_, 1);
  balanceRow.Children().Append(gasBalanceValue_);
  content.Children().Append(balanceRow);

  needsGasPanel_ = StackPanel();
  needsGasPanel_.Spacing(6);
  needsGasPanel_.Margin(Thickness{0, 6, 0, 0});
  needsGasPanel_.Children().Append(MakeText(Loc("add_tao_for_gas"), 15, colors::TextBrush()));
  needsGasPanel_.Children().Append(
      MakeText(hstring{urnw::Format("send_tao_to_mirror", FormatTao(kSuggestedGasTao),
                                    urnw::Widen(gas_.mirrorSs58))},
               12, colors::MutedBrush(), true));
  auto mirror = MakeText(hstring{urnw::Widen(gas_.mirrorSs58)}, 13, colors::TextBrush(), true);
  mirror.IsTextSelectionEnabled(true);
  needsGasPanel_.Children().Append(mirror);
  Button copy;
  copy.Content(LocBox("earnings_copy_address"));
  copy.HorizontalAlignment(HorizontalAlignment::Left);
  copy.IsEnabled(!gas_.mirrorSs58.empty());
  copy.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->CopyMirrorAddress();
  });
  needsGasPanel_.Children().Append(copy);
  content.Children().Append(needsGasPanel_);

  content.Children().Append(HairLine());

  // the epochs, one row each, moved along by the claim events
  rowsPanel_ = StackPanel();
  content.Children().Append(rowsPanel_);

  // the progress line while the SDK sends and waits for receipts
  progressRow_ = StackPanel();
  progressRow_.Orientation(Orientation::Horizontal);
  progressRow_.Spacing(8);
  progressRow_.Margin(Thickness{0, 8, 0, 0});
  ProgressRing ring;
  ring.Width(16);
  ring.Height(16);
  ring.IsActive(true);
  progressRow_.Children().Append(ring);
  progressRow_.Children().Append(MakeText(Loc("earnings_claim_sending"), 13, colors::MutedBrush()));
  progressRow_.Visibility(Visibility::Collapsed);
  content.Children().Append(progressRow_);

  // A refusal renders HERE, on the sheet, not on the page behind it.
  errorText_ = MakeText(hstring{}, 12, colors::DangerBrush(), true);
  errorText_.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText_);

  claimButton_ = Button();
  claimButton_.HorizontalAlignment(HorizontalAlignment::Stretch);
  claimButton_.Margin(Thickness{0, 8, 0, 0});
  claimButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->StartClaim();
  });
  content.Children().Append(claimButton_);

  ScrollViewer scroll;
  scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
  scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
  scroll.MaxHeight(560);
  scroll.Content(content);
  dialog_.Content(scroll);

  ApplyState();
}

UIElement ClaimAlphaSheet::BuildRow(Row const& row) {
  Grid grid;
  grid.ColumnSpacing(12);
  grid.MinHeight(32);
  grid.ColumnDefinitions().Append(StarColumn(2));
  grid.ColumnDefinitions().Append(StarColumn(3));
  grid.ColumnDefinitions().Append(AutoColumn());

  auto epoch = MakeText(hstring{urnw::Format("epoch_row_title", row.claim.epoch)}, 13,
                        colors::TextBrush());
  epoch.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(epoch, 0);
  grid.Children().Append(epoch);

  auto amount = MakeText(hstring{FormatAlphaRao(row.claim.amountRao)}, 13, colors::TextBrush());
  amount.VerticalAlignment(VerticalAlignment::Center);
  Grid::SetColumn(amount, 1);
  grid.Children().Append(amount);

  // the state cell: a word, coloured by what it means, and the transaction
  // link once there is a hash to link
  StackPanel state;
  state.Orientation(Orientation::Horizontal);
  state.Spacing(8);
  state.VerticalAlignment(VerticalAlignment::Center);
  hstring word;
  Brush brush = colors::MutedBrush();
  std::string txHash = row.claim.txHash;
  switch (row.state) {
    case Row::State::Sending:
      word = Loc("earnings_claim_sending");
      break;
    case Row::State::Sent:
      word = Loc("claim_sent");
      brush = colors::TextBrush();
      break;
    case Row::State::Confirmed:
      word = Loc("claim_confirmed");
      brush = colors::MakeBrush(colors::kUrGreen);
      break;
    case Row::State::Failed:
      word = Loc("claim_failed");
      brush = colors::DangerBrush();
      break;
    case Row::State::Idle:
      word = ClaimStatusText(row.claim.status);
      if (row.claim.status == "claimable") brush = colors::MakeBrush(colors::kUrGreen);
      else if (row.claim.status == "claimed") brush = colors::TextBrush();
      else if (row.claim.status == "expired") brush = colors::DangerBrush();
      break;
  }
  state.Children().Append(MakeText(word, 13, brush));
  if (!txHash.empty() && !explorerTxUrl_.empty()) {
    HyperlinkButton link;
    link.Content(LocBox("earnings_view_transaction"));
    link.Padding(Thickness{0, 0, 0, 0});
    link.Click([weak = weak_from_this(), txHash](auto const&, auto const&) {
      if (auto self = weak.lock()) self->OpenTx(txHash);
    });
    state.Children().Append(link);
  }
  Grid::SetColumn(state, 2);
  grid.Children().Append(state);

  StackPanel cell;
  cell.Children().Append(grid);
  if (row.state == Row::State::Failed) {
    cell.Children().Append(
        MakeText(FailureText(row.claim.epoch, row.message), 12, colors::DangerBrush(), true));
  } else if (row.state == Row::State::Idle && !row.claim.message.empty() &&
             row.claim.status != "claimable" && row.claim.status != "claimed") {
    // the SDK's reason an epoch cannot be claimed (not finalized, expired...)
    cell.Children().Append(MakeText(H(row.claim.message), 12, colors::MutedBrush(), true));
  }
  return cell;
}

void ClaimAlphaSheet::ApplyState() {
  totalValue_.Text(hstring{FormatAlphaRao(TotalClaimableRao())});
  acrossText_.Text(
      hstring{urnw::Format("claim_across_epochs", static_cast<int64_t>(ClaimableCount()))});
  gasBalanceValue_.Text(gas_.tao ? hstring{FormatTao(*gas_.tao) + L" TAO"} : hstring{L"-"});
  needsGasPanel_.Visibility(needsGas_ ? Visibility::Visible : Visibility::Collapsed);

  rowsPanel_.Children().Clear();
  for (auto const& row : rows_) rowsPanel_.Children().Append(BuildRow(row));

  progressRow_.Visibility(sending_ ? Visibility::Visible : Visibility::Collapsed);

  const int64_t total = TotalClaimableRao();
  claimButton_.Content(winrt::box_value(
      hstring{urnw::Format("claim_amount_button", FormatAlphaRao(total))}));
  claimButton_.IsEnabled(allowActions_ && total > 0 && !needsGas_ && !sending_);
  // Once every claimable epoch confirmed the button has nothing left to do and
  // says so by its figure ("Claim 0.0000"), which is disabled above.
}

void ClaimAlphaSheet::StartClaim() {
  if (sending_ || !claimer_) return;
  if (!allowActions_) {
    // Disabled without a session, so reaching this is a bug rather than a
    // user path: say so instead of returning.
    urnw::LogError("claim sheet: refusing a claim - the sheet was opened with no device session");
    errorText_.Text(Loc("please_login_to_urnetwork"));
    errorText_.Visibility(Visibility::Visible);
    return;
  }
  std::vector<int64_t> epochs;
  for (auto& row : rows_) {
    if (row.claim.status == "claimable" && row.state != Row::State::Confirmed) {
      row.state = Row::State::Sending;
      row.message.clear();
      epochs.push_back(row.claim.epoch);
    }
  }
  if (epochs.empty()) return;
  sending_ = true;
  errorText_.Visibility(Visibility::Collapsed);
  ApplyState();

  // The events arrive on the SDK's thread; each is marshalled here and applied
  // through a weak reference, so a dismissed sheet drops them on the floor and
  // the claim itself carries on regardless (it is the chain's, not the
  // dialog's).
  auto weak = weak_from_this();
  auto queue = queue_;
  auto find = [](std::vector<Row>& rows, int64_t epoch) -> Row* {
    for (auto& row : rows) {
      if (row.claim.epoch == epoch) return &row;
    }
    return nullptr;
  };
  ClaimEvents events;
  events.sent = [weak, queue, find](int64_t epoch, std::string txHash) {
    queue.TryEnqueue([weak, find, epoch, txHash] {
      auto self = weak.lock();
      if (!self) return;
      if (auto* row = find(self->rows_, epoch)) {
        row->state = Row::State::Sent;
        row->claim.txHash = txHash;
      }
      self->ApplyState();
    });
  };
  events.confirmed = [weak, queue, find](int64_t epoch, std::string txHash, int64_t amountRao) {
    queue.TryEnqueue([weak, find, epoch, txHash, amountRao] {
      auto self = weak.lock();
      if (!self) return;
      if (auto* row = find(self->rows_, epoch)) {
        row->state = Row::State::Confirmed;
        row->claim.status = "claimed";
        row->claim.txHash = txHash;
        if (amountRao > 0) row->claim.amountRao = amountRao;
      }
      self->anyConfirmed_ = true;
      self->ApplyState();
    });
  };
  events.failed = [weak, queue, find](int64_t epoch, std::string message) {
    urnw::LogError("claim sheet: epoch {} failed: {}", epoch, message);
    queue.TryEnqueue([weak, find, epoch, message] {
      auto self = weak.lock();
      if (!self) return;
      if (auto* row = find(self->rows_, epoch)) {
        row->state = Row::State::Failed;
        row->message = message;
        if (MentionsExpiry(message)) row->claim.status = "expired";
      }
      // "needs gas" is a funding problem, not an epoch's: surface the mirror
      // address above the rows as well as the reason on the row
      if (MentionsGas(message)) self->needsGas_ = true;
      self->ApplyState();
    });
  };
  events.done = [weak, queue] {
    queue.TryEnqueue([weak] {
      auto self = weak.lock();
      if (!self) return;
      self->sending_ = false;
      // a row the SDK never reported on is not still sending
      for (auto& row : self->rows_) {
        if (row.state == Row::State::Sending) row.state = Row::State::Idle;
      }
      self->ApplyState();
      if (self->anyConfirmed_ && self->onClaimed_) self->onClaimed_();
    });
  };
  claimer_(std::move(epochs), std::move(events));
}

void ClaimAlphaSheet::CopyMirrorAddress() {
  namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
  if (gas_.mirrorSs58.empty()) return;
  try {
    dt::DataPackage package;
    package.SetText(H(gas_.mirrorSs58));
    dt::Clipboard::SetContent(package);
  } catch (...) {
    urnw::LogWarn("claim sheet: the clipboard refused the mirror address");
  }
}

void ClaimAlphaSheet::OpenTx(std::string const& txHash) {
  if (txHash.empty() || explorerTxUrl_.empty()) return;
  std::string url = explorerTxUrl_;
  const size_t at = url.find("%s");
  if (at != std::string::npos) {
    url.replace(at, 2, txHash);
  } else {
    if (!url.empty() && url.back() != '/') url += '/';
    url += txHash;
  }
  try {
    winrt::Windows::System::Launcher::LaunchUriAsync(Uri(H(url)));
  } catch (...) {
    urnw::LogWarn("claim sheet: could not open {}", url);
  }
}

}  // namespace urnw
