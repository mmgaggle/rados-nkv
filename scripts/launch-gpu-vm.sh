#!/bin/bash
#  Copyright (C) 2026 IBM, Inc.
#
#  This is free software; you can redistribute it and/or modify it under the
#  terms of the GNU Lesser General Public License version 3, as published by
#  the Free Software Foundation.  See file COPYING.
#
# Headless GPU-passthrough VM: GPU bd:00.0 (vfio-pci) + pci-mmio-bridge +
# SPDK vfio-user NVMe-KV controller. poll-interval-ns=10000 (10us, fast poll).
#
# Persistent copy of the proven one-shot launch (was /tmp/cgi-launch-gpu-vm.sh,
# which /tmp clears on reboot). Run AFTER scripts/stage-64mib-host.sh has brought
# up vstart + the (new) nvmf_tgt + `rados-nkv up` so /tmp/cgi-muser/kv/cntrl exists.
exec /home/kyle/src/qemu-xio/build/qemu-system-x86_64 \
  -machine q35,accel=kvm -cpu EPYC -smp cpus=16 -m 32768 \
  -display none -serial file:/tmp/cgi-gpu-vm-console.log -monitor none -no-reboot \
  -device pcie-root-port,id=pcie.1,chassis=1 \
  -device vfio-pci,bus=pcie.1,host=0000:bd:00.0 \
  -device pci-mmio-bridge,id=mmio-bridge,shadow-gpa=0x80000000,shadow-size=8192,poll-interval-ns=10000,addr=8.0 \
  -trace enable=pci_mmio_* \
  -object memory-backend-memfd,id=mem-vfio-user,size=32768M,share=on \
  -numa node,memdev=mem-vfio-user \
  -device pcie-root-port,id=pcie-vfu.1,chassis=2 \
  -device '{"driver":"vfio-user-pci","id":"vfukv","bus":"pcie-vfu.1","socket":{"path":"/tmp/cgi-muser/kv/cntrl","type":"unix"}}' \
  -drive if=virtio,format=qcow2,file=/home/kyle/src/rocm-xio/build/vm-images/rocm-xio-vm.qcow2 \
  -netdev user,id=net0,hostfwd=tcp::2222-:22 \
  -device virtio-net-pci,netdev=net0 \
  -qmp unix:/tmp/cgi-qmp.sock,server,nowait
