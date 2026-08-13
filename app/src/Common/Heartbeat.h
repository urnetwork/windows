// THE HEARTBEAT — one bit that settles "when did it die", and half of the bit
// that settles "how".
//
// WHY THIS EXISTS. urnetworkd has vanished four times with a live tunnel and
// left a log whose last line is whatever happened to be logged before the
// death. Those lines are event-driven, so the gap between the last one and the
// death has been as much as 28 seconds — 28 seconds of SDK activity to correlate
// against, for a process that gives no other signal. A record rewritten once a
// second closes that to about one second, which is the difference between "it
// died somewhere in this half-minute" and "it died in the second after this
// packet".
//
// WHAT IT WRITES. A single FIXED-WIDTH record, overwritten in place at offset 0
// every tick. Not appended: an append would grow without bound for a service
// that is meant to run for weeks, and nothing older than the last tick has any
// value — the file's entire job is to answer "what was the last instant this
// process was alive, and what did it think it was doing". Fixed width is what
// makes the overwrite safe: a shorter record can never leave the tail of a
// longer one behind, so the file never reads as a mixture of two ticks.
//
// The record opens with a UTF-8 BOM because Windows PowerShell 5.1 — what the
// owner has — reads a BOM-less file in the ANSI code page. Same reasoning as
// Log.cpp's BOM, arrived at the same way.
//
// WHAT IT MUST NEVER DO. Wedge shutdown. It runs on its own detached thread,
// waits on an event nothing else owns, holds no lock any other path takes, and
// is never joined. The stop signal wakes it immediately; if it ever did block,
// the process would leave without it rather than waiting.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace urnw {

// ---- the lock-free tunnel-state mirror -------------------------------------
//
// The heartbeat CANNOT ask TunnelController for its state. Status() takes
// mutex_, and this class's own header already establishes that a connect
// attempt wedged inside the SDK holds mutex_ forever — which is precisely the
// condition a heartbeat most needs to be able to record. A heartbeat that can
// block on the wedge it exists to timestamp is worse than none.
//
// So every write to TunnelController::state_ publishes here instead, as a bare
// const char* into the string literals ToString(TunnelState) returns. Storing a
// pointer to static storage means the read side needs no allocation, no lock
// and no lifetime rule.
void PublishTunnelState(const char* state);
const char* PublishedTunnelState();

// ---- the record ------------------------------------------------------------

// Every record is EXACTLY this many bytes, BOM and trailing CRLF included.
inline constexpr size_t kHeartbeatRecordBytes = 256;

// Pure, so the width and content rules can be proved by `urnetworkd selftest`
// on a machine where the death itself cannot be reproduced. `stamp` is the
// caller's already-formatted wall clock, so the formatter has no clock in it.
// Over-long input is truncated rather than allowed to change the width.
std::string FormatHeartbeatRecord(unsigned long pid, long long uptimeSeconds,
                                  const char* tunnelState,
                                  std::string_view stamp);

// "01:02:03" from 3723. Hours are not wrapped at 24 — a service that has been
// up for three days should say 72:xx:xx, not look like it restarted.
std::string FormatUptime(long long seconds);

// ---- the file --------------------------------------------------------------

class HeartbeatFile {
 public:
  HeartbeatFile() = default;
  ~HeartbeatFile();

  HeartbeatFile(const HeartbeatFile&) = delete;
  HeartbeatFile& operator=(const HeartbeatFile&) = delete;

  // Create/open `file` for in-place rewriting, shared for read, write and
  // delete so the owner can read — or delete — a live service's heartbeat
  // without stopping anything. False leaves LastError() set.
  bool Open(const std::filesystem::path& file);
  // One record at offset 0. Silent on failure by design: a heartbeat that
  // logged its own write errors would, on a full or unwritable disk, produce a
  // line per second in the log it is supposed to be helping someone read.
  void Beat(long long uptimeSeconds, const char* tunnelState);
  void Close();

  const std::filesystem::path& Path() const { return path_; }
  const std::string& LastError() const { return error_; }

 private:
  void* handle_ = nullptr;  // HANDLE; null when closed
  std::filesystem::path path_;
  std::string error_;
};

// ---- the thread ------------------------------------------------------------

// Extra work to do on each tick, on the heartbeat thread. The service passes a
// glog flush; nothing else may go here that can block, because everything on
// this thread runs between one heartbeat and the next.
using HeartbeatTick = void (*)();

// Start the ~1 Hz heartbeat writing to `file`, on a detached thread that owns
// everything it touches. Call once. `tick` may be null.
//
// Detached and never joined ON PURPOSE — see the header note. StopHeartbeat()
// signals it; it writes one final record and returns, normally within
// microseconds, and the process does not wait to find out.
void StartHeartbeat(const std::filesystem::path& file, HeartbeatTick tick);

// Ask the heartbeat to stop. Idempotent, safe before StartHeartbeat, and safe
// from a console control handler.
void StopHeartbeat();

// How long this process has been running, from the first call to StartHeartbeat
// or the first call to this — whichever came first. Reported by the heartbeat
// record and by the atexit line.
std::chrono::seconds ProcessUptime();

}  // namespace urnw
