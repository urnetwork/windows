// Subscription balance store: the app-side port of macOS
// SubscriptionBalanceViewModel. Fetches Api::subscriptionBalance into a
// snapshot (used / pending / available / start bytes + the Pro plan state),
// keeps it fresh with a 30-second background poll while the window is visible,
// and offers a 5-second confirmation poll with a 2-minute deadline for after a
// checkout or a code redeem, so the plan flips as soon as the server's payment
// webhook lands.
//
// Pro is readable OFFLINE from the stored jwt (LocalState::parseByJwt), which
// seeds the snapshot at login; the server is the source of truth afterwards,
// and the jwt is refreshed whenever the two disagree in either direction
// (an upgrade and a lapse both go stale in the token).
//
// Threading: all public methods run on the UI thread; SDK callbacks marshal
// back through the DispatcherQueue. The change handler fires on the UI thread.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <functional>

#include <winrt/Microsoft.UI.Dispatching.h>

#include "SdkHost.h"

namespace urnw {

// One reading of the network's balance + plan.
struct BalanceSnapshot {
  int64_t usedByteCount = 0;
  int64_t pendingByteCount = 0;
  int64_t availableByteCount = 0;
  int64_t startBalanceByteCount = 0;  // the "daily data balance" row
  bool isPro = false;
  bool guest = false;
  bool loaded = false;  // at least one successful fetch this session
};

// Confirmation-poll state, for the upgrade flow UI.
struct BalancePollState {
  bool confirming = false;  // the 5s post-checkout poll is running
  // The confirmation poll gave up without the server confirming Pro (a lost or
  // slow payment webhook). The purchase is still likely to land: the background
  // poll and the next launch pick it up. The UI must say so instead of spinning.
  bool timedOut = false;
};

class SubscriptionBalanceStore {
 public:
  using ChangeHandler =
      std::function<void(BalanceSnapshot const&, BalancePollState const&)>;

  explicit SubscriptionBalanceStore(SdkHost& sdk) : sdk_(sdk) {}
  ~SubscriptionBalanceStore();

  // Create the timers on the UI thread's queue. Call once, before Start().
  void Initialize(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue);

  void SetChangeHandler(ChangeHandler h) { onChange_ = std::move(h); }
  // The background poll only runs while this returns true (window visible).
  // The confirmation poll ignores it: the user is off in the browser paying.
  void SetVisibilityGate(std::function<bool()> gate) { visible_ = std::move(gate); }

  // Login: seed Pro/guest offline from the stored jwt, fetch once, and begin
  // the 30s background poll (it stops itself once Pro with balance).
  void Start();
  // Logout: stop the timers and clear the snapshot.
  void Stop();
  // Fetch now (navigating to the account panel, window shown, redeem success).
  void Refresh();

  // Re-derive Pro from the (freshly refreshed) jwt. Wired to the sdk's jwt-refresh
  // listener so a mid-session Pro change -- notably a Pro->free lapse a Pro
  // network's paused poll would miss -- is reflected right away, and so the jwt's
  // Pro claim (jwtPro_) advances only when a refresh actually lands. Must be called
  // on the UI thread (AppController marshals it via OnUi).
  void OnJwtRefreshed();

  // After a checkout was handed to the browser (or a balance code redeemed):
  // poll every 5 seconds until the server confirms, giving up after 2 minutes.
  void StartConfirmationPolling();
  void ClearTimeout();

  BalanceSnapshot Current() const { return snapshot_; }
  BalancePollState CurrentPoll() const { return {confirming_, timedOut_}; }

 private:
  void Fetch();
  void Apply(urnet::SubscriptionBalanceResult const& result);
  // Pro with a positive balance: nothing left to poll for (macOS
  // isSupporterWithBalance).
  bool IsSupporterWithBalance() const {
    return snapshot_.isPro && snapshot_.availableByteCount > 0;
  }
  void EnsureBackgroundPolling();
  void StopBackground();
  void StopConfirmation(bool timedOut);
  void Publish();

  SdkHost& sdk_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer backgroundTimer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer confirmTimer_{nullptr};
  ChangeHandler onChange_;
  std::function<bool()> visible_;

  BalanceSnapshot snapshot_;
  bool jwtPro_ = false;     // the jwt's Pro claim (stale across plan changes)
  bool loading_ = false;    // one fetch in flight at a time
  bool confirming_ = false;
  bool timedOut_ = false;
  int64_t deadlineMillis_ = 0;   // confirmation poll gives up past this
  uint32_t generation_ = 0;      // drops fetch results from a superseded session
};

}  // namespace urnw
