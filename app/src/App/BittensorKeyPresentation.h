// How Earnings words what the sdk decides about the one-tap Bittensor key and
// its claims (apple/BTEASY.md, flows 1 to 4 of the approved mocks): which store
// key each of the sdk's ids reads as, which body the payout card draws for the
// sdk's state, the Key screen's steps, and the claim relay's reasons.
//
// Every decision is the sdk's: BittensorLocalKeyFlow (bittensor_local_key_flow.go)
// says what the card and the Key screen offer, which disclaimer each screen
// shows, when the words show, when a key is made or signs; SnClaimStatus
// (sn_claim_relay.go) says who claims and why a balance waits. Nothing here makes
// a key, signs, or decides when to; this header only picks words and layout for
// what the sdk said, so every platform shows the same thing.
//
// Pure and header-only, for the reason SnConsentPresentation.h gives: a WinUI 3
// app cannot be built off Windows, so what can be is verified by
// tools/bittensor-key-tests.cpp on any host with a C++20 compiler. The sdk's ids
// are mirrored here as the generated wrapper names them (urnetwork_sdk.hpp); the
// spec ties each to the header when one is at hand.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace urnw::btkey {

// ---- the sdk's ids -----------------------------------------------------------

// The card's states (urnet::BittensorLocalKeyState*).
inline constexpr std::string_view kStateLoading = "loading";
inline constexpr std::string_view kStateConsentUnavailable = "consent_unavailable";
inline constexpr std::string_view kStateStoreUnavailable = "store_unavailable";
inline constexpr std::string_view kStateNone = "none";
inline constexpr std::string_view kStateSaveNowShowing = "save_now_showing";
inline constexpr std::string_view kStateSaveNowConsentFailed = "save_now_consent_failed";
inline constexpr std::string_view kStateSaveLaterNotLinked = "save_later_not_linked";
inline constexpr std::string_view kStateSaveLaterLinked = "save_later_linked";
inline constexpr std::string_view kStateSavedAndDeleted = "saved_and_deleted";
inline constexpr std::string_view kStateElsewhere = "elsewhere";

// Why the last action stopped (urnet::BittensorLocalKeyFailure*), and the
// consent's refusals, which keep their sdk codes (urnet::SnErrorCode*).
inline constexpr std::string_view kFailureGenerate = "generate_failed";
inline constexpr std::string_view kFailureStore = "store_failed";
inline constexpr std::string_view kFailureStoreUnavailable = "store_unavailable";
inline constexpr std::string_view kFailureDelete = "delete_failed";
inline constexpr std::string_view kFailureNotAllowed = "not_allowed";
inline constexpr std::string_view kFailureReplacementNotConfirmed = "replacement_not_confirmed";
inline constexpr std::string_view kFailureConsent = "consent_failed";
inline constexpr std::string_view kCodeChallengeExpired = "challenge_expired";
inline constexpr std::string_view kCodeSignatureMismatch = "signature_mismatch";
inline constexpr std::string_view kCodeNetworkOwnerOnly = "network_owner_only";

// What the Key screen asks two words for (urnet::BittensorLocalKeyConfirm*).
inline constexpr std::string_view kConfirmNone = "";
inline constexpr std::string_view kConfirmDelete = "delete";
inline constexpr std::string_view kConfirmKeep = "keep";

// A typed word, to the two-word check (urnet::BittensorLocalKeyWord*).
inline constexpr std::string_view kWordWrong = "wrong";

// What the one reminder asks of the app (urnet::BittensorLocalKeyReminder*).
inline constexpr std::string_view kReminderNone = "none";
inline constexpr std::string_view kReminderSchedule = "schedule";
inline constexpr std::string_view kReminderDeliverNow = "deliver_now";
inline constexpr std::string_view kReminderCancel = "cancel";

