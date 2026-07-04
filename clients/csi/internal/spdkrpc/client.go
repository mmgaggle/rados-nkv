// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

// Package spdkrpc is a small SPDK JSON-RPC 2.0 client over the target's unix
// socket, plus the typed rados-nkv provisioning verbs the CSI controller drives.
//
// Greenlight decision 1 (spdk-k8s.1, 2026-07-02): the control API between the
// CSI plugins and the rados-nkv front is SPDK JSON-RPC (not gRPC), reusing the
// verbs the target already registers (target/bdev_kvrados_rpc.c + base nvmf).
package spdkrpc

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"strings"
	"sync/atomic"
	"syscall"
	"time"
)

// RPC is the subset of SPDK JSON-RPC operations the CSI controller needs.
// It is an interface so the controller can be unit-tested with a fake.
type RPC interface {
	BdevKvradosCreate(ctx context.Context, opts BdevKvradosCreateOpts) error
	BdevKvradosDelete(ctx context.Context, name string) error
	NvmfCreateSubsystem(ctx context.Context, nqn, serial string) error
	NvmfSubsystemAddNs(ctx context.Context, nqn, bdev string) (uint32, error)
	NvmfSubsystemRemoveNs(ctx context.Context, nqn string, nsid uint32) error
	NvmfGetSubsystems(ctx context.Context) ([]Subsystem, error)
}

// Client speaks SPDK JSON-RPC 2.0 over the target's unix socket (default
// /var/run/spdk.sock). It dials one connection per call, which keeps id
// matching trivial and mirrors how rpc.py drives short provisioning sequences.
type Client struct {
	sock    string
	timeout time.Duration
	id      atomic.Uint64
}

// NewClient returns a Client bound to the given unix socket path.
func NewClient(sock string) *Client {
	return &Client{sock: sock, timeout: 30 * time.Second}
}

var _ RPC = (*Client)(nil)

type rpcRequest struct {
	JSONRPC string `json:"jsonrpc"`
	ID      uint64 `json:"id"`
	Method  string `json:"method"`
	Params  any    `json:"params,omitempty"`
}

// Error is an SPDK JSON-RPC error object. SPDK reports failures as a negative
// errno in Code (e.g. -17 EEXIST) with strerror text in Message; base-nvmf RPCs
// may instead use the JSON-RPC INVALID_PARAMS code with a descriptive message,
// so IsAlreadyExists / IsNotFound fall back to matching Message.
type Error struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func (e *Error) Error() string { return fmt.Sprintf("spdk rpc error %d: %s", e.Code, e.Message) }

// NewError builds an SPDK-style JSON-RPC error. Exported for tests and callers
// that synthesize target error codes.
func NewError(code int, message string) error { return &Error{Code: code, Message: message} }

type rpcResponse struct {
	Result json.RawMessage `json:"result"`
	Error  *Error          `json:"error"`
}

func (c *Client) call(ctx context.Context, method string, params, result any) error {
	var d net.Dialer
	conn, err := d.DialContext(ctx, "unix", c.sock)
	if err != nil {
		return fmt.Errorf("dial %s: %w", c.sock, err)
	}
	defer conn.Close()

	deadline := time.Now().Add(c.timeout)
	if dl, ok := ctx.Deadline(); ok && dl.Before(deadline) {
		deadline = dl
	}
	_ = conn.SetDeadline(deadline)

	req := rpcRequest{JSONRPC: "2.0", ID: c.id.Add(1), Method: method, Params: params}
	if err := json.NewEncoder(conn).Encode(&req); err != nil {
		return fmt.Errorf("encode %s: %w", method, err)
	}

	var resp rpcResponse
	if err := json.NewDecoder(conn).Decode(&resp); err != nil {
		return fmt.Errorf("decode %s response: %w", method, err)
	}
	if resp.Error != nil {
		return resp.Error
	}
	if result != nil && len(resp.Result) > 0 {
		if err := json.Unmarshal(resp.Result, result); err != nil {
			return fmt.Errorf("unmarshal %s result: %w", method, err)
		}
	}
	return nil
}

// IsAlreadyExists reports whether err is an SPDK "already exists" failure.
func IsAlreadyExists(err error) bool {
	var re *Error
	if !errors.As(err, &re) {
		return false
	}
	if re.Code == -int(syscall.EEXIST) {
		return true
	}
	return strings.Contains(strings.ToLower(re.Message), "exist")
}

// IsNotFound reports whether err is an SPDK "not found" failure.
func IsNotFound(err error) bool {
	var re *Error
	if !errors.As(err, &re) {
		return false
	}
	switch re.Code {
	case -int(syscall.ENOENT), -int(syscall.ENODEV):
		return true
	}
	m := strings.ToLower(re.Message)
	return strings.Contains(m, "no such") ||
		strings.Contains(m, "not found") ||
		strings.Contains(m, "does not exist")
}
