// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package driver

import (
	"context"
	"os"
	"testing"
	"time"

	csi "github.com/container-storage-interface/spec/lib/go/csi"

	"github.com/mmgaggle/rados-nkv/clients/csi/internal/spdkrpc"
)

// TestE2E_CreateDeleteAgainstLiveTarget drives the controller against a real
// nkv_tgt over SPDK JSON-RPC. It is skipped unless NKV_RPC_SOCK and NKV_EXECUTOR
// are set, so normal `go test` stays hermetic. This is the test that validates
// the real JSON-RPC wire and the IsAlreadyExists/IsNotFound error classification
// that the fake cannot.
//
//	NKV_RPC_SOCK=/path/spdk.sock NKV_EXECUTOR=ofi+tcp://127.0.0.1:1234 \
//	  go test ./internal/driver -run TestE2E -v
func TestE2E_CreateDeleteAgainstLiveTarget(t *testing.T) {
	sock := os.Getenv("NKV_RPC_SOCK")
	exec := os.Getenv("NKV_EXECUTOR")
	if sock == "" || exec == "" {
		t.Skip("set NKV_RPC_SOCK and NKV_EXECUTOR to run the live e2e")
	}

	cfg := Config{DriverName: DefaultDriverName, DefaultNQN: DefaultNQN, DefaultExecutor: exec}
	rpc := spdkrpc.NewClient(sock)
	cs := newControllerServer(cfg, rpc)

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	req := &csi.CreateVolumeRequest{
		Name:               "pvc-e2e-1",
		VolumeCapabilities: mountCaps(),
		CapacityRange:      &csi.CapacityRange{RequiredBytes: 1 << 30},
		Parameters:         map[string]string{"computeContextSeed": "e2e/test", "isolationClass": "per-tenant"},
	}

	first, err := cs.CreateVolume(ctx, req)
	if err != nil {
		t.Fatalf("CreateVolume: %v", err)
	}
	id := first.GetVolume().GetVolumeId()
	t.Logf("created volume id=%s", id)

	// Idempotent re-create exercises the real EEXIST classification + NSID recovery.
	second, err := cs.CreateVolume(ctx, req)
	if err != nil {
		t.Fatalf("CreateVolume (idempotent): %v", err)
	}
	if id != second.GetVolume().GetVolumeId() {
		t.Fatalf("idempotent create returned different ids: %q vs %q", id, second.GetVolume().GetVolumeId())
	}

	h, err := decodeVolumeID(id)
	if err != nil {
		t.Fatalf("decode volume id: %v", err)
	}
	subs, err := rpc.NvmfGetSubsystems(ctx)
	if err != nil {
		t.Fatalf("nvmf_get_subsystems: %v", err)
	}
	if _, ok := spdkrpc.FindNSID(subs, h.NQN, h.Bdev); !ok {
		t.Fatalf("namespace for bdev %s not found under %s after create", h.Bdev, h.NQN)
	}
	t.Logf("verified namespace present: bdev=%s nsid=%d nqn=%s", h.Bdev, h.NSID, h.NQN)

	// Delete, then delete again (idempotent -> exercises real not-found classification).
	if _, err := cs.DeleteVolume(ctx, &csi.DeleteVolumeRequest{VolumeId: id}); err != nil {
		t.Fatalf("DeleteVolume: %v", err)
	}
	if _, err := cs.DeleteVolume(ctx, &csi.DeleteVolumeRequest{VolumeId: id}); err != nil {
		t.Fatalf("DeleteVolume (idempotent): %v", err)
	}

	subs, err = rpc.NvmfGetSubsystems(ctx)
	if err != nil {
		t.Fatalf("nvmf_get_subsystems (post-delete): %v", err)
	}
	if _, ok := spdkrpc.FindNSID(subs, h.NQN, h.Bdev); ok {
		t.Fatalf("namespace for bdev %s still present after delete", h.Bdev)
	}
	t.Logf("e2e create/idempotent/delete OK")
}
