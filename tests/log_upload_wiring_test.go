// SPDX-License-Identifier: MPL-2.0

// The call sites of "send feedback with logs" whether or not a tunnel runs
// (support inbox 2090). The decisions are pure and run in log_upload_test.go;
// what makes the service upload its own logs with no session, on a device that
// touches nothing else and is retired again, off the session lock and one at a
// time, and what makes the app ask it first off the UI thread, fall back to its
// DeviceRemote and learn the outcome from the service's status, lives in the
// service and in SdkHost, which need Windows and WinRT. So these read their
// sources, comments stripped so prose cannot satisfy a contract.

package tests

import (
	"strings"
	"testing"
)

// The upload_logs branch of ControlServer::Handle, through the next branch.
func uploadLogsBranch(t *testing.T) string {
	t.Helper()
	source := stripComments(readServiceSource(t, "ControlServer.cpp"))
	start := strings.Index(source, "type == proto::msg::kUploadLogs")
	if start < 0 {
		t.Fatal("ControlServer.cpp no longer serves upload_logs")
	}
	end := strings.Index(source[start:], "} else if")
	if end < 0 {
		t.Fatal("cannot find the end of the upload_logs branch in ControlServer.cpp")
	}
	return source[start : start+end]
}

// The text of `body` from `opener` through the first `closer` after it, or ""
// when either is missing.
func uploadSpan(body, opener, closer string) string {
	start := strings.Index(body, opener)
	if start < 0 {
		return ""
	}
	end := strings.Index(body[start:], closer)
	if end < 0 {
		return ""
	}
	return body[start : start+end+len(closer)]
}

// The service validates the feedback id (it becomes part of the url) and the
// identity a standalone device would register under before anything is
// touched, hands the controller the diagnostics writer for the carrier's line,
// and answers with the device, the upload's id, or that one is in flight.
func TestLogUploadWiringServiceValidatesBeforeUploading(t *testing.T) {
	branch := uploadLogsBranch(t)
	provideRequire(t, "upload_logs", branch,
		"request.get<proto::UploadLogs>()",
		"proto::LooksLikeFeedbackId(req.feedback_id)",
		"rpcsession::IsPairableInstanceId(req.instance_id)",
		"req.by_jwt.empty()",
		"req.network_space_json.empty()",
		"reply.log_upload_carrier = result.carrier;",
		"reply.log_upload_id = result.uploadId;",
		"reply.log_upload_busy = result.busy;")
	provideRequireOrder(t, "upload_logs", branch,
		"proto::LooksLikeFeedbackId(req.feedback_id)", "tunnel_.UploadLogs(req, [this](std::string_view chosen) {")
	provideRequireOrder(t, "upload_logs", branch,
		"tunnel_.UploadLogs(req, [this](std::string_view chosen) {", "diagnostics_.NoteLogUpload(chosen);")
	// the app's own log files: opened only once the request is valid, and only
	// while acting as the app's pipe client, then handed to the upload
	provideRequire(t, "upload_logs", branch,
		"pipe_.RunAsClient([&] { appLogFiles = OpenAppLogHandles(req.app_log_dir); })",
		"}, std::move(appLogFiles));")
	provideRequireOrder(t, "upload_logs", branch, "proto::LooksLikeFeedbackId(req.feedback_id)",
		"pipe_.RunAsClient(")
	provideRequireOrder(t, "upload_logs", branch, "pipe_.RunAsClient(",
		"tunnel_.UploadLogs(req, [this](std::string_view chosen) {")
	requireNone(t, "upload_logs", branch, "CreateFileW(", "FindFirstFile", "OpenAppLogHandles(req.app_log_dir);\n")
}

