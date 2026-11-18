// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "BrowserApprovalSheet.h"

#include <chrono>
#include <optional>
#include <utility>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.System.h>

#include "AppController.h"
#include "Config.h"
#include "Localization.h"
#include "Log.h"
#include "PageContext.h"
#include "SessionLimitNotice.h"  // a sign-in the account's session cap refused
#include "SheetFit.h"  // sheetfit: sheets clamp to the window at open time
#include "Strings.h"
#include "UrColors.h"
#include "UrComponents.h"
#include "UrIoApproval.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace urnw::pages;

// NOTE on captures: control event handlers capture the owning sheet weakly (see
// StatsSheets.cpp). The page that hosts the sheet holds its shared_ptr while the dialog is
// showing, so lock() always succeeds during interaction; an answer from the SDK, which
// arrives after the sheet may be gone, finds out the same way.

namespace urnw {
namespace {

// The paste flow's button opens the site to sign in at, as this sheet always has.
constexpr const wchar_t* kManualSignInUrl = L"https://ur.io";

// A copied link is taken off the clipboard this long after, unless something else was copied since.
constexpr std::chrono::seconds kClipboardLife{60};

// The launcher's own verdict, observed. Defined at the end of the file.
winrt::Windows::Foundation::IAsyncOperation<bool> LaunchInBrowser(hstring url);

// ---- the strings --------------------------------------------------------------
// Every one of these is a key the localization store tags windows; the English beside an Adv
// lookup is the store's. There is no check code and no string for one.

hstring TitleText() { return Adv("adv_browser_sign_in_title", L"Browser sign-in"); }

// The action ("Continue in browser" in the docs): starts the approval where it is offered, opens
// the site to sign in at where it is not.
hstring ActionText() { return Adv("adv_open_ur_io", L"Open ur.io"); }

hstring BodyText() {
  return Adv("adv_browser_sign_in_body",
             L"Sign in at ur.io in your browser — your password manager and existing "
             L"Google or Apple sessions work there. Then paste a one-time auth code "
             L"from any signed-in device (Account → Create auth code).");
}

// ---- small builders -----------------------------------------------------------

// An app or system style by key, or null: the sheets' own idiom (AuthSheets.cpp), which finds the
// keys of the merged dictionaries too.
Style StyleNamed(std::wstring_view key) {
  auto app = Application::Current();
  if (!app) return nullptr;
  return app.Resources().TryLookup(winrt::box_value(hstring{key})).try_as<Style>();
}

TextBlock MakeText(hstring const& text, double fontSize,
                   winrt::Microsoft::UI::Xaml::Media::Brush const& brush = nullptr,
                   bool wrap = true) {
  TextBlock block;
  block.Text(text);
  block.FontSize(fontSize);
  if (brush) block.Foreground(brush);
  if (wrap) block.TextWrapping(TextWrapping::Wrap);
  return block;
}

// The command bar of a page. ContentDialog splits it evenly between the buttons it shows, so a
// page keeps to short words in it (the trap NetworkServerSheet's comment records); an empty
// text hides the button.
void SetBar(ContentDialog const& dialog, hstring const& primary, hstring const& secondary,
            hstring const& close, ContentDialogButton defaultButton) {
  dialog.PrimaryButtonText(primary);
  dialog.SecondaryButtonText(secondary);
  dialog.CloseButtonText(close);
  dialog.DefaultButton(defaultButton);
}

// Anything the person copied since is theirs: the link comes off the clipboard only if it is still
// the text that is there. Runs on the UI thread, where the clipboard is read.
winrt::fire_and_forget ClearClipboardIfUnchanged(std::string copied) {
  namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
  try {
    const dt::DataPackageView content = dt::Clipboard::GetContent();
    if (content.Contains(dt::StandardDataFormats::Text())) {
      const hstring now = co_await content.GetTextAsync();
      if (Narrow(now.c_str()) == copied) dt::Clipboard::Clear();
    }
  } catch (winrt::hresult_error const&) {
    // the clipboard is busy or gone; nothing this knows of is left on it
  }
  approval::Wipe(copied);
}

winrt::fire_and_forget ClearClipboardLater(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue,
                                           std::string copied) {
  co_await winrt::resume_after(kClipboardLife);  // resumes on a thread-pool thread
  queue.TryEnqueue([copied = std::move(copied)]() mutable {
    ClearClipboardIfUnchanged(std::move(copied));
  });
}

}  // namespace

// ---- BrowserApprovalSheet -----------------------------------------------------

std::shared_ptr<BrowserApprovalSheet> BrowserApprovalSheet::Create(XamlRoot const& root) {
  auto sheet = std::shared_ptr<BrowserApprovalSheet>(new BrowserApprovalSheet());
  // Asked once, here: the page check and the process do not change while the sheet is open, and
  // a sheet that began as the paste flow stays that, rather than growing a button under the
  // person's cursor.
  sheet->approvalOffered_ = urnw::App().ApprovalOffered();
  sheet->Build(root);
  return sheet;
}

bool BrowserApprovalSheet::Open() const {
  return !closed_ && urnw::App().WindowHides() == hides_;
}

void BrowserApprovalSheet::Build(XamlRoot const& root) {
  hides_ = urnw::App().WindowHides();
  dialog_ = ContentDialog();
  dialog_.XamlRoot(root);
  dialog_.Background(colors::SheetBrush());

  // One cell, every page in it, one visible at a time (UpgradeSheet's pages share a Grid the same way).
  Grid content;
  content.MinWidth(sheetfit::Width(root, 400));
  BuildStart();
  BuildWaiting();
  BuildSigningIn();
  BuildConfirm();
  BuildMessage();
  content.Children().Append(startPanel_);
  content.Children().Append(waitingPanel_);
  content.Children().Append(signingInPanel_);
  content.Children().Append(confirmPanel_);
  content.Children().Append(messagePanel_);

  ScrollViewer scroll;
  scroll.Content(content);
  scroll.MaxHeight(sheetfit::Height(root, 560));
  dialog_.Content(scroll);

  // Every button of a page but the one that closes the sheet leads to another page, so none of
  // them is allowed to close it.
  dialog_.PrimaryButtonClick([weak = weak_from_this()](
                                 auto const&, ContentDialogButtonClickEventArgs const& args) {
    args.Cancel(true);
    if (auto self = weak.lock()) self->OnPrimary();
  });
  // Sign in with a pasted code is the one button that does close the sheet: the page that
  // hosts it signs in once the sheet is gone, as it has always done.
  dialog_.SecondaryButtonClick(
      [weak = weak_from_this()](auto const&, ContentDialogButtonClickEventArgs const&) {
        if (auto self = weak.lock()) self->SubmitPastedCode();
      });
  dialog_.Opened([weak = weak_from_this()](auto const&, auto const&) {
    auto self = weak.lock();
    // the paste flow opens on its field, as it always has
    if (self && !self->approvalOffered_ && self->page_ == Page::Start) {
      self->pasteBox_.Focus(FocusState::Programmatic);
    }
  });
  dialog_.Closed([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->OnClosed();
  });

  ShowPage(Page::Start);
}

void BrowserApprovalSheet::BuildStart() {
  startPanel_ = StackPanel();
  startPanel_.Spacing(12);

  // The one action of the page: the browser, and where it goes, as plain text beside it. With
  // the approval it starts the attempt and the host is the page's; without it the button opens
  // the site to sign in at, and the body below names it.
  Grid row;
  row.ColumnSpacing(12);
  ColumnDefinition buttonColumn;
  buttonColumn.Width(GridLength{0, GridUnitType::Auto});
  ColumnDefinition hostColumn;
  hostColumn.Width(GridLength{1, GridUnitType::Star});
  row.ColumnDefinitions().Append(buttonColumn);
  row.ColumnDefinitions().Append(hostColumn);
  actionButton_ = Button();
  actionButton_.Content(winrt::box_value(ActionText()));
  // the accent belongs to the approval; the paste flow's accent stays on its Sign in
  if (approvalOffered_) {
    if (auto style = StyleNamed(L"AccentButtonStyle")) actionButton_.Style(style);
  }
  actionButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->OnAction();
  });
  row.Children().Append(actionButton_);
  if (approvalOffered_) {
    TextBlock host = MakeText(H(std::string(config::kApprovalHost)), 12, colors::MutedBrush(), false);
    host.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(host, 1);
    row.Children().Append(host);
  }

  startNote_ = MakeText(hstring{}, 12, colors::DangerBrush());
  startNote_.Visibility(Visibility::Collapsed);

  // wrapped, muted, small - NetworkServerSheet's description is the reference
  TextBlock body = MakeText(BodyText(), 12, colors::MutedBrush());

  // The field holds a credential: a password box keeps it out of the UI Automation tree, which
  // any process on the machine can read, and it is emptied the moment it is submitted or left.
  pasteBox_ = PasswordBox();
  pasteBox_.PlaceholderText(Loc("auth_code"));
  Automation::AutomationProperties::SetName(pasteBox_, Loc("auth_code"));
  pasteBox_.PasswordRevealMode(PasswordRevealMode::Hidden);
  if (auto style = StyleNamed(L"UrPasswordInputStyle")) pasteBox_.Style(style);

  if (approvalOffered_) {
    startPanel_.Children().Append(row);
    startPanel_.Children().Append(startNote_);
    TextBlock either = MakeText(Loc("or"), 12, colors::MutedBrush(), false);
    either.HorizontalAlignment(HorizontalAlignment::Center);
    startPanel_.Children().Append(either);
    startPanel_.Children().Append(body);
  } else {
    startPanel_.Children().Append(body);
    startPanel_.Children().Append(row);
    startPanel_.Children().Append(startNote_);
  }
  startPanel_.Children().Append(pasteBox_);
}

