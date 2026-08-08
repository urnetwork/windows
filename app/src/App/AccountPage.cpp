// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "AccountPage.h"

#include <algorithm>
#include <array>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Automation.h>

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
  {
    // Prose, so it may wrap - but it still carries the pane row's inset and
    // bottom hairline, or a verdict about the name appears to float between two
    // rows belonging to neither.
    Border box;
    box.Padding(ThicknessHelper::FromLengths(12, 8, 12, 8));
    box.BorderBrush(urnw::colors::BorderBrush());
    box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
    box.Child(nameStatus_);
    host.Children().Append(box);
  }

  // The password row is a ROW like every other one in this pane, not a loose
  // button under a card: the label says what it is and the trailing word says
  // what pressing it does.
  auto row = kit::MakePaneTwoLineRow(Loc("update_password"));
  changePasswordButton_ = Button();
  changePasswordButton_.Content(winrt::box_value(Loc("send")));
  // No auth to send a link to yet; enabled once the account load says otherwise.
  changePasswordButton_.IsEnabled(false);
  changePasswordButton_.Click([this](auto const&, auto const&) { SendPasswordReset(); });
  Automation::AutomationProperties::SetFullDescription(changePasswordButton_,
                                                       Loc("update_password"));
  row.trailing.Children().Append(changePasswordButton_);
  host.Children().Append(row.root);
}

// ---- the profile name's explicit edit mode (R4) ----------------------------
//
// The spec's Profile section asks for inline edit with Save appearing only while
// editing. What shipped was a permanently-open TextBox with a permanently-live
// Save button under it: no way to tell whether the name on screen was the saved
// one or something half-typed, and a Save that was always inviting a write.
//
// So the row has two states and exactly one of them is visible. Entering edit
// seeds the box from the value the last load wrote, which is also what Cancel
// restores - the box is never the source of truth for the name.
void AccountPage::SetEditingName(bool editing) {
  editingName_ = editing;
  w_.NetworkNameRow().Visibility(editing ? Visibility::Collapsed : Visibility::Visible);
  w_.NetworkNameEditPanel().Visibility(editing ? Visibility::Visible : Visibility::Collapsed);
  if (editing) {
    w_.NetworkNameBox().Text(H(networkName_));
    w_.NetworkNameBox().Focus(FocusState::Programmatic);
  }
}

// The row AND its text: a fixed-height row wrapped around an empty TextBlock is
// still an empty row, and it drew a 38px hole in the middle of the profile group
// on every signed-out frame.
void AccountPage::SetAuthText(winrt::hstring const& text) {
  w_.AccountAuthText().Text(text);
  w_.AccountAuthRow().Visibility(text.empty() ? Visibility::Collapsed : Visibility::Visible);
}

void AccountPage::ApplyNetworkName(std::string const& name) {
  networkName_ = name;
  kit::SetTextOrCollapse(w_.NetworkNameValue(), H(name));
}

void AccountPage::OnEditNetworkName(IInspectable const&, RoutedEventArgs const&) {
  // The row is only actionable with an account behind it; ApplyAccountState
  // disables it otherwise, and this is the second guard for the keyboard path.
  if (!Sdk().IsLoggedIn()) return;
  SetEditingName(true);
}

void AccountPage::OnCancelNetworkName(IInspectable const&, RoutedEventArgs const&) {
  SetEditingName(false);
  nameStatus_.Text(L"");
}

void AccountPage::ResetForSignOut() {
  // userAuth_ is the dangerous one: SendPasswordReset mails a link to it, so a
  // value left over from the previous session mails the PREVIOUS account's
  // owner. needsNameClaim_ would likewise pick that account's save branch.
  userAuth_.clear();
  referralCode_.clear();
  totalReferrals_ = 0;
  needsNameClaim_ = false;
  w_.NetworkNameBox().Text(L"");
  ApplyNetworkName({});
  SetEditingName(false);
  SetAuthText({});
  ApplyAccountState(FieldState::NoSession);
  ApplyFieldState(w_.ReferralText(), FieldState::NoSession);
  w_.RoyaltyBadge().Visibility(Visibility::Collapsed);
  RenderBalanceCodes({}, FieldState::NoSession);
}

void AccountPage::ApplyAccountState(rows::FieldState state) {
  ApplyFieldState(nameStatus_, state);
  // Nothing on this card is actionable without the account behind it.
  const bool loaded = state == FieldState::Loaded;
  w_.NetworkNameRow().IsEnabled(loaded);
  w_.NetworkNameBox().IsEnabled(loaded);
  w_.SaveNameButton().IsEnabled(loaded);
  changePasswordButton_.IsEnabled(loaded && !userAuth_.empty());
  // Leaving the editor open over a card that has just lost its account would
  // offer a Save that cannot run.
  if (!loaded && editingName_) SetEditingName(false);
}

