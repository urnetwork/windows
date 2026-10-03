// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "SolanaWalletSheets.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "Localization.h"
#include "Log.h"
#include "PageContext.h"
#include "SheetFit.h"  // sheetfit: sheets clamp to the window at open time
#include "SettingsSheets.h"  // rows::Lookup
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;

namespace urnw {
namespace {

namespace automation = winrt::Microsoft::UI::Xaml::Automation;
namespace peers = winrt::Microsoft::UI::Xaml::Automation::Peers;

// The wallet bridge opens a browser and the user may take a while in it; a
// plain api call does not. (WalletPage's two values.)
constexpr int kBridgeTimeoutMs = 180'000;
constexpr int kApiTimeoutMs = 20'000;
// the pause after the last keystroke that checks the address
constexpr int kCheckDebounceMs = 300;

// the token every legacy payout is made in
constexpr const char* kUsdcTokenType = "USDC";

// A line in one of App.xaml's pane text styles. `wrap` undoes the styles'
// one-line trimming, which is right for a row and wrong for a sentence.
TextBlock StyledText(hstring const& text, std::wstring_view styleKey, bool wrap) {
  TextBlock line;
  line.Style(rows::Lookup(styleKey));
  line.Text(text);
  if (wrap) {
    line.TextWrapping(TextWrapping::Wrap);
    line.TextTrimming(TextTrimming::None);
  }
  return line;
}

// A pane action button inside a dialog. The pane styles inset the button by the
// pane's 12px, which the dialog's content already has.
Button ActionButton(hstring const& label, std::wstring_view styleKey) {
  Button button;
  button.Style(rows::Lookup(styleKey));
  button.Margin(ThicknessHelper::FromUniformLength(0));
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  button.Content(winrt::box_value(label));
  return button;
}

ColumnDefinition StarColumn() {
  ColumnDefinition col;
  col.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
  return col;
}

// the machine's store key as a line, "" for none
hstring KeyText(const char* key) {
  return (key == nullptr || *key == '\0') ? hstring{} : Loc(key);
}

// The status and the failure lines are live regions, so a screen reader hears a
// new text without moving to it. WinUI announces a live region only when its
// peer raises LiveRegionChanged, and a peer exists only while assistive
// technology is asking.
void AnnounceIfChanged(TextBlock const& line, hstring const& before) {
  if (line.Text().empty() || line.Text() == before) return;
  if (auto peer = peers::FrameworkElementAutomationPeer::FromElement(line)) {
    peer.RaiseAutomationEvent(peers::AutomationEvents::LiveRegionChanged);
  }
}

}  // namespace

hstring SolanaFailureText(std::string const& detail) {
  const char* key = solana::FailureKey(detail);
  return detail.empty() ? Loc(key) : hstring{urnw::Format(key, urnw::Widen(detail))};
}

std::shared_ptr<ConnectSolanaWalletSheet> ConnectSolanaWalletSheet::Create(
    XamlRoot const& root, SdkHost& sdk, bool allowActions,
    std::function<void(std::string)> onConnected, std::function<void()> onAbandonedLink) {
  auto sheet = std::shared_ptr<ConnectSolanaWalletSheet>(new ConnectSolanaWalletSheet(
      sdk, allowActions, std::move(onConnected), std::move(onAbandonedLink)));
  sheet->Build(root);
  return sheet;
}

ConnectSolanaWalletSheet::ConnectSolanaWalletSheet(SdkHost& sdk, bool allowActions,
                                                   std::function<void(std::string)> onConnected,
                                                   std::function<void()> onAbandonedLink)
    : sdk_(sdk),
      allowActions_(allowActions),
      onConnected_(std::move(onConnected)),
      onAbandonedLink_(std::move(onAbandonedLink)) {}

ConnectSolanaWalletSheet::~ConnectSolanaWalletSheet() { StopTimers(); }

void ConnectSolanaWalletSheet::Build(XamlRoot const& root) {
  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Title(winrt::box_value(Loc("connect_solana_wallet")));
  // Stays enabled while a request is out: dismissing makes it stale (Closed,
  // below), so its answer finds no sheet to act on.
  dialog_.CloseButtonText(Loc("cancel"));
  dialog_.Background(colors::SheetBrush());

  StackPanel content;
  content.Spacing(10);
  content.MinWidth(sheetfit::Width(root, 420));

  // what this is, and for how long
  content.Children().Append(
      StyledText(Loc("connect_solana_wallet_note"), L"UrKeyTextStyle", /*wrap=*/true));
  content.Children().Append(
      StyledText(Loc("usdc_payouts_until_migration"), L"UrRowNoteStyle", /*wrap=*/true));

  // a wallet app: the desktop bridge needs the provider up front
  Grid providers;
  providers.ColumnSpacing(8);
  providers.ColumnDefinitions().Append(StarColumn());
  providers.ColumnDefinitions().Append(StarColumn());
  phantomButton_ = ActionButton(Loc("phantom"), L"UrPaneActionPrimaryStyle");
  phantomButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->StartBridge(WalletConnect::Provider::Phantom);
  });
  Grid::SetColumn(phantomButton_, 0);
  providers.Children().Append(phantomButton_);
  solflareButton_ = ActionButton(Loc("solflare"), L"UrPaneActionSecondaryStyle");
  solflareButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->StartBridge(WalletConnect::Provider::Solflare);
  });
  Grid::SetColumn(solflareButton_, 1);
  providers.Children().Append(solflareButton_);
  content.Children().Append(providers);

  // "Waiting" is a state the user must be able to SEE: the browser is open and
  // the sheet is waiting for it to come back, or the account is being written.
  statusText_ = StyledText(hstring{}, L"UrRowNoteStyle", /*wrap=*/true);
  statusText_.Visibility(Visibility::Collapsed);
  automation::AutomationProperties::SetLiveSetting(statusText_,
                                                   peers::AutomationLiveSetting::Polite);
  content.Children().Append(statusText_);

  // a pasted address, folded under a link
  HyperlinkButton manualToggle;
  manualToggle.Content(winrt::box_value(Loc("enter_address_manually")));
  manualToggle.Padding(ThicknessHelper::FromUniformLength(0));
  manualToggle.HorizontalAlignment(HorizontalAlignment::Left);
  manualToggle.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->ToggleManual();
  });
  content.Children().Append(manualToggle);

  manualPanel_ = StackPanel();
  manualPanel_.Spacing(8);
  manualPanel_.Visibility(Visibility::Collapsed);
  addressBox_ = TextBox();
  addressBox_.Style(rows::Lookup(L"UrTextInputStyle"));
  addressBox_.PlaceholderText(Loc("enter_a_solana_usdc_wallet_address"));
  // a placeholder is not a name
  automation::AutomationProperties::SetName(addressBox_, Loc("usdc_wallet_address"));
  addressBox_.TextChanged([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->OnAddressChanged();
  });
  manualPanel_.Children().Append(addressBox_);
  // the field's supporting line (WalletAddressVerdictText's idiom)
  verdictText_ = StyledText(hstring{}, L"UrRowNoteStyle", /*wrap=*/true);
  verdictText_.Visibility(Visibility::Collapsed);
  manualPanel_.Children().Append(verdictText_);
  connectButton_ = ActionButton(Loc("connect"), L"UrPaneActionSecondaryStyle");
  connectButton_.IsEnabled(false);
  connectButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->SubmitManual();
  });
  manualPanel_.Children().Append(connectButton_);
  content.Children().Append(manualPanel_);

  // A failure renders HERE, on the sheet: a snackbar raised behind a modal is a
  // message the user cannot read.
  errorText_ = StyledText(hstring{}, L"UrRowNoteStyle", /*wrap=*/true);
  errorText_.Foreground(colors::DangerBrush());
  errorText_.Visibility(Visibility::Collapsed);
  automation::AutomationProperties::SetLiveSetting(errorText_,
                                                   peers::AutomationLiveSetting::Assertive);
  content.Children().Append(errorText_);

  dialog_.Content(content);

  auto queue = dialog_.DispatcherQueue();
  // debounce the address check while typing: each keystroke restarts the
  // window, and only the pause checks
  debounceTimer_ = queue.CreateTimer();
  debounceTimer_.Interval(std::chrono::milliseconds(kCheckDebounceMs));
  debounceTimer_.IsRepeating(false);
  debounceTimer_.Tick([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->CheckAddress();
  });
  // WalletConnect has no timeout, and its on_error only fires when the deep link
  // comes BACK carrying an error. A closed browser tab produces nothing at all,
  // and a request can drop its callback, so this is what gives the controls back.
  watchdog_ = queue.CreateTimer();
  watchdog_.IsRepeating(false);
  watchdog_.Tick([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->OnWatchdog();
  });

  // Dismissed mid-flow (Cancel, Esc): whatever is out is stale from here on.
  dialog_.Closed([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) {
      // a create call still out may land all the same, and its answer finds no sheet
      const bool linking = self->machine_.state == solana::ConnectState::Linking;
      self->closed_ = true;
      ++self->flowGeneration_;
      ++self->checkGeneration_;
      self->StopTimers();
      if (linking && self->onAbandonedLink_) self->onAbandonedLink_();
    }
  });

  Render();
}

