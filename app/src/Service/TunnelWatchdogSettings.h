// The tunnel watchdog's tunables (TunnelWatchdog.h): its cadence, the
// dead-tunnel verdict's thresholds, the freeze bars, its join budget, the
// network-change window and the capture transaction's timing. Each is a field
// with its default here, and the watchdog reads it from the instance it was
// built with (WatchdogChannel::settings), never from a constant, so a test or a
// caller can move it. The controller hands the capture part to each session's
// CaptureReadiness, so the readiness, the sampler that feeds it and the
// evaluator that watches it read the same values.
//
// Every number is justified against something measured or recorded in this
// repo, not guessed: a failsafe with invented thresholds is a second bug
// wearing the first one's clothes.
//
// Pure C++ with no Windows or SDK header, so tools/watchdog-settings-tests.cpp
// reads the same defaults the service runs with. The verdict's shape follows
// urnetwork/linux's DeadTunnelSettings (TunnelWatchdog.hpp).
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

#include "CaptureReadiness.h"     // CaptureReadinessSettings
#include "NetworkChangeNotify.h"  // NetworkNotifySettings

namespace urnw {

// The dead-tunnel verdict's thresholds (Evaluate, DeadTunnelVerdict.h). All
// durations are milliseconds on the steady clock the verdict is given.
struct DeadTunnelSettings {
  // DEAD_FAST, and the carrying veto's window: a packet back out of the tun
  // within it means the tunnel carries, and nothing fires.
  //
  // Must exceed the longest HONEST machine-wide inbound gap. The Windows DNS
  // client's query schedule across a server list runs ~12-13 s and a
  // black-holed TCP connect costs ~21 s (both recorded on kConnectingWindow in
  // TunnelController.cpp) — but the TCP one stalls ONE FLOW, not every flow,
  // while this window is machine-wide. 20 s is >1.5x the DNS schedule and under
  // the ~30 s at which a human has already concluded the internet is broken.
  // Zero or less: no carrying veto, and NoInbound fires as soon as its packets
  // are in.
  int64_t deadFastMillis = 20000;
  // Packets into the tun since anything came back that it takes to arm
  // DEAD_FAST: REAL committed traffic, not one stray packet. With the connected
  // policy in force the host stack retransmits, so a machine that is genuinely
  // trying produces this in well under a second; an idle machine never does,
  // and MUST NOT — an idle blocked machine is indistinguishable from an idle
  // working one. Zero: NoInbound needs no traffic, and judges an idle machine.
  uint64_t deadFastOutboundPackets = 8;
  // DEAD_SLOW: no proven exit at all for this long, and nothing coming back to
  // contradict it. One honest COLD attempt after a roam is DNS (<=13 s) + TCP
  // (<=21 s) + handshake, ~35 s; 90 s is ~2.5x that, so a roam that recovers on
  // its second attempt is never torn down, and the never-proven connect on a
  // hostile network is bounded at ~90 s instead of forever. Zero or less:
  // NoExit fires as soon as the sampler has answered once with no proven exit.
  int64_t deadSlowMillis = 90000;
  // UNRESPONSIVE: the sampler has not COMPLETED a getExits() in this long. 15
  // of the default 2 s samples: a cgo call that has not returned in 30 s is
  // wedged, not slow, and this is the ONLY condition that can fire during the
  // silent-death family of task #39, because it needs no cooperation from the
  // SDK whatsoever. Zero or less: fires on the first tick nothing vetoes.
  int64_t sdkUnresponsiveMillis = 30000;
  // How overdue a sample must be before the unresponsive countdown is worth
  // SHOWING (TunnelStatus::failsafe_armed). Below it is the normal sample
  // cadence, and reporting "armed" there would mean reporting it forever. Zero
  // or less: the countdown always counts towards the notice.
  int64_t sdkUnresponsiveArmMillis = 10000;
  // How close a verdict must be before the UI is told a countdown is running:
  // the teardown must never be a surprise, and a warning 90 s early is noise
  // that teaches the owner to ignore the one that matters. Zero: shown only
  // when a verdict is due on this tick; negative: never shown.
  int64_t failsafeNoticeMillis = 30000;
};

struct TunnelWatchdogSettings {
  // How often the sampler reads the SDK (getExits), ms. The verdict thread
  // ticks faster and touches no SDK, so a wedged sampler cannot stop the verdict
  // being reached. Zero or less: the sampler reads on every pass, without
  // sleeping — a busy loop, not a supported value.
  int64_t sampleIntervalMillis = 2000;
  // How often the verdict is recomputed, ms: faster than the sampler and below
  // the shortest verdict window, so the verdict is never decided by the
  // sampling rate. Zero or less: the evaluator never sleeps — a busy loop, not
  // a supported value.
  int64_t evaluateIntervalMillis = 1000;
  // THE FREEZE BAR, ms: an evaluator tick this late means THIS PROCESS WAS NOT
  // RUNNING. Every verdict window is a duration on a steady clock, and Modern
  // Standby (S0ix) freezes every thread while that clock keeps advancing, so a
  // laptop closed for the night resumes with a sample age of eight hours; the
  // first tick after it would fire UNRESPONSIVE about an sdk that was never
  // asked. A gap this large is a freeze however it was caused — modern standby,
  // hibernate, a suspended VM, a machine so loaded the evaluator missed five
  // ticks — and the session is owed a fresh grace period rather than a verdict.
  // Five evaluator ticks: far beyond any scheduling jitter and far below the
  // shortest window it protects. A machine that stalls the evaluator forever
  // gets no failsafe rather than a wrong one, which is the correct way round.
  // Zero or less: every tick is a freeze, so every window is rebased on every
  // tick and the failsafe never decides.
  int64_t frozenMillis = 5000;
  // The network-change bar, ms: a freeze this long is also told to the SDK as a
  // network change (FreezeResponseFor, NetworkChangeNotify.h). A real sleep or
  // a modern-standby resume is long, minutes to hours, and the far end has
  // dropped the connections left across it; a load stall of a few seconds is
  // not a network change, and a kick for it would cancel every transport's dial
  // for nothing. Zero or less: every freeze is also a network change.
  int64_t freezeNetworkChangeMillis = 15000;
  // How long Stop() waits for each of the watchdog's threads before abandoning
  // it, ms. Same discipline, and the same reasoning, as kSdkTeardownBudget
  // (StopBudget.h): a healthy getExits() returns in microseconds, so this can
  // only expire on a wedge, and a stop that waits on a wedge is the failure this
  // whole area exists to remove. Both joins sit at the top of every stop, so the
  // selftest counts them against the service's stop wait hint. Zero or less: a
  // thread that has not already returned is abandoned at once.
  int64_t joinBudgetMillis = 250;
  // How often a bounded join looks again at the thread it waits for, ms: a
  // returning thread also wakes the wait, so this only bounds a missed wake, and
  // it is small against the budget. Zero or less: it looks again without
  // sleeping, a busy wait for the length of the budget.
  int64_t joinPollMillis = 10;
  // The verdict's thresholds.
  DeadTunnelSettings deadTunnel;
  // The capture transaction's timing, which the controller hands each session's
  // CaptureReadiness and the sampler's re-probe reads.
  CaptureReadinessSettings capture;
  // The sampler's network-change window: one kick per burst.
  NetworkNotifySettings networkNotify;
};

// The relationships the defaults are justified by. Over the default instance:
// a caller that moves a value takes on its own relationship.
static_assert(TunnelWatchdogSettings{}.frozenMillis ==
                  5 * TunnelWatchdogSettings{}.evaluateIntervalMillis,
              "the default freeze bar is five of the evaluator's ticks");
static_assert(TunnelWatchdogSettings{}.freezeNetworkChangeMillis >
                  TunnelWatchdogSettings{}.frozenMillis,
              "a freeze long enough to be a network change is a freeze");
static_assert(TunnelWatchdogSettings{}.frozenMillis <
                  TunnelWatchdogSettings{}.deadTunnel.deadFastMillis,
              "the freeze bar sits below the shortest window it protects");
static_assert(TunnelWatchdogSettings{}.deadTunnel.sdkUnresponsiveMillis >=
                  15 * TunnelWatchdogSettings{}.sampleIntervalMillis,
              "the wedge threshold is at least 15 consecutive missed samples");
static_assert(TunnelWatchdogSettings{}.deadTunnel.sdkUnresponsiveMillis >
                  TunnelWatchdogSettings{}.deadTunnel.sdkUnresponsiveArmMillis,
              "the unresponsive countdown is shown before it fires");
static_assert(TunnelWatchdogSettings{}.networkNotify.windowMillis <
                      TunnelWatchdogSettings{}.evaluateIntervalMillis &&
                  TunnelWatchdogSettings{}.evaluateIntervalMillis <
                      TunnelWatchdogSettings{}.deadTunnel.deadFastMillis,
              "window < evaluate interval < the shortest verdict window");

}  // namespace urnw
