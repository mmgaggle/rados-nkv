// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package spdkrpc

import "context"

// BdevKvradosCreateOpts mirrors the decoder field names in
// target/bdev_kvrados_rpc.c (name, executor_endpoint, read_only, max_key_size,
// max_value_size, num_keys). bdev_kvrados is a pure forwarder, so an executor
// endpoint is always required.
type BdevKvradosCreateOpts struct {
	Name             string
	ExecutorEndpoint string
	ReadOnly         bool
	MaxKeySize       uint32
	MaxValueSize     uint32
	NumKeys          uint64
}

// BdevKvradosCreate creates the RADOS-backed KV forwarder bdev.
func (c *Client) BdevKvradosCreate(ctx context.Context, o BdevKvradosCreateOpts) error {
	params := map[string]any{"name": o.Name}
	if o.ExecutorEndpoint != "" {
		params["executor_endpoint"] = o.ExecutorEndpoint
	}
	if o.ReadOnly {
		params["read_only"] = true
	}
	if o.MaxKeySize > 0 {
		params["max_key_size"] = o.MaxKeySize
	}
	if o.MaxValueSize > 0 {
		params["max_value_size"] = o.MaxValueSize
	}
	if o.NumKeys > 0 {
		params["num_keys"] = o.NumKeys
	}
	var name string // result is the bdev name; not needed by callers
	return c.call(ctx, "bdev_kvrados_create", params, &name)
}

// BdevKvradosDelete removes a KV forwarder bdev.
func (c *Client) BdevKvradosDelete(ctx context.Context, name string) error {
	return c.call(ctx, "bdev_kvrados_delete", map[string]any{"name": name}, nil)
}

// NvmfCreateSubsystem creates an NVMe-oF subsystem (allow-any-host), idempotent
// at the caller. Mirrors `nvmf_create_subsystem <nqn> -s <serial> -a`.
func (c *Client) NvmfCreateSubsystem(ctx context.Context, nqn, serial string) error {
	params := map[string]any{
		"nqn":            nqn,
		"allow_any_host": true,
	}
	if serial != "" {
		params["serial_number"] = serial
	}
	return c.call(ctx, "nvmf_create_subsystem", params, nil)
}

// NvmfSubsystemAddNs attaches bdev as a namespace under nqn and returns the
// assigned NSID (CSI=KV is auto-detected from the kvrados bdev).
func (c *Client) NvmfSubsystemAddNs(ctx context.Context, nqn, bdev string) (uint32, error) {
	params := map[string]any{
		"nqn":       nqn,
		"namespace": map[string]any{"bdev_name": bdev},
	}
	var nsid uint32
	err := c.call(ctx, "nvmf_subsystem_add_ns", params, &nsid)
	return nsid, err
}

// NvmfSubsystemRemoveNs detaches the namespace nsid from nqn.
func (c *Client) NvmfSubsystemRemoveNs(ctx context.Context, nqn string, nsid uint32) error {
	params := map[string]any{"nqn": nqn, "nsid": nsid}
	return c.call(ctx, "nvmf_subsystem_remove_ns", params, nil)
}

// Namespace is one entry of a subsystem's namespace list.
type Namespace struct {
	NSID     uint32 `json:"nsid"`
	BdevName string `json:"bdev_name"`
}

// Subsystem is a subset of an nvmf_get_subsystems entry.
type Subsystem struct {
	NQN        string      `json:"nqn"`
	Namespaces []Namespace `json:"namespaces"`
}

// NvmfGetSubsystems returns the target's subsystems and their namespaces.
func (c *Client) NvmfGetSubsystems(ctx context.Context) ([]Subsystem, error) {
	var subs []Subsystem
	err := c.call(ctx, "nvmf_get_subsystems", nil, &subs)
	return subs, err
}

// HasSubsystem reports whether nqn is present in a subsystem listing. Used for
// idempotent check-then-act: base-nvmf reports "already exists" as a generic
// INTERNAL_ERROR whose message does not contain "exist" (verified against a live
// target), so idempotency keys off the listing, not off create error strings.
func HasSubsystem(subs []Subsystem, nqn string) bool {
	for _, s := range subs {
		if s.NQN == nqn {
			return true
		}
	}
	return false
}

// FindNSID resolves the NSID of bdev under nqn from a subsystem listing. Used to
// make nvmf_subsystem_add_ns idempotent (recover the NSID after an EEXIST).
func FindNSID(subs []Subsystem, nqn, bdev string) (uint32, bool) {
	for _, s := range subs {
		if s.NQN != nqn {
			continue
		}
		for _, ns := range s.Namespaces {
			if ns.BdevName == bdev {
				return ns.NSID, true
			}
		}
	}
	return 0, false
}
