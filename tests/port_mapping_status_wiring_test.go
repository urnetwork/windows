// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// The router port mapping's status line (connect EXTENDER.md S10): under the
// "Open a port on my router" switch, while the switch is on and the role
// reports a state, one line of what the router did: not needed, asking,
// forwarding these ports by this method, or not opened. The device reports the
// state with two texts that are the same in every language,
// PortMappingMethodName and PortMappingPorts, for the line to put in its own
// sentence; PortMappingError, the router's words, and PortMappingExternalIp can
// name an address, so they stay in the device's log and never reach a screen.
// The decision is ExtenderPresentation's (extender-tests.cpp runs it, and
// provide-protocol-tests.cpp the status on the wire); these read the app's
// sources and stores, which need Windows and WinRT to build, with comments
// stripped so prose cannot satisfy a contract.

// The Connect page draws the line the presentation decided, composed from the
// store, under the switch's description, hidden while there is none; both
// devices' statuses feed it through the one view reader.
func TestPortMappingStatusLineWiring(t *testing.T) {
	// the store's key, the mapped line through Format with the sdk's two texts
	// widened from UTF-8 as its {0} and {1}
	visual := stripComments(readAppSource(t, "ProvideModeVisual.h"))
	text := definitionBody(t, "ProvideModeVisual.h", visual,
		"inline std::wstring PortMappingStatusText(PortMappingRowModel const& model)")
	provideRequire(t, "PortMappingStatusText", text,
		"ComposePortMappingStatusText<std::wstring>(",
		"urnw::Localized(key)",
		"urnw::Format(key, ports, method)",
		"urnw::Widen(utf8)")

	connect := stripComments(readAppSource(t, "ConnectPage.cpp"))
	row := definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyPortMappingRow()")
	provideRequire(t, "ConnectPage::ApplyPortMappingRow", row,
		"const hstring status{urnw::PortMappingStatusText(model)};",
		"w_.PortMappingStatus().Text(status);",
		"const bool statusShown = !model.statusKey.empty();",
		"w_.PortMappingStatus().Visibility(statusShown ? Visibility::Visible : Visibility::Collapsed);")
	// the switch carries the line as its help text, as the Extender switch
	// carries its state
	provideRequireOrder(t, "ConnectPage::ApplyPortMappingRow", row,
		"AutomationProperties::SetHelpText(", "w_.PortMappingToggle(), status);")
	// the line is the model's: the page reads none of the status's own fields
	requireNone(t, "ConnectPage::ApplyPortMappingRow", row, "extenderProvideView_.portMapping")
	// a pushed status and a guess both repaint it, and so does a new language,
	// all through the one row writer
	provideRequireOrder(t, "ConnectPage::ApplyExtenderProvideRow",
		definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::ApplyExtenderProvideRow()"),
		"ApplySpeedTestsRow();", "ApplyPortMappingRow();")
	provideRequire(t, "ConnectPage::OnPortMappingToggled",
		definitionBody(t, "ConnectPage.cpp", connect, "void ConnectPage::OnPortMappingToggled("),
		"ApplyPortMappingRow();")

	// the markup: under the description, before the switch, hidden until there
	// is a line, and wrapping rather than cut, as the description does
	xaml := readAppSource(t, "MainWindow.xaml")
	markup := sourceBetween(t, "MainWindow.xaml", xaml, `x:Name="PortMappingRow"`, "</Border>")
	provideRequireOrder(t, "the port mapping row", markup,
		`x:Name="PortMappingDescription"`, `x:Name="PortMappingStatus"`)
	provideRequireOrder(t, "the port mapping row", markup,
		`x:Name="PortMappingStatus"`, `x:Name="PortMappingToggle"`)
	status := sourceBetween(t, "the port mapping row", markup, `x:Name="PortMappingStatus"`, "/>")
	provideRequire(t, "the status line", status,
		`Visibility="Collapsed"`, `TextWrapping="Wrap"`, `TextTrimming="None"`,
		`Style="{StaticResource UrRowNoteStyle}"`)

	// A session's device's status from its listener and the provider-only
	// device's from the service's document both go through the view reader,
	// whole; the service writes the device's status whole, texts and all.
	source := sdkHostSource(t)
	provideRequire(t, "PublishExtenderProvideStatus",
		definitionBody(t, "SdkHost.cpp", source,
			"void SdkHost::PublishExtenderProvideStatus(std::optional<urnet::ExtenderProvideStatus> status)"),
		"ExtenderProvideStatusViewOf(status, readSetting, readMeasurements, readPortMapping)")
	provideRequire(t, "ShowProviderOnlyStatsLocked",
		definitionBody(t, "SdkHost.cpp", source,
			"void SdkHost::ShowProviderOnlyStatsLocked(const proto::ProviderStats& stats)"),
		"proto::ExtenderProvideStatusOf<urnet::ExtenderProvideStatus>(stats)")
	provideRequire(t, "ProviderStats",
		definitionBody(t, "TunnelController.cpp", tunnelControllerSource(t),
			"proto::ProviderStats TunnelController::ProviderStats()"),
		"stats.extender_provide_status = *status;")
}