void ConnectSolanaWalletSheet::ToggleManual() {
  manualOpen_ = !manualOpen_;
  manualPanel_.Visibility(manualOpen_ ? Visibility::Visible : Visibility::Collapsed);
  if (manualOpen_) addressBox_.Focus(FocusState::Programmatic);
}

// ---- a wallet app ------------------------------------------------------------

void ConnectSolanaWalletSheet::StartBridge(WalletConnect::Provider provider) {
  if (closed_) return;
  // The buttons are disabled without a session, so reaching this is a bug
  // rather than a user path - which is why it says so instead of just returning.
  if (!allowActions_) {
    urnw::LogError("solana wallet: refusing a wallet connect - the sheet was opened with no session");
    return;
  }
  if (!solana::ChooseProvider(machine_)) return;  // a round trip is already out
  const uint32_t generation = ArmFlow(kBridgeTimeoutMs);
  Render();

  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.ConnectSolanaWallet(
      provider, [queue, weak, generation](bool ok, std::string address, std::string error) {
        // on whichever thread delivered the deep link
        queue.TryEnqueue([weak, generation, ok, address, error] {
          if (auto self = weak.lock()) self->ApplyPublicKey(generation, ok, address, error);
        });
      });
}

void ConnectSolanaWalletSheet::ApplyPublicKey(uint32_t generation, bool ok,
                                              std::string const& address,
                                              std::string const& error) {
  if (closed_ || generation != flowGeneration_) {
    urnw::LogWarn("solana wallet: dropping a wallet connect answer for an abandoned request (ok={})",
                  ok);
    return;
  }
  if (!ok) {
    watchdog_.Stop();
    urnw::LogError("solana wallet: wallet connect failed: {}", error);
    solana::BridgeError(machine_, error);
    Render();
    return;
  }
  if (!solana::PublicKey(machine_)) return;
  // Straight to the account, unchecked here: the server validates the key on
  // create, as the android and apple originals relied on.
  Link(address);
}

