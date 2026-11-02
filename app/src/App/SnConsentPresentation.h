// Everything Earnings' payout consent decides before it touches a XAML object:
// what its panel after settlement v2 shows for every state of the consent read
// and of a signing in flight, which coldkey the user typed for it, which store
// sentence a refusal shows, and what is submitted once the wallet has signed.
//
// Why it exists: settlement v2 pays a network's app-only providers only to a
// coldkey the network's owner consented to (v2/CONSENT.md, sn VERSION2-FINAL.md
// §11). One sr25519 signature by the coldkey over the server's exact network
// consent message covers every provider client of the network, from the next
// epoch until it is changed. The flow, as every app runs it: choose the wallet,
// type the coldkey the consent names (checked by the SDK's ss58 rule before any
// request), have the server issue the message for it (SnNetworkConsentChallenge),
// have the wallet sign it under the consent purpose through the coldkey
// connect's own connector, the SDK session helper with its bridge page
// (SdkHost::SignNetworkConsent), and submit it
// (SnNetworkConsentSubmit) only when the signature is the server's message's,
// by the coldkey it names. Change wallet runs the same flow again.
//
// It is all here, and all pure, for the reason SnPayoutPresentation.h gives: a
// WinUI 3 app cannot be built off Windows, so every decision expressed on plain
// values is verified by tools/sn-consent-tests.cpp on any host with a C++20
// compiler. Header-only, without WinRT or the SDK. The SDK's status and
// challenge are mirrored as plain views; WalletPage.cpp copies the fields
// across at its boundary (StatusFrom, ChallengeFrom).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace urnw::snconsent {

// The codes a refusal carries, as the SDK's error strings start with them
// ("code: detail"): urnet::SnErrorCodeChallengeExpired (the session's own
// expiry, or the server's refusal of an expired message),
// urnet::SnErrorCodeSignatureMismatch (SnSetWalletResult's Error.Code for a
// signature that does not verify for the coldkey) and
// urnet::SnErrorCodeNetworkOwnerOnly (a session that is not the network
// owner's). The spec ties them to the header.
inline constexpr std::string_view kCodeChallengeExpired = "challenge_expired";
inline constexpr std::string_view kCodeSignatureMismatch = "signature_mismatch";
inline constexpr std::string_view kCodeNetworkOwnerOnly = "network_owner_only";

// The network's consent as the SDK reads it (SnNetworkConsentStatus): its
// newest accepted consent, active while it pays or is about to (its through
// epoch has not passed). The server takes only a consent for a later epoch, so
// one signed a moment ago is active, starting next epoch.
struct Status {
  bool active = false;
  std::string coldkeySs58;
  int64_t fromEpoch = 0;
  int64_t throughEpoch = 0;

  bool operator==(const Status&) const = default;
};

// The server's challenge (SnNetworkConsentChallengeResult): the exact message
// the coldkey signs, the coldkey and epochs it names, and when it expires.
struct Challenge {
  std::string message;
  std::string coldkeySs58;
  int64_t fromEpoch = 0;
  int64_t throughEpoch = 0;
  int64_t expiresAt = 0;  // unix seconds

  bool operator==(const Challenge&) const = default;
};

// Where the page's read of the consent stands.
enum class Fetch {
  Idle,  // nothing asked: no session to read it with
  Loading,
  Ready,
  Failed,
};

// The panel's first line.
enum class Line {
  Hidden,
  None,    // sn_consent_none: no consent pays this network's earnings
  Active,  // sn_consent_active, with the consent's coldkey shortened
};

// What the panel shows, under its heading (sn_consent_title).
struct PanelView {
  bool shown = false;
  bool loading = false;  // "Loading..." in the header
  bool failed = false;   // the read failed: something_went_wrong in the header
  Line line = Line::Hidden;
  std::string coldkeySs58;  // the consent's, for sn_consent_active
  // sn_consent_body, sn_consent_change_note or sn_consent_signing; "" for none
  std::string noteKey;
  // sn_consent_sign or sn_consent_change; "" for no button
  std::string buttonKey;
  bool buttonEnabled = false;

  bool operator==(const PanelView&) const = default;
};

