// Executable spec for "send feedback with logs" whether or not a tunnel runs
// (support inbox 2090): which device in the service carries the upload
// (Common/LogUpload.h), when the service builds no standalone device for it,
// how long that one may wait for its upload, the upload in flight that runs
// off the session lock and one at a time (logupload::Flight), what the app
// does with the service's answer and how it learns the outcome, the app's
// request thread and its exit rule (App/FeedbackLogUpload.h), and the
// upload_logs request, reply and status fields on the control pipe
// (Common/Protocol.h), the feedback id check among them. Run against the same
// headers the service and the app compile; it needs nlohmann/json, like them.
//
// Threads are held by barriers, never by the clock, except the one budget the
// requester's destruction is bounded by, which is what that check is about. A
// stand-in call that runs on the caller's own thread returns at once and fails
// the check that names it, rather than hanging the run.
//
//   c++ -std=c++20 -I ../src/Common -I ../src/App -I <dir with nlohmann/json.hpp> log-upload-tests.cpp -o /tmp/log-upload-tests && /tmp/log-upload-tests
//
// SPDX-License-Identifier: MPL-2.0

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AppLogFiles.h"
#include "FeedbackLogUpload.h"
#include "LogUpload.h"
#include "Protocol.h"

using namespace urnw;

namespace {

int gFailures = 0;
int gCases = 0;

void Check(bool condition, const std::string& what) {
  ++gCases;
  if (!condition) {
    ++gFailures;
    std::cout << "  FAIL " << what << "\n";
  }
}

// Notes that a call has returned, for a helper thread that releases a held
// step once it has, or after a while when it waits for the step, as it must.
// Either way the order of the two returns is then decided, and read off a
// shared counter.
class ReturnNote {
 public:
  void Note() {
    {
      std::scoped_lock lock(mutex_);
      noted_ = true;
    }
    wake_.notify_all();
  }

