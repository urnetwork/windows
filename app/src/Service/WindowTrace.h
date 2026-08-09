// The window tracer: a high-frequency, change-triggered record of the SDK's exit
// window, written by urnetworkd into the service log while a session is live.
//
// ---------------------------------------------------------------------------
// WHY THIS EXISTS INSTEAD OF `-v=2`
//
// The previous diagnosis stalled on one question — why the first window's
// transports never came up inside their ~13 s life — and concluded it needed an
// elevated repro at glog `-v=2`, because cancellation masks the dial outcome and
// the real result is only logged behind V(2).
//
// THAT VERBOSITY IS NOT REACHABLE FROM THIS SIDE. Measured against the vendored
// artifact (app/third_party/urnetwork-sdk/amd64/URnetworkSdk.dll), not inferred:
//
//   1. sdk.go's package init() calls initGlog(), which ends with
//      flag.Set("v", "0"). That runs during Go runtime init at LoadLibrary time,
//      i.e. before any C++ in this process gets control, and it OVERWRITES the
//      flag rather than defaulting it.
//   2. `flag.Parse` is not linked into the DLL — the flag package's own error
//      strings ("flag provided but not defined", "flag needs an argument", "bad
//      flag syntax") are all absent from the binary, while flag.Lookup/Set/Var
//      are present. os.Args is therefore never consulted, so `-v=2` on
//      urnetworkd's command line is inert.
//   3. The glog fork (github.com/urnetwork/glog) has no environment support:
//      zero occurrences of the string "GLOG_" in the DLL. Its only exported
//      setters are SetLogDir and SetMaxLogSize — there is no SetVerbosity.
//   4. Of the 631 `urnet_*` exports, the only log-related ones are
//      urnet_flush_glog, urnet_get_log_dir, urnet_set_log_dir and
//      urnet_device_upload_logs. Nothing exposes flag.Set or a level.
//
// So raising V needs a one-line change in sdk.go plus an SDK rebuild, and this
// box cannot do that rebuild (the cgo build needs ../connect and ../glog, and
// neither sibling is checked out).
//
// ---------------------------------------------------------------------------
// WHAT THIS CAPTURES INSTEAD, AND WHY IT IS THE SAME EVIDENCE
//
// Everything the stalled question needs is already on the DeviceLocal's typed
// reliability surface, at v=0, with no rebuild:
//
//   window formation   urnet::Exit rows appearing and disappearing over time.
//                      Sampled at a fixed interval and logged on CHANGE, so the
//                      arrival TIMES are the record — which is a direct
//                      measurement of the hypothesis that NextConnectTime()
//                      serialises dials at ~1.2/s on a shared ClientStrategy
//                      against a staircase that sustains ~1.8/s. Count the
//                      +exit lines per second.
//   dial outcomes      Exit::DialFailureCount, Warning, WarningCause,
//                      Quarantined, Proven, FlowCount, Tier/EffectiveTier,
//                      ProbeAgeSeconds — per exit, per sample. This is the half
//                      cancellation was hiding: a cancelled dial leaves no log
//                      line, but it does leave a counter.
//   aggregate verdicts ReliabilityMetrics::DialFailuresIntercepted,
//                      VerdictsHeldSharedFate / TransportDown / UplinkStale,
//                      RemovalsDeferred, ProbesSent/Answered,
//                      ProvidersQualified, FlowsReraced/Rebound.
//   the deadline       ReliabilitySettings::FormationPollTimeoutMillis and
//                      SharedFateWindowMillis, dumped once at trace start —
//                      the two numbers (formationpolltimeout=200,
//                      sharedfatewindow=10000) the prior diagnosis named as
//                      candidate (b), read off the running device rather than
//                      assumed.
//
// What it does NOT capture, so nobody reads more into it than is there:
// NextConnectTime itself, the per-transport dial addresses, and anything inside
// github.com/urnetwork/connect that never reaches a typed getter. Those still
// need V(2) and therefore an SDK change.
//
// ---------------------------------------------------------------------------
// OFF UNLESS ASKED FOR, AND LOUD WHEN ON
//
// Driven by URNETWORK_SDK_TRACE, read once at session start. Absent or "0" and
// this class allocates nothing and starts no thread. When it IS on it says so at
// WARN with the privacy consequence spelled out, because the trace names
// PROVIDER CLIENT IDS — every exit in the window, for the whole session, in a
// file the owner may hand to someone else.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "Sdk.h"

namespace urnw {

// The parsed value of URNETWORK_SDK_TRACE.
//
// A PURE FUNCTION OVER THE STRING, for ConsoleArgs' reason: the selftest asserts
// the whole matrix without starting a device, a thread or a session.
struct WindowTraceConfig {
  bool enabled = false;
  // Sampling period. Bounded below because this calls into the SDK on every
  // tick and an operator typing a small number should not be able to turn a
  // diagnostic into a load generator, and bounded above because a period longer
  // than the window's own life would miss the thing being measured.
  std::chrono::milliseconds interval{100};
  // Non-empty when the value was not understood. The trace is then OFF and this
  // is logged — a malformed diagnostic flag must not silently become an enabled
  // one, and equally must not stop the service starting.
  std::string error;
};

inline constexpr int kWindowTraceMinIntervalMs = 20;
inline constexpr int kWindowTraceMaxIntervalMs = 10000;
inline constexpr int kWindowTraceDefaultIntervalMs = 100;

// "" / "0" / "off" / "false" / "no"      -> off, no error
// "1" / "on" / "true" / "yes"            -> on at the default interval
// a plain decimal in [20, 10000]         -> on at that many milliseconds
// anything else                          -> off, with an error
//
// Case-insensitive; surrounding whitespace ignored. Deliberately the SAME
// allow-list shape URNETWORK_RPC_ONLY uses (SdkHost.h), so an operator who knows
// one knows the other.
WindowTraceConfig ParseWindowTrace(const std::string& value);

// Read URNETWORK_SDK_TRACE from the environment and parse it.
WindowTraceConfig WindowTraceFromEnvironment();

class WindowTrace {
 public:
  WindowTrace() = default;
  ~WindowTrace();
  WindowTrace(const WindowTrace&) = delete;
  WindowTrace& operator=(const WindowTrace&) = delete;

  // Begin tracing `device`. No-op when cfg.enabled is false.
  //
  // THE CALLER OWNS `device` AND MUST OUTLIVE THE TRACE. This holds a raw
  // pointer for the life of the thread, so Stop() has to run BEFORE the device
  // is closed or destroyed — TunnelController::StopLocked does it at the top,
  // ahead of the moves. A trace still running during device->close() would be
  // calling into an object being torn down.
  void Start(urnet::DeviceLocal* device, const WindowTraceConfig& cfg);

  // Stop and JOIN. Idempotent, and safe to call when nothing was started.
  void Stop();

  bool Running() const { return running_.load(std::memory_order_acquire); }

 private:
  void Run(urnet::DeviceLocal* device, std::chrono::milliseconds interval);

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool cancelled_ = false;
  std::atomic<bool> running_{false};
};

}  // namespace urnw
