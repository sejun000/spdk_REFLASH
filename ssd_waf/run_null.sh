#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SPDK_TGT_SCRIPT=${SPDK_TGT_SCRIPT:-"$ROOT_DIR/ssd_waf/spdk_tgt.sh"}
CREATE_NULL_SCRIPT=${CREATE_NULL_SCRIPT:-"$ROOT_DIR/ssd_waf/create_null.sh"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
NVMF_TRTYPE=${NVMF_TRTYPE:-tcp}
NVMF_ADRFAM=${NVMF_ADRFAM:-ipv4}
NVMF_TRADDR=${NVMF_TRADDR:-127.0.0.1}
NVMF_TRSVCID=${NVMF_TRSVCID:-4420}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:icache0}

log() {
    echo "[run_null] $*"
}

wait_for_rpc() {
    local timeout=${1:-30}
    local waited=0
    while [[ ! -S "$RPC_SOCKET" ]]; do
        if (( waited >= timeout )); then
            log "RPC socket ${RPC_SOCKET} not ready after ${timeout}s"
            return 1
        fi
        sleep 1
        ((waited++))
    done
    return 0
}

start_spdk_tgt() {
    if pgrep -f spdk_tgt >/dev/null 2>&1; then
        log "spdk_tgt already running"
        return 0
    fi

    log "Launching spdk_tgt via ${SPDK_TGT_SCRIPT}"
    nohup sudo -E "${SPDK_TGT_SCRIPT}" &
    local tgt_pid=$!
    disown $tgt_pid

    if ! wait_for_rpc 30; then
        log "Waiting for RPC socket failed; stopping spdk_tgt"
        sudo kill "$tgt_pid" >/dev/null 2>&1 || true
        exit 1
    fi
    log "spdk_tgt is ready (RPC socket ${RPC_SOCKET})"

    # Set log level to WARNING (suppress NOTICE logs)
    sudo "$ROOT_DIR/scripts/rpc.py" -s "$RPC_SOCKET" log_set_print_level WARNING 2>/dev/null || true
}

create_null() {
    log "Running create_null.sh (pure null bdev, no icache)"
    sudo -E "${CREATE_NULL_SCRIPT}"
    log "Null bdev creation complete"
}

connect_host() {
    if ! command -v nvme >/dev/null 2>&1; then
        log "'nvme' CLI not found; skipping connect step"
        return 0
    fi

    log "Connecting host NVMe controller (trtype=${NVMF_TRTYPE}, addr=${NVMF_TRADDR}, port=${NVMF_TRSVCID}, nqn=${NVMF_SUBSYSTEM})"
    if sudo nvme connect -t "${NVMF_TRTYPE}" -a "${NVMF_TRADDR}" -s "${NVMF_TRSVCID}" -n "${NVMF_SUBSYSTEM}"; then
        log "nvme connect succeeded"
    else
        log "nvme connect failed (might already be connected); continuing"
    fi
}

echo ""
echo "========================================"
echo "  Null Bdev Baseline Test"
echo "========================================"
echo ""
echo "This creates a pure null bdev (no icache layer)"
echo "to measure baseline NVMe-oF TCP performance."
echo ""
echo "========================================"
echo ""

# Bind hugepages for SPDK
log "Setting up SPDK hugepages (HUGEMEM=8192)..."
sudo HUGEMEM=8192 "${ROOT_DIR}/scripts/setup.sh"

start_spdk_tgt
create_null
connect_host
