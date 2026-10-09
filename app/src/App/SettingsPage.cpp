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
#include "ClientEvents.h"
#include "CloudProxyLink.h"
#include "FeedbackSendState.h"
#include "Ids.h"
#include "LaunchAtStartup.h"
#include "Localization.h"
#include "Log.h"
#include "MainWindow.xaml.h"
#include "ManageSubscription.h"
#include "PageContext.h"
#include "PaymentRefusal.h"
#include "Strings.h"
#include "SupportContact.h"
#include "UpdateChecker.h"
#include "UrColors.h"
#include "Version.h"

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

// The store key a label SHOULD have, with the English it falls back to until
// that key lands. This is DeveloperPage's Dev() idiom, verbatim and for the same
// reason: Localized() returns the key id itself on a miss, so the fallback is
// taken exactly when the store has nothing, and the screen localizes with no
// code change the moment @urnetwork/localizations grows the key.
//
// Used for two section titles and one About row label. 945 keys ship and none
// of "Advanced", "About" or "App version" is among them, and all are surfaces
// the reconciled IA names explicitly. They extract mechanically:
//
//     grep -oE 'Missing\("[a-z0-9_]+"' SettingsPage.cpp | sort -u
hstring Missing(std::string_view key, const wchar_t* english) {
  std::wstring value = urnw::Localized(key);
  if (value == urnw::Widen(key)) return hstring{english};
  return hstring{value};
}

}  // namespace

SettingsPage::SettingsPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window), snackbar_(window.SupportInfo(), window.DispatcherQueue()) {}

void SettingsPage::ApplyStrings() {
  BuildSections();  // idempotent

  // support: two pane headers, and a landmark name each so a screen reader
  // can tell the form from the way to reach a human
  w_.SupportPaneATitle().Text(Loc("feedback"));
  w_.SupportPaneBTitle().Text(Loc("support"));
  // Why the screen exists, in one sentence. Already in the store, used nowhere
  // until this destination: the panel opened on five bare controls and no
  // sentence.
  w_.SupportIntroText().Text(Loc("site_app_support_intro"));
  w_.FeedbackRating().Caption(Loc("how_are_we_doing"));
  w_.FeedbackText().Header(LocBox("anything_else"));
  // The box shipped with no content whatsoever - an unlabelled tick offering to
  // upload the user's logs. The string existed the whole time.
  w_.FeedbackIncludeLogs().Content(LocBox("feedback_include_logs"));
  // The primary action's label ("Send", or "Sending…" while the request is out;
  // ApplyFeedbackSendButton owns both): the one way to submit this form is
  // never nameless to a screen reader.
  ApplyFeedbackSendButton();
  Automation::AutomationProperties::SetName(w_.SupportPaneA(), Loc("feedback"));
  Automation::AutomationProperties::SetName(w_.SupportPaneB(), Loc("support"));

  // settings: three pane headers, and a landmark name each so a screen reader
  // can tell the three regions apart. About folds first (1400dip of window);
  // its rows keep a second door at the foot of General (the fold hosts in
  // BuildSections), which is what the first About pane lacked.
  w_.SettingsPaneATitle().Text(Loc("general"));
  w_.SettingsPaneBTitle().Text(Loc("device"));
  w_.SettingsPaneCTitle().Text(Missing("about", L"About"));
  Automation::AutomationProperties::SetName(w_.SettingsPaneA(), Loc("general"));
  Automation::AutomationProperties::SetName(w_.SettingsPaneB(), Loc("device"));
  Automation::AutomationProperties::SetName(w_.SettingsPaneC(), Missing("about", L"About"));

  // The heading over the destructive end. It sits on ACCOUNT now (the rows under
  // it are Sign out and Delete account), which is why it is painted from here
  // rather than from AccountPage: this class still owns those rows.
  w_.SettingsAccountHeading().Text(Loc("account"));
}

// ---- section construction --------------------------------------------------

void SettingsPage::BuildSections() {
  if (built_) return;
  built_ = true;

  // R4 / spec override #2: THE ACCOUNT-SUBJECT SECTIONS ARE BUILT ONTO ACCOUNT.
  //
  // Login methods, the auth code, the client id, the bonus referral code, the
  // referral network, Manage Subscription and Delete account are not settings -
  // they are the account - and the reconciled IA moves them to that destination.
  // What did NOT move is this class: it owns their sheets, their loads, their
  // echo guards and their FieldState wiring, and re-homing four hundred lines of
  // that into AccountPage would have been a rewrite dressed up as a move. So the
  // builders stay here and the HOSTS they build into are Account's.
  //
  // Two consequences worth knowing:
  //   * LoadSettings() is what fills these rows, so MainWindow's navigation relay
  //     now calls it for the ACCOUNT destination as well as for Settings.
  //   * ResetForSignOut() still clears them from here, which is correct - they
  //     are still this object's state.
  //
  // Pane mode (rows::SetPaneMode) makes the same builder calls emit the pane
  // vocabulary instead of cards. It is set around the whole build because
  // Account is a pane shell; the Settings sections below are switched back for
  // now and follow in their own change.
  rows::SetPaneMode(true);
  BuildSecuritySection(w_.AccountSecurityHost());
  BuildReferralSection(w_.ReferralsNetworkHost());
  BuildSubscriptionSection(w_.AccountPlanExtraHost());
  BuildDangerSection();
  rows::SetPaneMode(false);

  // ---- what stays on Settings: preferences, in three panes ----------------
  // col 0 what the app DOES, col 1 what this machine IS, col 2 what the app is.
  // Each pane is one constrained column of rows, which is how the Windows
  // single-column settings guidance and the full-bleed pane model reconcile:
  // three columns of ~660dip in the 2062dip window this app is judged in.
  //
  // About folds first (1400dip of window) and Device below 900
  // (MainWindow::ApplyBreakpoint's settingsThree/settingsTwo gates), and the
  // fold rule bars a foldable pane from owning content with no second door:
  // About owns the version/update rows, Device the Advanced-mode toggle (with
  // Advanced OFF at a narrow window there was no way to turn it on). So those
  // rows build twice, the BuildSupportContactSection pattern one destination
  // over: once into their own pane above, once into the fold hosts at the foot
  // of General, and the gates show exactly one copy of each -
  // ApplyAboutPaneVisible for the version copy (which joins the Licenses row's
  // fold copy under its About strip, so the folded foot of General reads as
  // one About block), ApplyPaneBFolded for the Advanced toggle.
  rows::SetPaneMode(true);
  auto general = w_.SettingsSections();
  auto device = w_.SettingsSectionsRight();
  auto about = w_.SettingsAboutHost();
  BuildGeneralSection(general);
  BuildConnectionsSection(general);
  BuildDeviceSection(device);
  BuildIdentitySection(device);
  BuildAdvancedSection(device);
  BuildVersionSection(about);
  BuildLicensesRows(about, general);
  BuildStayInTouchSection(about);
  versionFoldHost_ = StackPanel();
  versionFoldHost_.Visibility(aboutPaneVisible_ ? Visibility::Collapsed
                                                : Visibility::Visible);
  general.Children().Append(versionFoldHost_);
  BuildVersionFoldSection(versionFoldHost_);
  paneBFoldHost_ = StackPanel();
  paneBFoldHost_.Visibility(paneBFolded_ ? Visibility::Visible : Visibility::Collapsed);
  general.Children().Append(paneBFoldHost_);
  BuildAdvancedFoldSection(paneBFoldHost_);
  rows::SetPaneMode(false);

  // ---- Support: the way to reach a human, in BOTH its homes ----------------
  // The same section is built twice: into pane B for the two-pane widths, and
  // into SupportContactInline (under the Send button) for the folded one. The
  // breakpoint shows exactly one of the hosts, so only one copy is ever on
  // screen; the inline copy carries the group header because there is no pane
  // header strip naming it there.
  rows::SetPaneMode(true);
  BuildSupportContactSection(w_.SupportContactHost(), false);
  BuildSupportContactSection(w_.SupportContactInline(), true);
  rows::SetPaneMode(false);

  // Everything that can be read without a round trip, so the page is not blank
  // before (or without) a load: the client id and the persisted kill switch.
  ApplyLocalDeviceState();
  // urnet::version() is EMPTY in this SDK build (the "sdk initialized:
  // version=" startup line shows it too), so the app version is what actually
  // identifies the build here.
  const std::string sdkVersion = urnet::version();
  ApplyValue(versionValue_, sdkVersion.empty() ? Sdk().appVersion() : sdkVersion);
  // ...and the fold-gated copy (BuildVersionFoldSection): one value, both rows.
  ApplyValue(versionValueFold_, sdkVersion.empty() ? Sdk().appVersion() : sdkVersion);
}

