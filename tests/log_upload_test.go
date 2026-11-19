// SPDX-License-Identifier: MPL-2.0

// The log upload spec (app/tools/log-upload-tests.cpp) built against the
// shared headers and run, with its negative controls: a mutated copy of a
// header must fail the harness, naming the check that guards that defect.

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The headers the log upload harness includes, by the directory under app/src
// each lives in.
var logUploadHeaderDirs = map[string]string{
	"AppLogFiles.h":       "Common",
	"LogUpload.h":         "Common",
	"Protocol.h":          "Common",
	"ProvideLifecycle.h":  "Common",
	"FeedbackLogUpload.h": "App",
}

// Builds the log upload harness against copies of the headers it includes
// (Common/LogUpload.h, Common/Protocol.h with the ProvideLifecycle.h it
// includes, and App/FeedbackLogUpload.h) in an isolated include directory, so
// `mutate`, when set, can rewrite one of them for a negative control. Skips
// without nlohmann/json, as the provide protocol harness does.
func logUploadTestProgram(t *testing.T, mutate map[string]func(string) string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("log upload tests require a C++20 compiler: ", err)
	}
	jsonFlags := provideJsonFlags(t)
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	for headerName, headerDir := range logUploadHeaderDirs {
		source, err := os.ReadFile(filepath.Join(root, "app", "src", headerDir, headerName))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if change := mutate[headerName]; change != nil {
			content = change(content)
			if content == string(source) {
				t.Fatalf("negative control did not change the production %s", headerName)
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, headerName), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, "log-upload-tests")
	args := []string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir}
	args = append(args, jsonFlags...)
	args = append(args, filepath.Join(root, "app", "tools", "log-upload-tests.cpp"), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build log-upload-tests.cpp: %v\n%s", err, output)
	}
	return program
}

// Execute the spec: the carrier, the standalone device's refusals and
// lifetime, the app's fallback, the request, reply and status on the wire, the
// feedback id check, the upload in flight (off the caller's thread, one at a
// time, its device kept until the call returns, its outcome and finish hook)
// and the app's request thread with its exit rule.
func TestLogUpload(t *testing.T) {
	program := logUploadTestProgram(t, nil)
	output, err := exec.Command(program).CombinedOutput()
	if err != nil {
		t.Fatalf("log upload: %v\n%s", err, output)
	}
	t.Logf("%s", output)
}