// The upload is admitted before anything is built; the device that runs
// carries it, by its handle; with none, a standalone device is built as the
// provider-only device is, after the refusals and after the last one is
// retired, in provide mode never, and nothing after that. The sdk's call (the
// zip) is not made here, under the session lock.
func TestLogUploadWiringStandaloneDeviceTouchesNoMachineState(t *testing.T) {
	upload := definitionBody(t, "TunnelController.cpp", tunnelControllerSource(t),
		"TunnelController::LogUploadResult TunnelController::UploadLogs(")
	provideRequire(t, "UploadLogs", upload,
		"logupload::CarrierFor(device_ != nullptr, providerDevice_ != nullptr)",
		"deviceHandle = device_->handle();",
		"deviceHandle = providerDevice_->handle();",
		"SweepAbandonedTeardowns()",
		"SelfRestartPending()",
		"slot->space = ImportNetworkSpaceLocked(request.network_space_json);",
		"slot->device->setProvideControlMode(\"never\");",
		"logUpload_ = slot;",
		"deviceHandle = slot->device->handle();",
		"result.busy = true;")
	provideRequireOrder(t, "UploadLogs", upload, "lock.try_lock_for(kStopLockBudget)",
		"logupload::CarrierFor(")
	provideRequireOrder(t, "UploadLogs", upload, "logUploadFlight_->Begin(which, SteadyMillis())",
		"NewDeviceLocked(*slot->space")
	provideRequireOrder(t, "UploadLogs", upload, "if (uploadId == 0) {", "result.busy = true;")
	provideRequireOrder(t, "UploadLogs", upload, "logupload::StandaloneRefusalFor(",
		"RetireLogUploadDeviceLocked();")
	provideRequireOrder(t, "UploadLogs", upload, "RetireLogUploadDeviceLocked();",
		"NewDeviceLocked(*slot->space")
	provideRequireOrder(t, "UploadLogs", upload, "NewDeviceLocked(*slot->space",
		"slot->device->setProvideControlMode(\"never\");")
	for _, forbidden := range []string{
		"Wintun::Load(", "WintunAdapter::Create(", "std::make_unique<NetworkConfig>",
		"ApplyWfpLocked(", "wfp_.Apply(", "setRpcServer(", "std::make_unique<EgressMonitor>",
		"std::make_unique<PacketPump>", "SetActiveMarker(", "splitTunnel_.",
		"PushExcludedToDriver(", "setFlowOwnerLookup(", "setEgressInterfaceIndex(",
		"SetStateLocked(", " device_ = ", "providerDevice_ = ", "networkSpace_ = ",
		"OpenProviderStatsLocked(", "WatchProviderNetworkLocked(",
		// the zip belongs to the upload's thread, never to the session lock
		"->uploadLogs(", "urnet_device_upload_logs(", "urnet_device_local_upload_logs_with_files(",
	} {
		if strings.Contains(upload, forbidden) {
			t.Errorf("UploadLogs must not call %q: the standalone device changes nothing on "+
				"this machine, and the zip runs off the session lock", forbidden)
		}
	}
}

// The carrier's line is handed to the diagnostics writer before the upload's
// thread starts the zip; the thread takes the flight, the device's handle and
// the waiter's signal and nothing of the controller, and calls the sdk by the
// handle; the callback ends the upload in the flight and tells the waiter,
// which retires the standalone device on that report or at the bound; a device
// built before a failure is closed.
func TestLogUploadWiringUploadAndRetirement(t *testing.T) {
	source := tunnelControllerSource(t)
	upload := definitionBody(t, "TunnelController.cpp", source,
		"TunnelController::LogUploadResult TunnelController::UploadLogs(")
	provideRequireOrder(t, "UploadLogs", upload, "if (noteCarrier) noteCarrier(carrierName);",
		"logUploadFlight_->Run(uploadId, deviceHandle,")
	if strings.Contains(upload, "logAppInfo(") {
		t.Error("UploadLogs must write the carrier's line through the hook, not into the log itself")
	}
	run := uploadSpan(upload, "logUploadFlight_->Run(uploadId, deviceHandle,", "});")
	provideRequire(t, "the upload's thread", run,
		"[flight = logUploadFlight_, uploadId, deviceHandle,",
		"UploadLogsOnDevice(flight, uploadId, deviceHandle, feedbackId,")
	requireNone(t, "the upload's thread", run, "this")
	provideRequire(t, "UploadLogs", upload,
		"slot->uploadReported = true;",
		"slot->reported.notify_all();",
		"slot->reported.wait_for(slotLock, logupload::kStandaloneDeviceMaxLifetime,",
		"return slot->uploadReported || !slot->device;",
		".detach();",
		"result.uploadId = uploadId;")
	// the failure path is the catch block alone: the waiter's close further down
	// must not stand in for it
	catchBody := uploadSpan(upload, "catch (const std::exception&)", "return result;")
	provideRequire(t, "the failure path", catchBody,
		"logUploadFlight_->Finish(uploadId, logupload::FlightState::Failed);",
		"CloseLogUploadDevice(slot);")
	waiter := uploadSpan(upload, "std::thread([slot]", ".detach();")
	if waiter == "" || !strings.Contains(waiter, "CloseLogUploadDevice(slot);") {
		t.Error("UploadLogs must start a detached waiter that owns only the slot and closes the device")
	}

	thread := definitionBody(t, "TunnelController.cpp", source, "void UploadLogsOnDevice(")
	provideRequire(t, "UploadLogsOnDevice", thread,
		"urnet_device_local_upload_logs_with_files(",
		"deviceHandle, feedbackId.c_str(), uploadLogsFilesJson.c_str(), &OnLogUploadReport,\n      report.get(), &error);",
		"report.release();",
		"flight->Finish(uploadId, logupload::FlightState::Failed);",
		"urnet_free_string(error);")
	// an upload that never started tells the waiter too, or a standalone
	// device would wait out its bound
	provideRequireOrder(t, "UploadLogsOnDevice", thread, "if (reported) reported();",
		"flight->Finish(uploadId, logupload::FlightState::Failed);")
	callback := definitionBody(t, "TunnelController.cpp", source, "void OnLogUploadReport(")
	provideRequire(t, "OnLogUploadReport", callback,
		"logupload::FlightState::Refused",
		"logupload::FlightState::Failed",
		"if (report->reported) report->reported();",
		"report->flight->Finish(report->uploadId, state);")
	for name, body := range map[string]string{"UploadLogsOnDevice": thread, "OnLogUploadReport": callback} {
		requireNone(t, name, body, "TunnelController::", "mutex_")
	}
}

