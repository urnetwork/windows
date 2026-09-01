// SPDX-License-Identifier: MPL-2.0

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeVector(t *testing.T, contents string) string {
	t.Helper()
	filename := filepath.Join(t.TempDir(), "vector.xml")
	if err := os.WriteFile(filename, []byte(contents), 0o600); err != nil {
		t.Fatal(err)
	}
	return filename
}

func TestConvertFlatAndGradientPaths(t *testing.T) {
	filename := writeVector(t, `<vector xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:aapt="http://schemas.android.com/aapt"
    android:viewportWidth="32dp" android:viewportHeight="16">
  <path android:fillColor="#abc" android:pathData="M0,0   L1,1" />
  <path android:pathData="M2,2">
    <aapt:attr name="android:fillColor">
      <gradient android:startX="0" android:startY="1" android:endX="2" android:endY="3">
        <item android:offset="0.25" android:color="#FF010203" />
        <item android:offset="1" android:color="#FF040506" />
      </gradient>
    </aapt:attr>
  </path>
</vector>`)
	size := 18.0
	output, err := convert(filename, "BrandIcon", &size)
	if err != nil {
		t.Fatal(err)
	}
	for _, expected := range []string{
		`<Viewbox x:Key="BrandIcon" Width="18.0" Height="18.0">`,
		`<Canvas Width="32.0" Height="16.0">`,
		`<Path Fill="#aabbcc" Data="M0,0 L1,1" />`,
		`<LinearGradientBrush MappingMode="Absolute" StartPoint="0.0,1.0" EndPoint="2.0,3.0">`,
		`<GradientStop Offset="0.25" Color="#FF010203" />`,
	} {
		if !strings.Contains(output, expected) {
			t.Fatalf("output does not contain %q:\n%s", expected, output)
		}
	}
}

func TestUnsupportedGradientFails(t *testing.T) {
	filename := writeVector(t, `<vector xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:aapt="http://schemas.android.com/aapt">
  <path android:pathData="M0,0">
    <aapt:attr name="android:fillColor"><gradient android:type="radial" /></aapt:attr>
  </path>
</vector>`)
	_, err := convert(filename, "", nil)
	if err == nil || !strings.Contains(err.Error(), "unsupported gradient type: radial") {
		t.Fatalf("expected unsupported-gradient error, got %v", err)
	}
}

func TestArgumentsAllowOptionsAfterInput(t *testing.T) {
	parsed, err := parseArguments([]string{"icon.xml", "--name", "Brand", "--size=24"})
	if err != nil {
		t.Fatal(err)
	}
	if parsed.input != "icon.xml" || parsed.name != "Brand" || parsed.size == nil || *parsed.size != 24 {
		t.Fatalf("unexpected arguments: %+v", parsed)
	}
}
