// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package driver

import (
	"context"
	"syscall"
	"testing"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/mmgaggle/rados-nkv/clients/csi/internal/spdkrpc"
)

// fakeRPC is an in-memory SPDK JSON-RPC stand-in that records provisioning
// calls and models add/remove-ns bookkeeping well enough to exercise the
// controller's idempotency paths.
type fakeRPC struct {
	created     map[string]spdkrpc.BdevKvradosCreateOpts
	nsByBdev    map[string]uint32 // bdev -> nsid, per (single) nqn
	nqn         string
	nextNSID    uint32
	deletedBdev []string
	removedNS   []uint32
	failAddNs   bool // when true, AddNs returns EEXIST (bdev already attached)
}

func newFakeRPC() *fakeRPC {
	return &fakeRPC{
		created:  map[string]spdkrpc.BdevKvradosCreateOpts{},
		nsByBdev: map[string]uint32{},
		nextNSID: 1,
	}
}

func (f *fakeRPC) BdevKvradosCreate(_ context.Context, o spdkrpc.BdevKvradosCreateOpts) error {
	if _, ok := f.created[o.Name]; ok {
		return spdkrpc.NewError(-int(syscall.EEXIST), "File exists")
	}
	f.created[o.Name] = o
	return nil
}

func (f *fakeRPC) BdevKvradosDelete(_ context.Context, name string) error {
	if _, ok := f.created[name]; !ok {
		return spdkrpc.NewError(-int(syscall.ENODEV), "No such device")
	}
	delete(f.created, name)
	f.deletedBdev = append(f.deletedBdev, name)
	return nil
}

func (f *fakeRPC) NvmfCreateSubsystem(_ context.Context, nqn, _ string) error {
	if f.nqn == nqn {
		return spdkrpc.NewError(-int(syscall.EEXIST), "subsystem exists")
	}
	f.nqn = nqn
	return nil
}

func (f *fakeRPC) NvmfSubsystemAddNs(_ context.Context, _, bdev string) (uint32, error) {
	if f.failAddNs {
		return 0, spdkrpc.NewError(-int(syscall.EEXIST), "namespace exists")
	}
	if _, ok := f.nsByBdev[bdev]; ok {
		// Already attached: model SPDK's EEXIST so the controller recovers the
		// NSID via nvmf_get_subsystems.
		return 0, spdkrpc.NewError(-int(syscall.EEXIST), "namespace exists")
	}
	id := f.nextNSID
	f.nextNSID++
	f.nsByBdev[bdev] = id
	return id, nil
}

func (f *fakeRPC) NvmfSubsystemRemoveNs(_ context.Context, _ string, nsid uint32) error {
	for bdev, id := range f.nsByBdev {
		if id == nsid {
			delete(f.nsByBdev, bdev)
		}
	}
	f.removedNS = append(f.removedNS, nsid)
	return nil
}

func (f *fakeRPC) NvmfGetSubsystems(_ context.Context) ([]spdkrpc.Subsystem, error) {
	var nss []spdkrpc.Namespace
	for bdev, nsid := range f.nsByBdev {
		nss = append(nss, spdkrpc.Namespace{NSID: nsid, BdevName: bdev})
	}
	return []spdkrpc.Subsystem{{NQN: f.nqn, Namespaces: nss}}, nil
}

func testConfig() Config {
	return Config{
		DriverName:      DefaultDriverName,
		DefaultNQN:      DefaultNQN,
		DefaultExecutor: "ofi+tcp://127.0.0.1:1234",
	}
}

func mountCaps() []*csi.VolumeCapability {
	return []*csi.VolumeCapability{{
		AccessType: &csi.VolumeCapability_Mount{Mount: &csi.VolumeCapability_MountVolume{}},
		AccessMode: &csi.VolumeCapability_AccessMode{Mode: csi.VolumeCapability_AccessMode_SINGLE_NODE_WRITER},
	}}
}

