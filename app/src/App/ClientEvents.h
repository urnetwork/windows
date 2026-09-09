// The app-wide client event queue: the one place the desktop app records the
// product events the onboarding optimization loop reads (POST /client/events,
// a closed schema the server validates). The Linux ClientEvents.hpp, ported.
//
// The SDK owns both the schema and the queue. Every event is built by an SDK
// constructor (urnet_new_*_event, which returns the event as JSON with its
// props) and handed to the SDK's ClientEventQueue over the C ABI
// (urnet_client_event_queue_add), which fills the envelope (at, platform,
// app_version, locale, session), persists the pending events in the network
// space, batches them to the server and retries. Nothing here assembles an
// event by hand, so the desktop cannot drift from the other apps; the typed
// hpp wrapper (ClientEventQueue::add(ClientEvent)) is deliberately not used
// because the typed ClientEvent carries no props.
//
// Threading: the facade may be called from any thread; the SDK queue is
// thread-safe on its own.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string>

namespace urnw {

class ClientEventQueue {
 public:
  // networkSpace: the SDK network space's C handle (the queue sends through
  // its Api, so a session's jwt applies).
  ClientEventQueue(uint64_t networkSpace, const std::string& appVersion, const std::string& locale);
  ~ClientEventQueue();

  ClientEventQueue(const ClientEventQueue&) = delete;
  ClientEventQueue& operator=(const ClientEventQueue&) = delete;

  // Sends what is pending now (window hide, logout, quit).
  void Flush();
  // Sends and waits for the batch, bounded (quit).
  void FlushAndWait(int64_t timeoutMillis);
  // A new session id for the events that follow (launch, sign-in).
  void NewSession();
  int64_t Pending() const;
  std::string Session() const;

  // ---- the facade: one method per SDK event constructor -------------------
  void OnboardingStepShown(const std::string& step, int64_t index, int64_t elapsedMs);
  void OnboardingStepCompleted(const std::string& step, int64_t index, int64_t elapsedMs);
  void OnboardingStepSkipped(const std::string& step, int64_t index, int64_t elapsedMs);
  void ConnectFirst();
  void FeedbackSubmitted(int64_t rating, const std::string& reason, const std::string& text);
  void PurchaseStarted(const std::string& store, const std::string& product,
                       const std::string& plan, bool trial, double price,
                       const std::string& currency);
  void PurchaseCompleted(const std::string& store, const std::string& product,
                         const std::string& plan, bool trial, double price,
                         const std::string& currency);
  void PurchaseCancelled(const std::string& store, const std::string& product,
                         const std::string& plan, bool trial, double price,
                         const std::string& currency);
  void PurchaseFailed(const std::string& store, const std::string& product,
                      const std::string& plan, bool trial, double price,
                      const std::string& currency, const std::string& errorClass);
  void OfferScreenShown(const std::string& surface, const std::string& experiment,
                        const std::string& variant, const std::string& tier, double priceShown,
                        const std::string& currency, int64_t expiresInS);
  void OfferCardTapped(const std::string& plan);
  void OfferCtaTapped(const std::string& plan, const std::string& store);
  void OfferDeclined(const std::string& control, int64_t elapsedMs);
  void SignupOptoutChanged(bool productUpdates);
  void WidgetAdded(const std::string& kind);

 private:
  // Hands the SDK constructor's JSON to the SDK queue (ownership: freed with
  // urnet_free_string).
  void Add(char* sdkEventJson);

  uint64_t handle_ = 0;
};

// The locale tag the events and the client registration carry ("en-US", BCP
// 47), from the app's primary language.
std::string ClientEventLocale();
// The IANA time zone id the client registration carries ("Europe/Berlin"),
// from Windows.Globalization.Calendar (ICU ids). Empty when unknown.
std::string LocalTimeZoneId();

}  // namespace urnw
