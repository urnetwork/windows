// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "AccountPage.h"

#include <algorithm>
#include <array>

#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "StatsFormat.h"
#include "Strings.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;
using namespace urnw::rows;

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

// apple AccountNavStackView.needsNameClaim: true when the account carries NONE
// of the identity methods, i.e. it is still on its auto-generated name.
bool NeedsNameClaim(urnet::NetworkUser const& user) {
  static constexpr const char* kIdentityMethods[] = {"email", "phone", "google", "apple",
                                                     "solana"};
  if (!user.auth_types) {
    // The old single-auth_type shape; treat it the same way.
    return std::find(std::begin(kIdentityMethods), std::end(kIdentityMethods),
                     user.auth_type) == std::end(kIdentityMethods);
  }
  for (auto const& type : *user.auth_types) {
    for (auto const* identity : kIdentityMethods) {
      if (type == identity) return false;
    }
  }
  return true;
}
}  // namespace

AccountPage::AccountPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window) {}

void AccountPage::BuildProfileExtra() {
  if (built_) return;
  built_ = true;
  auto host = w_.AccountProfileExtra();

  // One line under the name field carrying the load state, the save verdict, or
  // the password-reset outcome. Without it a failed save was invisible: the box
  // kept whatever had been typed and nothing else happened.
  nameStatus_ = TextBlock();
  nameStatus_.FontSize(12);
  nameStatus_.TextWrapping(TextWrapping::Wrap);
  host.Children().Append(nameStatus_);

  changePasswordButton_ = Button();
  changePasswordButton_.Content(winrt::box_value(Loc("update_password")));
  changePasswordButton_.HorizontalAlignment(HorizontalAlignment::Left);
  // No auth to send a link to yet; enabled once the account load says otherwise.
  changePasswordButton_.IsEnabled(false);
  changePasswordButton_.Click([this](auto const&, auto const&) { SendPasswordReset(); });
  host.Children().Append(changePasswordButton_);
}

void AccountPage::ApplyAccountState(rows::FieldState state) {
  ApplyFieldState(nameStatus_, state);
  // Nothing on this card is actionable without the account behind it.
  const bool loaded = state == FieldState::Loaded;
  w_.NetworkNameBox().IsEnabled(loaded);
  w_.SaveNameButton().IsEnabled(loaded);
  changePasswordButton_.IsEnabled(loaded && !userAuth_.empty());
}

void AccountPage::ApplyStrings() {
  BuildProfileExtra();  // idempotent

  // account — plan + usage card, redeemed codes, profile, referrals
  w_.AccountPlanLabel().Text(Loc("plan"));
  w_.AccountPlanValueText().Text(Loc("free"));
  w_.AccountUpgradeButton().Content(LocBox("upgrade"));
  w_.AccountDailyLabel().Text(Loc("daily_data_balance_label"));
  w_.RedeemRowText().Text(Loc("redeem_balance_code"));
  w_.BalanceCodesLabel().Text(Loc("balance_codes_title"));
  // NOT "no balance codes found" here: that is a claim about the server's
  // answer, and at this point nothing has been asked. LoadBalanceCodes owns it.
  w_.AccountHeading().Text(Loc("account"));
  w_.NetworkNameBox().Header(LocBox("network_name_label"));
  w_.SaveNameButton().Content(LocBox("save"));
  w_.ReferralsHeading().Text(Loc("referrals"));
  w_.RoyaltyText().Text(Loc("referral_royalty"));

  // Every async field on this card starts in the state that says nothing has
  // been requested. Without this they were BLANK before a load - and
  // --preview-ui never runs one, which is how it was found: three empty cards
  // that looked like a broken screen rather than a signed-out one.
  if (!initialStatesApplied_) {
    initialStatesApplied_ = true;
    ApplyAccountState(FieldState::NoSession);
    ApplyFieldState(w_.ReferralText(), FieldState::NoSession);
    w_.BalanceCodesEmptyText().Visibility(Visibility::Visible);
    ApplyFieldState(w_.BalanceCodesEmptyText(), FieldState::NoSession);
  }
}