void BrowserApprovalSheet::BuildWaiting() {
  waitingPanel_ = StackPanel();
  waitingPanel_.Spacing(12);
  waitingPanel_.Margin(Thickness{0, 12, 0, 12});

  ProgressRing ring;
  ring.Width(36);
  ring.Height(36);
  ring.IsActive(true);
  ring.HorizontalAlignment(HorizontalAlignment::Center);
  waitingPanel_.Children().Append(ring);

  TextBlock title = MakeText(Loc("waiting_for_approval"), 18, colors::TextBrush());
  title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  title.HorizontalAlignment(HorizontalAlignment::Center);
  waitingPanel_.Children().Append(title);

  // where the browser was sent: plain text from the build constant, never anything a link carried
  TextBlock host = MakeText(H(std::string(config::kApprovalHost)), 12, colors::MutedBrush());
  host.HorizontalAlignment(HorizontalAlignment::Center);
  host.TextAlignment(TextAlignment::Center);
  waitingPanel_.Children().Append(host);

  // if the launcher said no: the reason. The link to open by hand is the Copy button's, and is not
  // shown: text on a page is readable by every program of the session, and the link carries the
  // attempt's state.
  launchError_ = MakeText(hstring{}, 12, colors::DangerBrush());
  launchError_.TextAlignment(TextAlignment::Center);
  launchError_.Visibility(Visibility::Collapsed);
  waitingPanel_.Children().Append(launchError_);

  StackPanel buttons;
  buttons.Orientation(Orientation::Horizontal);
  buttons.Spacing(8);
  buttons.HorizontalAlignment(HorizontalAlignment::Center);
  againButton_ = Button();
  againButton_.Content(winrt::box_value(ActionText()));
  againButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->OnAction();
  });
  buttons.Children().Append(againButton_);
  copyButton_ = Button();
  copyButton_.Content(winrt::box_value(Loc("copy_to_clipboard")));
  copyButton_.Click([weak = weak_from_this()](auto const&, auto const&) {
    if (auto self = weak.lock()) self->CopyLink();
  });
  buttons.Children().Append(copyButton_);
  waitingPanel_.Children().Append(buttons);
}

