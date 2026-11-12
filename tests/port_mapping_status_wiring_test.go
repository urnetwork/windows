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