// ---- a pasted address --------------------------------------------------------

void ConnectSolanaWalletSheet::OnAddressChanged() {
  if (closed_) return;
  ++checkGeneration_;  // drop any check still in flight
  solana::Typed(machine_);
  Render();
  debounceTimer_.Stop();  // restart the debounce window on every keystroke
  debounceTimer_.Start();
}

// Syntax first, locally: a typo never reaches the network. Then the server, for
// SOL only - the menu item names Solana.
void ConnectSolanaWalletSheet::CheckAddress() {
  if (closed_) return;
  const std::string text = winrt::to_string(addressBox_.Text());
  const uint32_t generation = ++checkGeneration_;
  // --preview-ui asks the network for nothing, and a build with no api has
  // nothing to ask
  const bool askServer = allowActions_ && sdk_.apiReady();
  const bool ask = solana::Debounced(machine_, text, askServer);
  Render();
  if (!ask) return;

  urnet::WalletValidateAddressArgs args;
  args.address = machine_.address;
  args.chain = urnet::SOL;
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().walletValidateAddress(
      args, [queue, weak, generation](std::optional<urnet::WalletValidateAddressResult> result,
                                      std::optional<std::string> err) {
        // "this is not a Solana address" and "the question got no answer" are
        // different lines: a failing validator must not look like a typo
        solana::ServerVerdict verdict = solana::ServerVerdict::Unavailable;
        if (err) {
          urnw::LogError("solana wallet: walletValidateAddress(SOL) failed: {}", *err);
        } else if (result) {
          verdict = result->valid.value_or(false) ? solana::ServerVerdict::Valid
                                                  : solana::ServerVerdict::Invalid;
        } else {
          urnw::LogError("solana wallet: walletValidateAddress(SOL) returned no result");
        }
        queue.TryEnqueue([weak, generation, verdict] {
          if (auto self = weak.lock()) self->ApplyVerdict(generation, verdict);
        });
      });
}

