#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
RPC_BIN=${RPC_BIN:-"$ROOT_DIR/scripts/rpc.py"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
UBLK_DEV_ID=${UBLK_DEV_ID:-0}
FTL_NAME=${FTL_NAME:-ftl0}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:${FTL_NAME}}

RPC=("$RPC_BIN" "-s" "$RPC_SOCKET")

# Check if spdk_tgt is running
pids=$(pgrep -f spdk_tgt || true)

# Exit early if spdk_tgt not running
#if [[ -z "$pids" ]]; then
#    echo "[exit_ftl] spdk_tgt not running"
#    exit 0
#fi

# Disconnect NVMe-oF sessions first (primary method)
if command -v nvme >/dev/null 2>&1; then
    echo "[exit_ftl] disconnecting NVMe sessions for ${NVMF_SUBSYSTEM}"
    sudo nvme disconnect -n "${NVMF_SUBSYSTEM}" 2>/dev/null || true
    sleep 1
fi

# Stop ublk device (fallback method)
if [[ -e "/dev/ublkb${UBLK_DEV_ID}" ]]; then
    echo "[exit_ftl] stopping ublk device /dev/ublkb${UBLK_DEV_ID}"
    "${RPC[@]}" ublk_stop_disk "${FTL_NAME}" 2>/dev/null || true
    sleep 1
fi

# Delete FTL bdev with fast shutdown
echo "[exit_ftl] deleting FTL bdev ${FTL_NAME}"
"${RPC[@]}" bdev_ftl_delete -b "${FTL_NAME}" -f 2>/dev/null || true
sleep 1

# Kill spdk_tgt
echo "$pids" | xargs -r sudo kill -TERM
sleep 1
remaining=$(pgrep -f spdk_tgt || true)
if [[ -n "$remaining" ]]; then
    echo "$remaining" | xargs -r sudo kill -KILL
fi
echo "[exit_ftl] killed spdk_tgt processes: $pids"

# Cleanup ublk device (force delete if stuck)
if command -v ublk >/dev/null 2>&1; then
    echo "[exit_ftl] cleaning up ublk device ${UBLK_DEV_ID}"
    sudo ublk del -n "${UBLK_DEV_ID}" 2>/dev/null || true
fi

# Reload ublk module if needed
if lsmod | grep -q ublk_drv; then
    echo "[exit_ftl] reloading ublk_drv module"
    sudo rmmod ublk_drv 2>/dev/null || true
    sudo modprobe ublk_drv 2>/dev/null || true
fi
