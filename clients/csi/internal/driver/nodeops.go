// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package driver

import (
	"fmt"
	"os"
	"os/exec"
	"strings"
	"syscall"
)

// nodeOps abstracts the host mount + SELinux operations NodePublish performs so
// the node service is unit-testable with a fake.
type nodeOps interface {
	MkdirAll(path string, perm os.FileMode) error
	BindMount(source, target string) error
	Unmount(target string) error
	Chcon(path, mcsLevel string) error // recursive relabel to the pod's MCS level
	RemoveAll(path string) error
}

// hostOps is the real nodeOps against the host filesystem.
type hostOps struct{}

func (hostOps) MkdirAll(path string, perm os.FileMode) error { return os.MkdirAll(path, perm) }
func (hostOps) RemoveAll(path string) error                  { return os.RemoveAll(path) }

func (hostOps) BindMount(source, target string) error {
	if err := syscall.Mount(source, target, "", syscall.MS_BIND, ""); err != nil {
		return fmt.Errorf("bind-mount %s -> %s: %w", source, target, err)
	}
	return nil
}

func (hostOps) Unmount(target string) error {
	err := syscall.Unmount(target, 0)
	if err == nil || err == syscall.EINVAL || err == syscall.ENOENT {
		return nil // unmounted, not-mounted, or gone -> idempotent
	}
	return fmt.Errorf("unmount %s: %w", target, err)
}

// Chcon relabels the per-pod socket dir to the pod's SCC-assigned MCS level so
// only that pod's category can reach it (greenlight decision 3). Shelling out to
// chcon avoids a go-selinux dependency for this slice.
func (hostOps) Chcon(path, mcsLevel string) error {
	out, err := exec.Command("chcon", "-R", "-l", mcsLevel, path).CombinedOutput()
	if err != nil {
		return fmt.Errorf("chcon -l %s %s: %w (%s)", mcsLevel, path, err, strings.TrimSpace(string(out)))
	}
	return nil
}
