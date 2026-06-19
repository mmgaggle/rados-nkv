#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 IBM Corporation. All rights reserved.
#
# Tear down the two-tier rados-nkvx target (leaves Ceph + the RDMA network up).
sudo -n pkill -f 'nvmf_tgt -r /tmp/nkvx/rpc.sock' 2>/dev/null || true
sudo -n pkill -x nkvx_service 2>/dev/null || true
sleep 0.5
rm -rf /tmp/nkvx
echo "two-tier rados-nkvx DOWN (Ceph + network left up)."
