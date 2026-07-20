// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "SubscriptionBalance.h"

#include <chrono>

#include "Log.h"

namespace urnw {
namespace {

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// the usage-bar background refresh (macOS backgroundPollingInterval)
constexpr auto kBackgroundInterval = std::chrono::seconds(30);
// the post-checkout confirmation poll (macOS pollingInterval)
constexpr auto kConfirmInterval = std::chrono::seconds(5);
// give up confirming after this long (macOS maxPollingDuration)
constexpr int64_t kConfirmDeadlineMillis = 120 * 1000;

}  // namespace

SubscriptionBalanceStore::~SubscriptionBalanceStore() {
  if (backgroundTimer_) backgroundTimer_.Stop();
  if (confirmTimer_) confirmTimer_.Stop();
}

void SubscriptionBalanceStore::Initialize(
    winrt::Microsoft::UI::Dispatching::DispatcherQueue queue) {
  queue_ = queue;

  backgroundTimer_ = queue_.CreateTimer();
  backgroundTimer_.Interval(kBackgroundInterval);
  backgroundTimer_.Tick([this](auto const&, auto const&) {
    // skip the network round trip while the window is hidden to the tray; the
    // snapshot resyncs on show (Refresh)
    if (visible_ && !visible_()) return;
    Fetch();
  });

  confirmTimer_ = queue_.CreateTimer();
  confirmTimer_.Interval(kConfirmInterval);
  confirmTimer_.Tick([this](auto const&, auto const&) {
    Fetch();
    // the server never confirmed within the window: stop hammering the api and
    // tell the user, rather than spinning for the rest of the session
    if (confirming_ && NowMillis() >= deadlineMillis_) {
      StopConfirmation(/*timedOut=*/true);
      EnsureBackgroundPolling();
      Publish();
    }
  });
}

void SubscriptionBalanceStore::Start() {
  ++generation_;
  loading_ = false;
  timedOut_ = false;
  confirming_ = false;
  snapshot_ = {};

  // Pro and guest are readable without any network call: they are claims baked
  // into the stored jwt.
  if (auto jwt = sdk_.ParsedJwt()) {
    jwtPro_ = jwt->Pro;
    snapshot_.isPro = jwt->Pro;
    snapshot_.guest = jwt->GuestMode;
  } else {
    jwtPro_ = false;
  }
  Publish();

  Fetch();
  EnsureBackgroundPolling();
}

void SubscriptionBalanceStore::Stop() {
  ++generation_;  // any fetch still in flight is dropped
  loading_ = false;
  StopConfirmation(/*timedOut=*/false);
  StopBackground();
  timedOut_ = false;
  snapshot_ = {};
  jwtPro_ = false;
  Publish();
}

void SubscriptionBalanceStore::Refresh() { Fetch(); }

void SubscriptionBalanceStore::StartConfirmationPolling() {
  if (confirming_) return;
  StopBackground();
  // a fresh confirmation attempt: clear any previous give-up, arm the deadline
  timedOut_ = false;
  confirming_ = true;
  deadlineMillis_ = NowMillis() + kConfirmDeadlineMillis;
  if (confirmTimer_) confirmTimer_.Start();
  Publish();
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
  jwtPro_ = byJwt->Pro;  // the claim actually landed; advance the tracking
  if (snapshot_.isPro == jwtPro_) return;
  snapshot_.isPro = jwtPro_;
  // a lapse back to free resumes the background poll; Pro stops it (Apply parity)
  if (!jwtPro_) EnsureBackgroundPolling();
  Publish();
}

void SubscriptionBalanceStore::Fetch() {
  if (loading_ || !sdk_.apiReady()) return;
  loading_ = true;
  const uint32_t generation = generation_;
  auto queue = queue_;
  sdk_.api().subscriptionBalance(
      [this, queue, generation](std::optional<urnet::SubscriptionBalanceResult> result,
                                std::optional<std::string> err) {
        // sdk callback thread: only marshal (the store lives for the process,
        // owned by AppController)
        queue.TryEnqueue([this, generation, result = std::move(result),
                          err = std::move(err)] {
          if (generation != generation_) return;  // logout superseded this fetch
          loading_ = false;
          if (err || !result) {
            if (err) LogWarn("balance: fetch failed: {}", *err);
            return;  // keep the last snapshot; the poll retries
          }
          Apply(*result);
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

  // The server is the source of truth for Pro, and current_subscription is
  // set exactly when the network is Pro. The jwt's Pro claim is baked in when
  // the token is issued, so it goes stale on BOTH an upgrade and a lapse:
  // refresh the jwt whenever the two disagree, in either direction (macOS
  // SubscriptionBalanceViewModel parity). Request it once per flip, not on
  // every poll.
  const bool serverIsPro = result.current_subscription.has_value();
  if (serverIsPro != jwtPro_) {
    // trigger a refresh; the jwt-refresh listener (OnJwtRefreshed) advances jwtPro_
    // from the real new claim once it lands. Advancing it here optimistically would
    // "forget" the disagreement when RefreshJwt no-ops (device not yet up on resume),
    // leaving the stored token's stale Pro claim frozen for its lifetime.
    sdk_.RefreshJwt();
  }
  snapshot_.isPro = serverIsPro;

  if (IsSupporterWithBalance()) {
    // nothing left to poll for
    StopConfirmation(/*timedOut=*/false);
    StopBackground();
  } else if (!confirming_) {
    EnsureBackgroundPolling();
  }
  Publish();
}

void SubscriptionBalanceStore::EnsureBackgroundPolling() {
  if (!backgroundTimer_ || confirming_ || IsSupporterWithBalance()) return;
  if (!backgroundTimer_.IsRunning()) backgroundTimer_.Start();
}

void SubscriptionBalanceStore::StopBackground() {
  if (backgroundTimer_) backgroundTimer_.Stop();
}

void SubscriptionBalanceStore::StopConfirmation(bool timedOut) {
  if (confirmTimer_) confirmTimer_.Stop();
  confirming_ = false;
  if (timedOut) timedOut_ = true;
}

void SubscriptionBalanceStore::Publish() {
  if (onChange_) onChange_(snapshot_, {confirming_, timedOut_});
}

}  // namespace urnw
