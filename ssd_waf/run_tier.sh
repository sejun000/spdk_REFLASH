#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SPDK_TGT_SCRIPT=${SPDK_TGT_SCRIPT:-"$ROOT_DIR/ssd_waf/spdk_tgt.sh"}
CREATE_TIER_SCRIPT=${CREATE_TIER_SCRIPT:-"$ROOT_DIR/ssd_waf/create_tier.sh"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
NVMF_TRTYPE=${NVMF_TRTYPE:-tcp}
NVMF_ADRFAM=${NVMF_ADRFAM:-ipv4}
NVMF_TRADDR=${NVMF_TRADDR:-127.0.0.1}
NVMF_TRSVCID=${NVMF_TRSVCID:-4420}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:icache0}

# Device BDFs for pre-format (must match create_tier.sh)
CACHE_BDF=${CACHE_BDF:-0000:06:00.0}
BACKEND_BDF=${BACKEND_BDF:-0000:07:00.0}
SKIP_PRE_FORMAT=${SKIP_PRE_FORMAT:-0}

log() {
    echo "[run_tier] $*"
}

# Pre-format devices before SPDK takes over (uses nvme-cli)
pre_format_devices() {
    if [[ "${SKIP_PRE_FORMAT}" == "1" ]]; then
        log "Skipping pre-format (SKIP_PRE_FORMAT=1)"
        return 0
    fi

    # Get NVMe controller name from BDF (e.g., nvme2)
    local cache_dev=$(ls -d /sys/bus/pci/devices/${CACHE_BDF}/nvme/nvme* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)
    local backend_dev=$(ls -d /sys/bus/pci/devices/${BACKEND_BDF}/nvme/nvme* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)

    echo ""
    echo "========================================"
    echo "  Device Format/Reset Confirmation"
    echo "========================================"
    echo ""
    echo "The following devices will be formatted/reset:"
    echo ""
    if [[ -n "$cache_dev" ]] && [[ -e "/dev/${cache_dev}n1" ]]; then
        echo "  Cache (ZNS):   /dev/${cache_dev}n1  [Zone Reset All]"
    else
        echo "  Cache (ZNS):   Not found at ${CACHE_BDF}"
    fi
    if [[ -n "$backend_dev" ]] && [[ -e "/dev/${backend_dev}n1" ]]; then
        echo "  Backend:       /dev/${backend_dev}n1  [Format]"
    else
        echo "  Backend:       Not found at ${BACKEND_BDF}"
    fi
    echo ""
    echo "========================================"
    echo ""
    read -p "Proceed with format/reset? [y/N]: " confirm
    if [[ "${confirm,,}" != "y" ]]; then
        log "User cancelled. Exiting."
        exit 0
    fi
    echo ""

    log "Pre-formatting devices before SPDK startup..."

    # Cache device (ZNS) - reset all zones
    if [[ -n "$cache_dev" ]] && [[ -e "/dev/${cache_dev}n1" ]]; then
        log "Resetting ZNS zones on /dev/${cache_dev}n1"
        sudo nvme format /dev/${cache_dev}n1 -s 2 --force -l 2 >/dev/null && \
            log "Zone reset completed" || \
            log "Zone reset failed or not ZNS device, continuing..."
    fi

    # Backend device (regular NVMe) - format with 4K block size to match ZNS cache
    if [[ -n "$backend_dev" ]] && [[ -e "/dev/${backend_dev}n1" ]]; then
        # Find LBA format index with 4K block size (lbads:12 = 2^12 = 4096)
        local lbaf_4k=$(sudo nvme id-ns "/dev/${backend_dev}n1" 2>/dev/null | \
            grep -E "^lbaf\s+[0-9]+.*lbads:12" | head -1 | \
            sed -E 's/^lbaf\s+([0-9]+).*/\1/')
        if [[ -z "$lbaf_4k" ]]; then
            log "WARNING: No 4K LBA format found, using default (lbaf 0)"
            lbaf_4k=0
        fi
        log "Formatting /dev/${backend_dev}n1 with lbaf=${lbaf_4k} (4K block size)"
        sudo nvme format "/dev/${backend_dev}n1" -l "${lbaf_4k}" -f 2>/dev/null && \
            log "Format completed" || \
            log "Format failed, continuing..."
    fi

    sleep 2
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

create_tier() {
    log "Running create_tier.sh"
    sudo -E "${CREATE_TIER_SCRIPT}"
    log "Tier creation complete"
}

# Set QoS limit on icache bdev (MB/s, 0 = unlimited)
ICACHE_QOS_MBPS=${ICACHE_QOS_MBPS:-0}

set_qos_limit() {
    if [[ "${ICACHE_QOS_MBPS}" == "0" ]]; then
        log "QoS limit disabled (ICACHE_QOS_MBPS=0)"
        return 0
    fi

    local bdev_name="${ICACHE_NAME:-icache0}"
    log "Setting QoS limit on ${bdev_name}: ${ICACHE_QOS_MBPS} MB/s"
    sudo "$ROOT_DIR/scripts/rpc.py" -s "$RPC_SOCKET" \
        bdev_set_qos_limit "${bdev_name}" --rw_mbytes_per_sec "${ICACHE_QOS_MBPS}" && \
        log "QoS limit set successfully" || \
        log "Failed to set QoS limit"
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

# Reset SPDK binding so kernel driver can see devices for format
log "Resetting SPDK binding (scripts/setup.sh reset)..."
sudo "${ROOT_DIR}/scripts/setup.sh" reset

pre_format_devices

# Bind devices to SPDK (vfio-pci/uio) so spdk_tgt can use them
# HUGEMEM=8192 allocates 4096 x 2MB hugepages = 8GB for DMA buffers
log "Binding devices to SPDK (scripts/setup.sh) with HUGEMEM=8192..."
sudo HUGEMEM=8192 "${ROOT_DIR}/scripts/setup.sh"

start_spdk_tgt
create_tier
set_qos_limit
connect_host
