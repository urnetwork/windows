// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "SettingsPage.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>

#include "BalanceSheets.h"  // SetMarkdownLinkText, for the community link rows
#include "Ids.h"
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
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

// The community links. Both URLs also appear inside their localized markdown
// strings; these constants are what the affordance actually opens, and the two
// must be kept in step (the apple client carries the same duplication).
constexpr wchar_t kDiscordUrl[] = L"https://discord.com/invite/RUNZXMwPRK";
constexpr wchar_t kDePinHubUrl[] = L"https://depinhub.io/projects/urnetwork";

// A loaded value, or the Empty state when the server genuinely returned
// nothing. Never a bare em dash: rows::FieldState exists precisely because one
// dash cannot mean "not requested", "in flight", "nothing" and "failed" at once.
void ApplyValue(TextBlock const& field, std::string const& value) {
  if (value.empty()) {
    ApplyFieldState(field, FieldState::Empty);
    return;
  }
  ApplyFieldState(field, FieldState::Loaded, winrt::to_hstring(value));
}

// apple AuthMethods.parseAuthMethods: the server's auth_types list is the
// source, with the older single auth_type + userAuth shape as the fallback.
std::vector<std::string> ParseAuthMethods(urnet::NetworkUser const& user) {
  std::vector<std::string> methods;
  if (user.auth_types) {
    for (auto const& type : *user.auth_types) {
      if (!type.empty()) methods.push_back(type);
    }
  }
  if (!methods.empty()) return methods;
  if (!user.auth_type.empty()) methods.push_back(user.auth_type);
  if (user.user_auth && !user.user_auth->empty()) {
    const std::string derived =
        user.user_auth->find('@') != std::string::npos ? "email" : *user.user_auth;
    if (std::find(methods.begin(), methods.end(), derived) == methods.end()) {
      methods.push_back(derived);
    }
  }
  return methods;
}

// The auth type is a SERVER IDENTIFIER ("email", "google", "solana"), not a UI
// string, so it is rendered as data with its first letter raised - exactly as
// apple's methodDisplayName does, and for the same reason: there is no
// localization key per provider and inventing English ones would be worse.
std::string AuthMethodLabel(std::string const& authType) {
  if (authType.empty()) return authType;
  std::string label = authType;
  label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
  return label;
}

void OpenUrl(std::wstring_view url) {
  try {
    winrt::Windows::System::Launcher::LaunchUriAsync(
        winrt::Windows::Foundation::Uri(hstring{url}));
  } catch (...) {
    LogWarn("settings: could not open {}", urnw::Narrow(url));
  }
}

}  // namespace

SettingsPage::SettingsPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window), snackbar_(window.SupportInfo(), window.DispatcherQueue()) {}

void SettingsPage::ApplyStrings() {
  BuildSections();  // idempotent

  // support
  w_.FeedbackHeading().Text(Loc("feedback"));
  // Why the screen exists. Already in the store, used nowhere until now: the
  // panel opened on five bare controls and no sentence.
  w_.SupportIntroText().Text(Loc("site_app_support_intro"));
  w_.FeedbackRating().Caption(Loc("how_are_we_doing"));
  w_.FeedbackText().Header(LocBox("anything_else"));
  // The box shipped with no content whatsoever - an unlabelled tick offering to
  // upload the user's logs. The string existed the whole time.
  w_.FeedbackIncludeLogs().Content(LocBox("feedback_include_logs"));
  // Send is now glyph + label, and a Button whose Content is a Panel gets NO
  // automatic automation name, so it needs an explicit one or the only way to
  // submit this form is nameless to a screen reader.
  w_.SendFeedbackText().Text(Loc("send"));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      w_.SendFeedbackButton(), Loc("send"));

  // settings
  w_.SplitTunnelHeading().Text(Loc("app_split_rules"));
  w_.SplitTunnelDescription().Text(Loc("apps_listed_bypass_vpn"));
  w_.ManageAppSplitButton().Content(LocBox("manage_apps"));
  w_.SettingsAccountHeading().Text(Loc("account"));
  w_.SignOutButton().Content(LocBox("sign_out"));
  w_.ProtocolLink().Content(LocBox("uses_ur_protocol"));
}

// ---- section construction --------------------------------------------------

void SettingsPage::BuildSections() {
  if (built_) return;
  built_ = true;

  // TWO hosts, not one. At desktop widths they are two card columns (D4); at
  // flyout width the second stacks under the first, so the split has to be by
  // SUBJECT rather than by length, or the narrow reading order comes out
  // shuffled. Left is what this account and this device ARE; right is how they
  // connect and who they talk to.
  auto host = w_.SettingsSections();
  auto right = w_.SettingsSectionsRight();
  BuildAccountSection(host);
  BuildDeviceSection(host);
  BuildSubscriptionSection(host);
  BuildConnectionsSection(right);
  BuildIdentitySection(right);
  BuildStayInTouchSection(right);
  BuildLogsSection(right);
  BuildVersionSection(right);
  BuildDangerSection();

  // Everything that can be read without a round trip, so the page is not blank
  // before (or without) a load: the client id and the persisted kill switch.
  ApplyLocalDeviceState();
  // urnet::version() is EMPTY in this SDK build (the "sdk initialized:
  // version=" startup line shows it too), so the app version is what actually
  // identifies the build here.
  const std::string sdkVersion = urnet::version();
  ApplyValue(versionValue_, sdkVersion.empty() ? Sdk().appVersion() : sdkVersion);
}

