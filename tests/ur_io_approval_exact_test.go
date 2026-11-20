// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"strings"
	"testing"
)

// The small functions that DECIDE in the browser approval's Windows code, pinned to the statements they may hold. The
// pins in ur_io_approval_wiring_test.go check that fragments of a function occur in order; they cannot see a branch whose
// test was turned round, a guard that was removed, a delivery that was dropped or two fields that were swapped, and each
// of those changes what the app does with a sign-in: whether the action is offered where it must not be, what the person
// who decides which network their computer joins is shown, which half of a key pair is sent to a browser, whether a
// credential is registered before anyone said yes. The gate (HandleAuthLink) is pinned the same way in
// auth_link_wiring_test.go; this is the same allow-list for the functions around it. The text of each function below
// is the whole text it may have (comments and whitespace aside), so a change to one is a change to this table made on
// purpose, with the review that deserves. Layout and wiring that decide nothing (the larger builders of the sheet, whose
// controls and margins are free but whose panels and appends are pinned in ur_io_approval_wiring_test.go, the clipboard,
// the launcher's coroutine, the log lines of the page check) are pinned by the wiring tests, not here.

func urIoLogin(t *testing.T) string {
	return stripComments(lf(readAppSource(t, "LoginPage.cpp")))
}

func urIoSheet(t *testing.T) string {
	return stripComments(lf(readAppSource(t, "BrowserApprovalSheet.cpp")))
}

func urIoProtocol(t *testing.T) string {
	return stripComments(lf(readAppSource(t, "ProtocolHandler.cpp")))
}

func urIoUnseal(t *testing.T) string {
	return stripComments(lf(readAppSource(t, "ApprovalUnseal.h")))
}

func urIoWallet(t *testing.T) string {
	return stripComments(lf(readAppSource(t, "WalletConnect.cpp")))
}

func urIoCheck(t *testing.T) string {
	return stripComments(lf(readAppSource(t, "ApprovalCheck.cpp")))
}

// A definition and the text it must have: the file it is in, how that file is read, a name for the case, the line its
// definition starts with, and the definition.
type urIoExactPin struct {
	file      string
	read      func(*testing.T) string
	name      string
	signature string
	want      string
}

