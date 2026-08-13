// The ONE aggregate connection state every user-facing surface renders — the
// connect page's status line, the window status strip, and the tray icon.
//
// WHY THIS EXISTS. The app used to say "Connected" from the tunnel state alone
// (AppController) or from the SDK's four-value connection status (ConnectPage),
// and NOTHING watched whether any exit was actually proven and carrying bytes.
// The two failure modes the owner hit are both silences of that design:
// "Connected but nothing works" (RECOVERY.md documents it as a known gap: the
// transport died, the WFP policy fails closed, and no surface can say so) and
// "yellow forever" (every provider stuck InEvaluation while the headline reads
// Connected). The evidence to say better already reaches the app — the
// provider grid over the device RPC carries a per-provider state, and "Added"
// is the SDK accepting a provider the way the service log's `[rel] proven=M`
// counts them — it was only ever rendered as decorative dots.
//
// The derivation is PURE and lives here, in Common, for the VersionGrammar
// reason: the service selftest links this header and pins the whole transition
// table, so "which word does the UI say for these facts" is a tested claim,
// not a hope. No Windows headers, no allocation, no clock of its own — time
// comes in as an argument (steady-clock millis), which is what makes the
// hysteresis testable.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace urnw::health {

// The five states a user can be told, plus Disconnected. Ordered roughly by
// how much they claim; nothing here maps to protocol values and the order is
// NOT load-bearing.
enum class State {
  // The control pipe is down: no service, so certainly no tunnel. This is also
  // the signed-out launch state (the pipe is only dialled by a bootstrap), so
  // signed-out can never render a false Connected.
  NoService,
  // A service is reachable but nothing is connected or connecting — including
  // an rpc-only session, which deliberately carries nothing.
  Disconnected,
  // A connect is in flight and the provider window is still empty.
  Connecting,
  // The window has providers and NONE is proven yet — the honest name for
  // "all yellow". With the tunnel up this is a fails-closed state: traffic is
  // routed into the tunnel and held, and the UI must say so.
  Evaluating,
  // At least one provider is proven (grid state "Added"), or — with no grid
  // feed to consult — the tunnel's own unverified claim (see Tracker).
  Connected,
  // Was Connected in this attempt and every proven provider has been gone for
  // longer than the hold. Same fails-closed consequence as Evaluating; the
  // different word is the difference between "still working on it" and "this
  // was working and stopped", which is exactly what the owner could not see.
  Degraded,
  // The SDK's window honesty layer declared the attempt failed: zero providers
  // Added past BOTH outcome deadlines (45s to an automatic silent window
  // rebuild, 45s more to this). Terminal until the user retries — the page
  // renders a failure line with the stall reason and a Retry action — or the
  // SDK clears it because a provider finally landed (proven recovery is
  // immediate and one-sided, like everywhere else in this table). This is the
  // "no infinite yellow" state: it can only be entered on the SDK's explicit
  // word (WindowStatus.Failed), never inferred locally from elapsed time.
  Failed,
};

// For logs and tests only — never user-facing (the UI renders store strings).
inline constexpr const char* ToString(State s) {
  switch (s) {
    case State::NoService: return "no_service";
    case State::Disconnected: return "disconnected";
    case State::Connecting: return "connecting";
    case State::Evaluating: return "evaluating";
    case State::Connected: return "connected";
    case State::Degraded: return "degraded";
    case State::Failed: return "failed";
  }
  return "unknown";
}

// What the SDK's connection status contributes: is anything being attempted at
// all. The four live values plus this app's two clamp sentinels (RPC_ONLY,
// SERVICE_DOWN — see SdkHost::ReadStats) fold into three answers; anything
// unrecognised is Inactive, for the ParseConnectStatus reason — an unknown
// status must not leave any surface claiming a connection the SDK never made.
enum class Activity {
  Inactive,    // DISCONNECTED / RPC_ONLY / SERVICE_DOWN / empty / unknown
  Connecting,  // CONNECTING or DESTINATION_SET (android folds these too)
  Active,      // CONNECTED
};

