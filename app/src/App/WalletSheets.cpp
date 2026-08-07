// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "WalletSheets.h"

#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "Localization.h"
#include "Log.h"
#include "PageContext.h"
#include "StatsFormat.h"
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Documents;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace urnw::pages;

namespace urnw {
namespace {

using ShapeEllipse = winrt::Microsoft::UI::Xaml::Shapes::Ellipse;

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

LinearGradientBrush ChainGradient(std::string const& blockchain) {
  LinearGradientBrush brush;
  brush.StartPoint(Point{0.5f, 0.0f});
  brush.EndPoint(Point{0.5f, 1.0f});
  // iOS WalletIcon.swift
  winrt::Windows::UI::Color from{255, 0x8A, 0x46, 0xFF};  // polygon
  winrt::Windows::UI::Color to{255, 0x6E, 0x38, 0xCC};
  if (blockchain == urnet::SOL) {
    from = {255, 0x99, 0x45, 0xFF};
    to = {255, 0x14, 0xF1, 0x95};
  } else if (blockchain == urnet::TAO) {
    from = {255, 0x1C, 0x1C, 0x1C};
    to = {255, 0x3A, 0x3A, 0x3A};
  }
  GradientStop a;
  a.Color(from);
  a.Offset(0.0);
  GradientStop b;
  b.Color(to);
  b.Offset(1.0);
  brush.GradientStops().Append(a);
  brush.GradientStops().Append(b);
  return brush;
}

// One "label over value" cell, the unit every stat row on this destination is
// built from.
StackPanel StatCell(hstring const& label, hstring const& value, double fontSize = 22,
                    Brush const& valueBrush = nullptr) {
  StackPanel cell;
  cell.Children().Append(MakeLabel(label));
  cell.Children().Append(MakeValue(value, fontSize, valueBrush));
  return cell;
}

}  // namespace

// ---- shared presentation -----------------------------------------------------

std::wstring ChainDisplayName(std::string const& blockchain) {
  if (blockchain == urnet::SOL) return urnw::Localized("solana");
  if (blockchain == urnet::TAO) return urnw::Localized("bittensor");
  if (blockchain == urnet::MATIC) return urnw::Localized("polygon");
  return urnw::Widen(blockchain);  // an unknown chain: its raw id
}

std::wstring MaskAddress(std::string const& address) {
  if (address.empty()) return L"";
  const size_t take = (std::min)(address.size(), static_cast<size_t>(6));
  return L"***" + urnw::Widen(address.substr(address.size() - take));
}

std::wstring FormatUsdcAmount(double amount) {
  wchar_t buf[64];
  std::swprintf(buf, std::size(buf), L"%.2f", amount);
  return buf;
}

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

std::string ExplorerTxUrl(std::string const& blockchain, std::string const& txHash) {
  if (txHash.empty()) return {};
  if (blockchain == urnet::SOL) return "https://solscan.io/tx/" + txHash;
  return "https://polygonscan.com/tx/" + txHash;
}

FrameworkElement WalletIconElement(std::string const& blockchain, double size) {
  Grid host;
  host.Width(size);
  host.Height(size);

  ShapeEllipse disc;
  disc.Width(size);
  disc.Height(size);
  disc.Fill(ChainGradient(blockchain));
  host.Children().Append(disc);

  TextBlock mark;
  mark.Text(hstring{urnw::Widen(blockchain)});
  mark.FontFamily(FontFamily(L"ms-appx:///Assets/Fonts/pp_neue_bit_bold.ttf#PP NeueBit"));
  mark.FontSize(size * 0.42);
  mark.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  mark.Foreground(colors::MakeBrush(winrt::Windows::UI::Colors::White()));
  mark.HorizontalAlignment(HorizontalAlignment::Center);
  mark.VerticalAlignment(VerticalAlignment::Center);
  host.Children().Append(mark);
  return host;
}

FrameworkElement PayoutWalletTag(bool isPayoutWallet) {
  Border chip;
  chip.CornerRadius(CornerRadiusHelper::FromUniformRadius(6));
  chip.Padding(Thickness{6, 2, 6, 2});
  if (!isPayoutWallet) {
    chip.Visibility(Visibility::Collapsed);
    return chip;
  }
  chip.Background(colors::MakeBrush(colors::WithAlpha(colors::kOffWhite, 0x0A)));
  TextBlock tag;
  tag.Text(Loc("default_txt"));
  tag.FontFamily(FontFamily(L"ms-appx:///Assets/Fonts/pp_neue_bit_bold.ttf#PP NeueBit"));
  tag.FontSize(16);
  tag.FontWeight(winrt::Windows::UI::Text::FontWeights::Bold());
  tag.Foreground(colors::MutedBrush());
  chip.Child(tag);
  return chip;
}

UIElement BuildPointsBreakdown(PointsBreakdown const& points, bool seekerHolder) {
  StackPanel root;
  root.Spacing(0);

  root.Children().Append(MakeText(Loc("points_breakdown"), 15, colors::TextBrush()));

  Grid columns;
  columns.Margin(Thickness{0, 12, 0, 0});
  columns.ColumnSpacing(16);
  for (int i = 0; i < 3; ++i) {
    ColumnDefinition col;
    col.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    columns.ColumnDefinitions().Append(col);
  }
  auto addColumn = [&columns](int index, hstring const& label, double value) {
    auto cell = StatCell(label, hstring{FormatPointsValue(value)});
    Grid::SetColumn(cell, index);
    columns.Children().Append(cell);
  };
  addColumn(0, Loc("payout"), points.payout);
  addColumn(1, Loc("referral"), points.referral);
  addColumn(2, Loc("reliability"), points.reliability);
  root.Children().Append(columns);

  root.Children().Append(HairLine());

  // The Seeker multiplier row only exists for a holder (iOS
  // AccountPointsBreakdown). Gold is reserved product-wide for the Pro
  // entitlement, so this uses the brand green, which is the "on / earning"
  // semantic everywhere else in the app.
  if (seekerHolder) {
    Grid row;
    row.ColumnSpacing(12);
    ColumnDefinition grow;
    grow.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    ColumnDefinition fit;
    fit.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Auto));
    row.ColumnDefinitions().Append(grow);
    row.ColumnDefinitions().Append(fit);

