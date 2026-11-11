// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "SubscriptionBalance.h"

#include <algorithm>
#include <chrono>

#include "CryptoPaySdk.h"
#include "GuestConversion.h"
#include "Log.h"
#include "Paths.h"
#include "PaymentPastDueSdk.h"

namespace urnw {
namespace {

// Monotonic, not wall-clock: this feeds the confirmation budget
// (ConfirmationPollGate), which must never jump because the system clock was
// adjusted.
int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// the usage-bar background refresh (macOS backgroundPollingInterval)
constexpr auto kBackgroundInterval = std::chrono::seconds(30);
// the post-checkout confirmation poll (macOS pollingInterval). Its give-up
// budget (kConfirmationBudgetMillis) only burns while this poll actually runs:
// hiding, minimizing or focusing another app pauses both, so however long the
// user spends typing card details in the browser, they come back to a poll
// that still has its remaining budget.
constexpr auto kConfirmInterval = std::chrono::seconds(5);

}  // namespace

SubscriptionBalanceStore::~SubscriptionBalanceStore() {
  if (backgroundTimer_) backgroundTimer_.Stop();
  if (confirmTimer_) confirmTimer_.Stop();
  if (referralTimer_) referralTimer_.Stop();
  if (settleTimer_) settleTimer_.Stop();
}

void SubscriptionBalanceStore::Initialize(
    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue) {
  queue_ = queue;
  proTracker_ = urnet::newSubscriptionProTracker();

  backgroundTimer_ = queue_.CreateTimer();
  backgroundTimer_.Interval(kBackgroundInterval);
  backgroundTimer_.Tick([this](auto const&, auto const&) {
    Fetch();
  });

  referralTimer_ = queue_.CreateTimer();
  referralTimer_.Interval(kBackgroundInterval);
  referralTimer_.Tick([this](auto const&, auto const&) {
    FetchReferral();
  });

  // a start connect waits this long for its balance fetch, then decides
  // without it (BalanceGate.h: a failed fetch does not block)
  settleTimer_ = queue_.CreateTimer();
  settleTimer_.Interval(std::chrono::milliseconds(balance::kStartConnectFetchTimeoutMs));
  settleTimer_.IsRepeating(false);
  settleTimer_.Tick([this](auto const&, auto const&) {
    LogWarn("balance: start-connect fetch timed out");
    Settle();
  });

  confirmTimer_ = queue_.CreateTimer();
  confirmTimer_.Interval(kConfirmInterval);
  confirmTimer_.Tick([this](auto const&, auto const&) {
    // the server never confirmed within the window: stop hammering the api and
    // tell the user, rather than spinning for the rest of the session
    if (gate_.ExpiredAt(NowMillis())) {
      StopConfirmation(/*timedOut=*/true);
      EnsureBackgroundPolling();
      Publish();
      return;
    }
    Fetch();
  });
}

void SubscriptionBalanceStore::Start() {
  started_ = true;
  ++generation_;
  loading_ = false;
  timedOut_ = false;
  StopConfirmation(/*timedOut=*/false);
  snapshot_ = {};
  fetchSettledAtMillis_ = 0;

  // Pro and guest are readable without any network call: they are claims baked
  // into the stored jwt. The sdk's decision reads the Pro claim until the
  // first fetch lands.
  serverGuest_ = false;
  proTracker_.reset();
  TrackStoredJwt();
  snapshot_.isPro = proTracker_.getIsPro();
  if (auto jwt = sdk_.ParsedJwt()) {
    jwtGuest_ = jwt->GuestMode;
  } else {
    jwtGuest_ = false;
  }
  snapshot_.guest = IsGuestNetwork(jwtGuest_, serverGuest_);
  Publish();

  if (visible_) {
    Fetch();
    FetchReferral();
  }
  EnsureBackgroundPolling();
  EnsureReferralPolling();
}

void SubscriptionBalanceStore::Stop() {
  started_ = false;
  ++generation_;  // any fetch still in flight is dropped
  loading_ = false;
  StopConfirmation(/*timedOut=*/false);
  StopBackground();
  StopReferralPolling();
  referral_.Reset();
  totals_.Reset();
  referralLoading_ = false;
  timedOut_ = false;
  snapshot_ = {};
  fetchSettledAtMillis_ = 0;
  proTracker_.reset();
  jwtGuest_ = false;
  serverGuest_ = false;
  Publish();
  // nothing left to wait for; the waiters ask again (and find no session)
  Settle();
}

void SubscriptionBalanceStore::Refresh() { Fetch(); }

void SubscriptionBalanceStore::FetchThen(std::function<void()> settled) {
  if (!started_ || !sdk_.IsLoggedIn()) {
    // no balance to fetch: decide without it
    fetchSettledAtMillis_ = NowMillis();
    if (settled) settled();
    return;
  }
  settleWaiters_.push_back(std::move(settled));
  if (settleTimer_ && !settleTimer_.IsRunning()) settleTimer_.Start();
  Fetch();  // joins a fetch already in flight
}

void SubscriptionBalanceStore::Settle() {
  fetchSettledAtMillis_ = NowMillis();
  if (settleTimer_) settleTimer_.Stop();
  auto waiters = std::move(settleWaiters_);
  settleWaiters_.clear();
  for (auto& waiter : waiters) {
    if (waiter) waiter();
  }
}

void SubscriptionBalanceStore::SetVisible(bool visible) {
  if (visible_ == visible) return;
  visible_ = visible;
  // the gate banks the confirmation budget on hide and re-arms it on show
  const bool resume = gate_.SetVisible(visible, NowMillis());
  if (!visible_) {
    StopBackground();
    StopReferralPolling();
    if (confirmTimer_) confirmTimer_.Stop();
    return;
  }
  if (!started_) return;
  FetchReferral();
  EnsureReferralPolling();
  if (gate_.Confirming()) {
    if (resume) ResumeConfirmationPolling();
    return;
  }
  Fetch();
  EnsureBackgroundPolling();
}

void SubscriptionBalanceStore::SetFocused(bool focused) {
  const bool resume = gate_.SetFocused(focused, NowMillis());
  if (!gate_.Running()) {
    // focus loss: the confirmation poll and its budget pause (the background
    // poll is not confirming and keeps its visibility gate)
    if (confirmTimer_) confirmTimer_.Stop();
    return;
  }
  if (resume && started_) ResumeConfirmationPolling();
}

void SubscriptionBalanceStore::StartConfirmationPolling() {
  if (gate_.Confirming()) return;
  StopBackground();
  // a fresh confirmation attempt: clear any previous give-up, arm a full budget
  timedOut_ = false;
  gate_.Start(NowMillis());
  Publish();
  ResumeConfirmationPolling();
}

void SubscriptionBalanceStore::ResumeConfirmationPolling() {
  // hidden or unfocused: the gate holds the budget until it opens again
  if (!started_ || !gate_.Running()) return;
  if (gate_.ExpiredAt(NowMillis())) {
    StopConfirmation(/*timedOut=*/true);
    EnsureBackgroundPolling();
    Publish();
    return;
  }
  if (confirmTimer_ && !confirmTimer_.IsRunning()) confirmTimer_.Start();
  // an immediate poll, so a payment that completed while the window was
  // unfocused (the whole point of a hosted checkout) confirms on the first
  // frame back rather than after one more interval
  Fetch();
}

void SubscriptionBalanceStore::ClearTimeout() {
  if (!timedOut_) return;
  timedOut_ = false;
  Publish();
}

void SubscriptionBalanceStore::OnJwtRefreshed() {
  auto byJwt = sdk_.ParsedJwt();
  if (!byJwt) return;
  // The claim tracking advances only when a new jwt actually landed. The
  // refresh renewed the client token, so the stored jwt is usually the one
  // read before, and once a fetch answered Pro stays the server's: taking Pro
  // from the unchanged claim here flipped the plan back on every refresh
  // (support inbox 8211).
  TrackStoredJwt();
  const bool isPro = proTracker_.getIsPro();  // moves only before the first fetch
  // a refresh signs the jwt without GuestMode; the server's guest still holds
  jwtGuest_ = byJwt->GuestMode;
  const bool guest = IsGuestNetwork(jwtGuest_, serverGuest_);
  const bool guestChanged = snapshot_.guest != guest;
  snapshot_.guest = guest;
  if (snapshot_.isPro == isPro) {
    if (guestChanged) Publish();
    return;
  }
  snapshot_.isPro = isPro;
  // a lapse back to free resumes the background poll; Pro stops it (Apply parity)
  if (!isPro) EnsureBackgroundPolling();
  Publish();
}

void SubscriptionBalanceStore::TrackStoredJwt() {
  if (const std::string byJwt = sdk_.StoredByJwt(); !byJwt.empty()) {
    proTracker_.setJwt(byJwt);
  }
}

void SubscriptionBalanceStore::Fetch() {
  // IsLoggedIn(), not apiReady(): apiReady is api_.has_value(), set at SDK INIT
  // rather than at login. subscriptionBalance is authenticated, and this call
  // site has a BACKGROUND POLLER behind it - so unguarded it was a repeating
  // unauthenticated request, not a one-off.
  if (loading_ || !sdk_.IsLoggedIn()) return;
  loading_ = true;
  const uint32_t generation = generation_;
  auto queue = queue_;
  // the storefront variant of the balance call: the same balance plus the
  // price tier, the welcome offer and the experiment assignments. There is no
  // storefront on the desktop, so the server resolves the tier from billing
  // or the request's country (an estimate until the card's country is known).
  sdk_.api().subscriptionBalanceForStorefront(
      "", [this, queue, generation](std::optional<urnet::SubscriptionBalanceResult> result,
                                    std::optional<std::string> err) {
        // sdk callback thread: only marshal (the store lives for the process,
        // owned by AppController)
        queue.TryEnqueue([this, generation, result = std::move(result),
                          err = std::move(err)] {
          if (generation != generation_) return;  // logout superseded this fetch
          loading_ = false;
          if (err || !result) {
            if (err) LogWarn("balance: fetch failed: {}", *err);
            // keep the last snapshot; the poll retries
            Settle();
            return;
          }
          Apply(*result);
          Settle();
        });
      });
}

void SubscriptionBalanceStore::Apply(urnet::SubscriptionBalanceResult const& result) {
  snapshot_.availableByteCount = result.balance_byte_count;
  snapshot_.pendingByteCount = result.open_transfer_byte_count;
  snapshot_.usedByteCount = result.start_balance_byte_count - result.balance_byte_count -
                            result.open_transfer_byte_count;
  snapshot_.startBalanceByteCount = result.start_balance_byte_count;
  snapshot_.loaded = true;
  snapshot_.fetchedAtMillis = NowMillis();
  // no login method on the network (a legacy guest), read live by the server:
  // right even after a refresh cleared the jwt claim
  serverGuest_ = result.guest.value_or(false);
  snapshot_.guest = IsGuestNetwork(jwtGuest_, serverGuest_);
  if (result.price_tier) {
    snapshot_.tier.name =
        result.price_tier->name.empty() ? kPriceTierStandard : result.price_tier->name;
    if (0 < result.price_tier->yearly_usd) snapshot_.tier.yearly = result.price_tier->yearly_usd;
    if (0 < result.price_tier->monthly_usd) snapshot_.tier.monthly = result.price_tier->monthly_usd;
    if (!result.price_tier->currency.empty()) snapshot_.tier.currency = result.price_tier->currency;
  }
  if (result.onboarding_offer) {
    ApplyOffer(*result.onboarding_offer);
  } else {
    snapshot_.offer.active = false;
  }
  // the coins on sale and the crypto term, read through the one adapter that
  // names the sdk's crypto fields (CryptoPaySdk.h)
  snapshot_.cryptoAssets = cryptopay::AssetsFromSdk(result);
  snapshot_.cryptoUnavailableAssets = cryptopay::UnavailableAssetsFromSdk(result);
  snapshot_.cryptoTerm = cryptopay::TermFromSdk(result);
  // a failed Stripe renewal still being retried, read through its one adapter
  // (PaymentPastDueSdk.h)
  snapshot_.paymentPastDue = pastdue::PastDueFromSdk(result);
  snapshot_.offerVariant.clear();
  snapshot_.offerExperimentId.clear();
  if (result.experiments) {
    for (auto const& a : *result.experiments) {
      if (a.surface == "offer.in_app") {
        snapshot_.offerVariant = a.variant;
        snapshot_.offerExperimentId = a.experiment_id;
      }
    }
  }

  // The server is the source of truth for Pro, and current_subscription is
  // set exactly when the network is Pro. The jwt's Pro claim is baked in when
  // the token is issued, so it goes stale on BOTH an upgrade and a lapse. The
  // sdk's decision (urnet::SubscriptionProTracker) keeps Pro the server's from
  // here on and asks for one refresh per stored jwt: the refresh renews the
  // client token and leaves the stored jwt as it was, so asking on every poll
  // refreshed on every poll (support inbox 8211). A stored jwt renewed since
  // (it renews on its own schedule) is read again here.
  const bool serverIsPro = result.current_subscription.has_value();
  TrackStoredJwt();
  const bool refreshJwt = proTracker_.balanceLoaded(serverIsPro);
  snapshot_.isPro = proTracker_.getIsPro();
  if (refreshJwt) sdk_.RefreshJwt();
  snapshot_.subscriptionStoreFamily =
      result.current_subscription ? urnet::classifySubscriptionStore(result.current_subscription->store)
                                  : std::string();

  if (IsSupporterWithBalance()) {
    // nothing left to poll for
    StopConfirmation(/*timedOut=*/false);
    StopBackground();
  } else if (!gate_.Confirming()) {
    EnsureBackgroundPolling();
  }
  Publish();
}

void SubscriptionBalanceStore::EnsureBackgroundPolling() {
  if (!backgroundTimer_ || !started_ || !visible_ || gate_.Confirming() ||
      IsSupporterWithBalance()) {
    return;
  }
  if (!backgroundTimer_.IsRunning()) backgroundTimer_.Start();
}

void SubscriptionBalanceStore::StopBackground() {
  if (backgroundTimer_) backgroundTimer_.Stop();
}

void SubscriptionBalanceStore::StopConfirmation(bool timedOut) {
  if (confirmTimer_) confirmTimer_.Stop();
  gate_.Stop();
  if (timedOut) timedOut_ = true;
}

// ---- referrals (the king-frog gold celebrations) ----------------------------

// The SDK zip built after 2026-09-02 carries the referral terms on
// GetNetworkReferralCodeResult (server pro.yml referral, via the api). Define
// URNW_SDK_REFERRAL_TERMS=0 to build against an older zip, which leaves the
// display defaults in force.
#ifndef URNW_SDK_REFERRAL_TERMS
#define URNW_SDK_REFERRAL_TERMS 1
#endif

namespace {

// bytes granted per period -> whole GiB per day, the number the apps print;
// 0 when either value is unknown
int64_t GibPerDay(int64_t byteCount, int64_t periodSeconds) {
  if (byteCount <= 0 || periodSeconds <= 0) return 0;
  const double perDay = static_cast<double>(byteCount) * 86400.0 / static_cast<double>(periodSeconds);
  return static_cast<int64_t>(perDay / (1024.0 * 1024.0 * 1024.0) + 0.5);
}

// The server's terms, keeping a default for any value the server left at zero
// (no pro.yml: no cap, no grant).
ReferralTerms TermsFromResult(urnet::GetNetworkReferralCodeResult const& result) {
  ReferralTerms terms;
#if URNW_SDK_REFERRAL_TERMS
  if (0 < result.max_referrals) terms.maxReferrals = result.max_referrals;
  const int64_t bonus = GibPerDay(result.bonus_per_referral_bytes, result.bonus_period_seconds);
  if (0 < bonus) terms.bonusGibPerDay = bonus;
  const int64_t referred = GibPerDay(result.referred_bonus_bytes, result.bonus_period_seconds);
  if (0 < referred) terms.referredBonusGibPerDay = referred;
#else
  (void)result;
#endif
  return terms;
}

}  // namespace

void SubscriptionBalanceStore::FetchReferral() {
  // IsLoggedIn(), same reasoning as Fetch(): this sits behind a background
  // poller and getNetworkReferralCode is authenticated.
  if (referralLoading_ || !sdk_.IsLoggedIn()) return;
  referralLoading_ = true;
  const uint32_t generation = generation_;
  auto queue = queue_;
  sdk_.api().getNetworkReferralCode(
      [this, queue, generation](
          std::optional<urnet::GetNetworkReferralCodeResult> result,
          std::optional<std::string> err) {
        // sdk callback thread: only marshal (the store lives for the process,
        // owned by AppController)
        queue.TryEnqueue([this, generation, result = std::move(result),
                          err = std::move(err)] {
          if (generation != generation_) return;  // logout superseded this fetch
          referralLoading_ = false;
          if (err || !result || result->error) {
            LogWarn("referral: fetch failed: {}",
                    err ? *err
                        : (result && result->error ? result->error->message
                                                   : std::string("no result")));
            // keep the last reading (the poll retries), but a card with no
            // code says so rather than spinning
            totals_.Fail();
            if (referral_.Fail()) Publish();
            return;
          }
          const bool repaint = referral_.Succeed(result->referral_code);
          totals_.Succeed(result->total_referrals);
          terms_ = TermsFromResult(*result);
          if (result->referral_code) {
            MaybeCelebrateReferrals(*result->referral_code, result->total_referrals);
          }
          // the referral card follows the store; this read is the one it waits on
          if (repaint) Publish();
        });
      });
}

// The celebration baseline is the count the last celebration (or the first
// observation) left behind, persisted per network in the app prefs so an
// increment observed on this machine celebrates exactly once.
void SubscriptionBalanceStore::MaybeCelebrateReferrals(std::string const& code,
                                                       int64_t count) {
  auto jwt = sdk_.ParsedJwt();
  if (!jwt || !jwt->NetworkId) return;
  const std::string key = "referral_celebrated_count_" + *jwt->NetworkId;

  nlohmann::json prefs = LoadAppPrefs();
  if (!prefs.contains(key)) {
    // first observation for this network on this machine: baseline only --
    // pre-existing referrals are old news, not a surprise
    SaveAppPref(key.c_str(), count);
    return;
  }

  const int64_t previous = prefs.value(key, int64_t{0});
  if (count > previous) {
    ReferralCelebration celebration{count - previous, previous == 0};
    SaveAppPref(key.c_str(), count);
    if (onReferralCelebration_) onReferralCelebration_(celebration);
  } else if (count < previous) {
    // referrals can be unlinked; re-baseline quietly
    SaveAppPref(key.c_str(), count);
  }
}

void SubscriptionBalanceStore::RetryReferral() {
  if (!started_) return;
  referral_.Retry();
  totals_.Retry();
  Publish();
  FetchReferral();  // an in-flight read answers this retry instead
}

void SubscriptionBalanceStore::EnsureReferralPolling() {
  if (!referralTimer_ || !started_ || !visible_) return;
  if (!referralTimer_.IsRunning()) referralTimer_.Start();
}

void SubscriptionBalanceStore::StopReferralPolling() {
  if (referralTimer_) referralTimer_.Stop();
  if (settleTimer_) settleTimer_.Stop();
}

void SubscriptionBalanceStore::ApplyOffer(urnet::OnboardingOffer const& offer) {
  OfferView& view = snapshot_.offer;
  view.active = offer.state == "active";
  if (0 < offer.percent_off) view.percentOff = offer.percent_off;
  if (0 < offer.months_free) view.monthsFree = offer.months_free;
  if (0 < offer.regular_year_usd) view.regularYear = offer.regular_year_usd;
  view.firstYear = 0 < offer.first_year_usd ? offer.first_year_usd
                                            : OfferFirstYear(view.regularYear, view.percentOff);
  if (!offer.currency.empty()) view.currency = offer.currency;
  view.expiresAt = offer.expires_at;
}

void SubscriptionBalanceStore::SetOffer(urnet::OnboardingOffer const& offer) {
  ApplyOffer(offer);
  Publish();
}

void SubscriptionBalanceStore::Publish() {
  if (onChange_) onChange_(snapshot_, {gate_.Confirming(), timedOut_});
}

}  // namespace urnw
