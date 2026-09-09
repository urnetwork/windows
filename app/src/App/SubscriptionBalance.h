// Subscription balance store: the app-side port of macOS
// SubscriptionBalanceViewModel. Fetches Api::subscriptionBalance into a
// snapshot (used / pending / available / start bytes + the Pro plan state),
// keeps it fresh with a 30-second background poll while the window is visible,
// and offers a 5-second confirmation poll with a 2-minute active-polling
// budget for after a checkout or a code redeem, so the plan flips as soon as
// the server's payment webhook lands. The budget pauses whenever the window
// loses focus (the poll stops with it), so the clock never runs while the
// user is off paying in the browser.
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
#include <optional>
#include <string>

#include <winrt/Microsoft.UI.Dispatching.h>

#include "PricePresentation.h"
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
  // The plan response's price tier (standard/regional, from the storefront
  // country the server resolved), the network's welcome offer and the
  // in-app offer experiment's assignment (mmm/onboarding/PLAN.md). Defaults
  // until fetched.
  PriceTierView tier;
  OfferView offer;
  std::string offerVariant;       // offer.in_app variant ("" when unassigned)
  std::string offerExperimentId;
};

// Confirmation-poll state, for the upgrade flow UI.
struct BalancePollState {
  bool confirming = false;  // the 5s post-checkout poll is running
  // The confirmation poll gave up without the server confirming Pro (a lost or
  // slow payment webhook). The purchase is still likely to land: the background
  // poll and the next launch pick it up. The UI must say so instead of spinning.
  bool timedOut = false;
};

// The referral program's numbers. The server's pro.yml is the single source of
// truth and GET /account/referral-code carries them with the code
// (max_referrals, bonus_per_referral_bytes, referred_bonus_bytes,
// bonus_period_seconds), so every app prints the same cap and bonus. The
// defaults only cover the moment before the first fetch and a server that
// reports no terms.
struct ReferralTerms {
  int64_t maxReferrals = 20;
  int64_t bonusGibPerDay = 3;
  int64_t referredBonusGibPerDay = 3;

  // how many of the network's referrals it is paid for
  int64_t PaidReferrals(int64_t totalReferrals) const {
    if (totalReferrals <= 0) return 0;
    if (0 < maxReferrals && maxReferrals < totalReferrals) return maxReferrals;
    return totalReferrals;
  }
  // the GiB/day the network earns from its referrals
  int64_t EarnedGibPerDay(int64_t totalReferrals) const {
    return PaidReferrals(totalReferrals) * bonusGibPerDay;
  }
};

// A batch of newly observed referrals for the local network. `isFirst` marks
// the crowning: the count went from zero to earned, which gets the full-screen
// celebration; later batches get the gold toast.
struct ReferralCelebration {
  int64_t joined = 0;
  bool isFirst = false;
};

class SubscriptionBalanceStore {
 public:
  using ChangeHandler =
      std::function<void(BalanceSnapshot const&, BalancePollState const&)>;
  using ReferralCelebrationHandler = std::function<void(ReferralCelebration const&)>;

  explicit SubscriptionBalanceStore(SdkHost& sdk) : sdk_(sdk) {}
  ~SubscriptionBalanceStore();

  // Create the timers on the UI thread's queue. Call once, before Start().
  void Initialize(winrt::Microsoft::UI::Dispatching::DispatcherQueue queue);

  void SetChangeHandler(ChangeHandler h) { onChange_ = std::move(h); }
  // Stop/start every timer with window visibility. A confirmation pauses with
  // the window: its give-up budget only burns while the poll actually runs, so
  // time spent unfocused (typing card details in the browser) costs nothing,
  // and showing the window again fires an immediate poll with the banked
  // budget. Re-showing also always fetches once — even for a Pro network whose
  // background poll is stopped — so a plan bought or lapsed on the web lands
  // on the next focus.
  void SetVisible(bool visible);

  // Login: seed Pro/guest offline from the stored jwt, fetch once, and begin
  // the 30s background poll. Once Pro with balance the periodic poll stops,
  // but every window (re)activation still fetches once (SetVisible), so the
  // stop is per focus interval, never for the whole session — a web-side
  // purchase, lapse, or cancellation shows on the next focus.
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
  // poll every 5 seconds until the server confirms, giving up after 2 minutes
  // of ACTIVE polling. The budget pauses with the poll (SetVisible), so a slow
  // browser checkout can never burn it down to a false TimedOut.
  void StartConfirmationPolling();
  void ClearTimeout();

  BalanceSnapshot Current() const { return snapshot_; }
  BalancePollState CurrentPoll() const { return {confirming_, timedOut_}; }
  // A freshly issued offer (POST /onboarding/offer/issue) lands here so every
  // plan surface prints it before the next poll. Publishes.
  void SetOffer(urnet::OnboardingOffer const& offer);

  // ---- referrals (the king-frog gold celebrations) --------------------------
  // The network's referral code + total, refreshed on its own 30s poll while
  // the window is visible. Unlike the balance poll this never stops for Pro:
  // referrals keep landing either way. An observed increment over the
  // persisted per-network baseline fires the celebration handler exactly once
  // (the first observation only records the baseline, so pre-existing
  // referrals -- reinstall, second machine -- are old news, not a surprise).
  void SetReferralCelebrationHandler(ReferralCelebrationHandler h) {
    onReferralCelebration_ = std::move(h);
  }
  std::optional<std::string> ReferralCode() const { return referralCode_; }
  int64_t TotalReferrals() const { return totalReferrals_; }
  // the cap and bonus, from the server with the code (defaults until then)
  urnw::ReferralTerms ReferralTerms() const { return terms_; }

 private:
  void Fetch();
  void FetchReferral();
  void MaybeCelebrateReferrals(std::string const& code, int64_t count);
  void EnsureReferralPolling();
  void StopReferralPolling();
  void Apply(urnet::SubscriptionBalanceResult const& result);
  void ApplyOffer(urnet::OnboardingOffer const& offer);
  // Pro with a positive balance: nothing left to poll for (macOS
  // isSupporterWithBalance).
  bool IsSupporterWithBalance() const {
    return snapshot_.isPro && snapshot_.availableByteCount > 0;
  }
  void EnsureBackgroundPolling();
  void ResumeConfirmationPolling();
  // Focus loss: stop the confirmation timer and bank the unspent budget so
  // ResumeConfirmationPolling can re-arm from where it left off.
  void PauseConfirmationPolling();
  void StopBackground();
  void StopConfirmation(bool timedOut);
  void Publish();

  SdkHost& sdk_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer backgroundTimer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer confirmTimer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer referralTimer_{nullptr};
  ChangeHandler onChange_;
  ReferralCelebrationHandler onReferralCelebration_;
  std::optional<std::string> referralCode_;
  int64_t totalReferrals_ = 0;
  urnw::ReferralTerms terms_;
  bool referralLoading_ = false;
  BalanceSnapshot snapshot_;
  bool started_ = false;
  bool visible_ = false;
  bool jwtPro_ = false;     // the jwt's Pro claim (stale across plan changes)
  bool loading_ = false;    // one fetch in flight at a time
  bool confirming_ = false;
  bool timedOut_ = false;
  // The confirmation give-up budget, counted in ACTIVE polling time only.
  // deadlineMillis_ (monotonic) is armed while the confirm timer runs;
  // pausing banks what is left back into confirmRemainingMillis_.
  int64_t deadlineMillis_ = 0;
  int64_t confirmRemainingMillis_ = 0;
  uint32_t generation_ = 0;      // drops fetch results from a superseded session
};

}  // namespace urnw
