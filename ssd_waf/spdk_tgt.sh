#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
APP=${SPDK_TGT_APP:-"$ROOT_DIR/build/bin/spdk_tgt"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
LOG_FILE=${SPDK_TGT_LOG:-/tmp/spdk_tgt.log}
CPU_MASK=${SPDK_TGT_CPUMASK:-0x2}

if [[ ! -x "$APP" ]]; then
    echo "spdk_tgt binary not found at $APP" >&2
    exit 1
fi

# Enable core dump
ulimit -c unlimited

echo "[spdk_tgt] starting $APP (cpu mask ${CPU_MASK}, rpc socket ${RPC_SOCKET})"
exec "$APP" -m "$CPU_MASK" -r "${RPC_SOCKET}" &>> "$LOG_FILE"
