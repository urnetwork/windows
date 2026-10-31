// SPDX-License-Identifier: MPL-2.0

//go:build !windows

package main

import (
	"os/exec"
	"syscall"
)

// A deadline also stops the ssh/scp children.
func ownProcessGroup(cmd *exec.Cmd) {
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Cancel = func() error { return syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL) }
}

func processExists(pid int) bool {
	return syscall.Kill(pid, 0) == nil
}