var urIoExactPins = []urIoExactPin{
	// the word the log gets for a link: a helper every auth link passes, stray or not, that raises, shows and signs in nothing
	{"AppController.cpp", urIoController, "AuthLinkWhy", "std::string_view AuthLinkWhy(const authlink::Return& link, const approval::Decision& decision) {", `
std::string_view AuthLinkWhy(const authlink::Return& link, const approval::Decision& decision) {
  const bool parserKnowsBetter = decision.why == approval::Why::NoAttempt || decision.why == approval::Why::InvalidShape;
  if (link.kind == authlink::Kind::Invalid && parserKnowsBetter) return authlink::ToString(link.reject);
  return approval::ToString(decision.why);
}`},
	// the attempt's state comes from the OS and from nothing else
	{"AppController.cpp", urIoController, "RandomBytes", "bool RandomBytes(unsigned char* out, std::size_t length) {", `
bool RandomBytes(unsigned char* out, std::size_t length) {
  return ::BCryptGenRandom(nullptr, out, static_cast<ULONG>(length), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}`},
	// the sheet asks about the network the SDK host names now
	{"AppController.cpp", urIoController, "AppController::ApprovalOffered", "bool AppController::ApprovalOffered() {", `
bool AppController::ApprovalOffered() {
  return ApprovalOfferedIn(sdk_.CurrentApprovalNetwork());
}`},
	// the action is offered only when the build, the network (one a request can name), the process, the sign-in state, the page (live,
	// and listing the version that network needs) and the scheme's handler all say yes
	{"AppController.cpp", urIoController, "AppController::ApprovalOfferedIn", "bool AppController::ApprovalOfferedIn(const approval::Network& network) {", `
bool AppController::ApprovalOfferedIn(const approval::Network& network) {
  if (!config::kBrowserApprovalEnabled) return false;
  if (network.kind == approval::NetworkKind::None) {
    LogInfo("approval: not offered: this network's API cannot be named in a request");
    return false;
  }
  if (ProcessIsElevated()) {
    LogInfo("approval: not offered: this process is elevated");
    return false;
  }
  if (ApprovalContext().signedIn) {
    LogInfo("approval: not offered: the app is signed in");
    return false;
  }
  StartApprovalPageCheck();
  if (ApprovalPageState() != ApprovalPage::Live) {
    LogInfo("approval: not offered: the page is not known to be live");
    return false;
  }
  if (!ApprovalPageLists(approval::ContractFor(network.kind))) {
    LogInfo("approval: not offered: the page does not list this network's contract version");
    return false;
  }
  switch (ProtocolHandlerStatus()) {
    case ProtocolHandlerState::Ours:
    case ProtocolHandlerState::Absent:
      return true;
    case ProtocolHandlerState::Foreign:
      LogInfo("approval: not offered: another program handles urnetwork:// links");
      return false;
    case ProtocolHandlerState::Unsafe:
      LogInfo("approval: not offered: this exe's path cannot be registered as the handler");
      return false;
    case ProtocolHandlerState::Failed:
      LogInfo("approval: not offered: the urnetwork:// handler could not be looked at");
      return false;
  }
  return false;
}`},
	// the page check is begun only where the action could be offered: in a network a request can name
	{"AppController.cpp", urIoController, "AppController::StartApprovalCheck", "void AppController::StartApprovalCheck() {", `
void AppController::StartApprovalCheck() {
  if constexpr (config::kBrowserApprovalEnabled) {
    if (sdk_.CurrentApprovalNetwork().kind != approval::NetworkKind::None && !ProcessIsElevated()) StartApprovalPageCheck();
  }
}`},
	// an attempt starts only after everything that can still fail has passed; a live one is read back only while its flow is the
	// last one and the page would still accept its request. The network is read once, and the same one decides whether the action is
	// offered and what the request names: the official network names nothing (version 1), any other names its API host (version 2)
	{"AppController.cpp", urIoController, "AppController::BeginApproval", "std::optional<approval::Slot::View> AppController::BeginApproval() {", `
std::optional<approval::Slot::View> AppController::BeginApproval() {
  const approval::Clocks now = approval::Now();
  if (std::optional<approval::Slot::View> live = approval_.Peek(now)) {
    if (approval_.Flow() == sdk_.CurrentBrowserFlow() && approval_.Reopenable(now)) return live;
    CancelApproval(approval::ClearReason::Superseded);
  }
  const approval::Network network = sdk_.CurrentApprovalNetwork();
  if (!ApprovalOfferedIn(network)) return std::nullopt;
  if (!EnsureProtocolHandler()) {
    LogWarn("approval: the urnetwork:// handler is not this app's; the paste flow stays");
    return std::nullopt;
  }
  std::optional<approval::KeyPair> keys = approval::FreshKeyPair(approval::NewKeyPair);
  if (!keys) {
    LogWarn("approval: no usable key pair; the paste flow stays");
    return std::nullopt;
  }
  approval::Slot::Params params;
  params.publicKey = std::move(keys->publicKey);
  params.secretKey = std::move(keys->secretKey);
  params.api = network.api;
  params.now = approval::Now();
  params.flow = sdk_.BeginBrowserApproval();
  std::optional<approval::Slot::View> view = approval_.Begin(std::move(params), RandomBytes);
  if (!view) {
    LogWarn("approval: no entropy for a state; the paste flow stays");
    return std::nullopt;
  }
  if (approvalTimer_) {
    approvalTimer_.Stop();
    approvalTimer_.Start();
  }
  LogInfo("approval: an attempt started (version {})", approval::ContractFor(network.kind));
  return view;
}`},
	// the request is the build constant's page and the attempt's own values
	{"AppController.cpp", urIoController, "AppController::ApprovalRequestUrl", "std::string AppController::ApprovalRequestUrl(const approval::Slot::View& view) const {", `
std::string AppController::ApprovalRequestUrl(const approval::Slot::View& view) const {
  return approval::BuildRequestUrl(config::kApprovalPageUrl, view);
}`},
	// every end of the attempt
	{"AppController.cpp", urIoController, "AppController::CancelApproval", "void AppController::CancelApproval(approval::ClearReason reason) {", `
void AppController::CancelApproval(approval::ClearReason reason) {
  if (approvalTimer_) approvalTimer_.Stop();
  if (!approval_.Live()) return;
  approval_.Clear(reason);
  LogInfo("approval: the attempt ended: {}", approval::ToString(reason));
}`},
	// the five minutes are up: end the attempt, and tell the sheet whether or not the gate already wiped it
	{"AppController.cpp", urIoController, "AppController::OnApprovalTimer", "void AppController::OnApprovalTimer() {", `
void AppController::OnApprovalTimer() {
  CancelApproval(approval::ClearReason::TimedOut);
  if (LoginPage* login = LoginPageOrNull()) login->OnApprovalExpired();
}`},
	// the page the gate hands an answer to
	{"AppController.cpp", urIoController, "AppController::LoginPageOrNull", "LoginPage* AppController::LoginPageOrNull() {", `
LoginPage* AppController::LoginPageOrNull() {
  if (!window_) return nullptr;
  if (auto self = window_.try_as<winrt::URnetwork::implementation::MainWindow>()) return &self->login();
  return nullptr;
}`},
	// every outcome of the redeem is delivered, and the credential is held, never registered; the code goes out, and what comes back
	// is kept, only while the flow the approval began in is still the one that started last (a network change takes the next number),
	// and the number is judged under the lock the handle is read under. A refusal by the account's cap of active sessions is a
	// refusal like any other, carried as the token the sheet has words for (SessionLimitNotice.h).
	{"SdkHost.cpp", urIoHost, "SdkHost::RedeemApprovedCode", "void SdkHost::RedeemApprovedCode(const std::string& code, std::function<void(ApprovedNetwork)> done) {", `
void SdkHost::RedeemApprovedCode(const std::string& code, std::function<void(ApprovedNetwork)> done) {
  const std::uint64_t authEpoch = signOutRun_.Epoch();
  urnet::AuthCodeLoginArgs args;
  args.auth_code = code;
  const std::uint64_t flow = approvalFlow_.load(std::memory_order_acquire);
  const auto answer = [this, done, flow, authEpoch](std::optional<urnet::AuthCodeLoginResult> result,
                                         std::optional<std::string> err) {
    ApprovedNetwork out;
    if (err || !result) {
      out.failure = ApprovedNetwork::Failure::Transport;
      out.error = err ? *err : "no result";
      LogWarn("sdkhost: redeeming the approved code did not complete");
      if (done) done(out);
      return;
    }
    if (std::string error = sessionlimit::SignInErrorOf(result->error); !error.empty()) {
      out.failure = ApprovedNetwork::Failure::Refused;
      out.error = std::move(error);
      LogWarn("sdkhost: the server refused the approved code");
      if (done) done(out);
      return;
    }
    const std::optional<std::string> networkName =
        result->by_jwt.empty() ? std::nullopt : JwtClaimString(result->by_jwt, "network_name");
    if (!networkName || networkName->empty()) {
      out.failure = ApprovedNetwork::Failure::Refused;
      out.error = "the approval did not name a network";
      LogWarn("sdkhost: the approved code gave no network to confirm");
      if (done) done(out);
      return;
    }
    bool superseded = false;
    {
      std::scoped_lock lock(mutex_);
      superseded = flow == 0 || !walletFlows_.IsCurrent(flow) || !AuthEventIsCurrent(authEpoch);
      if (!superseded) {
        pendingApprovedJwt_ = result->by_jwt;
        pendingApprovedEpoch_ = authEpoch;
      }
    }
    if (superseded) {
      out.failure = ApprovedNetwork::Failure::Refused;
      out.error = "the approval was superseded by another sign-in";
      LogWarn("sdkhost: the approved code was redeemed after another flow took over; its network is dropped");
      if (done) done(out);
      return;
    }
    LogInfo("sdkhost: the approved code was redeemed; its network is held for the person to confirm");
    out.ok = true;
    out.networkName = *networkName;
    if (done) done(out);
  };
  bool superseded = false;
  {
    std::scoped_lock lock(mutex_);
    if (api_) {
      superseded = flow == 0 || !walletFlows_.IsCurrent(flow);
      if (!superseded) {
        api_->authCodeLogin(args, answer);
        return;
      }
    }
  }
  ApprovedNetwork out;
  if (superseded) {
    out.failure = ApprovedNetwork::Failure::Refused;
    out.error = "the approval was superseded by another sign-in";
    LogWarn("sdkhost: the approved code was not redeemed: another flow took over since the approval began");
  } else {
    out.failure = ApprovedNetwork::Failure::Transport;
    out.error = "the network session went away during sign-in";
  }
  if (done) done(out);
}`},
	// the one place the held credential is registered
	{"SdkHost.cpp", urIoHost, "SdkHost::ConfirmApprovedNetwork", "void SdkHost::ConfirmApprovedNetwork(std::function<void(AuthResult)> done) {", `
void SdkHost::ConfirmApprovedNetwork(std::function<void(AuthResult)> done) {
  std::string jwt;
  std::uint64_t authEpoch = 0;
  {
    std::scoped_lock lock(mutex_);
    if (pendingApprovedJwt_) {
      jwt = std::move(*pendingApprovedJwt_);
      authEpoch = pendingApprovedEpoch_;
      pendingApprovedJwt_.reset();
    }
  }
  if (jwt.empty()) {
    if (done) done({false, false, "no approved sign-in is pending"});
    return;
  }
  SetRegistrationAuthState(authEpoch, AuthState::Authenticating);
  RegisterNetworkClient(jwt, [this, done, authEpoch](AuthResult r) {
    if (!r.ok) {
      std::scoped_lock lock(mutex_);
      if (AuthEventIsCurrent(authEpoch) && api_) api_->setByJwt("");
    }
    if (done) done(r);
  }, authEpoch);
}`},
	// the held credential is dropped
	{"SdkHost.cpp", urIoHost, "SdkHost::DiscardApprovedNetwork", "void SdkHost::DiscardApprovedNetwork() {", `
void SdkHost::DiscardApprovedNetwork() {
  std::scoped_lock lock(mutex_);
  pendingApprovedJwt_.reset();
}`},
	// an approval supersedes the other flows and starts nothing else; the number it takes is the one the redeem of its code is held to
	{"SdkHost.cpp", urIoHost, "SdkHost::BeginBrowserApproval", "std::uint64_t SdkHost::BeginBrowserApproval() {", `
std::uint64_t SdkHost::BeginBrowserApproval() {
  const std::uint64_t flow = CancelPendingWalletFlows("superseded by a ur.io approval");
  approvalFlow_.store(flow, std::memory_order_release);
  DiscardApprovedNetwork();
  if (authState_.load() == AuthState::Authenticating) SetAuthState(AuthState::LoggedOut);
  return flow;
}`},
	// the network an approval would be for, read under a lock of its own and never mutex_: a copy of the snapshot
	{"SdkHost.cpp", urIoHost, "SdkHost::CurrentApprovalNetwork", "approval::Network SdkHost::CurrentApprovalNetwork() const {", `
approval::Network SdkHost::CurrentApprovalNetwork() const {
  std::scoped_lock lock(approvalNetworkMutex_);
  return approvalNetwork_;
}`},
	// ... and written the same way, by the two places that know what the space is
	{"SdkHost.cpp", urIoHost, "SdkHost::SetApprovalNetwork", "void SdkHost::SetApprovalNetwork(approval::Network network) {", `
void SdkHost::SetApprovalNetwork(approval::Network network) {
  std::scoped_lock lock(approvalNetworkMutex_);
  approvalNetwork_ = std::move(network);
}`},
	// the official space is the operator's host and no url of the user's own, whose API is the operator's own; any other is named by the
	// API url the client really uses, when that is https://host[:port]; one that cannot be read names nothing
	{"SdkHost.cpp", urIoHost, "SdkHost::UpdateApprovalNetworkLocked", "void SdkHost::UpdateApprovalNetworkLocked() {", `
void SdkHost::UpdateApprovalNetworkLocked() {
  approval::Network network;
  try {
    if (networkSpace_.has_value()) {
      const bool official = networkSpace_->getHostName() == ids::kNetworkSpaceHostName &&
                            networkSpace_->getConfiguredApiUrl().empty() && networkSpace_->getConfiguredPlatformUrl().empty();
      network = approval::NetworkOf(official, networkSpace_->getApiUrl(), ids::kNetworkSpaceHostName);
    }
  } catch (const std::exception&) {
    LogWarn("sdkhost: the network space could not be read for the approval");
    network = approval::Network();
  }
  SetApprovalNetwork(std::move(network));
}`},
	// an approved answer goes to the open sheet, or waits for the sheet in the way
	{"LoginPage.cpp", urIoLogin, "LoginPage::OnApprovedCode", "void LoginPage::OnApprovedCode(std::string code) {", `
void LoginPage::OnApprovedCode(std::string code) {
  if (approvalSheet_ && approvalSheet_->Open()) {
    approvalSheet_->BeginRedeem(std::move(code));
    return;
  }
  approval::Wipe(pendingApprovedCode_);
  pendingApprovedCode_ = std::move(code);
  approvedCodeWaits_ = 0;
  if (w_.sheetOpen()) w_.CloseSheetsForHide();
  ContinueApprovedCode();
}`},
	// the wait for a sheet to close is bounded, and the code is wiped when it is dropped, and the person is told it was
	{"LoginPage.cpp", urIoLogin, "LoginPage::ContinueApprovedCode", "void LoginPage::ContinueApprovedCode() {", `
void LoginPage::ContinueApprovedCode() {
  constexpr int kMaxWaits = 40;
  if (w_.sheetOpen() && ++approvedCodeWaits_ <= kMaxWaits) {
    if (!approvedCodeTimer_.IsRunning()) approvedCodeTimer_.Start();
    return;
  }
  approvedCodeTimer_.Stop();
  std::string code = std::move(pendingApprovedCode_);
  pendingApprovedCode_.clear();
  if (code.empty()) return;
  if (w_.sheetOpen()) {
    approval::Wipe(code);
    LogWarn("login: an approved sign-in arrived while a sheet that cannot be closed was open; it was dropped");
    if (snackbar_) snackbar_->Show(Loc("auth_code_error"), InfoBarSeverity::Informational, 15000);
    return;
  }
  ShowApprovalSheet(std::move(code));
}`},
	// a denial goes to the sheet
	{"LoginPage.cpp", urIoLogin, "LoginPage::OnApprovalDenied", "void LoginPage::OnApprovalDenied() {", `
void LoginPage::OnApprovalDenied() {
  LogInfo("login: the browser sign-in was cancelled in the browser");
  if (approvalSheet_) approvalSheet_->ShowDenied();
}`},
	// a timeout goes to the sheet
	{"LoginPage.cpp", urIoLogin, "LoginPage::OnApprovalExpired", "void LoginPage::OnApprovalExpired() {", `
void LoginPage::OnApprovalExpired() {
  if (approvalSheet_) approvalSheet_->ShowTimedOut();
}`},
	// the neutral line of a stray: only a signed-out window that is on screen, once in half a minute
	{"LoginPage.cpp", urIoLogin, "LoginPage::ShowStrayNotice", "void LoginPage::ShowStrayNotice() {", `
void LoginPage::ShowStrayNotice() {
  if (!snackbar_ || w_.LoginRoot().Visibility() != Visibility::Visible) return;
  if (!urnw::App().WindowShown()) return;
  if (!strayLine_.Allow(std::chrono::steady_clock::now())) return;
  snackbar_->Show(Loc("auth_code_error"), InfoBarSeverity::Informational, 15000);
}`},
	// the one place the sheet is shown, under the single-sheet gate; an answer that waited for the gate goes on the moment it is clear
	{"LoginPage.cpp", urIoLogin, "LoginPage::ShowApprovalSheet", "winrt::fire_and_forget LoginPage::ShowApprovalSheet(std::string approvedCode) {", `
winrt::fire_and_forget LoginPage::ShowApprovalSheet(std::string approvedCode) {
  if (w_.sheetOpen()) {
    approval::Wipe(approvedCode);
    co_return;
  }
  auto self = w_.get_strong();
  w_.SetSheetOpen(true);
  std::string pasted;
  try {
    approvalSheet_ = urnw::BrowserApprovalSheet::Create(self->Content().XamlRoot());
    if (!approvedCode.empty()) approvalSheet_->BeginRedeem(std::move(approvedCode));
    co_await approvalSheet_->Dialog().ShowAsync();
    pasted = approvalSheet_->TakePastedCode();
  } catch (winrt::hresult_error const& e) {
    urnw::LogError("browser sign-in sheet: {} (0x{:08x})", urnw::Narrow(std::wstring{e.message()}),
                   static_cast<uint32_t>(e.code()));
  } catch (std::exception const& e) {
    urnw::LogError("browser sign-in sheet: {}", e.what());
  }
  approvalSheet_.reset();
  w_.SetSheetOpen(false);
  if (!pendingApprovedCode_.empty()) ContinueApprovedCode();
  approval::Wipe(approvedCode);
  if (!pasted.empty()) SignInWithAuthCode(std::move(pasted));
}`},
	// a swept or closed sheet is not open
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::Open", "bool BrowserApprovalSheet::Open() const {", `
bool BrowserApprovalSheet::Open() const {
  return !closed_ && urnw::App().WindowHides() == hides_;
}`},
	// what the Confirm page shows: the label and the network's name, in the text colour and in the page that asks the question
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::BuildConfirm", "void BrowserApprovalSheet::BuildConfirm() {", `
void BrowserApprovalSheet::BuildConfirm() {
  confirmPanel_ = StackPanel();
  confirmPanel_.Spacing(4);
  confirmPanel_.Children().Append(MakeText(Loc("network_name_label"), 12, colors::MutedBrush()));
  networkText_ = MakeText(hstring{}, 18, colors::TextBrush());
  networkText_.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
  confirmPanel_.Children().Append(networkText_);
}`},
	// the page that waits for the redeem and for the registration: a spinner that says what it is
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::BuildSigningIn", "void BrowserApprovalSheet::BuildSigningIn() {", `
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
}`},
	// the page that says a timeout or a failure: its text is in it
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::BuildMessage", "void BrowserApprovalSheet::BuildMessage() {", `
void BrowserApprovalSheet::BuildMessage() {
  messagePanel_ = StackPanel();
  messagePanel_.Margin(Thickness{0, 12, 0, 12});
  messageText_ = MakeText(hstring{}, 14, colors::MutedBrush());
  messagePanel_.Children().Append(messageText_);
}`},
	// which controls and buttons each page shows, and what a page leaves behind
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::ShowPage", "void BrowserApprovalSheet::ShowPage(Page page) {", `
void BrowserApprovalSheet::ShowPage(Page page) {
  if (page != Page::Start) pasteBox_.Password(L"");
  if (page != Page::Waiting) {
    approval::Wipe(link_);
    launchError_.Visibility(Visibility::Collapsed);
  }
  if (page == Page::Start) {
    startNote_.Visibility(Visibility::Collapsed);
    actionButton_.IsEnabled(!starting_);
  }
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
      SetBar(dialog_, none, none, confirmed_ ? none : Loc("cancel"), ContentDialogButton::None);
      dialog_.Title(winrt::box_value(TitleText()));
      break;
    case Page::Confirm:
      SetBar(dialog_, Loc("sign_in"), none, Loc("cancel"), ContentDialogButton::None);
      dialog_.Title(winrt::box_value(Loc("sign_in")));
      break;
    case Page::TimedOut:
    case Page::Failed:
      SetBar(dialog_, Loc("try_again"), none, Loc("cancel"), ContentDialogButton::Primary);
      dialog_.Title(winrt::box_value(TitleText()));
      break;
  }
}`},
	// a failure shows its message on the failure page
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::ShowFailed", "void BrowserApprovalSheet::ShowFailed(hstring const& message) {", `
void BrowserApprovalSheet::ShowFailed(hstring const& message) {
  messageText_.Text(message);
  messageText_.Foreground(colors::DangerBrush());
  ShowPage(Page::Failed);
}`},
	// only a sheet that is waiting is told it timed out
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::ShowTimedOut", "void BrowserApprovalSheet::ShowTimedOut() {", `
void BrowserApprovalSheet::ShowTimedOut() {
  if (closed_ || page_ != Page::Waiting) return;
  LogInfo("browser sign-in sheet: the attempt timed out");
  messageText_.Text(Loc("sso_sign_in_timed_out"));
  messageText_.Foreground(colors::MutedBrush());
  ShowPage(Page::TimedOut);
}`},
	// a denial returns a waiting sheet to its start
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::ShowDenied", "void BrowserApprovalSheet::ShowDenied() {", `
void BrowserApprovalSheet::ShowDenied() {
  if (closed_ || (page_ != Page::Waiting && page_ != Page::Start)) return;
  ShowPage(Page::Start);
}`},
	// the question: which network
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::ShowConfirm", "void BrowserApprovalSheet::ShowConfirm(std::string const& networkName) {", `
void BrowserApprovalSheet::ShowConfirm(std::string const& networkName) {
  networkText_.Text(H(networkName));
  ShowPage(Page::Confirm);
}`},
	// the pasted code leaves the sheet once
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::TakePastedCode", "std::string BrowserApprovalSheet::TakePastedCode() {", `
std::string BrowserApprovalSheet::TakePastedCode() {
  std::string code = std::move(pasted_);
  pasted_.clear();
  return code;
}`},
	// the attempt, the request, the observed launch, and what is shown when the launcher refuses
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::StartApproval", "winrt::fire_and_forget BrowserApprovalSheet::StartApproval() {", `
winrt::fire_and_forget BrowserApprovalSheet::StartApproval() {
  auto self = shared_from_this();
  if (starting_ || closed_) co_return;
  starting_ = true;
  try {
    actionButton_.IsEnabled(false);
    againButton_.IsEnabled(false);
    std::optional<approval::Slot::View> view = urnw::App().BeginApproval();
    std::string url;
    if (view) {
      url = urnw::App().ApprovalRequestUrl(*view);
      approval::Wipe(view->state);
      approval::Wipe(view->publicKey);
    }
    if (url.empty()) {
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
    const bool launched = co_await LaunchInBrowser(H(link_));
    starting_ = false;
    if (closed_ || page_ != Page::Waiting) co_return;
    actionButton_.IsEnabled(true);
    againButton_.IsEnabled(true);
    if (launched) {
      launchError_.Visibility(Visibility::Collapsed);
      LogInfo("browser sign-in sheet: the browser was opened on the approval page");
      co_return;
    }
    LogWarn("browser sign-in sheet: the browser could not be opened; the link can be copied");
    launchError_.Text(Loc("sso_browser_open_failed"));
    launchError_.Visibility(Visibility::Visible);
  } catch (...) {
    starting_ = false;
    LogWarn("browser sign-in sheet: the approval page could not be updated");
  }
}`},
	// the one button of the Confirm page is the yes
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::OnPrimary", "void BrowserApprovalSheet::OnPrimary() {", `
void BrowserApprovalSheet::OnPrimary() {
  switch (page_) {
    case Page::Confirm:
      Confirm();
      break;
    case Page::TimedOut:
    case Page::Failed:
      ShowPage(Page::Start);
      break;
    case Page::Start:
    case Page::Waiting:
    case Page::SigningIn:
      break;
  }
}`},
	// the page's answer is redeemed, and the code wiped the moment the sdk has it
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::BeginRedeem", "void BrowserApprovalSheet::BeginRedeem(std::string code) {", `
void BrowserApprovalSheet::BeginRedeem(std::string code) {
  if (!Open()) {
    approval::Wipe(code);
    return;
  }
  confirmed_ = false;
  ShowPage(Page::SigningIn);
  auto weak = weak_from_this();
  auto queue = dialog_.DispatcherQueue();
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
}`},
	// what the person is shown for each answer of the redeem; the account's cap of active sessions says what to do, not a bad code
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::OnRedeemed", "void BrowserApprovalSheet::OnRedeemed(SdkHost::ApprovedNetwork answer) {", `
void BrowserApprovalSheet::OnRedeemed(SdkHost::ApprovedNetwork answer) {
  if (closed_) {
    if (answer.ok) Sdk().DiscardApprovedNetwork();
    return;
  }
  if (!answer.ok) {
    LogWarn("browser sign-in sheet: the approval could not be redeemed");
    if (const char* key = sessionlimit::SignInRefusalMessageKey(answer.error)) {
      ShowFailed(Loc(key));
      return;
    }
    ShowFailed(answer.failure == SdkHost::ApprovedNetwork::Failure::Transport
                   ? Loc("something_went_wrong")
                   : Loc("auth_code_error"));
    return;
  }
  const std::string name = kit::SanitizeExternalDisplayText(answer.networkName);
  if (name.empty()) {
    Sdk().DiscardApprovedNetwork();
    LogWarn("browser sign-in sheet: the approval named no network the person could recognise");
    ShowFailed(Loc("auth_code_error"));
    return;
  }
  ShowConfirm(name);
}`},
	// the yes: the held network is registered
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::Confirm", "void BrowserApprovalSheet::Confirm() {", `
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
}`},
	// what happens when the registration is answered; a refusal the sign-in screens have words for (the session cap's, the
	// client limit's) shows them, never its token
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::OnConfirmed", "void BrowserApprovalSheet::OnConfirmed(AuthResult const& result) {", `
void BrowserApprovalSheet::OnConfirmed(AuthResult const& result) {
  if (closed_) return;
  if (result.ok) {
    Finish();
    return;
  }
  LogWarn("browser sign-in sheet: the device could not be registered");
  if (const char* key = sessionlimit::SignInRefusalKey(result.error)) {
    ShowFailed(Loc(key));
    return;
  }
  ShowFailed(result.error.empty() ? Loc("something_went_wrong") : H(result.error));
}`},
	// the paste flow: the one live path of a build with the approval off
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::SubmitPastedCode", "void BrowserApprovalSheet::SubmitPastedCode() {", `
void BrowserApprovalSheet::SubmitPastedCode() {
  pasted_ = Narrow(pasteBox_.Password().c_str());
  pasteBox_.Password(L"");
}`},
	// a closed sheet drops what it held, and cancels the attempt only when the person closed it
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::OnClosed", "void BrowserApprovalSheet::OnClosed() {", `
void BrowserApprovalSheet::OnClosed() {
  closed_ = true;
  pasteBox_.Password(L"");
  approval::Wipe(link_);
  if (page_ == Page::Confirm && !confirmed_) {
    Sdk().DiscardApprovedNetwork();
    LogInfo("browser sign-in sheet: closed without a yes; the redeemed network was dropped");
  }
  if (urnw::App().WindowHides() == hides_) urnw::App().CancelApproval(approval::ClearReason::Cancel);
}`},
	// the sheet closes itself, and nothing else does
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::Finish", "void BrowserApprovalSheet::Finish() {", `
void BrowserApprovalSheet::Finish() {
  if (!closed_) dialog_.Hide();
}`},
	// only a value that is not there is "no value"; a value that could not be read is not the same as an empty key
	{"ProtocolHandler.cpp", urIoProtocol, "ReadString", "Text ReadString(std::wstring_view subkey, const wchar_t* name) {", `
