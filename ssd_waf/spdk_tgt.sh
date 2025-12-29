#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
APP=${SPDK_TGT_APP:-"$ROOT_DIR/build/bin/spdk_tgt"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
LOG_FILE=${SPDK_TGT_LOG:-/tmp/spdk_tgt.log}
# Use cores 0-8: TCP on cores 0-7, log_worker on core 8
CPU_MASK=${SPDK_TGT_CPUMASK:-0x1FF}

if [[ ! -x "$APP" ]]; then
    echo "spdk_tgt binary not found at $APP" >&2
    exit 1
fi

# Enable core dump
ulimit -c unlimited

# Use tcmalloc if available
TCMALLOC_LIB="/usr/lib/x86_64-linux-gnu/libtcmalloc.so"
if [[ -f "$TCMALLOC_LIB" ]]; then
    export LD_PRELOAD="$TCMALLOC_LIB"
    echo "[spdk_tgt] using tcmalloc: $TCMALLOC_LIB"
fi

echo "[spdk_tgt] starting $APP (cpu mask ${CPU_MASK}, rpc socket ${RPC_SOCKET})"
exec "$APP" -m "$CPU_MASK" -r "${RPC_SOCKET}" -L icache &>> "$LOG_FILE"
