// SPDX-License-Identifier: MPL-2.0
#include "pch.h"

#include "ClientEvents.h"

#include <urnetwork_sdk.h>
#include <winrt/Windows.Globalization.h>

#include "Localization.h"
#include "Log.h"
#include "Strings.h"

namespace urnw {

std::string ClientEventLocale() {
  const std::string tag = Narrow(PrimaryLanguage());
  return tag.empty() ? std::string("en") : tag;
}

std::string LocalTimeZoneId() {
  try {
    // Windows.Globalization.Calendar reports the ICU (IANA) zone id of the
    // system time zone ("America/Los_Angeles"), not the Windows registry name
    winrt::Windows::Globalization::Calendar calendar;
    return Narrow(std::wstring{calendar.GetTimeZone()});
  } catch (...) {
    return std::string();
  }
}

ClientEventQueue::ClientEventQueue(uint64_t networkSpace, const std::string& appVersion,
                                   const std::string& locale)
    : handle_(urnet_new_client_event_queue(networkSpace, "windows", appVersion.c_str(),
                                           locale.c_str())) {}

ClientEventQueue::~ClientEventQueue() {
  if (!handle_) return;
  urnet_client_event_queue_close(handle_);
  urnet_release(handle_);
  handle_ = 0;
}

void ClientEventQueue::Flush() {
  if (handle_) urnet_client_event_queue_flush(handle_);
}

void ClientEventQueue::FlushAndWait(int64_t timeoutMillis) {
  if (handle_) urnet_client_event_queue_flush_and_wait(handle_, timeoutMillis);
}

void ClientEventQueue::NewSession() {
  if (handle_) urnet_client_event_queue_new_session(handle_);
}

int64_t ClientEventQueue::Pending() const {
  return handle_ ? urnet_client_event_queue_pending_count(handle_) : 0;
}

std::string ClientEventQueue::Session() const {
  if (!handle_) return std::string();
  char* s = urnet_client_event_queue_get_session(handle_);
  std::string out = s ? s : "";
  if (s) urnet_free_string(s);
  return out;
}

void ClientEventQueue::Add(char* sdkEventJson) {
  if (!sdkEventJson) return;
  if (handle_) urnet_client_event_queue_add(handle_, sdkEventJson);
  urnet_free_string(sdkEventJson);
}

// ---- the facade -------------------------------------------------------------

void ClientEventQueue::OnboardingStepShown(const std::string& step, int64_t index, int64_t elapsedMs) {
  Add(urnet_new_onboarding_step_shown_event(step.c_str(), index, elapsedMs));
}
void ClientEventQueue::OnboardingStepCompleted(const std::string& step, int64_t index, int64_t elapsedMs) {
  Add(urnet_new_onboarding_step_completed_event(step.c_str(), index, elapsedMs));
}
void ClientEventQueue::OnboardingStepSkipped(const std::string& step, int64_t index, int64_t elapsedMs) {
  Add(urnet_new_onboarding_step_skipped_event(step.c_str(), index, elapsedMs));
}
void ClientEventQueue::ConnectFirst() { Add(urnet_new_connect_first_event()); }
void ClientEventQueue::FeedbackSubmitted(int64_t rating, const std::string& reason,
                                         const std::string& text) {
  Add(urnet_new_feedback_submitted_event(rating, reason.c_str(), text.c_str()));
}
void ClientEventQueue::PurchaseStarted(const std::string& store, const std::string& product,
                                       const std::string& plan, bool trial, double price,
                                       const std::string& currency) {
  Add(urnet_new_purchase_started_event(store.c_str(), product.c_str(), plan.c_str(), trial, price,
                                       currency.c_str()));
}
void ClientEventQueue::PurchaseCompleted(const std::string& store, const std::string& product,
                                         const std::string& plan, bool trial, double price,
                                         const std::string& currency) {
  Add(urnet_new_purchase_completed_event(store.c_str(), product.c_str(), plan.c_str(), trial,
                                         price, currency.c_str()));
}
void ClientEventQueue::PurchaseCancelled(const std::string& store, const std::string& product,
                                         const std::string& plan, bool trial, double price,
                                         const std::string& currency) {
  Add(urnet_new_purchase_cancelled_event(store.c_str(), product.c_str(), plan.c_str(), trial,
                                         price, currency.c_str()));
}
void ClientEventQueue::PurchaseFailed(const std::string& store, const std::string& product,
                                      const std::string& plan, bool trial, double price,
                                      const std::string& currency, const std::string& errorClass) {
  Add(urnet_new_purchase_failed_event(store.c_str(), product.c_str(), plan.c_str(), trial, price,
                                      currency.c_str(), errorClass.c_str()));
}
void ClientEventQueue::OfferScreenShown(const std::string& surface, const std::string& experiment,
                                        const std::string& variant, const std::string& tier,
                                        double priceShown, const std::string& currency,
                                        int64_t expiresInS) {
  Add(urnet_new_offer_screen_shown_event(surface.c_str(), experiment.c_str(), variant.c_str(),
                                         tier.c_str(), priceShown, currency.c_str(), expiresInS));
}
void ClientEventQueue::OfferCardTapped(const std::string& plan) {
  Add(urnet_new_offer_card_tapped_event(plan.c_str()));
}
void ClientEventQueue::OfferCtaTapped(const std::string& plan, const std::string& store) {
  Add(urnet_new_offer_cta_tapped_event(plan.c_str(), store.c_str()));
}
void ClientEventQueue::OfferDeclined(const std::string& control, int64_t elapsedMs) {
  Add(urnet_new_offer_declined_event(control.c_str(), elapsedMs));
}
void ClientEventQueue::SignupOptoutChanged(bool productUpdates) {
  Add(urnet_new_signup_optout_changed_event(productUpdates));
}
void ClientEventQueue::WidgetAdded(const std::string& kind) {
  Add(urnet_new_widget_added_event(kind.c_str()));
}

}  // namespace urnw