void BrowserApprovalSheet::BuildSigningIn() {
  signingInPanel_ = StackPanel();
  signingInPanel_.Margin(Thickness{0, 24, 0, 24});
  ProgressRing ring;
  ring.Width(36);
  ring.Height(36);
  ring.IsActive(true);
  ring.HorizontalAlignment(HorizontalAlignment::Center);
  Automation::AutomationProperties::SetName(ring, Loc("sign_in"));
  signingInPanel_.Children().Append(ring);
}

void BrowserApprovalSheet::BuildConfirm() {
  confirmPanel_ = StackPanel();
  confirmPanel_.Spacing(4);
  confirmPanel_.Children().Append(MakeText(Loc("network_name_label"), 12, colors::MutedBrush()));
  // the name of the network the answer would sign this computer in to, filtered by the caller
  networkText_ = MakeText(hstring{}, 18, colors::TextBrush());
  networkText_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  confirmPanel_.Children().Append(networkText_);
}

void BrowserApprovalSheet::BuildMessage() {
  messagePanel_ = StackPanel();
  messagePanel_.Margin(Thickness{0, 12, 0, 12});
  messageText_ = MakeText(hstring{}, 14, colors::MutedBrush());
  messagePanel_.Children().Append(messageText_);
}

void BrowserApprovalSheet::ShowPage(Page page) {
  // What a page leaves behind: the paste field holds a credential, and the link is a secret
  // until it has been opened.
  if (page != Page::Start) pasteBox_.Password(L"");
  if (page != Page::Waiting) {
    approval::Wipe(link_);
    launchError_.Visibility(Visibility::Collapsed);
  }
  if (page == Page::Start) {
    startNote_.Visibility(Visibility::Collapsed);
    actionButton_.IsEnabled(!starting_);
  }
  // "again" waits for the launcher's answer to the click before it, if there is one
  if (page == Page::Waiting) againButton_.IsEnabled(!starting_);
  page_ = page;
  const auto visibleWhen = [page](Page shown) {
    return page == shown ? Visibility::Visible : Visibility::Collapsed;
  };
  startPanel_.Visibility(visibleWhen(Page::Start));
  waitingPanel_.Visibility(visibleWhen(Page::Waiting));
  signingInPanel_.Visibility(visibleWhen(Page::SigningIn));
  confirmPanel_.Visibility(visibleWhen(Page::Confirm));
  messagePanel_.Visibility(page == Page::TimedOut || page == Page::Failed ? Visibility::Visible
                                                                         : Visibility::Collapsed);

  const hstring none;
  switch (page) {
    case Page::Start:
      SetBar(dialog_, none, Loc("sign_in"), Loc("cancel"), ContentDialogButton::Secondary);
      dialog_.Title(winrt::box_value(TitleText()));
      break;
    case Page::Waiting:
      SetBar(dialog_, none, none, Loc("cancel"), ContentDialogButton::None);
      dialog_.Title(winrt::box_value(TitleText()));
      break;
    case Page::SigningIn:
      // once the person has said yes there is nothing left to cancel
      SetBar(dialog_, none, none, confirmed_ ? none : Loc("cancel"), ContentDialogButton::None);
      dialog_.Title(winrt::box_value(TitleText()));
      break;
    case Page::Confirm:
      // no default: a stray Enter is not the person's yes
      SetBar(dialog_, Loc("sign_in"), none, Loc("cancel"), ContentDialogButton::None);
      dialog_.Title(winrt::box_value(Loc("sign_in")));
      break;
    case Page::TimedOut:
    case Page::Failed:
      SetBar(dialog_, Loc("try_again"), none, Loc("cancel"), ContentDialogButton::Primary);
      dialog_.Title(winrt::box_value(TitleText()));
      break;
  }
}