func TestCreateVolume_HappyPath(t *testing.T) {
	f := newFakeRPC()
	cs := newControllerServer(testConfig(), f)

	resp, err := cs.CreateVolume(context.Background(), &csi.CreateVolumeRequest{
		Name:               "pvc-abc",
		VolumeCapabilities: mountCaps(),
		CapacityRange:      &csi.CapacityRange{RequiredBytes: 4 << 30},
		Parameters: map[string]string{
			"computeContextSeed": "llama3-8b/fp8",
			"isolationClass":     "per-tenant",
		},
	})
	if err != nil {
		t.Fatalf("CreateVolume: %v", err)
	}
	v := resp.GetVolume()
	if v.GetCapacityBytes() != 4<<30 {
		t.Errorf("capacity: got %d want %d", v.GetCapacityBytes(), int64(4<<30))
	}
	h, derr := decodeVolumeID(v.GetVolumeId())
	if derr != nil {
		t.Fatalf("decode volume id %q: %v", v.GetVolumeId(), derr)
	}
	if h.NQN != DefaultNQN {
		t.Errorf("nqn: got %q want %q", h.NQN, DefaultNQN)
	}
	if _, ok := f.created[h.Bdev]; !ok {
		t.Errorf("bdev %q was not created", h.Bdev)
	}
	if got := f.created[h.Bdev].ExecutorEndpoint; got != "ofi+tcp://127.0.0.1:1234" {
		t.Errorf("executor endpoint not forwarded: %q", got)
	}
	if v.GetVolumeContext()["computeContextSeed"] != "llama3-8b/fp8" {
		t.Errorf("computeContextSeed not echoed in volume context")
	}
}

func TestCreateVolume_RejectsBlock(t *testing.T) {
	f := newFakeRPC()
	cs := newControllerServer(testConfig(), f)

	_, err := cs.CreateVolume(context.Background(), &csi.CreateVolumeRequest{
		Name: "pvc-block",
		VolumeCapabilities: []*csi.VolumeCapability{{
			AccessType: &csi.VolumeCapability_Block{Block: &csi.VolumeCapability_BlockVolume{}},
			AccessMode: &csi.VolumeCapability_AccessMode{Mode: csi.VolumeCapability_AccessMode_SINGLE_NODE_WRITER},
		}},
	})
	if status.Code(err) != codes.InvalidArgument {
		t.Fatalf("expected InvalidArgument for Block, got %v", err)
	}
	if len(f.created) != 0 {
		t.Errorf("no bdev should be created when validation fails")
	}
}

func TestCreateVolume_MissingExecutor(t *testing.T) {
	cfg := testConfig()
	cfg.DefaultExecutor = "" // and no param
	cs := newControllerServer(cfg, newFakeRPC())

	_, err := cs.CreateVolume(context.Background(), &csi.CreateVolumeRequest{
		Name:               "pvc-noexec",
		VolumeCapabilities: mountCaps(),
	})
	if status.Code(err) != codes.InvalidArgument {
		t.Fatalf("expected InvalidArgument for missing executor, got %v", err)
	}
}

func TestCreateVolume_InvalidParam(t *testing.T) {
	cs := newControllerServer(testConfig(), newFakeRPC())
	_, err := cs.CreateVolume(context.Background(), &csi.CreateVolumeRequest{
		Name:               "pvc-badiso",
		VolumeCapabilities: mountCaps(),
		Parameters:         map[string]string{"isolationClass": "bogus"},
	})
	if status.Code(err) != codes.InvalidArgument {
		t.Fatalf("expected InvalidArgument for bad isolationClass, got %v", err)
	}
}