    StackPanel text;
    auto verified = MakeText(Loc("seeker_token_verified"), 14, colors::MakeBrush(colors::kUrGreen));
    text.Children().Append(verified);
    text.Children().Append(MakeText(Loc("you_re_earning_2x_points"), 12, colors::MutedBrush(), true));
    Grid::SetColumn(text, 0);
    row.Children().Append(text);

    auto plus = MakeValue(hstring{urnw::Format("plus_amount", FormatPointsValue(points.multiplier))});
    plus.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(plus, 1);
    row.Children().Append(plus);

    root.Children().Append(row);
    root.Children().Append(HairLine());
  }

  StackPanel total;
  total.HorizontalAlignment(HorizontalAlignment::Right);
  auto net = MakeValue(hstring{FormatPointsValue(points.net)}, 38);
  net.HorizontalAlignment(HorizontalAlignment::Right);
  total.Children().Append(net);
  auto caption = MakeText(Loc("net_points_earned"), 12, colors::MutedBrush());
  caption.HorizontalAlignment(HorizontalAlignment::Right);
  total.Children().Append(caption);
  root.Children().Append(total);

  return root;
}

// ---- WalletDetailSheet -------------------------------------------------------

std::shared_ptr<WalletDetailSheet> WalletDetailSheet::Create(
    XamlRoot const& root, SdkHost& sdk, urnet::AccountWallet const& wallet,
    bool isPayoutWallet, std::vector<urnet::AccountPayment> const& payments,
    bool allowActions, std::function<void()> onChanged,
    std::function<void(hstring)> onSuccess) {
  auto sheet = std::shared_ptr<WalletDetailSheet>(
      new WalletDetailSheet(sdk, wallet, isPayoutWallet, payments, allowActions,
                            std::move(onChanged), std::move(onSuccess)));
  sheet->Build(root);
  return sheet;
}

