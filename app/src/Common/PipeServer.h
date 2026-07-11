// Server side of the control pipe, hosted by the service. Creates the named
// pipe with a restrictive security descriptor, accepts one client connection at
// a time (the tray app), dispatches JSON requests to a handler, and can push
// unsolicited events (tunnel state changes) to the connected client.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace urnw {

class PipeServer {
 public:
  // Handles a request JSON, returns the reply JSON. Runs on the connection
  // thread. Must not block indefinitely.
  using RequestHandler = std::function<nlohmann::json(const nlohmann::json&)>;

  PipeServer() = default;
  ~PipeServer();

  PipeServer(const PipeServer&) = delete;
  PipeServer& operator=(const PipeServer&) = delete;

  // Start accepting connections. handler is invoked per request.
  bool Start(RequestHandler handler);
  void Stop();

  // Push an unsolicited event to the currently connected client (no-op if none).
  void PushEvent(const nlohmann::json& event);

 private:
  void AcceptLoop();
  void ServeConnection(void* pipe);

  std::atomic<bool> stopping_{false};
  std::thread acceptThread_;
  RequestHandler handler_;

  std::mutex writeMutex_;
  void* activePipe_ = nullptr;  // HANDLE of the connected client, or null
  void* stopEvent_ = nullptr;   // manual-reset event to break the accept wait
};

}  // namespace urnw