Text ReadString(std::wstring_view subkey, const wchar_t* name) {
  Text text;
  const std::wstring path = KeyPath(subkey);
  const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
  DWORD bytes = 0;
  const LONG sized = ::RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, flags, nullptr, nullptr, &bytes);
  if (IsAbsent(sized)) return text;
  if (sized != ERROR_SUCCESS) {
    text.readable = false;
    return text;
  }
  std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
  bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
  const LONG read = ::RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, flags, nullptr, value.data(), &bytes);
  if (IsAbsent(read)) return text;
  if (read != ERROR_SUCCESS) {
    text.readable = false;
    return text;
  }
  value.resize(bytes / sizeof(wchar_t));
  while (!value.empty() && value.back() == L'\0') value.pop_back();
  text.value = std::move(value);
  return text;
}`},
	// whether a value is there, of any type; -1 when the registry would not say
	{"ProtocolHandler.cpp", urIoProtocol, "HasValue", "int HasValue(std::wstring_view subkey, const wchar_t* name) {", `
int HasValue(std::wstring_view subkey, const wchar_t* name) {
  const std::wstring path = KeyPath(subkey);
  const LONG found = ::RegGetValueW(HKEY_CURRENT_USER, path.c_str(), name, RRF_RT_ANY | RRF_NOEXPAND, nullptr, nullptr, nullptr);
  if (found == ERROR_SUCCESS) return 1;
  return IsAbsent(found) ? 0 : -1;
}`},
	// the user's key as read: nothing when the registry would not say, so nothing is written over what could not be looked at
	{"ProtocolHandler.cpp", urIoProtocol, "ReadUserKey", "std::optional<protocol::UserKey> ReadUserKey() {", `
