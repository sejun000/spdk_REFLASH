#!/usr/bin/env python3
"""Send NVMe Get Features (FID=0x1D, FDP) via SPDK RPC"""

import sys
import os
import base64
import struct
import json

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))
from spdk.rpc.client import JSONRPCClient

CTRL_NAME = sys.argv[1] if len(sys.argv) > 1 else "cache_ctrl"
SOCK = "/var/tmp/spdk.sock"

# Build 64-byte NVMe Admin command: Get Features (opcode=0x0A), FID=0x1D (FDP)
cmd = bytearray(64)
cmd[0] = 0x0A  # opcode: Get Features
struct.pack_into('<I', cmd, 40, 0x1D)  # CDW10: FID=0x1D

cmdbuf_b64 = base64.urlsafe_b64encode(cmd).decode()

client = JSONRPCClient(SOCK)
result = client.call('bdev_nvme_send_cmd', {
    'name': CTRL_NAME,
    'cmd_type': 'admin',
    'data_direction': 'c2h',
    'cmdbuf': cmdbuf_b64,
    'data_len': 4096,
})

# Decode completion queue entry
cpl_b64 = result.get('cpl', '')
cpl_bytes = base64.urlsafe_b64decode(cpl_b64)

if len(cpl_bytes) >= 4:
    cdw0 = struct.unpack_from('<I', cpl_bytes, 0)[0]
    fdp_enabled = cdw0 & 0x1
    print(f"CDW0: 0x{cdw0:08X}")
    print(f"FDP Enabled: {'YES' if fdp_enabled else 'NO'}")
else:
    print("Unexpected response:")
    print(json.dumps(result, indent=2))