void SettingsPage::BuildAccountSection(Panel const& host) {
  // Deliberately unheaded. The markup below already carries an "Account"
  // heading (over Sign out), and a second one here read as a stutter on screen
  // - two identical brand headings on one page. Every row in this card names
  // itself, which is also how macOS's settings form presents them.
  auto card = Card(host);

  // Client ID - the identifier support asks for; copy is the only action.
  clientIdValue_ = ValueActionRow(card, Loc("client_id"), Loc("copy"), clientIdCopy_);
  clientIdCopy_.IsEnabled(false);
  clientIdCopy_.Click([this](auto const&, auto const&) {
    if (clientId_.empty()) return;
    CopyToClipboard(clientId_);
    snackbar_.Show(Loc("client_id_copied_to_clipboard"), InfoBarSeverity::Success);
  });

  Divider(card);

  // Bonus referral code - what a friend types on sign up.
  referralCodeValue_ =
      ValueActionRow(card, Loc("bonus_referral_code_label"), Loc("copy"), referralCodeCopy_);
  referralCodeCopy_.IsEnabled(false);
  referralCodeCopy_.Click([this](auto const&, auto const&) {
    if (referralCode_.empty()) return;
    CopyToClipboard(referralCode_);
    snackbar_.Show(Loc("bonus_referral_code_copied_to_clipboard"), InfoBarSeverity::Success);
  });

  Divider(card);

  // Referral network - who referred THIS network, editable in a sheet.
  auto referralButton = NavRow(card, Loc("referral_network"), referralNetworkValue_);
  referralButton.Click([this](auto const&, auto const&) { ShowReferralNetworkSheet(); });

  // Both referral rows start in the state that says WHY they are empty. Without
  // this they rendered as blank cells before any load ran - the exact "is this
  // empty, loading, or broken?" ambiguity FieldState exists to remove, and it
  // was visible on screen because --preview-ui never calls LoadSettings.
  ApplyFieldState(referralCodeValue_, FieldState::NoSession);
  ApplyFieldState(referralNetworkValue_, FieldState::NoSession);

  Divider(card);

  // Auth code - a short-lived credential for signing in on another device.
  auto authCodeButton = ButtonRow(card, Loc("auth_code"),
                                  Loc("created_auth_codes_expire_after_5_minutes"),
                                  Loc("site_app_create_auth_code"));
  authCodeButton.Click([this](auto const&, auto const&) { ShowAuthCodeSheet(); });

  Divider(card);

  // Sign-in methods, one row each, with remove; and an add affordance.
  TextBlock methodsLabel;
  methodsLabel.Text(Loc("site_app_login_methods"));
  methodsLabel.Style(Lookup(L"UrLabelStyle"));
  card.Children().Append(methodsLabel);
  authMethodsPanel_ = StackPanel();
  authMethodsPanel_.Spacing(4);
  card.Children().Append(authMethodsPanel_);
  Button addAuth;
  addAuth.Content(winrt::box_value(Loc("add")));
  addAuth.HorizontalAlignment(HorizontalAlignment::Left);
  addAuth.Click([this](auto const&, auto const&) { ShowAddAuthSheet(); });
  card.Children().Append(addAuth);
  // Nothing has been asked for yet; LoadSettings moves this on.
  RenderAuthMethods(FieldState::NoSession);
}

void SettingsPage::BuildDeviceSection(Panel const& host) {
  Heading(host, Loc("device"), L"");
  auto card = Card(host);
  auto nameButton = NavRow(card, Loc("device_name_label"), deviceNameValue_);
  nameButton.Click([this](auto const&, auto const&) { ShowDeviceNameSheet(); });
  Divider(card);
  // Spec is server-assigned and read-only, exactly as on macOS.
  deviceSpecValue_ = ValueRow(card, Loc("device_spec_label"));
  ApplyFieldState(deviceNameValue_, FieldState::NoSession);
  ApplyFieldState(deviceSpecValue_, FieldState::NoSession);
}

void SettingsPage::BuildConnectionsSection(Panel const& host) {
  Heading(host, Loc("site_app_connections"), L"");
  auto card = Card(host);

  // Kill switch. The note is the shipped one-liner for what it actually does,
  // because "kill switch" alone does not say which way it runs.
  killSwitch_ = ToggleRow(card, Loc("kill_switch"), Loc("site_app_kill_switch_note"));
  killSwitch_.Toggled([this](auto const&, auto const&) { OnKillSwitchToggled(); });

  Divider(card);

  TextBlock unused{nullptr};
  auto blockedButton = NavRow(card, Loc("blocked_locations_2"), unused);
  blockedButton.Click([this](auto const&, auto const&) { ShowBlockedLocationsSheet(); });
}