void AccountPage::LoadAccount() {
  if (!Sdk().IsLoggedIn()) {
    // apiReady() is api_.has_value(), set at SDK INIT rather than at login, so
    // it is not the session test - using it here would fire an unauthenticated
    // request and render the 401 as an empty account.
    ApplyAccountState(FieldState::NoSession);
    LoadReferralInfo();
    LoadBalanceCodes();
    return;
  }
  ApplyAccountState(FieldState::Loading);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getNetworkUser([queue, weak](std::optional<urnet::GetNetworkUserResult> result,
                                           std::optional<std::string> err) {
    std::string error;
    if (result && result->error) error = result->error->message;
    else if (err) error = *err;
    const bool failed = !error.empty() || !result || !result->network_user;
    if (failed) {
      LogWarn("account: getNetworkUser failed: {}", error);
      queue.TryEnqueue([weak] {
        if (auto self = weak.get()) self->account().ApplyAccountState(FieldState::Failed);
      });
      return;
    }
    urnet::NetworkUser u = *result->network_user;
    const bool claim = NeedsNameClaim(u);
    queue.TryEnqueue([weak, u, claim] {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->account();
      page.needsNameClaim_ = claim;
      page.userAuth_ = u.user_auth ? *u.user_auth : std::string();
      self->NetworkNameBox().Text(H(u.network_name));
      const std::wstring auth = urnw::Widen(page.userAuth_);
      self->AccountAuthText().Text(hstring{urnw::Format(
          u.verified ? "account_auth_verified" : "account_auth_unverified", auth)});
      page.ApplyAccountState(FieldState::Loaded);
      // The load succeeded, so the status line has nothing left to say; the
      // save/reset paths write it next.
      self->account().nameStatus_.Text(L"");
    });
  });
  LoadReferralInfo();
  LoadBalanceCodes();
}