void BrowserApprovalSheet::ShowStartNote(hstring const& message) {
  startNote_.Text(message);
  startNote_.Visibility(Visibility::Visible);
}

void BrowserApprovalSheet::ShowFailed(hstring const& message) {
  messageText_.Text(message);
  messageText_.Foreground(colors::DangerBrush());
  ShowPage(Page::Failed);
}

void BrowserApprovalSheet::ShowTimedOut() {
  // only a sheet that is waiting is told: anything else is on a page of its own
  if (closed_ || page_ != Page::Waiting) return;
  LogInfo("browser sign-in sheet: the attempt timed out");
  messageText_.Text(Loc("sso_sign_in_timed_out"));
  messageText_.Foreground(colors::MutedBrush());
  ShowPage(Page::TimedOut);
}

void BrowserApprovalSheet::ShowDenied() {
  // the person cancelled on the page: nothing was shared and nothing is signed in, so the sheet
  // is back where it began
  if (closed_ || (page_ != Page::Waiting && page_ != Page::Start)) return;
  ShowPage(Page::Start);
}

void BrowserApprovalSheet::ShowConfirm(std::string const& networkName) {
  networkText_.Text(H(networkName));
  ShowPage(Page::Confirm);
}

std::string BrowserApprovalSheet::TakePastedCode() {
  std::string code = std::move(pasted_);
  pasted_.clear();
  return code;
}

