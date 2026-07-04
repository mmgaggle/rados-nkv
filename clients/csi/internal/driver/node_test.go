// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package driver

import (
	"context"
	"os"
	"syscall"
	"testing"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/mmgaggle/rados-nkv/clients/csi/internal/spdkrpc"
)

type fakeNodeRPC struct {
	transportEnsured bool
	listeners        map[string]string // socketDir -> nqn
	removed          []string
	failAddExists    bool
}

func newFakeNodeRPC() *fakeNodeRPC { return &fakeNodeRPC{listeners: map[string]string{}} }

func (f *fakeNodeRPC) EnsureVfiouserTransport(_ context.Context) error {
	f.transportEnsured = true
	return nil
}
func (f *fakeNodeRPC) NvmfSubsystemAddListener(_ context.Context, nqn, traddr string) error {
	if f.failAddExists {
		return spdkrpc.NewError(-int(syscall.EEXIST), "listener exists")
	}
	f.listeners[traddr] = nqn
	return nil
}
func (f *fakeNodeRPC) NvmfSubsystemRemoveListener(_ context.Context, _, traddr string) error {
	delete(f.listeners, traddr)
	f.removed = append(f.removed, traddr)
	return nil
}

type fakeOps struct {
	dirs    map[string]bool
	mounts  map[string]string // target -> source
	chcon   map[string]string // path -> level
	removed []string
}

func newFakeOps() *fakeOps {
	return &fakeOps{dirs: map[string]bool{}, mounts: map[string]string{}, chcon: map[string]string{}}
}

func (o *fakeOps) MkdirAll(p string, _ os.FileMode) error { o.dirs[p] = true; return nil }
func (o *fakeOps) BindMount(src, tgt string) error        { o.mounts[tgt] = src; return nil }
func (o *fakeOps) Unmount(tgt string) error               { delete(o.mounts, tgt); return nil }
func (o *fakeOps) Chcon(p, lvl string) error              { o.chcon[p] = lvl; return nil }
func (o *fakeOps) RemoveAll(p string) error               { o.removed = append(o.removed, p); return nil }

func newNodeTestConfig(t *testing.T) Config {
	return Config{NodeID: "node-1", MuserRoot: "/run/muser", StateDir: t.TempDir(), RequireMCS: true, EnableNode: true}
}

func publishReq(volumeID, target string, mcs string) *csi.NodePublishVolumeRequest {
	vc := map[string]string{podNamespaceKey: "team-a", podUIDKey: "uid-123"}
	if mcs != "" {
		vc[mcsLevelKey] = mcs
	}
	return &csi.NodePublishVolumeRequest{
		VolumeId:         volumeID,
		TargetPath:       target,
		VolumeCapability: mountCaps()[0],
		VolumeContext:    vc,
	}
}

func TestNodePublish_HappyPath(t *testing.T) {
	rpc := newFakeNodeRPC()
	ops := newFakeOps()
	ns := newNodeServer(newNodeTestConfig(t), rpc, ops)
	nqn := DefaultNQN + ":team-a"
	vid := encodeVolumeID("kv-abc", 1, nqn)

	_, err := ns.NodePublishVolume(context.Background(), publishReq(vid, "/pods/uid-123/vol", "s0:c10,c20"))
	if err != nil {
		t.Fatalf("NodePublish: %v", err)
	}
	if !rpc.transportEnsured {
		t.Error("VFIOUSER transport was not ensured")
	}
	// exactly one listener, on the per-namespace NQN
	if len(rpc.listeners) != 1 {
		t.Fatalf("expected 1 listener, got %d", len(rpc.listeners))
	}
	var socketDir string
	for dir, gotNQN := range rpc.listeners {
		socketDir = dir
		if gotNQN != nqn {
			t.Errorf("listener NQN: got %q want %q", gotNQN, nqn)
		}
	}
	if ops.chcon[socketDir] != "s0:c10,c20" {
		t.Errorf("socket not chcon'd to the pod MCS level: %v", ops.chcon)
	}
	if ops.mounts["/pods/uid-123/vol"] != socketDir {
		t.Errorf("target not bind-mounted to the socket dir: %v", ops.mounts)
	}
}

func TestNodePublish_RejectsBlock(t *testing.T) {
	ns := newNodeServer(newNodeTestConfig(t), newFakeNodeRPC(), newFakeOps())
	req := publishReq(encodeVolumeID("kv-x", 1, DefaultNQN), "/pods/x/vol", "s0:c1")
	req.VolumeCapability = &csi.VolumeCapability{
		AccessType: &csi.VolumeCapability_Block{Block: &csi.VolumeCapability_BlockVolume{}},
	}
	_, err := ns.NodePublishVolume(context.Background(), req)
	if status.Code(err) != codes.InvalidArgument {
		t.Fatalf("expected InvalidArgument for Block, got %v", err)
	}
}