// HOW THE ACCOUNT SIGNS IN. Built onto Account's pane B (spec override #2):
// login methods, the short-lived auth code, and the client id support asks for.
void SettingsPage::BuildSecuritySection(Panel const& host) {
  // "Secure Your Account" is the shipped string closest to a Security heading;
  // the store has no "Security" key. Reported as a needed addition.
  Heading(host, Loc("secure_your_account"), L"");
  auto card = Card(host);

  // Sign-in methods FIRST: it is the section's subject, and it was buried under
  // three other rows when this lived on Settings.
  authMethodsPanel_ = StackPanel();
  card.Children().Append(authMethodsPanel_);
  auto addAuth = ButtonRow(card, Loc("site_app_login_methods"), hstring{}, Loc("add"));
  addAuth.Click([this](auto const&, auto const&) { ShowAddAuthSheet(); });
  // Nothing has been asked for yet; LoadSettings moves this on.
  RenderAuthMethods(FieldState::NoSession);

  // Auth code - a short-lived credential for signing in on another device.
  auto authCodeButton = ButtonRow(card, Loc("auth_code"),
                                  Loc("created_auth_codes_expire_after_5_minutes"),
                                  Loc("site_app_create_auth_code"));
  authCodeButton.Click([this](auto const&, auto const&) { ShowAuthCodeSheet(); });

  // Client ID - the identifier support asks for; copy is the only action.
  clientIdValue_ = ValueActionRow(card, Loc("client_id"), Loc("copy"), clientIdCopy_);
  clientIdCopy_.IsEnabled(false);
  clientIdCopy_.Click([this](auto const&, auto const&) {
    if (clientId_.empty()) return;
    CopyToClipboard(clientId_);
    snackbar_.Show(Loc("client_id_copied_to_clipboard"), InfoBarSeverity::Success);
  });
}

// WHO REFERRED WHOM. Built onto the Refer and earn page (ReferralsView), under
// the shared referral card and the figures ReferralsPage builds: this class
// still owns the referral-network sheet, its load and its FieldState wiring.
// The bonus code (with copy and share) is part of the card now.
void SettingsPage::BuildReferralSection(Panel const& host) {
  auto card = Card(host);

  // Referral network - who referred THIS network, editable in a sheet.
  auto referralButton = NavRow(card, Loc("referral_network"), referralNetworkValue_);
  referralButton.Click([this](auto const&, auto const&) { ShowReferralNetworkSheet(); });

  // The row starts in the state that says WHY it is empty. Without this it
  // rendered as a blank cell before any load ran - the exact "is this empty,
  // loading, or broken?" ambiguity FieldState exists to remove, and it was
  // visible on screen because --preview-ui never calls LoadSettings.
  ApplyFieldState(referralNetworkValue_, FieldState::NoSession);
}

void SettingsPage::BuildDeviceSection(Panel const& host) {
  // No group header: the pane header strip above already carries this word, and
  // a 28px strip repeating it read as a stutter - the same reason the
  // leaderboard has no panel heading under its destination title.
  auto card = Card(host);
  auto nameButton = NavRow(card, Loc("device_name_label"), deviceNameValue_);
  nameButton.Click([this](auto const&, auto const&) { ShowDeviceNameSheet(); });
  // Spec is server-assigned and read-only, exactly as on macOS.
  deviceSpecValue_ = ValueRow(card, Loc("device_spec_label"));
  ApplyFieldState(deviceNameValue_, FieldState::NoSession);
  ApplyFieldState(deviceSpecValue_, FieldState::NoSession);
}

// GENERAL. One preference ships today - whether the account wants product mail -
// and it IS a preference, so it opens the settings destination instead of
// sitting at the bottom of a community card as it used to.
void SettingsPage::BuildGeneralSection(Panel const& host) {
  // No group header: the pane header strip above already carries this word, and
  // a 28px strip repeating it read as a stutter - the same reason the
  // leaderboard has no panel heading under its destination title.
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
  {
    Border box;
    box.Padding(ThicknessHelper::FromLengths(12, 8, 12, 8));
    box.BorderBrush(colors::BorderBrush());
    box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
    box.Child(productUpdatesState_);
    card.Children().Append(box);
  }
  ApplyFieldState(productUpdatesState_, FieldState::NoSession);

  // Check for updates automatically (beta spec §5). A LOCAL preference, unlike
  // the account row above it: persisted in app_prefs.json beside Advanced Mode
  // — it describes this installation, not the account — which is why it needs
  // no session, no FieldState and no server round-trip. Default ON; turning it
  // on also fires a check right away (see SetAutoCheckEnabled). The labels are
  // Adv() ids like every update-surface string.
  autoUpdateCheck_ = ToggleRow(
      card, Adv("upd_auto_check", L"Check for updates automatically"),
      Adv("upd_auto_check_note",
          L"Look for new releases shortly after launch and every six hours. "
          L"Nothing is ever installed without a click."));
  autoUpdateCheck_.IsOn(urnw::UpdateChecker::AutoCheckEnabled());
  autoUpdateCheck_.Toggled([this](auto const&, auto const&) {
    urnw::pages::Updates().SetAutoCheckEnabled(autoUpdateCheck_.IsOn());
  });

  // Launch URnetwork on system startup, as on macOS (owner decision,
  // 2026-10-05): this installation's own registration with Windows, so it
  // needs no session. Off until the user turns it on; a sign-in then starts
  // the app in the tray (StartupRegistration.h). The value is Windows's, read
  // again whenever the page loads, since Task Manager can switch it off too.
  launchAtStartup_ = ToggleRow(card, Loc("launch_urnetwork_on_system_startup"), hstring{});
  ApplyLaunchAtStartup();
  launchAtStartup_.Toggled([this](auto const&, auto const&) { OnLaunchAtStartupToggled(); });
}