void BrowserApprovalSheet::OnAction() {
  if (approvalOffered_) {
    StartApproval();
  } else {
    OpenManualSignIn();
  }
}

winrt::fire_and_forget BrowserApprovalSheet::StartApproval() {
  auto self = shared_from_this();  // the sheet outlives its own wait for the launcher
  if (starting_ || closed_) co_return;
  starting_ = true;
  // Nobody awaits this coroutine, and an exception that leaves one ends the app: whatever a page
  // cannot be told (a window that is going away) is dropped with a line in the log.
  try {
    actionButton_.IsEnabled(false);
    againButton_.IsEnabled(false);

    // A second click reads the live attempt back (it is not extended) while the page would still
    // accept its request. With none live, or one the page would refuse by now, this starts one,
    // once the user's urnetwork:// handler starts this exe and the shell says so, and supersedes
    // whatever waited for the browser or a wallet. The View is a secret until the request it
    // builds has been opened, so it is wiped as soon as the request exists.
    std::optional<approval::Slot::View> view = urnw::App().BeginApproval();
    std::string url;
    if (view) {
      url = urnw::App().ApprovalRequestUrl(*view);
      approval::Wipe(view->state);
      approval::Wipe(view->publicKey);
    }
    if (url.empty()) {
      // an attempt that cannot be opened is not left alive
      if (view) urnw::App().CancelApproval(approval::ClearReason::Cancel);
      LogWarn("browser sign-in sheet: the approval could not be started; the paste flow stays");
      starting_ = false;
      againButton_.IsEnabled(true);
      ShowPage(Page::Start);
      ShowStartNote(Loc("something_went_wrong"));
      co_return;
    }
    link_ = std::move(url);
    ShowPage(Page::Waiting);

    // Observed, not fire-and-forget: a page that says it waits for an approval nobody can give,
    // because no browser opened, is worse than one that says the browser did not open.
    const bool launched = co_await LaunchInBrowser(H(link_));
    starting_ = false;
    if (closed_ || page_ != Page::Waiting) co_return;  // the answer, a timeout or a cancel got there first
    actionButton_.IsEnabled(true);
    againButton_.IsEnabled(true);
    if (launched) {
      launchError_.Visibility(Visibility::Collapsed);
      LogInfo("browser sign-in sheet: the browser was opened on the approval page");
      co_return;
    }
    // the attempt stays: the link opened by hand in any browser on this computer is answered the
    // same way, and Copy is how it gets there
    LogWarn("browser sign-in sheet: the browser could not be opened; the link can be copied");
    launchError_.Text(Loc("sso_browser_open_failed"));
    launchError_.Visibility(Visibility::Visible);
  } catch (...) {
    starting_ = false;
    LogWarn("browser sign-in sheet: the approval page could not be updated");
  }
}