  void WaitOrAfter(std::chrono::milliseconds budget) {
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, budget, [this] { return noted_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  bool noted_ = false;
};

// synthetic values only, shaped like the real ones
proto::UploadLogs SampleRequest() {
  proto::UploadLogs r;
  r.feedback_id = "f00dfeed-0000-4000-8000-000000000001";
  r.by_jwt = "client.jwt.value";
  r.network_space_json = R"({"key":{"host_name":"network.example","env_name":"test"}})";
  r.instance_id = "0000feed-0000-4000-8000-0000000000aa";
  r.device_description = "DESKTOP-1";
  r.device_spec = "windows amd64";
  r.app_version = "2026.10.5-1";
  return r;
}

// (a) the device that runs carries it, the session's first, and a standalone
// device only when neither runs
void TestCarrier() {
  Check(logupload::CarrierFor(true, false) == logupload::Carrier::Tunnel,
        "carrier: the session's device");
  Check(logupload::CarrierFor(false, true) == logupload::Carrier::Provider,
        "carrier: the provider-only device while there is no session");
  Check(logupload::CarrierFor(false, false) == logupload::Carrier::Standalone,
        "carrier: a standalone device only when neither runs");
  Check(logupload::CarrierFor(true, true) == logupload::Carrier::Tunnel,
        "carrier: the session's device wins should both ever run");
  Check(std::string(logupload::ToString(logupload::Carrier::Tunnel)) == "tunnel" &&
            std::string(logupload::ToString(logupload::Carrier::Provider)) == "provider" &&
            std::string(logupload::ToString(logupload::Carrier::Standalone)) == "standalone",
        "carrier: the reply's words");
}

// (b) the standalone device's refusals: a held device or a pending restart,
// and nothing about the kill switch, whose armed floor permits this service
void TestStandaloneRefusals() {
  Check(logupload::StandaloneRefusalFor(false, false) == logupload::StandaloneRefusal::None,
        "refusal: none by default");
  Check(logupload::StandaloneRefusalFor(true, false) ==
            logupload::StandaloneRefusal::DeviceStillHeld,
        "refusal: a teardown still holds this identity");
  Check(logupload::StandaloneRefusalFor(false, true) ==
            logupload::StandaloneRefusal::RestartPending,
        "refusal: the service is restarting itself");
  Check(logupload::StandaloneRefusalFor(true, true) ==
            logupload::StandaloneRefusal::DeviceStillHeld,
        "refusal: the held device is named first");
  Check(std::string(logupload::RefusalReason(logupload::StandaloneRefusal::None)).empty(),
        "refusal: no reason for none");
  for (const logupload::StandaloneRefusal refusal :
       {logupload::StandaloneRefusal::DeviceStillHeld,
        logupload::StandaloneRefusal::RestartPending}) {
    Check(!std::string(logupload::RefusalReason(refusal)).empty(),
          "refusal: every refusal has a reason for the reply");
  }
}

// (c) how long a standalone device may wait for its upload
void TestStandaloneLifetime() {
  Check(logupload::kStandaloneDeviceMaxLifetime == std::chrono::minutes(30),
        "lifetime: 30 minutes without a report");
}

// (d) the app falls back only when the service did not take it; a busy
// service ends it too, since the server would refuse a second upload
void TestAppStep() {
  using logupload::AppStep;
  using logupload::ServiceAnswer;
  Check(logupload::AppStepAfterService(ServiceAnswer::Accepted, false) == AppStep::Done,
        "app: the service took it");
  Check(logupload::AppStepAfterService(ServiceAnswer::Accepted, true) == AppStep::Done,
        "app: no second upload through the DeviceRemote beside the service's");
  Check(logupload::AppStepAfterService(ServiceAnswer::Busy, true) == AppStep::Done,
        "app: no second upload beside the one in flight");
  Check(logupload::AppStepAfterService(ServiceAnswer::NotTaken, true) == AppStep::DeviceRemote,
        "app: the old path while a session is bound");
  Check(logupload::AppStepAfterService(ServiceAnswer::NotTaken, false) == AppStep::Skip,
        "app: nothing can carry it");
}

// (e) the request survives the wire, and an absent field is empty, not a throw
void TestUploadLogsJson() {
  Check(std::string(proto::msg::kUploadLogs) == "upload_logs", "wire: the verb");
  const proto::UploadLogs sent = SampleRequest();
  const nlohmann::json request = proto::Request(proto::msg::kUploadLogs, sent);
  Check(proto::TypeOf(request) == "upload_logs", "wire: the request carries its type");
  const nlohmann::json wire = nlohmann::json::parse(proto::DumpForWire(request));
  const proto::UploadLogs got = wire.get<proto::UploadLogs>();
  Check(got.feedback_id == sent.feedback_id, "wire: feedback_id");
  Check(got.by_jwt == sent.by_jwt, "wire: by_jwt");
  Check(got.network_space_json == sent.network_space_json, "wire: network_space_json");
  Check(got.instance_id == sent.instance_id, "wire: instance_id");
  Check(got.device_description == sent.device_description, "wire: device_description");
  Check(got.device_spec == sent.device_spec, "wire: device_spec");
  Check(got.app_version == sent.app_version, "wire: app_version");
  const proto::UploadLogs sparse =
      nlohmann::json::parse(R"({"feedback_id":null})").get<proto::UploadLogs>();
  Check(sparse.feedback_id.empty() && sparse.by_jwt.empty(),
        "wire: absent and null fields read as empty");
}

// (f) the reply names the device and the upload, only when it answers one,
// or says one is in flight; a service that predates the verb answers "unknown
// request type" and names none
void TestReply() {
  proto::Reply reply;
  reply.ok = true;
  reply.in_reply_to = proto::msg::kUploadLogs;
  reply.log_upload_carrier = "standalone";
  reply.log_upload_id = 1759700000123;
  const proto::Reply back =
      nlohmann::json::parse(proto::DumpForWire(nlohmann::json(reply))).get<proto::Reply>();
  Check(back.ok && back.log_upload_carrier == "standalone", "reply: the carrier comes back");
  Check(back.log_upload_id == 1759700000123, "reply: the upload's id comes back");
  Check(!back.log_upload_busy, "reply: an admitted upload is not busy");
  proto::Reply busy;
  busy.in_reply_to = proto::msg::kUploadLogs;
  busy.log_upload_busy = true;
  const proto::Reply busyBack =
      nlohmann::json::parse(proto::DumpForWire(nlohmann::json(busy))).get<proto::Reply>();
  Check(!busyBack.ok && busyBack.log_upload_busy, "reply: busy comes back");
  proto::Reply other;
  other.ok = true;
  Check(!nlohmann::json(other).contains("log_upload_carrier") &&
            !nlohmann::json(other).contains("log_upload_id") &&
            !nlohmann::json(other).contains("log_upload_busy"),
        "reply: no upload fields in any other reply");
  const proto::Reply older =
      nlohmann::json::parse(
          R"({"type":"reply","ok":false,"error":"unknown request type: upload_logs"})")
          .get<proto::Reply>();
  Check(!older.ok && older.log_upload_carrier.empty() && older.log_upload_id == 0 &&
            !older.log_upload_busy,
        "reply: an older service's answer is a refusal with no carrier, id or busy");
}

// (g) the status carries the upload, so its outcome reaches the app; an older
// service's status names none
void TestStatusCarriesTheUpload() {
  proto::TunnelStatus status;
  status.log_upload_id = 1759700000123;
  status.log_upload_state = "refused";
  status.log_upload_carrier = "provider";
  const proto::TunnelStatus back =
      nlohmann::json::parse(nlohmann::json(status).dump()).get<proto::TunnelStatus>();
  Check(back.log_upload_id == 1759700000123 && back.log_upload_state == "refused" &&
            back.log_upload_carrier == "provider",
        "status: the upload comes back");
  nlohmann::json older = nlohmann::json(status);
  older.erase("log_upload_id");
  older.erase("log_upload_state");
  older.erase("log_upload_carrier");
  const proto::TunnelStatus olderBack = older.get<proto::TunnelStatus>();
  Check(olderBack.log_upload_id == 0 && olderBack.log_upload_state.empty(),
        "status: an older service names no upload");
}

// (h) status' words for where the upload is, and back
void TestFlightStateNames() {
  using logupload::FlightState;
  for (const FlightState state : {FlightState::None, FlightState::Running, FlightState::Uploaded,
                                  FlightState::Refused, FlightState::Failed}) {
    Check(logupload::FlightStateFromString(logupload::ToString(state)) == state,
          std::string("flight state: \"") + logupload::ToString(state) + "\" round trips");
  }
  Check(std::string(logupload::ToString(FlightState::None)).empty(), "flight state: none is empty");
  Check(logupload::FlightStateFromString("a state from a newer service") == FlightState::None,
        "flight state: an unknown word is none");
  Check(!logupload::IsFinished(FlightState::Running) && logupload::IsFinished(FlightState::Failed),
        "flight state: running is not finished, failed is");
  Check(logupload::kSilentUploadMaxMillis == 30LL * 60 * 1000,
        "flight: a silent upload is given up on after the standalone device's 30 minutes");
}

// (i) the upload runs on a thread of its own: Run returns while the sdk's call
// (held by a barrier) is still zipping, and the flight answers another request
// meanwhile, so the session lock and the control pipe that run both are never
// held by a slow upload. Then the call's outcome is what status reads, and the
// finish hook (the service's status push) has run.
void TestFlightRunsTheUploadOffTheCallersThread() {
  auto flight = std::make_shared<logupload::Flight>(100);
  std::atomic<int> pushes{0};
  flight->SetOnFinished([&pushes] { pushes += 1; });
  const int64_t uploadId = flight->Begin(logupload::Carrier::Tunnel, 1000);
  Check(uploadId == 100, "flight: the first upload is numbered from the first id");

  const std::thread::id caller = std::this_thread::get_id();
  std::atomic<bool> ranOnCaller{false};
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  flight->Run(uploadId, /*deviceHandle=*/7,
              [&ranOnCaller, &entered, released, flight, uploadId, caller] {
                if (std::this_thread::get_id() == caller) {
                  ranOnCaller = true;
                  return;
                }
                entered.set_value();
                released.wait();
                flight->Finish(uploadId, logupload::FlightState::Uploaded);
              });
  Check(!ranOnCaller.load(), "flight: the upload does not run on the caller's thread");
  if (ranOnCaller.load()) return;
  entered.get_future().wait();
  Check(flight->Read(1001).state == logupload::FlightState::Running,
        "flight: status is answered while the call is held");
  Check(flight->Begin(logupload::Carrier::Tunnel, 1001) == 0,
        "flight: a second request is answered, busy, while the call is held");
  release.set_value();
  Check(flight->WaitReturned(std::chrono::seconds(60)), "flight: the thread returns");
  const logupload::Flight::Reading done = flight->Read(1002);
  Check(done.id == 100 && done.state == logupload::FlightState::Uploaded &&
            done.carrier == logupload::Carrier::Tunnel,
        "flight: status reads the call's outcome");
  Check(pushes.load() == 1, "flight: the finish hook ran once");
}

// (j) one upload at a time, and until its thread is out of the sdk's call (an
// outcome can come before the call returns); then the next, numbered after it
void TestFlightAdmitsOneUploadAtATime() {
  auto flight = std::make_shared<logupload::Flight>(1);
  const int64_t first = flight->Begin(logupload::Carrier::Standalone, 0);
  Check(first == 1, "one at a time: the first is admitted");
  Check(flight->Begin(logupload::Carrier::Tunnel, 1) == 0, "one at a time: busy while running");

  const std::thread::id caller = std::this_thread::get_id();
  std::atomic<bool> ranOnCaller{false};
  std::promise<void> reported;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  flight->Run(first, /*deviceHandle=*/9,
              [&ranOnCaller, &reported, released, flight, first, caller] {
                if (std::this_thread::get_id() == caller) {
                  ranOnCaller = true;
                  return;
                }
                flight->Finish(first, logupload::FlightState::Refused);
                reported.set_value();
                released.wait();
              });
  Check(!ranOnCaller.load(), "one at a time: the upload does not run on the caller's thread");
  if (ranOnCaller.load()) return;
  reported.get_future().wait();
  Check(flight->Read(2).state == logupload::FlightState::Refused,
        "one at a time: the outcome is read before the call returns");
  Check(flight->Begin(logupload::Carrier::Tunnel, 2) == 0,
        "one at a time: busy while its thread is still in the call");
  release.set_value();
  Check(flight->WaitReturned(std::chrono::seconds(60)), "one at a time: the thread returns");
  Check(flight->Begin(logupload::Carrier::Tunnel, 3) == 2,
        "one at a time: the next is admitted, numbered after it");
}

// (k) the call's device stays alive until the call returns: a teardown that
// releases it meanwhile hands it to the flight, which lets go of it on the
// upload's thread after the call; any other device is handed back at once
void TestFlightKeepsTheCallsDeviceUntilTheCallReturns() {
  auto flight = std::make_shared<logupload::Flight>(1);
  const int64_t uploadId = flight->Begin(logupload::Carrier::Provider, 0);
  const std::thread::id caller = std::this_thread::get_id();
  std::atomic<bool> ranOnCaller{false};
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  flight->Run(uploadId, /*deviceHandle=*/7, [&ranOnCaller, &entered, released, caller] {
    if (std::this_thread::get_id() == caller) {
      ranOnCaller = true;
      return;
    }
    entered.set_value();
    released.wait();
  });
  Check(!ranOnCaller.load(), "kept device: the upload does not run on the caller's thread");
  if (ranOnCaller.load()) return;
  entered.get_future().wait();

  auto device = std::make_shared<std::string>("the call's device");
  std::weak_ptr<std::string> callsDevice = device;
  Check(flight->KeepUntilReturned(7, std::move(device)) == nullptr,
        "kept device: the call's device is kept");
  Check(!callsDevice.expired(), "kept device: alive while the call runs");
  auto otherDevice = std::make_shared<std::string>("another device");
  Check(flight->KeepUntilReturned(8, otherDevice) == otherDevice,
        "kept device: another device is handed back");
  release.set_value();
  Check(flight->WaitReturned(std::chrono::seconds(60)), "kept device: the thread returns");
  Check(callsDevice.expired(), "kept device: released once the call returned");
  auto later = std::make_shared<std::string>("the call's device, torn down later");
  Check(flight->KeepUntilReturned(7, later) == later,
        "kept device: out of the call, the device is the teardown's to release");
}

// (l) one outcome per upload, the first, and one push for it; an upload that
// never reports is given up on at the bound, so a post that hangs cannot keep
// every later upload out
void TestFlightKeepsTheFirstOutcomeAndGivesUpOnASilentUpload() {
  auto flight = std::make_shared<logupload::Flight>(40);
  std::atomic<int> pushes{0};
  flight->SetOnFinished([&pushes] { pushes += 1; });
  Check(flight->Read(0).id == 0 && flight->Read(0).state == logupload::FlightState::None,
        "outcome: nothing before the first upload");
  const int64_t first = flight->Begin(logupload::Carrier::Standalone, 0);
  flight->Finish(first + 1, logupload::FlightState::Uploaded);
  Check(flight->Read(1).state == logupload::FlightState::Running,
        "outcome: another upload's outcome is not this one's");
  flight->Finish(first, logupload::FlightState::Running);
  Check(flight->Read(1).state == logupload::FlightState::Running, "outcome: running is not one");
  flight->Finish(first, logupload::FlightState::Uploaded);
  flight->Finish(first, logupload::FlightState::Failed);
  Check(flight->Read(2).state == logupload::FlightState::Uploaded, "outcome: the first is kept");
  Check(pushes.load() == 1, "outcome: one push for it");

  const int64_t silent = flight->Begin(logupload::Carrier::Standalone, 10);
  Check(silent == first + 1, "outcome: numbered after the last");
  Check(flight->Read(10 + logupload::kSilentUploadMaxMillis - 1).state ==
            logupload::FlightState::Running,
        "outcome: a silent upload runs until the bound");
  Check(flight->Read(10 + logupload::kSilentUploadMaxMillis).state ==
            logupload::FlightState::Failed,
        "outcome: a silent upload is failed at the bound");
  flight->Finish(silent, logupload::FlightState::Uploaded);
  Check(flight->Read(10 + logupload::kSilentUploadMaxMillis).state ==
            logupload::FlightState::Failed,
        "outcome: a callback arriving after changes nothing");
  Check(flight->Begin(logupload::Carrier::Tunnel, 10 + logupload::kSilentUploadMaxMillis) ==
            silent + 1,
        "outcome: the next upload is admitted");
}

// (m) the finish hook reaches into the service, so it is cleared before the
// service goes: ClearOnFinished waits out a hook that is running, and none
// runs after it. The hook is held, and released once the clear has returned
// or, when the clear waits for it as it must, after a while.
void TestFlightClearWaitsOutARunningHook() {
  auto flight = std::make_shared<logupload::Flight>(1);
  std::atomic<int> clock{0};
  std::atomic<int> hookReturnedAt{0};
  std::atomic<int> hookRuns{0};
  std::promise<void> hookEntered;
  std::promise<void> hookRelease;
  std::shared_future<void> hookReleased = hookRelease.get_future().share();
  flight->SetOnFinished([&clock, &hookReturnedAt, &hookRuns, &hookEntered, hookReleased] {
    if (hookRuns.fetch_add(1) != 0) return;
    hookEntered.set_value();
    hookReleased.wait();
    hookReturnedAt = ++clock;
  });
  const int64_t uploadId = flight->Begin(logupload::Carrier::Tunnel, 0);
  std::thread finisher([flight, uploadId] {
    flight->Finish(uploadId, logupload::FlightState::Uploaded);
  });
  hookEntered.get_future().wait();
  ReturnNote cleared;
  std::thread release([&cleared, &hookRelease] {
    cleared.WaitOrAfter(std::chrono::seconds(2));
    hookRelease.set_value();
  });
  flight->ClearOnFinished();
  const int clearReturnedAt = ++clock;
  cleared.Note();
  release.join();
  finisher.join();
  Check(hookReturnedAt.load() != 0 && hookReturnedAt.load() < clearReturnedAt,
        "finish hook: clearing waits out a running hook");
  const int64_t later = flight->Begin(logupload::Carrier::Tunnel, 1);
  flight->Finish(later, logupload::FlightState::Uploaded);
  Check(hookRuns.load() == 1, "finish hook: none runs once cleared");
}

// (n) the app learns the outcome of the upload it waits on, by the id the reply
// named, from the service's status: only once it is finished, and never
// another upload's
void TestCompletionReachesTheApp() {
  using logupload::FlightState;
  Check(!logupload::CompletionFor(0, 0, FlightState::None).has_value(),
        "completion: nothing pending, nothing to learn");
  Check(!logupload::CompletionFor(0, 5, FlightState::Uploaded).has_value(),
        "completion: an upload the app does not wait on");
  Check(!logupload::CompletionFor(5, 5, FlightState::Running).has_value(),
        "completion: not before it is finished");
  Check(!logupload::CompletionFor(5, 6, FlightState::Uploaded).has_value(),
        "completion: never another upload's");
  for (const FlightState state :
       {FlightState::Uploaded, FlightState::Refused, FlightState::Failed}) {
    const auto outcome = logupload::CompletionFor(5, 5, state);
    Check(outcome.has_value() && *outcome == state,
          std::string("completion: the outcome \"") + logupload::ToString(state) + "\"");
  }
}

// (o) the app's request runs on its own thread: Send returns while the ask is
// held; the old path runs only when the service did not take it and a
// DeviceRemote is bound; and one request at a time
void TestFeedbackLogUploadAsksOffTheUiThread() {
  struct Case {
    logupload::ServiceAnswer answer;
    bool deviceRemoteBound;
    bool wantOldPath;
  };
  for (const Case c : {Case{logupload::ServiceAnswer::Accepted, true, false},
                       Case{logupload::ServiceAnswer::Busy, true, false},
                       Case{logupload::ServiceAnswer::NotTaken, true, true},
                       Case{logupload::ServiceAnswer::NotTaken, false, false}}) {
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> askedOnCaller{false};
    std::atomic<int> oldPathCalls{0};
    std::promise<void> askEntered;
    std::promise<void> askRelease;
    std::shared_future<void> askReleased = askRelease.get_future().share();
    FeedbackLogUpload upload(
        [&askedOnCaller, &askEntered, askReleased, caller, c](const std::string&) {
          if (std::this_thread::get_id() == caller) {
            askedOnCaller = true;
            return c.answer;
          }
          askEntered.set_value();
          askReleased.wait();
          return c.answer;
        },
        [&oldPathCalls, c](const std::string&) -> std::function<void()> {
          if (!c.deviceRemoteBound) return {};
          return [&oldPathCalls] { oldPathCalls += 1; };
        });
    Check(upload.Send("f00dfeed-0000-4000-8000-000000000001"), "request: sent");
    Check(!askedOnCaller.load(), "request: the ask does not run on the caller's thread");
    if (askedOnCaller.load()) return;
    askEntered.get_future().wait();
    Check(!upload.Send("f00dfeed-0000-4000-8000-000000000002"),
          "request: one at a time, a second is refused while the first runs");
    askRelease.set_value();
    Check(upload.WaitIdle(std::chrono::seconds(60)), "request: it ends");
    Check((oldPathCalls.load() == 1) == c.wantOldPath,
          std::string("request: the old path ") + (c.wantOldPath ? "runs" : "does not run") +
              " for this answer");
  }
}

// (p) Cancel waits out a step that uses the host (here the ask, held and
// released once Cancel has returned or, when it waits for the ask as it must,
// after a while), and no step runs after it.
void TestFeedbackLogUploadCancelWaitsOutAHostStep() {
  const std::thread::id caller = std::this_thread::get_id();
  std::atomic<bool> askedOnCaller{false};
  std::atomic<int> clock{0};
  std::atomic<int> askReturnedAt{0};
  std::atomic<int> prepared{0};
  std::promise<void> askEntered;
  std::promise<void> askRelease;
  std::shared_future<void> askReleased = askRelease.get_future().share();
  FeedbackLogUpload upload(
      [&askedOnCaller, &clock, &askReturnedAt, &askEntered, askReleased,
       caller](const std::string&) {
        if (std::this_thread::get_id() == caller) {
          askedOnCaller = true;
          return logupload::ServiceAnswer::Busy;
        }
        askEntered.set_value();
        askReleased.wait();
        askReturnedAt = ++clock;
        return logupload::ServiceAnswer::NotTaken;
      },
      [&prepared](const std::string&) -> std::function<void()> {
        prepared += 1;
        return {};
      });
  Check(upload.Send("f00dfeed-0000-4000-8000-000000000003"), "cancel: sent");
  Check(!askedOnCaller.load(), "cancel: the ask does not run on the caller's thread");
  if (askedOnCaller.load()) return;
  askEntered.get_future().wait();
  ReturnNote cancelled;
  std::thread release([&cancelled, &askRelease] {
    cancelled.WaitOrAfter(std::chrono::seconds(2));
    askRelease.set_value();
  });
  upload.Cancel();
  const int cancelReturnedAt = ++clock;
  cancelled.Note();
  release.join();
  Check(upload.WaitIdle(std::chrono::seconds(60)), "cancel: the request ends");
  Check(askReturnedAt.load() != 0 && askReturnedAt.load() < cancelReturnedAt,
        "cancel: waits out the running ask");
  Check(prepared.load() == 0, "cancel: no step that uses the host runs after it");
  Check(!upload.Send("f00dfeed-0000-4000-8000-000000000004"), "cancel: nothing is sent after it");
}

// (q) destruction waits for the old path's call only up to its budget: the
// call (held by a barrier past the budget) touches nothing of the host, so it
// is left to finish on its own, sharing only the channel. Destruction runs on
// a helper thread, so that a destruction that waits for the call fails the
// check (after a generous guard) instead of hanging the run.
void TestFeedbackLogUploadExitLeavesTheOldPathsCall() {
  const std::thread::id caller = std::this_thread::get_id();
  std::atomic<bool> calledOnCaller{false};
  std::promise<void> callEntered;
  std::promise<void> callRelease;
  std::shared_future<void> callReleased = callRelease.get_future().share();
  auto callEnded = std::make_shared<std::promise<void>>();
  std::future<void> callEndedFuture = callEnded->get_future();
  auto upload = std::make_unique<FeedbackLogUpload>(
      [](const std::string&) { return logupload::ServiceAnswer::NotTaken; },
      [&calledOnCaller, &callEntered, callReleased, callEnded,
       caller](const std::string&) -> std::function<void()> {
        return [&calledOnCaller, &callEntered, callReleased, callEnded, caller] {
          if (std::this_thread::get_id() == caller) {
            calledOnCaller = true;
            callEnded->set_value();
            return;
          }
          callEntered.set_value();
          callReleased.wait();
          callEnded->set_value();
        };
      },
      std::chrono::milliseconds(100));
  Check(upload->Send("f00dfeed-0000-4000-8000-000000000005"), "exit: sent");
  Check(!calledOnCaller.load(), "exit: the old path's call does not run on the caller's thread");
  if (calledOnCaller.load()) return;
  callEntered.get_future().wait();
  std::promise<void> destroyed;
  std::future<void> destroyedFuture = destroyed.get_future();
  std::thread destroyer([&upload, &destroyed] {
    upload.reset();
    destroyed.set_value();
  });
  const bool returned =
      destroyedFuture.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
  Check(returned && callEndedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::timeout,
        "exit: destruction returns while the old path's call still runs");
  callRelease.set_value();
  destroyer.join();
  callEndedFuture.wait();
}

// (s) the app's own log files ride in the service's zip (Common/AppLogFiles.h):
// the request names the app's log directory, additively on the wire; the
// service acts as the app only in a local directory, takes only glog names,
// the newest first and no more than the cap, files written at once in name
// order
void TestAppLogFiles() {
  proto::UploadLogs sent = SampleRequest();
  sent.app_log_dir = "C:\\Users\\someone\\AppData\\Local\\URnetwork\\app\\logs";
  const proto::UploadLogs got =
      nlohmann::json::parse(proto::DumpForWire(proto::Request(proto::msg::kUploadLogs, sent)))
          .get<proto::UploadLogs>();
  Check(got.app_log_dir == sent.app_log_dir, "app logs: the request names the app's directory");
  Check(!nlohmann::json(SampleRequest()).contains("app_log_dir"),
        "app logs: a request that names none sends no field, as an older app's");
  Check(nlohmann::json::parse(R"({"feedback_id":"x"})").get<proto::UploadLogs>().app_log_dir.empty(),
        "app logs: an older app's request names no directory");

  for (const char* name : {"URnetwork.exe.HOST.someone.log.INFO.20260901-000000.101",
                           "URnetwork.exe.HOST.someone.log.WARNING.20260901-000000.101",
                           "URnetwork.exe.HOST.someone.log.ERROR.20260901-000000.101",
                           "URnetwork.exe.HOST.someone.log.FATAL.20260901-000000.101"}) {
    Check(applogs::LooksLikeGlogFileName(name), std::string("app logs: takes \"") + name + "\"");
  }
  for (const char* name : {"", "urnetwork-app.log", "app_prefs.json", "rpc_session.json",
                           "URnetwork.exe.INFO", ".a.log.INFO.1", "..\\a.log.INFO.1",
                           "a/b.log.INFO.1", "a.log.INFO.1:stream", "a\nb.log.INFO.1"}) {
    Check(!applogs::LooksLikeGlogFileName(name),
          std::string("app logs: a glog name only, refuses \"") + name + "\"");
  }

  // besides its glog files, the app's Go crash file and its own log, as the
  // sdk's upload takes them (sdk log_special_files.go), and nothing else
  for (const char* name : {"URnetwork.exe.HOST.someone.log.INFO.20260901-000000.101", "go-crash.txt",
                           "urnetwork-app.log"}) {
    Check(applogs::LooksLikeAppLogFileName(name), std::string("app logs: takes \"") + name + "\"");
  }
  for (const char* name : {"", "app_prefs.json", "rpc_session.json", "go-crash.txt.tmp",
                           ".go-crash.txt", "a\\go-crash.txt", "urnetwork-app.log:stream",
                           "urnetwork-app.log.1", "URnetwork.exe.INFO"}) {
    Check(!applogs::LooksLikeAppLogFileName(name),
          std::string("app logs: a glog name or the app's own files only, refuses \"") + name + "\"");
  }

  for (const char* dir : {"C:\\Users\\someone\\AppData\\Local\\URnetwork\\app\\logs",
                          "D:/worktree/.localstate/logs", "c:\\logs\\"}) {
    Check(applogs::LooksLikeLocalDirectory(dir),
          std::string("app logs: a local directory, \"") + dir + "\"");
  }
  for (const char* dir : {"", "logs", "C:logs", "\\\\server\\share\\logs", "//server/share/logs",
                          "\\\\?\\C:\\logs", "\\\\.\\pipe\\x", "C:\\logs\\..\\..\\Windows",
                          "C:\\logs\\.\\x", "C:\\logs:stream", "C:\\lo\ngs"}) {
    Check(!applogs::LooksLikeLocalDirectory(dir),
          std::string("app logs: never a share, a device or a climb, refuses \"") + dir + "\"");
  }

  std::vector<applogs::AppLogEntry> entries;
  for (size_t i = 0; i < applogs::kMaxAppLogFiles + 3; ++i) {
    entries.push_back(applogs::AppLogEntry{
        "URnetwork.exe.HOST.someone.log.INFO.20260901-000000." + std::to_string(100 + i), 1000 + i});
  }
  entries.push_back(applogs::AppLogEntry{"app_prefs.json", 999999});
  entries.push_back(applogs::AppLogEntry{"urnetwork-app.log", 99999});
  entries.push_back(applogs::AppLogEntry{"go-crash.txt", 1});
  entries.push_back(applogs::AppLogEntry{"URnetwork.exe.HOST.someone.log.ERROR.b", 5000});
  entries.push_back(applogs::AppLogEntry{"URnetwork.exe.HOST.someone.log.ERROR.a", 5000});
  const std::vector<std::string> picked = applogs::PickAppLogFiles(entries);
  Check(picked.size() == applogs::kMaxAppLogFiles, "app logs: no more than the cap");
  Check(!picked.empty() && picked[0] == "go-crash.txt",
        "app logs: the Go crash file first, however old");
  Check(picked.size() > 3 && picked[1] == "urnetwork-app.log" &&
            picked[2] == "URnetwork.exe.HOST.someone.log.ERROR.a" &&
            picked[3] == "URnetwork.exe.HOST.someone.log.ERROR.b",
        "app logs: then the newest first, files written at once in name order");
  Check(std::find(picked.begin(), picked.end(), "app_prefs.json") == picked.end(),
        "app logs: a name the upload does not take is never taken, however new");
  Check(std::find(picked.begin(), picked.end(),
                  "URnetwork.exe.HOST.someone.log.INFO.20260901-000000.100") == picked.end(),
        "app logs: the oldest are the ones left out");
  Check(std::string(applogs::kAppLogFilesSource) == "app", "app logs: the zip folder is app/");
}

// (r) the feedback id becomes a path segment of the API url, so only the
// server's own ids pass
void TestFeedbackId() {
  Check(proto::LooksLikeFeedbackId("f00dfeed-0000-4000-8000-000000000001"),
        "feedback id: a lower case uuid");
  Check(proto::LooksLikeFeedbackId("F00DFEED-0000-4000-8000-000000000001"),
        "feedback id: an upper case uuid");
  for (const char* bad : {
           "",
           "not-a-feedback-id",
           "../../network/provider-status",
           "f00dfeed-0000-4000-8000-00000000000",    // one short
           "f00dfeed-0000-4000-8000-0000000000012",  // one long
           "f00dfeed/0000-4000-8000-000000000001",   // a path separator for a dash
           "f00dfeed-0000-4000-8000-00000000000?",   // a query
           "f00dfeed-0000-4000-8000-00000000000 ",   // whitespace
           "{00dfeed-0000-4000-8000-00000000000}",   // braces
           "f00dfeed0000040000800000000000000001",   // no dashes
           "g00dfeed-0000-4000-8000-000000000001",   // not hex
       }) {
    Check(!proto::LooksLikeFeedbackId(bad), std::string("feedback id: refuses \"") + bad + "\"");
  }
}

}  // namespace

int main() {
  TestCarrier();
  TestStandaloneRefusals();
  TestStandaloneLifetime();
  TestAppStep();
  TestUploadLogsJson();
  TestReply();
  TestStatusCarriesTheUpload();
  TestFeedbackId();
  TestAppLogFiles();
  TestFlightStateNames();
  TestFlightRunsTheUploadOffTheCallersThread();
  TestFlightAdmitsOneUploadAtATime();
  TestFlightKeepsTheCallsDeviceUntilTheCallReturns();
  TestFlightKeepsTheFirstOutcomeAndGivesUpOnASilentUpload();
  TestFlightClearWaitsOutARunningHook();
  TestCompletionReachesTheApp();
  TestFeedbackLogUploadAsksOffTheUiThread();
  TestFeedbackLogUploadCancelWaitsOutAHostStep();
  TestFeedbackLogUploadExitLeavesTheOldPathsCall();
  std::cout << (gCases - gFailures) << "/" << gCases << " log upload checks passed\n";
  return gFailures == 0 ? 0 : 1;
}
