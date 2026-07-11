// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.Interop.h>

#include "AppController.h"
#include "Strings.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;

namespace winrt::URnetwork::implementation {

namespace {
urnw::SdkHost& Sdk() { return urnw::App().sdk(); }
hstring H(std::string const& s) { return winrt::to_hstring(s); }
}  // namespace

MainWindow::MainWindow() {
  InitializeComponent();
  ExtendsContentIntoTitleBar(true);
  ApplyAuthState(Sdk().IsLoggedIn() ? urnw::AuthState::LoggedIn
                                    : urnw::AuthState::LoggedOut,
                 "");
}

// ---- auth ----------------------------------------------------------------

void MainWindow::OnSignIn(IInspectable const&, RoutedEventArgs const&) {
  LoginError().IsOpen(false);
  SignInButton().IsEnabled(false);
  const std::string email = urnw::Narrow(EmailBox().Text().c_str());
  const std::string password = urnw::Narrow(PasswordBox().Password().c_str());

  Sdk().LoginWithPassword(email, password, [this](urnw::AuthResult r) {
    DispatcherQueue().TryEnqueue([this, r] {
      SignInButton().IsEnabled(true);
      if (!r.ok && !r.error.empty()) {
        LoginError().Severity(InfoBarSeverity::Error);
        LoginError().Message(H(r.error));
        LoginError().IsOpen(true);
      } else if (r.verification_required) {
        LoginError().Severity(InfoBarSeverity::Informational);
        LoginError().Message(L"Check your email/phone for a verification code.");
        LoginError().IsOpen(true);
      }
    });
  });
}

void MainWindow::OnUseCode(IInspectable const&, RoutedEventArgs const&) {
  const std::string code = urnw::Narrow(CodeBox().Text().c_str());
  if (code.empty()) return;
  CodeButton().IsEnabled(false);
  Sdk().LoginWithCode(code, [this](urnw::AuthResult r) {
    DispatcherQueue().TryEnqueue([this, r] {
      CodeButton().IsEnabled(true);
      if (!r.ok && !r.error.empty()) {
        LoginError().Severity(InfoBarSeverity::Error);
        LoginError().Message(H(r.error));
        LoginError().IsOpen(true);
      }
    });
  });
}

void MainWindow::OnSignOut(IInspectable const&, RoutedEventArgs const&) {
  Sdk().Logout();
}

// ---- connect -------------------------------------------------------------

void MainWindow::OnConnectToggle(IInspectable const&, RoutedEventArgs const&) {
  if (connected_)
    Sdk().Disconnect();
  else
    Sdk().ConnectBestAvailable();
}

// ---- navigation ----------------------------------------------------------

void MainWindow::OnNavSelectionChanged(NavigationView const&,
                                       NavigationViewSelectionChangedEventArgs const& args) {
  auto item = args.SelectedItem().try_as<NavigationViewItem>();
  if (!item) return;
  const auto tag = winrt::unbox_value_or<hstring>(item.Tag(), L"connect");

  ConnectView().Visibility(tag == L"connect" ? Visibility::Visible : Visibility::Collapsed);
  AccountView().Visibility(tag == L"account" ? Visibility::Visible : Visibility::Collapsed);
  WalletView().Visibility(tag == L"wallet" ? Visibility::Visible : Visibility::Collapsed);
  LeaderboardView().Visibility(tag == L"leaderboard" ? Visibility::Visible : Visibility::Collapsed);
  SupportView().Visibility(tag == L"support" ? Visibility::Visible : Visibility::Collapsed);
  SettingsView().Visibility(tag == L"settings" ? Visibility::Visible : Visibility::Collapsed);

  if (!Sdk().apiReady()) return;
  if (tag == L"account") LoadAccount();
  else if (tag == L"wallet") LoadWallet();
  else if (tag == L"leaderboard") LoadLeaderboard();
}

// ---- account -------------------------------------------------------------

void MainWindow::LoadAccount() {
  Sdk().api().getNetworkUser([this](std::optional<urnet::GetNetworkUserResult> result,
                                    std::optional<std::string>) {
    if (!result || !result->network_user) return;
    urnet::NetworkUser u = *result->network_user;
    DispatcherQueue().TryEnqueue([this, u] {
      NetworkNameBox().Text(H(u.network_name));
      std::string auth = (u.user_auth ? *u.user_auth : std::string()) +
                         (u.verified ? "  (verified)" : "  (unverified)");
      AccountAuthText().Text(H(auth));
    });
  });
  Sdk().api().getNetworkReferralCode(
      [this](std::optional<urnet::GetNetworkReferralCodeResult> result,
             std::optional<std::string>) {
        if (!result) return;
        std::string code = result->referral_code ? *result->referral_code : "—";
        int64_t total = result->total_referrals;
        DispatcherQueue().TryEnqueue([this, code, total] {
          ReferralText().Text(H("Code: " + code + "   ·   " +
                                std::to_string(total) + " referrals"));
        });
      });
}

void MainWindow::OnSaveNetworkName(IInspectable const&, RoutedEventArgs const&) {
  urnet::NetworkUserUpdateArgs args;
  args.network_name = urnw::Narrow(NetworkNameBox().Text().c_str());
  Sdk().api().networkUserUpdate(
      args, [](std::optional<urnet::NetworkUserUpdateResult>, std::optional<std::string>) {});
}

// ---- wallet --------------------------------------------------------------

void MainWindow::LoadWallet() {
  Sdk().api().getAccountWallets([this](std::optional<urnet::GetAccountWalletsResult> result,
                                       std::optional<std::string>) {
    if (!result || !result->wallets) return;
    urnet::AccountWalletsList wallets = *result->wallets;
    DispatcherQueue().TryEnqueue([this, wallets] {
      WalletsList().Items().Clear();
      for (auto const& w : wallets) {
        std::string label = w.blockchain + "  " + w.wallet_address;
        WalletsList().Items().Append(winrt::box_value(H(label)));
      }
    });
  });
}

void MainWindow::OnUpgrade(IInspectable const&, RoutedEventArgs const&) {
  // Create the payment intent, then hand the user to the hosted checkout in the
  // system browser (decided flow: Stripe via browser). The concrete checkout URL
  // is a backend integration point; we open the account/billing page which
  // completes the intent. TODO(backend): return a checkout URL from the intent.
  UpgradeButton().IsEnabled(false);
  urnet::StripeCreatePaymentIntentArgs args;
  Sdk().api().createStripePaymentIntent(
      args, [this](std::optional<urnet::StripeCreatePaymentIntentResult> result,
                   std::optional<std::string> err) {
        DispatcherQueue().TryEnqueue([this, err] {
          UpgradeButton().IsEnabled(true);
          auto uri = winrt::Windows::Foundation::Uri(
              H("https://" + Sdk().linkHostName() + "/account"));
          winrt::Windows::System::Launcher::LaunchUriAsync(uri);
        });
      });
}

void MainWindow::OnRedeemCode(IInspectable const&, RoutedEventArgs const&) {
  const std::string secret = urnw::Narrow(BalanceCodeBox().Text().c_str());
  if (secret.empty()) return;
  RedeemButton().IsEnabled(false);
  urnet::RedeemBalanceCodeArgs args;
  args.secret = secret;
  Sdk().api().redeemBalanceCode(
      args, [this](std::optional<urnet::RedeemBalanceCodeResult> result,
                   std::optional<std::string> err) {
        bool ok = result && result->transfer_balance.has_value();
        std::string msg = ok ? "Balance code redeemed."
                             : (result && result->error ? "Invalid or used code."
                                                        : (err ? *err : "Failed."));
        DispatcherQueue().TryEnqueue([this, ok, msg] {
          RedeemButton().IsEnabled(true);
          WalletInfo().Severity(ok ? InfoBarSeverity::Success : InfoBarSeverity::Error);
          WalletInfo().Message(H(msg));
          WalletInfo().IsOpen(true);
          if (ok) BalanceCodeBox().Text(L"");
        });
      });
}

// ---- leaderboard ---------------------------------------------------------

void MainWindow::LoadLeaderboard() {
  urnet::GetLeaderboardArgs args;
  Sdk().api().getLeaderboard(
      args, [this](std::optional<urnet::LeaderboardResult> result,
                   std::optional<std::string>) {
        if (!result || !result->earners) return;
        urnet::LeaderboardEarnersList earners = *result->earners;
        DispatcherQueue().TryEnqueue([this, earners] {
          LeaderboardList().Items().Clear();
          int rank = 1;
          for (auto const& e : earners) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%d.  %s  —  %.1f MiB", rank++,
                          e.network_name.c_str(), e.net_mib_count);
            LeaderboardList().Items().Append(winrt::box_value(winrt::to_hstring(buf)));
          }
        });
      });
}

