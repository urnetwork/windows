// Client side of the app<->service control pipe. The tray app uses this to
// drive the service: start/stop the tunnel, push split-tunnel config, and
// receive unsolicited state-change events.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace urnw {

class PipeClient {
 public:
  using EventHandler = std::function<void(const nlohmann::json&)>;
  using DisconnectHandler = std::function<void()>;

  PipeClient() = default;
  ~PipeClient();

  PipeClient(const PipeClient&) = delete;
  PipeClient& operator=(const PipeClient&) = delete;

  // Connect to the service pipe (waits briefly if the pipe is busy). Returns
  // false if the service is not running / not reachable.
  bool Connect();
  bool IsConnected() const { return connected_.load(); }
  void Close();

  // Unsolicited events (service -> app) are delivered on the reader thread.
  // Set before Connect().
  void SetEventHandler(EventHandler handler) { onEvent_ = std::move(handler); }

  // The service went away: the pipe broke while we were connected, and no
  // orderly Close() asked for it. Delivered ONCE per connection, on the reader
  // thread, as that thread exits.
  //
  // It exists because the service is what HOLDS the tunnel — its process dying
  // takes the wintun adapter and the dynamic WFP session with it — and until
  // this callback there was no signal at all that it had. The app kept whatever
  // it last cached and went on rendering a tunnel that no longer existed.
  //
  // A handler MUST NOT call Connect() or Close() on this object: both join the
  // reader thread, which is the thread the handler is running on.
  void SetDisconnectHandler(DisconnectHandler handler) {
    onDisconnect_ = std::move(handler);
  }

  // Send a request and block for its matching reply (by "type"). Throws
  // std::runtime_error on transport failure. Returns the reply JSON.
  nlohmann::json Call(const nlohmann::json& request, int timeoutMs = 30000);

 private:
  void ReaderLoop();
  bool WriteLine(const std::string& line);

  void* pipe_ = nullptr;  // HANDLE
  std::atomic<bool> connected_{false};
  std::atomic<bool> stopping_{false};
  std::thread reader_;
  EventHandler onEvent_;
  DisconnectHandler onDisconnect_;

  // pending reply plumbing: a single in-flight Call at a time (the control
  // channel is request/response; concurrent calls serialize on callMutex_)
  std::mutex callMutex_;
  std::mutex replyMutex_;
  std::condition_variable replyCv_;
  bool haveReply_ = false;
  nlohmann::json reply_;
  std::string awaitingType_;
};

}  // namespace urnw