// The funds disclaimer (urnet::BittensorFundsDisclaimer*): the sdk hands out the
// id of the store key itself, and the places a screen asks about.
inline constexpr std::string_view kDisclaimerNone = "";
inline constexpr std::string_view kDisclaimerFull = "bittensor_funds_disclaimer";
inline constexpr std::string_view kDisclaimerFundsOnly = "bittensor_funds_disclaimer_funds_only";
inline constexpr std::string_view kPlaceChangeWallet = "change_wallet";
inline constexpr std::string_view kPlaceClaimSheet = "claim_sheet";

// Why a v2 balance waits (urnet::SnClaimReason*), and why Claim now can't be
// tapped (urnet::SnClaimNowReason*).
inline constexpr std::string_view kReasonNothingPending = "nothing_pending";
inline constexpr std::string_view kReasonUnderThreshold = "under_threshold";
inline constexpr std::string_view kReasonQueued = "queued";
inline constexpr std::string_view kReasonCapReached = "cap_reached";
inline constexpr std::string_view kReasonRefused = "refused";
inline constexpr std::string_view kReasonRelayNotRunning = "relay_not_running";
inline constexpr std::string_view kReasonRelayPaused = "relay_paused";
inline constexpr std::string_view kClaimNowUnderFloor = "under_floor";
inline constexpr std::string_view kClaimNowRequested = "requested";
inline constexpr std::string_view kClaimNowLimit = "limit";
// What the main control does (urnet::SnClaimAction*): nothing to claim.
inline constexpr std::string_view kActionNone = "none";

// ---- the funds disclaimer ------------------------------------------------------

// The store key of the disclaimer the sdk names, "" for none. The owner's words
// are those two keys, word for word: anything else the sdk might say shows
// nothing rather than a raw id.
inline std::string DisclaimerKey(std::string_view id) {
  if (id == kDisclaimerFull || id == kDisclaimerFundsOnly) return std::string(id);
  return std::string();
}

// ---- failures --------------------------------------------------------------------

// What a failure the flow reports reads as: a store key, or the sdk's or the
// server's own message when it has words of its own and the store has none.
// Both empty for no failure.
struct FailureText {
  std::string key;
  std::string message;

  bool empty() const { return key.empty() && message.empty(); }
  bool operator==(const FailureText&) const = default;
};

inline FailureText FailureTextFor(std::string_view code, std::string_view message) {
  FailureText text;
  if (code.empty()) return text;
  if (code == kCodeChallengeExpired) {
    text.key = "sn_consent_expired";
  } else if (code == kCodeSignatureMismatch) {
    text.key = "sn_consent_failed";
  } else if (code == kCodeNetworkOwnerOnly || code == kFailureNotAllowed) {
    text.key = "sn_consent_owner_only";
  } else if (code == kFailureGenerate) {
    text.key = "bittensor_key_failure_generate";
  } else if (code == kFailureStore) {
    text.key = "bittensor_key_failure_store";
  } else if (code == kFailureStoreUnavailable) {
    text.key = "bittensor_key_failure_keychain";
  } else if (code == kFailureDelete) {
    text.key = "bittensor_key_failure_delete";
  } else if (code == kFailureReplacementNotConfirmed) {
    // the card warns first, so this reads only if a warning was skipped
    text.key = "bittensor_key_replace_message";
  } else if (message.empty()) {
    text.key = "bittensor_key_failure_consent";
  } else {
    text.message = std::string(message);
  }
  return text;
}

// ---- the payout card -------------------------------------------------------------

// What the network's consent read says, as the consent panel already knows it.
enum class ConsentLine {
  Unknown,  // loading, failed, or not read: the panel's header says which
  None,     // no consent in force
  Active,   // a consent pays a coldkey
};

// The body the payout card draws (the mocks' frames).
enum class CardBody {
  WalletCard,        // the panel as it was: sn_consent_* lines, Sign with wallet or Change wallet
  SetUp,             // 1a, 1b: no payout wallet, Set up payouts and its two links
  FinishSetup,       // 3i: a key here the consent doesn't pay, and no consent
  StoreUnavailable,  // the store can't answer: bittensor_key_failure_keychain, Retry
  Linked,            // 1c, 1i: the consent pays the key on this device
  Deleted,           // 1l, 2h: the consent pays a key this device made and no longer has
  Elsewhere,         // 3a, 3d: an outside wallet or another device's key, one card for both
};