std::optional<protocol::UserKey> ReadUserKey() {
  protocol::UserKey key;
  HKEY handle = nullptr;
  const std::wstring path = KeyPath(L"");
  const LONG opened = ::RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &handle);
  if (IsAbsent(opened)) return key;
  if (opened != ERROR_SUCCESS) return std::nullopt;
  ::RegCloseKey(handle);
  key.exists = true;
  const int urlProtocol = HasValue(L"", std::wstring(protocol::kUrlProtocolValue).c_str());
  const int delegated = HasValue(protocol::kCommandSubkey, L"DelegateExecute");
  const Text description = ReadString(L"", nullptr);
  const Text owner = ReadString(L"", std::wstring(protocol::kOwnerValue).c_str());
  const Text command = ReadString(protocol::kCommandSubkey, nullptr);
  if (urlProtocol < 0 || delegated < 0 || !description.readable || !owner.readable || !command.readable) {
    return std::nullopt;
  }
  key.urlProtocol = urlProtocol == 1;
  key.delegateExecute = delegated == 1;
  key.description = description.value;
  key.owner = owner.value;
  key.command = command.value;
  return key;
}`},
	// every write is one the plan names, in the plan's order, and the first that fails stops the rest
	{"ProtocolHandler.cpp", urIoProtocol, "Apply", "bool Apply(const protocol::Plan& plan) {", `