// Put each defect back in a copy of its header and require the harness to
// fail on the check that names it.
func TestLogUploadNegativeControls(t *testing.T) {
	type negativeControl struct {
		defect     string
		headerName string
		find       string
		replace    string
		want       string
	}
	controls := []negativeControl{
		{
			defect:     "the provider-only device carries it while a session runs",
			headerName: "LogUpload.h",
			find:       "  if (sessionDevice) return Carrier::Tunnel;\n  if (providerDevice) return Carrier::Provider;",
			replace:    "  if (providerDevice) return Carrier::Provider;\n  if (sessionDevice) return Carrier::Tunnel;",
			want:       "carrier: the session's device wins should both ever run",
		},
		{
			defect:     "a standalone device beside a running provider-only device",
			headerName: "LogUpload.h",
			find:       "  if (providerDevice) return Carrier::Provider;\n",
			replace:    "  (void)providerDevice;\n",
			want:       "carrier: the provider-only device while there is no session",
		},
		{
			defect:     "a standalone device while a teardown still holds the identity",
			headerName: "LogUpload.h",
			find:       "  if (deviceStillHeld) return StandaloneRefusal::DeviceStillHeld;\n",
			replace:    "  (void)deviceStillHeld;\n",
			want:       "refusal: a teardown still holds this identity",
		},
		{
			defect:     "a standalone device that outlives a stuck upload by hours",
			headerName: "LogUpload.h",
			find:       "kStandaloneDeviceMaxLifetime{30}",
			replace:    "kStandaloneDeviceMaxLifetime{300}",
			want:       "lifetime: 30 minutes without a report",
		},
		{
			defect:     "the app uploads through the DeviceRemote beside the service",
			headerName: "LogUpload.h",
			find:       "  if (answer != ServiceAnswer::NotTaken) return AppStep::Done;\n  if (deviceRemoteBound) return AppStep::DeviceRemote;",
			replace:    "  if (deviceRemoteBound) return AppStep::DeviceRemote;\n  if (answer != ServiceAnswer::NotTaken) return AppStep::Done;",
			want:       "app: no second upload through the DeviceRemote beside the service's",
		},
		{
			defect:     "the app falls back beside an upload in flight",
			headerName: "LogUpload.h",
			find:       "  if (answer != ServiceAnswer::NotTaken) return AppStep::Done;",
			replace:    "  if (answer == ServiceAnswer::Accepted) return AppStep::Done;",
			want:       "app: no second upload beside the one in flight",
		},
		{
			defect:     "the flight runs the upload on the caller's thread",
			headerName: "LogUpload.h",
			find:       "    std::thread([flight = shared_from_this(), id, call = std::move(call)] {",
			replace:    "    struct InPlace {\n      explicit InPlace(std::function<void()> work) { work(); }\n      void detach() {}\n    };\n    InPlace([flight = shared_from_this(), id, call = std::move(call)] {",
			want:       "flight: the upload does not run on the caller's thread",
		},
		{
			defect:     "more than one upload in flight",
			headerName: "LogUpload.h",
			find:       "    if (BusyWithLock()) return 0;\n",
			replace:    "",
			want:       "one at a time: busy while running",
		},
		{
			defect:     "an upload admitted while the last one's thread is still in the call",
			headerName: "LogUpload.h",
			find:       "    return calling_ || releasing_ || reading_.state == FlightState::Running;",
			replace:    "    return reading_.state == FlightState::Running;",
			want:       "one at a time: busy while its thread is still in the call",
		},
		{
			defect:     "the call's device released under it",
			headerName: "LogUpload.h",
			find:       "    if (!calling_ || callingDeviceHandle_ != deviceHandle || deviceHandle == 0) return device;\n",
			replace:    "    (void)deviceHandle;\n    return device;\n",
			want:       "kept device: the call's device is kept",
		},
		{
			defect:     "a later outcome overwrites the first",
			headerName: "LogUpload.h",
			find:       "      if (reading_.id != id || IsFinished(reading_.state) || !IsFinished(state)) return;",
			replace:    "      if (reading_.id != id || !IsFinished(state)) return;",
			want:       "outcome: the first is kept",
		},
		{
			defect:     "a silent upload never given up on",
			headerName: "LogUpload.h",
			find:       "        nowMillis - sinceMillis_ >= kSilentUploadMaxMillis) {",
			replace:    "        nowMillis - sinceMillis_ >= kSilentUploadMaxMillis * 1000000) {",
			want:       "outcome: a silent upload is failed at the bound",
		},
		{
			defect:     "clearing the finish hook does not wait out a running one",
			headerName: "LogUpload.h",
			find:       "    wake_.wait(lock, [this] { return notifying_ == 0; });\n",
			replace:    "",
			want:       "finish hook: clearing waits out a running hook",
		},
		{
			defect:     "another upload's outcome taken for the app's",
			headerName: "LogUpload.h",
			find:       "  if (pendingId == 0 || statusId != pendingId || !IsFinished(statusState)) return std::nullopt;",
			replace:    "  if (pendingId == 0 || statusId == 0 || !IsFinished(statusState)) return std::nullopt;",
			want:       "completion: never another upload's",
		},
		{
			defect:     "the app's request runs on the caller's thread",
			headerName: "FeedbackLogUpload.h",
			find:       "    thread_ = std::thread([channel = channel_, feedbackId = std::move(feedbackId)] {",
			replace:    "    struct InPlace {\n      explicit InPlace(std::function<void()> work) { work(); }\n    };\n    InPlace inPlace([channel = channel_, feedbackId = std::move(feedbackId)] {",
			want:       "request: the ask does not run on the caller's thread",
		},
		{
			defect:     "Cancel does not wait out a host step",
			headerName: "FeedbackLogUpload.h",
			find:       "    channel_->wake.wait(lock, [this] { return !channel_->hostStep; });\n",
			replace:    "",
			want:       "cancel: waits out the running ask",
		},
		{
			defect:     "the app's exit waits for the old path's call without a bound",
			headerName: "FeedbackLogUpload.h",
			find:       "      return channel_->wake.wait_for(lock, callJoinBudget_, [this] { return !channel_->running; });",
			replace:    "      (void)callJoinBudget_;\n      channel_->wake.wait(lock, [this] { return !channel_->running; });\n      return true;",
			want:       "exit: destruction returns while the old path's call still runs",
		},
		{
			defect:     "a path separator in the feedback id",
			headerName: "Protocol.h",
			find:       "      if (c != '-') return false;\n",
			replace:    "",
			want:       `feedback id: refuses "f00dfeed/0000-4000-8000-000000000001"`,
		},
		{
			defect:     "the feedback id left off the wire",
			headerName: "Protocol.h",
			find:       "      {\"feedback_id\", v.feedback_id},\n",
			replace:    "",
			want:       "wire: feedback_id",
		},
		{
			defect:     "the carrier left off the reply",
			headerName: "Protocol.h",
			find:       "  if (!v.log_upload_carrier.empty()) j[\"log_upload_carrier\"] = v.log_upload_carrier;\n",
			replace:    "",
			want:       "reply: the carrier comes back",
		},
		{
			defect:     "the upload's id left off the reply",
			headerName: "Protocol.h",
			find:       "  if (v.log_upload_id != 0) j[\"log_upload_id\"] = v.log_upload_id;\n",
			replace:    "",
			want:       "reply: the upload's id comes back",
		},
		{
			defect:     "the upload left off the status",
			headerName: "Protocol.h",
			find:       "      {\"log_upload_state\", v.log_upload_state},\n",
			replace:    "",
			want:       "status: the upload comes back",
		},
		{
			defect:     "the app's log directory left off the wire",
			headerName: "Protocol.h",
			find:       "  if (!v.app_log_dir.empty()) j[\"app_log_dir\"] = v.app_log_dir;\n",
			replace:    "",
			want:       "app logs: the request names the app's directory",
		},
		{
			defect:     "a file glog did not write taken from the app's directory",
			headerName: "AppLogFiles.h",
			find:       "  if (name.empty() || name.size() > 255 || name.front() == '.') return false;\n",
			replace:    "  return !name.empty();\n",
			want:       `app logs: a glog name only, refuses "app_prefs.json"`,
		},
		{
			defect:     "a share acted on as the app",
			headerName: "AppLogFiles.h",
			find:       "  if (!((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z'))) return false;\n  if (path[1] != ':' || (path[2] != '\\\\' && path[2] != '/')) return false;\n",
			replace:    "  (void)drive;\n",
			want:       `app logs: never a share, a device or a climb, refuses "\\server\share\logs"`,
		},
		{
			defect:     "more of the app's files than the cap",
			headerName: "AppLogFiles.h",
			find:       "    if (names.size() >= kMaxAppLogFiles) break;\n",
			replace:    "",
			want:       "app logs: no more than the cap",
		},
		{
			defect:     "the oldest of the app's files taken first",
			headerName: "AppLogFiles.h",
			find:       "    if (a.lastWriteTime != b.lastWriteTime) return a.lastWriteTime > b.lastWriteTime;",
			replace:    "    if (a.lastWriteTime != b.lastWriteTime) return a.lastWriteTime < b.lastWriteTime;",
			want:       "app logs: then the newest first, files written at once in name order",
		},
	}
	for _, control := range controls {
		find, replace := control.find, control.replace
		program := logUploadTestProgram(t, map[string]func(string) string{
			control.headerName: func(source string) string {
				return strings.Replace(source, find, replace, 1)
			},
		})
		output, err := exec.Command(program).CombinedOutput()
		if err == nil || !strings.Contains(string(output), control.want) {
			t.Errorf("negative control %q was not detected (want %q): %v\n%s", control.defect,
				control.want, err, output)
		}
	}
}