// Checked at the moment of the press, not only at Build: the buttons are
// already disabled without a session, so reaching this is a bug rather than a
// user path - which is exactly why it says so out loud instead of returning.
bool WalletDetailSheet::CanAct() {
  if (allowActions_) return true;
  urnw::LogError("wallet sheet: refusing an api write - the sheet was opened "
                 "with no session");
  ShowError(Loc("please_login_to_urnetwork"));
  return false;
}

void WalletDetailSheet::Build(XamlRoot const& root) {
  dialog_ = MakeDialog(root, Loc("wallet"));

  StackPanel content;
  content.Spacing(8);
  content.MinWidth(460);

  // identity: icon, "<Chain> wallet", the masked address, the payout tag
  Grid header;
  header.ColumnSpacing(12);
  ColumnDefinition iconCol;
  iconCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Auto));
  ColumnDefinition textCol;
  textCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  ColumnDefinition tagCol;
  tagCol.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Auto));
  header.ColumnDefinitions().Append(iconCol);
  header.ColumnDefinitions().Append(textCol);
  header.ColumnDefinitions().Append(tagCol);

  auto icon = WalletIconElement(wallet_.blockchain);
  Grid::SetColumn(icon, 0);
  header.Children().Append(icon);

  StackPanel identity;
  identity.VerticalAlignment(VerticalAlignment::Center);
  identity.Children().Append(
      MakeText(hstring{ChainDisplayName(wallet_.blockchain)}, 15, colors::TextBrush()));
  auto address = MakeText(hstring{MaskAddress(wallet_.wallet_address)}, 18, colors::TextBrush());
  address.FontFamily(FontFamily(L"ms-appx:///Assets/Fonts/pp_neue_bit_bold.ttf#PP NeueBit"));
  identity.Children().Append(address);
  Grid::SetColumn(identity, 1);
  header.Children().Append(identity);

  auto tag = PayoutWalletTag(isPayoutWallet_);
  tag.VerticalAlignment(VerticalAlignment::Top);
  Grid::SetColumn(tag, 2);
  header.Children().Append(tag);
  content.Children().Append(header);

  // the full address, which is the thing a user came here to copy
  content.Children().Append(MakeLabel(Loc("site_app_wallet_address")));
  auto full = MakeText(hstring{urnw::Widen(wallet_.wallet_address)}, 13, colors::TextBrush(), true);
  full.IsTextSelectionEnabled(true);
  content.Children().Append(full);

  content.Children().Append(HairLine());

  // actions. TAO wallets are recorded for future use only: the server refuses
  // to make one the payout wallet, so the affordance is replaced by the reason.
  if (wallet_.blockchain == urnet::TAO) {
    content.Children().Append(
        MakeText(Loc("bittensor_wallet_future_use"), 12, colors::MutedBrush(), true));
  } else if (!isPayoutWallet_) {
    makeDefaultButton_ = Button();
    makeDefaultButton_.Content(LocBox("make_default"));
    makeDefaultButton_.HorizontalAlignment(HorizontalAlignment::Stretch);
    makeDefaultButton_.IsEnabled(allowActions_);
    makeDefaultButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
      if (auto self = weak.lock()) self->MakeDefault();
    });
    content.Children().Append(makeDefaultButton_);
  }

  removeButton_ = Button();
  removeButton_.Content(LocBox("remove_wallet"));
  removeButton_.HorizontalAlignment(HorizontalAlignment::Stretch);
  // Disabled is the signal. With no session these two are the only controls on
  // the sheet that would talk to the server, and they are the reason a sample
  // wallet card could reach the api at all.
  removeButton_.IsEnabled(allowActions_);
  removeButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->RemoveWallet();
  });
  content.Children().Append(removeButton_);

  // Removal is destructive and there is no undo, so it takes two presses: the
  // first arms it and says what is about to happen, the second commits.
  confirmText_ = MakeText(Loc("are_you_sure_you_want_to_remove_this_wallet"), 12,
                          colors::DangerBrush(), true);
  confirmText_.Visibility(Visibility::Collapsed);
  content.Children().Append(confirmText_);

  // A refusal renders HERE, on the sheet, not on the page behind it.
  errorText_ = MakeText(hstring{}, 12, colors::DangerBrush(), true);
  errorText_.Visibility(Visibility::Collapsed);
  content.Children().Append(errorText_);

  // this wallet's payouts
  content.Children().Append(HairLine());
  content.Children().Append(MakeText(Loc("earnings"), 15, colors::TextBrush()));
  if (payments_.empty()) {
    content.Children().Append(MakeText(Loc("no_payouts_found"), 12, colors::MutedBrush(), true));
  } else {
    for (auto const& payment : payments_) {
      Grid row;
      row.ColumnSpacing(12);
      row.Margin(Thickness{0, 6, 0, 0});
      ColumnDefinition left;
      left.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
      ColumnDefinition right;
      right.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Auto));
      row.ColumnDefinitions().Append(left);
      row.ColumnDefinitions().Append(right);

      const std::string when = payment.complete_time  ? *payment.complete_time
                               : payment.create_time ? *payment.create_time
                                                     : std::string();
      auto date = MakeText(hstring{ShortDate(when)}, 13, colors::MutedBrush());
      Grid::SetColumn(date, 0);
      row.Children().Append(date);

      const bool completed = payment.completed && *payment.completed;
      hstring amount =
          completed
              ? hstring{urnw::Format("plus_amount_usdc",
                                     FormatUsdcAmount(payment.token_amount.value_or(0.0)))}
              : Loc("pending_payout");
      auto value = MakeText(amount, 13,
                            completed ? colors::TextBrush() : colors::MutedBrush());
      Grid::SetColumn(value, 1);
      row.Children().Append(value);
      content.Children().Append(row);
    }
  }

  ScrollViewer scroll;
  scroll.MaxHeight(520);
  scroll.Content(content);
  dialog_.Content(scroll);
}

