// SPDX-License-Identifier: MPL-2.0

package main

import (
	"os"
	"os/exec"
)

// The driver runs on the macOS/Linux hosts; this only lets the package build
// on a Windows checkout. A deadline kills bash alone, not its children.
func ownProcessGroup(cmd *exec.Cmd) {}

func processExists(pid int) bool {
	process, err := os.FindProcess(pid)
	if err != nil {
		return false
	}
	process.Release()
	return true
}