void SettingsPage::BuildIdentitySection(Panel const& host) {
  Heading(host, Loc("post_quantum_identity"), L"");
  auto card = Card(host);
  TextBlock unused{nullptr};
  auto button = NavRow(card, Loc("provider_identities"), unused);
  button.Click([this](auto const&, auto const&) { ShowIdentitySheet(); });
  Supporting(card, Loc("post_quantum_identity_explanation"));
}

void SettingsPage::BuildStayInTouchSection(Panel const& host) {
  Heading(host, Loc("stay_in_touch"), L"");
  auto card = Card(host);

  productUpdatesState_ = TextBlock();
  productUpdatesState_.FontSize(12);
  productUpdatesState_.TextWrapping(TextWrapping::Wrap);
  productUpdates_ = ToggleRow(card, Loc("send_product_updates"), hstring{});
  productUpdates_.IsEnabled(false);  // until the current value has been read
  productUpdates_.Toggled([this](auto const&, auto const&) { OnProductUpdatesToggled(); });
  // This was the ONE async field with no FieldState: a failed read left the
  // toggle disabled, byte-identical on screen to "no session" and to "still
  // loading". The line under it says which.
  card.Children().Append(productUpdatesState_);
  ApplyFieldState(productUpdatesState_, FieldState::NoSession);

  Divider(card);

  // Both community rows are the store's own markdown strings, rendered with the
  // link inline (SetMarkdownLinkText). That keeps the whole shipped sentence -
  // there is no plain-text variant of the DePIN Hub line - and needs no extra
  // "Open" word beside it.
  TextBlock discord;
  SetMarkdownLinkText(discord, Localized("join_the_community_on_discord_https_discord_com"), 14);
  card.Children().Append(discord);

  Divider(card);

  TextBlock depin;
  SetMarkdownLinkText(depin, Localized("verified_project_on_depin_hub_https_depinhub_io"), 14);
  card.Children().Append(depin);
}

void SettingsPage::BuildSubscriptionSection(Panel const& host) {
  auto card = Card(host);
  // Opens the Stripe customer portal in the browser; there is nothing to show
  // beside the label, so the whole row is the affordance.
  TextBlock unused{nullptr};
  manageSubscription_ = NavRow(card, Loc("site_app_manage_subscription"), unused);
  manageSubscription_.Click([this](auto const&, auto const&) { OpenCustomerPortal(); });
}

void SettingsPage::BuildLogsSection(Panel const& host) {
  Heading(host, Loc("export_logs"), L"");
  auto card = Card(host);
  // Saving to a file the user picks is the ONLY log affordance here now.
  //
  // There used to be a second row labelled "Share logs" that called
  // Device::uploadLogs. Three things were wrong with it and all three matter:
  // "Share logs" is apple's label for its LOCAL share sheet, not a server
  // upload, so it disclosed nothing about what left the machine; the feedback
  // id was minted client-side with newId(), so the upload correlated with
  // nothing and support could never find it; and it acknowledged with "Thanks
  // for the feedback!" when no feedback had been sent.
  //
  // Uploading now happens where apple does it - inside the feedback flow, with
  // the SERVER-issued feedback id, and only when the user ticks the box (see
  // OnSendFeedback). That makes the upload correlated, disclosed and consented.
  auto save = ButtonRow(card, Loc("save_logs"), hstring{}, Loc("save"));
  save.Click([this](auto const&, auto const&) { SaveLogsToFile(); });
}

void SettingsPage::BuildVersionSection(Panel const& host) {
  auto card = Card(host);
  versionValue_ = ValueRow(card, Loc("version_info"));
}

void SettingsPage::BuildDangerSection() {
  auto host = w_.SettingsDangerSection();
  deleteAccountButton_ = Button();
  deleteAccountButton_.Content(winrt::box_value(Loc("delete_account_2")));
  deleteAccountButton_.Foreground(colors::DangerBrush());
  deleteAccountButton_.HorizontalAlignment(HorizontalAlignment::Left);
  deleteAccountButton_.Click([this](auto const&, auto const&) { ShowDeleteAccountSheet(); });
  host.Children().Append(deleteAccountButton_);
}

// ---- loads -----------------------------------------------------------------

void SettingsPage::ResetForSignOut() {
  // Identity first: every one of these describes the account that just left.
  // networkName_ is the dangerous one - it is what the delete gate compares
  // against, and networkDelete acts on whatever JWT is current.
  clientId_.clear();
  referralCode_.clear();
  networkName_.clear();
  deviceName_.clear();
  authTypes_.clear();
  preferencesLoaded_ = false;

  // Then the visible state, so nothing on screen still claims to describe it.
  ApplyFieldState(clientIdValue_, FieldState::NoSession);
  ApplyFieldState(referralCodeValue_, FieldState::NoSession);
  ApplyFieldState(referralNetworkValue_, FieldState::NoSession);
  ApplyFieldState(deviceNameValue_, FieldState::NoSession);
  ApplyFieldState(deviceSpecValue_, FieldState::NoSession);
  RenderAuthMethods(FieldState::NoSession);
  clientIdCopy_.IsEnabled(false);
  referralCodeCopy_.IsEnabled(false);
  applyingPreference_ = true;
  productUpdates_.IsOn(false);
  applyingPreference_ = false;
  productUpdates_.IsEnabled(false);
  ApplyFieldState(productUpdatesState_, FieldState::NoSession);
}