uint32_t WalletDetailSheet::SetBusy(bool busy) {
  busy_ = busy;
  // Never re-enable what has no session behind it: !busy must not undo the
  // disable that Build() applied.
  if (makeDefaultButton_) makeDefaultButton_.IsEnabled(!busy && allowActions_);
  if (removeButton_) removeButton_.IsEnabled(!busy && allowActions_);

  if (!busy) {
    if (watchdog_) watchdog_.Stop();
    return requestGeneration_;
  }
  const uint32_t generation = ++requestGeneration_;
  if (!watchdog_) {
    watchdog_ = dialog_.DispatcherQueue().CreateTimer();
    watchdog_.IsRepeating(false);
    watchdog_.Interval(std::chrono::milliseconds(kRequestTimeoutMs));
    // Registered ONCE. busy_ is only true for the current generation, so the
    // handler needs no captured one of its own.
    watchdog_.Tick([weak = weak_from_this()](auto const&, auto const&) {
      auto self = weak.lock();
      if (!self || !self->busy_) return;
      // Bumping it here is what makes giving up final: a late success can no
      // longer hide the sheet and reload the page behind a message that told
      // the user the request had failed.
      ++self->requestGeneration_;
      urnw::LogError("wallet: request timed out with no callback after {} ms",
                     kRequestTimeoutMs);
      self->SetBusy(false);
      self->ShowError(Loc("something_went_wrong"));
    });
  }
  watchdog_.Start();
  return generation;
}

bool WalletDetailSheet::SettleRequest(uint32_t generation) {
  if (requestGeneration_ != generation) {
    urnw::LogWarn("wallet sheet: dropping an answer to a request already given up on");
    return false;
  }
  SetBusy(false);
  return true;
}

void WalletDetailSheet::ShowError(hstring const& message) {
  errorText_.Text(message);
  errorText_.Visibility(Visibility::Visible);
}