bool Apply(const protocol::Plan& plan) {
  for (const protocol::Write& write : plan.writes) {
    const std::wstring path = KeyPath(write.subkey);
    const LONG written = ::RegSetKeyValueW(HKEY_CURRENT_USER, path.c_str(), write.name.empty() ? nullptr : write.name.c_str(),
                                           REG_SZ, write.value.c_str(),
                                           static_cast<DWORD>((write.value.size() + 1) * sizeof(wchar_t)));
    if (written != ERROR_SUCCESS) return false;
  }
  return true;
}`},
	// what the shell will run for the scheme
	{"ProtocolHandler.cpp", urIoProtocol, "ShellCommand", "std::optional<std::wstring> ShellCommand() {", `
std::optional<std::wstring> ShellCommand() {
  DWORD chars = 0;
  const HRESULT sized = ::AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_COMMAND,
                                            std::wstring(protocol::kScheme).c_str(), L"open", nullptr, &chars);
  if (FAILED(sized) || chars == 0) return std::nullopt;
  std::wstring command(chars, L'\0');
  if (FAILED(::AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_COMMAND, std::wstring(protocol::kScheme).c_str(),
                                 L"open", command.data(), &chars))) {
    return std::nullopt;
  }
  while (!command.empty() && command.back() == L'\0') command.pop_back();
  return command;
}`},
	// the read-back: the shell's own answer is the proof that a browser launch starts this exe
	{"ProtocolHandler.cpp", urIoProtocol, "ShellStartsThisExe", "bool ShellStartsThisExe(const std::wstring& exe) {", `