// The panel, from whether this session can start a signing (an api session,
// and no coldkey connect out), the read, the consent it answered with (kept
// while the next read is out; none after a failed read) and whether a consent
// signing is out. Hidden until a read is asked for: the consent is the
// network's, so it needs a session and not a coldkey. No consent: sn_consent_none,
// what signing does (sn_consent_body) and Sign with wallet. A consent: whose
// coldkey it pays (sn_consent_active), and Change wallet with the next-epoch
// note, which is also the state right after a signing. While a signing is out
// the note says the wallet is awaited (sn_consent_signing) and the button waits.
inline PanelView PanelFor(bool canSign, Fetch fetch, const std::optional<Status>& status,
                          bool signing) {
  PanelView view;
  if (fetch == Fetch::Idle) return view;
  view.shown = true;
  view.loading = fetch == Fetch::Loading;
  view.failed = fetch == Fetch::Failed;
  const std::optional<Status> known = view.failed ? std::nullopt : status;
  // nothing to say until a read answers: the header says Loading or the failure
  if (!known && !signing) return view;
  const bool active = known && known->active;
  if (known) {
    view.line = active ? Line::Active : Line::None;
    if (active) view.coldkeySs58 = known->coldkeySs58;
  }
  view.noteKey = signing  ? "sn_consent_signing"
                 : active ? "sn_consent_change_note"
                          : "sn_consent_body";
  view.buttonKey = active ? "sn_consent_change" : "sn_consent_sign";
  view.buttonEnabled = canSign && !signing;
  return view;
}

// The coldkey the consent will name, from what the user typed (never
// prefilled: the payout coldkey is a deliberate choice, and may be another
// account than the one that signs in): trimmed, and only once `valid` passes,
// the SDK's ss58 rule (urnet::validateSs58), before any request is made for it.
// nullopt for nothing typed, or an address `valid` refuses.
template <typename Valid>
std::optional<std::string> ColdkeyFrom(std::string_view typed, Valid valid) {
  constexpr std::string_view kSpace = " \t\r\n";
  const size_t first = typed.find_first_not_of(kSpace);
  if (first == std::string_view::npos) return std::nullopt;
  const std::string coldkey(typed.substr(first, typed.find_last_not_of(kSpace) - first + 1));
  if (!valid(coldkey)) return std::nullopt;
  return coldkey;
}

// The store sentence for a refusal of the consent flow, from the SDK's code or
// an error string that starts with one ("challenge_expired: ..."): an expired
// challenge (sn_consent_expired), a signature that does not verify for the
// coldkey (sn_consent_failed), a session that is not the network owner's
// (sn_consent_owner_only). "" for any other refusal, which the page shows on
// its existing error surface.
inline std::string RefusalKey(std::string_view codeOrError) {
  const auto is = [codeOrError](std::string_view code) {
    return codeOrError.starts_with(code) &&
           (codeOrError.size() == code.size() || codeOrError[code.size()] == ':');
  };
  if (is(kCodeChallengeExpired)) return "sn_consent_expired";
  if (is(kCodeSignatureMismatch)) return "sn_consent_failed";
  if (is(kCodeNetworkOwnerOnly)) return "sn_consent_owner_only";
  return std::string();
}

// What goes to SnNetworkConsentSubmit once the wallet has signed: the
// challenge's own coldkey and message, byte for byte, with the wallet's
// signature.
struct Submission {
  std::string coldkeySs58;
  std::string message;
  std::string signatureHex;

  bool operator==(const Submission&) const = default;
};

// The session already refuses a hand-back for another message or another
// account; this is the page's own guarantee that it submits nothing but the
// server's message, for the coldkey the message names. nullopt (the page says
// sn_consent_failed) when the signed answer is not for exactly that message and
// coldkey, or carries no signature.
inline std::optional<Submission> SubmissionFor(const Challenge& challenge,
                                               std::string_view signedAddress,
                                               std::string_view signedMessage,
                                               std::string_view signatureHex) {
  if (challenge.message.empty() || challenge.coldkeySs58.empty() || signatureHex.empty()) {
    return std::nullopt;
  }
  if (signedMessage != challenge.message || signedAddress != challenge.coldkeySs58) {
    return std::nullopt;
  }
  return Submission{challenge.coldkeySs58, challenge.message, std::string(signatureHex)};
}

// ---- at the SDK's boundary (WalletPage.cpp, SdkHost.cpp)
//
// Templates over the generated wrapper's types (urnetwork_sdk.hpp's
// urnet::SnNetworkConsentStatus and urnet::SnNetworkConsentChallengeResult,
// whose fields are the json shapes' snake_case names): the spec builds them
// against stand-ins with the wrapper's names and types and, when a header is at
// hand, against the header itself; the page and the host instantiate them with
// the SDK's own.

// The SDK's SnNetworkConsentStatus as the plain view.
template <typename SdkStatus>
Status StatusFrom(const SdkStatus& status) {
  Status out;
  out.active = status.active;
  out.coldkeySs58 = status.coldkey_ss58;
  out.fromEpoch = status.from_epoch;
  out.throughEpoch = status.through_epoch;
  return out;
}

// The SDK's SnNetworkConsentChallengeResult as the plain view.
template <typename SdkChallenge>
Challenge ChallengeFrom(const SdkChallenge& challenge) {
  Challenge out;
  out.message = challenge.message;
  out.coldkeySs58 = challenge.coldkey_ss58;
  out.fromEpoch = challenge.from_epoch;
  out.throughEpoch = challenge.through_epoch;
  out.expiresAt = challenge.expires_at;
  return out;
}

}  // namespace urnw::snconsent