// ADVANCED. The home the Advanced Mode toggle drops into, and export logs.
// The ONE apply path: MainWindow publishes the standing value here, whether it
// came from the toggle below or was restored from disk before any view existed.
// applyingAdvancedMode_ stops the resulting IsOn write from echoing back out as
// a user edit.
void SettingsPage::ApplyAdvancedMode(bool on) {
  if (!advancedMode_) return;  // the section is not built yet
  if (advancedMode_.IsOn() == on) return;
  applyingAdvancedMode_ = true;
  advancedMode_.IsOn(on);
  // ...and the fold-gated copy (BuildAdvancedFoldSection) under the SAME echo
  // guard: the one apply path writes both instances, so the two toggles can
  // never read differently.
  if (advancedModeFold_) advancedModeFold_.IsOn(on);
  applyingAdvancedMode_ = false;
}

void SettingsPage::ApplyPaneBFolded(bool folded) {
  paneBFolded_ = folded;
  if (!paneBFoldHost_) return;  // not built yet; BuildSections replays the state
  paneBFoldHost_.Visibility(folded ? Visibility::Visible : Visibility::Collapsed);
}

// The Advanced-mode toggle's SECOND door (BuildAdvancedSection is the primary;
// its comment carries the one-apply-path rule, which this copy follows
// verbatim: the toggle only writes through SdkHost, and MainWindow's apply
// path writes BOTH toggles back under the one echo guard). Export logs stays
// pane-B-only - a fold hides a convenience, not a capability, and the fold
// rule's doors are for what has no other way in.
void SettingsPage::BuildAdvancedFoldSection(Panel const& host) {
  Heading(host, Missing("advanced", L"Advanced"), hstring{});
  auto card = Card(host);
  advancedModeFold_ = ToggleRow(
      card, Adv("adv_advanced_mode", L"Advanced mode"),
      Adv("adv_advanced_mode_note",
          L"Show raw values, identifiers, the connection inspector and the "
          L"reliability tuning surface across the app."));
  advancedModeFold_.IsOn(Sdk().CurrentAdvancedMode());
  advancedModeFold_.Toggled([this](auto const&, auto const&) {
    if (applyingAdvancedMode_) return;  // the apply path wrote it; do not echo back
    w_.SetAdvancedMode(advancedModeFold_.IsOn());
  });
}

// The version/update rows' SECOND door (BuildVersionSection is the primary,
// and its comment carries the why and the replay contract). The About-pane
// copy needs no group header - its pane header strip names it - and this fold
// copy needs none either: it renders directly under the Licenses row's fold
// copy, whose About strip (BuildLicensesRows) heads the foot of General
// whenever the pane is folded, so the two read as one About block.
void SettingsPage::BuildVersionFoldSection(Panel const& host) {
  auto card = Card(host);
  versionValueFold_ = ValueRow(card, Loc("version_info"));
  // the build's own stamp (Common/Version.h), verbatim - see the primary
  ApplyValue(ValueRow(card, Missing("app_version", L"App version")),
             urnw::version::kString);
  updateStateValueFold_ = ValueRow(card, Loc("update"));
  auto checkNow = ButtonRow(
      card, Loc("dev_check_updates"),
      Adv("upd_manual_note",
          L"Runs the release check now; the outcome lands on the row above."),
      Adv("upd_check_now", L"Check now"));
  checkNow.Click([](auto const&, auto const&) { urnw::pages::Updates().CheckNow(); });
  // Replay the standing state into the row just built: the primary's copy of
  // this call ran while this row did not exist (the fold section builds after
  // pane B's), and ApplyUpdateCheck writes BOTH instances - so this is also
  // what keeps the two rows on one outcome.
  ApplyUpdateCheck(urnw::pages::Updates().Current());
}

void SettingsPage::BuildAdvancedSection(Panel const& host) {
  Heading(host, Missing("advanced", L"Advanced"), hstring{});
  auto card = Card(host);

  // ADVANCED MODE. First in the Advanced group, above Export logs, because
  // everything the flag reveals hangs off it — including the Developer
  // destination, which folds behind it rather than staying a rail item.
  //
  // Merged here from D5: R4 rebuilt this page into panes and left this host
  // empty deliberately (a row wearing an invented English label is worse than
  // no row), while D5 built the row against the pre-pane Settings. The label
  // goes through Adv(), the same fallback the developer surface established —
  // key id in source, English until the key lands upstream — which is what
  // makes the row honest rather than invented.
  advancedModeHost_ = StackPanel();
  card.Children().Append(advancedModeHost_);

  // The toggle does NOT apply the mode itself. It persists through SdkHost,
  // which publishes back through the handler MainWindow bound in its
  // constructor, which lands in ApplyAdvancedMode — the same path a value
  // restored from disk takes. One apply path, so "set it in Settings" and "it
  // was already on at launch" cannot produce two different screens.
  advancedMode_ = ToggleRow(
      advancedModeHost_, Adv("adv_advanced_mode", L"Advanced mode"),
      Adv("adv_advanced_mode_note",
          L"Show raw values, identifiers, the connection inspector and the "
          L"reliability tuning surface across the app."));
  advancedMode_.IsOn(Sdk().CurrentAdvancedMode());
  advancedMode_.Toggled([this](auto const&, auto const&) {
    if (applyingAdvancedMode_) return;  // the apply path wrote it; do not echo back
    w_.SetAdvancedMode(advancedMode_.IsOn());
  });

  // Saving to a file the user picks is the ONLY log affordance here.
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
  auto save = ButtonRow(card, Loc("save_logs"), Loc("export_logs"), Loc("save"));
  save.Click([this](auto const&, auto const&) { SaveLogsToFile(); });
}

