// The Settings destination and the Support destination (the feedback form),
// which iOS carries inside the same settings surface.
//
// Settings is macOS SettingsForm parity: account (client id, referral code,
// referral network, auth codes, sign-in methods), device (name, spec),
// connections (kill switch, blocked locations), post-quantum identity,
// preferences (product updates), subscription, logs, community links, version,
// and — below sign out — delete account.
//
// The sections above the split rules are BUILT IN CODE, into the three empty
// host panels the markup provides (SettingsSections, SettingsDangerSection).
// SettingsSheets.h explains why, and carries the row kit they are built from.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include "ServiceSetup.h"
#include "SettingsSheets.h"
#include "StatsSheets.h"
#include "UrComponents.h"

namespace winrt::URnetwork::implementation {
struct MainWindow;
}

namespace urnw {

class SettingsPage {
 public:
  explicit SettingsPage(winrt::URnetwork::implementation::MainWindow& window);

  // Builds the code-built sections on first call, then (re)labels everything.
  void ApplyStrings();
  // Advanced Mode changed (D5): reflect it in the toggle. Called from
  // MainWindow::ApplyAdvancedMode, which is the one apply path — the toggle
  // itself only writes, it never applies.
  void ApplyAdvancedMode(bool on);
  // The service manager's snapshot changed (beta spec §3): show the uninstall
  // row only while a service is actually registered. Hidden — not disabled —
  // for NotInstalled / ConsoleMode / Unknown, because an affordance for
  // removing something that is not there is noise, and in ConsoleMode the verb
  // would only fight the developer's console run. Pushed by
  // MainWindow::ApplyServiceSetup, the one writer of that snapshot.
  void ApplyServiceSetup(urnw::ServiceSetup::Snapshot const& snap);

  // The settings destination's API loads: network user (sign-in methods,
  // network name), device info, referral code + network, account preferences.
  // Skipped by --preview-ui exactly as the other destinations' loads are.
  void LoadSettings();

