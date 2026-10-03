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

// The Referrals row: one whole-row button in pane B that opens the "Refer and
// earn" page. The code, the figures, the crowned state and the referral
// network all live there now (spec: referrals are their own section).
void AccountPage::BuildReferralsNav() {
  if (referralsNavBuilt_) return;
  referralsNavBuilt_ = true;
  rows::SetPaneMode(true);
  auto card = rows::Card(w_.AccountReferralsNavHost());
  auto button = rows::NavRow(card, Loc("referrals"), referralsNavValue_);
  button.Click([this](auto const&, auto const&) { w_.OpenReferrals(); });
  rows::SetPaneMode(false);
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
  RenderBalanceCodes({}, FieldState::NoSession);
  // The extender pane is per network space, so a signed-out window must not
  // keep the previous space's dns name, hosts or private secret on screen.
  if (extenderBuilt_) {
    advancedOpen_ = false;
    advancedPanel_.Visibility(Visibility::Collapsed);
    kit::SetTextOrCollapse(w_.AccountPaneDMeta(), {});
    ApplyExtenderForm(FieldState::NoSession, {}, {}, {}, /*hasController=*/false);
  }
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
  BuildReferralsNav();  // idempotent

  // the three pane headers
  w_.AccountPaneATitle().Text(Loc("plan"));
  w_.AccountPaneBTitle().Text(Loc("account"));
  w_.AccountPaneCTitle().Text(Loc("balance_codes_title"));
  w_.AccountPaneDTitle().Text(Loc("extenders"));
  // Landmark names, so a screen reader can tell three regions apart.
  Automation::AutomationProperties::SetName(w_.AccountPaneA(), Loc("plan"));
  Automation::AutomationProperties::SetName(w_.AccountPaneB(), Loc("account"));
  Automation::AutomationProperties::SetName(w_.AccountPaneC(), Loc("balance_codes_title"));
  Automation::AutomationProperties::SetName(w_.AccountPaneD(), Loc("extenders"));
  // Pane D is built once (BuildExtenderPane is one-shot), so unlike the two
  // builders below it cannot simply be re-run after a language change.
  ApplyExtenderStrings();

  // pane A: plan + usage
  w_.AccountPlanValueText().Text(Loc("free"));
  w_.AccountUpgradeButton().Content(LocBox("upgrade"));
  w_.AccountUsageGroupLabel().Text(Loc("data_usage"));
  w_.AccountDailyLabel().Text(Loc("daily_data_balance_label"));
  // the usage bar's three figures, labelled with the legend's own keys
  w_.AccountUsedLabel().Text(Loc("used_data_key"));
  w_.AccountPendingLabel().Text(Loc("pending_data_key"));
  w_.AccountAvailableLabel().Text(Loc("available_data_key"));
  // the pane C count's label is pane C's title key - one name for one thing
  w_.AccountBalanceCodesLabel().Text(Loc("balance_codes_title"));
  w_.RedeemRowText().Text(Loc("redeem_balance_code"));
  Automation::AutomationProperties::SetName(w_.RedeemRowButton(), Loc("redeem_balance_code"));

  // pane B: profile
  w_.AccountProfileGroupLabel().Text(Loc("profile"));
  // pane B: the row that opens the Refer and earn page
  w_.AccountReferralsGroupLabel().Text(Loc("refer_and_earn"));
  w_.AccountNetworkNameLabel().Text(Loc("network_name_label"));
  Automation::AutomationProperties::SetName(w_.NetworkNameRow(), Loc("network_name_label"));
  w_.NetworkNameBox().Header(LocBox("network_name_label"));
  w_.SaveNameButton().Content(LocBox("save"));
  w_.CancelNameButton().Content(LocBox("cancel"));

  // Every async field on this card starts in the state that says nothing has
  // been requested. Without this they were BLANK before a load - and
  // --preview-ui never runs one, which is how it was found: three empty cards
  // that looked like a broken screen rather than a signed-out one.
  if (!initialStatesApplied_) {
    initialStatesApplied_ = true;
    ApplyAccountState(FieldState::NoSession);
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
    LoadExtenderSettings();
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
  // pane D (K6): a local read of the space through the view controller, not an
  // api call, so it runs on the same navigation as everything else here
  LoadExtenderSettings();
}

void AccountPage::LoadReferralInfo() {
  if (!Sdk().IsLoggedIn()) return;
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
          // the code, the count and the crowned state show on the Refer and
          // earn page (ReferralsPage); here they feed pane A's referral rows
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
  // Pane A's Balance Codes count row: a real count shows for every answered
  // fetch (Loaded AND Empty - zero is an answer); NoSession / Loading / Failed
  // collapse the whole row rather than drawing a label over nothing. The value
  // goes through SetTextOrCollapse so "no count" is one empty string, and the
  // row mirrors it - the rule that helper was written for (UrComponents.h:98).
  const bool answered = state == FieldState::Loaded || state == FieldState::Empty;
  const hstring count = answered ? hstring{std::to_wstring(codes.size())} : hstring{};
  kit::SetTextOrCollapse(w_.AccountBalanceCodesValue(), count);
  w_.AccountBalanceCodesRow().Visibility(count.empty() ? Visibility::Collapsed
                                                       : Visibility::Visible);
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

// ---- pane D: extenders (connect/EXTENDER.md K6, K7) ------------------------
//
// Three edited values through the SDK's ExtenderViewController -- the extender
// dns name, the gossip url and the manual host list -- with the derived default
// as each empty field's placeholder, because an empty field MEANS the default
// and a box pre-filled with it would turn every save into an explicit override.
// Then the legacy private extender behind an Advanced row, and the two buttons
// that open the share and import sheets.
//
// Everything the form DECIDES is ExtenderPresentation.h, which is pure and
// tested off-Windows (tools/extender-tests.cpp); what is here is the building
// and the SDK calls.
//
// EVERY SDK CALL ON THIS PANE RUNS OFF THE UI THREAD. The view controller is
// opened from the DeviceRemote, so even reading the settings is an rpc to the
// service, and the private-extender write restarts the space's network client
// and node. A wedged service must not stall the window for an rpc timeout on
// every navigation to Account.

namespace {

// A labelled field row on the pane's rhythm: the 12px inset and the bottom
// hairline every other row in this pane carries, with the control under its
// label rather than beside it -- a url does not fit in a trailing slot.
TextBox AddFieldRow(Panel const& host, hstring const& label, bool multiline,
                    TextBlock& outCaption) {
  Border box;
  box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
  box.BorderBrush(urnw::colors::BorderBrush());
  box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  StackPanel column;
  column.Spacing(6);
  TextBlock caption;
  caption.Text(label);
  caption.FontSize(12);
  caption.Foreground(urnw::colors::MutedBrush());
  column.Children().Append(caption);
  outCaption = caption;
  TextBox field;
  field.Style(rows::Lookup(L"UrTextInputStyle"));
  if (multiline) {
    field.AcceptsReturn(true);
    field.TextWrapping(TextWrapping::Wrap);
    field.Height(84);
  }
  Automation::AutomationProperties::SetName(field, label);
  column.Children().Append(field);
  box.Child(column);
  host.Children().Append(box);
  return field;
}

// A full-width command on the pane's own bar style, the shape the plan pane's
// Upgrade button already uses.
Button AddActionRow(Panel const& host, hstring const& label, bool primary) {
  Button button;
  button.Content(winrt::box_value(label));
  button.Style(
      rows::Lookup(primary ? L"UrPaneActionPrimaryStyle" : L"UrPaneActionSecondaryStyle"));
  Automation::AutomationProperties::SetName(button, label);
  host.Children().Append(button);
  return button;
}

// Prose on the pane's rhythm: the row inset and hairline, wrapping allowed.
TextBlock AddNoteRow(Panel const& host, hstring const& text, Border& outRow) {
  Border box;
  box.Padding(ThicknessHelper::FromLengths(12, 8, 12, 8));
  box.BorderBrush(urnw::colors::BorderBrush());
  box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  TextBlock note;
  note.Text(text);
  note.FontSize(12);
  note.TextWrapping(TextWrapping::Wrap);
  note.Foreground(urnw::colors::MutedBrush());
  box.Child(note);
  host.Children().Append(box);
  outRow = box;
  return note;
}

}  // namespace

void AccountPage::BuildExtenderPane() {
  if (extenderBuilt_) return;
  extenderBuilt_ = true;
  auto host = w_.AccountExtenderHost();

  // Every label and button below is registered with the store id it came from,
  // so ApplyExtenderStrings can re-text it after a language change. This pane
  // is built once (unlike BuildProfileExtra, which ApplyStrings can simply
  // re-run), so without the register its labels would keep the language they
  // were first built in.
  auto header = [&](const char* key) {
    auto made = kit::MakePaneGroupHeader(Loc(key));
    extenderLabels_.emplace_back(made.title, key);
    host.Children().Append(made.root);
  };
  auto field = [&](Panel const& into, const char* key, bool multiline) {
    TextBlock caption{nullptr};
    TextBox box = AddFieldRow(into, Loc(key), multiline, caption);
    extenderLabels_.emplace_back(caption, key);
    return box;
  };
  auto action = [&](Panel const& into, const char* key, bool primary) {
    Button button = AddActionRow(into, Loc(key), primary);
    extenderButtons_.emplace_back(button, key);
    return button;
  };

  header("extender_settings");
  extenderDnsBox_ = field(host, "extender_dns_name", /*multiline=*/false);
  extenderGossipBox_ = field(host, "gossip_url", /*multiline=*/false);
  extenderHostsBox_ = field(host, "extender_hosts", /*multiline=*/true);
  {
    // the hint belongs to the host list, so it sits under it rather than at the
    // bottom of the group where it would read as a note about all three fields
    Border row{nullptr};
    extenderLabels_.emplace_back(AddNoteRow(host, Loc("extender_hosts_hint"), row),
                                 "extender_hosts_hint");
  }

  extenderSaveButton_ = action(host, "save", /*primary=*/true);
  extenderSaveButton_.Click([this](auto const&, auto const&) { SaveExtenderSettings(); });

  // K6 on iOS: "the tunnel extension picks the values up at its next start and
  // the app says so". Windows is in exactly that position -- the tunnel runs in
  // the service process, which imported this space's values when the session
  // started -- so the same sentence belongs here, standing rather than fired
  // once after a save, and only while there IS a session to be behind.
  extenderNote_ =
      AddNoteRow(host, Loc("extender_settings_next_connect"), extenderNoteRow_);
  extenderLabels_.emplace_back(extenderNote_, "extender_settings_next_connect");
  extenderNoteRow_.Visibility(Visibility::Collapsed);

  // One line carrying the load state and the save verdict. Without it a failed
  // save is invisible: the boxes keep what was typed and nothing else happens.
  // NOT registered for re-localization: what it says is a verdict written at
  // runtime, not a label, and re-texting it would overwrite that verdict.
  {
    Border row{nullptr};
    extenderStatus_ = AddNoteRow(host, {}, row);
  }

  // ---- advanced: the legacy private extender -------------------------------
  // A disclosure row rather than a WinUI Expander: the pane's vocabulary is
  // rows with a fixed height and a hairline, and an Expander would be the only
  // control on this destination wearing a different one.
  auto advanced = kit::MakePaneTwoLineRowButton(Loc("advanced"), Loc("private_extender"));
  advancedButton_ = advanced.root;
  extenderLabels_.emplace_back(advanced.title, "advanced");
  extenderLabels_.emplace_back(advanced.note, "private_extender");
  Automation::AutomationProperties::SetName(advancedButton_, Loc("advanced"));
  advancedButton_.Click([this](auto const&, auto const&) {
    advancedOpen_ = !advancedOpen_;
    advancedPanel_.Visibility(advancedOpen_ ? Visibility::Visible : Visibility::Collapsed);
  });
  host.Children().Append(advancedButton_);

  advancedPanel_ = StackPanel();
  advancedPanel_.Visibility(Visibility::Collapsed);
  {
    Border row{nullptr};
    extenderLabels_.emplace_back(
        AddNoteRow(advancedPanel_, Loc("private_extender_hint"), row),
        "private_extender_hint");
  }
  privateIpBox_ = field(advancedPanel_, "private_extender_ip", /*multiline=*/false);
  privateSecretBox_ = field(advancedPanel_, "private_extender_secret", /*multiline=*/false);
  privateSaveButton_ = action(advancedPanel_, "save", /*primary=*/false);
  privateSaveButton_.Click([this](auto const&, auto const&) { SavePrivateExtender(); });
  host.Children().Append(advancedPanel_);

  // ---- share and import (K7) -----------------------------------------------
  header("share_extenders");
  shareExtendersButton_ = action(host, "share_extenders", /*primary=*/false);
  shareExtendersButton_.Click([this](auto const&, auto const&) { ShowExtenderShareSheet(); });
  importExtendersButton_ = action(host, "import_extenders", /*primary=*/false);
  importExtendersButton_.Click([this](auto const&, auto const&) { ShowExtenderImportSheet(); });
}

void AccountPage::ApplyExtenderStrings() {
  if (!extenderBuilt_) return;
  for (auto const& [label, key] : extenderLabels_) {
    if (label) label.Text(Loc(key));
  }
  for (auto const& [button, key] : extenderButtons_) {
    if (!button) continue;
    button.Content(winrt::box_value(Loc(key)));
    Automation::AutomationProperties::SetName(button, Loc(key));
  }
  Automation::AutomationProperties::SetName(advancedButton_, Loc("advanced"));
}

winrt::fire_and_forget AccountPage::LoadExtenderSettings() {
  BuildExtenderPane();
  auto self = w_.get_strong();
  auto weak = w_.get_weak();
  auto queue = w_.DispatcherQueue();

  // Taken on the UI thread and held for the whole call: the shared reference is
  // what keeps the C handle registered while the session teardown may be
  // dropping the host's own (SdkHost::ExtenderController).
  const auto controller = Sdk().ExtenderController();
  if (!controller) {
    // The view controller is opened from the DeviceRemote, so the form needs a
    // live service session. Saying which of the two is missing is the whole
    // point of FieldState: "please log in" to a signed-in user is a lie.
    ApplyExtenderForm(Sdk().IsLoggedIn() ? FieldState::NoDevice : FieldState::NoSession, {},
                      {}, {}, /*hasController=*/false);
    co_return;
  }
  urnw::SdkHost* const sdk = &Sdk();

  ExtenderSettingsForm form;
  std::string networkHost;
  std::string privateIp;
  std::string privateSecret;
  FieldState state = FieldState::Loaded;

  co_await winrt::resume_background();
  try {
    ExtenderSettingsView view;
    if (const auto settings = controller->getSettings()) {
      view.dnsName = settings->DnsName;
      view.dnsNameDefault = settings->DnsNameDefault;
      view.gossipUrl = settings->GossipUrl;
      view.gossipUrlDefault = settings->GossipUrlDefault;
      view.networkHost = settings->NetworkHost;
      if (settings->Hosts) view.hosts = *settings->Hosts;
      if (settings->RootPublicKeys) view.rootPublicKeys = *settings->RootPublicKeys;
      view.rootPublicKeysDefault = settings->RootPublicKeysDefault;
    }
    form = ExtenderSettingsFormFor(view);
    networkHost = view.networkHost;
    // A local read, but it takes the host's own lock, which the session worker
    // holds for whole bootstraps - so it belongs on this side of the hop too.
    if (const auto privateExtender = sdk->CurrentNetExtender()) {
      privateIp = privateExtender->ip;
      privateSecret = privateExtender->secret;
    }
  } catch (const std::exception& e) {
    LogWarn("account: extender settings read failed: {}", e.what());
    state = FieldState::Failed;
  } catch (...) {
    LogWarn("account: extender settings read failed");
    state = FieldState::Failed;
  }

  queue.TryEnqueue([weak, state, form, networkHost, privateIp, privateSecret] {
    auto window = weak.get();
    if (!window) return;
    auto& page = window->account();
    kit::SetTextOrCollapse(window->AccountPaneDMeta(), H(networkHost));
    page.ApplyExtenderForm(state, form, privateIp, privateSecret, /*hasController=*/true);
  });
}

void AccountPage::ApplyExtenderForm(FieldState state, ExtenderSettingsForm const& form,
                                    std::string const& privateIp,
                                    std::string const& privateSecret, bool hasController) {
  if (!extenderBuilt_) return;
  const bool live = hasController && state != FieldState::Failed;
  for (auto const& box : {extenderDnsBox_, extenderGossipBox_, extenderHostsBox_,
                          privateIpBox_, privateSecretBox_}) {
    box.IsEnabled(live);
  }
  extenderSaveButton_.IsEnabled(live && !savingExtender_);
  privateSaveButton_.IsEnabled(live && !savingExtender_);
  shareExtendersButton_.IsEnabled(hasController);
  importExtendersButton_.IsEnabled(hasController);
  extenderNoteRow_.Visibility(hasController ? Visibility::Visible : Visibility::Collapsed);

  if (!live) {
    if (hasController) {
      // the read itself failed; that is not the same as having nothing to read
      kit::ApplySupportingText(extenderStatus_, {}, kit::ValidationState::NotChecked);
      rows::ApplyFieldState(extenderStatus_, state);
      return;
    }
    // The store has a line written for exactly this surface, and it covers BOTH
    // halves of the FieldState distinction in one sentence ("Sign in AND
    // connect"): with no session there is nothing to sign in as, and with no
    // service there is nothing to manage extenders through. Splitting it into
    // the two generic lines would invent a distinction the copy does not make.
    kit::ApplySupportingText(extenderStatus_, Loc("extenders_no_session"),
                             kit::ValidationState::NotChecked);
    return;
  }

  extenderDnsBox_.Text(H(form.dnsNameText));
  extenderDnsBox_.PlaceholderText(
      form.dnsNameDefaultValue.empty()
          ? hstring{}
          : hstring{urnw::Format("extender_default_value",
                                 urnw::Widen(form.dnsNameDefaultValue))});
  extenderGossipBox_.Text(H(form.gossipUrlText));
  extenderGossipBox_.PlaceholderText(
      form.gossipUrlDefaultValue.empty()
          ? hstring{}
          : hstring{urnw::Format("extender_default_value",
                                 urnw::Widen(form.gossipUrlDefaultValue))});
  extenderHostsBox_.Text(H(form.hostsText));
  privateIpBox_.Text(H(privateIp));
  privateSecretBox_.Text(H(privateSecret));
  kit::ApplySupportingText(extenderStatus_, {}, kit::ValidationState::NotChecked);
}

winrt::fire_and_forget AccountPage::SaveExtenderSettings() {
  if (savingExtender_) co_return;
  const auto controller = Sdk().ExtenderController();
  if (!controller) co_return;

  auto self = w_.get_strong();
  auto weak = w_.get_weak();
  auto queue = w_.DispatcherQueue();
  const std::string dnsName = winrt::to_string(extenderDnsBox_.Text());
  const std::string gossipUrl = winrt::to_string(extenderGossipBox_.Text());
  const std::vector<std::string> hosts =
      ParseExtenderHostLines(winrt::to_string(extenderHostsBox_.Text()));

  savingExtender_ = true;
  extenderSaveButton_.IsEnabled(false);
  privateSaveButton_.IsEnabled(false);
  kit::ApplySupportingText(extenderStatus_, Loc("loading"), kit::ValidationState::Validating);

  bool ok = false;
  co_await winrt::resume_background();
  try {
    // Empty means the derived default; the SDK trims and restarts the space's
    // network client and node in place (K6).
    controller->setSettings(dnsName, gossipUrl,
                            hosts.empty() ? std::optional<urnet::StringList>{}
                                          : std::optional<urnet::StringList>{hosts});
    ok = true;
  } catch (const std::exception& e) {
    LogWarn("account: extender settings save failed: {}", e.what());
  } catch (...) {
    LogWarn("account: extender settings save failed");
  }

  queue.TryEnqueue([weak, ok] {
    auto window = weak.get();
    if (!window) return;
    auto& page = window->account();
    page.savingExtender_ = false;
    page.extenderSaveButton_.IsEnabled(true);
    page.privateSaveButton_.IsEnabled(true);
    if (!ok) {
      kit::ApplySupportingText(page.extenderStatus_, Loc("something_went_wrong"),
                               kit::ValidationState::Invalid);
      return;
    }
    // Re-read FIRST: the SDK normalises what it stored, a cleared box comes
    // back as a default whose value the placeholder has to name -- and the
    // re-read owns this status line, so writing the verdict before it would be
    // writing to a line about to be cleared. The re-read is itself async, and
    // its own completion only touches the fields, so the verdict written here
    // survives it.
    page.LoadExtenderSettings();
    kit::ApplySupportingText(page.extenderStatus_, Loc("extender_settings_saved"),
                             kit::ValidationState::Valid);
  });
}

winrt::fire_and_forget AccountPage::SavePrivateExtender() {
  if (savingExtender_) co_return;
  auto self = w_.get_strong();
  auto weak = w_.get_weak();
  auto queue = w_.DispatcherQueue();
  const std::string ip = TrimWhitespace(winrt::to_string(privateIpBox_.Text()));
  const std::string secret = TrimWhitespace(winrt::to_string(privateSecretBox_.Text()));
  // The host outlives the window; a pointer taken here is what the background
  // half uses, rather than reading a member through `this` after the hop.
  urnw::SdkHost* const sdk = &Sdk();

  std::optional<urnet::NetExtender> value;
  if (!ip.empty()) {
    urnet::NetExtender netExtender;
    netExtender.ip = ip;
    netExtender.secret = secret;
    value = netExtender;
  }

  savingExtender_ = true;
  extenderSaveButton_.IsEnabled(false);
  privateSaveButton_.IsEnabled(false);
  kit::ApplySupportingText(extenderStatus_, Loc("loading"), kit::ValidationState::Validating);

  // No rpc, but it takes the host's own lock and the space manager restarts the
  // space's network client and node, so it is not a UI-thread call either.
  bool ok = false;
  co_await winrt::resume_background();
  ok = sdk->SetNetExtender(value);

  queue.TryEnqueue([weak, ok] {
    auto window = weak.get();
    if (!window) return;
    auto& page = window->account();
    page.savingExtender_ = false;
    page.extenderSaveButton_.IsEnabled(true);
    page.privateSaveButton_.IsEnabled(true);
    // The standing note under the Save button already says when the tunnel
    // picks this up; this line is only the verdict on the write.
    kit::ApplySupportingText(
        page.extenderStatus_,
        ok ? Loc("extender_settings_saved") : Loc("something_went_wrong"),
        ok ? kit::ValidationState::Valid : kit::ValidationState::Invalid);
  });
}

winrt::fire_and_forget AccountPage::ShowExtenderShareSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    extenderShareSheet_ = urnw::ExtenderShareSheet::Create(self->Content().XamlRoot(), Sdk());
    co_await extenderShareSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  extenderShareSheet_.reset();
  w_.SetSheetOpen(false);
}

winrt::fire_and_forget AccountPage::ShowExtenderImportSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    // The picker needs this window's HWND or it throws E_ACCESSDENIED instead
    // of opening (SettingsPage::SaveLogsToFile).
    HWND hwnd{};
    if (auto native = self.try_as<::IWindowNative>()) native->get_WindowHandle(&hwnd);
    auto weak = w_.get_weak();
    extenderImportSheet_ = urnw::ExtenderImportSheet::Create(
        self->Content().XamlRoot(), hwnd, Sdk(), [weak] {
          // an import may have replaced the dns name, gossip url and root keys
          if (auto window = weak.get()) window->account().LoadExtenderSettings();
        });
    co_await extenderImportSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  extenderImportSheet_.reset();
  w_.SetSheetOpen(false);
}

}  // namespace urnw