void SettingsPage::BuildConnectionsSection(Panel const& host) {
  Heading(host, Loc("site_app_connections"), hstring{});
  auto card = Card(host);

  // Kill switch. The shipped note — "Block browser traffic when URnetwork is
  // disconnected" (site_app_kill_switch_note) — is wrong twice, and the second
  // way is what the owner ran into. It is not browser-only: it is a machine-wide
  // WFP policy. And "when disconnected" is the exact reading under which a
  // machine left blocked after pressing Disconnect looks CORRECT — it is the
  // sentence that made a plain bug read as a feature. What this guards is an
  // UNEXPECTED loss, and nothing else.
  StackPanel killSwitchControls;
  killSwitchControls.Orientation(Orientation::Horizontal);
  killSwitchControls.Spacing(8);

  Button killSwitchInfo;
  killSwitchInfo.Width(28);
  killSwitchInfo.Height(28);
  killSwitchInfo.Padding(ThicknessHelper::FromUniformLength(0));
  killSwitchInfo.Background(nullptr);
  killSwitchInfo.BorderThickness(ThicknessHelper::FromUniformLength(0));
  FontIcon killSwitchInfoGlyph;
  killSwitchInfoGlyph.FontFamily(
      winrt::Microsoft::UI::Xaml::Media::FontFamily(L"Segoe Fluent Icons"));
  killSwitchInfoGlyph.Glyph(L"\uE946");  // Info
  killSwitchInfoGlyph.FontSize(14);
  killSwitchInfoGlyph.Foreground(colors::MutedBrush());
  killSwitchInfo.Content(killSwitchInfoGlyph);
  const auto killSwitchInfoName =
      Adv("show_kill_switch_exception", L"Show kill switch exception");
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      killSwitchInfo, killSwitchInfoName);
  ToolTipService::SetToolTip(killSwitchInfo, winrt::box_value(killSwitchInfoName));
  killSwitchInfo.Click(
      [this](auto const&, auto const&) { ShowKillSwitchException(); });
  killSwitchControls.Children().Append(killSwitchInfo);

  killSwitch_ = ToggleSwitch();
  killSwitch_.Style(Lookup(L"UrSwitchToggleStyle"));
  // the insufficient-balance acceptance driver's kill-switch case toggles this
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetAutomationId(
      killSwitch_, L"acceptance.settings.kill-switch");
  killSwitchControls.Children().Append(killSwitch_);
  Row(card, Loc("kill_switch"),
      Adv("adv_kill_switch_note",
          L"If the tunnel drops unexpectedly, block this device's traffic "
          L"instead of letting it out unprotected."),
      killSwitchControls);
  killSwitch_.Toggled([this](auto const&, auto const&) { OnKillSwitchToggled(); });
  // What the toggle does NOT do, said before the disclosure below because it is
  // the question a user reading the word "kill switch" actually has. It is also
  // a promise the service has always kept and the app used not to: a deliberate
  // stop lifts the policy whatever this toggle says
  // (TunnelController::RevertMachineStateLocked's finalDisarm branch). Lockdown
  // — block whenever not connected — is a different product decision with its
  // own separately-worded toggle, and it is deliberately not shipped here.
  Supporting(card, Adv("adv_kill_switch_deliberate",
                       L"Pressing Disconnect always restores your internet "
                       L"straight away. The kill switch only applies to drops "
                       L"you did not ask for - a tunnel that stops carrying "
                       L"traffic, a network change, or the URnetwork service "
                       L"stopping."));
  // The one thing the shipped note does not say, and the one thing that is not
  // true of this feature: "nothing leaves" holds while idle, but NOT during a
  // connection attempt. The service opens a DNS hole to reach our servers, and
  // that hole cannot be scoped to us - Windows performs name lookups in the DNS
  // Client service, so the permit is address-scoped and every app on the device
  // is inside it for the length of the attempt (Service/WfpPolicy.h, filter 9b).
  // Disclosed here rather than left to the log, because a user who reads "block
  // traffic when disconnected" and is not told this has been told something
  // false about the seconds that matter most.
  Supporting(card, Adv("adv_kill_switch_dns_window",
                       L"While it is on and nothing is connecting, nothing "
                       L"leaves this device - name lookups included. During a "
                       L"connection attempt, name lookups from any app on this "
                       L"device can leave in the clear so URnetwork can reach "
                       L"its servers; everything else stays blocked."));

  TextBlock unused{nullptr};
  auto blockedButton = NavRow(card, Loc("blocked_locations_2"), unused);
  blockedButton.Click([this](auto const&, auto const&) { ShowBlockedLocationsSheet(); });

  // VLESS: a server of the user's own that the client strategy also dials
  // through, for networks that block direct connections. A value of the
  // network space rather than of the session, so it needs no device; the login
  // screen's network sheet opens the same sheet before sign-in.
  auto vlessButton = NavRow(card, Loc("vless"), unused);
  vlessButton.Click([this](auto const&, auto const&) { ShowVlessSheet(); });

  // App split rules, in from the loose heading-plus-card-plus-button that used
  // to sit under both settings columns. It is a VPN-and-privacy preference like
  // the two above it, so it is a row like them.
  auto splitButton = ButtonRow(card, Loc("app_split_rules"), Loc("apps_listed_bypass_vpn"),
                               Loc("manage_apps"));
  splitButton.Click(
      [this](auto const& sender, auto const& args) { OnManageAppSplitTunnel(sender, args); });

  // Cloud proxies. The app has no protocol switch, so WireGuard, SOCKS and
  // HTTPS proxies are created on ur.io, and this row opens that page in the
  // browser (CloudProxyLink.h).
  auto proxies = kit::MakePaneTwoLineRowButton(Loc("use_wireguard_socks_https_proxy"),
                                               Loc("use_wireguard_socks_https_proxy_note"));
  proxies.root.Click([this](auto const&, auto const&) { OpenCloudProxies(); });
  card.Children().Append(proxies.root);

  // Uninstall the VPN service (beta spec §3). Last in the group: it is the one
  // machine-level action on a page of preferences. Labels are Adv() ids — the
  // Windows wording of the service row, not linux's systemd one (see the banner
  // in ConnectPage::ApplyServiceSetup) — and the row starts collapsed until a
  // classification proves a service is actually registered; ApplyServiceSetup
  // below is the only writer of that visibility.
  serviceRowHost_ = StackPanel();
  card.Children().Append(serviceRowHost_);
  uninstallServiceButton_ = ButtonRow(
      serviceRowHost_, Adv("svc_service_label", L"VPN service"),
      Adv("svc_uninstall_note_windows",
          L"Remove the Windows service URnetwork uses to carry traffic."),
      Adv("svc_uninstall_action", L"Uninstall"));
  uninstallServiceButton_.Click(
      [this](auto const&, auto const&) { ConfirmUninstallService(); });
  serviceRowHost_.Visibility(Visibility::Collapsed);
}