// The router's words never reach a screen: no app source reads
// PortMappingError or PortMappingExternalIp, so no view, line, tooltip or help
// text can carry them, whatever a later change draws. The device logs them.
func TestPortMappingErrorIsNeverShown(t *testing.T) {
	files := appSourceFiles(t, ".cpp", ".h", ".idl", ".xaml")
	if len(files) < 100 {
		t.Fatalf("found only %d app sources; update this contract", len(files))
	}
	xamlComment := regexp.MustCompile(`(?s)<!--.*?-->`)
	for _, name := range sortedNames(files) {
		code := files[name]
		if strings.EqualFold(filepath.Ext(name), ".xaml") {
			code = xamlComment.ReplaceAllString(code, "")
		} else {
			code = stripComments(code)
		}
		for _, field := range []string{"PortMappingError", "PortMappingExternalIp"} {
			if strings.Contains(code, field) {
				t.Errorf("%s reads %s; the router's words and its address stay in the device's log",
					name, field)
			}
		}
	}
	// nor does the view the line is drawn from have a place for them
	presentation := stripComments(readAppSource(t, "ExtenderPresentation.h"))
	view := sourceBetween(t, "ExtenderPresentation.h", presentation,
		"struct ExtenderProvideStatusView {", "};")
	provideRequire(t, "ExtenderProvideStatusView", view,
		"std::string portMapping;", "std::string portMappingMethodName;",
		"std::string portMappingPorts;")
	requireNone(t, "ExtenderProvideStatusView", view, "portMappingError", "portMappingExternalIp",
		"externalIp")
}

// Every catalog says each line, the mapped one with the ports as {0} and the
// method as {1} and nothing else to fill: urnw::Format fills them with
// std::vformat, which throws on an argument a translation names that the line
// does not pass.
func TestPortMappingStatusLineStrings(t *testing.T) {
	root := repositoryRoot(t)
	english := map[string]string{
		"extender_port_mapping_not_needed": "Not needed: this device has a public address.",
		"extender_port_mapping_mapping":    "Asking your router…",
		"extender_port_mapping_mapped":     "Your router forwards {0} ({1}).",
		"extender_port_mapping_failed":     "Your router didn't open the ports. Trying again later.",
	}
	catalogs, err := filepath.Glob(filepath.Join(root, "app", "src", "App", "Strings", "*", "Resources.resw"))
	if err != nil || len(catalogs) < 28 {
		t.Fatalf("catalogs: %v %v", catalogs, err)
	}
	placeholder := regexp.MustCompile(`\{[^}]*\}`)
	for key, want := range english {
		if got := reswValue(t, root, "en", key); got != want {
			t.Errorf("en: %s = %q, want %q", key, got, want)
		}
		wantPlaceholders := placeholder.FindAllString(want, -1)
		for _, catalog := range catalogs {
			locale := filepath.Base(filepath.Dir(catalog))
			got := reswValue(t, root, locale, key)
			if got == "" {
				t.Errorf("%s: no %s", locale, key)
				continue
			}
			gotPlaceholders := placeholder.FindAllString(got, -1)
			if len(gotPlaceholders) != len(wantPlaceholders) {
				t.Errorf("%s: %s = %q fills %v, want %v", locale, key, got, gotPlaceholders,
					wantPlaceholders)
				continue
			}
			for _, each := range wantPlaceholders {
				if strings.Count(got, each) != 1 {
					t.Errorf("%s: %s = %q must name %s once", locale, key, got, each)
				}
			}
		}
	}
}