winrt::fire_and_forget BrowserApprovalSheet::OpenManualSignIn() {
  auto self = shared_from_this();
  if (starting_ || closed_) co_return;
  starting_ = true;
  try {
    actionButton_.IsEnabled(false);
    const bool launched = co_await LaunchInBrowser(hstring{kManualSignInUrl});
    starting_ = false;
    if (closed_) co_return;
    actionButton_.IsEnabled(true);
    if (launched) {
      startNote_.Visibility(Visibility::Collapsed);
      co_return;
    }
    LogWarn("browser sign-in sheet: the browser could not be opened");
    ShowStartNote(Loc("sso_browser_open_failed"));
  } catch (...) {
    starting_ = false;
    LogWarn("browser sign-in sheet: the start page could not be updated");
  }
}

void BrowserApprovalSheet::CopyLink() {
  if (link_.empty()) return;
  namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
  try {
    dt::DataPackage package;
    package.SetText(H(link_));
    // The link carries the attempt's state, so it stays out of Clipboard History (Win+V, readable
    // by anything that asks) and off the cloud clipboard. Both flags are OFF by default: plain
    // Clipboard::SetContent opts into both, and is not used here.
    dt::ClipboardContentOptions options;
    options.IsAllowedInHistory(false);
    options.IsRoamable(false);
    if (!dt::Clipboard::SetContentWithOptions(package, options)) {
      // not the fallback to the plain call: that is the leak this exists to close
      LogError("browser sign-in sheet: the clipboard refused a history-excluded copy");
      return;
    }
  } catch (winrt::hresult_error const&) {
    LogWarn("browser sign-in sheet: the clipboard could not be written");
    return;
  }
  ClearClipboardLater(dialog_.DispatcherQueue(), link_);
}

void BrowserApprovalSheet::OnPrimary() {
  switch (page_) {
    case Page::Confirm:
      Confirm();
      break;
    case Page::TimedOut:
    case Page::Failed:
      // back to the start: the next attempt is the person's click
      ShowPage(Page::Start);
      break;
    case Page::Start:
    case Page::Waiting:
    case Page::SigningIn:
      break;
  }
}

void BrowserApprovalSheet::BeginRedeem(std::string code) {
  if (!Open()) {
    approval::Wipe(code);
    return;
  }
  confirmed_ = false;
  ShowPage(Page::SigningIn);
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
  // An SDK thread answers, and the page is touched on the UI thread only. An answer that finds the
  // sheet gone has nothing the person said yes to: the network it redeemed is dropped.
  Sdk().RedeemApprovedCode(code, [weak, queue](SdkHost::ApprovedNetwork answer) {
    queue.TryEnqueue([weak, answer = std::move(answer)]() mutable {
      if (auto self = weak.lock()) {
        self->OnRedeemed(std::move(answer));
      } else if (answer.ok) {
        Sdk().DiscardApprovedNetwork();
      }
    });
  });
  approval::Wipe(code);
}