void SettingsPage::LoadSettings() {
  ApplyLocalDeviceState();
  if (!Sdk().IsLoggedIn()) {
    // No token: nothing here can be fetched, and every field says so rather
    // than sitting on a dash or a spinner that will never resolve. This is the
    // state --preview-ui shows, and the state the owner sees before a login
    // exists on this box.
    ApplyFieldState(referralCodeValue_, FieldState::NoSession);
    ApplyFieldState(referralNetworkValue_, FieldState::NoSession);
    ApplyFieldState(deviceNameValue_, FieldState::NoSession);
    ApplyFieldState(deviceSpecValue_, FieldState::NoSession);
    RenderAuthMethods(FieldState::NoSession);
    productUpdates_.IsEnabled(false);
    ApplyFieldState(productUpdatesState_, FieldState::NoSession);
    return;
  }
  LoadNetworkUser();
  LoadDeviceInfo();
  LoadReferral();
  LoadPreferences();
}

void SettingsPage::ApplyLocalDeviceState() {
  // The client id is the device's, so it needs a session; the kill switch is
  // NOT - it is persisted in the app LocalState and readable with the tunnel
  // down (SdkHost::CurrentKillSwitch).
  clientId_.clear();
  if (Sdk().hasDevice()) {
    try {
      clientId_ = Sdk().device().getClientId();
    } catch (const std::exception& e) {
      LogWarn("settings: client id read failed: {}", e.what());
    }
  }
  // The client id comes off the DEVICE, so a signed-in user with no service
  // running has no id - and telling them to log in would be a lie.
  if (clientId_.empty()) {
    ApplyFieldState(clientIdValue_,
                    Sdk().IsLoggedIn() ? FieldState::NoDevice : FieldState::NoSession);
  } else {
    ApplyFieldState(clientIdValue_, FieldState::Loaded, winrt::to_hstring(clientId_));
  }
  clientIdCopy_.IsEnabled(!clientId_.empty());

  applyingKillSwitch_ = true;
  killSwitch_.IsOn(Sdk().CurrentKillSwitch());
  applyingKillSwitch_ = false;
}

void SettingsPage::LoadNetworkUser() {
  if (!Sdk().IsLoggedIn()) {
    RenderAuthMethods(FieldState::NoSession);
    return;
  }
  RenderAuthMethods(FieldState::Loading);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getNetworkUser([queue, weak](std::optional<urnet::GetNetworkUserResult> result,
                                           std::optional<std::string> err) {
    // A failure here used to leave "Loading..." on screen for the life of the
    // process, which is indistinguishable from a slow network and is the exact
    // shape of bug this project keeps paying for. It now terminates, visibly.
    std::string error;
    if (result && result->error) error = result->error->message;
    else if (err) error = *err;
    const bool failed = !error.empty() || !result || !result->network_user;
    std::vector<std::string> methods;
    std::string networkName;
    if (!failed) {
      methods = ParseAuthMethods(*result->network_user);
      networkName = result->network_user->network_name;
    }
    if (failed) LogWarn("settings: getNetworkUser failed: {}", error);
    queue.TryEnqueue([weak, failed, methods = std::move(methods), networkName]() mutable {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->settings();
      if (failed) {
        page.authTypes_.clear();
        page.RenderAuthMethods(FieldState::Failed);
        return;
      }
      page.authTypes_ = std::move(methods);
      page.networkName_ = networkName;
      page.RenderAuthMethods(page.authTypes_.empty() ? FieldState::Empty : FieldState::Loaded);
    });
  });
}

void SettingsPage::LoadDeviceInfo() {
  // apple SettingsViewModel.fetchDeviceInfo: find THIS client in the network's
  // client list and read its name and spec off it. Without a client id there is
  // nothing to match on, and that is a session problem, not an empty result.
  if (clientId_.empty()) {
    // Same distinction: with a session but no device there is no client id to
    // match this machine against in the network's client list.
    const FieldState state =
        Sdk().IsLoggedIn() ? FieldState::NoDevice : FieldState::NoSession;
    ApplyFieldState(deviceNameValue_, state);
    ApplyFieldState(deviceSpecValue_, state);
    return;
  }
  ApplyFieldState(deviceNameValue_, FieldState::Loading);
  ApplyFieldState(deviceSpecValue_, FieldState::Loading);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  const std::string clientId = clientId_;
  Sdk().api().getNetworkClients([queue, weak, clientId](
                                    std::optional<urnet::NetworkClientsResult> result,
                                    std::optional<std::string> err) {
    const bool failed = !result || err.has_value();
    if (failed) LogWarn("settings: getNetworkClients failed: {}", err ? *err : std::string());
    std::string name, spec;
    bool found = false;
    if (result && result->clients) {
      for (auto const& info : *result->clients) {
        if (!info.client_id || *info.client_id != clientId) continue;
        name = info.device_name.empty() ? info.description : info.device_name;
        spec = info.device_spec;
        found = true;
        break;
      }
    }
    if (!failed && !found) {
      // The call worked and this client is not in its own network's list. That
      // is not "empty" - it means something is wrong - so say so and log it.
      LogWarn("settings: client {} is not in the network client list", clientId);
    }
    queue.TryEnqueue([weak, failed, found, name, spec] {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->settings();
      if (failed || !found) {
        ApplyFieldState(page.deviceNameValue_, FieldState::Failed);
        ApplyFieldState(page.deviceSpecValue_, FieldState::Failed);
        return;
      }
      page.deviceName_ = name;
      ApplyValue(page.deviceNameValue_, name);
      ApplyValue(page.deviceSpecValue_, spec);
    });
  });
}