void AccountPage::ApplyStrings() {
  BuildProfileExtra();  // idempotent

  // the three pane headers
  w_.AccountPaneATitle().Text(Loc("plan"));
  w_.AccountPaneBTitle().Text(Loc("account"));
  w_.AccountPaneCTitle().Text(Loc("balance_codes_title"));
  // Landmark names, so a screen reader can tell three regions apart.
  Automation::AutomationProperties::SetName(w_.AccountPaneA(), Loc("plan"));
  Automation::AutomationProperties::SetName(w_.AccountPaneB(), Loc("account"));
  Automation::AutomationProperties::SetName(w_.AccountPaneC(), Loc("balance_codes_title"));

  // pane A: plan + usage
  w_.AccountPlanValueText().Text(Loc("free"));
  w_.AccountUpgradeButton().Content(LocBox("upgrade"));
  w_.AccountUsageGroupLabel().Text(Loc("data_usage"));
  w_.AccountDailyLabel().Text(Loc("daily_data_balance_label"));
  w_.RedeemRowText().Text(Loc("redeem_balance_code"));
  Automation::AutomationProperties::SetName(w_.RedeemRowButton(), Loc("redeem_balance_code"));

  // pane B: profile
  w_.AccountProfileGroupLabel().Text(Loc("profile"));
  w_.AccountNetworkNameLabel().Text(Loc("network_name_label"));
  Automation::AutomationProperties::SetName(w_.NetworkNameRow(), Loc("network_name_label"));
  w_.NetworkNameBox().Header(LocBox("network_name_label"));
  w_.SaveNameButton().Content(LocBox("save"));
  w_.CancelNameButton().Content(LocBox("cancel"));
  w_.RoyaltyText().Text(Loc("referral_royalty"));

  // Every async field on this card starts in the state that says nothing has
  // been requested. Without this they were BLANK before a load - and
  // --preview-ui never runs one, which is how it was found: three empty cards
  // that looked like a broken screen rather than a signed-out one.
  if (!initialStatesApplied_) {
    initialStatesApplied_ = true;
    ApplyAccountState(FieldState::NoSession);
    ApplyFieldState(w_.ReferralText(), FieldState::NoSession);
    RenderBalanceCodes({}, FieldState::NoSession);
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
      page.ApplyNetworkName(u.network_name);
      const std::wstring auth = urnw::Widen(page.userAuth_);
      page.SetAuthText(hstring{urnw::Format(
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
    RenderBalanceCodes({}, FieldState::NoSession);
    return;
  }
  RenderBalanceCodes({}, FieldState::Loading);
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
          self->account().RenderBalanceCodes(
              codes, failed ? FieldState::Failed
                            : codes.empty() ? FieldState::Empty : FieldState::Loaded);
        });
      });
}

// The redeemed-code table, and every state it can be in, in one place.
//
// It used to be three scattered writes to a panel and an empty TextBlock, and
// they disagreed: the "no balance codes found" line was left visible under a
// populated table by one path and cleared by another. One function, one shape.
//
// THREE columns, not four. This is a 380dip pane; code / data / redeemed is what
// fits without any column collapsing, and the expiry is the least load-bearing
// of the four (a redeemed code's data is already on the plan). The weights are
// stars with minimums, so the table narrows rather than clips.
void AccountPage::RenderBalanceCodes(urnet::RedeemedBalanceCodeList const& codes,
                                     rows::FieldState state) {
  auto panel = w_.BalanceCodesPanel();
  panel.Children().Clear();

  const bool loaded = state == FieldState::Loaded && !codes.empty();
  // ApplyFieldState writes the right sentence for every non-value state; the
  // line is a centred one inside the FULL-HEIGHT pane, so "nothing here" is not
  // a short card at the top of a tall column.
  w_.BalanceCodesEmptyText().Visibility(loaded ? Visibility::Collapsed
                                               : Visibility::Visible);
  if (state == FieldState::Empty) {
    // the shipped, specific empty line rather than the generic "None"
    w_.BalanceCodesEmptyText().Text(Loc("no_balance_codes_found"));
    w_.BalanceCodesEmptyText().Foreground(urnw::colors::FaintBrush());
  } else if (!loaded) {
    ApplyFieldState(w_.BalanceCodesEmptyText(), state);
  }

  kit::SetTextOrCollapse(w_.AccountPaneCMeta(),
                         loaded ? hstring{std::to_wstring(codes.size())} : hstring{});
  if (!loaded) return;

  const std::vector<double> weights{1.4, 1.0, 1.2};
  panel.Children().Append(
      kit::MakePaneTableHeader(weights, {Loc("code"), Loc("data"), Loc("redeemed")}));
  for (auto const& code : codes) {
    auto row = kit::MakePaneTableRow(weights);
    row.cells[0].Text(H(MaskSecret(code.secret)));
    row.cells[1].Text(H("+" + urnw::FormatByteCountCompact(code.balance_byte_count)));
    row.cells[2].Text(H(code.redeem_time ? IsoDate(*code.redeem_time) : std::string()));
    panel.Children().Append(row.root);
  }
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
      page.ApplyNetworkName(newName);
      // The write landed, so the editor has nothing left to edit. Leaving it
      // open is what made the old card ambiguous about which name was saved.
      page.SetEditingName(false);
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