// The spec catches each way the line could show a stale report, show under a
// switch that is off, print empty parentheses, swap its two texts, name the
// wrong state, drop a change the feed must publish, or carry the router's
// words.
func TestExtenderPresentationRejectsABrokenPortMappingStatusLine(t *testing.T) {
	cases := []struct {
		name string
		file string
		old  string
		new  string
		want string
	}{
		{
			name: "the state is not read",
			file: "ExtenderPresentation.h",
			old:  "  view.portMapping = status->PortMapping;\n",
			new:  "",
			want: "the state comes across as the sdk's token",
		},
		{
			name: "the ports text is no change",
			file: "ExtenderPresentation.h",
			old:  "           portMappingPorts == o.portMappingPorts &&\n",
			new:  "",
			want: "PortMappingPorts alone is a change the feed publishes",
		},
		{
			name: "the router's words ride the ports text",
			file: "ExtenderPresentation.h",
			old:  "  view.portMappingPorts = status->PortMappingPorts;\n",
			new:  "  view.portMappingPorts = status->PortMappingPorts + status->PortMappingError;\n",
			want: "the line never carries the router's words or an address",
		},
		{
			name: "the method fills the ports' place",
			file: "ExtenderPresentation.h",
			old:  "fromUtf8(model.statusPorts), fromUtf8(model.statusMethod)",
			new:  "fromUtf8(model.statusMethod), fromUtf8(model.statusPorts)",
			want: "mapped reads extender_port_mapping_mapped, the ports and then the method",
		},
		{
			name: "a new line is no change",
			file: "ExtenderPresentation.h",
			old:  "    return visible == o.visible && on == o.on && statusKey == o.statusKey &&\n",
			new:  "    return visible == o.visible && on == o.on &&\n",
			want: "a new line alone is a change",
		},
		{
			name: "the line shows under a switch that is off",
			file: "ExtenderPresentation.cpp",
			old:  "  if (!model.visible || !model.on) return model;\n",
			new:  "  if (!model.visible) return model;\n",
			want: "no line while the switch is off",
		},
		{
			name: "a mapped line without its texts",
			file: "ExtenderPresentation.cpp",
			old:  "    if (view.portMappingPorts.empty() || view.portMappingMethodName.empty()) return model;\n",
			new:  "",
			want: "a mapped status without its method or its ports shows no line",
		},
		{
			name: "failed reads as asking",
			file: "ExtenderPresentation.cpp",
			old:  `model.statusKey = "extender_port_mapping_failed";`,
			new:  `model.statusKey = "extender_port_mapping_mapping";`,
			want: "failed reads extender_port_mapping_failed",
		},
		{
			name: "a flip keeps the old role's report",
			file: "ExtenderPresentation.cpp",
			old:  "  guess.portMapping.clear();\n",
			new:  "",
			want: "a flip drops the role's last port mapping report",
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			requireExtenderPresentationFailure(t, c.file, func(source string) string {
				if strings.Count(source, c.old) != 1 {
					t.Fatalf("negative control expects exactly one %q in %s", c.old, c.file)
				}
				return strings.Replace(source, c.old, c.new, 1)
			}, c.want)
		})
	}
}
