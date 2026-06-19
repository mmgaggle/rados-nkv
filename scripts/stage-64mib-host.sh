#!/bin/bash
#  Copyright (C) 2026 IBM, Inc.
#
#  This is free software; you can redistribute it and/or modify it under the
#  terms of the GNU Lesser General Public License version 3, as published by
#  the Free Software Foundation.  See file COPYING.
#
# Post-reboot HOST staging for the 64 MiB GPU-initiated KV capture.
#
# Run this AFTER booting grub index 1 ("Fedora 6.19.10 (GPU->vfio passthrough)").
# It brings up the host side of the one-shot:
#   vfio-pci -> vstart Ceph + kvpool -> NEW nvmf_tgt -> `rados-nkv up`
# then prints the GPU-VM launch + in-guest 64 MiB capture commands.
#
# It deliberately STOPS before launching the GPU VM: the GPU run is a one-shot
# (VM teardown leaves the Strix Halo SMU dirty -> next launch needs a host
# reboot), so launch it with eyes on it.
set -euo pipefail

CGI=/home/kyle/src/ceph-gpu-initiated
SPDK=/home/kyle/src/spdk
CEPH=/home/kyle/src/ceph
GPU_BDF=0000:bd:00.0

echo "== 1. vfio-pci binding =="
if ! lspci -nnks "$GPU_BDF" | grep -q "Kernel driver in use: vfio-pci"; then
  echo "GPU $GPU_BDF is NOT on vfio-pci. Did you boot grub index 1? Aborting." >&2
  lspci -nnks "$GPU_BDF" >&2 || true
  exit 1
fi
sudo modprobe vfio-pci 2>/dev/null || true
ls -l /dev/vfio/ || true

echo "== 2. vstart Ceph (MON=1 OSD=3) + kvpool =="
cd "$CEPH/build"
MON=1 OSD=3 ../src/vstart.sh -n -d --without-dashboard
LD_LIBRARY_PATH="$CEPH/build/lib" ./bin/ceph -c ceph.conf -k keyring osd pool create kvpool 2>/dev/null || true

echo "== 3. start NEW nvmf_tgt (64 MiB-capable build) =="
ls -la "$SPDK/build/bin/nvmf_tgt"
rm -f /tmp/cgi-spdk.sock; rm -rf /tmp/cgi-muser
cd "$SPDK"
LD_LIBRARY_PATH="$CEPH/build/lib" nohup ./build/bin/nvmf_tgt \
  -r /tmp/cgi-spdk.sock --no-huge -s 1024 > /tmp/cgi-nvmf_tgt.log 2>&1 &
disown
# wait for the rpc socket
for i in $(seq 1 50); do [ -S /tmp/cgi-spdk.sock ] && break; sleep 0.2; done
[ -S /tmp/cgi-spdk.sock ] || { echo "nvmf_tgt rpc sock never appeared"; tail /tmp/cgi-nvmf_tgt.log; exit 1; }

echo "== 4. rados-nkv up (kvdev_rados default max_value_len = 64 MiB) =="
cd "$CGI"
VFU_DIR=/tmp/cgi-muser/kv \
  scripts/rados-nkv up \
    --sock /tmp/cgi-spdk.sock \
    --vfu-dir /tmp/cgi-muser/kv \
    --conf "$CEPH/build/ceph.conf" \
    --keyring "$CEPH/build/keyring" \
    --pool kvpool --namespace gpukv
[ -S /tmp/cgi-muser/kv/cntrl ] || { echo "vfio-user cntrl sock missing"; tail /tmp/cgi-nvmf_tgt.log; exit 1; }

cat <<'NEXT'

== HOST STAGING DONE ==

Optional host-side 64 MiB pre-check (no GPU cost) BEFORE launching the VM:
  drive a >128 KiB KV store/retrieve from a host KV client (kv_host_shim / NIXL
  rados_nkv roundtrip) against /tmp/cgi-muser/kv and confirm NO
  "Too many page entries" in /tmp/cgi-nvmf_tgt.log and correct lengths.

Then launch the GPU VM (one-shot):
  /home/kyle/src/ceph-gpu-initiated/scripts/launch-gpu-vm.sh &

In the guest (ssh -p 2222 kyle@localhost, pass: password):
  # module + device
  lsmod | grep rocm_xio ; ls /dev/rocm-xio /dev/nvme0
  # if the guest xio-tester predates this work, re-scp src/{common/xio-common.hip,
  # endpoints/nvme-ep/nvme-ep.h,endpoints/nvme-ep/nvme-ep.hip} and rebuild:
  #   cmake --build build --target xio-tester

  # 64 MiB HEADLINE: retrieve into VRAM (mode-8, contiguous -> coalesces to 1 iov)
  sudo LD_LIBRARY_PATH=/opt/rocm/lib HSA_FORCE_FINE_GRAIN_PCIE=1 \
    /home/kyle/rocm-xio/build/xio-tester -m 8 --pci-mmio-bridge nvme-ep \
    --controller /dev/nvme0 --kv-op store --key gpukey64m \
    --value-size 67108864 --data-buffer-size 67108864 --write-io 1 --lfsr-seed 0xC0FFEE
  sudo LD_LIBRARY_PATH=/opt/rocm/lib HSA_FORCE_FINE_GRAIN_PCIE=1 \
    /home/kyle/rocm-xio/build/xio-tester -m 8 --pci-mmio-bridge nvme-ep \
    --controller /dev/nvme0 --kv-op retrieve --key gpukey64m \
    --value-size 67108864 --data-buffer-size 67108864 --read-io 20

Verify the object on the host:
  cd /home/kyle/src/ceph/build
  OID=$(printf gpukey64m | xxd -p)
  LD_LIBRARY_PATH=lib ./bin/rados -c ceph.conf -k keyring -p kvpool -N gpukv stat $OID
  # expect: size 67108864
NEXT