struct CardLayout {
  CardBody body = CardBody::WalletCard;
  // under the wallet card's lines, with another consent in force: the key here
  // isn't paid (bittensor_key_card_other_consent), Finish setup, Back up or View
  bool otherConsentLines = false;
  // under the wallet card's lines: the store can't answer, Retry
  bool storeUnavailableLines = false;

  bool operator==(const CardLayout&) const = default;
};

// The card's body from the consent read and the sdk's card: with no consent the
// key's path while the sdk shows it (ShowsKeyPath); with a consent, the key's
// body for a key the consent pays, or that this device made, or that is
// elsewhere; the wallet card's lines otherwise, with the key's lines under them.
inline CardLayout CardLayoutFor(ConsentLine line, std::string_view state, bool showsKeyPath) {
  CardLayout layout;
  switch (line) {
    case ConsentLine::Unknown:
      return layout;
    case ConsentLine::None:
      if (!showsKeyPath) return layout;
      if (state == kStateSaveLaterNotLinked) {
        layout.body = CardBody::FinishSetup;
      } else if (state == kStateStoreUnavailable) {
        layout.body = CardBody::StoreUnavailable;
      } else {
        layout.body = CardBody::SetUp;
      }
      return layout;
    case ConsentLine::Active:
      if (state == kStateSaveLaterLinked) {
        layout.body = CardBody::Linked;
      } else if (state == kStateSavedAndDeleted) {
        layout.body = CardBody::Deleted;
      } else if (state == kStateElsewhere) {
        layout.body = CardBody::Elsewhere;
      } else if (state == kStateSaveLaterNotLinked) {
        layout.otherConsentLines = true;
      } else if (state == kStateStoreUnavailable) {
        layout.storeUnavailableLines = true;
      }
      return layout;
  }
  return layout;
}

// The title of a card whose consent pays this device's key, or one it made.
inline std::string PayoutsTitleKey(bool payoutsStartNextEpoch) {
  return payoutsStartNextEpoch ? "bittensor_key_payouts_start" : "bittensor_key_payouts_on";
}

// ---- the Key screen --------------------------------------------------------------

// The screen's content, one at a time, so the disclaimer shows once: the two
// words while the sdk asks for them, what deleting means after Delete and
// before the words, the screen itself otherwise.
enum class KeyStep {
  Main,
  DeleteExplained,
  Confirm,
};

inline KeyStep KeyStepFor(std::string_view confirming, bool deleteExplained) {
  if (confirming != kConfirmNone) return KeyStep::Confirm;
  if (deleteExplained) return KeyStep::DeleteExplained;
  return KeyStep::Main;
}

// The banner at the top of the main step.
enum class KeyBanner {
  None,
  Signing,  // bittensor_key_signing, with a spinner
  Retry,    // the failure, Retry, bittensor_key_save_now_retry
  SaveNow,  // bittensor_key_save_now_banner
};

inline KeyBanner KeyBannerFor(bool busy, bool offersRetry, bool saveNow) {
  if (busy) return KeyBanner::Signing;
  if (offersRetry) return KeyBanner::Retry;
  if (saveNow) return KeyBanner::SaveNow;
  return KeyBanner::None;
}

// A line of what deleting means: its key, and whether it names the address.
struct Bullet {
  std::string key;
  bool namesAddress = false;

  bool operator==(const Bullet&) const = default;
};

// What deleting means (1j, 2d): its title and bullets, a stored key's or Save
// now's, whose same step sets up payouts first.
struct DeleteText {
  std::string titleKey;
  std::vector<Bullet> bullets;

  bool operator==(const DeleteText&) const = default;
};