void SettingsPage::ApplyServiceSetup(urnw::ServiceSetup::Snapshot const& snap) {
  if (!serviceRowHost_) return;  // the section is not built yet
  using State = urnw::ServiceSetup::State;
  const State state = snap.observation.state;
  // Registered in any form — running, stopped, or wearing the wrong version —
  // is the evidence the row needs. Everything else hides it (see the header).
  const bool registered = state == State::Running || state == State::Stopped ||
                          state == State::VersionMismatch;
  serviceRowHost_.Visibility(registered ? Visibility::Visible
                                        : Visibility::Collapsed);
  // Disabled while EITHER elevated verb runs: two UAC prompts in flight is
  // exactly the confusion the one-click design exists to avoid.
  if (uninstallServiceButton_) uninstallServiceButton_.IsEnabled(!snap.busy);
}

void SettingsPage::BuildIdentitySection(Panel const& host) {
  Heading(host, Loc("post_quantum_identity"), hstring{});
  auto card = Card(host);
  TextBlock unused{nullptr};
  auto button = NavRow(card, Loc("provider_identities"), unused);
  button.Click([this](auto const&, auto const&) { ShowIdentitySheet(); });
  Supporting(card, Loc("post_quantum_identity_explanation"));
}

// The community half of ABOUT. The product-updates preference that used to open
// this card is a PREFERENCE and moved to General; what is left is the ways to
// reach the project and the protocol link, which is About material.
void SettingsPage::BuildStayInTouchSection(Panel const& host) {
  Heading(host, Loc("stay_in_touch"), hstring{});
  auto card = Card(host);

  // Both rows are the store's own markdown strings, rendered with the link
  // inline (SetMarkdownLinkText). That keeps the whole shipped sentence - there
  // is no plain-text variant of the DePIN Hub line - and needs no extra "Open"
  // word beside it. Each sits in a pane row so it shares the left edge and the
  // hairline grid with everything above it.
  auto linkRow = [&card](std::wstring const& markdown) {
    TextBlock text;
    SetMarkdownLinkText(text, markdown, 13);
    text.TextWrapping(TextWrapping::Wrap);
    Border box;
    box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
    box.BorderBrush(colors::BorderBrush());
    box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
    box.Child(text);
    card.Children().Append(box);
  };
  // The Discord invite is unreachable in some regions, so the support address
  // is offered beside it (support::kStayInTouchLinks).
  for (const auto link : support::kStayInTouchLinks) {
    switch (link) {
      case support::StayInTouchLink::Discord:
        linkRow(Localized("join_the_community_on_discord_https_discord_com"));
        break;
      case support::StayInTouchLink::SupportEmail:
        linkRow(support::SupportEmailMarkdown(Localized("email_support_at")));
        break;
      case support::StayInTouchLink::DePinHub:
        linkRow(Localized("verified_project_on_depin_hub_https_depinhub_io"));
        break;
    }
  }

  // The protocol link, which used to hang off the very bottom of the page under
  // everything else with nothing holding it there.
  HyperlinkButton protocol;
  protocol.Content(winrt::box_value(Loc("uses_ur_protocol")));
  protocol.NavigateUri(winrt::Windows::Foundation::Uri(L"https://ur.xyz"));
  protocol.FontSize(13);
  protocol.Padding(ThicknessHelper::FromLengths(0, 0, 0, 0));
  protocol.VerticalAlignment(VerticalAlignment::Center);
  auto protocolRow = kit::MakePaneRow(40);
  protocolRow.Child(protocol);
  card.Children().Append(protocolRow);
}

// REACHING A HUMAN. The feedback form is one-way; this is the other way, and
// support@ur.io and the Discord invite were already in the store
// (if_the_problem_persists_contact_us_at_support_ur) with no call site
// anywhere in the client. Built TWICE (BuildSections): into Support's pane B
// without a group header - the 40px pane header strip above already carries
// the word, and repeating it read as a stutter on Settings' device pane - and
// into the inline narrow-width host WITH one, because there is no pane header
// naming the section there.
void SettingsPage::BuildSupportContactSection(Panel const& host, bool withGroupHeader) {
  if (withGroupHeader) {
    host.Children().Append(kit::MakePaneGroupHeader(Loc("support")).root);
  }

  // The contact prose, exactly as BuildStayInTouchSection's linkRow: the
  // store's own markdown string rendered with the links inline
  // (SetMarkdownLinkText), so the whole shipped sentence survives and both
  // links are real Hyperlinks in the tab order, in a hairline-bottom row at
  // the pane's 12px inset.
  TextBlock text;
  SetMarkdownLinkText(text, Localized("if_the_problem_persists_contact_us_at_support_ur"), 13);
  text.TextWrapping(TextWrapping::Wrap);
  Border box;
  box.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
  box.BorderBrush(colors::BorderBrush());
  box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
  box.Child(text);
  host.Children().Append(box);

  // The protocol link as a whole-row chevron button: in pane mode NavRow
  // emits the 44px chevron row, which is what a tappable row is everywhere
  // else in the shell. The URL is the one the card model's HyperlinkButton
  // already navigated to.
  TextBlock unused{nullptr};
  auto row = rows::NavRow(host, Loc("learn_more_protocol_page"), unused);
  row.Click([this](auto const&, auto const&) { OpenProtocolPage(); });
}

void SettingsPage::BuildSubscriptionSection(Panel const& host) {
  auto card = Card(host);
  // Opens the Stripe customer portal in the browser; there is nothing to show
  // beside the label, so the whole row is the affordance.
  TextBlock unused{nullptr};
  manageSubscription_ = NavRow(card, Loc("site_app_manage_subscription"), unused);
  manageSubscription_.Click([this](auto const&, auto const&) { OpenCustomerPortal(); });
  // hidden until the balance shows a Stripe subscription (ApplySubscriptionStore)
  manageSubscription_.Visibility(Visibility::Collapsed);
}

void SettingsPage::ApplySubscriptionStore(std::string const& storeFamily) {
  if (!manageSubscription_) return;
  manageSubscription_.Visibility(urnw::ShowsManageSubscription(storeFamily) ? Visibility::Visible
                                                                            : Visibility::Collapsed);
}