void SettingsPage::LoadReferral() {
  if (!Sdk().IsLoggedIn()) {
    ApplyFieldState(referralCodeValue_, FieldState::NoSession);
    ApplyFieldState(referralNetworkValue_, FieldState::NoSession);
    return;
  }
  ApplyFieldState(referralCodeValue_, FieldState::Loading);
  ApplyFieldState(referralNetworkValue_, FieldState::Loading);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getNetworkReferralCode(
      [queue, weak](std::optional<urnet::GetNetworkReferralCodeResult> result,
                    std::optional<std::string> err) {
        std::string error;
        if (result && result->error) error = result->error->message;
        else if (err) error = *err;
        const bool failed = !error.empty() || !result;
        if (failed) LogWarn("settings: getNetworkReferralCode failed: {}", error);
        std::string code;
        if (!failed && result->referral_code) code = *result->referral_code;
        queue.TryEnqueue([weak, failed, code] {
          auto self = weak.get();
          if (!self) return;
          auto& page = self->settings();
          if (failed) {
            page.referralCode_.clear();
            page.referralCodeCopy_.IsEnabled(false);
            ApplyFieldState(page.referralCodeValue_, FieldState::Failed);
            return;
          }
          page.referralCode_ = code;
          ApplyValue(page.referralCodeValue_, code);
          page.referralCodeCopy_.IsEnabled(!code.empty());
        });
      });

  Sdk().api().getReferralNetwork([queue, weak](
                                     std::optional<urnet::GetReferralNetworkResult> result,
                                     std::optional<std::string> err) {
    // "No referral network found" is how the server says NONE - it answers on
    // the error channel of a lookup that succeeded. Verified against the beta
    // network: an account with no referral network returns exactly that, and
    // rendering it as "Something went wrong." was wrong on screen. Only a
    // TRANSPORT failure (err) is a real failure here; a structured response,
    // error or not, means the server answered.
    const bool failed = !result || err.has_value();
    if (failed) {
      LogWarn("settings: getReferralNetwork failed: {}", err ? *err : std::string("no result"));
    }
    std::string name;
    if (!failed && result->network) name = result->network->name;
    queue.TryEnqueue([weak, failed, name] {
      auto self = weak.get();
      if (!self) return;
      auto& field = self->settings().referralNetworkValue_;
      if (failed) {
        ApplyFieldState(field, FieldState::Failed);
        return;
      }
      ApplyValue(field, name);  // empty -> "None", which is the real answer
    });
  });
}

void SettingsPage::LoadPreferences() {
  ApplyFieldState(productUpdatesState_, FieldState::Loading);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().accountPreferencesGet(
      [queue, weak](std::optional<urnet::AccountPreferencesGetResult> result,
                    std::optional<std::string> err) {
        const bool failed = !result || err.has_value();
        if (failed) {
          // The toggle stays disabled, which is honest - we do not know what
          // the server holds, so offering to change it would be a guess - but
          // it now SAYS that rather than looking like it is still loading.
          LogWarn("settings: accountPreferencesGet failed: {}", err ? *err : std::string());
          queue.TryEnqueue([weak] {
            auto self = weak.get();
            if (!self) return;
            ApplyFieldState(self->settings().productUpdatesState_, FieldState::Failed);
          });
          return;
        }
        const bool allow = result->product_updates && *result->product_updates;
        queue.TryEnqueue([weak, allow] {
          auto self = weak.get();
          if (!self) return;
          auto& page = self->settings();
          // Write the loaded value WITHOUT letting the Toggled echo post it
          // straight back (apple AccountPreferencesViewModel's three-flag
          // suppression, minus the flag we do not need).
          page.applyingPreference_ = true;
          page.productUpdates_.IsOn(allow);
          page.applyingPreference_ = false;
          page.preferencesLoaded_ = true;
          page.productUpdates_.IsEnabled(true);
          page.productUpdatesState_.Text(L"");  // the toggle itself is the state now
        });
      });
}