func TestNodePublish_MissingPodInfo(t *testing.T) {
	ns := newNodeServer(newNodeTestConfig(t), newFakeNodeRPC(), newFakeOps())
	req := publishReq(encodeVolumeID("kv-x", 1, DefaultNQN), "/pods/x/vol", "s0:c1")
	req.VolumeContext = map[string]string{mcsLevelKey: "s0:c1"} // no pod ns/uid
	_, err := ns.NodePublishVolume(context.Background(), req)
	if status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("expected FailedPrecondition for missing pod info, got %v", err)
	}
}

func TestNodePublish_FailClosedNoMCS(t *testing.T) {
	ns := newNodeServer(newNodeTestConfig(t), newFakeNodeRPC(), newFakeOps())
	_, err := ns.NodePublishVolume(context.Background(),
		publishReq(encodeVolumeID("kv-x", 1, DefaultNQN), "/pods/x/vol", "")) // no MCS
	if status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("expected FailedPrecondition (fail-closed) when MCS unknown, got %v", err)
	}
}

func TestNodePublish_SkipMCSWhenNotRequired(t *testing.T) {
	cfg := newNodeTestConfig(t)
	cfg.RequireMCS = false
	ops := newFakeOps()
	ns := newNodeServer(cfg, newFakeNodeRPC(), ops)
	_, err := ns.NodePublishVolume(context.Background(),
		publishReq(encodeVolumeID("kv-x", 1, DefaultNQN), "/pods/x/vol", ""))
	if err != nil {
		t.Fatalf("NodePublish (RequireMCS=false): %v", err)
	}
	if len(ops.chcon) != 0 {
		t.Errorf("chcon should be skipped when MCS unknown and not required")
	}
}

func TestNodePublish_Idempotent(t *testing.T) {
	rpc := newFakeNodeRPC()
	ns := newNodeServer(newNodeTestConfig(t), rpc, newFakeOps())
	req := publishReq(encodeVolumeID("kv-x", 1, DefaultNQN), "/pods/x/vol", "s0:c1")
	if _, err := ns.NodePublishVolume(context.Background(), req); err != nil {
		t.Fatalf("first publish: %v", err)
	}
	if _, err := ns.NodePublishVolume(context.Background(), req); err != nil {
		t.Fatalf("second publish (idempotent): %v", err)
	}
	if len(rpc.listeners) != 1 {
		t.Errorf("idempotent publish should not add a second listener, got %d", len(rpc.listeners))
	}
}

func TestNodeUnpublish_HappyPath(t *testing.T) {
	rpc := newFakeNodeRPC()
	ops := newFakeOps()
	ns := newNodeServer(newNodeTestConfig(t), rpc, ops)
	vid := encodeVolumeID("kv-x", 1, DefaultNQN)
	target := "/pods/x/vol"
	if _, err := ns.NodePublishVolume(context.Background(), publishReq(vid, target, "s0:c1")); err != nil {
		t.Fatalf("publish: %v", err)
	}
	if _, err := ns.NodeUnpublishVolume(context.Background(), &csi.NodeUnpublishVolumeRequest{VolumeId: vid, TargetPath: target}); err != nil {
		t.Fatalf("unpublish: %v", err)
	}
	if len(rpc.listeners) != 0 {
		t.Errorf("listener not removed on unpublish: %v", rpc.listeners)
	}
	if _, ok := ops.mounts[target]; ok {
		t.Errorf("target still mounted after unpublish")
	}
	if len(rpc.removed) != 1 {
		t.Errorf("expected one listener removed, got %d", len(rpc.removed))
	}
}

func TestNodeUnpublish_Idempotent(t *testing.T) {
	ns := newNodeServer(newNodeTestConfig(t), newFakeNodeRPC(), newFakeOps())
	// No prior publish (no state) -> unmount best-effort, success.
	_, err := ns.NodeUnpublishVolume(context.Background(),
		&csi.NodeUnpublishVolumeRequest{VolumeId: encodeVolumeID("kv-x", 1, DefaultNQN), TargetPath: "/pods/x/vol"})
	if err != nil {
		t.Fatalf("idempotent unpublish (no state): %v", err)
	}
}

func TestNodeGetInfoAndCaps(t *testing.T) {
	ns := newNodeServer(newNodeTestConfig(t), newFakeNodeRPC(), newFakeOps())
	info, err := ns.NodeGetInfo(context.Background(), &csi.NodeGetInfoRequest{})
	if err != nil || info.GetNodeId() != "node-1" {
		t.Fatalf("NodeGetInfo: id=%q err=%v", info.GetNodeId(), err)
	}
	if _, err := ns.NodeGetCapabilities(context.Background(), &csi.NodeGetCapabilitiesRequest{}); err != nil {
		t.Fatalf("NodeGetCapabilities: %v", err)
	}
}
