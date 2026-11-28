// SPDX-License-Identifier: MPL-2.0

package tests

import (
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// The app's languages (support inbox 182 and 270). The localizations store
// generates one Strings/<locale>/Resources.resw per language, Turkish and
// Persian included, and MakePri indexes only the resw files App.vcxproj lists
// as PRIResource: a generated language missing there never reaches
// resources.pri, and the app shows English to its reader.
func TestEveryGeneratedLanguageIsIndexed(t *testing.T) {
	appDir := filepath.Join(repositoryRoot(t), "app", "src", "App")
	entries, err := os.ReadDir(filepath.Join(appDir, "Strings"))
	if err != nil {
		t.Fatal(err)
	}
	generated := []string{}
	for _, entry := range entries {
		if entry.IsDir() {
			if _, err := os.Stat(filepath.Join(appDir, "Strings", entry.Name(), "Resources.resw")); err == nil {
				generated = append(generated, entry.Name())
			}
		}
	}
	project, err := os.ReadFile(filepath.Join(appDir, "App.vcxproj"))
	if err != nil {
		t.Fatal(err)
	}
	indexed := []string{}
	for _, m := range regexp.MustCompile(`<PRIResource Include="Strings\\([^\\"]+)\\Resources\.resw" />`).FindAllStringSubmatch(string(project), -1) {
		indexed = append(indexed, m[1])
	}
	sort.Strings(generated)
	sort.Strings(indexed)
	if strings.Join(generated, " ") != strings.Join(indexed, " ") {
		t.Errorf("Strings/ has %v; App.vcxproj indexes %v", generated, indexed)
	}
	for _, language := range []string{"tr", "fa"} {
		if !strings.Contains(" "+strings.Join(indexed, " ")+" ", " "+language+" ") {
			t.Errorf("%s is not indexed", language)
		}
	}
}
