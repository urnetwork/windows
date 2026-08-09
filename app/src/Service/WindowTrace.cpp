// SPDX-License-Identifier: MPL-2.0
#include "WindowTrace.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <vector>

#include "Log.h"
#include "Strings.h"

namespace urnw {
namespace {

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

std::string Trim(const std::string& s) {
  const size_t first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

// One exit's whole observable state, as a single comparable string.
//
// Compared rather than diffed field-by-field ON PURPOSE: a new field on
// urnet::Exit then shows up in the trace automatically the day the SDK is
// re-vendored, instead of being silently dropped by a hand-written comparator
// that nobody remembers to extend. This trace exists to answer a question we do
// not yet know the shape of.
std::string DescribeExit(const urnet::Exit& e) {
  return std::format(
      "tier={}/{} flows={} dial_failures={} proven={} warning={}{} "
      "quarantined={} done={} p2p_only={} probe_age_s={}",
      e.Tier, e.EffectiveTier, e.FlowCount, e.DialFailureCount,
      e.Proven ? "y" : "n", e.Warning ? "y" : "n",
      e.WarningCause.empty() ? "" : (" cause=" + e.WarningCause),
      e.Quarantined ? "y" : "n", e.Done ? "y" : "n", e.P2pOnly ? "y" : "n",
      e.ProbeAgeSeconds);
}

std::string ExitKey(const urnet::Exit& e) {
  return e.ClientId.value_or(std::string("<no-client-id>"));
}

// The metrics counters, as name=value pairs, so the diff below can report just
// the ones that MOVED. Listed explicitly rather than reflected because there is
// no reflection to use; the cost is that a new counter needs a line here, and
// the compiler will not remind anyone. The exit rows above are the part that
// must not need maintenance, and they do not.
std::vector<std::pair<const char*, int64_t>> MetricPairs(
    const urnet::ReliabilityMetrics& m) {
  return {
      {"flows_opened", m.FlowsOpened},
      {"exit_loss_events", m.ExitLossEvents},
      {"flows_lost_to_exit", m.FlowsLostToExit},
      {"recovery_count", m.RecoveryCount},
      {"recovery_missed", m.RecoveryMissed},
      {"recovery_pending", m.RecoveryPending},
      // The counter the stalled question is really about: a dial that failed
      // and was intercepted leaves no log line at v=0, but it does move this.
      {"dial_failures_intercepted", m.DialFailuresIntercepted},
      {"flows_reraced", m.FlowsReraced},
      {"flows_rebound", m.FlowsRebound},
      {"rebinds_accepted", m.RebindsAccepted},
      {"rebinds_redialed", m.RebindsRedialed},
      {"verdicts_held_uplink_stale", m.VerdictsHeldUplinkStale},
      {"verdicts_held_transport_down", m.VerdictsHeldTransportDown},
      // Candidate (b) from the prior diagnosis, as a counter: a window held
      // back by the shared-fate rule moves this rather than logging.
      {"verdicts_held_shared_fate", m.VerdictsHeldSharedFate},
      {"removals_deferred", m.RemovalsDeferred},
      {"probes_sent", m.ProbesSent},
      {"probes_answered", m.ProbesAnswered},
      {"providers_qualified", m.ProvidersQualified},
      {"busy_probes_sent", m.BusyProbesSent},
      {"busy_probes_acquitted", m.BusyProbesAcquitted},
      {"scheduler_pauses_detected", m.SchedulerPausesDetected},
      {"groups_followed", m.GroupsFollowed},
      {"groups_scattered", m.GroupsScattered},
  };
}

}  // namespace

WindowTraceConfig ParseWindowTrace(const std::string& raw) {
  WindowTraceConfig out;
  const std::string v = Lower(Trim(raw));
  if (v.empty() || v == "0" || v == "off" || v == "false" || v == "no") {
    return out;  // off, and that is not an error
  }
  if (v == "1" || v == "on" || v == "true" || v == "yes") {
    out.enabled = true;
    out.interval = std::chrono::milliseconds(kWindowTraceDefaultIntervalMs);
    return out;
  }
  // A number, or nothing. Shape before magnitude, for ParseConsoleArgs' reason:
  // reporting "out of range" for "fast" sends the operator looking for a number
  // they never typed.
  for (const char c : v) {
    if (c < '0' || c > '9') {
      out.error = std::format(
          "URNETWORK_SDK_TRACE={} is neither a switch nor a number. Use "
          "1/on/true/yes for the default {}ms period, or a plain interval in "
          "milliseconds ({}..{}).",
          raw, kWindowTraceDefaultIntervalMs, kWindowTraceMinIntervalMs,
          kWindowTraceMaxIntervalMs);
      return out;
    }
  }
  // Longer than five digits is already past the ceiling; accumulate only what
  // cannot overflow into range.
  long long n = -1;
  if (v.size() <= 5) {
    n = 0;
    for (const char c : v) n = n * 10 + (c - '0');
  }
  if (n < kWindowTraceMinIntervalMs || n > kWindowTraceMaxIntervalMs) {
    out.error = std::format(
        "URNETWORK_SDK_TRACE={} is out of range: the sampling period must be "
        "{}..{} ms. Below that this becomes a load generator on the very "
        "session it is measuring; above it, a window can form and collapse "
        "between two samples.",
        raw, kWindowTraceMinIntervalMs, kWindowTraceMaxIntervalMs);
    return out;
  }
  out.enabled = true;
  out.interval = std::chrono::milliseconds(static_cast<int>(n));
  return out;
}

WindowTraceConfig WindowTraceFromEnvironment() {
  constexpr DWORD kMax = 64;
  wchar_t buf[kMax] = {0};
  const DWORD n = ::GetEnvironmentVariableW(L"URNETWORK_SDK_TRACE", buf, kMax);
  // Unset, or longer than anything we accept. Treated as unset rather than
  // truncated: a truncated "1000000" is "10000", which would silently be a
  // different, valid setting.
  if (n == 0 || n >= kMax) return {};
  return ParseWindowTrace(Narrow(std::wstring(buf, n)));
}

WindowTrace::~WindowTrace() { Stop(); }

void WindowTrace::Start(urnet::DeviceLocal* device, const WindowTraceConfig& cfg) {
  if (!cfg.enabled || device == nullptr) return;
  Stop();
  {
    std::scoped_lock lock(mutex_);
    cancelled_ = false;
  }
  running_.store(true, std::memory_order_release);

  // LOUD, because it has a privacy cost and because a trace nobody knows is
  // running is a trace that ends up in a support bundle.
  LogWarn("trace: ======== SDK WINDOW TRACE IS ON (URNETWORK_SDK_TRACE) "
          "========");
  LogWarn("trace: sampling the exit window and the reliability counters every "
          "{}ms for the life of this session, and logging every CHANGE. THIS "
          "NAMES PROVIDER CLIENT IDS — every exit that enters the window, with "
          "its dial-failure count and warning cause — in this service log. Do "
          "not ship the log anywhere without reading it first. Unset "
          "URNETWORK_SDK_TRACE to turn it off.",
          static_cast<long long>(cfg.interval.count()));
  LogWarn("trace: this is NOT glog -v=2. The vendored SDK pins glog v to 0 in "
          "its own package init and exports no way to change it (no flag.Parse "
          "in the binary, no GLOG_ env support, no verbosity export), so V(2) "
          "needs an sdk.go change and an SDK rebuild. What this reads instead "
          "is the typed reliability surface, which carries the dial outcomes "
          "that cancellation was masking.");

  thread_ = std::thread([this, device, interval = cfg.interval] {
    Run(device, interval);
  });
}

void WindowTrace::Stop() {
  if (!thread_.joinable()) return;
  {
    std::scoped_lock lock(mutex_);
    cancelled_ = true;
  }
  wake_.notify_all();
  thread_.join();
  running_.store(false, std::memory_order_release);
  LogInfo("trace: window trace stopped");
}

void WindowTrace::Run(urnet::DeviceLocal* device,
                      std::chrono::milliseconds interval) {
  // THE DEADLINES, ONCE, AT THE TOP. FormationPollTimeoutMillis and
  // SharedFateWindowMillis are candidate (b) of the prior diagnosis — a window
  // viability deadline shorter than a cold platform dial. Read them off the
  // RUNNING device rather than quoting the defaults, so the log says what this
  // machine actually had rather than what the source says it should have.
  static std::atomic<bool> settingsLogged{false};
  if (auto settings = ReadSdkList(settingsLogged, "getReliabilitySettings",
                                  [&] { return device->getReliabilitySettings(); })) {
    const auto& s = *settings;
    LogWarn("trace: window deadlines in force: formation_poll_timeout={}ms "
            "shared_fate_window={}ms shared_fate_min_exits={} "
            "evaluation_pool_multiple={} max_flows_per_exit={} "
            "probe_timeout={}ms provider_probe={} standing_reserve={} "
            "effective_tier_selection={} heartbeat_interval={}ms",
            s.FormationPollTimeoutMillis, s.SharedFateWindowMillis,
            s.SharedFateMinExits, s.EvaluationPoolMultiple, s.MaxFlowsPerExit,
            s.ProbeTimeoutMillis, s.ProviderProbe ? "on" : "off",
            s.StandingReserve ? "on" : "off",
            s.EffectiveTierSelection ? "on" : "off", s.HeartbeatIntervalMillis);
  } else {
    LogWarn("trace: getReliabilitySettings() returned nothing — the device has "
            "no multi client to override yet, so the reliability stack is on "
            "its own defaults. The deadlines are NOT recorded for this run.");
  }

  const auto started = std::chrono::steady_clock::now();
  auto elapsedMs = [&started] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
  };