void SettingsPage::BuildVersionSection(Panel const& host) {
  // No group header: the pane header strip above already carries this word, and
  // a 28px strip repeating it read as a stutter - the same reason the
  // leaderboard has no panel heading under its destination title.
  auto card = Card(host);
  versionValue_ = ValueRow(card, Loc("version_info"));
  // The build's own stamp (Common/Version.h), distinct from the SDK row above:
  // release tags, the update banner and the VERSIONINFO resource all speak this
  // exact string, so About must show it verbatim. "0.0.0-dev" outside CI is the
  // correct answer, not a bug. Applied here, not in LoadSettings — it is a
  // compile-time constant and needs no round trip.
  ApplyValue(ValueRow(card, Missing("app_version", L"App version")),
             urnw::version::kString);

  // THE UPDATE STATE, next to the version it describes (beta spec §5). The
  // auto-check toggle lives on the General pane; what this pane had no surface
  // for is what that toggle produces — whether THIS build is current. Home's
  // banner only appears when there is something to install, so "am I up to
  // date?" had no answer anywhere in Settings. The value row always leads
  // with the running build (version::kString, the same stamp the row above
  // shows) and appends the last check's outcome; ApplyUpdateCheck is the one
  // writer. The action row's click is the developer screen's own trigger —
  // CheckNow coalesces with a queued or running check, so the button needs no
  // gating. State words go through upd_ ids like the toggle's: the store
  // carries none of the updater's wording (PageContext.h documents the
  // prefix); "Update" and "Check for updates" ARE store keys and come through
  // Loc.
  updateStateValue_ = ValueRow(card, Loc("update"));
  auto checkNow = ButtonRow(
      card, Loc("dev_check_updates"),
      Adv("upd_manual_note",
          L"Runs the release check now; the outcome lands on the row above."),
      Adv("upd_check_now", L"Check now"));
  checkNow.Click([](auto const&, auto const&) { urnw::pages::Updates().CheckNow(); });
  // Replay the standing state into the row just built: these sections build on
  // the first ApplyStrings, and the outcome of the launch check (or a manual
  // check fired before this destination was ever opened) must not be lost to
  // having arrived early. Same bind-then-replay shape as the window's.
  ApplyUpdateCheck(urnw::pages::Updates().Current());
}

// ---- the update checker (beta spec §5) --------------------------------------

void SettingsPage::ApplyUpdateCheck(UpdateChecker::Snapshot const& snap) {
  using Outcome = UpdateChecker::CheckOutcome;
  if (!updateStateValue_) return;  // the section is not built yet
  // The value ALWAYS leads with the running build — the row is where the build
  // identifies itself — and appends the last check's outcome. Version strings
  // are DATA (release grammar), so composing them around the Adv() state
  // words hides no literal from the store. The state words stay short:
  // ValueRow ellipsizes its value at 260px (SettingsSheets.cpp), and the
  // developer screen's report line carries the long-form version of the same
  // outcome, newest-release tag included.
  std::wstring text = urnw::Widen(urnw::version::kString);
  switch (snap.lastCheck) {
    case Outcome::NeverRan:
      text += L" — " + AdvW("upd_state_not_checked", L"not checked yet");
      break;
    case Outcome::InFlight:
      text += L" — " + AdvW("upd_state_checking", L"checking…");
      break;
    case Outcome::NoUpdate:
      text += L" — " + AdvW("upd_state_current", L"up to date");
      break;
    case Outcome::UpdateFound:
      text += L" — " + AdvW("upd_state_available", L"update available:") + L" v" +
              snap.newestVersion;
      break;
    case Outcome::DevBuild:
      text += L" — " + AdvW("upd_state_dev_build", L"dev build, never self-updates");
      break;
    default:  // Failed
      text += L" — " + AdvW("upd_state_failed", L"check failed");
      break;
  }
  updateStateValue_.Text(hstring{text});
  // ...and the fold-gated copy (BuildVersionFoldSection): one snapshot, both
  // rows, so the two can never disagree about the last check's outcome.
  if (updateStateValueFold_) updateStateValueFold_.Text(hstring{text});
}

// LICENSES: the open source software and data attributions the app ships
// (LicensesPage), opened in Settings' place. The row belongs to About, under
// the version rows - what the app is, then what it is built from.
//
// About is also the first pane to fold (MainWindow::ApplyBreakpoint, < 1400dip),
// and some of these licenses REQUIRE their attribution to be reachable in the
// product (GeoLite2's MaxMind notice), so a second copy of the row waits at the
// foot of General under an About strip of its own. Exactly one of the two is
// visible at any width: ApplyAboutPaneVisible switches them.
void SettingsPage::BuildLicensesRows(Panel const& about, Panel const& general) {
  auto row = [this](Panel const& card) {
    TextBlock unused{nullptr};
    auto button = NavRow(card, Loc("licenses"), unused);
    button.Click([this](auto const&, auto const&) { w_.OpenLicenses(); });
  };
  licensesAboutRow_ = StackPanel();
  about.Children().Append(licensesAboutRow_);
  row(Card(licensesAboutRow_));

  licensesGeneralRow_ = StackPanel();
  general.Children().Append(licensesGeneralRow_);
  Heading(licensesGeneralRow_, Missing("about", L"About"), hstring{});
  row(Card(licensesGeneralRow_));

  ApplyAboutPaneVisible(aboutPaneVisible_);
}

void SettingsPage::ApplyAboutPaneVisible(bool visible) {
  aboutPaneVisible_ = visible;
  // The version rows' fold copy shares this gate: About visible means the
  // primary rows are on screen, About folded means the copy takes over.
  if (versionFoldHost_) {
    versionFoldHost_.Visibility(visible ? Visibility::Collapsed : Visibility::Visible);
  }
  if (!licensesAboutRow_) return;  // the sections are not built yet
  licensesAboutRow_.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
  licensesGeneralRow_.Visibility(visible ? Visibility::Collapsed : Visibility::Visible);
}

void SettingsPage::BuildDangerSection() {
  auto host = w_.AccountDangerHost();
  // WHOLE-ROW affordances, not a label with a button beside it repeating the
  // same word. Both rows previously read "Sign out [Sign out]".
  TextBlock unused{nullptr};
  auto signOut = NavRow(host, Loc("sign_out"), unused);
  signOut.Click([this](auto const& sender, auto const& args) { OnSignOut(sender, args); });

  // NOT red here. The spec is explicit that a destructive command must not be a
  // bright red control sitting in a default row, and that red belongs to the
  // confirmation context - which for this one is DeleteAccountSheet, where the
  // user types the network name back before the button is enabled at all.
  TextBlock unusedDelete{nullptr};
  deleteAccountButton_ = NavRow(host, Loc("delete_account_2"), unusedDelete);
  deleteAccountButton_.Click([this](auto const&, auto const&) { ShowDeleteAccountSheet(); });
}

// ---- loads -----------------------------------------------------------------

