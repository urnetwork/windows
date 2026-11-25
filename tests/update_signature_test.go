// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The update's second signature check (Common/UpdateSignature.h), compiled
// and run by app/tools/update-signature-tests.cpp: SHA-512 and Ed25519 known
// answers, the SHA256SUMS format, and every verdict, with a test key in place
// of the release key. The negative controls rewrite the header and require the
// program to fail.

func updateSignatureRun(t *testing.T, mutate func(string) string) ([]byte, error) {
	t.Helper()
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("update signature tests require a C++20 compiler: ", err)
	}
	root := repositoryRoot(t)
	common := filepath.Join(root, "app", "src", "Common")
	dir := t.TempDir()
	if mutate != nil {
		data, err := os.ReadFile(filepath.Join(common, "UpdateSignature.h"))
		if err != nil {
			t.Fatal(err)
		}
		changed := mutate(string(data))
		if changed == string(data) {
			t.Fatal("negative control did not change UpdateSignature.h")
		}
		common = filepath.Join(dir, "Common")
		if err := os.Mkdir(common, 0700); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(common, "UpdateSignature.h"), []byte(changed), 0600); err != nil {
			t.Fatal(err)
		}
	}
	program := filepath.Join(dir, "update-signature-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I"+common,
		filepath.Join(root, "app", "tools", "update-signature-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build update signature tests: %v\n%s", err, output)
	}
	return exec.Command(program).CombinedOutput()
}

func TestUpdateSignature(t *testing.T) {
	output, err := updateSignatureRun(t, nil)
	if err != nil || !strings.Contains(string(output), "PASS update-signature-tests") {
		t.Fatalf("update signature tests: %v\n%s", err, output)
	}
}

func TestUpdateSignatureNegativeControls(t *testing.T) {
	controls := map[string]func(string) string{
		"verification always passes": func(s string) string {
			return strings.Replace(s, "return std::memcmp(r, signature.data(), 32) == 0;", "return true;", 1)
		},
		"S < L unchecked": func(s string) string {
			return strings.Replace(s, "if (!BelowL(signature.data() + 32)) return false;", "", 1)
		},
		"installer hash unchecked": func(s string) string {
			return strings.Replace(s, "if (actual != *want) {", "if (false) {", 1)
		},
		"unsigned never refused": func(s string) string {
			return strings.Replace(s, "if (required) return", "if (required && false) return", 1)
		},
		"signature required already": func(s string) string {
			return strings.Replace(s, "kRequireUpdateSignature = false", "kRequireUpdateSignature = true", 1)
		},
	}
	for name, mutate := range controls {
		t.Run(name, func(t *testing.T) {
			output, err := updateSignatureRun(t, mutate)
			if err == nil {
				t.Fatalf("passed with the check broken:\n%s", output)
			}
		})
	}
}
