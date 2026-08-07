// The settings/account long tail: the row kit the settings sections are built
// from, and the ContentDialogs they open.
//
// WHY THESE SECTIONS ARE BUILT IN CODE. MainWindow.xaml is the one file every
// parallel phase touches, and the settings surface is ~15 sections deep. Adding
// them as markup would put a merge conflict between this phase and every other
// one, for no benefit: the rows are uniform (label / note / trailing control),
// so a builder expresses them more compactly than the markup would. The page
// gets three empty host panels in the markup (SettingsSections,
// SettingsDangerSection, AccountProfileExtra) and fills them here.
//
// The row kit reads its look out of App.xaml (UrCardStyle, UrLabelStyle,
// UrSwitchToggleStyle, ...) rather than restating colours, so these sections
// stay in step with the markup-built ones above them.
//
// Every sheet follows the established pattern (AuthSheets.h, StatsSheets.h):
// enable_shared_from_this, a static Create(root, ...), a ContentDialog on the
// brand sheet surface, weak captures in handlers, and the window holding the
// shared_ptr for as long as the dialog shows.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include "SdkHost.h"

namespace urnw {

// ---- the row kit ----------------------------------------------------------
namespace rows {

// A style from App.xaml by key, or nullptr when it is missing. Nullptr is a
// legal Style() assignment, so a renamed key degrades to the default look
// instead of throwing out of a constructor.
winrt::Microsoft::UI::Xaml::Style Lookup(std::wstring_view key);

// A UrCardStyle card wrapping a vertical panel. Returns the panel to fill;
// `outHost` receives the border to append to the page.
winrt::Microsoft::UI::Xaml::Controls::StackPanel Card(
    winrt::Microsoft::UI::Xaml::Controls::Panel const& host, double spacing = 12);

// The 18pt brand heading the markup sections use above each card.
void Heading(winrt::Microsoft::UI::Xaml::Controls::Panel const& host,
             winrt::hstring const& text);

// muted 12pt supporting copy
winrt::Microsoft::UI::Xaml::Controls::TextBlock Supporting(
    winrt::Microsoft::UI::Xaml::Controls::Panel const& host, winrt::hstring const& text);

// label (+ optional muted note beneath) on the left, `trailing` on the right.
winrt::Microsoft::UI::Xaml::Controls::Grid Row(
    winrt::Microsoft::UI::Xaml::Controls::Panel const& host, winrt::hstring const& label,
    winrt::hstring const& note,
    winrt::Microsoft::UI::Xaml::FrameworkElement const& trailing);

// label / note row whose trailing control is a UrSwitchToggleStyle switch.
winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch ToggleRow(
    winrt::Microsoft::UI::Xaml::Controls::Panel const& host, winrt::hstring const& label,
    winrt::hstring const& note);

// label / note row whose trailing control is a button. Returns the button so the
// caller attaches Click and can disable it.
winrt::Microsoft::UI::Xaml::Controls::Button ButtonRow(
    winrt::Microsoft::UI::Xaml::Controls::Panel const& host, winrt::hstring const& label,
    winrt::hstring const& note, winrt::hstring const& action, bool danger = false);

// label / right-aligned value row. Returns the value TextBlock to write into.
winrt::Microsoft::UI::Xaml::Controls::TextBlock ValueRow(
    winrt::Microsoft::UI::Xaml::Controls::Panel const& host, winrt::hstring const& label);

// label / right-aligned value / trailing action button, all on ONE line - the
// shape the client-id and referral-code rows need. Appending the button
// separately puts it on a line of its own, which reads as an unrelated control.
winrt::Microsoft::UI::Xaml::Controls::TextBlock ValueActionRow(
    winrt::Microsoft::UI::Xaml::Controls::Panel const& host, winrt::hstring const& label,
    winrt::hstring const& action, winrt::Microsoft::UI::Xaml::Controls::Button& outButton);

// A whole-row tappable card button with a chevron, for rows that open a sheet.
winrt::Microsoft::UI::Xaml::Controls::Button NavRow(
    winrt::Microsoft::UI::Xaml::Controls::Panel const& host, winrt::hstring const& label,
    winrt::Microsoft::UI::Xaml::Controls::TextBlock& outValue);

// The terminal states every asynchronously-filled field on these surfaces must
// reach. This exists because the alternative keeps shipping: a field that only
// ever knows "I have a value" renders the SAME em dash whether the call has not
// run, is in flight, came back empty, or failed - and a spinner with no
// resolution is indistinguishable from a slow network. A mechanism with no
// observable signal does not exist.
//
//   NoSession  the load cannot run: there is no token (signed out, --preview-ui)
//   NoDevice   there IS a session, but the value comes off the DeviceRemote and
//              the service is not up. Distinct from NoSession because telling a
//              signed-in user to log in is a lie - seen on the beta network,
//              where the client id and the device rows read "Please login to
//              URnetwork" while the account was plainly signed in.
//   Loading    a request is in flight
//   Loaded     a value came back; the caller supplies it
//   Empty      the request succeeded and there is genuinely nothing
//   Failed     the request failed; ALSO log the underlying error at the callsite
enum class FieldState { NoSession, NoDevice, Loading, Loaded, Empty, Failed };

// Writes `state` onto a value TextBlock, using `loadedText` only for Loaded.
// Failure is the danger colour; the other non-value states are faint, so
// "nothing here" never looks like a value the user is meant to read.
void ApplyFieldState(winrt::Microsoft::UI::Xaml::Controls::TextBlock const& value,
                     FieldState state, winrt::hstring const& loadedText = {});

// a 1px UrBorderBrush separator
void Divider(winrt::Microsoft::UI::Xaml::Controls::Panel const& host);

void CopyToClipboard(std::string const& text);

// A ContentDialog on the brand sheet surface, with a close button. Shared by
// every sheet below so the surface treatment lives in one place.
winrt::Microsoft::UI::Xaml::Controls::ContentDialog MakeSheet(
    winrt::Microsoft::UI::Xaml::XamlRoot const& root, winrt::hstring const& title);

}  // namespace rows

// ---- Edit device name (apple SettingsForm deviceName row) ------------------
//
// Api::deviceSetName with no device_id names THIS device (the client the JWT was
// issued to), which is the row's meaning.
class DeviceNameSheet : public std::enable_shared_from_this<DeviceNameSheet> {
 public:
  // `onSaved` runs on the UI thread after a successful save, so the settings row
  // can re-read the name without a second round trip.
  static std::shared_ptr<DeviceNameSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      std::string const& current, std::function<void(std::string)> onSaved);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  DeviceNameSheet(SdkHost& sdk, std::function<void(std::string)> onSaved)
      : sdk_(sdk), onSaved_(std::move(onSaved)) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root, std::string const& current);
  void Submit();
  void ApplyResult(bool ok, std::string const& error, std::string const& name);

  SdkHost& sdk_;
  std::function<void(std::string)> onSaved_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox nameBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  bool saving_ = false;
};

