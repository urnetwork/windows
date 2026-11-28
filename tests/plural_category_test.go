// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os/exec"
	"path/filepath"
	"testing"
)

// Compile and execute the portable plural category spec: the CLDR category
// Plural() selects a .resw entry with, Persian's one covering 0 and 1 and
// Turkish's only 1.
func TestPluralCategory(t *testing.T) {
	compiler, err := exec.LookPath("c++")
	if err != nil {
		t.Fatal("plural category tests require a C++20 compiler: ", err)
	}
	appDir := filepath.Join(repositoryRoot(t), "app")
	program := filepath.Join(t.TempDir(), "plural-category-tests")
	build := exec.Command(compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
		"-I"+filepath.Join(appDir, "src", "App"),
		filepath.Join(appDir, "tools", "plural-category-tests.cpp"), "-o", program)
	if output, err := build.CombinedOutput(); err != nil {
		t.Fatalf("build plural category tests: %v\n%s", err, output)
	}
	if output, err := exec.Command(program).CombinedOutput(); err != nil {
		t.Fatalf("plural category: %v\n%s", err, output)
	} else {
		t.Logf("%s", output)
	}
}
