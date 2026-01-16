#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
RPC_BIN=${RPC_BIN:-"$ROOT_DIR/scripts/rpc.py"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
RPC=("$RPC_BIN" "-s" "$RPC_SOCKET")

OCF_NAME=${OCF_NAME:-ocf0}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:${OCF_NAME}}
UBLK_DEV_ID=${UBLK_DEV_ID:-0}

log() {
    echo "[exit_ocf] $*"
}

# Disconnect NVMe-oF
disconnect_nvmf() {
    if command -v nvme >/dev/null 2>&1; then
        log "Disconnecting NVMe-oF..."
        sudo nvme disconnect -n "${NVMF_SUBSYSTEM}" 2>/dev/null || true
    fi
}

# Stop ublk device
stop_ublk() {
    if [[ -e "/dev/ublkb${UBLK_DEV_ID}" ]]; then
        log "Stopping ublk device..."
        sudo "${RPC[@]}" ublk_stop_disk "${UBLK_DEV_ID}" 2>/dev/null || true
        sleep 1
    fi
}

# Delete OCF bdev
delete_ocf() {
    if [[ -S "$RPC_SOCKET" ]]; then
        log "Deleting OCF bdev ${OCF_NAME}..."
        sudo "${RPC[@]}" bdev_ocf_delete "${OCF_NAME}" 2>/dev/null || true
        sleep 1
    fi
}

# Kill spdk_tgt and related processes
kill_spdk_tgt() {
    log "Stopping spdk_tgt..."

    # Kill the actual spdk_tgt binary first
    local spdk_pid=$(ps -ef | grep "[s]pdk_tgt" | grep -v grep | awk '{print $2}')
    if [[ -n "$spdk_pid" ]]; then
        for pid in $spdk_pid; do
            log "Killing spdk_tgt process: $pid"
            sudo kill -9 "$pid" 2>/dev/null || true
        done
    fi

    # Kill sudo wrapper processes for spdk_tgt.sh
    local sudo_pids=$(ps -ef | grep "[s]pdk_tgt.sh" | grep -v grep | awk '{print $2}')
    if [[ -n "$sudo_pids" ]]; then
        for pid in $sudo_pids; do
            log "Killing sudo wrapper: $pid"
            sudo kill -9 "$pid" 2>/dev/null || true
        done
    fi

    sleep 1
}

# Main
log "Cleaning up OCF setup..."

disconnect_nvmf
kill_spdk_tgt

log "=== OCF cleanup complete ==="