bool ShellStartsThisExe(const std::wstring& exe) {
  const std::optional<std::wstring> command = ShellCommand();
  return command && protocol::EqualsIgnoreCase(*command, protocol::OpenCommand(exe));
}`},
	// write the handler (marker first) and read it back through the shell
	{"ProtocolHandler.cpp", urIoProtocol, "EnsureProtocolHandler", "bool EnsureProtocolHandler() {", `
bool EnsureProtocolHandler() {
  const std::wstring exe = ExePath();
  const std::optional<protocol::UserKey> key = ReadUserKey();
  if (exe.empty() || !key) {
    LogWarn("protocol: the urnetwork:// handler could not be looked at");
    return false;
  }
  const protocol::Plan plan = protocol::PlanEnsure(*key, exe, Widen(version::kString), kPolicy);
  LogInfo("protocol: the urnetwork:// handler: {}", protocol::ToString(plan.decision));
  if (plan.decision == protocol::Decision::RefuseForeign || plan.decision == protocol::Decision::RefuseUnsafePath) {
    return false;
  }
  if (!Apply(plan)) {
    LogWarn("protocol: the urnetwork:// handler could not be written");
    return false;
  }
  const bool ours = ShellStartsThisExe(exe);
  LogInfo("protocol: a browser launch of urnetwork:// {} this exe", ours ? "starts" : "does not start");
  return ours;
}`},
	// where the scheme stands, without changing anything
	{"ProtocolHandler.cpp", urIoProtocol, "ProtocolHandlerStatus", "ProtocolHandlerState ProtocolHandlerStatus() {", `
ProtocolHandlerState ProtocolHandlerStatus() {
  const std::wstring exe = ExePath();
  const std::optional<protocol::UserKey> key = ReadUserKey();
  if (exe.empty() || !key) return ProtocolHandlerState::Failed;
  const protocol::Plan plan = protocol::PlanEnsure(*key, exe, Widen(version::kString), kPolicy);
  switch (plan.decision) {
    case protocol::Decision::RefuseUnsafePath:
      return ProtocolHandlerState::Unsafe;
    case protocol::Decision::RefuseForeign:
      return ProtocolHandlerState::Foreign;
    case protocol::Decision::Create:
    case protocol::Decision::Absent:
      return ProtocolHandlerState::Absent;
    case protocol::Decision::Repoint:
    case protocol::Decision::Adopt:
      return ProtocolHandlerState::Ours;
    case protocol::Decision::Current:
      return ShellStartsThisExe(exe) ? ProtocolHandlerState::Ours : ProtocolHandlerState::Foreign;
  }
  return ProtocolHandlerState::Failed;
}`},
	// two byte strings compared without an early exit
	{"ApprovalUnseal.h", urIoUnseal, "SameBytes", "inline bool SameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {", `
inline bool SameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  return SameSecret(std::string_view(reinterpret_cast<const char*>(a.data()), a.size()),
                    std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
}`},
	// which half of the sdk's pair is the public one: the request carries it to the browser, the other never leaves the app
	{"ApprovalUnseal.h", urIoUnseal, "KeyPairFrom", "inline std::optional<KeyPair> KeyPairFrom(const urnet::WalletKeyPair& made) {", `