inline DeleteText DeleteTextFor(bool saveNow) {
  if (saveNow) {
    return DeleteText{"bittensor_key_delete_save_now_title",
                      {{"bittensor_key_save_now_bullet_address", true},
                       {"bittensor_key_delete_bullet_no_recovery", false},
                       {"bittensor_key_save_now_bullet_phrase", false},
                       {"bittensor_key_save_now_bullet_convert", false}}};
  }
  return DeleteText{"bittensor_key_delete_title_2",
                    {{"bittensor_key_delete_bullet_no_recovery", false},
                     {"bittensor_key_delete_bullet_address", true},
                     {"bittensor_key_delete_bullet_phrase", false},
                     {"bittensor_key_delete_bullet_convert", false}}};
}

// The two words (1h, 1k, 2e): Keep's or Delete's title, body and button.
struct ConfirmText {
  std::string titleKey;
  std::string bodyKey;    // Keep's explanation, "" for Delete
  std::string promptKey;  // what to enter
  std::string buttonKey;
  bool destructive = false;  // coral filled

  bool operator==(const ConfirmText&) const = default;
};

inline ConfirmText ConfirmTextFor(std::string_view confirming, bool saveNow) {
  if (confirming == kConfirmKeep) {
    return ConfirmText{"bittensor_key_keep_title_2",
                       saveNow ? "bittensor_key_keep_save_now_body" : "bittensor_key_keep_body",
                       "bittensor_key_keep_prompt", "bittensor_key_keep_confirm_2", false};
  }
  return ConfirmText{"bittensor_key_enter_two_words", "", "bittensor_key_enter_two_words_body",
                     saveNow ? "bittensor_key_save_now_delete_confirm" : "bittensor_key_delete_confirm",
                     true};
}

// "0:58": what is left until the words hide by themselves, rounded up to the
// second, never below 0:00.
inline std::string Countdown(int64_t untilMillis, int64_t nowMillis) {
  int64_t left = untilMillis - nowMillis;
  if (left < 0) left = 0;
  const int64_t seconds = (left + 999) / 1000;
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", static_cast<long long>(seconds / 60),
                static_cast<long long>(seconds % 60));
  return buffer;
}

// ---- the one reminder ------------------------------------------------------------

// What the app does with the sdk's reminder. Windows has no notification the
// system delivers later for this app, so the app posts it itself: it waits for
// the moment while it runs (Arm), and reports nothing for a schedule, since the
// sdk takes a reported schedule as delivered once its moment passes, which a
// closed app would make untrue. A later refresh asks again, and a moment that
// passed while the app was closed comes back as deliver now. Only a reminder
// that went out is reported (ReminderDelivered).
enum class ReminderStep {
  Nothing,
  Arm,      // post it at the sdk's moment, if the app still runs then
  PostNow,  // post it, then ReminderDelivered
  Disarm,   // a moment armed is not waited for any more
};

inline ReminderStep ReminderStepFor(std::string_view action) {
  if (action == kReminderSchedule) return ReminderStep::Arm;
  if (action == kReminderDeliverNow) return ReminderStep::PostNow;
  if (action == kReminderCancel) return ReminderStep::Disarm;
  return ReminderStep::Nothing;
}

// ---- claims through the relay ------------------------------------------------------

// URnetwork is the one claiming, now or once a balance or a new day allows: the
// relay row shows, and Claim now. Otherwise the device's own claim is the way.
inline bool RelayClaims(std::string_view reason) {
  return reason != kReasonRelayNotRunning && reason != kReasonRelayPaused;
}

// Why a balance waits: a title, and the line under it.
enum class ReasonDetail {
  None,
  Threshold,         // sn_claim_relay_under_threshold_body, the threshold in TAO and in SN25α
  ThresholdNoPrice,  // sn_claim_relay_under_threshold_no_price, in TAO only
  Waits,             // sn_claim_relay_waits
};

struct ReasonText {
  std::string titleKey;  // "" for none
  ReasonDetail detail = ReasonDetail::None;

  bool operator==(const ReasonText&) const = default;
};