// ASCII-only case fold, matching android's ConnectStatus.fromString. The SDK
// emits exact uppercase; folding is tolerance, not a requirement.
inline constexpr bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if ('a' <= ca && ca <= 'z') ca = static_cast<char>(ca - ('a' - 'A'));
    if ('a' <= cb && cb <= 'z') cb = static_cast<char>(cb - ('a' - 'A'));
    if (ca != cb) return false;
  }
  return true;
}

inline constexpr Activity ActivityFromStatus(std::string_view status) {
  if (EqualsIgnoreAsciiCase(status, "CONNECTED")) return Activity::Active;
  if (EqualsIgnoreAsciiCase(status, "CONNECTING") ||
      EqualsIgnoreAsciiCase(status, "DESTINATION_SET"))
    return Activity::Connecting;
  return Activity::Inactive;
}

// Provider-grid cell classification (ProviderGridPoint::State, the same five
// strings ConnectCanvas::ParsePointState renders). "Added" is the SDK
// accepting the provider — the app-side stand-in for the reliability
// heartbeat's `proven` count, which does not itself cross the device RPC.
inline constexpr bool CellProven(std::string_view state) { return state == "Added"; }
// A cell that occupies the window at all: evaluation, accepted, or failed.
// Removed (and empty/unknown) cells are on their way out and claim nothing.
inline constexpr bool CellOccupiesWindow(std::string_view state) {
  return !state.empty() && state != "Removed";
}

// One snapshot of everything the derivation consumes. The caller (ReadStats)
// fills it from facts it already holds; nothing here issues a call.
struct Signals {
  // The control pipe is up (ServiceClient::IsConnected).
  bool serviceConnected = false;
  // The SDK connection status, folded (ActivityFromStatus over the CLAMPED
  // LiveStats value, so rpc-only and service-down already read as Inactive).
  Activity activity = Activity::Inactive;
  // A LIVE grid feed produced this snapshot (the ConnectViewController is
  // open). False while the window is hidden — the presentation-scoped feed is
  // closed then, so an empty grid is ABSENCE OF EVIDENCE, not evidence of an
  // empty window, and the tracker must not sharpen a claim from it.
  bool gridKnown = false;
  // Provider cells occupying the window (CellOccupiesWindow), or the SDK's own
  // window-size figure — whichever the caller trusts more; the tracker only
  // asks "is the window populated".
  int64_t windowSize = 0;
  // Cells in the Added state (CellProven).
  int64_t provenCount = 0;
  // The SDK's own terminal verdict on this attempt (WindowStatus.Failed over
  // the device RPC): zero Added past both of the window's outcome deadlines.
  // Cleared by the rpc-only and service-down clamps like every other claim —
  // a session that carries nothing cannot have failed to connect.
  bool windowFailed = false;
};

// The stateful half: attempt identity and the degrade hold. One instance lives
// in SdkHost behind its own small lock; everything else reads the result off
// LiveStats.
class Tracker {
 public:
  // How long proven may sit at zero, mid-attempt, before Connected becomes
  // Degraded. The point is the owner's "blips must not flap": a provider
  // migration or a probe cycle takes proven to zero for a second or two
  // routinely, and a status line that flickers Degraded on every one would be
  // noise nobody trusts. Chosen inside the task's 5–10s band; recovery in the
  // other direction is IMMEDIATE (one proven exit is Connected, no wait).
  static constexpr int64_t kDegradeHoldMillis = 7000;