void SettingsPage::ResetForSignOut() {
  // Identity first: every one of these describes the account that just left.
  // networkName_ is the dangerous one - it is what the delete gate compares
  // against, and networkDelete acts on whatever JWT is current.
  clientId_.clear();
  networkName_.clear();
  deviceName_.clear();
  authTypes_.clear();
  preferencesLoaded_ = false;

  // Then the visible state, so nothing on screen still claims to describe it.
  ApplyFieldState(clientIdValue_, FieldState::NoSession);
  ApplyFieldState(referralNetworkValue_, FieldState::NoSession);
  ApplyFieldState(deviceNameValue_, FieldState::NoSession);
  ApplyFieldState(deviceSpecValue_, FieldState::NoSession);
  RenderAuthMethods(FieldState::NoSession);
  clientIdCopy_.IsEnabled(false);
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
  ApplyLaunchAtStartup();
}

void SettingsPage::ApplyLaunchAtStartup() {
  if (!launchAtStartup_) return;  // the section is not built yet
  applyingLaunchAtStartup_ = true;
  launchAtStartup_.IsOn(urnw::LaunchAtStartupEnabled());
  applyingLaunchAtStartup_ = false;
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
    ApplyFieldState(referralNetworkValue_, FieldState::NoSession);
    return;
  }
  ApplyFieldState(referralNetworkValue_, FieldState::Loading);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
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
    // In a pane the state line has to sit on the pane's grid like everything
    // else: bare, it read as a caption floating above the row below it, with
    // neither the 12px inset nor the hairline the rows around it carry.
    if (rows::PaneMode()) {
      Border box;
      box.Padding(ThicknessHelper::FromLengths(12, 8, 12, 8));
      box.BorderBrush(colors::BorderBrush());
      box.BorderThickness(ThicknessHelper::FromLengths(0, 0, 0, 1));
      box.Child(note);
      authMethodsPanel_.Children().Append(box);
      return;
    }
    authMethodsPanel_.Children().Append(note);
    return;
  }
  for (auto const& authType : authTypes_) {
    Button remove;
    remove.Content(winrt::box_value(Loc("remove")));
    // NOT red. The spec is explicit that Remove must not be a bright red control
    // in every default row, and that red belongs to the destructive confirmation
    // context - which for this one is ConfirmRemoveAuth's dialog, where removing
    // the only sign-in method is spelled out.
    if (!rows::PaneMode()) remove.Foreground(colors::DangerBrush());
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

// The uninstall confirmation, in ConfirmRemoveAuth's exact shape: defaults to
// Cancel, Enter must not uninstall, and the destructive word appears only on
// the button that commits. The body says the two things the click will cost —
// the app cannot connect afterwards, and Windows will ask for permission — so
// the UAC prompt that follows is expected rather than alarming.
winrt::fire_and_forget SettingsPage::ConfirmUninstallService() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    auto dialog = rows::MakeSheet(self->Content().XamlRoot(),
                                  Adv("svc_service_label", L"VPN service"));
    dialog.PrimaryButtonText(Adv("svc_uninstall_action", L"Uninstall"));
    dialog.CloseButtonText(Loc("cancel"));
    dialog.DefaultButton(ContentDialogButton::Close);  // Enter must not remove
    TextBlock body;
    body.Text(Adv("svc_uninstall_confirm_windows",
                  L"This stops the URnetwork service and removes it from "
                  L"Windows. The app can't connect until it is set up again. "
                  L"Windows will ask for administrator permission."));
    body.FontSize(14);
    body.TextWrapping(TextWrapping::Wrap);
    body.MinWidth(320);
    dialog.Content(body);
    if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
      self->BeginServiceUninstall();
    }
  } catch (...) {
  }
  w_.SetSheetOpen(false);
}

// ---- actions ---------------------------------------------------------------

winrt::fire_and_forget SettingsPage::ShowKillSwitchException() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    auto dialog = rows::MakeSheet(
        self->Content().XamlRoot(),
        Adv("kill_switch_exception", L"Kill switch exception"));
    dialog.CloseButtonText(Loc("got_it"));
    dialog.DefaultButton(ContentDialogButton::Close);
    TextBlock body;
    // the tun captures ::/0 minus the local scopes (net::kTunCaptureV6), so
    // the only public-route exception is SMTP on port 25
    body.Text(Adv(
        "kill_switch_exception_smtp_detail",
        L"While the VPN is connected, IPv6 is routed through URnetwork like "
        L"IPv4. Outbound SMTP on TCP port 25 bypasses the VPN, even when the "
        L"kill switch is on, which may expose your local public IP to those "
        L"mail servers. SMTP on ports 465 and 587 stays in the VPN and must "
        L"establish TLS."));
    body.FontSize(14);
    body.TextWrapping(TextWrapping::Wrap);
    body.MinWidth(320);
    // the safety-rule exception depends on the kill switch, unlike the two
    // above: off, that traffic leaves from the local ip; on, it is dropped
    TextBlock safetyRules;
    safetyRules.Text(Adv(
        "kill_switch_exception_unrecognized_encrypted",
        L"When the kill switch is off, traffic that URnetwork safety rules keep "
        L"off the network, such as unrecognized encrypted protocols, bypasses the "
        L"VPN and uses your local public IP. With the kill switch on, that traffic "
        L"is blocked."));
    safetyRules.FontSize(14);
    safetyRules.TextWrapping(TextWrapping::Wrap);
    safetyRules.MinWidth(320);
    StackPanel content;
    content.Spacing(12);
    content.Children().Append(body);
    content.Children().Append(safetyRules);
    dialog.Content(content);
    co_await dialog.ShowAsync();
  } catch (...) {
  }
  w_.SetSheetOpen(false);
}

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

void SettingsPage::OnLaunchAtStartupToggled() {
  if (applyingLaunchAtStartup_) return;  // the read wrote it; do not echo it back
  const bool wanted = launchAtStartup_.IsOn();
  if (urnw::SetLaunchAtStartup(wanted)) return;
  // As macOS does when SMAppService refuses: back to what Windows actually has.
  ApplyLaunchAtStartup();
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
        // the server's refusal in this app's words (PaymentRefusal.h); a
        // failure with no answer from the server keeps the sdk's words
        std::optional<PaymentRefusalText> refusal;
        if (result && result->error) {
          refusal = PaymentRefusalTextFor(*result->error, "something_went_wrong");
        } else if (err) {
          error = *err;
        }
        queue.TryEnqueue([weak, url, refusal, error] {
          auto window = weak.get();
          if (!window) return;
          auto& page = window->settings();
          page.manageSubscription_.IsEnabled(true);
          if (!url.empty()) {
            page.LaunchCustomerPortal(url);
            return;
          }
          if (refusal) {
            page.settingsSnackbar().Show(
                hstring{PaymentRefusalMessage(Localized(refusal->key), Widen(refusal->detail))},
                InfoBarSeverity::Error);
            return;
          }
          page.settingsSnackbar().Show(
              error.empty() ? Loc("something_went_wrong") : winrt::to_hstring(error),
              InfoBarSeverity::Error);
        });
      });
  co_return;
}

