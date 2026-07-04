// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package driver

import (
	"context"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/mmgaggle/rados-nkv/clients/csi/internal/spdkrpc"
)

// TestE2ENode_ListenerLifecycle validates the node plugin's real SPDK JSON-RPC
// wire format against a live nkv_tgt: ensure VFIOUSER transport, add a per-pod
// vfio-user listener on a subsystem, confirm the cntrl socket materializes, then
// remove it. Skipped unless NKV_RPC_SOCK is set. Self-contained (creates its own
// subsystem), so it does not depend on the controller e2e or run order.
func TestE2ENode_ListenerLifecycle(t *testing.T) {
	sock := os.Getenv("NKV_RPC_SOCK")
	if sock == "" {
		t.Skip("set NKV_RPC_SOCK to run the live node e2e")
	}
	c := spdkrpc.NewClient(sock)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	const nqn = "nqn.2026-06.io.ceph-gpu:kv:e2e-node"
	if err := c.NvmfCreateSubsystem(ctx, nqn, "SPDKNODE01"); err != nil && !spdkrpc.IsAlreadyExists(err) {
		t.Fatalf("nvmf_create_subsystem: %v", err)
	}
	if err := c.EnsureVfiouserTransport(ctx); err != nil {
		t.Fatalf("EnsureVfiouserTransport: %v", err)
	}

	dir, err := os.MkdirTemp("", "nkv-nodesock-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(dir)

	if err := c.NvmfSubsystemAddListener(ctx, nqn, dir); err != nil {
		t.Fatalf("NvmfSubsystemAddListener: %v", err)
	}

	cntrl := filepath.Join(dir, "cntrl")
	var appeared bool
	for i := 0; i < 50; i++ {
		if fi, err := os.Stat(cntrl); err == nil && fi.Mode()&os.ModeSocket != 0 {
			appeared = true
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if !appeared {
		t.Fatalf("vfio-user cntrl socket did not appear at %s", cntrl)
	}
	t.Logf("vfio-user listener socket materialized: %s", cntrl)

	if err := c.NvmfSubsystemRemoveListener(ctx, nqn, dir); err != nil {
		t.Fatalf("NvmfSubsystemRemoveListener: %v", err)
	}
	t.Logf("node listener lifecycle OK (add -> socket -> remove)")
}
