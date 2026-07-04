// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package driver

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"log/slog"
	"os"
	"path/filepath"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"github.com/mmgaggle/rados-nkv/clients/csi/internal/spdkrpc"
)

// podInfoOnMount reserved volume-context keys (CSIDriver.spec.podInfoOnMount=true).
const (
	podNamespaceKey = "csi.storage.k8s.io/pod.namespace"
	podUIDKey       = "csi.storage.k8s.io/pod.uid"
	// mcsLevelKey lets a mutating webhook (or a test) inject the pod's SELinux MCS
	// level directly, until the k8s-API resolver lands (see resolveMCS).
	mcsLevelKey = "seLinuxMcsLevel"
)

var errMCSUnavailable = errors.New(
	"pod SELinux MCS level not available (inject via volume context 'seLinuxMcsLevel'; " +
		"k8s-API resolver is a follow-up)")

// nodeServer implements the CSI Node service. NodePublishVolume materializes a
// per-pod vfio-user socket on the volume's (per-k8s-namespace) subsystem, labels
// it to the pod's SELinux MCS level, and bind-mounts it into the pod.
type nodeServer struct {
	csi.UnimplementedNodeServer
	cfg Config
	rpc spdkrpc.NodeRPC
	ops nodeOps
}

func newNodeServer(cfg Config, rpc spdkrpc.NodeRPC, ops nodeOps) *nodeServer {
	return &nodeServer{cfg: cfg, rpc: rpc, ops: ops}
}

func (s *nodeServer) NodeGetInfo(_ context.Context, _ *csi.NodeGetInfoRequest) (*csi.NodeGetInfoResponse, error) {
	return &csi.NodeGetInfoResponse{NodeId: s.cfg.NodeID}, nil
}

func (s *nodeServer) NodeGetCapabilities(_ context.Context, _ *csi.NodeGetCapabilitiesRequest) (*csi.NodeGetCapabilitiesResponse, error) {
	// Socket materialization happens entirely in NodePublish; no STAGE/UNSTAGE or
	// volume stats.
	return &csi.NodeGetCapabilitiesResponse{}, nil
}

// publishState is what NodeUnpublish needs to tear down, stashed at publish time
// (NodeUnpublish gets only volume_id + target_path, no volume_context/pod info).
type publishState struct {
	NQN       string `json:"nqn"`
	SocketDir string `json:"socketDir"`
}

func (s *nodeServer) stateFile(volumeID, targetPath string) string {
	sum := sha256.Sum256([]byte(volumeID + "\x00" + targetPath))
	return filepath.Join(s.cfg.StateDir, hex.EncodeToString(sum[:16])+".json")
}

func shortVol(volumeID string) string {
	sum := sha256.Sum256([]byte(volumeID))
	return "vol-" + hex.EncodeToString(sum[:8])
}

// resolveMCS returns the pod's SELinux MCS level, or errMCSUnavailable.
func resolveMCS(volCtx map[string]string) (string, error) {
	if lvl := volCtx[mcsLevelKey]; lvl != "" {
		return lvl, nil
	}
	// TODO(spdk-csi.2 follow-up): resolve the pod's SCC-assigned MCS level via the
	// k8s API (pod securityContext.seLinuxOptions.level, or the namespace's
	// openshift.io/sa.scc.mcs annotation). Needs client-go.
	return "", errMCSUnavailable
}