void BrowserApprovalSheet::OnRedeemed(SdkHost::ApprovedNetwork answer) {
  if (closed_) {
    // closed while the code was out: nothing here has the person's yes
    if (answer.ok) Sdk().DiscardApprovedNetwork();
    return;
  }
  if (!answer.ok) {
    // never retried: a spent or refused code is spent, and the server answers an invalid, an
    // expired and a used one the same way
    LogWarn("browser sign-in sheet: the approval could not be redeemed");
    // The account's cap of active sessions refused the sign-in the code was for (SdkHost's token,
    // SessionLimitNotice.h): what to do, not a bad code. The person approves again once a session
    // is signed out.
    if (const char* key = sessionlimit::SignInRefusalMessageKey(answer.error)) {
      ShowFailed(Loc(key));
      return;
    }
    ShowFailed(answer.failure == SdkHost::ApprovedNetwork::Failure::Transport
                   ? Loc("something_went_wrong")
                   : Loc("auth_code_error"));
    return;
  }
  // The name is the network's own, as the credential carries it: filtered like every external name
  // the chrome shows. A name with nothing left to show is nothing the person can say yes to.
  const std::string name = kit::SanitizeExternalDisplayText(answer.networkName);
  if (name.empty()) {
    Sdk().DiscardApprovedNetwork();
    LogWarn("browser sign-in sheet: the approval named no network the person could recognise");
    ShowFailed(Loc("auth_code_error"));
    return;
  }
  ShowConfirm(name);
}

void BrowserApprovalSheet::Confirm() {
  confirmed_ = true;
  ShowPage(Page::SigningIn);
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
  Sdk().ConfirmApprovedNetwork([weak, queue](AuthResult result) {
    queue.TryEnqueue([weak, result = std::move(result)] {
      if (auto self = weak.lock()) self->OnConfirmed(result);
    });
  });
}

void BrowserApprovalSheet::OnConfirmed(AuthResult const& result) {
  if (closed_) return;  // the sign-in goes on without the sheet, and is told as any sign-in is
  if (result.ok) {
    // the auth state has swapped the window for the home view, and the tray says Signed in
    Finish();
    return;
  }
  // the server's own text, as every other sign-in failure shows it; the code is spent, so the
  // person starts again
  LogWarn("browser sign-in sheet: the device could not be registered");
  // a refusal the sign-in screens have words for (the account's session cap, the network's client
  // limit: SdkHost's tokens, SessionLimitNotice.h) says what to do, never its token
  if (const char* key = sessionlimit::SignInRefusalKey(result.error)) {
    ShowFailed(Loc(key));
    return;
  }
  ShowFailed(result.error.empty() ? Loc("something_went_wrong") : H(result.error));
}

void BrowserApprovalSheet::SubmitPastedCode() {
  pasted_ = Narrow(pasteBox_.Password().c_str());
  pasteBox_.Password(L"");
}

void BrowserApprovalSheet::OnClosed() {
  closed_ = true;
  pasteBox_.Password(L"");
  approval::Wipe(link_);
  // A network redeemed for a confirm that never came is not signed in to.
  if (page_ == Page::Confirm && !confirmed_) {
    Sdk().DiscardApprovedNetwork();
    LogInfo("browser sign-in sheet: closed without a yes; the redeemed network was dropped");
  }
  // The person closing the sheet cancels the attempt behind it. A hide to the tray does not: it
  // counted itself (WindowHides) before it swept the sheets, whenever this event arrives, and the
  // browser may still answer.
  if (urnw::App().WindowHides() == hides_) urnw::App().CancelApproval(approval::ClearReason::Cancel);
}

void BrowserApprovalSheet::Finish() {
  if (!closed_) dialog_.Hide();
}

// ---- the launcher ---------------------------------------------------------------

namespace {

winrt::Windows::Foundation::IAsyncOperation<bool> LaunchInBrowser(hstring url) {
  bool launched = false;
  try {
    launched = co_await winrt::Windows::System::Launcher::LaunchUriAsync(
        winrt::Windows::Foundation::Uri(url));
  } catch (...) {
    // the url is a secret and is not logged; the caller says that the browser did not open
    launched = false;
  }
  co_return launched;
}

}  // namespace

}  // namespace urnw