void SettingsPage::RenderAuthMethods(rows::FieldState state) {
  authMethodsPanel_.Children().Clear();
  if (state != FieldState::Loaded || authTypes_.empty()) {
    // Loading / NoSession / Empty / Failed all get a line that SAYS which one
    // it is. Before this, every one of them rendered "Loading..." forever.
    TextBlock note;
    note.FontSize(12);
    note.TextWrapping(TextWrapping::Wrap);
    ApplyFieldState(note, state == FieldState::Loaded ? FieldState::Empty : state);
    authMethodsPanel_.Children().Append(note);
    return;
  }
  for (auto const& authType : authTypes_) {
    Button remove;
    remove.Content(winrt::box_value(Loc("remove")));
    remove.Foreground(colors::DangerBrush());
    // A modal confirm, as apple's SettingsView does it - NOT the two-click arm
    // this used to be. That arm had three faults at once: a double-click armed
    // and committed in a single gesture, it never disarmed, and it offered no
    // way to back out once armed. Removing your only sign-in method locks you
    // out of the network permanently, so the confirmation has to be a distinct
    // deliberate act with an explicit Cancel.
    remove.Click([this, authType](auto const&, auto const&) { ConfirmRemoveAuth(authType); });
    Row(authMethodsPanel_, winrt::to_hstring(AuthMethodLabel(authType)), hstring{}, remove);
  }
}

// apple SettingsView's confirmationDialog: names the method, defaults to
// Cancel, and commits only on the destructive button.
winrt::fire_and_forget SettingsPage::ConfirmRemoveAuth(std::string authType) {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    auto dialog = rows::MakeSheet(self->Content().XamlRoot(), Loc("site_app_login_methods"));
    dialog.PrimaryButtonText(Loc("remove"));
    dialog.CloseButtonText(Loc("cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);  // Enter must not remove
    // WHICH method is going. The auth type is server data, not a UI string.
    TextBlock body;
    body.Text(winrt::to_hstring(AuthMethodLabel(authType)));
    body.FontSize(14);
    body.TextWrapping(TextWrapping::Wrap);
    body.MinWidth(320);
    dialog.Content(body);
    if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
      RemoveAuth(authType);
    }
  } catch (...) {
  }
  w_.SetSheetOpen(false);
}

// ---- actions ---------------------------------------------------------------

void SettingsPage::OnKillSwitchToggled() {
  if (applyingKillSwitch_) return;  // the load wrote it; do not echo it back
  const bool wanted = killSwitch_.IsOn();
  const bool applied = Sdk().SetKillSwitch(wanted);
  // Read it BACK rather than trusting the write. The product-updates toggle
  // already reverts and says so on failure; this is the toggle where a wrong
  // state costs privacy rather than an email, so it gets the stronger check -
  // the value the SDK actually holds now, not the return code alone.
  const bool actual = Sdk().CurrentKillSwitch();
  if (applied && actual == wanted) return;
  LogWarn("settings: kill switch did not apply (wanted={} actual={})", wanted, actual);
  applyingKillSwitch_ = true;
  killSwitch_.IsOn(actual);
  applyingKillSwitch_ = false;
  snackbar_.Show(Loc("something_went_wrong"), InfoBarSeverity::Error);
}

void SettingsPage::OnProductUpdatesToggled() {
  if (applyingPreference_ || !preferencesLoaded_) return;
  const bool allow = productUpdates_.IsOn();
  productUpdates_.IsEnabled(false);

  urnet::AccountPreferencesSetArgs args;
  args.product_updates = allow;
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().accountPreferencesUpdate(
      args, [queue, weak, allow](std::optional<urnet::AccountPreferencesSetResult> result,
                                 std::optional<std::string> err) {
        // AccountPreferencesSetResult is empty, so a result with no transport
        // error is the whole success test.
        const bool ok = !err && result.has_value();
        queue.TryEnqueue([weak, ok, allow] {
          auto self = weak.get();
          if (!self) return;
          auto& page = self->settings();
          page.productUpdates_.IsEnabled(true);
          if (ok) return;
          // Snap back to what the server still holds, and SAY WHY: a toggle
          // that silently returns to its old position reads as a broken
          // control rather than as a failed write.
          page.applyingPreference_ = true;
          page.productUpdates_.IsOn(!allow);
          page.applyingPreference_ = false;
          page.settingsSnackbar().Show(Loc("couldnt_update_preferences"),
                                       InfoBarSeverity::Error);
        });
      });
}

void SettingsPage::RemoveAuth(std::string const& authType) {
  if (!Sdk().IsLoggedIn()) return;
  urnet::RemoveAuthArgs args;
  args.auth_type = authType;
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().removeAuth(args, [queue, weak](std::optional<urnet::RemoveAuthResult> result,
                                             std::optional<std::string> err) {
    std::string error;
    if (result && result->error) error = result->error->message;
    else if (err) error = *err;
    queue.TryEnqueue([weak, error] {
      auto self = weak.get();
      if (!self) return;
      auto& page = self->settings();
      if (!error.empty()) {
        // apple assigns this to a property nothing renders; here it is said out
        // loud, because "the row is still there" is not a diagnosis.
        page.settingsSnackbar().Show(winrt::to_hstring(error), InfoBarSeverity::Error);
      }
      page.LoadNetworkUser();  // re-read the list either way
    });
  });
}

