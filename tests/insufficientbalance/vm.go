// SPDX-License-Identifier: MPL-2.0

package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// The VM through build/all/windows/lib.sh, the lifecycle test-main.sh and the
// product build share: the same image, overlay boot, ssh key and options,
// hard-bounded ssh probes, hermetic guest policy and graceful shutdown. Each
// call is one bash process that sources lib.sh and restores the VM identity
// from state, since every driver verb is a separate process.
type libVm struct {
	lib     string
	qemuLog string
}

func newLibVm(lib, qemuLog string) *libVm {
	return &libVm{lib: lib, qemuLog: qemuLog}
}

// win_init resets the VM identity; it is restored from the IB_* values.
const libPrelude = `set -euo pipefail
source "$IB_WINDOWS_LIB"
win_init
WIN_QEMU_PID="${IB_QEMU_PID:-}"
WIN_RUN_DIR="${IB_RUN_DIR:-}"
WIN_MON_SOCK="${IB_MON_SOCK:-}"
`

func (self *libVm) bash(ctx context.Context, st *vmState, script string, args ...string) (string, error) {
	cmd := exec.CommandContext(ctx, "bash", append([]string{"-c", libPrelude + script, "ib-windows"}, args...)...)
	cmd.Env = append(os.Environ(), "IB_WINDOWS_LIB="+self.lib)
	if st != nil {
		cmd.Env = append(cmd.Env, "IB_RUN_DIR="+st.RunDir, "IB_MON_SOCK="+st.MonSock)
		// never 0: lib.sh would signal its own process group
		if 0 < st.QemuPid {
			cmd.Env = append(cmd.Env, "IB_QEMU_PID="+strconv.Itoa(st.QemuPid))
		}
		if st.SshPort != "" {
			cmd.Env = append(cmd.Env, "SSH_PORT="+st.SshPort)
		}
	}
	// own process group, so a deadline also stops ssh/scp children
	ownProcessGroup(cmd)
	cmd.WaitDelay = 5 * time.Second
	var stdout, stderr bytes.Buffer
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	if err := cmd.Run(); err != nil {
		return stdout.String(), fmt.Errorf("%w: %s", err, lastLine(stderr.String()))
	}
	return stdout.String(), nil
}

func lastLine(s string) string {
	lines := strings.Split(strings.TrimSpace(s), "\n")
	return bounded(strings.TrimSpace(lines[len(lines)-1]), 160)
}

// QEMU runs in the background past this process; its output goes to a log
// file so it never holds the runner's stdout pipe open.
func (self *libVm) Boot(ctx context.Context) (*vmState, error) {
	stdout, err := self.bash(ctx, nil, `win_ensure_ssh_key >&2
[ -f "$IMAGE" ] || { echo "Windows VM image is missing; run build/all/windows/setup.sh" >&2; exit 1; }
{ win_boot_vm; } >"$1" 2>&1 </dev/null
printf '%s %s %s %s\n' "$WIN_QEMU_PID" "$WIN_RUN_DIR" "$WIN_MON_SOCK" "$SSH_PORT"
`, self.qemuLog)
	if err != nil {
		return nil, err
	}
	return parseBootLine(stdout)
}

func isBootRunDir(path string) bool {
	return filepath.IsAbs(path) && strings.HasPrefix(filepath.Base(filepath.Clean(path)), "tmp.")
}

func parseBootLine(stdout string) (*vmState, error) {
	fields := strings.Fields(stdout)
	if len(fields) != 4 {
		return nil, errors.New("lib.sh did not report the booted VM")
	}
	pid, err := strconv.Atoi(fields[0])
	if err != nil || pid <= 0 {
		return nil, errors.New("lib.sh reported no QEMU pid")
	}
	if !isBootRunDir(fields[1]) {
		return nil, errors.New("lib.sh reported an unexpected VM run dir")
	}
	return &vmState{QemuPid: pid, RunDir: fields[1], MonSock: fields[2], SshPort: fields[3]}, nil
}

// The pid must still be this VM's QEMU (its command line names the overlay in
// the run dir): a recycled pid, or another QEMU, is never signalled.
func (self *libVm) Alive(st *vmState) bool {
	if st == nil || st.QemuPid <= 0 || !isBootRunDir(st.RunDir) || !processExists(st.QemuPid) {
		return false
	}
	out, err := exec.Command("ps", "-p", strconv.Itoa(st.QemuPid), "-o", "args=").Output()
	return err == nil && strings.Contains(string(out), "qemu-system") &&
		strings.Contains(string(out), filepath.Join(st.RunDir, "overlay.qcow2"))
}

func (self *libVm) WaitSsh(ctx context.Context, st *vmState, tries int) error {
	_, err := self.bash(ctx, st, `win_wait_ssh "$1"`, strconv.Itoa(tries))
	return err
}

func (self *libVm) Prepare(ctx context.Context, st *vmState) error {
	_, err := self.bash(ctx, st, `win_prepare_hermetic_guest`)
	return err
}

func (self *libVm) Ssh(ctx context.Context, st *vmState, deadlineSeconds int, command string) (string, error) {
	return self.bash(ctx, st, `win_ssh_probe "$1" "$2"`, strconv.Itoa(deadlineSeconds), command)
}

func (self *libVm) CopyTo(ctx context.Context, st *vmState, local, remote string) error {
	ctx, cancel := context.WithTimeout(ctx, 5*time.Minute)
	defer cancel()
	_, err := self.bash(ctx, st, `win_scp_to "$1" "$2"`, local, remote)
	return err
}

func (self *libVm) CopyFrom(ctx context.Context, st *vmState, remote, local string) error {
	ctx, cancel := context.WithTimeout(ctx, 2*time.Minute)
	defer cancel()
	_, err := self.bash(ctx, st, `win_scp_from "$1" "$2"`, remote, local)
	return err
}

func (self *libVm) Screendump(ctx context.Context, st *vmState, path string) error {
	_, err := self.bash(ctx, st, `[ -S "$WIN_MON_SOCK" ] && win_mon "$WIN_MON_SOCK" "screendump $1"`, path)
	return err
}

// Only a live QEMU is shut down; the run dir is removed either way, and only
// when it is the mktemp directory win_boot_vm made.
func (self *libVm) Shutdown(ctx context.Context, st *vmState) error {
	stop := vmState{SshPort: st.SshPort}
	if self.Alive(st) {
		stop.QemuPid = st.QemuPid
	}
	if isBootRunDir(st.RunDir) {
		stop.RunDir = st.RunDir
	}
	st = &stop
	_, err := self.bash(ctx, st, `win_shutdown_vm`)
	return err
}