// ---- support -------------------------------------------------------------

void MainWindow::OnSendFeedback(IInspectable const&, RoutedEventArgs const&) {
  urnet::FeedbackSendArgs args;
  args.star_count = static_cast<int64_t>(FeedbackRating().Value());
  const std::string text = urnw::Narrow(FeedbackText().Text().c_str());
  if (!text.empty()) {
    urnet::FeedbackSendNeeds needs;
    needs.other = text;
    args.needs = needs;
  }
  Sdk().api().sendFeedback(
      args, [this](std::optional<urnet::FeedbackSendResult>, std::optional<std::string>) {
        DispatcherQueue().TryEnqueue([this] {
          SupportInfo().Severity(InfoBarSeverity::Success);
          SupportInfo().Message(L"Thanks for the feedback!");
          SupportInfo().IsOpen(true);
          FeedbackText().Text(L"");
        });
      });
}

// ---- split tunnel (settings) ---------------------------------------------

void MainWindow::OnAddExcludedApp(IInspectable const&, RoutedEventArgs const&) {
  auto picker = winrt::Windows::Storage::Pickers::FileOpenPicker();
  auto init = picker.as<::IInitializeWithWindow>();
  HWND hwnd = nullptr;
  this->try_as<::IWindowNative>()->get_WindowHandle(&hwnd);
  init->Initialize(hwnd);
  picker.FileTypeFilter().Append(L".exe");
  picker.PickSingleFileAsync().Completed([this](auto const& op, auto) {
    auto file = op.GetResults();
    if (!file) return;
    std::string path = urnw::Narrow(file.Path().c_str());
    DispatcherQueue().TryEnqueue([this, path] {
      excludedApps_.push_back(path);
      ExcludedAppsList().Items().Append(winrt::box_value(H(path)));
      PushExcludedApps();
    });
  });
}