// ---- Create auth code (apple AuthCodeCreate) -------------------------------
//
// One button, then the code with a copy affordance and the five-minute expiry
// note. The auth-code LIMIT is a distinct error the server flags on its own
// field, and the store already carries a string for it.
class AuthCodeSheet : public std::enable_shared_from_this<AuthCodeSheet> {
 public:
  static std::shared_ptr<AuthCodeSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit AuthCodeSheet(SdkHost& sdk) : sdk_(sdk) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void Create();
  void ApplyResult(std::optional<urnet::AuthCodeCreateResult> result, std::string const& err);

  SdkHost& sdk_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button createButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ProgressRing ring_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel codePanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock codeText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{nullptr};
  std::string code_;
  bool creating_ = false;
};

// ---- Add auth method (apple AddAuthSheet) ----------------------------------
//
// Email/phone + password. The wallet legs of iOS's sheet (Solana/Bittensor) are
// NOT here: they need the browser signing bridge, which is P5's surface.
class AddAuthSheet : public std::enable_shared_from_this<AddAuthSheet> {
 public:
  static std::shared_ptr<AddAuthSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      std::function<void()> onChanged);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  AddAuthSheet(SdkHost& sdk, std::function<void()> onChanged)
      : sdk_(sdk), onChanged_(std::move(onChanged)) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void Validate();
  void Submit();
  void ApplyResult(bool ok, std::string const& error);

  SdkHost& sdk_;
  std::function<void()> onChanged_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox authBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::PasswordBox passwordBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  bool submitting_ = false;
};

// ---- Referral network (apple UpdateReferralNetworkSheet) -------------------
//
// Shows the network this one was referred by, lets it be set from a code, and
// lets it be unlinked. Unlink is destructive in the "you lose future points"
// sense, so it takes the second confirmation the store's string describes.
class ReferralNetworkSheet : public std::enable_shared_from_this<ReferralNetworkSheet> {
 public:
  static std::shared_ptr<ReferralNetworkSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk,
      std::function<void()> onChanged);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  ReferralNetworkSheet(SdkHost& sdk, std::function<void()> onChanged)
      : sdk_(sdk), onChanged_(std::move(onChanged)) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void Load();
  void ApplyCurrent(rows::FieldState state, std::string const& name);
  void Submit();
  // Unlink is a two-step on DIFFERENT controls: the inline button arms, the
  // dialog's primary commits, the dialog's close button cancels. One control
  // doing both let a double-click commit in a single gesture.
  void ArmUnlink();
  void DisarmUnlink();
  void Unlink();
  void ShowError(winrt::hstring const& message);

  SdkHost& sdk_;
  std::function<void()> onChanged_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock currentText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox codeBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Button unlinkButton_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  // the unlink warning names the network being unlinked, so the confirmation
  // step needs the name that was loaded
  std::string currentName_;
  bool unlinkArmed_ = false;
  bool busy_ = false;
};

