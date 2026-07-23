#!/usr/bin/env python3
"""Read NVMe SMART log Host Written via SPDK RPC passthrough"""

import sys
import os
import base64
import struct

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))
from spdk.rpc.client import JSONRPCClient

CTRL_NAME = sys.argv[1] if len(sys.argv) > 1 else "cache_ctrl"
SOCK = "/var/tmp/spdk.sock"

# SMART log: LID=0x02, read full 512 bytes
LID = 0x02
SIZE = 512
NUMD = (SIZE // 4) - 1

cmd = bytearray(64)
cmd[0] = 0x02  # opcode: Get Log Page
struct.pack_into('<I', cmd, 4, 0xFFFFFFFF)        # NSID
cdw10 = LID | ((NUMD & 0xFFFF) << 16)
struct.pack_into('<I', cmd, 40, cdw10)
cdw11 = (NUMD >> 16) & 0xFFFF
struct.pack_into('<I', cmd, 44, cdw11)

cmdbuf_b64 = base64.urlsafe_b64encode(cmd).decode()

client = JSONRPCClient(SOCK)
result = client.call('bdev_nvme_send_cmd', {
    'name': CTRL_NAME,
    'cmd_type': 'admin',
    'data_direction': 'c2h',
    'cmdbuf': cmdbuf_b64,
    'data_len': SIZE,
})

cpl_b64 = result.get('cpl', '')
cpl_bytes = base64.urlsafe_b64decode(cpl_b64)
if len(cpl_bytes) >= 8:
    status = struct.unpack_from('<H', cpl_bytes, 14)[0] >> 1
    if status != 0:
        print(f"Command failed, status: 0x{status:04X}")
        sys.exit(1)

data = base64.urlsafe_b64decode(result.get('data', ''))

# SMART log offsets (128-bit / 16-byte values, use lower 8 bytes)
# Offset 32: Data Units Read
# Offset 48: Data Units Written (host written)
# Unit: 1000 x 512 bytes = 512KB per unit

units_read = struct.unpack_from('<Q', data, 32)[0]
units_written = struct.unpack_from('<Q', data, 48)[0]

read_bytes = units_read * 512000
written_bytes = units_written * 512000

print(f"Data Units Read:    {units_read} ({read_bytes / 1e9:.2f} GB)")
print(f"Data Units Written: {units_written} ({written_bytes / 1e9:.2f} GB)  <-- Host Written")