void ConnectSolanaWalletSheet::ApplyVerdict(uint32_t generation, solana::ServerVerdict verdict) {
  if (closed_ || generation != checkGeneration_) return;  // a later edit superseded this
  solana::Verdict(machine_, verdict);
  Render();
}

void ConnectSolanaWalletSheet::SubmitManual() {
  if (closed_) return;
  if (!allowActions_) {
    urnw::LogError("solana wallet: refusing an api write - the sheet was opened with no session");
    return;
  }
  if (!solana::Submit(machine_)) return;
  Link(machine_.address);
}

// ---- either way ----------------------------------------------------------------

// WalletViewController::addExternalWallet parity: the account wallet on SOL,
// with the USDC token type.
void ConnectSolanaWalletSheet::Link(std::string const& address) {
  const uint32_t generation = ArmFlow(kApiTimeoutMs);
  Render();

  urnet::CreateAccountWalletArgs args;
  args.blockchain = urnet::SOL;
  args.wallet_address = address;
  args.default_token_type = kUsdcTokenType;
  auto queue = dialog_.DispatcherQueue();
  auto weak = weak_from_this();
  sdk_.api().createAccountWallet(
      args, [queue, weak, generation](std::optional<urnet::CreateAccountWalletResult> result,
                                      std::optional<std::string> err) {
        const std::string walletId =
            (result && result->wallet_id) ? *result->wallet_id : std::string();
        const bool ok = !err && !walletId.empty();
        // a server error is not localizable; it is shown as the reason when
        // there is one
        const std::string error = err ? *err : std::string();
        if (!ok) urnw::LogError("solana wallet: createAccountWallet failed: {}", error);
        queue.TryEnqueue([weak, generation, ok, walletId, error] {
          if (auto self = weak.lock()) self->ApplyCreateResult(generation, ok, walletId, error);
        });
      });
}

void ConnectSolanaWalletSheet::ApplyCreateResult(uint32_t generation, bool ok,
                                                 std::string const& walletId,
                                                 std::string const& error) {
  if (closed_ || generation != flowGeneration_) {
    urnw::LogWarn("solana wallet: dropping a create result for an abandoned request (ok={})", ok);
    return;
  }
  watchdog_.Stop();
  solana::CreateResult(machine_, ok, walletId, error);
  Render();
  if (machine_.state != solana::ConnectState::Linked) return;
  // the sheet closes, so the page says what happened
  if (onConnected_) onConnected_(walletId);
  dialog_.Hide();
}

uint32_t ConnectSolanaWalletSheet::ArmFlow(int timeoutMs) {
  const uint32_t generation = ++flowGeneration_;
  watchdog_.Stop();
  watchdog_.Interval(std::chrono::milliseconds(timeoutMs));
  watchdog_.Start();
  return generation;
}

void ConnectSolanaWalletSheet::OnWatchdog() {
  if (closed_) return;
  if (!solana::Timeout(machine_)) return;
  // Bumping the generation is what makes giving up final: the real answer,
  // whenever it turns up, no longer matches and is dropped.
  ++flowGeneration_;
  urnw::LogError("solana wallet: a wallet connect request never answered - giving up on it");
  Render();
}

void ConnectSolanaWalletSheet::StopTimers() {
  if (debounceTimer_) debounceTimer_.Stop();
  if (watchdog_) watchdog_.Stop();
}

void ConnectSolanaWalletSheet::Render() {
  const bool providers = allowActions_ && solana::ProvidersEnabled(machine_);
  phantomButton_.IsEnabled(providers);
  solflareButton_.IsEnabled(providers);
  addressBox_.IsEnabled(solana::EntryEnabled(machine_));
  connectButton_.IsEnabled(allowActions_ && solana::ConnectEnabled(machine_));

  const hstring statusBefore = statusText_.Text();
  kit::SetTextOrCollapse(statusText_, KeyText(solana::StatusKey(machine_)));
  AnnounceIfChanged(statusText_, statusBefore);

  // muted while checking, danger for a refusal
  kit::SetTextOrCollapse(verdictText_, KeyText(solana::CheckKey(machine_)));
  verdictText_.Foreground(solana::CheckIsError(machine_) ? colors::DangerBrush()
                                                         : colors::MutedBrush());

  const hstring failureBefore = errorText_.Text();
  kit::SetTextOrCollapse(errorText_, machine_.state == solana::ConnectState::Failed
                                         ? SolanaFailureText(machine_.detail)
                                         : hstring{});
  AnnounceIfChanged(errorText_, failureBefore);
}

}  // namespace urnw