inline ReasonText ReasonTextFor(std::string_view reason, int64_t thresholdTaoRao, int64_t thresholdRao) {
  ReasonText text;
  if (reason == kReasonUnderThreshold) {
    text.titleKey = "sn_claim_relay_under_threshold";
    if (0 < thresholdTaoRao) {
      text.detail = 0 < thresholdRao ? ReasonDetail::Threshold : ReasonDetail::ThresholdNoPrice;
    }
  } else if (reason == kReasonQueued) {
    text.titleKey = "sn_claim_relay_queued";
  } else if (reason == kReasonCapReached) {
    text.titleKey = "sn_claim_relay_cap";
  } else if (reason == kReasonRefused) {
    text.titleKey = "sn_claim_relay_refused";
  } else if (reason == kReasonRelayNotRunning) {
    text.titleKey = "sn_claim_relay_not_running";
    text.detail = ReasonDetail::Waits;
  } else if (reason == kReasonRelayPaused) {
    text.titleKey = "sn_claim_relay_paused";
    text.detail = ReasonDetail::Waits;
  }
  return text;
}

// The line under Claim now: who pays and how often, or why it can't act.
enum class ClaimNowNote {
  None,
  Paid,        // sn_claim_relay_claim_now_paid, on the tile
  PaidEach,    // sn_claim_relay_claim_now_paid_each, in the claim sheet
  UnderFloor,  // sn_claim_relay_under_floor, the floor in TAO
  Requested,   // sn_claim_relay_requested
  AgainOn,     // sn_claim_relay_again_on, the limit's date
};

inline ClaimNowNote ClaimNowNoteFor(bool available, int64_t periodDays, std::string_view claimNowReason,
                                    int64_t floorTaoRao, int64_t nextAtMillis, bool inSheet) {
  if (available) {
    if (periodDays <= 0) return ClaimNowNote::None;
    return inSheet ? ClaimNowNote::PaidEach : ClaimNowNote::Paid;
  }
  if (claimNowReason == kClaimNowUnderFloor) {
    return 0 < floorTaoRao ? ClaimNowNote::UnderFloor : ClaimNowNote::None;
  }
  if (claimNowReason == kClaimNowRequested) return ClaimNowNote::Requested;
  if (claimNowReason == kClaimNowLimit) {
    return 0 < nextAtMillis ? ClaimNowNote::AgainOn : ClaimNowNote::None;
  }
  return ClaimNowNote::None;
}

// The claimed state (the mocks' 4d): URnetwork's claim is the latest and nothing
// is claimable, so Claim now has nothing to act on. The line under it says when
// it is available again, while its limit holds, and nothing else.
inline ClaimNowNote ClaimedNoteFor(int64_t nextAtMillis) {
  return 0 < nextAtMillis ? ClaimNowNote::AgainOn : ClaimNowNote::None;
}

// The claimed state's words: "Claimed by URnetwork. 0.8420 SN25α is on your key,
// 5F3s…kQ9v." with what URnetwork's claim paid and the coldkey it landed on (the
// sdk's ClaimAmountRao and ColdkeySs58), or "Claimed" when the status lacks either.
enum class ClaimedWords {
  Claimed,  // claim_confirmed
  OnKey,    // sn_claim_relay_claimed: {0} the amount, number only; {1} the address, shortened
};

inline ClaimedWords ClaimedWordsFor(int64_t claimAmountRao, std::string_view coldkeySs58) {
  if (claimAmountRao <= 0 || coldkeySs58.empty()) return ClaimedWords::Claimed;
  return ClaimedWords::OnKey;
}

// ---- amounts ------------------------------------------------------------------------

// "0.0125": an amount in rao of TAO, for a sentence that names TAO itself.
inline std::string TaoAmount(int64_t taoRao) {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.4f", static_cast<double>(taoRao) / 1e9);
  return buffer;
}

// "3", "1.5" or "0.0420": an estimate in rao of SN25α, as precise as its size
// needs, for "about 3 SN25α".
inline std::string ApproxAlpha(int64_t rao) {
  const double value = static_cast<double>(rao) / 1e9;
  char buffer[48];
  if (10 <= value) {
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
  } else if (1 <= value) {
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
  }
  return buffer;
}

}  // namespace urnw::btkey
