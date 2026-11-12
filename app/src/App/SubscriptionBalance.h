// Subscription balance store: the app-side port of macOS
// SubscriptionBalanceViewModel. Fetches Api::subscriptionBalance into a
// snapshot (used / pending / available / start bytes + the Pro plan state),
// keeps it fresh with a 30-second background poll while the window is visible,
// and offers a 5-second confirmation poll with a 2-minute active-polling
// budget for after a checkout or a code redeem, so the plan flips as soon as
// the server's payment webhook lands. The confirmation poll and its budget run
// only while the window is visible AND focused (ConfirmationPollGate): a hosted
// checkout leaves the window visible behind the browser, so the clock must
// also stop on focus loss while the user is off paying.
//
// Pro is the sdk's decision (urnet::SubscriptionProTracker): readable OFFLINE
// from the stored jwt's claim, which seeds the snapshot at login, and the
// server's answer from the first fetch on. When the two disagree (an upgrade
// and a lapse both go stale in the token) the jwt is refreshed once per
// stored jwt: the refresh renews the client token, not the stored network
// jwt, so taking Pro from the unchanged claim on the refresh flipped the plan
// back on every background poll and asked for a refresh on every one
// (support inbox 8211).
//
// The plan shown (snapshot.planView) is the same decision. Before the server
// answers, a claim of Free is not shown as Free, since the claim stays Free
// after a purchase until the network jwt renews: the plan reads Loading, or
// Failed once a fetch failed, and offers no upgrade (support inbox 4074).
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
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>

#include "ConfirmationPollGate.h"
#include "CryptoPayPresentation.h"
#include "PaymentPastDue.h"
#include "PlanView.h"
#include "PricePresentation.h"
#include "ReferralCodeState.h"
#include "ReferralTotalsState.h"
#include "SdkHost.h"

namespace urnw {

// One reading of the network's balance + plan.
struct BalanceSnapshot {
  int64_t usedByteCount = 0;
  int64_t pendingByteCount = 0;
  int64_t availableByteCount = 0;
  int64_t startBalanceByteCount = 0;  // the "daily data balance" row
  bool isPro = false;
  // What the plan card shows (PlanView.h), from the sdk's decision: Pro or
  // Free as the server answered (Pro also by the stored jwt's claim before it
  // does), else Loading or Failed, never Free. isPro is its being Pro.
  planview::PlanView planView = planview::PlanView::Loading;
  // the current subscription's store family (urnet::classifySubscriptionStore:
  // "stripe", "apple", "google", "crypto" for a year paid once in a coin,
  // "other"), "" without a subscription
  std::string subscriptionStoreFamily;
  bool guest = false;
  bool loaded = false;  // at least one successful fetch this session
  int64_t fetchedAtMillis = 0;  // steady clock, the last successful fetch
  // The plan response's price tier (standard/regional, from the storefront
  // country the server resolved), the network's welcome offer and the
  // in-app offer experiment's assignment (mmm/onboarding/PLAN.md). Defaults
  // until fetched.
  PriceTierView tier;
  OfferView offer;
  std::string offerVariant;       // offer.in_app variant ("" when unassigned)
  std::string offerExperimentId;
  // The coins the server sells now (crypto-pay CONTRACT §2.2 crypto_assets),
  // what the payment-method rows offer (cryptopay::PaymentTabsFor). Empty
  // from an older server and with every coin switched off, which keeps every
  // billing surface exactly as it was.
  urnet::CryptoPaymentAssetList cryptoAssets;
  // The coins that are on but cannot be quoted now
  // (crypto_unavailable_assets), which the rows show greyed, "Temporarily
  // unavailable", beside the others instead of leaving out. Empty from a
  // server before 2026-11-10.
  urnet::CryptoPaymentAssetList cryptoUnavailableAssets;
  // The network's latest one-time crypto credit still in its grace
  // (crypto_term): the renewal notice's dates. Empty when there is none.
  cryptopay::CryptoTermView cryptoTerm;
  // A Stripe renewal that did not go through and is still being retried
  // (payment_past_due): the past-due notice shows exactly while it is
  // present. Absent from an older server.
  pastdue::PaymentPastDueView paymentPastDue;
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
  // Stop/start every timer with window visibility (shown and not minimized).
  // A confirmation pauses with the window, and showing it again fires an
  // immediate poll with the banked budget. Re-showing also always fetches
  // once — even for a Pro network whose background poll is stopped — so a
  // plan bought or lapsed on the web lands on the next show.
  void SetVisible(bool visible);
  // Window activation. Only the confirmation poll follows focus (the
  // background poll and the presentation keep running while another app is
  // in front): its give-up budget only burns while the poll actually runs, so
  // time spent typing card details in the browser costs nothing, and
  // refocusing the window fires an immediate poll with the banked budget.
  void SetFocused(bool focused);

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
  // Fetch now for a start connect (BalanceGate.h, FetchBalance) and call
  // `settled` once the fetch has settled: succeeded, failed, or not back
  // within kStartConnectFetchTimeoutMs. Waiters share one fetch.
  void FetchThen(std::function<void()> settled);
  // steady clock, when a fetch last settled (0 = never this session)
  int64_t FetchSettledAtMillis() const { return fetchSettledAtMillis_; }

