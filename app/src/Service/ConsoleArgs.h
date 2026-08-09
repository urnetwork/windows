// The option parser for `urnetworkd console`, as a PURE FUNCTION over the
// argument vector.
//
// It lives here rather than inline in wmain for one reason: the flags it parses
// decide whether this process is allowed to rewrite the machine's routing table,
// and that decision has to be testable on a box where running the thing under
// test is not allowed. `urnetworkd selftest` calls ParseConsoleArgs directly and
// asserts the whole matrix — absent, valid, out of range, malformed, duplicated,
// and combined with --rpc-only — without starting anything. A parser reachable
// only from wmain can only be tested by running wmain, i.e. by starting a
// service, which is exactly the thing that must not happen to check a typo.
//
// Nothing in this header touches Windows, the filter engine, an adapter or a
// route. It is std::wstring in and a struct out.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <vector>

namespace urnw {

// --- the staged bring-up flag (--stop-after=<N>) ----------------------------
//
// TunnelController::StartLocked is an eight-step sequence and it logs its own
// progress as [n/8]. These are those n, and they are the flag's whole domain:
//
//   1/8 wintun adapter        (needs elevation; creates an interface)
//   2/8 sdk egress bind       (process-local)
//   3/8 network space
//   4/8 DeviceLocal
//   5/8 mTLS rpc listener
//   ---- THE FENCE: everything above is inert with respect to this machine ----
//   6/8 firewall policy + address/MTU/routes/DNS   <- THE FIRST DESTRUCTIVE ACT
//   7/8 split tunnel
//   8/8 packet pump
//
// The flag exists because there was no way to create the adapter (step 1) and
// stop before step 6. --rpc-only stops before 6 too, but it does so by SKIPPING
// step 1 entirely, so it can never answer "does the adapter come up cleanly, and
// does it go away again, on this machine?" — which is the question standing in
// front of the first live tunnel.
inline constexpr int kStopAfterMinStep = 1;
inline constexpr int kStopAfterMaxStep = 8;

// The last step an rpc-only session ever reaches. StartLocked returns at the
// fence in that mode, so 5 is a ceiling imposed by the mode itself and not by
// this flag. Named here so EffectiveStopStep below can state the relationship
// between the two rather than leave it implied.
inline constexpr int kRpcOnlyCeilingStep = 5;

// Force a step number into 1..8, IN THE SAFE DIRECTION.
//
// The parser already rejects anything out of range, so this is the second line
// of defence, for a caller that got there another way. It matters which way it
// rounds: the one unacceptable answer for a nonsense value is "run all eight
// steps", because that is a full tunnel bring-up on a machine whose operator
// asked for a partial one. So a value below the range clamps to the EARLIEST
// stop and a value above it to the last — never to "no stop at all".
constexpr int ClampStopAfterStep(int step) {
  if (step < kStopAfterMinStep) return kStopAfterMinStep;
  if (step > kStopAfterMaxStep) return kStopAfterMaxStep;
  return step;
}

// The last step the bring-up sequence will run, given the flag and the rpc-only
// clamp together.
//
// THE FLAG CAN ONLY EVER MOVE THIS EARLIER. rpc-only's ceiling is a property of
// the mode — StartLocked returns at the fence — so the two compose as a MINIMUM,
// never as a maximum. `--rpc-only --stop-after=7` is 5, not 7: the flag asked
// for more than the clamp allows and got the clamp. That is the invariant the
// selftest pins, because the failure it prevents is a debug flag that quietly
// re-enables the destructive half of a process launched specifically so that it
// could not reach it.
//
// stopAfterStep == 0 means the flag was not given: the ceiling is then the
// mode's alone. Note this answers "how far does the sequence go", NOT "does it
// tear down afterwards" — --stop-after=8 and no flag at all share a ceiling of
// 8 and differ in what happens next.
constexpr int EffectiveStopStep(int stopAfterStep, bool rpcOnly) {
  const int ceiling = rpcOnly ? kRpcOnlyCeilingStep : kStopAfterMaxStep;
  if (stopAfterStep <= 0) return ceiling;
  const int requested = ClampStopAfterStep(stopAfterStep);
  return requested < ceiling ? requested : ceiling;
}

// --- the parse result -------------------------------------------------------

struct ConsoleArgs {
  // false => REFUSE TO RUN. There is no partially-understood invocation: a
  // malformed --stop-after must not fall back to "no stop", because "no stop" is
  // a full tunnel bring-up and the operator typed the flag precisely to avoid
  // one. Every rejection path below sets this and fills in `error`.
  bool ok = true;
  // Wide, so it can name the offending argument without a narrowing conversion
  // (and so this header needs no Windows dependency to do it). Printed to stderr
  // by wmain exactly as the existing unknown-option message was.
  std::wstring error;

