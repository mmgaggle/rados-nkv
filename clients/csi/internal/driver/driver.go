// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

// Package driver implements the rados-nkv CSI driver: the CSI Identity and
// Controller services that provision NVMe-KV namespaces on a rados-nkv target
// over SPDK JSON-RPC.
package driver

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"os"
	"strings"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc"

	"github.com/mmgaggle/rados-nkv/clients/csi/internal/spdkrpc"
)

const (
	// DefaultDriverName is the CSI driver name (must match the CSIDriver object
	// and the StorageClass provisioner field).
	DefaultDriverName = "csi.rados-nkv.io"

	// DefaultNQN mirrors scripts/rados-nkv's default subsystem NQN.
	DefaultNQN = "nqn.2026-06.io.ceph-gpu:kv"

	// DriverVersion is the reported plugin version.
	DriverVersion = "0.1.0-dev"
)

// Config holds the driver's runtime configuration.
type Config struct {
	DriverName      string
	NodeID          string
	Endpoint        string
	RPCSock         string
	DefaultExecutor string
	DefaultNQN      string

	// Node-service config.
	MuserRoot  string // root dir for per-pod vfio-user socket dirs
	StateDir   string // where NodePublish stashes teardown state
	RequireMCS bool   // fail NodePublish closed if the pod's MCS level is unknown

	// Which services this process serves.
	EnableController bool
	EnableNode       bool
}

const (
	defaultMuserRoot = "/var/run/muser"
	defaultStateDir  = "/var/lib/rados-nkv-csi/state"
)

// Driver wires the CSI gRPC services to an SPDK JSON-RPC client.
type Driver struct {
	cfg Config
	srv *grpc.Server
	rpc *spdkrpc.Client
}

// New constructs a Driver from cfg.
func New(cfg Config) (*Driver, error) {
	if cfg.DriverName == "" {
		cfg.DriverName = DefaultDriverName
	}
	if cfg.DefaultNQN == "" {
		cfg.DefaultNQN = DefaultNQN
	}
	if cfg.MuserRoot == "" {
		cfg.MuserRoot = defaultMuserRoot
	}
	if cfg.StateDir == "" {
		cfg.StateDir = defaultStateDir
	}
	if cfg.RPCSock == "" {
		return nil, fmt.Errorf("rpc-sock is required")
	}
	if !cfg.EnableController && !cfg.EnableNode {
		return nil, fmt.Errorf("at least one of the controller or node service must be enabled")
	}
	return &Driver{
		cfg: cfg,
		rpc: spdkrpc.NewClient(cfg.RPCSock),
	}, nil
}

// Run starts the gRPC server and blocks until ctx is cancelled or Serve fails.
func (d *Driver) Run(ctx context.Context) error {
	network, addr, err := parseEndpoint(d.cfg.Endpoint)
	if err != nil {
		return err
	}
	if network == "unix" {
		if rmErr := os.Remove(addr); rmErr != nil && !os.IsNotExist(rmErr) {
			return fmt.Errorf("remove stale socket %q: %w", addr, rmErr)
		}
	}
	lis, err := net.Listen(network, addr)
	if err != nil {
		return fmt.Errorf("listen on %s://%s: %w", network, addr, err)
	}

	d.srv = grpc.NewServer(grpc.UnaryInterceptor(logInterceptor))
	csi.RegisterIdentityServer(d.srv, &identityServer{cfg: d.cfg})
	if d.cfg.EnableController {
		csi.RegisterControllerServer(d.srv, newControllerServer(d.cfg, d.rpc))
	}
	if d.cfg.EnableNode {
		csi.RegisterNodeServer(d.srv, newNodeServer(d.cfg, d.rpc, hostOps{}))
	}

	go func() {
		<-ctx.Done()
		slog.Info("shutting down gRPC server")
		d.srv.GracefulStop()
	}()

	slog.Info("rados-nkv-csi listening",
		"endpoint", d.cfg.Endpoint, "driver", d.cfg.DriverName, "version", DriverVersion)
	return d.srv.Serve(lis)
}

func parseEndpoint(ep string) (network, addr string, err error) {
	switch {
	case strings.HasPrefix(ep, "unix://"):
		return "unix", strings.TrimPrefix(ep, "unix://"), nil
	case strings.HasPrefix(ep, "tcp://"):
		return "tcp", strings.TrimPrefix(ep, "tcp://"), nil
	default:
		return "", "", fmt.Errorf("unsupported endpoint scheme (want unix:// or tcp://): %q", ep)
	}
}

func logInterceptor(ctx context.Context, req any, info *grpc.UnaryServerInfo, handler grpc.UnaryHandler) (any, error) {
	slog.Debug("grpc call", "method", info.FullMethod)
	resp, err := handler(ctx, req)
	if err != nil {
		slog.Error("grpc call failed", "method", info.FullMethod, "err", err)
	}
	return resp, err
}
