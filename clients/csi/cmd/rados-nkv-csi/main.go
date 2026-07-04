// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

// Command rados-nkv-csi is the CSI driver for NVMe-KV-on-RADOS (rados-nkv).
//
// This binary currently hosts the CSI Identity + Controller services
// (bead spdk-csi.1): it provisions per-tenant NVMe-KV namespaces on a rados-nkv
// target over SPDK JSON-RPC. The Node service (spdk-csi.2), which materializes
// the per-tenant vfio-user socket into the pod, is added later.
//
// Greenlight (spdk-k8s.1, 2026-07-02): control API = SPDK JSON-RPC; tenant
// mapping = StorageClass params + ConfigMap. See docs/csi-driver-plan.md.
package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"os"
	"os/signal"
	"syscall"

	"github.com/mmgaggle/rados-nkv/clients/csi/internal/driver"
)

func main() {
	var (
		endpoint = flag.String("endpoint", "unix:///csi/csi.sock", "CSI gRPC endpoint the sidecars connect to")
		nodeID   = flag.String("nodeid", "", "node identifier (required for the node service; optional for controller-only)")
		rpcSock  = flag.String("rpc-sock", "/var/run/spdk.sock", "rados-nkv target SPDK JSON-RPC unix socket")
		drvName  = flag.String("drivername", driver.DefaultDriverName, "CSI driver name")
		execEP   = flag.String("executor-endpoint", "", "default rados-nkvx executor NA address for bdev_kvrados_create (StorageClass param overrides)")
		defNQN   = flag.String("default-nqn", driver.DefaultNQN, "default NVMe subsystem NQN when the PVC k8s namespace is unknown")
		ctrlSvc  = flag.Bool("controller-service", true, "serve the CSI controller service (run as the controller Deployment)")
		nodeSvc  = flag.Bool("node-service", false, "serve the CSI node service (run as the node DaemonSet)")
		muser    = flag.String("muser-root", "/var/run/muser", "root dir for per-pod vfio-user socket dirs (node service)")
		stateDir = flag.String("state-dir", "/var/lib/rados-nkv-csi/state", "dir where NodePublish stashes teardown state (node service)")
		reqMCS   = flag.Bool("require-mcs", true, "fail NodePublish closed if the pod's SELinux MCS level is unknown (node service)")
		debug    = flag.Bool("debug", false, "enable debug logging")
		showVer  = flag.Bool("version", false, "print version and exit")
	)
	flag.Parse()

	if *showVer {
		fmt.Println(driver.DriverVersion)
		return
	}

	lvl := slog.LevelInfo
	if *debug {
		lvl = slog.LevelDebug
	}
	slog.SetDefault(slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: lvl})))

	d, err := driver.New(driver.Config{
		DriverName:       *drvName,
		NodeID:           *nodeID,
		Endpoint:         *endpoint,
		RPCSock:          *rpcSock,
		DefaultExecutor:  *execEP,
		DefaultNQN:       *defNQN,
		MuserRoot:        *muser,
		StateDir:         *stateDir,
		RequireMCS:       *reqMCS,
		EnableController: *ctrlSvc,
		EnableNode:       *nodeSvc,
	})
	if err != nil {
		slog.Error("failed to construct driver", "err", err)
		os.Exit(1)
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	if err := d.Run(ctx); err != nil {
		slog.Error("driver exited with error", "err", err)
		os.Exit(1)
	}
}
