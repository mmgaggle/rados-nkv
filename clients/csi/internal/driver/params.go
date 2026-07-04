// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package driver

import (
	"fmt"
	"strconv"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

// volumeParams is the parsed StorageClass/VolumeContext parameter set
// (docs/csi-driver-plan.md sec.5). Greenlight decision 2 (spdk-k8s.1,
// 2026-07-02): the tenant->(pool,ns,cap) mapping home is these params plus a
// ConfigMap, with a documented CRD migration path -- not a CRD today.
type volumeParams struct {
	ComputeContextSeed string
	IsolationClass     string // shared | per-tenant | kata
	Transport          string // auto | socket | device
	NamespaceTenancy   string // private | shared
	CephxScope         string
	NQN                string
	Serial             string
	ExecutorEndpoint   string
	ReadOnly           bool
	MaxKeySize         uint32
	MaxValueSize       uint32
	NumKeys            uint64
}

const (
	isolationShared    = "shared"
	isolationPerTenant = "per-tenant"
	isolationKata      = "kata"

	transportAuto   = "auto"
	transportSocket = "socket"
	transportDevice = "device"

	tenancyPrivate = "private"
	tenancyShared  = "shared"

	defaultSerial = "SPDKKVR01"
)

func parseParams(p map[string]string, cfg Config) (volumeParams, error) {
	vp := volumeParams{
		ComputeContextSeed: p["computeContextSeed"],
		IsolationClass:     valueOr(p, "isolationClass", isolationPerTenant),
		Transport:          valueOr(p, "transport", transportAuto),
		NamespaceTenancy:   valueOr(p, "namespaceTenancy", tenancyPrivate),
		CephxScope:         p["cephxScope"],
		NQN:                valueOr(p, "subsystemNqn", cfg.DefaultNQN),
		Serial:             valueOr(p, "subsystemSerial", defaultSerial),
		ExecutorEndpoint:   valueOr(p, "executorEndpoint", cfg.DefaultExecutor),
		ReadOnly:           p["readOnly"] == "true",
	}

	switch vp.IsolationClass {
	case isolationShared, isolationPerTenant, isolationKata:
	default:
		return vp, fmt.Errorf("invalid isolationClass %q (want shared|per-tenant|kata)", vp.IsolationClass)
	}
	switch vp.Transport {
	case transportAuto, transportSocket, transportDevice:
	default:
		return vp, fmt.Errorf("invalid transport %q (want auto|socket|device)", vp.Transport)
	}
	switch vp.NamespaceTenancy {
	case tenancyPrivate, tenancyShared:
	default:
		return vp, fmt.Errorf("invalid namespaceTenancy %q (want private|shared)", vp.NamespaceTenancy)
	}

	n, err := parseOptUint(p, "maxKeySize", 32)
	if err != nil {
		return vp, err
	}
	vp.MaxKeySize = uint32(n)

	n, err = parseOptUint(p, "maxValueSize", 32)
	if err != nil {
		return vp, err
	}
	vp.MaxValueSize = uint32(n)

	vp.NumKeys, err = parseOptUint(p, "numKeys", 64)
	if err != nil {
		return vp, err
	}

	return vp, nil
}

func valueOr(m map[string]string, k, def string) string {
	if v, ok := m[k]; ok && v != "" {
		return v
	}
	return def
}

func parseOptUint(m map[string]string, key string, bits int) (uint64, error) {
	v, ok := m[key]
	if !ok || v == "" {
		return 0, nil
	}
	n, err := strconv.ParseUint(v, 10, bits)
	if err != nil {
		return 0, fmt.Errorf("invalid %s %q: %w", key, v, err)
	}
	return n, nil
}

// validateVolumeCapabilities enforces the KV carrier invariant: Filesystem
// (mount) only, never Block. KV never enumerates as a kernel block device; the
// pod always drives userspace via vfio/NVMe-KV (docs/csi-driver-plan.md sec.5).
func validateVolumeCapabilities(caps []*csi.VolumeCapability) error {
	for _, c := range caps {
		if c.GetBlock() != nil {
			return fmt.Errorf("volumeMode=Block is not supported: KV never enumerates as a kernel block device; use Filesystem")
		}
		if c.GetMount() == nil {
			return fmt.Errorf("only the Filesystem (mount) volume capability is supported")
		}
	}
	return nil
}

// capacityToQuota maps the CSI capacity request to a quota (bytes). Capacity is
// a quota, not an allocation: KV namespaces grow in RADOS
// (docs/csi-driver-plan.md sec.5). Returns 0 when unspecified.
func capacityToQuota(cr *csi.CapacityRange) int64 {
	if cr == nil {
		return 0
	}
	if req := cr.GetRequiredBytes(); req > 0 {
		return req
	}
	return cr.GetLimitBytes()
}