void MainWindow::OnRemoveExcludedApp(IInspectable const&, RoutedEventArgs const&) {
  int index = ExcludedAppsList().SelectedIndex();
  if (index < 0 || index >= static_cast<int>(excludedApps_.size())) return;
  excludedApps_.erase(excludedApps_.begin() + index);
  ExcludedAppsList().Items().RemoveAt(index);
  PushExcludedApps();
}

void MainWindow::PushExcludedApps() { Sdk().SetExcludedApps(excludedApps_); }

// ---- state relay ---------------------------------------------------------

void MainWindow::OnAuthStateChanged(urnw::AuthState state, std::string const& error) {
  ApplyAuthState(state, error);
}

void MainWindow::ApplyAuthState(urnw::AuthState state, std::string const& error) {
  const bool loggedIn = (state == urnw::AuthState::LoggedIn);
  LoginPanel().Visibility(loggedIn ? Visibility::Collapsed : Visibility::Visible);
  HomeNav().Visibility(loggedIn ? Visibility::Visible : Visibility::Collapsed);
  if (!error.empty()) {
    LoginError().Severity(InfoBarSeverity::Error);
    LoginError().Message(H(error));
    LoginError().IsOpen(true);
  }
}

void MainWindow::OnTunnelStateChanged(urnw::proto::TunnelStatus const& status) {
  SetConnectedUi(status.state == urnw::proto::TunnelState::Up);
}

void MainWindow::SetConnectedUi(bool connected) {
  connected_ = connected;
  StatusText().Text(connected ? L"Connected" : L"Disconnected");
  ConnectButton().Content(winrt::box_value(connected ? hstring(L"Disconnect")
                                                     : hstring(L"Connect")));
}

// ---- live stats (macOS parity) -------------------------------------------

namespace {
std::string FormatBitRate(int64_t bps) {
  double v = static_cast<double>(bps);
  const char* u = "bps";
  if (v >= 1e9) { v /= 1e9; u = "Gbps"; }
  else if (v >= 1e6) { v /= 1e6; u = "Mbps"; }
  else if (v >= 1e3) { v /= 1e3; u = "Kbps"; }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f %s", v, u);
  return buf;
}
}  // namespace

void MainWindow::OnStatsChanged(urnw::LiveStats const& stats) { ApplyStats(stats); }

void MainWindow::ApplyStats(urnw::LiveStats const& stats) {
  // Provider window size ("Connected to N providers"), like macOS.
  ProviderCountText().Text(
      stats.connected
          ? H("Connected to " + std::to_string(stats.providerCount) + " provider" +
              (stats.providerCount == 1 ? "" : "s"))
          : hstring(L""));

  // Live throughput feed: down / up bit rate.
  ThroughputText().Text(stats.connected
                            ? H("↓ " + FormatBitRate(stats.downBitsPerSecond) +
                                "    ↑ " + FormatBitRate(stats.upBitsPerSecond))
                            : hstring(L""));

  // Insufficient-balance warning (auto-disconnect happens in the SDK).
  BalanceWarning().IsOpen(stats.insufficientBalance);

  // Provide stats.
  std::string provide;
  if (stats.provideEnabled) {
    provide = stats.providePaused
                  ? "Providing (paused)"
                  : "Providing to " + std::to_string(stats.provideClients) + " client" +
                        (stats.provideClients == 1 ? "" : "s");
  }
  ProvideStatsText().Text(H(provide));
}

}  // namespace winrt::URnetwork::implementation