void AccountPage::LoadReferralInfo() {
  if (!Sdk().IsLoggedIn()) {
    ApplyFieldState(w_.ReferralText(), FieldState::NoSession);
    w_.RoyaltyBadge().Visibility(Visibility::Collapsed);
    return;
  }
  ApplyFieldState(w_.ReferralText(), FieldState::Loading);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getNetworkReferralCode(
      [queue, weak](std::optional<urnet::GetNetworkReferralCodeResult> result,
                    std::optional<std::string> err) {
        std::string error;
        if (result && result->error) error = result->error->message;
        else if (err) error = *err;
        if (!error.empty() || !result) {
          LogWarn("account: getNetworkReferralCode failed: {}", error);
          queue.TryEnqueue([weak] {
            auto self = weak.get();
            if (!self) return;
            ApplyFieldState(self->ReferralText(), FieldState::Failed);
            self->RoyaltyBadge().Visibility(Visibility::Collapsed);
          });
          return;
        }
        std::string code = result->referral_code ? *result->referral_code : std::string();
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
  if (!Sdk().IsLoggedIn()) {
    w_.BalanceCodesPanel().Children().Clear();
    w_.BalanceCodesEmptyText().Visibility(Visibility::Visible);
    ApplyFieldState(w_.BalanceCodesEmptyText(), FieldState::NoSession);
    return;
  }
  w_.BalanceCodesEmptyText().Visibility(Visibility::Visible);
  ApplyFieldState(w_.BalanceCodesEmptyText(), FieldState::Loading);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getNetworkRedeemedBalanceCodes(
      [queue, weak](std::optional<urnet::GetNetworkRedeemedBalanceCodesResult> result,
                    std::optional<std::string> err) {
        // No error field on this result, so a missing result or a transport
        // error is the only failure signal - and without checking it a 401
        // arrived as an empty list and rendered as "No balance codes found".
        const bool failed = !result || err.has_value();
        if (failed) {
          LogWarn("account: getNetworkRedeemedBalanceCodes failed: {}",
                  err ? *err : std::string("no result"));
        }
        urnet::RedeemedBalanceCodeList codes;
        if (!failed && result->balance_codes) codes = *result->balance_codes;
        queue.TryEnqueue([weak, failed, codes = std::move(codes)] {
          auto self = weak.get();
          if (!self) return;
          auto panel = self->BalanceCodesPanel();
          panel.Children().Clear();
          if (failed) {
            self->BalanceCodesEmptyText().Visibility(Visibility::Visible);
            ApplyFieldState(self->BalanceCodesEmptyText(), FieldState::Failed);
            return;
          }
          self->BalanceCodesEmptyText().Visibility(codes.empty() ? Visibility::Visible
                                                                 : Visibility::Collapsed);
          if (codes.empty()) {
            // the shipped, specific empty line, restored after any state write
            self->BalanceCodesEmptyText().Text(Loc("no_balance_codes_found"));
            self->BalanceCodesEmptyText().Foreground(urnw::colors::FaintBrush());
            return;
          }

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

// Claim and change are DIFFERENT operations and the account decides which.
// Claim is for an account still on its auto-generated name and puts no reclaim
// cooldown on the old one; change applies a 24h cooldown that protects the name
// being given up (apple ProfileView). The previous implementation called
// networkUserUpdate, which is neither.
void AccountPage::OnSaveNetworkName(IInspectable const&, RoutedEventArgs const&) {
  if (savingName_ || !Sdk().IsLoggedIn()) return;
  const std::string name = TrimWhitespace(urnw::Narrow(w_.NetworkNameBox().Text().c_str()));
  if (name.empty()) {
    kit::ApplySupportingText(nameStatus_, Loc("network_name_length_error"),
                             kit::ValidationState::Invalid);
    return;
  }
  savingName_ = true;
  w_.SaveNameButton().IsEnabled(false);
  kit::ApplySupportingText(nameStatus_, Loc("loading"), kit::ValidationState::Validating);

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  // Both results carry the same shape, so one continuation serves both.
  auto done = [queue, weak](std::string newName, std::string error) {
    if (!error.empty()) LogWarn("account: network name save failed: {}", error);
    queue.TryEnqueue([weak, newName, error] {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->account();
      page.savingName_ = false;
      self->SaveNameButton().IsEnabled(true);
      if (!error.empty()) {
        // A server refusal is the interesting case here ("already taken", "too
        // similar") and it is not localizable, so it is shown verbatim.
        kit::ApplySupportingText(page.nameStatus_, H(error), kit::ValidationState::Invalid);
        return;
      }
      // MISSING STRING: the store has no "Network name changed to {}". The
      // server's accepted name in the brand green is the acknowledgement -
      // it is data, and the colour is the signal.
      self->NetworkNameBox().Text(H(newName));
      kit::ApplySupportingText(page.nameStatus_, H(newName), kit::ValidationState::Valid);
    });
  };

  if (needsNameClaim_) {
    urnet::ClaimNetworkNameArgs args;
    args.new_name = name;
    Sdk().api().claimNetworkName(
        args, [done](std::optional<urnet::ClaimNetworkNameResult> result,
                     std::optional<std::string> err) {
          std::string error;
          if (result && result->error) error = result->error->message;
          else if (err) error = *err;
          else if (!result) error = urnw::Narrow(urnw::Localized("something_went_wrong"));
          done(result ? result->network_name : std::string(), error);
        });
    return;
  }
  urnet::ChangeNetworkNameArgs args;
  args.new_name = name;
  Sdk().api().changeNetworkName(
      args, [done](std::optional<urnet::ChangeNetworkNameResult> result,
                   std::optional<std::string> err) {
        std::string error;
        if (result && result->error) error = result->error->message;
        else if (err) error = *err;
        else if (!result) error = urnw::Narrow(urnw::Localized("something_went_wrong"));
        done(result ? result->network_name : std::string(), error);
      });
}

void AccountPage::SendPasswordReset() {
  if (sendingReset_ || userAuth_.empty() || !Sdk().IsLoggedIn()) return;
  sendingReset_ = true;
  changePasswordButton_.IsEnabled(false);

  const std::string userAuth = userAuth_;
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  // AuthPasswordResetResult has no error field, so a result plus no transport
  // error is the whole success test.
  Sdk().api().authPasswordReset(
      [&] {
        urnet::AuthPasswordResetArgs args;
        args.user_auth = userAuth;
        return args;
      }(),
      [queue, weak, userAuth](std::optional<urnet::AuthPasswordResetResult> result,
                              std::optional<std::string> err) {
        const bool ok = !err && result.has_value();
        if (!ok) LogWarn("account: authPasswordReset failed: {}", err ? *err : std::string());
        queue.TryEnqueue([weak, ok, userAuth] {
          auto self = weak.get();
          if (!self) return;
          auto& page = self->account();
          page.sendingReset_ = false;
          page.changePasswordButton_.IsEnabled(true);
          if (ok) {
            kit::ApplySupportingText(
                page.nameStatus_,
                hstring{urnw::Format("password_reset_link_sent_to", urnw::Widen(userAuth))},
                kit::ValidationState::Valid);
            return;
          }
          kit::ApplySupportingText(page.nameStatus_, Loc("error_sending_password_reset_link"),
                                   kit::ValidationState::Invalid);
        });
      });
}

}  // namespace urnw