winrt::fire_and_forget SettingsPage::OpenCustomerPortal() {
  auto self = w_.get_strong();  // keep the window alive across the call
  if (!Sdk().IsLoggedIn()) {
    // co_return alone made this row a control that swallowed the click and
    // said nothing at all.
    snackbar_.Show(Loc("please_login_to_urnetwork"), InfoBarSeverity::Warning);
    co_return;
  }
  manageSubscription_.IsEnabled(false);

  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  urnet::StripeCreateCustomerPortalArgs args;
  Sdk().api().stripeCreateCustomerPortal(
      args, [queue, weak](std::optional<urnet::StripeCreateCustomerPortalResult> result,
                          std::optional<std::string> err) {
        std::string url, error;
        if (result && result->url) url = *result->url;
        if (result && result->error) error = result->error->message;
        else if (err) error = *err;
        queue.TryEnqueue([weak, url, error] {
          auto window = weak.get();
          if (!window) return;
          auto& page = window->settings();
          page.manageSubscription_.IsEnabled(true);
          if (!url.empty()) {
            OpenUrl(urnw::Widen(url));
            return;
          }
          page.settingsSnackbar().Show(
              error.empty() ? Loc("something_went_wrong") : winrt::to_hstring(error),
              InfoBarSeverity::Error);
        });
      });
  co_return;
}

winrt::fire_and_forget SettingsPage::SaveLogsToFile() {
  auto self = w_.get_strong();
  const auto source = urnw::LogFilePath();
  if (source.empty() || !std::filesystem::exists(source)) {
    snackbar_.Show(Loc("no_log_files_found"), InfoBarSeverity::Warning);
    co_return;
  }
  try {
    winrt::Windows::Storage::Pickers::FileSavePicker picker;
    // A WinUI 3 desktop picker has no implicit owner window; without this it
    // throws E_ACCESSDENIED rather than opening.
    HWND hwnd{};
    if (auto native = self.try_as<::IWindowNative>()) native->get_WindowHandle(&hwnd);
    if (hwnd) picker.as<::IInitializeWithWindow>()->Initialize(hwnd);
    picker.SuggestedFileName(hstring{source.stem().wstring()});
    auto types = winrt::single_threaded_vector<hstring>({L".log"});
    picker.FileTypeChoices().Insert(Loc("export_logs"), types);

    auto file = co_await picker.PickSaveFileAsync();
    if (!file) co_return;  // cancelled; not a failure
    std::filesystem::copy_file(source, std::filesystem::path{std::wstring{file.Path()}},
                               std::filesystem::copy_options::overwrite_existing);
    snackbar_.Show(Loc("save_logs"), InfoBarSeverity::Success);
  } catch (const std::exception& e) {
    LogWarn("settings: save logs failed: {}", e.what());
    snackbar_.Show(Loc("something_went_wrong"), InfoBarSeverity::Error);
  } catch (...) {
    LogWarn("settings: save logs failed");
    snackbar_.Show(Loc("something_went_wrong"), InfoBarSeverity::Error);
  }
}

// Attach the SDK's log directory to a feedback report the server has already
// accepted, identified by ITS id. Called only from OnSendFeedback, only when
// the user ticked the box - apple's FeedbackView contract. Never a standalone
// affordance: an upload the user did not ask for, correlated with nothing, is
// exfiltration with a friendly label.
//
// Failure is silent BY DESIGN here and only here: the feedback itself was
// accepted, so telling the user their report failed would be false, and the
// attachment is an extra. It is logged.
void SettingsPage::UploadLogs(std::string const& feedbackId) {
  if (feedbackId.empty() || !Sdk().hasDevice()) {
    LogWarn("settings: log attach skipped (feedbackId={} device={})",
            feedbackId.empty() ? "none" : "present", Sdk().hasDevice());
    return;
  }
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  try {
    Sdk().device().uploadLogs(feedbackId,
                              [queue, weak](std::optional<urnet::UploadLogsResult> result,
                                            std::optional<std::string> err) {
                                std::string error;
                                if (result && result->error) error = result->error->message;
                                else if (err) error = *err;
                                if (!error.empty()) {
                                  LogWarn("settings: log attach failed: {}", error);
                                }
                              });
  } catch (const std::exception& e) {
    // Device::uploadLogs throws synchronously when the C call fails.
    LogWarn("settings: log attach threw: {}", e.what());
  }
}

// ---- sheets ----------------------------------------------------------------

winrt::fire_and_forget SettingsPage::ShowDeviceNameSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    auto weak = w_.get_weak();
    deviceNameSheet_ = urnw::DeviceNameSheet::Create(
        self->Content().XamlRoot(), Sdk(), deviceName_, [weak](std::string name) {
          auto window = weak.get();
          if (!window) return;
          auto& page = window->settings();
          page.deviceName_ = name;
          ApplyFieldState(page.deviceNameValue_, FieldState::Loaded,
                          winrt::to_hstring(name));
          page.settingsSnackbar().Show(Loc("device_name_updated"), InfoBarSeverity::Success);
        });
    co_await deviceNameSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  deviceNameSheet_.reset();
  w_.SetSheetOpen(false);
}

winrt::fire_and_forget SettingsPage::ShowAuthCodeSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    authCodeSheet_ = urnw::AuthCodeSheet::Create(self->Content().XamlRoot(), Sdk());
    co_await authCodeSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  authCodeSheet_.reset();
  w_.SetSheetOpen(false);
}