func (s *nodeServer) NodePublishVolume(ctx context.Context, req *csi.NodePublishVolumeRequest) (*csi.NodePublishVolumeResponse, error) {
	volumeID := req.GetVolumeId()
	targetPath := req.GetTargetPath()
	if volumeID == "" || targetPath == "" {
		return nil, status.Error(codes.InvalidArgument, "NodePublishVolume: volume id and target path are required")
	}
	vc := req.GetVolumeCapability()
	if vc == nil {
		return nil, status.Error(codes.InvalidArgument, "NodePublishVolume: volume capability is required")
	}
	if err := validateVolumeCapabilities([]*csi.VolumeCapability{vc}); err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}

	// Already published (idempotent re-invoke)?
	if _, err := s.readState(volumeID, targetPath); err == nil {
		return &csi.NodePublishVolumeResponse{}, nil
	}

	h, err := decodeVolumeID(volumeID)
	if err != nil {
		return nil, status.Errorf(codes.InvalidArgument, "NodePublishVolume: %v", err)
	}

	volCtx := req.GetVolumeContext()
	podNS := volCtx[podNamespaceKey]
	podUID := volCtx[podUIDKey]
	if podNS == "" || podUID == "" {
		return nil, status.Error(codes.FailedPrecondition,
			"NodePublishVolume: pod namespace/uid missing (set CSIDriver podInfoOnMount=true)")
	}

	// Per-pod socket dir on the volume's (per-namespace) subsystem.
	socketDir := filepath.Join(s.cfg.MuserRoot, podNS, podUID, shortVol(volumeID))
	if err := s.ops.MkdirAll(socketDir, 0o750); err != nil {
		return nil, status.Errorf(codes.Internal, "mkdir socket dir %s: %v", socketDir, err)
	}

	if err := s.rpc.EnsureVfiouserTransport(ctx); err != nil {
		return nil, status.Errorf(codes.Internal, "ensure VFIOUSER transport: %v", err)
	}
	if err := s.rpc.NvmfSubsystemAddListener(ctx, h.NQN, socketDir); err != nil && !spdkrpc.IsAlreadyExists(err) {
		return nil, status.Errorf(codes.Internal, "add vfio-user listener %s @ %s: %v", h.NQN, socketDir, err)
	}

	// SELinux MCS: confine the socket to the pod's category. Fail closed by default.
	if level, mErr := resolveMCS(volCtx); mErr == nil {
		if err := s.ops.Chcon(socketDir, level); err != nil {
			return nil, status.Errorf(codes.Internal, "chcon socket to MCS %s: %v", level, err)
		}
	} else if s.cfg.RequireMCS {
		return nil, status.Errorf(codes.FailedPrecondition, "NodePublishVolume: %v", mErr)
	} else {
		slog.Warn("MCS labeling skipped (RequireMCS=false)", "socket", socketDir, "err", mErr)
	}

	if err := s.ops.MkdirAll(targetPath, 0o750); err != nil {
		return nil, status.Errorf(codes.Internal, "mkdir target %s: %v", targetPath, err)
	}
	if err := s.ops.BindMount(socketDir, targetPath); err != nil {
		return nil, status.Errorf(codes.Internal, "bind-mount %s -> %s: %v", socketDir, targetPath, err)
	}

	if err := s.writeState(volumeID, targetPath, publishState{NQN: h.NQN, SocketDir: socketDir}); err != nil {
		return nil, status.Errorf(codes.Internal, "persist publish state: %v", err)
	}
	slog.Info("NodePublish ok", "volume", volumeID, "nqn", h.NQN, "socket", socketDir, "target", targetPath)
	return &csi.NodePublishVolumeResponse{}, nil
}

func (s *nodeServer) NodeUnpublishVolume(ctx context.Context, req *csi.NodeUnpublishVolumeRequest) (*csi.NodeUnpublishVolumeResponse, error) {
	volumeID := req.GetVolumeId()
	targetPath := req.GetTargetPath()
	if targetPath == "" {
		return nil, status.Error(codes.InvalidArgument, "NodeUnpublishVolume: target path is required")
	}

	// Unmount first (idempotent).
	if err := s.ops.Unmount(targetPath); err != nil {
		return nil, status.Errorf(codes.Internal, "unmount %s: %v", targetPath, err)
	}

	// Tear down the per-pod socket if we recorded it (idempotent if state is gone).
	st, err := s.readState(volumeID, targetPath)
	if err == nil {
		if rErr := s.rpc.NvmfSubsystemRemoveListener(ctx, st.NQN, st.SocketDir); rErr != nil && !spdkrpc.IsNotFound(rErr) {
			return nil, status.Errorf(codes.Internal, "remove vfio-user listener %s @ %s: %v", st.NQN, st.SocketDir, rErr)
		}
		if rErr := s.ops.RemoveAll(st.SocketDir); rErr != nil {
			return nil, status.Errorf(codes.Internal, "remove socket dir %s: %v", st.SocketDir, rErr)
		}
		s.removeState(volumeID, targetPath)
	}
	slog.Info("NodeUnpublish ok", "volume", volumeID, "target", targetPath)
	return &csi.NodeUnpublishVolumeResponse{}, nil
}

func (s *nodeServer) writeState(volumeID, targetPath string, st publishState) error {
	if err := os.MkdirAll(s.cfg.StateDir, 0o750); err != nil {
		return err
	}
	b, err := json.Marshal(st)
	if err != nil {
		return err
	}
	return os.WriteFile(s.stateFile(volumeID, targetPath), b, 0o640)
}

func (s *nodeServer) readState(volumeID, targetPath string) (publishState, error) {
	var st publishState
	b, err := os.ReadFile(s.stateFile(volumeID, targetPath))
	if err != nil {
		return st, err
	}
	return st, json.Unmarshal(b, &st)
}

func (s *nodeServer) removeState(volumeID, targetPath string) {
	_ = os.Remove(s.stateFile(volumeID, targetPath))
}