func TestCreateVolume_Idempotent(t *testing.T) {
	f := newFakeRPC()
	cs := newControllerServer(testConfig(), f)
	req := &csi.CreateVolumeRequest{
		Name:               "pvc-same",
		VolumeCapabilities: mountCaps(),
		CapacityRange:      &csi.CapacityRange{RequiredBytes: 1 << 30},
	}

	first, err := cs.CreateVolume(context.Background(), req)
	if err != nil {
		t.Fatalf("first CreateVolume: %v", err)
	}
	second, err := cs.CreateVolume(context.Background(), req)
	if err != nil {
		t.Fatalf("second CreateVolume (idempotent) failed: %v", err)
	}
	if first.GetVolume().GetVolumeId() != second.GetVolume().GetVolumeId() {
		t.Errorf("idempotent CreateVolume returned different ids: %q vs %q",
			first.GetVolume().GetVolumeId(), second.GetVolume().GetVolumeId())
	}
	if len(f.created) != 1 {
		t.Errorf("expected exactly one bdev after two identical creates, got %d", len(f.created))
	}
}

func TestCreateVolume_AddNsExistsRecoversNSID(t *testing.T) {
	f := newFakeRPC()
	// Pre-seed as if the ns already exists and add_ns will report EEXIST.
	bdev := bdevName("pvc-exists")
	f.nqn = DefaultNQN
	f.nsByBdev[bdev] = 7
	f.failAddNs = true
	cs := newControllerServer(testConfig(), f)

	resp, err := cs.CreateVolume(context.Background(), &csi.CreateVolumeRequest{
		Name:               "pvc-exists",
		VolumeCapabilities: mountCaps(),
	})
	if err != nil {
		t.Fatalf("CreateVolume with existing ns: %v", err)
	}
	h, _ := decodeVolumeID(resp.GetVolume().GetVolumeId())
	if h.NSID != 7 {
		t.Errorf("expected recovered NSID 7, got %d", h.NSID)
	}
}

func TestDeleteVolume_HappyPath(t *testing.T) {
	f := newFakeRPC()
	cs := newControllerServer(testConfig(), f)
	cv, err := cs.CreateVolume(context.Background(), &csi.CreateVolumeRequest{
		Name:               "pvc-del",
		VolumeCapabilities: mountCaps(),
	})
	if err != nil {
		t.Fatalf("CreateVolume: %v", err)
	}
	id := cv.GetVolume().GetVolumeId()

	if _, err := cs.DeleteVolume(context.Background(), &csi.DeleteVolumeRequest{VolumeId: id}); err != nil {
		t.Fatalf("DeleteVolume: %v", err)
	}
	if len(f.deletedBdev) != 1 {
		t.Errorf("expected one bdev deleted, got %d", len(f.deletedBdev))
	}
	if len(f.removedNS) != 1 {
		t.Errorf("expected one ns removed, got %d", len(f.removedNS))
	}
}

func TestDeleteVolume_Idempotent(t *testing.T) {
	cs := newControllerServer(testConfig(), newFakeRPC())
	// Well-formed id for a volume that was never created: remove/delete tolerate
	// not-found, so this must succeed.
	id := encodeVolumeID("kv-deadbeefdeadbeef", 3, DefaultNQN)
	if _, err := cs.DeleteVolume(context.Background(), &csi.DeleteVolumeRequest{VolumeId: id}); err != nil {
		t.Fatalf("idempotent DeleteVolume (well-formed, unknown): %v", err)
	}
	// Malformed id: also treated as already-deleted.
	if _, err := cs.DeleteVolume(context.Background(), &csi.DeleteVolumeRequest{VolumeId: "garbage"}); err != nil {
		t.Fatalf("idempotent DeleteVolume (malformed): %v", err)
	}
}

func TestVolumeIDRoundTrip(t *testing.T) {
	// NQN contains a colon; the codec must preserve it.
	const nqn = "nqn.2026-06.io.ceph-gpu:kv"
	id := encodeVolumeID("kv-0123456789abcdef", 42, nqn)
	h, err := decodeVolumeID(id)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if h.Bdev != "kv-0123456789abcdef" || h.NSID != 42 || h.NQN != nqn {
		t.Errorf("round-trip mismatch: %+v", h)
	}
}
