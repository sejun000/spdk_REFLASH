#!/usr/bin/env python3
"""Send NVMe Get Log Page (LID=208/0xD0, offset=60, size=16) via SPDK RPC"""

import sys
import os
import base64
import struct

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))
from spdk.rpc.client import JSONRPCClient

CTRL_NAME = sys.argv[1] if len(sys.argv) > 1 else "cache_ctrl"
SOCK = "/var/tmp/spdk.sock"

LID = 208        # 0xD0
OFFSET = 60      # byte offset within log page
SIZE = 16        # bytes to read
NUMD = (SIZE // 4) - 1  # number of dwords minus 1

# Build 64-byte NVMe Admin command: Get Log Page (opcode=0x02)
cmd = bytearray(64)
cmd[0] = 0x02                                          # opcode: Get Log Page
struct.pack_into('<I', cmd, 4, 0xFFFFFFFF)             # NSID
# CDW10: LID[7:0] | LSP[15:8] | NUMDL[27:16]
cdw10 = LID | ((NUMD & 0xFFFF) << 16)
struct.pack_into('<I', cmd, 40, cdw10)
# CDW11: NUMDU[15:0]
cdw11 = (NUMD >> 16) & 0xFFFF
struct.pack_into('<I', cmd, 44, cdw11)
# CDW12: Log Page Offset Lower
struct.pack_into('<I', cmd, 48, OFFSET)
# CDW13: Log Page Offset Upper
struct.pack_into('<I', cmd, 52, 0)

cmdbuf_b64 = base64.urlsafe_b64encode(cmd).decode()

client = JSONRPCClient(SOCK)
result = client.call('bdev_nvme_send_cmd', {
    'name': CTRL_NAME,
    'cmd_type': 'admin',
    'data_direction': 'c2h',
    'cmdbuf': cmdbuf_b64,
    'data_len': SIZE,
})

# Decode response
cpl_b64 = result.get('cpl', '')
cpl_bytes = base64.urlsafe_b64decode(cpl_b64)
if len(cpl_bytes) >= 8:
    cdw0 = struct.unpack_from('<I', cpl_bytes, 0)[0]
    status = struct.unpack_from('<H', cpl_bytes, 14)[0] >> 1
    if status != 0:
        print(f"Command failed, status: 0x{status:04X}")
        sys.exit(1)

data_b64 = result.get('data', '')
data = base64.urlsafe_b64decode(data_b64)

print(f"Log Page {LID} (0x{LID:02X}), offset={OFFSET}, size={SIZE}")
print(f"Raw hex: {data.hex()}")
print(f"Raw bytes: {list(data)}")

# Hex dump byte by byte
print("Byte dump:")
for i in range(len(data)):
    addr = OFFSET + i
    print(f"  [0x{addr:02X}] = 0x{data[i]:02X} ({data[i]:3d})")

# Parse as uint32 and uint64
if len(data) >= 16:
    for i in range(0, 16, 4):
        val32 = struct.unpack_from('<I', data, i)[0]
        print(f"  uint32 @ 0x{OFFSET+i:02X}: {val32} (0x{val32:08X})")
    for i in range(0, 16, 8):
        val64 = struct.unpack_from('<Q', data, i)[0]
        print(f"  uint64 @ 0x{OFFSET+i:02X}: {val64} (0x{val64:016X})")
