// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"strings"
	"testing"
)

// The connect button and the balance recovery's rebuild connect to the
// selection without reading it on the UI thread (owner report 2026-10-16):
// SdkHost::SelectedLocation takes mutex_, which the session worker holds
// through a whole connect, and with no presented controller it reads the
// DeviceRemote, whose state lock a held command keeps for the 60 s rpc
// timeout. The worker reads it instead (ConnectKind::Selected). Read with
// every comment blanked.

func TestConnectSelectionIsReadOnTheWorker(t *testing.T) {
	page := stripComments(readAppSource(t, "ConnectPage.cpp"))
	toggle := functionBody(page, "void ConnectPage::OnConnectToggle(")
	if toggle == "" {
		t.Fatal("ConnectPage::OnConnectToggle is missing")
	}
	quitForbid(t, "ConnectPage::OnConnectToggle", toggle,
		"it takes mutex_ on the UI thread, behind a whole connect", "SelectedLocation(")
	if strings.Count(toggle, "Sdk().ConnectSelected();") != 2 {
		t.Error("ConnectPage::OnConnectToggle: the press and the gate's second ask must both connect through ConnectSelected")
	}
	quitRequireSequence(t, "ConnectPage::OnConnectToggle's gate", toggle,
		`AdmitStartConnect\("connect button", \[retry\] \{`, `if \(retry\) Sdk\(\)\.Disconnect\(\);`,
		`Sdk\(\)\.ConnectSelected\(\);`, `connectStatus_ = ConnectStatus::Connecting;`)
	if optimistic := strings.Index(toggle, "connectStatus_ = ConnectStatus::Connecting;"); optimistic < 0 ||
		!strings.Contains(toggle[optimistic:], "Sdk().ConnectSelected();") {
		t.Error("ConnectPage::OnConnectToggle: the press does not connect through ConnectSelected after its optimistic state")
	}

	app := stripComments(readAppSource(t, "AppController.cpp"))
	observe := functionBody(app, "void AppController::ObserveBalanceRecovery(")
	quitForbid(t, "AppController::ObserveBalanceRecovery", observe,
		"it takes mutex_ on the UI thread, behind a whole connect", "SelectedLocation(")
	quitRequireSequence(t, "AppController::ObserveBalanceRecovery's rebuild", observe,
		regexp.QuoteMeta("sdk_.RetryRefusedConnect([this] { sdk_.ConnectSelected(); });"))

	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	entry := functionBody(host, "void SdkHost::ConnectSelected(")
	quitRequireSequence(t, "SdkHost::ConnectSelected", entry,
		regexp.QuoteMeta(`AdmitStartConnect("connect (selection)", [this] { ConnectSelected(); })`),
		regexp.QuoteMeta("HasSession() ? ConnectKind::Selected : ConnectKind::BestAvailable"),
		regexp.QuoteMeta("RequestSession(std::move(r));"))
	locked := functionBody(host, "void SdkHost::ConnectLocked(")
	quitRequireSequence(t, "SdkHost::ConnectLocked", locked,
		regexp.QuoteMeta("if (request.kind == ConnectKind::Selected) {"),
		regexp.QuoteMeta("selected = connectVc_->getSelectedLocation();"),
		regexp.QuoteMeta("selected = device_->getConnectLocation();"),
		regexp.QuoteMeta("if (IsBestAvailableSelected(selected)) {"),
		regexp.QuoteMeta("request.kind = ConnectKind::BestAvailable;"),
		regexp.QuoteMeta("request.kind = ConnectKind::Location;"),
		regexp.QuoteMeta("connectVc_->connectBestAvailable();"))

	// a Selected request is a connect to the gesture table, the sign-out
	// fence and the exit drain
	header := stripComments(readAppSource(t, "SdkHost.h"))
	for _, row := range []string{
		"case ConnectKind::Selected:      return signoutfence::Work::Connect;",
		"case ConnectKind::Selected:      return gesture::Gesture::Connect;",
		"case ConnectKind::Selected:      return exitdrain::Queued::Connect;",
	} {
		if !strings.Contains(header, row) {
			t.Errorf("SdkHost.h is missing %q", row)
		}
	}
}