void WalletDetailSheet::MakeDefault() {
  // Busy is not an error. A second press while the first request is in flight
  // used to draw "Error setting default wallet" under a request that was still
  // perfectly alive and about to succeed.
  if (busy_) return;
  if (!CanAct()) return;
  if (!wallet_.wallet_id || wallet_.wallet_id->empty()) {
    ShowError(Loc("error_setting_default_wallet"));
    return;
  }
  const uint32_t generation = SetBusy(true);
  errorText_.Visibility(Visibility::Collapsed);
  urnet::SetPayoutWalletArgs args;
  args.wallet_id = *wallet_.wallet_id;

  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().setPayoutWallet(
      args, [queue, weak, generation](std::optional<urnet::SetPayoutWalletResult> result,
                                      std::optional<std::string> err) {
        const bool ok = result.has_value() && !err;
        const std::string error = err ? *err : std::string();
        if (!ok) urnw::LogError("wallet: setPayoutWallet failed: {}", error);
        queue.TryEnqueue([weak, ok, error, generation] {
          auto self = weak.lock();
          if (!self) return;
          if (!self->SettleRequest(generation)) return;
          if (!ok) {
            self->ShowError(error.empty()
                                ? Loc("error_setting_default_wallet")
                                : hstring{urnw::Format(
                                      "error_setting_default_wallet_with_reason",
                                      urnw::Widen(error))});
            return;
          }
          // the sheet closes, so the confirmation belongs on the page
          if (self->onSuccess_) self->onSuccess_(Loc("payout_wallet_updated"));
          if (self->onChanged_) self->onChanged_();
          self->dialog_.Hide();
        });
      });
}

void WalletDetailSheet::RemoveWallet() {
  if (busy_) return;
  if (!removeArmed_) {
    removeArmed_ = true;
    confirmText_.Visibility(Visibility::Visible);
    removeButton_.Content(LocBox("remove"));
    return;
  }
  CommitRemoveWallet();
}

void WalletDetailSheet::CommitRemoveWallet() {
  if (!CanAct()) return;
  if (!wallet_.wallet_id || wallet_.wallet_id->empty()) {
    ShowError(Loc("something_went_wrong"));
    return;
  }
  const uint32_t generation = SetBusy(true);
  errorText_.Visibility(Visibility::Collapsed);
  urnet::RemoveWalletArgs args;
  args.wallet_id = *wallet_.wallet_id;

  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().removeWallet(
      args, [queue, weak, generation](std::optional<urnet::RemoveWalletResult> result,
                                      std::optional<std::string> err) {
        const bool ok = result && result->success && !err;
        std::string error = err ? *err : std::string();
        if (error.empty() && result && result->error) error = result->error->message;
        if (!ok) urnw::LogError("wallet: removeWallet failed: {}", error);
        queue.TryEnqueue([weak, ok, error, generation] {
          auto self = weak.lock();
          if (!self) return;
          if (!self->SettleRequest(generation)) return;
          if (!ok) {
            // the server's message is not localizable; show it when there is one
            self->ShowError(error.empty() ? Loc("something_went_wrong")
                                          : hstring{urnw::Widen(error)});
            return;
          }
          // NO SNACKBAR HERE, deliberately, unlike MakeDefault - which raises
          // "Payout wallet updated" because the store HAS that sentence. There
          // is no "wallet removed" in it: all 931 en keys were read looking for
          // one, and the nearest, "Saved", is about settings and would be a lie
          // about a deletion. Inventing English is not on the table, so removal
          // reports itself the way the rest of the product reports one (blocked
          // locations, split rules): the sheet closes and the card is gone from
          // the list underneath. Raised in the P4 report as a missing string.
          if (self->onChanged_) self->onChanged_();
          self->dialog_.Hide();
        });
      });
}

// ---- PayoutDetailSheet -------------------------------------------------------

