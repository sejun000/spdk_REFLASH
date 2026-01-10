#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
RPC="$ROOT_DIR/scripts/rpc.py -s $RPC_SOCKET"

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
        sudo $RPC ublk_stop_disk "${UBLK_DEV_ID}" 2>/dev/null || true
        sleep 1
    fi
}

# Delete OCF bdev
delete_ocf() {
    if [[ -S "$RPC_SOCKET" ]]; then
        log "Deleting OCF bdev ${OCF_NAME}..."
        sudo $RPC bdev_ocf_delete "${OCF_NAME}" 2>/dev/null || true
        sleep 1
    fi
}

# Kill spdk_tgt
kill_spdk_tgt() {
    if pgrep -f spdk_tgt >/dev/null 2>&1; then
        log "Stopping spdk_tgt..."
        sudo pkill -f spdk_tgt || true
        sleep 2
    fi
}

# Reset SPDK binding
reset_spdk() {
    log "Resetting SPDK binding..."
    sudo "${ROOT_DIR}/scripts/setup.sh" reset 2>/dev/null || true
}

# Main
log "Cleaning up OCF setup..."

disconnect_nvmf
stop_ublk
delete_ocf
kill_spdk_tgt
reset_spdk

log "=== OCF cleanup complete ==="