inline std::optional<KeyPair> KeyPairFrom(const urnet::WalletKeyPair& made) {
  try {
    std::optional<std::vector<uint8_t>> secretBytes = urnet::decodeBase58(made.PrivateKeyBase58);
    const std::optional<std::vector<uint8_t>> publicBytes = urnet::decodeBase58(made.PublicKeyBase58);
    const bool whole = secretBytes && secretBytes->size() == 32 && publicBytes && publicBytes->size() == 32;
    if (secretBytes) Wipe(*secretBytes);
    if (!whole) return std::nullopt;
    return KeyPair{made.PublicKeyBase58, made.PrivateKeyBase58};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}`},
	// a new pair from the sdk, its own copy of the secret half overwritten
	{"ApprovalUnseal.h", urIoUnseal, "NewKeyPair", "inline std::optional<KeyPair> NewKeyPair() {", `
inline std::optional<KeyPair> NewKeyPair() {
  try {
    std::optional<urnet::WalletKeyPair> made = urnet::generateWalletKeyPair();
    const detail::OnExit overwrite{[&] {
      if (made) Wipe(made->PrivateKeyBase58);
    }};
    if (!made) return std::nullopt;
    return KeyPairFrom(*made);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}`},
	// the one narrow function that opens the page's box
	{"ApprovalUnseal.h", urIoUnseal, "Unseal", "inline std::optional<std::string> Unseal(std::string_view ownSecret, const authlink::Return& link) {", `
inline std::optional<std::string> Unseal(std::string_view ownSecret, const authlink::Return& link) {
  std::string secretText(ownSecret);
  std::optional<std::vector<uint8_t>> secret;
  std::vector<uint8_t> shared, constant, plain;
  std::string sharedBase58;
  const detail::OnExit overwrite{[&] {
    Wipe(secretText);
    if (secret) Wipe(*secret);
    Wipe(shared);
    Wipe(constant);
    Wipe(plain);
    Wipe(sharedBase58);
  }};
  try {
    secret = urnet::decodeBase58(secretText);
    const std::optional<std::vector<uint8_t>> sealedBy = urnet::decodeBase58(link.epk);
    if (!secret || !sealedBy) return std::nullopt;
    shared = urnet::generateSharedSecret(*secret, *sealedBy);
    constant = urnet::generateSharedSecret(*secret, std::vector<uint8_t>(32, 0));
    if (shared.empty() || detail::SameBytes(shared, constant)) return std::nullopt;
    sharedBase58 = urnet::encodeBase58(shared.data(), static_cast<int32_t>(shared.size()));
    plain = urnet::decryptData(link.data, link.nonce, sharedBase58);
    const nlohmann::json opened = nlohmann::json::parse(plain.begin(), plain.end(), nullptr, false);
    if (!opened.is_object()) return std::nullopt;
    const auto code = opened.find("c");
    const auto state = opened.find("s");
    if (code == opened.end() || state == opened.end() || !code->is_string() || !state->is_string()) return std::nullopt;
    if (!SameSecret(state->get<std::string>(), link.state)) return std::nullopt;
    std::string answer = code->get<std::string>();
    if (answer.empty() || answer.size() > 2048) return std::nullopt;
    return answer;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}`},
	// decryption that cannot throw: a failure is an empty answer
	{"WalletConnect.cpp", urIoWallet, "TryDecrypt", "std::optional<std::vector<uint8_t>> TryDecrypt(const std::string& dataBase58,", `
std::optional<std::vector<uint8_t>> TryDecrypt(const std::string& dataBase58,
                                               const std::string& nonceBase58,
                                               const std::string& sharedSecretBase58) {
  try {
    return urnet::decryptData(dataBase58, nonceBase58, sharedSecretBase58);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}`},
	// the wallet's key came off a link: the shared secret cannot throw
	{"WalletConnect.cpp", urIoWallet, "WalletConnect::SharedSecretBase58", "std::optional<std::string> WalletConnect::SharedSecretBase58() const {", `
std::optional<std::string> WalletConnect::SharedSecretBase58() const {
  if (!dappKeyPair_ || !walletEncryptionPublicKey_) return std::nullopt;
  auto priv = urnet::decodeBase58(dappKeyPair_->PrivateKeyBase58);
  auto pub = urnet::decodeBase58(*walletEncryptionPublicKey_);
  if (!priv || !pub) return std::nullopt;
  try {
    auto shared = urnet::generateSharedSecret(*priv, *pub);
    if (shared.empty()) return std::nullopt;
    return urnet::encodeBase58(shared.data(), static_cast<int32_t>(shared.size()));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}`},
	// the sheet asks once whether the approval is offered, and a sheet that began as the paste flow stays that
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::Create", "std::shared_ptr<BrowserApprovalSheet> BrowserApprovalSheet::Create(XamlRoot const& root) {", `
std::shared_ptr<BrowserApprovalSheet> BrowserApprovalSheet::Create(XamlRoot const& root) {
  auto sheet = std::shared_ptr<BrowserApprovalSheet>(new BrowserApprovalSheet());
  sheet->approvalOffered_ = urnw::App().ApprovalOffered();
  sheet->Build(root);
  return sheet;
}`},
	// the one action of the Start page: the approval where it is offered, the site to sign in at where it is not
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::OnAction", "void BrowserApprovalSheet::OnAction() {", `
void BrowserApprovalSheet::OnAction() {
  if (approvalOffered_) {
    StartApproval();
  } else {
    OpenManualSignIn();
  }
}`},
	// the paste flow's button, as it has always been: the site to sign in at, with the launcher's answer observed
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::OpenManualSignIn", "winrt::fire_and_forget BrowserApprovalSheet::OpenManualSignIn() {", `
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
}`},
	// a note on the Start page
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::ShowStartNote", "void BrowserApprovalSheet::ShowStartNote(hstring const& message) {", `
void BrowserApprovalSheet::ShowStartNote(hstring const& message) {
  startNote_.Text(message);
  startNote_.Visibility(Visibility::Visible);
}`},
	// this exe's full path, or empty: the path the key is written for
	{"ProtocolHandler.cpp", urIoProtocol, "ExePath", "std::wstring ExePath() {", `
std::wstring ExePath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) return std::wstring(buffer.data(), length);
    buffer.resize(buffer.size() * 2);
  }
}`},
	// where under the user's classes a write goes: the key and its subkey, joined by a backslash
	{"ProtocolHandler.cpp", urIoProtocol, "KeyPath", "std::wstring KeyPath(std::wstring_view subkey) {", `
std::wstring KeyPath(std::wstring_view subkey) {
  std::wstring path(protocol::kClassesKey);
  if (!subkey.empty()) {
    path.push_back(L'\\');
    path.append(subkey);
  }
  return path;
}`},
	// at each launch a key this app wrote follows the exe; it never creates one and never takes another's
	{"ProtocolHandler.cpp", urIoProtocol, "RefreshProtocolHandler", "void RefreshProtocolHandler() {", `
void RefreshProtocolHandler() {
  const std::wstring exe = ExePath();
  const std::optional<protocol::UserKey> key = ReadUserKey();
  if (exe.empty() || !key) return;
  const protocol::Plan plan = protocol::PlanRefresh(*key, exe, Widen(version::kString));
  if (plan.decision == protocol::Decision::RefuseUnsafePath) {
    LogWarn("protocol: the urnetwork:// handler is not brought up to date: this exe's path is unsafe");
    return;
  }
  if (plan.writes.empty()) return;
  if (Apply(plan)) {
    LogInfo("protocol: the urnetwork:// handler: {}", protocol::ToString(plan.decision));
  } else {
    LogWarn("protocol: the urnetwork:// handler could not be brought up to date");
  }
}`},
	// the command bar of a page: what each button says, and which is the default
	{"BrowserApprovalSheet.cpp", urIoSheet, "SetBar", "void SetBar(ContentDialog const& dialog, hstring const& primary, hstring const& secondary,", `
void SetBar(ContentDialog const& dialog, hstring const& primary, hstring const& secondary,
            hstring const& close, ContentDialogButton defaultButton) {
  dialog.PrimaryButtonText(primary);
  dialog.SecondaryButtonText(secondary);
  dialog.CloseButtonText(close);
  dialog.DefaultButton(defaultButton);
}`},
	// a copied link comes off the clipboard only if it is still what is there
	{"BrowserApprovalSheet.cpp", urIoSheet, "ClearClipboardIfUnchanged", "winrt::fire_and_forget ClearClipboardIfUnchanged(std::string copied) {", `
winrt::fire_and_forget ClearClipboardIfUnchanged(std::string copied) {
  namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
  try {
    const dt::DataPackageView content = dt::Clipboard::GetContent();
    if (content.Contains(dt::StandardDataFormats::Text())) {
      const hstring now = co_await content.GetTextAsync();
      if (Narrow(now.c_str()) == copied) dt::Clipboard::Clear();
    }
  } catch (winrt::hresult_error const&) {
  }
  approval::Wipe(copied);
}`},
	// a minute later, on the UI thread
	{"BrowserApprovalSheet.cpp", urIoSheet, "ClearClipboardLater", "winrt::fire_and_forget ClearClipboardLater(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue,", `
winrt::fire_and_forget ClearClipboardLater(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue,
                                           std::string copied) {
  co_await winrt::resume_after(kClipboardLife);
  queue.TryEnqueue([copied = std::move(copied)]() mutable {
    ClearClipboardIfUnchanged(std::move(copied));
  });
}`},
	// Copy: the link goes to the clipboard without history and without roaming, or not at all
	{"BrowserApprovalSheet.cpp", urIoSheet, "BrowserApprovalSheet::CopyLink", "void BrowserApprovalSheet::CopyLink() {", `
void BrowserApprovalSheet::CopyLink() {
  if (link_.empty()) return;
  namespace dt = winrt::Windows::ApplicationModel::DataTransfer;
  try {
    dt::DataPackage package;
    package.SetText(H(link_));
    dt::ClipboardContentOptions options;
    options.IsAllowedInHistory(false);
    options.IsRoamable(false);
    if (!dt::Clipboard::SetContentWithOptions(package, options)) {
      LogError("browser sign-in sheet: the clipboard refused a history-excluded copy");
      return;
    }
  } catch (winrt::hresult_error const&) {
    LogWarn("browser sign-in sheet: the clipboard could not be written");
    return;
  }
  ClearClipboardLater(dialog_.DispatcherQueue(), link_);
}`},
	// the launcher's own verdict, observed; a throw is a no
	{"BrowserApprovalSheet.cpp", urIoSheet, "LaunchInBrowser", "winrt::Windows::Foundation::IAsyncOperation<bool> LaunchInBrowser(hstring url) {", `
winrt::Windows::Foundation::IAsyncOperation<bool> LaunchInBrowser(hstring url) {
  bool launched = false;
  try {
    launched = co_await winrt::Windows::System::Launcher::LaunchUriAsync(
        winrt::Windows::Foundation::Uri(url));
  } catch (...) {
    launched = false;
  }
  co_return launched;
}`},
	// the probe: one GET of the descriptor constant, three seconds, no redirect, one closed word for how it went, and the versions the page lists
	{"ApprovalCheck.cpp", urIoCheck, "Probe", "std::string_view Probe(std::uint32_t& listed) {", `
std::string_view Probe(std::uint32_t& listed) {
  const auto started = std::chrono::steady_clock::now();
  const auto tooSlow = [&started] { return std::chrono::steady_clock::now() - started > approval::kCheckBudget; };
  const DWORD budgetMs = static_cast<DWORD>(approval::kCheckBudget.count());
  const std::wstring url = Widen(config::kApprovalDescriptorUrl);
  wchar_t host[256] = {};
  wchar_t path[1024] = {};
  wchar_t extra[1024] = {};
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.lpszHostName = host;
  parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  parts.lpszExtraInfo = extra;
  parts.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
  if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) return "bad-url";
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  if (!secure && parts.nScheme != INTERNET_SCHEME_HTTP) return "bad-url";
  const std::wstring userAgent = L"URnetwork-Windows/" + Widen(version::kString);
  Handle session(::WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.h) return "no-connection";
  ::WinHttpSetTimeouts(session.h, static_cast<int>(budgetMs), static_cast<int>(budgetMs), static_cast<int>(budgetMs),
                       static_cast<int>(budgetMs));
  Handle connection(::WinHttpConnect(session.h, host, parts.nPort, 0));
  if (!connection.h) return "no-connection";
  const std::wstring target = std::wstring(path) + extra;
  Handle request(::WinHttpOpenRequest(connection.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
  if (!request.h) return "no-connection";
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (!::WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy))) return "no-connection";
  DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  ::WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
  ::WinHttpAddRequestHeaders(request.h, L"Accept: application/json", static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
  if (!::WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(request.h, nullptr)) {
    return tooSlow() ? "too-slow" : "no-connection";
  }
  if (tooSlow()) return "too-slow";
  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  if (!::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
      status != 200) {
    return "bad-status";
  }
  wchar_t contentType[256] = {};
  DWORD contentTypeBytes = sizeof(contentType) - sizeof(wchar_t);
  if (!::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, contentType,
                             &contentTypeBytes, WINHTTP_NO_HEADER_INDEX)) {
    contentType[0] = L'\0';
  }
  std::string body;
  for (;;) {
    if (tooSlow()) return "too-slow";
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(request.h, &available)) return "unreadable";
    if (available == 0) break;
    char chunk[1024];
    DWORD read = 0;
    if (!::WinHttpReadData(request.h, chunk, available < sizeof(chunk) ? available : static_cast<DWORD>(sizeof(chunk)),
                           &read) ||
        read == 0) {
      return "unreadable";
    }
    body.append(chunk, read);
    if (body.size() > approval::kMaxDescriptorBytes) break;
  }
  if (tooSlow()) return "too-slow";
  const approval::Listing answer = approval::ReadDescriptor(Narrow(contentType), body);
  listed = answer.contracts;
  return approval::ToString(answer.verdict);
}`},
	// the worker's one answer, kept for the run whichever way it went: the versions the page lists first, then the state that says Live
	{"ApprovalCheck.cpp", urIoCheck, "Run", "void Run() {", `