  std::map<std::string, std::string> lastExits;
  std::map<std::string, int64_t> lastMetrics;
  bool firstSample = true;

  // Failure is LATCHED, not per-tick: a device that cannot answer answers every
  // 100ms, and an unlatched warning would bury the trace it is meant to carry.
  static std::atomic<bool> exitsLogged{false};
  static std::atomic<bool> metricsLogged{false};

  for (;;) {
    {
      std::unique_lock lock(mutex_);
      if (wake_.wait_for(lock, interval, [this] { return cancelled_; })) break;
    }

    // ---- the window -------------------------------------------------------
    std::map<std::string, std::string> now;
    if (auto exits = ReadSdkList(exitsLogged, "getExits",
                                 [&] { return device->getExits(); })) {
      for (const auto& e : *exits) now.emplace(ExitKey(e), DescribeExit(e));
    }

    for (const auto& [id, desc] : now) {
      auto prev = lastExits.find(id);
      if (prev == lastExits.end()) {
        // The line whose TIMESTAMPS are the measurement: count these per second
        // to read the window's fill rate straight off the log.
        LogInfo("trace: [+{}ms] +exit {} ({}) window={}", elapsedMs(), id, desc,
                now.size());
      } else if (prev->second != desc) {
        LogInfo("trace: [+{}ms] ~exit {} ({})", elapsedMs(), id, desc);
      }
    }
    for (const auto& [id, desc] : lastExits) {
      if (!now.count(id))
        LogInfo("trace: [+{}ms] -exit {} (last: {}) window={}", elapsedMs(), id,
                desc, now.size());
    }
    if (firstSample || now.size() != lastExits.size()) {
      LogInfo("trace: [+{}ms] window size {} -> {}", elapsedMs(),
              lastExits.size(), now.size());
    }
    lastExits.swap(now);

    // ---- the counters -----------------------------------------------------
    if (auto metrics = ReadSdkList(metricsLogged, "getReliabilityMetrics", [&] {
          return device->getReliabilityMetrics();
        })) {
      std::string moved;
      for (const auto& [name, value] : MetricPairs(*metrics)) {
        auto prev = lastMetrics.find(name);
        const bool changed = prev == lastMetrics.end() ? value != 0
                                                       : prev->second != value;
        if (changed) {
          if (!moved.empty()) moved += " ";
          moved += std::format("{}={}", name, value);
        }
        lastMetrics[name] = value;
      }
      if (!moved.empty())
        LogInfo("trace: [+{}ms] metrics {}", elapsedMs(), moved);
    }

    firstSample = false;
  }

  // GLOG BUFFERS. The SDK's own INFO/ERROR files are written by glog, which
  // holds a buffer, and this repo never flushed it — so an elevated run that is
  // killed (which is how a wedged one ends) loses the tail of exactly the SDK
  // log the trace is there to be read alongside. Flush on the way out, where the
  // cost is one call at teardown.
  urnet::flushGlog();
  LogInfo("trace: [+{}ms] final window size {}; sdk glog flushed", elapsedMs(),
          lastExits.size());
}

}  // namespace urnw
