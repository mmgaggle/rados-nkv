// SPDX-License-Identifier: BSD-3-Clause
// Copyright (C) 2026 IBM, Inc.

package driver

import (
	"context"
	"testing"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

func TestIdentity_GetPluginInfo(t *testing.T) {
	id := &identityServer{cfg: testConfig()}
	resp, err := id.GetPluginInfo(context.Background(), &csi.GetPluginInfoRequest{})
	if err != nil {
		t.Fatalf("GetPluginInfo: %v", err)
	}
	if resp.GetName() != DefaultDriverName {
		t.Errorf("name: got %q want %q", resp.GetName(), DefaultDriverName)
	}
	if resp.GetVendorVersion() != DriverVersion {
		t.Errorf("version: got %q want %q", resp.GetVendorVersion(), DriverVersion)
	}
}

func TestIdentity_Capabilities(t *testing.T) {
	id := &identityServer{cfg: testConfig()}
	resp, err := id.GetPluginCapabilities(context.Background(), &csi.GetPluginCapabilitiesRequest{})
	if err != nil {
		t.Fatalf("GetPluginCapabilities: %v", err)
	}
	var hasController bool
	for _, c := range resp.GetCapabilities() {
		if c.GetService().GetType() == csi.PluginCapability_Service_CONTROLLER_SERVICE {
			hasController = true
		}
	}
	if !hasController {
		t.Errorf("expected CONTROLLER_SERVICE capability")
	}

	probe, err := id.Probe(context.Background(), &csi.ProbeRequest{})
	if err != nil {
		t.Fatalf("Probe: %v", err)
	}
	if !probe.GetReady().GetValue() {
		t.Errorf("Probe should report ready")
	}
}

func TestController_Capabilities(t *testing.T) {
	cs := newControllerServer(testConfig(), newFakeRPC())
	resp, err := cs.ControllerGetCapabilities(context.Background(), &csi.ControllerGetCapabilitiesRequest{})
	if err != nil {
		t.Fatalf("ControllerGetCapabilities: %v", err)
	}
	var hasCreateDelete bool
	for _, c := range resp.GetCapabilities() {
		if c.GetRpc().GetType() == csi.ControllerServiceCapability_RPC_CREATE_DELETE_VOLUME {
			hasCreateDelete = true
		}
	}
	if !hasCreateDelete {
		t.Errorf("expected CREATE_DELETE_VOLUME capability")
	}
}

func TestController_ValidateVolumeCapabilities(t *testing.T) {
	cs := newControllerServer(testConfig(), newFakeRPC())

	// Filesystem is confirmed.
	ok, err := cs.ValidateVolumeCapabilities(context.Background(), &csi.ValidateVolumeCapabilitiesRequest{
		VolumeId:           encodeVolumeID("kv-x", 1, DefaultNQN),
		VolumeCapabilities: mountCaps(),
	})
	if err != nil {
		t.Fatalf("ValidateVolumeCapabilities(mount): %v", err)
	}
	if ok.GetConfirmed() == nil {
		t.Errorf("Filesystem capability should be confirmed")
	}

	// Block is not confirmed (message set, no Confirmed).
	blk, err := cs.ValidateVolumeCapabilities(context.Background(), &csi.ValidateVolumeCapabilitiesRequest{
		VolumeId: encodeVolumeID("kv-x", 1, DefaultNQN),
		VolumeCapabilities: []*csi.VolumeCapability{{
			AccessType: &csi.VolumeCapability_Block{Block: &csi.VolumeCapability_BlockVolume{}},
		}},
	})
	if err != nil {
		t.Fatalf("ValidateVolumeCapabilities(block): %v", err)
	}
	if blk.GetConfirmed() != nil {
		t.Errorf("Block capability must not be confirmed")
	}
	if blk.GetMessage() == "" {
		t.Errorf("Block rejection should carry a reason message")
	}
}