winrt::fire_and_forget SettingsPage::ShowAddAuthSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    auto weak = w_.get_weak();
    addAuthSheet_ = urnw::AddAuthSheet::Create(self->Content().XamlRoot(), Sdk(), [weak] {
      if (auto window = weak.get()) window->settings().LoadNetworkUser();
    });
    co_await addAuthSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  addAuthSheet_.reset();
  w_.SetSheetOpen(false);
}

winrt::fire_and_forget SettingsPage::ShowReferralNetworkSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    auto weak = w_.get_weak();
    referralSheet_ = urnw::ReferralNetworkSheet::Create(self->Content().XamlRoot(), Sdk(), [weak] {
      if (auto window = weak.get()) window->settings().LoadReferral();
    });
    co_await referralSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  referralSheet_.reset();
  w_.SetSheetOpen(false);
}

winrt::fire_and_forget SettingsPage::ShowBlockedLocationsSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    blockedSheet_ = urnw::BlockedLocationsSheet::Create(self->Content().XamlRoot(), Sdk());
    co_await blockedSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  blockedSheet_.reset();
  w_.SetSheetOpen(false);
}

winrt::fire_and_forget SettingsPage::ShowIdentitySheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    identitySheet_ = urnw::PostQuantumIdentitySheet::Create(self->Content().XamlRoot(), Sdk());
    co_await identitySheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  identitySheet_.reset();
  w_.SetSheetOpen(false);
}

winrt::fire_and_forget SettingsPage::ShowDeleteAccountSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    // No cached name is handed over: the sheet reads the CURRENT session's own
    // name and refuses to arm without it (SettingsSheets.h).
    deleteSheet_ = urnw::DeleteAccountSheet::Create(self->Content().XamlRoot(), Sdk());
    co_await deleteSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  deleteSheet_.reset();
  w_.SetSheetOpen(false);
}

// ---- split tunnel ----------------------------------------------------------

void SettingsPage::OnManageAppSplitTunnel(IInspectable const&, RoutedEventArgs const&) {
  ShowAppRulesSheet();
}

winrt::fire_and_forget SettingsPage::ShowAppRulesSheet() {
  if (w_.sheetOpen()) co_return;  // only one ContentDialog can show at a time
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    appRulesSheet_ = urnw::AppRulesSheet::Create(self->Content().XamlRoot(), Sdk());
    co_await appRulesSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  appRulesSheet_.reset();
  w_.SetSheetOpen(false);
}

// ---- account ---------------------------------------------------------------

void SettingsPage::OnSignOut(IInspectable const&, RoutedEventArgs const&) {
  Sdk().Logout();
}

void SettingsPage::ShowPreviewSnackbar() {
  snackbar_.Show(Loc("thanks_for_the_feedback"), InfoBarSeverity::Success);
}

// ---- support ---------------------------------------------------------------

void SettingsPage::OnSendFeedback(IInspectable const&, RoutedEventArgs const&) {
  // This had NO session guard at all and reported success unconditionally: a
  // 401 rendered as "Thanks for the feedback!" while nothing had been sent.
  if (!Sdk().IsLoggedIn()) {
    snackbar_.Show(Loc("please_login_to_urnetwork"), InfoBarSeverity::Warning);
    return;
  }
  urnet::FeedbackSendArgs args;
  args.star_count = static_cast<int64_t>(w_.FeedbackRating().Value());
  const std::string text = urnw::Narrow(w_.FeedbackText().Text().c_str());
  if (!text.empty()) {
    urnet::FeedbackSendNeeds needs;
    needs.other = text;
    args.needs = needs;
  }
  const bool attachLogs =
      w_.FeedbackIncludeLogs().IsChecked() && w_.FeedbackIncludeLogs().IsChecked().Value();

  w_.SendFeedbackButton().IsEnabled(false);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().sendFeedback(
      args, [queue, weak, attachLogs](std::optional<urnet::FeedbackSendResult> result,
                                      std::optional<std::string> err) {
        // FeedbackSendResult carries NO error field - only feedback_id - so a
        // result plus no transport error is the whole success test. Reporting
        // success unconditionally, as this used to, turned a 401 into "Thanks
        // for the feedback!".
        const std::string error = err ? *err : std::string();
        const bool ok = error.empty() && result.has_value();
        if (!ok) LogWarn("settings: sendFeedback failed: {}", error);
        // The SERVER's feedback id is what ties an upload to the report support
        // will actually read. A client-minted id correlates with nothing.
        std::string feedbackId;
        if (ok && result->feedback_id) feedbackId = *result->feedback_id;
        queue.TryEnqueue([weak, ok, error, attachLogs, feedbackId] {
          auto self = weak.get();
          if (!self) return;
          auto& page = self->settings();
          self->SendFeedbackButton().IsEnabled(true);
          if (!ok) {
            page.settingsSnackbar().Show(
                error.empty() ? Loc("error_sending_feedback") : winrt::to_hstring(error),
                InfoBarSeverity::Error);
            return;
          }
          page.settingsSnackbar().Show(Loc("thanks_for_the_feedback"),
                                       InfoBarSeverity::Success);
          self->FeedbackText().Text(L"");
          self->FeedbackIncludeLogs().IsChecked(false);
          if (attachLogs && !feedbackId.empty()) page.UploadLogs(feedbackId);
        });
      });
}

}  // namespace urnw
