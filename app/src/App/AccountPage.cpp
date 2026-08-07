// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "AccountPage.h"

#include <array>

#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "StatsFormat.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;

namespace urnw {

// winrt::implements makes IInspectable a member typedef of every C++/WinRT
// implementation type, which is why MainWindow could name it unqualified. A
// plain class outside that hierarchy has to bring it in.
using winrt::Windows::Foundation::IInspectable;

namespace {
// the YYYY-MM-DD prefix of an ISO timestamp (redeemed-codes list dates)
std::string IsoDate(std::string const& isoTime) {
  return isoTime.size() >= 10 ? isoTime.substr(0, 10) : isoTime;
}

// first 3 ... last 3 of a redeemed code's secret (macOS TransferBalanceCodesView)
std::string MaskSecret(std::string const& secret) {
  constexpr size_t keep = 3;
  if (secret.size() <= keep * 2) return std::string(secret.size(), '.');
  return secret.substr(0, keep) + "..." + secret.substr(secret.size() - keep);
}
}  // namespace

AccountPage::AccountPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window) {}

void AccountPage::ApplyStrings() {
  // account — plan + usage card, redeemed codes, profile, referrals
  w_.AccountPlanLabel().Text(Loc("plan"));
  w_.AccountPlanValueText().Text(Loc("free"));
  w_.AccountUpgradeButton().Content(LocBox("upgrade"));
  w_.AccountDailyLabel().Text(Loc("daily_data_balance_label"));
  w_.RedeemRowText().Text(Loc("redeem_balance_code"));
  w_.BalanceCodesLabel().Text(Loc("balance_codes_title"));
  w_.BalanceCodesEmptyText().Text(Loc("no_balance_codes_found"));
  w_.AccountHeading().Text(Loc("account"));
  w_.NetworkNameBox().Header(LocBox("network_name_label"));
  w_.SaveNameButton().Content(LocBox("save"));
  w_.ReferralsHeading().Text(Loc("referrals"));
  w_.RoyaltyText().Text(Loc("referral_royalty"));
}

void AccountPage::LoadAccount() {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getNetworkUser([queue, weak](std::optional<urnet::GetNetworkUserResult> result,
                                           std::optional<std::string>) {
    if (!result || !result->network_user) return;
    urnet::NetworkUser u = *result->network_user;
    queue.TryEnqueue([weak, u] {
      auto self = weak.get();
      if (!self) return;
      self->NetworkNameBox().Text(H(u.network_name));
      const std::wstring auth = urnw::Widen(u.user_auth ? *u.user_auth : std::string());
      self->AccountAuthText().Text(hstring{urnw::Format(
          u.verified ? "account_auth_verified" : "account_auth_unverified", auth)});
    });
  });
  LoadReferralInfo();
  LoadBalanceCodes();
}

void AccountPage::LoadReferralInfo() {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getNetworkReferralCode(
      [queue, weak](std::optional<urnet::GetNetworkReferralCodeResult> result,
                    std::optional<std::string>) {
        if (!result) return;
        std::string code = result->referral_code ? *result->referral_code : "—";
        int64_t total = result->total_referrals;
        queue.TryEnqueue([weak, code, total] {
          auto self = weak.get();
          if (!self) return;
          auto& page = self->account();
          page.referralCode_ = code;
          page.totalReferrals_ = total;
          // referrals no longer use deep links; friends enter the code on sign up
          self->ReferralText().Text(
              hstring{urnw::Format("referral_summary", urnw::Widen(code), total)});
          // referral royalty: at least one referral earns the crowned frog
          // mascot (same as the ur.io site)
          self->RoyaltyBadge().Visibility(0 < total ? Visibility::Visible
                                                    : Visibility::Collapsed);
          self->ApplyBalance();  // the usage-bar referral rows
        });
      });
}

void AccountPage::LoadBalanceCodes() {
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getNetworkRedeemedBalanceCodes(
      [queue, weak](std::optional<urnet::GetNetworkRedeemedBalanceCodesResult> result,
                    std::optional<std::string>) {
        urnet::RedeemedBalanceCodeList codes;
        if (result && result->balance_codes) codes = *result->balance_codes;
        queue.TryEnqueue([weak, codes = std::move(codes)] {
          auto self = weak.get();
          if (!self) return;
          auto panel = self->BalanceCodesPanel();
          panel.Children().Clear();
          self->BalanceCodesEmptyText().Visibility(codes.empty() ? Visibility::Visible
                                                                 : Visibility::Collapsed);
          if (codes.empty()) return;

          // header + one row per redeemed code: code / data / redeemed / expires
          auto makeRow = [](hstring const& c0, hstring const& c1, hstring const& c2,
                            hstring const& c3, bool header) {
            Grid row;
            ColumnDefinition d0, d1, d2, d3;
            d0.Width(GridLength{1, GridUnitType::Star});
            d1.Width(GridLength{0, GridUnitType::Auto});
            d1.MinWidth(84);
            d2.Width(GridLength{0, GridUnitType::Auto});
            d2.MinWidth(96);
            d3.Width(GridLength{0, GridUnitType::Auto});
            d3.MinWidth(96);
            row.ColumnDefinitions().Append(d0);
            row.ColumnDefinitions().Append(d1);
            row.ColumnDefinitions().Append(d2);
            row.ColumnDefinitions().Append(d3);
            const std::array<hstring, 4> cells = {c0, c1, c2, c3};
            for (int i = 0; i < 4; ++i) {
              TextBlock cell;
              cell.Text(cells[i]);
              cell.FontSize(12);
              if (header) cell.Foreground(urnw::colors::MutedBrush());
              Grid::SetColumn(cell, i);
              row.Children().Append(cell);
            }
            return row;
          };
          panel.Children().Append(makeRow(Loc("code"), Loc("data"), Loc("redeemed"),
                                          Loc("expires"), /*header=*/true));
          for (auto const& code : codes) {
            panel.Children().Append(makeRow(
                H(MaskSecret(code.secret)),
                H("+" + urnw::FormatByteCountCompact(code.balance_byte_count)),
                H(code.redeem_time ? IsoDate(*code.redeem_time) : std::string()),
                H(code.end_time ? IsoDate(*code.end_time) : std::string()),
                /*header=*/false));
          }
        });
      });
}

void AccountPage::OnSaveNetworkName(IInspectable const&, RoutedEventArgs const&) {
  urnet::NetworkUserUpdateArgs args;
  args.network_name = urnw::Narrow(w_.NetworkNameBox().Text().c_str());
  Sdk().api().networkUserUpdate(
      args, [](std::optional<urnet::NetworkUserUpdateResult>, std::optional<std::string>) {});
}

}  // namespace urnw
