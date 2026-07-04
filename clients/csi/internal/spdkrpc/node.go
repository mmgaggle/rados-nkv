// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package spdkrpc

import "context"

// NodeRPC is the subset of SPDK JSON-RPC operations the CSI node plugin drives:
// materializing a per-pod vfio-user listener (socket) on the volume's
// (per-k8s-namespace) subsystem. Separate from the controller's RPC surface so
// each side can be faked independently.
type NodeRPC interface {
	EnsureVfiouserTransport(ctx context.Context) error
	NvmfSubsystemAddListener(ctx context.Context, nqn, traddr string) error
	NvmfSubsystemRemoveListener(ctx context.Context, nqn, traddr string) error
}

var _ NodeRPC = (*Client)(nil)

// EnsureVfiouserTransport creates the VFIOUSER transport if absent (idempotent).
func (c *Client) EnsureVfiouserTransport(ctx context.Context) error {
	err := c.call(ctx, "nvmf_create_transport", map[string]any{"trtype": "VFIOUSER"}, nil)
	if err != nil && !IsAlreadyExists(err) {
		return err
	}
	return nil
}

func vfiouserListenAddress(traddr string) map[string]any {
	// Mirrors `nvmf_subsystem_add_listener <nqn> -t VFIOUSER -a <dir> -s 0`.
	return map[string]any{"trtype": "VFIOUSER", "traddr": traddr, "trsvcid": "0"}
}

// NvmfSubsystemAddListener adds a VFIOUSER listener for nqn at the socket dir
// traddr (creates <traddr>/cntrl).
func (c *Client) NvmfSubsystemAddListener(ctx context.Context, nqn, traddr string) error {
	params := map[string]any{"nqn": nqn, "listen_address": vfiouserListenAddress(traddr)}
	return c.call(ctx, "nvmf_subsystem_add_listener", params, nil)
}

// NvmfSubsystemRemoveListener removes the VFIOUSER listener for nqn at traddr.
func (c *Client) NvmfSubsystemRemoveListener(ctx context.Context, nqn, traddr string) error {
	params := map[string]any{"nqn": nqn, "listen_address": vfiouserListenAddress(traddr)}
	return c.call(ctx, "nvmf_subsystem_remove_listener", params, nil)
}