  // Hand the stored jwt to the Pro decision after a jwt refresh (the sdk's
  // jwt-refresh listener), so the claim tracking advances only when a new jwt
  // actually lands. Its claim counts only before the first fetch: once the
  // server answered, Pro stays the server's until the next fetch. Must be
  // called on the UI thread (AppController marshals it via OnUi).
  void OnJwtRefreshed();

  // After a checkout was handed to the browser: poll every 5 seconds until the
  // server confirms Pro, giving up after 2 minutes of ACTIVE polling. The
  // budget pauses with the poll (SetVisible, SetFocused), so a slow browser
  // checkout can never burn it down to a false TimedOut.
  void StartConfirmationPolling();
  void ClearTimeout();

  BalanceSnapshot Current() const { return snapshot_; }
  BalancePollState CurrentPoll() const { return {gate_.Confirming(), timedOut_}; }
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
  std::optional<std::string> ReferralCode() const { return referral_.Code(); }
  // what the referral card shows where the code goes
  ReferralCodeView ReferralView() const { return referral_.View(); }
  // Read the code again now (the card's Try again). Publishes.
  void RetryReferral();
  int64_t TotalReferrals() const { return totals_.Total(); }
  // what a "Total referrals" figure shows: the count, or an error until a read lands
  ReferralTotalsView TotalsView() const { return totals_.View(); }
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
  // The gate opened (or a confirmation started): arm the timer and poll now,
  // or give up when the banked budget is already spent.
  void ResumeConfirmationPolling();
  void StopBackground();
  void StopConfirmation(bool timedOut);
  // hands the stored jwt to proTracker_ (an empty read changes nothing)
  void TrackStoredJwt();
  // snapshot_.planView and snapshot_.isPro as proTracker_ decides them now;
  // whether either changed
  bool SyncPlan();
  void Publish();
  // a fetch settled: run the FetchThen waiters
  void Settle();

  SdkHost& sdk_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueue queue_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer backgroundTimer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer confirmTimer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer referralTimer_{nullptr};
  winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer settleTimer_{nullptr};
  std::vector<std::function<void()>> settleWaiters_;  // FetchThen
  int64_t fetchSettledAtMillis_ = 0;
  ChangeHandler onChange_;
  ReferralCelebrationHandler onReferralCelebration_;
  ReferralCodeFetch referral_;
  ReferralTotalsFetch totals_;
  urnw::ReferralTerms terms_;
  bool referralLoading_ = false;
  BalanceSnapshot snapshot_;
  bool started_ = false;
  bool visible_ = false;
  // the sdk's Pro and jwt refresh decision (made in Initialize, driven from
  // the UI thread only); snapshot_.isPro is its GetIsPro
  // and snapshot_.planView its GetShownPlan (a failed fetch turns it Failed)
  urnet::SubscriptionProTracker proTracker_;
  // snapshot_.guest is IsGuestNetwork(jwtGuest_, serverGuest_): the jwt's
  // GuestMode claim (a refresh clears it) or the server's `guest` (no login
  // method, read live)
  bool jwtGuest_ = false;
  bool serverGuest_ = false;
  bool loading_ = false;    // one fetch in flight at a time
  bool timedOut_ = false;
  // whether a confirmation runs, and its give-up budget in ACTIVE polling time
  ConfirmationPollGate gate_{kConfirmationBudgetMillis};
  uint32_t generation_ = 0;      // drops fetch results from a superseded session
};

}  // namespace urnw