std::shared_ptr<PayoutDetailSheet> PayoutDetailSheet::Create(
    XamlRoot const& root, urnet::AccountPayment const& payment,
    PointsBreakdown const& points, bool seekerHolder) {
  auto sheet = std::shared_ptr<PayoutDetailSheet>(new PayoutDetailSheet());

  const bool completed = payment.completed && *payment.completed;
  const std::string when = payment.complete_time  ? *payment.complete_time
                           : payment.create_time ? *payment.create_time
                                                 : std::string();
  const hstring title = completed
                            ? hstring{urnw::Format("date_payout", ShortDate(when))}
                            : Loc("pending_payout");
  sheet->dialog_ = MakeDialog(root, title);

  StackPanel content;
  content.Spacing(12);
  content.MinWidth(460);

  Border pointsCard;
  pointsCard.Background(colors::CardBrush());
  pointsCard.CornerRadius(CornerRadiusHelper::FromUniformRadius(12));
  pointsCard.Padding(Thickness{16, 16, 16, 16});
  pointsCard.Child(BuildPointsBreakdown(points, seekerHolder));
  content.Children().Append(pointsCard);

  if (completed) {
    StackPanel amount;
    amount.Children().Append(MakeLabel(Loc("amount")));
    amount.Children().Append(MakeText(
        hstring{FormatUsdcAmount(payment.token_amount.value_or(0.0)) + L" " +
                (payment.token_type.empty() ? urnw::Localized("usdc")
                                            : urnw::Widen(payment.token_type))},
        14, colors::TextBrush()));
    content.Children().Append(amount);

    StackPanel wallet;
    wallet.Children().Append(MakeLabel(Loc("wallet_address")));
    auto addr = MakeText(hstring{urnw::Widen(payment.wallet_address)}, 13,
                         colors::TextBrush(), true);
    addr.IsTextSelectionEnabled(true);
    wallet.Children().Append(addr);
    content.Children().Append(wallet);

    StackPanel tx;
    tx.Children().Append(MakeLabel(Loc("transaction")));
    const std::string hash = payment.tx_hash.value_or(std::string());
    const std::string url =
        ExplorerTxUrl(payment.blockchain.value_or(std::string()), hash);
    if (url.empty()) {
      tx.Children().Append(MakeText(Loc("none"), 13, colors::MutedBrush()));
    } else {
      // the hash IS the link; a separate "view on explorer" row would be a new
      // string the store does not carry
      //
      // Uri THROWS on anything it cannot parse, and the hash it is built from
      // is a server string this client does not validate. The throw escaped
      // Create(), unwound into ShowPayoutDetail's catch(...), and the row the
      // user clicked did nothing whatsoever - no sheet, no message, no log
      // line. One unencodable character in one tx_hash and that payout is
      // simply unopenable, silently. Fall back to the hash as plain text: the
      // sheet is worth having without a working link, and the link is worth
      // nothing without the sheet.
      bool linked = false;
      try {
        HyperlinkButton link;
        link.Padding(Thickness{0, 0, 0, 0});
        link.NavigateUri(Uri(hstring{urnw::Widen(url)}));
        link.Content(MakeText(hstring{urnw::Widen(hash)}, 13, nullptr, true));
        tx.Children().Append(link);
        linked = true;
      } catch (winrt::hresult_error const& e) {
        urnw::LogError("payout: transaction url is not a uri ({}) - showing the hash "
                       "as plain text",
                       urnw::Narrow(std::wstring{e.message()}));
      }
      if (!linked) {
        tx.Children().Append(MakeText(hstring{urnw::Widen(hash)}, 13, colors::TextBrush(),
                                      true));
      }
    }
    content.Children().Append(tx);
  } else {
    // pending: what is actually known is how much data it covers so far
    StackPanel pending;
    pending.Children().Append(MakeText(
        hstring{urnw::Format(
            "pending_mb_provided",
            FormatUsdcAmount(static_cast<double>(payment.payout_byte_count) / 1'000'000.0))},
        13, colors::MutedBrush(), true));
    content.Children().Append(pending);
  }

  ScrollViewer scroll;
  scroll.MaxHeight(560);
  scroll.Content(content);
  sheet->dialog_.Content(scroll);
  return sheet;
}

}  // namespace urnw