// The standalone device never shares a moment with another device under the
// same identity: every teardown (and so every bring-up) and every
// provider-only start retire it first, the provider's after its refusals.
func TestLogUploadWiringEveryOtherDeviceRetiresIt(t *testing.T) {
	source := tunnelControllerSource(t)
	stop := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::StopLocked(bool finalDisarm, bool disconnect)")
	provideRequireOrder(t, "StopLocked", stop, "RetireProviderDeviceLocked();",
		"RetireLogUploadDeviceLocked();")
	provideRequireOrder(t, "StopLocked", stop, "RetireLogUploadDeviceLocked();",
		"TearDownSessionLocked(keepDevice, &kept);")
	start := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::StartProvider(")
	provideRequireOrder(t, "StartProvider", start, "provide::ProviderStartRefusal(mode, state)",
		"RetireLogUploadDeviceLocked();")
	provideRequireOrder(t, "StartProvider", start, "RetireLogUploadDeviceLocked();",
		"NewDeviceLocked(*providerSpace_")
	retire := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::RetireLogUploadDeviceLocked()")
	provideRequireOrder(t, "RetireLogUploadDeviceLocked", retire,
		"CloseLogUploadDevice(logUpload_);", "logUpload_.reset();")
}

// The device the upload's call is on stays alive until that call returns:
// each of the teardown workers closes it as always and then hands it to
// the flight, which it owns a share of, instead of releasing it. The
// controller's teardown clears the flight's finish hook first (the hook
// reaches into it) and gives a zipping upload a moment, and no more.
func TestLogUploadWiringTheCallsDeviceStaysAliveUntilTheCallReturns(t *testing.T) {
	source := tunnelControllerSource(t)
	session := definitionBody(t, "TunnelController.cpp", source,
		"bool TunnelController::TearDownSessionLocked(")
	sessionWorker := uploadSpan(session, "RunBounded(", "AbandonHazard::HoldsSessionDevice")
	provideRequire(t, "the session teardown worker", sessionWorker,
		"flight = logUploadFlight_, handback]")
	provideRequireOrder(t, "the session teardown worker", sessionWorker, "device->close();",
		"flight->KeepUntilReturned(deviceHandle,")
	provideRequireOrder(t, "the session teardown worker", sessionWorker,
		"flight->KeepUntilReturned(deviceHandle,", "device.reset();")

	// the provider-only device a start took and did not keep, closed as the
	// retire closes it
	handedOff := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::CloseHandedOffDeviceLocked(")
	handedOffWorker := uploadSpan(handedOff, "RunBounded(", "AbandonHazard::HoldsSessionDevice")
	provideRequire(t, "the handed-off close worker", handedOffWorker, "flight = logUploadFlight_]")
	provideRequireOrder(t, "the handed-off close worker", handedOffWorker, "device->close();",
		"flight->KeepUntilReturned(deviceHandle,")
	provideRequireOrder(t, "the handed-off close worker", handedOffWorker,
		"flight->KeepUntilReturned(deviceHandle,", "device.reset();")

	provider := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::RetireProviderDeviceLocked()")
	providerWorker := uploadSpan(provider, "RunBounded(", "AbandonHazard::HoldsSessionDevice")
	provideRequire(t, "the provider-only retire worker", providerWorker, "flight = logUploadFlight_]")
	provideRequireOrder(t, "the provider-only retire worker", providerWorker, "if (device) device->close();",
		"flight->KeepUntilReturned(deviceHandle,")
	provideRequireOrder(t, "the provider-only retire worker", providerWorker,
		"flight->KeepUntilReturned(deviceHandle,", "device.reset();")

	closeBody := definitionBody(t, "TunnelController.cpp", source,
		"void TunnelController::CloseLogUploadDevice(")
	provideRequire(t, "CloseLogUploadDevice", closeBody,
		"std::scoped_lock slotLock(slot->mutex);",
		"device = std::move(slot->device);",
		"flight = slot->flight]",
		"AbandonHazard::HoldsSessionDevice")
	provideRequireOrder(t, "CloseLogUploadDevice", closeBody, "device->close();",
		"flight->KeepUntilReturned(deviceHandle,")
	// taken under the slot's lock, closed outside it
	provideRequireOrder(t, "CloseLogUploadDevice", closeBody, "slot->space.reset();\n  }",
		"RunBounded(")

	teardown := definitionBody(t, "TunnelController.cpp", source, "TunnelController::~TunnelController()")
	provideRequireOrder(t, "~TunnelController", teardown, "logUploadFlight_->ClearOnFinished();", "Stop();")
	provideRequireOrder(t, "~TunnelController", teardown, "Stop();",
		"logUploadFlight_->WaitReturned(kLogUploadReturnBudget)")
}

// The outcome reaches the app: the flight's finish hook pushes the status, as
// a transition does, and the status reads the flight off its own lock, never
// the session's.
func TestLogUploadWiringTheStatusCarriesTheUpload(t *testing.T) {
	source := tunnelControllerSource(t)
	construct := definitionBody(t, "TunnelController.cpp", source, "TunnelController::TunnelController()")
	provideRequire(t, "TunnelController()", construct,
		"logUploadFlight_(std::make_shared<logupload::Flight>(NowMillis()))",
		"logUploadFlight_->SetOnFinished([this] { NotifyStateChanged(); });")
	status := definitionBody(t, "TunnelController.cpp", source, "proto::TunnelStatus TunnelController::Status()")
	provideRequire(t, "Status", status,
		"logUploadFlight_->Read(SteadyMillis())",
		"s.log_upload_id = upload.id;",
		"s.log_upload_state = logupload::ToString(upload.state);",
		"s.log_upload_carrier =")
	requireNone(t, "Status", status, "mutex_.", "lock(mutex_")
}

// The app hands the request to its own thread and returns: the page calls
// SdkHost, which sends it on FeedbackLogUpload (stopped first in ~SdkHost, by
// the network country watch's exit rule). The ask reads the session under the
// lock and asks the service without it; the old path reads the DeviceRemote's
// handle under the lock and calls the sdk by it with nothing of the host; the
// outcome is followed in every status the service pushes.
func TestLogUploadWiringAppAsksTheServiceFirst(t *testing.T) {
	client := definitionBody(t, "ServiceClient.cpp", stripComments(readAppSource(t, "ServiceClient.cpp")),
		"logupload::ServiceAnswer ServiceClient::UploadLogs(")
	provideRequire(t, "ServiceClient::UploadLogs", client,
		"proto::Request(proto::msg::kUploadLogs, body)",
		"*carrier = r.log_upload_carrier;",
		"*uploadId = r.log_upload_id;",
		"if (r.ok) return logupload::ServiceAnswer::Accepted;",
		"return r.log_upload_busy ? logupload::ServiceAnswer::Busy : logupload::ServiceAnswer::NotTaken;")

	host := sdkHostSource(t)
	send := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::UploadFeedbackLogs(const std::string& feedbackId)")
	provideRequire(t, "UploadFeedbackLogs", send, "feedbackLogUpload_->Send(feedbackId)")
	requireNone(t, "UploadFeedbackLogs", send, "service_.", "device_->", "lock(mutex_")

	ask := definitionBody(t, "SdkHost.cpp", host,
		"logupload::ServiceAnswer SdkHost::AskServiceToUploadLogs(const std::string& feedbackId)")
	provideRequire(t, "AskServiceToUploadLogs", ask,
		"request.feedback_id = feedbackId;",
		"request.by_jwt = localState_->getByClientJwt();",
		"request.instance_id = localState_->getInstanceId();",
		"request.device_description = DeviceDescription();",
		"request.device_spec = DeviceSpec();",
		"request.app_version = appVersion_;",
		"request.network_space_json = networkSpace_->toJson();",
		"pendingLogUploadId_.store(uploadId);")
	provideRequireOrder(t, "AskServiceToUploadLogs", ask,
		"request.network_space_json = networkSpace_->toJson();",
		"service_.UploadLogs(request, &carrier, &uploadId, &error)")
	// the session is read under the lock in its own scope, and the pipe call is
	// made with no lock held
	firstLock := strings.Index(ask, "std::scoped_lock lock(mutex_);")
	call := strings.Index(ask, "service_.UploadLogs(")
	if firstLock < 0 || call < 0 || !strings.Contains(ask[firstLock:call], "\n  }\n") ||
		strings.Count(ask, "lock(mutex_") != 1 {
		t.Error("AskServiceToUploadLogs must release mutex_ before asking the service")
	}

	prepare := definitionBody(t, "SdkHost.cpp", host,
		"std::function<void()> SdkHost::PrepareDeviceRemoteLogUpload(const std::string& feedbackId)")
	provideRequireOrder(t, "PrepareDeviceRemoteLogUpload", prepare, "std::scoped_lock lock(mutex_);",
		"const uint64_t deviceHandle = device_->handle();")
	provideRequire(t, "PrepareDeviceRemoteLogUpload", prepare,
		"return [deviceHandle, feedbackId] { UploadLogsThroughDeviceRemote(deviceHandle, feedbackId); };")
	oldPath := definitionBody(t, "SdkHost.cpp", host, "void UploadLogsThroughDeviceRemote(")
	provideRequire(t, "UploadLogsThroughDeviceRemote", oldPath,
		"urnet_device_upload_logs(deviceHandle, feedbackId.c_str(), &OnDeviceRemoteLogUploadResult,")
	requireNone(t, "UploadLogsThroughDeviceRemote", oldPath, "SdkHost::", "mutex_")

	follow := definitionBody(t, "SdkHost.cpp", host,
		"void SdkHost::FollowServiceLogUpload(const proto::TunnelStatus& st)")
	provideRequire(t, "FollowServiceLogUpload", follow,
		"logupload::CompletionFor(",
		"pendingLogUploadId_.compare_exchange_strong(pendingUploadId, 0)")
	adopt := definitionBody(t, "SdkHost.cpp", host, "void SdkHost::AdoptServiceFacts(const proto::TunnelStatus& st)")
	provideRequire(t, "AdoptServiceFacts", adopt, "FollowServiceLogUpload(st);")

	destroy := definitionBody(t, "SdkHost.cpp", host, "SdkHost::~SdkHost()")
	provideRequireOrder(t, "~SdkHost", destroy, "feedbackLogUpload_.reset();", "std::scoped_lock lock(mutex_);")
	header := stripComments(readAppSource(t, "SdkHost.h"))
	provideRequire(t, "SdkHost.h", header,
		"std::unique_ptr<FeedbackLogUpload> feedbackLogUpload_ = std::make_unique<FeedbackLogUpload>(")

	page := stripComments(readAppSource(t, "SettingsPage.cpp"))
	attach := definitionBody(t, "SettingsPage.cpp", page,
		"void SettingsPage::UploadLogs(std::string const& feedbackId)")
	provideRequire(t, "SettingsPage::UploadLogs", attach, "Sdk().UploadFeedbackLogs(feedbackId);")
	if strings.Contains(page, "device().uploadLogs(") {
		t.Error("SettingsPage must not upload through the DeviceRemote itself; SdkHost decides")
	}
	provideRequire(t, "OnSendFeedback", page,
		"if (attachLogs && !feedbackId.empty()) page.UploadLogs(feedbackId);")
}

// The app's own log files ride in the service's zip, under app/
// (Common/AppLogFiles.h). The app names its log directory, flushed first; the
// service lists and opens the files there only while acting as the app's pipe
// client, so Windows checks every open against the app's rights, and is the
// service again on every way out, or ends; it refuses a directory that is not
// a local one before acting at all, and keeps no link and no file that is not
// a disk file. The upload's thread hands them to the sdk by handle under app/
// and closes them once the call returned: the sdk read them in it.
func TestLogUploadWiringTheAppsLogFilesRideAsTheApp(t *testing.T) {
	pipe := stripComments(readCommonSource(t, "PipeServer.cpp"))
	runAsClient := definitionBody(t, "PipeServer.cpp", pipe,
		"bool PipeServer::RunAsClient(const std::function<void()>& work)")
	provideRequire(t, "RunAsClient", runAsClient,
		"pipe = static_cast<HANDLE>(activePipe_);",
		"if (!::RevertToSelf()) std::terminate();",
		"} revert;")
	provideRequireOrder(t, "RunAsClient", runAsClient, "if (!::ImpersonateNamedPipeClient(pipe)) {",
		"work();")
	provideRequireOrder(t, "RunAsClient", runAsClient, "} revert;", "work();")

	handles := stripComments(readServiceSource(t, "AppLogHandles.h"))
	open := definitionBody(t, "AppLogHandles.h", handles,
		"inline AppLogHandles OpenAppLogHandles(const std::string& dir)")
	provideRequireOrder(t, "OpenAppLogHandles", open,
		"if (!applogs::LooksLikeLocalDirectory(dir)) return files;", "::FindFirstFileExW(")
	provideRequire(t, "OpenAppLogHandles", open,
		"FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |",
		"if (!applogs::LooksLikeGlogFileName(name)) continue;",
		"applogs::PickAppLogFiles(std::move(entries))",
		"FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT",
		"::GetFileType(handle) != FILE_TYPE_DISK",
		"files.Add(name, handle);")
	requireNone(t, "OpenAppLogHandles", open, "Log(", "LogInfo(", "LogWarn(", "LogError(")

	source := tunnelControllerSource(t)
	thread := definitionBody(t, "TunnelController.cpp", source, "void UploadLogsOnDevice(")
	provideRequire(t, "UploadLogsOnDevice", thread,
		"uploadLogsFile.Source = applogs::kAppLogFilesSource;",
		"uploadLogsFile.FileDescriptor = static_cast<int64_t>(reinterpret_cast<intptr_t>(file.handle));")
	provideRequireOrder(t, "UploadLogsOnDevice", thread, "nlohmann::json(uploadLogsFiles).dump()",
		"urnet_device_local_upload_logs_with_files(")
	provideRequireOrder(t, "UploadLogsOnDevice", thread, "urnet_device_local_upload_logs_with_files(",
		"appLogFiles.CloseAll();")
	provideRequireOrder(t, "UploadLogsOnDevice", thread, "appLogFiles.CloseAll();", "if (started) {")
	upload := definitionBody(t, "TunnelController.cpp", source,
		"TunnelController::LogUploadResult TunnelController::UploadLogs(")
	run := uploadSpan(upload, "logUploadFlight_->Run(uploadId, deviceHandle,", "});")
	provideRequire(t, "the upload's thread", run,
		"appLogFiles = std::make_shared<AppLogHandles>(std::move(appLogFiles))]",
		"carrierName, reported, *appLogFiles);")

	ask := definitionBody(t, "SdkHost.cpp", sdkHostSource(t),
		"logupload::ServiceAnswer SdkHost::AskServiceToUploadLogs(const std::string& feedbackId)")
	provideRequireOrder(t, "AskServiceToUploadLogs", ask, "urnet::flushGlog();",
		"request.app_log_dir = urnet::getLogDir();")
	provideRequireOrder(t, "AskServiceToUploadLogs", ask, "request.app_log_dir = urnet::getLogDir();",
		"service_.UploadLogs(request, &carrier, &uploadId, &error)")
}