  bool rpc_only = false;
  // 0 = the flag was absent. Otherwise 1..8, already range-checked.
  int stop_after = 0;
};

// Parse everything AFTER the `console` verb.
//
// Strictness follows the existing --rpc-only rule, which was hardened for a
// reason worth repeating: `console --rpc-only --xyz` used to ignore --xyz
// silently while `console --xyz` errored, so a typo was swallowed exactly when
// the arguments most needed reading carefully. EVERY argument is checked here,
// and an unrecognised one is fatal.
//
// Repeated --rpc-only stays accepted, deliberately: it is accepted today and
// this change must not alter any invocation that works now. A repeated
// --stop-after is REJECTED, because unlike a boolean it carries a value, and
// `--stop-after=1 --stop-after=6` has two plausible readings, one of which
// rewrites the route table.
inline ConsoleArgs ParseConsoleArgs(const std::vector<std::wstring>& args) {
  const std::wstring kStopAfter = L"--stop-after";
  const std::wstring kStopAfterEq = L"--stop-after=";

  ConsoleArgs out;
  bool sawStopAfter = false;

  for (const std::wstring& arg : args) {
    if (arg == L"--rpc-only") {
      out.rpc_only = true;
      continue;
    }

    if (arg == kStopAfter) {
      out.ok = false;
      out.error = L"--stop-after needs a step number: --stop-after=<1..8>";
      return out;
    }

    if (arg.rfind(kStopAfterEq, 0) == 0) {
      if (sawStopAfter) {
        out.ok = false;
        out.error = L"--stop-after given more than once (" + arg +
                    L"); it carries a value, so a repeat is ambiguous rather "
                    L"than harmless — pass it exactly once";
        return out;
      }
      sawStopAfter = true;

      const std::wstring value = arg.substr(kStopAfterEq.size());
      if (value.empty()) {
        out.ok = false;
        out.error = L"--stop-after= has no value: --stop-after=<1..8>";
        return out;
      }
      // SHAPE FIRST, THEN MAGNITUDE. Checking the length before the digits made
      // "--stop-after=abc" report "out of range", which sends the operator
      // looking for a number they never typed. Whether it IS a number is the
      // first question; how big it is only matters once the answer is yes.
      for (const wchar_t c : value) {
        // ASCII digits only. No sign, no whitespace, no leading '+', no
        // full-width digits: every one of those is a mistype, and the only safe
        // answer to a mistyped stop point is to refuse to start.
        if (c < L'0' || c > L'9') {
          out.ok = false;
          out.error = L"--stop-after=" + value +
                      L" is not a number: N must be a plain decimal 1..8";
          return out;
        }
      }
      // No leading zero. "01" is not accepted as 1 for the same reason "007" is
      // not: this is an operator typing a step number by hand, and a parser that
      // quietly normalises one spelling has to normalise all of them or be
      // inconsistent about which typo it forgives. Refusing costs a retype;
      // forgiving selectively costs a rule nobody can state.
      if (value.size() > 1 && value[0] == L'0') {
        out.ok = false;
        out.error = L"--stop-after=" + value +
                    L" has a leading zero: N must be a plain 1..8";
        return out;
      }
      // Two digits is already past the top of the range, so anything longer is
      // rejected here rather than overflowed into range by the accumulate below.
      int n = 0;
      if (value.size() <= 2) {
        for (const wchar_t c : value) n = n * 10 + (c - L'0');
      }
      if (value.size() > 2 || n < kStopAfterMinStep || n > kStopAfterMaxStep) {
        out.ok = false;
        out.error = L"--stop-after=" + value + L" is out of range: the bring-up "
                    L"has 8 steps, so N must be 1..8";
        // The one value worth spelling out, because it is the one an operator
        // reaches for meaning the opposite of what it would do.
        if (value == L"0")
          out.error += L". 0 is not 'no stop' — omit the flag entirely for that";
        return out;
      }
      out.stop_after = n;
      continue;
    }

    out.ok = false;
    out.error = L"unknown option for console: " + arg;
    return out;
  }

  return out;
}

}  // namespace urnw