  // Fold one snapshot in. `nowMillis` is a monotonic clock in milliseconds
  // (steady_clock in the app; a plain integer in tests).
  State Update(const Signals& in, int64_t nowMillis) {
    reevalAtMillis_ = 0;
    if (!in.serviceConnected) {
      ResetAttempt();
      return state_ = State::NoService;
    }
    if (in.activity == Activity::Inactive) {
      ResetAttempt();
      return state_ = State::Disconnected;
    }
    if (!in.gridKnown) {
      // No live grid feed: no fresh evidence either way. An evidence-based
      // claim HOLDS (understating on stale evidence beats overstating on
      // none); with no evidence at all, report what the session itself says.
      // The Connected branch marks the attempt proven so that when the feed
      // opens (window shown) an empty first snapshot lands in the degrade
      // hold below instead of flapping Connecting for one frame — and so
      // that a reattach over a DEAD connection becomes Degraded after the
      // hold rather than staying green forever, which is the honest reading
      // of "the tunnel claims up and the evidence never arrived".
      if (state_ == State::Connected || state_ == State::Degraded ||
          state_ == State::Evaluating) {
        return state_;
      }
      if (in.activity == Activity::Active) {
        proven_ = true;
        provenLostAtMillis_ = -1;
        return state_ = State::Connected;
      }
      return state_ = State::Connecting;
    }
    if (in.provenCount >= 1) {
      // Recovery is immediate and one-sided: hysteresis exists to keep a blip
      // from flapping the GOOD state away, never to delay good news.
      proven_ = true;
      provenLostAtMillis_ = -1;
      return state_ = State::Connected;
    }
    if (in.windowFailed && !proven_) {
      // The SDK declared the attempt failed (zero Added past both outcome
      // deadlines). Below the proven branch on purpose: a proven provider is
      // stronger, newer evidence than a latch computed from the window's
      // empty stretch, and the SDK clears the latch on the next Added anyway.
      // The !proven_ guard is defensive symmetry — a failed latch can only
      // exist on a never-proven window by construction, and if that invariant
      // ever broke, Degraded (below) is the honest word for a session that
      // WAS working, not Failed.
      return state_ = State::Failed;
    }
    // proven == 0 from here down.
    if (proven_) {
      // This attempt WAS proven. Hold Connected through the blip window, then
      // call it what it is. Window size deliberately does not matter here — a
      // fully collapsed window after proof is the same loss.
      if (provenLostAtMillis_ < 0) provenLostAtMillis_ = nowMillis;
      if (nowMillis - provenLostAtMillis_ < kDegradeHoldMillis) {
        reevalAtMillis_ = provenLostAtMillis_ + kDegradeHoldMillis;
        return state_ = State::Connected;
      }
      return state_ = State::Degraded;
    }
    if (in.windowSize > 0) return state_ = State::Evaluating;
    return state_ = State::Connecting;
  }

  // The last answer, without folding anything in. For callers that need the
  // aggregate as an INPUT to a decision (the session worker deciding whether
  // the tray's one item is a connect or a disconnect) rather than as something
  // to render — those callers have no snapshot to fold and must not invent one.
  State Current() const { return state_; }

  // When the CLOCK (not a new SDK event) will change the answer: the pending
  // degrade hold's deadline, in the same clock as Update's nowMillis, or 0.
  // Grid events stop arriving exactly when everything is stuck, so whoever
  // renders this must re-ask at the deadline — the connect page does it from
  // its existing 1s tick.
  int64_t ReevalAtMillis() const { return reevalAtMillis_; }

  // A DELIBERATE new connect or disconnect (the session worker applying a
  // user's intent). Proof earned by the previous target must not survive it:
  // without this, changing location mid-session reads the rebuilding window
  // as "was connected, dropped" and shows Degraded for a state the user
  // chose. After this, the same facts read Connecting/Evaluating — which the
  // row-click coalescer's settle window also relies on being the calm answer.
  void NoteNewAttempt() {
    ResetAttempt();
    state_ = State::Connecting;
  }

 private:
  void ResetAttempt() {
    proven_ = false;
    provenLostAtMillis_ = -1;
    reevalAtMillis_ = 0;
  }

  State state_ = State::NoService;
  // This attempt has had a proven provider (or inherited the unverified
  // tunnel claim — see the !gridKnown branch).
  bool proven_ = false;
  int64_t provenLostAtMillis_ = -1;
  int64_t reevalAtMillis_ = 0;
};

}  // namespace urnw::health
