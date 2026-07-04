// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package driver

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"strconv"
	"strings"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/mmgaggle/rados-nkv/clients/csi/internal/spdkrpc"
)

// controllerServer implements the CSI Controller service. CreateVolume drives
// the rados-nkv provisioning sequence over SPDK JSON-RPC (see scripts/rados-nkv):
//
//	bdev_kvrados_create   -> RADOS-backed KV forwarder bdev
//	nvmf_create_subsystem -> subsystem to hold the namespace (idempotent)
//	nvmf_subsystem_add_ns -> attach the bdev as a KV namespace, returns NSID
//
// The vfio-user listener/socket is NOT created here: that is the node plugin's
// job at NodePublish (bead spdk-csi.2). Attach is node-local, so we do not
// advertise PUBLISH_UNPUBLISH_VOLUME and there is no ControllerPublishVolume.
type controllerServer struct {
	csi.UnimplementedControllerServer
	cfg Config
	rpc spdkrpc.RPC
}

func newControllerServer(cfg Config, rpc spdkrpc.RPC) *controllerServer {
	return &controllerServer{cfg: cfg, rpc: rpc}
}

// encodeVolumeID packs the handle DeleteVolume needs into a CSI volume ID.
// Format: "v1:<bdev>:<nsid>:<nqn>". bdev and nsid are colon-free and
// fixed-position; the NQN (which contains a colon) is captured last, verbatim.
func encodeVolumeID(bdev string, nsid uint32, nqn string) string {
	return fmt.Sprintf("v1:%s:%d:%s", bdev, nsid, nqn)
}

type volumeHandle struct {
	Bdev string
	NSID uint32
	NQN  string
}

func decodeVolumeID(id string) (volumeHandle, error) {
	parts := strings.SplitN(id, ":", 4)
	if len(parts) != 4 || parts[0] != "v1" {
		return volumeHandle{}, fmt.Errorf("malformed volume id %q", id)
	}
	nsid, err := strconv.ParseUint(parts[2], 10, 32)
	if err != nil {
		return volumeHandle{}, fmt.Errorf("malformed nsid in volume id %q: %w", id, err)
	}
	return volumeHandle{Bdev: parts[1], NSID: uint32(nsid), NQN: parts[3]}, nil
}

// bdevName derives a deterministic, SPDK-safe bdev name from the CSI volume
// name. The external-provisioner keys the CSI name off the PVC UID and reuses
// it across retries, so a deterministic bdev name makes CreateVolume idempotent.
func bdevName(volName string) string {
	sum := sha256.Sum256([]byte(volName))
	return "kv-" + hex.EncodeToString(sum[:8]) // "kv-" + 16 hex chars
}

func (s *controllerServer) CreateVolume(ctx context.Context, req *csi.CreateVolumeRequest) (*csi.CreateVolumeResponse, error) {
	name := req.GetName()
	if name == "" {
		return nil, status.Error(codes.InvalidArgument, "CreateVolume: name is required")
	}
	caps := req.GetVolumeCapabilities()
	if len(caps) == 0 {
		return nil, status.Error(codes.InvalidArgument, "CreateVolume: volume capabilities are required")
	}
	if err := validateVolumeCapabilities(caps); err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}

	params, err := parseParams(req.GetParameters(), s.cfg)
	if err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	if params.ExecutorEndpoint == "" {
		return nil, status.Error(codes.InvalidArgument,
			"CreateVolume: executorEndpoint not set (StorageClass param or --executor-endpoint); bdev_kvrados is a forwarder and requires one")
	}

	quota := capacityToQuota(req.GetCapacityRange())
	bdev := bdevName(name)
	nqn := params.NQN

	// 1. RADOS-backed KV forwarder bdev. Deterministic name -> tolerate EEXIST
	//    (the kvrados bdev create reliably reports -EEXIST).
	if err := s.rpc.BdevKvradosCreate(ctx, spdkrpc.BdevKvradosCreateOpts{
		Name:             bdev,
		ExecutorEndpoint: params.ExecutorEndpoint,
		ReadOnly:         params.ReadOnly,
		MaxKeySize:       params.MaxKeySize,
		MaxValueSize:     params.MaxValueSize,
		NumKeys:          params.NumKeys,
	}); err != nil && !spdkrpc.IsAlreadyExists(err) {
		return nil, status.Errorf(codes.Internal, "bdev_kvrados_create %q: %v", bdev, err)
	}

	// Snapshot subsystem state for idempotent check-then-act. base-nvmf reports
	// "already exists" as a generic INTERNAL_ERROR whose message does not contain
	// "exist" (verified against a live target), so idempotency keys off the
	// listing, not off create/add_ns error strings.
	subs, err := s.rpc.NvmfGetSubsystems(ctx)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "nvmf_get_subsystems: %v", err)
	}

	// 2. Ensure the subsystem exists.
	if !spdkrpc.HasSubsystem(subs, nqn) {
		if cerr := s.rpc.NvmfCreateSubsystem(ctx, nqn, params.Serial); cerr != nil && !spdkrpc.IsAlreadyExists(cerr) {
			// A concurrent creator may have won between our GET and here; confirm
			// against a fresh listing before failing.
			fresh, gerr := s.rpc.NvmfGetSubsystems(ctx)
			if gerr != nil || !spdkrpc.HasSubsystem(fresh, nqn) {
				return nil, status.Errorf(codes.Internal, "nvmf_create_subsystem %q: %v", nqn, cerr)
			}
			subs = fresh
		}
	}

	// 3. Attach the bdev as a KV namespace, or reuse the existing NSID.
	nsid, ok := spdkrpc.FindNSID(subs, nqn, bdev)
	if !ok {
		nsid, err = s.rpc.NvmfSubsystemAddNs(ctx, nqn, bdev)
		if err != nil {
			// Possibly attached concurrently; re-resolve from a fresh listing.
			fresh, gerr := s.rpc.NvmfGetSubsystems(ctx)
			if gerr == nil {
				if id, found := spdkrpc.FindNSID(fresh, nqn, bdev); found {
					nsid = id
					err = nil
				}
			}
			if err != nil {
				return nil, status.Errorf(codes.Internal, "nvmf_subsystem_add_ns %s %s: %v", nqn, bdev, err)
			}
		}
	}

	volCtx := map[string]string{
		"nqn":                nqn,
		"bdev":               bdev,
		"nsid":               strconv.FormatUint(uint64(nsid), 10),
		"computeContextSeed": params.ComputeContextSeed,
		"isolationClass":     params.IsolationClass,
		"transport":          params.Transport,
		"namespaceTenancy":   params.NamespaceTenancy,
		"cephxScope":         params.CephxScope,
		"quotaBytes":         strconv.FormatInt(quota, 10),
	}

	return &csi.CreateVolumeResponse{
		Volume: &csi.Volume{
			VolumeId:      encodeVolumeID(bdev, nsid, nqn),
			CapacityBytes: quota,
			VolumeContext: volCtx,
		},
	}, nil
}

