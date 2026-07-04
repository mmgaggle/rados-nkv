// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package spdkrpc

import (
	"fmt"
	"syscall"
	"testing"
)

func TestIsAlreadyExists(t *testing.T) {
	cases := []struct {
		name string
		err  error
		want bool
	}{
		{"eexist code", NewError(-int(syscall.EEXIST), "File exists"), true},
		{"message match", NewError(-32602, "namespace already exists"), true},
		{"unrelated", NewError(-int(syscall.ENOENT), "No such file"), false},
		{"wrapped", fmt.Errorf("add_ns: %w", NewError(-int(syscall.EEXIST), "x")), true},
		{"nil", nil, false},
	}
	for _, tc := range cases {
		if got := IsAlreadyExists(tc.err); got != tc.want {
			t.Errorf("%s: IsAlreadyExists=%v want %v", tc.name, got, tc.want)
		}
	}
}

func TestIsNotFound(t *testing.T) {
	cases := []struct {
		name string
		err  error
		want bool
	}{
		{"enoent", NewError(-int(syscall.ENOENT), "x"), true},
		{"enodev", NewError(-int(syscall.ENODEV), "x"), true},
		{"message match", NewError(-32602, "No such device or bdev"), true},
		{"exists", NewError(-int(syscall.EEXIST), "File exists"), false},
		{"nil", nil, false},
	}
	for _, tc := range cases {
		if got := IsNotFound(tc.err); got != tc.want {
			t.Errorf("%s: IsNotFound=%v want %v", tc.name, got, tc.want)
		}
	}
}

func TestFindNSID(t *testing.T) {
	subs := []Subsystem{
		{NQN: "nqn.a", Namespaces: []Namespace{{NSID: 1, BdevName: "KvA"}}},
		{NQN: "nqn.b", Namespaces: []Namespace{{NSID: 5, BdevName: "KvB"}, {NSID: 6, BdevName: "KvB2"}}},
	}
	if id, ok := FindNSID(subs, "nqn.b", "KvB2"); !ok || id != 6 {
		t.Errorf("FindNSID(nqn.b,KvB2)=%d,%v want 6,true", id, ok)
	}
	if _, ok := FindNSID(subs, "nqn.b", "KvA"); ok {
		t.Errorf("FindNSID should not match KvA under nqn.b")
	}
	if _, ok := FindNSID(subs, "nqn.z", "KvA"); ok {
		t.Errorf("FindNSID should not match unknown nqn")
	}
}
