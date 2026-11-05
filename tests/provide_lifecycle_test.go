// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The provide-while-disconnected specs: the lifecycle
// (Common/ProvideLifecycle.h) and the wire spec (Common/Protocol.h), each with
// negative controls that rewrite a copy of a header.

// Compile one of the provide-while-disconnected harnesses against the shared
// headers (Common/ProvideLifecycle.h, and Common/Protocol.h for the wire
// spec). The headers are copied into an isolated include dir, so
// `headerMutations`, when set, can rewrite one of them by name for a negative
// control and the copy of the other picks the rewritten one up. `extra` adds
// compiler flags (the json include for the protocol spec).
func provideTestProgram(t *testing.T, harness string, headerMutations map[string]func(string) string,
	extra ...string) string {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("provide tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	includeDir := t.TempDir()
	commonDir := filepath.Join(root, "app", "src", "Common")
	for _, name := range []string{"ProvideLifecycle.h", "Protocol.h"} {
		source, err := os.ReadFile(filepath.Join(commonDir, name))
		if err != nil {
			t.Fatal(err)
		}
		content := string(source)
		if change := headerMutations[name]; change != nil {
			content = change(content)
			if content == string(source) {
				t.Fatalf("negative control did not change the production %s", name)
			}
		}
		if err := os.WriteFile(filepath.Join(includeDir, name), []byte(content), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(includeDir, strings.TrimSuffix(harness, ".cpp"))
	args := append([]string{"-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + includeDir},
		extra...)
	args = append(args, filepath.Join(root, "app", "tools", harness), "-o", program)
	if output, err := exec.Command(compiler, args...).CombinedOutput(); err != nil {
		t.Fatalf("build %s: %v\n%s", harness, err, output)
	}
	return program
}

// Execute the lifecycle spec: the sdk tier for every mode connected and
// disconnected, the service's refusals, and the app's step for every state.
func TestProvideLifecycle(t *testing.T) {
	program := provideTestProgram(t, "provide-lifecycle-tests.cpp", nil)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("provide lifecycle: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// Run a mutated copy of the lifecycle and require that it fails, naming `want`.
func requireProvideFailure(t *testing.T, harness string, headerMutations map[string]func(string) string,
	want string, extra ...string) {
	t.Helper()
	program := provideTestProgram(t, harness, headerMutations, extra...)
	output, err := exec.Command(program).CombinedOutput()
	if err == nil || !strings.Contains(string(output), want) {
		t.Fatalf("negative control was not detected (want %q): %v\n%s", want, err, output)
	}
}

// The defect: on Windows every mode provided only while connected. Put that
// rule back and Always, Auto and Network must fail while disconnected.
func TestProvideLifecycleRejectsProvidingOnlyWhileConnected(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"return SdkTierFor(mode, connected) != Tier::None;",
				"return connected && SdkTierFor(mode, connected) != Tier::None;", 1)
		},
	}, "always provides while disconnected")
}

// Auto provides publicly only while connected (the sdk); idle, it serves the
// user's own devices. A table that made it public while idle must fail.
func TestProvideLifecycleRejectsAutoPublicWhileIdle(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"return connected ? Tier::Public : Tier::Network;",
				"return connected ? Tier::Public : Tier::Public;", 1)
		},
	}, "tiers: auto disconnected")
}

// One device across connect and disconnect. A stop that is not the user's
// disconnect (the failsafe, a failed or replaced start, a logout, the service
// stopping) must not keep the device; a disconnect that ignored the armed floor
// would leave a provider beside the kill switch's block; and a connect must not
// take a provider-only device built for another identity.
func TestProvideLifecycleRejectsKeepingTheDeviceOnAnyStop(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"if (!facts.sessionDevice || !facts.disconnect) return false;",
				"if (!facts.sessionDevice) return false;", 1)
		},
	}, "one device: a stop that is not the user's disconnect")
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"  state.firewallInForce = facts.firewallInForce;\n", "", 1)
		},
	}, "one device: the kill switch's armed floor closes the device")
}

func TestProvideLifecycleRejectsPromotingAnotherIdentity(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"         a.instanceId == b.instanceId && a.deviceDescription == b.deviceDescription &&",
				"         a.deviceDescription == b.deviceDescription &&", 1)
		},
	}, "one device: a connect with a changed instance builds its own device")
}

// A rebuild that does not name a differing field leaves the next one to be
// traced by hand, as the connect's jwt refresh was.
func TestProvideLifecycleRejectsAnUnnamedDifference(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"  note(a.instanceId != b.instanceId, \"instance_id\");\n", "", 1)
		},
	}, "one device: the differing fields name instance_id")
}

// The service must not start a provider that would leave through the armed
// floor's own permit.
func TestProvideLifecycleRejectsProviderUnderArmedFloor(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"  if (state.firewallInForce) return ProviderRefusal::KillSwitchArmed;\n", "", 1)
		},
	}, "refusal: the kill switch is armed after an unexpected drop")
}