func (s *controllerServer) DeleteVolume(ctx context.Context, req *csi.DeleteVolumeRequest) (*csi.DeleteVolumeResponse, error) {
	id := req.GetVolumeId()
	if id == "" {
		return nil, status.Error(codes.InvalidArgument, "DeleteVolume: volume id is required")
	}
	h, err := decodeVolumeID(id)
	if err != nil {
		// An id we cannot parse names nothing we can act on. CSI wants
		// DeleteVolume to be idempotent, so treat it as already deleted.
		return &csi.DeleteVolumeResponse{}, nil
	}

	// 1. Detach the namespace, but only if it is still attached -- check-then-act
	//    for the same reason as CreateVolume (base-nvmf error strings are not a
	//    reliable "not found" signal).
	subs, err := s.rpc.NvmfGetSubsystems(ctx)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "nvmf_get_subsystems: %v", err)
	}
	if _, present := spdkrpc.FindNSID(subs, h.NQN, h.Bdev); present {
		if err := s.rpc.NvmfSubsystemRemoveNs(ctx, h.NQN, h.NSID); err != nil && !spdkrpc.IsNotFound(err) {
			return nil, status.Errorf(codes.Internal, "nvmf_subsystem_remove_ns %s %d: %v", h.NQN, h.NSID, err)
		}
	}
	// 2. Delete the forwarder bdev (idempotent -- the kvrados bdev delete reports
	//    -ENODEV when absent, which IsNotFound recognizes).
	if err := s.rpc.BdevKvradosDelete(ctx, h.Bdev); err != nil && !spdkrpc.IsNotFound(err) {
		return nil, status.Errorf(codes.Internal, "bdev_kvrados_delete %s: %v", h.Bdev, err)
	}
	return &csi.DeleteVolumeResponse{}, nil
}

func (s *controllerServer) ControllerGetCapabilities(_ context.Context, _ *csi.ControllerGetCapabilitiesRequest) (*csi.ControllerGetCapabilitiesResponse, error) {
	rpcCap := func(t csi.ControllerServiceCapability_RPC_Type) *csi.ControllerServiceCapability {
		return &csi.ControllerServiceCapability{
			Type: &csi.ControllerServiceCapability_Rpc{
				Rpc: &csi.ControllerServiceCapability_RPC{Type: t},
			},
		}
	}
	return &csi.ControllerGetCapabilitiesResponse{
		Capabilities: []*csi.ControllerServiceCapability{
			rpcCap(csi.ControllerServiceCapability_RPC_CREATE_DELETE_VOLUME),
		},
	}, nil
}

func (s *controllerServer) ValidateVolumeCapabilities(_ context.Context, req *csi.ValidateVolumeCapabilitiesRequest) (*csi.ValidateVolumeCapabilitiesResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, status.Error(codes.InvalidArgument, "ValidateVolumeCapabilities: volume id is required")
	}
	if len(req.GetVolumeCapabilities()) == 0 {
		return nil, status.Error(codes.InvalidArgument, "ValidateVolumeCapabilities: volume capabilities are required")
	}
	if err := validateVolumeCapabilities(req.GetVolumeCapabilities()); err != nil {
		// Unsupported capability -> report not-confirmed with the reason (not an error).
		return &csi.ValidateVolumeCapabilitiesResponse{Message: err.Error()}, nil
	}
	return &csi.ValidateVolumeCapabilitiesResponse{
		Confirmed: &csi.ValidateVolumeCapabilitiesResponse_Confirmed{
			VolumeCapabilities: req.GetVolumeCapabilities(),
		},
	}, nil
}
