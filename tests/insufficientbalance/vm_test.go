// SPDX-License-Identifier: MPL-2.0

//go:build !windows

package main

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
)

// A stand-in for build/all/windows/lib.sh with the functions libVm calls.
// win_boot_vm backgrounds a long sleep in place of QEMU, like the real one.
const fakeLib = `
win_init() { WIN_QEMU_PID=""; WIN_RUN_DIR=""; WIN_MON_SOCK=""; SSH_PORT="${SSH_PORT:-2222}"; IMAGE="$FAKE_IMAGE"; }
win_ensure_ssh_key() { :; }
win_boot_vm() { WIN_RUN_DIR="$(mktemp -d)"; WIN_MON_SOCK="$WIN_RUN_DIR/mon.sock"; echo "qemu output"; sleep 30 & WIN_QEMU_PID=$!; }
win_ssh_probe() { printf '%s|%s\n' "$1" "$2"; }
win_shutdown_vm() {
  echo "pid=${WIN_QEMU_PID:-none} dir=${WIN_RUN_DIR:-none}" >"$FAKE_RECORD"
  [ -z "$WIN_RUN_DIR" ] || rm -rf "$WIN_RUN_DIR"
}
`

func TestLibVmOverFakeLib(t *testing.T) {
	dir := t.TempDir()
	lib := filepath.Join(dir, "lib.sh")
	if err := os.WriteFile(lib, []byte(fakeLib), 0600); err != nil {
		t.Fatal(err)
	}
	record := filepath.Join(dir, "record")
	image := filepath.Join(dir, "image.qcow2")
	t.Setenv("FAKE_RECORD", record)
	t.Setenv("FAKE_IMAGE", image)
	t.Setenv("SSH_PORT", "2232")
	v := newLibVm(lib, filepath.Join(dir, "qemu.log"))
	ctx := context.Background()

	if _, err := v.Boot(ctx); err == nil || !strings.Contains(err.Error(), "image is missing") {
		t.Fatalf("boot without an image: %v", err)
	}
	if err := os.WriteFile(image, nil, 0600); err != nil {
		t.Fatal(err)
	}
	// QEMU outlives the call; its output must not hold this call's pipes open
	start := time.Now()
	st, err := v.Boot(ctx)
	if err != nil {
		t.Fatal(err)
	}
	defer syscall.Kill(st.QemuPid, syscall.SIGKILL)
	if 10*time.Second < time.Since(start) {
		t.Fatal("boot waited for the background VM process")
	}
	if st.SshPort != "2232" || !isBootRunDir(st.RunDir) {
		t.Fatalf("state %+v", st)
	}
	if b, _ := os.ReadFile(filepath.Join(dir, "qemu.log")); string(b) != "qemu output\n" {
		t.Fatalf("qemu log %q", b)
	}

	out, err := v.Ssh(ctx, st, 30, `powershell -File C:/x.ps1 -Arg "a|b"`)
	if err != nil || out != "30|powershell -File C:/x.ps1 -Arg \"a|b\"\n" {
		t.Fatalf("ssh %q, %v", out, err)
	}

	// a sleep is not QEMU: never signalled, but the run dir still goes
	if v.Alive(st) {
		t.Fatal("a non-QEMU pid was treated as the VM")
	}
	if err := v.Shutdown(ctx, st); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(record); string(b) != "pid=none dir="+st.RunDir+"\n" {
		t.Fatalf("shutdown saw %q", b)
	}
	if _, err := os.Stat(st.RunDir); !os.IsNotExist(err) {
		t.Fatal("run dir remained")
	}

	// a run dir that win_boot_vm did not make is never handed to rm -rf
	if err := v.Shutdown(ctx, &vmState{RunDir: dir}); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(record); string(b) != "pid=none dir=none\n" {
		t.Fatalf("shutdown saw %q", b)
	}
}