winrt::fire_and_forget SettingsPage::LaunchCustomerPortal(std::string url) {
  auto self = w_.get_strong();  // keep the window alive across the launch
  // Await the launcher's verdict: a fire-and-forget launch that failed looked
  // exactly like a portal that opened (UPGRADE.md D5).
  bool launched = false;
  try {
    launched = co_await winrt::Windows::System::Launcher::LaunchUriAsync(
        winrt::Windows::Foundation::Uri(winrt::to_hstring(url)));
  } catch (winrt::hresult_error const& e) {
    LogWarn("settings: customer portal launch failed: {}", urnw::Narrow(std::wstring{e.message()}));
    launched = false;
  } catch (...) {
    launched = false;
  }
  if (launched) co_return;
  LogWarn("settings: the customer portal did not open");
  snackbar_.Show(Loc("site_billing_portal_error"), InfoBarSeverity::Error);
}

winrt::fire_and_forget SettingsPage::OpenCloudProxies() {
  auto self = w_.get_strong();  // keep the window alive across the launch
  bool launched = false;
  try {
    launched = co_await winrt::Windows::System::Launcher::LaunchUriAsync(
        winrt::Windows::Foundation::Uri(hstring{cloudproxy::kProxiesUrl}));
  } catch (winrt::hresult_error const& e) {
    LogWarn("settings: cloud proxies launch failed: {}", urnw::Narrow(std::wstring{e.message()}));
    launched = false;
  } catch (...) {
    launched = false;
  }
  if (launched) co_return;
  LogWarn("settings: the cloud proxies page did not open");
  snackbar_.Show(Loc("something_went_wrong"), InfoBarSeverity::Error);
}

winrt::fire_and_forget SettingsPage::OpenProtocolPage() {
  auto self = w_.get_strong();  // keep the window alive across the launch
  bool launched = false;
  try {
    launched = co_await winrt::Windows::System::Launcher::LaunchUriAsync(
        winrt::Windows::Foundation::Uri(L"https://ur.xyz"));
  } catch (winrt::hresult_error const& e) {
    LogWarn("settings: protocol page launch failed: {}", urnw::Narrow(std::wstring{e.message()}));
    launched = false;
  } catch (...) {
    launched = false;
  }
  if (launched) co_return;
  LogWarn("settings: the protocol page did not open");
  snackbar_.Show(Loc("something_went_wrong"), InfoBarSeverity::Error);
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

// Attach the service's logs to a feedback report the server has already
// accepted, identified by its id. Called only from OnSendFeedback, only when
// the user ticked the box - apple's FeedbackView contract. Never a standalone
// affordance: an upload the user did not ask for, correlated with nothing, is
// exfiltration with a friendly label.
//
// SdkHost asks the service first, which uploads its own logs whether or not a
// tunnel runs, and falls back to the DeviceRemote, the only path before the
// service could be asked. It returns at once: the request runs on its own
// thread (App/FeedbackLogUpload.h), never on this one.
//
// Failure is silent by design here and only here: the feedback itself was
// accepted, so telling the user their report failed would be false, and the
// attachment is an extra. It is logged.
void SettingsPage::UploadLogs(std::string const& feedbackId) {
  if (feedbackId.empty()) {
    LogWarn("settings: log attach skipped (no feedback id)");
    return;
  }
  Sdk().UploadFeedbackLogs(feedbackId);
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
    // the added line for the method (apple's snackbar, ur.io's done step)
    if (!addAuthSheet_->AddedMessageKey().empty()) {
      snackbar_.Show(Loc(addAuthSheet_->AddedMessageKey()), InfoBarSeverity::Success);
    }
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

winrt::fire_and_forget SettingsPage::ShowVlessSheet() {
  if (w_.sheetOpen()) co_return;
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  try {
    vlessSheet_ = urnw::VlessSheet::Create(self->Content().XamlRoot(), Sdk());
    co_await vlessSheet_->Dialog().ShowAsync();
  } catch (...) {
  }
  vlessSheet_.reset();
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

void SettingsPage::PrefillFromCampaign(std::string const& token, int rating,
                                       std::string const& reason) {
  if (1 <= rating && rating <= 5) w_.FeedbackRating().Value(static_cast<double>(rating));
  if (!reason.empty()) w_.FeedbackText().Text(winrt::to_hstring(reason));
  if (token.empty() || !Sdk().IsLoggedIn()) return;
  Sdk().api().onboardingFeedbackToken(
      token, rating, reason,
      [](std::optional<urnet::OnboardingFeedbackTokenResult>, std::optional<std::string> err) {
        if (err) LogWarn("settings: feedback token failed: {}", *err);
      });
}

// Send reads "Sending…" and stays disabled while the request is out; the
// label also names the button for a screen reader. Kept in feedbackSending_ so
// ApplyStrings mid-send keeps the right label.
void SettingsPage::SetFeedbackSending(bool sending) {
  feedbackSending_ = sending;
  ApplyFeedbackSendButton();
}

void SettingsPage::ApplyFeedbackSendButton() {
  const FeedbackSendButton button = FeedbackSendButtonFor(feedbackSending_);
  const winrt::hstring label = Loc(button.labelKey);
  w_.SendFeedbackButton().IsEnabled(button.enabled);
  // this pane's Send is a plain-content button (the pane model's, not the old
  // glyph-plus-label panel), so the label IS the content; the explicit name
  // stays so the one way to submit this form is never nameless
  w_.SendFeedbackButton().Content(winrt::box_value(label));
  winrt::Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(
      w_.SendFeedbackButton(), label);
}

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
  const int64_t sentRating = args.star_count;  // for feedback.submitted
  const std::string sentText = text;

  SetFeedbackSending(true);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().sendFeedback(
      args, [queue, weak, attachLogs, sentRating, sentText](
                std::optional<urnet::FeedbackSendResult> result, std::optional<std::string> err) {
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
        queue.TryEnqueue([weak, ok, error, attachLogs, feedbackId, sentRating, sentText] {
          auto self = weak.get();
          if (!self) return;
          auto& page = self->settings();
          page.SetFeedbackSending(false);
          if (!ok) {
            page.settingsSnackbar().Show(
                error.empty() ? Loc("error_sending_feedback") : winrt::to_hstring(error),
                InfoBarSeverity::Error);
            return;
          }
          if (Sdk().eventsReady()) Sdk().events().FeedbackSubmitted(sentRating, "", sentText);
          page.settingsSnackbar().Show(Loc("thanks_for_the_feedback"),
                                       InfoBarSeverity::Success);
          self->FeedbackText().Text(L"");
          self->FeedbackIncludeLogs().IsChecked(false);
          if (attachLogs && !feedbackId.empty()) page.UploadLogs(feedbackId);
        });
      });
}

}  // namespace urnw