void Run() {
  std::uint32_t listed = 0;
  const std::string_view word = Probe(listed);
  const bool live = word == approval::ToString(approval::DescriptorVerdict::Offered);
  g_contracts.store(live ? listed : 0U, std::memory_order_release);
  g_state.store(static_cast<int>(live ? ApprovalPage::Live : ApprovalPage::Down), std::memory_order_release);
  LogInfo("approval: the page check: {}", word);
}`},
	// an elevated token, or a token that cannot be read, is not offered the approval
	{"ApprovalCheck.cpp", urIoCheck, "ProcessIsElevated", "bool ProcessIsElevated() {", `
bool ProcessIsElevated() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return true;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const bool read = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
  ::CloseHandle(token);
  return !read || elevation.TokenIsElevated != 0;
}`},
	// the check starts once per run, and never blocks
	{"ApprovalCheck.cpp", urIoCheck, "StartApprovalPageCheck", "void StartApprovalPageCheck() {", `
void StartApprovalPageCheck() {
  int expected = static_cast<int>(ApprovalPage::NotChecked);
  if (!g_state.compare_exchange_strong(expected, static_cast<int>(ApprovalPage::Checking))) return;
  try {
    std::thread(Run).detach();
  } catch (const std::system_error&) {
    g_state.store(static_cast<int>(ApprovalPage::Down), std::memory_order_release);
    LogWarn("approval: the page check could not start");
  }
}`},
	// where the check stands
	{"ApprovalCheck.cpp", urIoCheck, "ApprovalPageState", "ApprovalPage ApprovalPageState() {", `
ApprovalPage ApprovalPageState() {
  return static_cast<ApprovalPage>(g_state.load(std::memory_order_acquire));
}`},
	// whether the page lists a version: only a page that answered Live lists anything
	{"ApprovalCheck.cpp", urIoCheck, "ApprovalPageLists", "bool ApprovalPageLists(std::int64_t contract) {", `
bool ApprovalPageLists(std::int64_t contract) {
  if (ApprovalPageState() != ApprovalPage::Live) return false;
  approval::Listing answer;
  answer.contracts = g_contracts.load(std::memory_order_acquire);
  return answer.Lists(contract);
}`},
}

func TestApprovalDecisionFunctionsAreExactlyWhatTheContractAllows(t *testing.T) {
	if len(urIoExactPins) < 50 {
		t.Fatalf("only %d definitions are pinned here; the table was emptied", len(urIoExactPins))
	}
	for _, pin := range urIoExactPins {
		t.Run(pin.name, func(t *testing.T) {
			requireExactDefinition(t, pin.file, pin.read(t), pin.signature, pin.want)
		})
	}
}

// The scope guard that every wipe of Unseal and NewKeyPair stands on. Those two are pinned whole above, and that says
// a wipe is registered; whether it ever runs is this class. A destructor that runs nothing, or runs it twice, leaves
// every wipe in the source and none of them done, and the spec that looks at the sdk's own copies cannot tell.
func TestApprovalScopeGuardRunsWhatItIsGivenOnce(t *testing.T) {
	guard := authLinkSquash(urIoUnseal(t))
	const opener = "template <class F> class OnExit {"
	start := strings.Index(guard, opener)
	if start < 0 {
		t.Fatalf("ApprovalUnseal.h no longer defines %q; update this contract", opener)
	}
	end := strings.Index(guard[start:], "};")
	if end < 0 {
		t.Fatal("cannot find the end of the scope guard in ApprovalUnseal.h")
	}
	want := authLinkSquash(`
template <class F>
class OnExit {
 public:
  explicit OnExit(F f) : f_(std::move(f)) {}
  OnExit(const OnExit&) = delete;
  OnExit& operator=(const OnExit&) = delete;
  ~OnExit() { f_(); }

 private:
  F f_;
};`)
	if got := guard[start : start+end+2]; got != want {
		t.Errorf("the scope guard of ApprovalUnseal.h does not match the text this contract allows: it runs what it is given, once, when it goes out of scope.\n got: %s\nwant: %s", got, want)
	}
}