// Nor beside a tunnel session: two devices under one identity.
func TestProvideLifecycleRejectsProviderBesideTunnel(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"  if (state.tunnelSession) return ProviderRefusal::TunnelSession;\n", "", 1)
		},
	}, "refusal: a tunnel session exists or is starting")
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"if (!facts.answered || facts.tunnelSession) return DisconnectedStep::None;",
				"if (!facts.answered) return DisconnectedStep::None;", 1)
		},
	}, "step: a tunnel session exists")
}

// Never stops a provider the app finds running; a step that left it running
// must fail.
func TestProvideLifecycleRejectsNeverLeavingTheProvider(t *testing.T) {
	requireProvideFailure(t, "provide-lifecycle-tests.cpp", map[string]func(string) string{
		"ProvideLifecycle.h": func(source string) string {
			return strings.Replace(source,
				"return facts.providerRunning ? DisconnectedStep::Stop : DisconnectedStep::None;\n  }\n  // The armed floor",
				"return DisconnectedStep::None;\n  }\n  // The armed floor", 1)
		},
	}, "step: a running provider is stopped")
}

// The wire spec needs nlohmann/json, like the service and the app; a host
// without it skips (set URNETWORK_JSON_INCLUDE).
func provideJsonFlags(t *testing.T) []string {
	t.Helper()
	jsonDir, found := jsonIncludeDir(repositoryRoot(t))
	if !found {
		t.Skip("no nlohmann/json.hpp (set URNETWORK_JSON_INCLUDE)")
	}
	if jsonDir == "" {
		return nil
	}
	return []string{"-isystem", jsonDir}
}

// Execute the wire spec: the verbs, the request, the status fields and an older
// service's silence, the device comparison and the facts the reconcile reads.
func TestProvideProtocol(t *testing.T) {
	extra := provideJsonFlags(t)
	program := provideTestProgram(t, "provide-protocol-tests.cpp", nil, extra...)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("provide protocol: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// A status reader that drops the provider bit shows "not providing" over a
// provider that is; a comparison that takes the provider transport policy for
// part of the device's identity rebuilds a device that only needs the policy
// applied in place, the one a disconnect just kept among them.
func TestProvideProtocolRejectsDroppedFields(t *testing.T) {
	extra := provideJsonFlags(t)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source, "  get(\"provider_running\", v.provider_running);\n", "", 1)
		},
	}, "status: provider_running round-trips", extra...)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source,
				"  return provide::SameIdentity(IdentityOf(a), IdentityOf(b));\n}",
				"  return provide::SameIdentity(IdentityOf(a), IdentityOf(b)) &&\n"+
					"         a.provider_transport_settings_json == b.provider_transport_settings_json;\n}", 1)
		},
	}, "same device: only the provider transport policy differs", extra...)
}

// A comparison that takes the token itself for the identity rebuilds the device
// a disconnect just kept: the app refreshes its token on every connect, and
// each device refreshes its own as it starts.
func TestProvideProtocolRejectsComparingTheRawJwt(t *testing.T) {
	extra := provideJsonFlags(t)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source, "return {JwtSubject(r.by_jwt), ",
				"return {std::string(r.by_jwt), ", 1)
		},
	}, "same device: a refreshed jwt for the same client", extra...)
}

// The provider-only device's statistics on the wire, read back on the generated
// header's own urnet::ThroughputPoint and urnet::TransportDistribution: what the
// service writes with their to_json comes back through the reply and their
// from_json unchanged. The header is git-ignored, so a host without one skips.
func TestProvideProtocolAgainstSdkHeader(t *testing.T) {
	root := repositoryRoot(t)
	sdkDir := providerStatusSdkHeaderDir(t, root)
	if sdkDir == "" {
		t.Skip("no urnetwork_sdk.hpp with the provider status API (set URNETWORK_SDK_INCLUDE)")
	}
	// System includes: the generated wrapper does not build with -Wextra -Werror.
	extra := append([]string{"-DURNW_PROVIDE_PROTOCOL_TESTS_SDK", "-isystem", sdkDir},
		provideJsonFlags(t)...)
	program := provideTestProgram(t, "provide-protocol-tests.cpp", nil, extra...)
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("provide protocol against the sdk header: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}

// A reader that drops the client count shows "Providing to 0 clients" over a
// provider that has some; a reply that does not carry the statistics reads, to
// the app, exactly like an older service that has none.
func TestProvideProtocolRejectsDroppedProviderStats(t *testing.T) {
	extra := provideJsonFlags(t)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source, "  get(\"client_count\", v.client_count);\n", "", 1)
		},
	}, "provider stats: client_count round-trips", extra...)
	requireProvideFailure(t, "provide-protocol-tests.cpp", map[string]func(string) string{
		"Protocol.h": func(source string) string {
			return strings.Replace(source,
				"  if (v.provider_stats) j[\"provider_stats\"] = *v.provider_stats;\n", "", 1)
		},
	}, "reply: provider_stats round-trips", extra...)
}
