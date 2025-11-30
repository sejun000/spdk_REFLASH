#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
RPC_BIN=${RPC_BIN:-"$ROOT_DIR/scripts/rpc.py"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:icache0}

if [[ ! -x "$RPC_BIN" ]]; then
    echo "RPC tool not found at $RPC_BIN" >&2
    exit 1
fi

if command -v nvme >/dev/null 2>&1; then
    echo "[exit_tgt] disconnecting NVMe sessions for ${NVMF_SUBSYSTEM}"
    if ! sudo nvme disconnect -n "${NVMF_SUBSYSTEM}"; then
        echo "[exit_tgt] nvme disconnect failed (possibly not connected)"
    fi
fi

pids=$(pgrep -f spdk_tgt || true)
if [[ -z "$pids" ]]; then
    echo "[exit_tgt] spdk_tgt not running" >&2
    exit 0
fi

echo "$pids" | xargs -r sudo kill -TERM
sleep 1
remaining=$(pgrep -f spdk_tgt || true)
if [[ -n "$remaining" ]]; then
    echo "$remaining" | xargs -r sudo kill -KILL
fi
echo "[exit_tgt] killed spdk_tgt processes: $pids"