  void OnManageAppSplitTunnel(winrt::Windows::Foundation::IInspectable const&,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSignOut(winrt::Windows::Foundation::IInspectable const&,
                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnSendFeedback(winrt::Windows::Foundation::IInspectable const&,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

  // --preview-ui only (Startup.h): raise the feedback acknowledgement, the
  // severity that SHOULD time out, so both snackbar behaviours are visible.
  void ShowPreviewSnackbar();

  // The page's own snackbar, reached by the UI-thread continuations of its SDK
  // callbacks (which resolve the window's weak reference and come back here).
  urnw::kit::Snackbar& settingsSnackbar() { return snackbar_; }

  // Re-read the network user; called by the add-auth sheet after it links a
  // method, and by the remove path after it unlinks one.
  void LoadNetworkUser();

  // Drop everything describing the account that just signed out, and put every
  // field back to NoSession. The network name is the dangerous one: it is what
  // the delete-account gate compares against, and Api::networkDelete acts on
  // the CURRENT JWT, so a name left over from the previous session turns the
  // confirmation ritual into a way to destroy a different network.
  void ResetForSignOut();

  // The blocked-locations sheet. Public because the Network destination's
  // detail pane is a second door to it: blocked countries are what the location
  // list is filtered by, so the list is where a user looks for them. Still
  // reachable from Settings' VPN group; one sheet, two entry points.
  winrt::fire_and_forget ShowBlockedLocationsSheet();

 private:
  winrt::fire_and_forget ShowAppRulesSheet();

  // ---- section construction (once, on the first ApplyStrings) ----
  void BuildSections();
  // R4: what used to be BuildAccountSection, split in two and built onto
  // ACCOUNT's panes (spec override #2). The builders stay here because this
  // class owns their sheets, loads and echo guards; only the host changed.
  void BuildSecuritySection(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);
  void BuildReferralSection(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);
  void BuildDeviceSection(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);
  // R4: the reconciled preference sections. General is the one preference that
  // ships; Advanced carries export logs and the Advanced Mode host.
  void BuildGeneralSection(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);
  void BuildAdvancedSection(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);
  void BuildConnectionsSection(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);
  void BuildIdentitySection(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);
  void BuildStayInTouchSection(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);
  void BuildSubscriptionSection(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);
  void BuildVersionSection(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);
  void BuildDangerSection();

  // ---- loads ----
  void LoadDeviceInfo();
  void LoadReferral();
  void LoadPreferences();
  // `state` is the terminal state of the network-user load; only Loaded
  // renders rows, and every other state renders the line that names it.
  void RenderAuthMethods(rows::FieldState state);
  void ApplyLocalDeviceState();  // client id + kill switch, straight off the SDK

  // ---- actions ----
  void OnKillSwitchToggled();
  void OnProductUpdatesToggled();
  // modal confirm, then RemoveAuth (apple SettingsView's confirmationDialog)
  winrt::fire_and_forget ConfirmRemoveAuth(std::string authType);
  void RemoveAuth(std::string const& authType);
  // modal confirm, then MainWindow::BeginServiceUninstall (elevated
  // `urnetworkd uninstall`). Same dialog shape as ConfirmRemoveAuth: defaults
  // to Cancel, commits only on the explicit destructive button.
  winrt::fire_and_forget ConfirmUninstallService();
  winrt::fire_and_forget OpenCustomerPortal();
  winrt::fire_and_forget SaveLogsToFile();
  // Attaches the SDK log directory to an ALREADY-ACCEPTED feedback report,
  // using the server's own feedback id. Only OnSendFeedback calls it, and only
  // when the user ticked the box.
  void UploadLogs(std::string const& feedbackId);

  // ---- sheets (one at a time, through the window's sheetOpen_ guard) ----
  winrt::fire_and_forget ShowDeviceNameSheet();
  winrt::fire_and_forget ShowAuthCodeSheet();
  winrt::fire_and_forget ShowAddAuthSheet();
  winrt::fire_and_forget ShowReferralNetworkSheet();
  winrt::fire_and_forget ShowIdentitySheet();
  winrt::fire_and_forget ShowDeleteAccountSheet();

  winrt::URnetwork::implementation::MainWindow& w_;
  // "Thanks for the feedback": a transient acknowledgement (iOS UrSnackBar)
  urnw::kit::Snackbar snackbar_;
  std::shared_ptr<urnw::AppRulesSheet> appRulesSheet_;

  bool built_ = false;

  // ---- the code-built controls the loads write into ----
  winrt::Microsoft::UI::Xaml::Controls::TextBlock clientIdValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button clientIdCopy_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock referralCodeValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button referralCodeCopy_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock referralNetworkValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel authMethodsPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock deviceNameValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock deviceSpecValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch advancedMode_{nullptr};
  // IsOn assignment raises Toggled, so a push from the apply path would be
  // written straight back through SdkHost without this. Same guard, same reason,
  // as applyingKillSwitch_.
  bool applyingAdvancedMode_ = false;
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch killSwitch_{nullptr};
  // The uninstall-service row (beta spec §3), wrapped in its own host panel so
  // visibility can collapse the WHOLE row — ButtonRow returns only the button,
  // and hiding a button inside a still-visible labelled row would leave a
  // caption pointing at nothing.
  winrt::Microsoft::UI::Xaml::Controls::StackPanel serviceRowHost_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button uninstallServiceButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch productUpdates_{nullptr};
  // why the toggle above is disabled, when it is (S4)
  winrt::Microsoft::UI::Xaml::Controls::TextBlock productUpdatesState_{nullptr};
  // "Check for updates automatically" (beta spec §5). No applying_ guard like
  // its neighbours: nothing ever writes IsOn back — the pref has one writer
  // (this toggle) and one reader path (the checker), so there is no echo.
  winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch autoUpdateCheck_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button manageSubscription_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock versionValue_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button deleteAccountButton_{nullptr};
  // >>> ADVANCED MODE GOES HERE (D5). <<< The first host in Settings' Advanced
  // group; see BuildAdvancedSection for the row shape to append.
  winrt::Microsoft::UI::Xaml::Controls::StackPanel advancedModeHost_{nullptr};

  // ---- loaded state ----
  std::string clientId_;
  std::string referralCode_;
  std::string networkName_;
  std::string deviceName_;
  std::vector<std::string> authTypes_;
  // Guards the product-updates write the way apple's AccountPreferencesViewModel
  // does: the toggle's own Toggled event fires when the LOAD writes it, and
  // without this that echo would post the value straight back to the server.
  bool applyingPreference_ = false;
  bool preferencesLoaded_ = false;
  // same echo guard for the kill switch, whose value is written by the load
  bool applyingKillSwitch_ = false;

  std::shared_ptr<urnw::DeviceNameSheet> deviceNameSheet_;
  std::shared_ptr<urnw::AuthCodeSheet> authCodeSheet_;
  std::shared_ptr<urnw::AddAuthSheet> addAuthSheet_;
  std::shared_ptr<urnw::ReferralNetworkSheet> referralSheet_;
  std::shared_ptr<urnw::BlockedLocationsSheet> blockedSheet_;
  std::shared_ptr<urnw::PostQuantumIdentitySheet> identitySheet_;
  std::shared_ptr<urnw::DeleteAccountSheet> deleteSheet_;
};

}  // namespace urnw