// ---- Blocked locations (apple BlockedLocationsView + AddBlockedLocationSheet)
//
// The list and the add-a-country picker in one sheet: the picker is a search
// over Api::getProviderLocations filtered to country rows, which is what iOS's
// add sheet lists.
class BlockedLocationsSheet : public std::enable_shared_from_this<BlockedLocationsSheet> {
 public:
  static std::shared_ptr<BlockedLocationsSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit BlockedLocationsSheet(SdkHost& sdk) : sdk_(sdk) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void LoadBlocked();
  void LoadCountries();
  void RenderBlocked();
  void RenderCountries();
  void Block(std::string const& locationId);
  void Unblock(std::string const& locationId);
  void ShowError(winrt::hstring const& message);

  SdkHost& sdk_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel blockedPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock blockedEmpty_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox search_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel countryPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};

  std::vector<urnet::BlockedLocation> blocked_;
  // country_location_id -> display name, from getProviderLocations
  std::vector<std::pair<std::string, std::string>> countries_;
  bool loadingCountries_ = false;
  // Both lists carry their own terminal state: without them a 401 arrived as an
  // empty list and rendered as the reassuring "No blocked locations".
  rows::FieldState blockedState_ = rows::FieldState::Loading;
  rows::FieldState countriesState_ = rows::FieldState::Loading;
};

// ---- Post quantum identity (apple PostQuantumIdentityPanel + Providers) ----
//
// This device's public identity key hash, with the identicon the SDK renders
// for it, and the per-provider identities the device has seen. DeviceRemote,
// so it needs a live session; with none it says so rather than showing an
// empty list that would read as "no providers".
class PostQuantumIdentitySheet
    : public std::enable_shared_from_this<PostQuantumIdentitySheet> {
 public:
  static std::shared_ptr<PostQuantumIdentitySheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit PostQuantumIdentitySheet(SdkHost& sdk) : sdk_(sdk) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  // Three DeviceRemote RPCs; runs them off the UI thread (see the definition).
  winrt::fire_and_forget Load();
  // The UI half of Load, run back on the UI thread with what the RPCs returned.
  void ApplyIdentity(bool failed, std::string const& hash, std::vector<uint8_t> const& key,
                     std::optional<urnet::ProviderIdentityList> const& providers);
  // PNG bytes -> BitmapImage is async at every step; see the definition.
  winrt::fire_and_forget SetIdenticon(std::vector<uint8_t> png);

  SdkHost& sdk_;
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock hashText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::Image identicon_{nullptr};
  // enabled only once there is a hash to put on the clipboard
  winrt::Microsoft::UI::Xaml::Controls::Button copyHash_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::StackPanel providerPanel_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock statusText_{nullptr};
  std::string hash_;
};

// ---- Delete account (apple SettingsForm deleteAccount) ---------------------
//
// Api::networkDelete is irreversible and takes no arguments, so the ONLY thing
// standing between a mis-click and a destroyed network is this sheet. It follows
// the pattern the store's own string names ("Type your network name to
// confirm"): the primary stays disabled until the typed text matches the network
// name exactly.
class DeleteAccountSheet : public std::enable_shared_from_this<DeleteAccountSheet> {
 public:
  // Takes NO network name. The sheet reads it itself, from the session that is
  // current when the sheet opens, and refuses to arm until that read succeeds.
  //
  // This is the fix for a real way to destroy the wrong network. The page's
  // cached name survived a sign-out, the sign-out button is on this very page,
  // and the per-destination loads only ran on navigation - so signing out of A
  // and into B left A's name in the gate, on screen, ready to be copied into
  // the confirm box. Api::networkDelete takes no arguments and acts on the
  // CURRENT JWT, so satisfying that gate would have deleted B. A gate is only
  // as good as the freshness of what it compares against, so it now owns that
  // freshness rather than trusting a caller.
  static std::shared_ptr<DeleteAccountSheet> Create(
      winrt::Microsoft::UI::Xaml::XamlRoot const& root, SdkHost& sdk);

  winrt::Microsoft::UI::Xaml::Controls::ContentDialog Dialog() const { return dialog_; }

 private:
  explicit DeleteAccountSheet(SdkHost& sdk) : sdk_(sdk) {}
  void Build(winrt::Microsoft::UI::Xaml::XamlRoot const& root);
  void LoadNetworkName();
  void ApplyName(rows::FieldState state, std::string const& name);
  void UpdateGate();
  void Submit();

  SdkHost& sdk_;
  // Empty until a fresh read of THIS session's network name lands. The gate
  // fails closed on empty, so an unresolved name can never arm the primary.
  std::string networkName_;
  winrt::Microsoft::UI::Xaml::Controls::TextBlock nameText_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBox confirmBox_{nullptr};
  winrt::Microsoft::UI::Xaml::Controls::TextBlock errorText_{nullptr};
  bool deleting_ = false;
};

}  // namespace urnw
