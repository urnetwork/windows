// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"strings"
	"testing"
)

// The failed state offers Disconnect beside Retry (Common/BalanceGate.h
// disconnectBeside). Retry captures the machine again while the held-traffic
// line says to disconnect, so with Retry alone the tray was the only way out.
// The window declares the control hidden, the page shows it from the same
// decision that labels Retry, labels it Disconnect, and its press only
// disconnects: no reconnect after it and no start-connect gate in front of it.
func TestConnectFailedStateOffersDisconnectBesideRetry(t *testing.T) {
	xaml := readAppSource(t, "MainWindow.xaml")
	button := regexp.MustCompile(`<Button x:Name="ConnectFailedDisconnectButton"[^>]*/>`).FindString(xaml)
	if button == "" {
		t.Fatal("MainWindow.xaml: no ConnectFailedDisconnectButton beside the connect button")
	}
	for _, want := range []string{`Click="OnConnectFailedDisconnect"`, `Visibility="Collapsed"`} {
		if !strings.Contains(button, want) {
			t.Errorf("MainWindow.xaml ConnectFailedDisconnectButton: missing %s", want)
		}
	}
	connectButton := strings.Index(xaml, `<Button x:Name="ConnectButton"`)
	if connectButton < 0 || strings.Index(xaml, button) < connectButton {
		t.Errorf("MainWindow.xaml: the Disconnect beside Retry is not placed after the connect button")
	}

	window := stripLineComments(readAppSource(t, "MainWindow.xaml.cpp"))
	forward := definitionBody(t, "MainWindow.xaml.cpp", window,
		"void MainWindow::OnConnectFailedDisconnect(")
	if !strings.Contains(forward, "connect_->OnConnectFailedDisconnect(") {
		t.Errorf("MainWindow::OnConnectFailedDisconnect: does not forward to the connect page")
	}

	page := stripLineComments(readAppSource(t, "ConnectPage.cpp"))
	press := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::OnConnectFailedDisconnect(")
	if !strings.Contains(press, "Sdk().Disconnect()") {
		t.Errorf("ConnectPage::OnConnectFailedDisconnect: does not disconnect")
	}
	for _, reconnect := range []string{"ConnectSelected(", "ConnectBestAvailable(", "AdmitStartConnect("} {
		if strings.Contains(press, reconnect) {
			t.Errorf("ConnectPage::OnConnectFailedDisconnect: calls %s; the press only disconnects", reconnect)
		}
	}

	status := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::ApplyConnectStatus()")
	shown := regexp.MustCompile(`ConnectFailedDisconnectButton\(\)\.Visibility\(\s*button\.disconnectBeside\b`)
	if !shown.MatchString(status) {
		t.Errorf("ConnectPage::ApplyConnectStatus: the Disconnect beside Retry does not follow DecideConnectButton's disconnectBeside")
	}

	labels := definitionBody(t, "ConnectPage.cpp", page, "void ConnectPage::ApplyStrings()")
	if !strings.Contains(labels, `ConnectFailedDisconnectButton().Content(LocBox("disconnect"))`) {
		t.Errorf("ConnectPage::ApplyStrings: the Disconnect beside Retry is not labelled Disconnect")
	}
}
