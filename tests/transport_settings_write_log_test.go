// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"regexp"
	"testing"
)

// Each transport settings write logs one line (support inbox 8667): the policy
// written, and whether it reached the service or only the app's saved copy.
// After a disconnect the service dialed only QUIC, most likely under a
// Transports choice made while no session was up, which reached only the saved
// copy with no line and seeded the service at the next connect. SdkHost.cpp
// needs Windows, so the contract is read off its source, comments stripped so
// prose cannot satisfy it.
func TestTransportSettingsWriteLogsWhereItWent(t *testing.T) {
	host := stripComments(readAppSource(t, "SdkHost.cpp"))
	transport := functionBody(host, "void SdkHost::ApplyTransportSettings(")
	if transport == "" {
		t.Fatal("SdkHost::ApplyTransportSettings is missing")
	}
	quitRequireSequence(t, "SdkHost::ApplyTransportSettings", transport,
		`if \(!write\) \{`, `return;`,
		// no session: the client policy waits for the next connect, and the
		// provider policy goes to the service in the reconcile
		`const char\* serviceReach = provider \? "gets it in the provider reconcile"\s*: "gets it at its next connect";`,
		`const bool attached = device_->getRemoteConnected\(\);`,
		`device_->setProviderTransportSettings\(settings\);`,
		`serviceReach = attached \? "applied it" : "gets it at the next sync";`,
		`serviceReach = "did not get it";`,
		`const char\* copyReach = "is not open";`,
		`localState_->setProviderTransportSettings\(settings\);`,
		`copyReach = "took it";`,
		`copyReach = "did not take it";`,
		`RequestProviderReconcile\("provider transport policy changed"\);`,
		`LogInfo\("sdkhost: \{\} transport settings written \(\{\}\): the service \{\}; the app's saved copy \{\}",\s*provider \? "provider" : "client", TransportSettingsLogText\(settings\), serviceReach,\s*copyReach\);`,
	)

	// the line names only the mode and the Auto priorities
	logText := functionBody(host, "std::string TransportSettingsLogText(const urnet::TransportSettings& settings)")
	if logText == "" {
		t.Fatal("TransportSettingsLogText is missing")
	}
	quitRequireSequence(t, "TransportSettingsLogText", logText,
		`std::string text = "mode=" \+ settings\.mode \+ " auto=";`,
		`if \(settings\.auto_mode_priorities\) \{`,
		`for \(const urnet::TransportModePriority& item : \*settings\.auto_mode_priorities\) \{`,
		`text \+= item\.mode \+ ":" \+ std::to_string\(item\.priority\);`,
		`return text;`,
	)
	for _, field := range regexp.MustCompile(`\b(settings|item)\.([A-Za-z_]+)`).FindAllStringSubmatch(logText, -1) {
		switch field[0] {
		case "settings.mode", "settings.auto_mode_priorities", "item.mode", "item.priority":
		default:
			t.Errorf("TransportSettingsLogText reads %s; the line names only the mode and the priorities", field[0])
		}
	}
}
