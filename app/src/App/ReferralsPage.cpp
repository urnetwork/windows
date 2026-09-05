// SPDX-License-Identifier: MPL-2.0
#include "pch.h"
#include "ReferralsPage.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "Localization.h"
#include "MainWindow.xaml.h"
#include "PageContext.h"
#include "SdkHost.h"
#include "UrColors.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using urnw::pages::Balance;
using urnw::pages::Loc;
using urnw::pages::Sdk;
using urnw::rows::ApplyFieldState;
using urnw::rows::FieldState;

namespace urnw {
namespace {

// The points event the referral figure sums (server AccountPointEvent).
constexpr const char* kEventReferral = "payout_linked_account";

// Integer when it rounds clean, else two decimals; hand-grouped thousands
// (locale-independent by design: the store owns the words, not the digits).
// The same rule WalletPage applies to the points headline.
std::wstring FormatPoints(double value) {
  char buffer[64];
  if (std::fabs(value - std::round(value)) < 0.005) {
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
  }
  std::string text = buffer;
  const size_t start = (!text.empty() && text[0] == '-') ? 1u : 0u;
  size_t end = text.find('.');
  if (end == std::string::npos) end = text.size();
  for (size_t insertAt = end; insertAt > start + 3;) {
    insertAt -= 3;
    text.insert(insertAt, ",");
  }
  return std::wstring(text.begin(), text.end());
}

}  // namespace

ReferralsPage::ReferralsPage(winrt::URnetwork::implementation::MainWindow& window)
    : w_(window) {}

void ReferralsPage::Build() {
  if (built_) return;
  built_ = true;
  auto host = w_.ReferralsHost();

  // 1. the shared referral card, on the pane's inset. The halo pulse follows
  //    the system animation setting, as it does on the onboarding step.
  bool animations = true;
  try {
    animations = winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled();
  } catch (...) {
  }
  Border cardBox;
  cardBox.Padding(ThicknessHelper::FromLengths(12, 12, 12, 4));
  StackPanel cardPanel;
  cardPanel.Spacing(8);
  card_.Build(cardPanel, animations, /*progressBox=*/false);
  cardBox.Child(cardPanel);
  host.Children().Append(cardBox);

  // 2. the figures, in the pane vocabulary (the referral-network row that
  //    SettingsPage builds under them uses the same switch).
  rows::SetPaneMode(true);
  rows::Heading(host, Loc("referrals"), L"");
  auto card = rows::Card(host);
  totalValue_ = rows::ValueRow(card, Loc("total_referrals"));
  pointsValue_ = rows::ValueRow(card, Loc("referral_points"));
  rows::SetPaneMode(false);

  // Every async field starts in the state that says nothing has been
  // requested (--preview-ui never runs a load).
  ApplyFieldState(totalValue_, FieldState::NoSession);
  ApplyFieldState(pointsValue_, FieldState::NoSession);
}

void ReferralsPage::ApplyStrings() {
  Build();  // idempotent
  w_.ReferralsPaneTitle().Text(Loc("refer_and_earn"));
  Automation::AutomationProperties::SetName(w_.ReferralsPane(), Loc("refer_and_earn"));
  // "‹ Account": the page has no rail item; this is the way back
  w_.ReferralsBackButton().Content(winrt::box_value(hstring{L"‹ " + std::wstring{Loc("account")}}));
  Automation::AutomationProperties::SetName(w_.ReferralsBackButton(), Loc("account"));
  w_.ReferralsBackButton().Click([this](auto const&, auto const&) { w_.CloseReferrals(); });
  card_.Apply();
}

// ---- lifecycle ---------------------------------------------------------------

void ReferralsPage::Load() {
  Build();
  card_.Apply();
  if (!Sdk().IsLoggedIn()) {
    // No token: nothing here can be fetched, and every field says so rather
    // than sitting on a dash or a spinner that never resolves.
    ApplyFieldState(totalValue_, FieldState::NoSession);
    ApplyFieldState(pointsValue_, FieldState::NoSession);
    return;
  }
  ApplyTotal();
  ApplyFieldState(pointsValue_, FieldState::Loading);
  auto queue = w_.DispatcherQueue();
  auto weak = w_.get_weak();
  Sdk().api().getAccountPoints([queue, weak](std::optional<urnet::AccountPointsResult> result,
                                             std::optional<std::string> err) {
    const bool ok = result && !err;
    double points = 0;
    if (ok && result->network_points) {
      for (auto const& point : *result->network_points) {
        if (point.event == kEventReferral) points += urnet::nanoPointsToPoints(point.point_value);
      }
    }
    if (!ok) {
      urnw::LogError("referrals: getAccountPoints failed{}", err ? (": " + *err) : std::string());
    }
    queue.TryEnqueue([weak, points, ok] {
      if (auto self = weak.get()) {
        self->referrals().ApplyPoints(ok ? FieldState::Loaded : FieldState::Failed, points);
      }
    });
  });
}

void ReferralsPage::OnBalance() {
  if (!built_) return;
  card_.Apply();
  if (Sdk().IsLoggedIn()) ApplyTotal();
}

void ReferralsPage::ResetForSignOut() {
  if (!built_) return;
  ApplyFieldState(totalValue_, FieldState::NoSession);
  ApplyFieldState(pointsValue_, FieldState::NoSession);
  card_.Apply();  // the store has dropped the departed network's code and count
}

// ---- painters ----------------------------------------------------------------

void ReferralsPage::ApplyTotal() {
  // the store's figure, "0" until its referral read lands
  ApplyFieldState(totalValue_, FieldState::Loaded,
                  hstring{std::to_wstring(Balance().TotalReferrals())});
}

void ReferralsPage::ApplyPoints(rows::FieldState state, double points) {
  if (state == FieldState::Loaded) {
    ApplyFieldState(pointsValue_, state, hstring{FormatPoints(points)});
  } else {
    ApplyFieldState(pointsValue_, state);
  }
}

}  // namespace urnw
