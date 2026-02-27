#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SPDK_TGT_SCRIPT=${SPDK_TGT_SCRIPT:-"$ROOT_DIR/ssd_waf/spdk_tgt.sh"}
CREATE_TIER_SCRIPT=${CREATE_TIER_SCRIPT:-"$ROOT_DIR/ssd_waf/create_tier_fdp.sh"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}

# Export mode: nvmeof (default, stable) or ublk
EXPORT_MODE=${EXPORT_MODE:-nvmeof}

# ublk settings
UBLK_DEV_ID=${UBLK_DEV_ID:-0}

# NVMe-oF settings (used when EXPORT_MODE=nvmeof)
NVMF_TRTYPE=${NVMF_TRTYPE:-tcp}
NVMF_ADRFAM=${NVMF_ADRFAM:-ipv4}
NVMF_TRADDR=${NVMF_TRADDR:-127.0.0.1}
NVMF_TRSVCID=${NVMF_TRSVCID:-4420}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:icache0}

# Device BDFs for FDP setup
# FDP SSD as cache (100GB limit)
CACHE_BDF=${CACHE_BDF:-0000:06:00.0}
BACKEND_BDF=${BACKEND_BDF:-0000:07:00.0}
SKIP_PRE_FORMAT=${SKIP_PRE_FORMAT:-0}

# FDP cache size: ~512GB (549,357,355,008 bytes = 25% of 2TB, aligned to 13079937024)
export CACHE_SPLIT_GB=${CACHE_SPLIT_GB:-256}
# Backend split size (0 = use full capacity)
export BACKEND_SPLIT_GB=${BACKEND_SPLIT_GB:-0}

log() {
    echo "[run_tier_fdp] $*"
}

# Find namespace device for NVMe controller (n1, n2, etc.)
find_nvme_ns() {
    local ctrl=$1
    for ns in n1 n2 n3 n4; do
        if [[ -e "/dev/${ctrl}${ns}" ]]; then
            echo "${ctrl}${ns}"
            return 0
        fi
    done
    return 1
}

# Pre-format devices before SPDK takes over (uses nvme-cli)
# For FDP: no zone reset needed, just format normally
pre_format_devices() {
    if [[ "${SKIP_PRE_FORMAT}" == "1" ]]; then
        log "Skipping pre-format (SKIP_PRE_FORMAT=1)"
        return 0
    fi

    # Get NVMe controller name from BDF (e.g., nvme2)
    local cache_ctrl=$(ls -d /sys/bus/pci/devices/${CACHE_BDF}/nvme/nvme* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)
    local backend_ctrl=$(ls -d /sys/bus/pci/devices/${BACKEND_BDF}/nvme/nvme* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)

    # Find actual namespace (n1, n2, etc.)
    local cache_dev=""
    local backend_dev=""
    if [[ -n "$cache_ctrl" ]]; then
        cache_dev=$(find_nvme_ns "$cache_ctrl" || true)
    fi
    if [[ -n "$backend_ctrl" ]]; then
        backend_dev=$(find_nvme_ns "$backend_ctrl" || true)
    fi

    echo ""
    echo "========================================"
    echo "  Device Format Confirmation (FDP Mode)"
    echo "========================================"
    echo ""
    echo "The following devices will be formatted:"
    echo ""
    if [[ -n "$cache_dev" ]] && [[ -e "/dev/${cache_dev}" ]]; then
        echo "  Cache (FDP):   /dev/${cache_dev}  [Format - ${CACHE_SPLIT_GB}GB limit]"
    else
        echo "  Cache (FDP):   Not found at ${CACHE_BDF}"
    fi
    if [[ -n "$backend_dev" ]] && [[ -e "/dev/${backend_dev}" ]]; then
        if [[ "${BACKEND_SPLIT_GB}" -gt 0 ]]; then
            echo "  Backend:       /dev/${backend_dev}  [Format - ${BACKEND_SPLIT_GB}GB limit]"
        else
            echo "  Backend:       /dev/${backend_dev}  [Format - Full capacity]"
        fi
    else
        echo "  Backend:       Not found at ${BACKEND_BDF}"
    fi
    echo ""
    echo "========================================"
    echo ""
    read -p "Proceed with format? [y/N]: " confirm
    if [[ "${confirm,,}" != "y" ]]; then
        log "Skipping format, continuing with bringup..."
        return 0
    fi
    echo ""

    log "Pre-formatting devices before SPDK startup..."

    # Cache device (FDP) - format with 4K block size (lbads:12), no metadata (ms:0)
    if [[ -n "$cache_dev" ]] && [[ -e "/dev/${cache_dev}" ]]; then
        local lbaf_4k=$(sudo nvme id-ns "/dev/${cache_dev}" 2>/dev/null | \
            grep -E "^lbaf\s+[0-9]+.*ms:0\s+lbads:12" | head -1 | \
            sed -E 's/^lbaf\s+([0-9]+).*/\1/')
        if [[ -z "$lbaf_4k" ]]; then
            log "WARNING: No 4K LBA format with ms:0 found for cache, using default (lbaf 0)"
            lbaf_4k=0
        fi
        log "Formatting FDP cache /dev/${cache_dev} with lbaf=${lbaf_4k} (4K, ms:0)"
        sudo nvme format "/dev/${cache_dev}" -l "${lbaf_4k}" -f 2>/dev/null && \
            log "Cache format completed" || \
            log "Cache format failed, continuing..."
    fi

    # Backend device - format with 4K block size (lbads:12), no metadata (ms:0)
    if [[ -n "$backend_dev" ]] && [[ -e "/dev/${backend_dev}" ]]; then
        local lbaf_4k=$(sudo nvme id-ns "/dev/${backend_dev}" 2>/dev/null | \
            grep -E "^lbaf\s+[0-9]+.*ms:0\s+lbads:12" | head -1 | \
            sed -E 's/^lbaf\s+([0-9]+).*/\1/')
        if [[ -z "$lbaf_4k" ]]; then
            log "WARNING: No 4K LBA format with ms:0 found for backend, using default (lbaf 0)"
            lbaf_4k=0
        fi
        log "Formatting /dev/${backend_dev} with lbaf=${lbaf_4k} (4K, ms:0)"
        sudo nvme format "/dev/${backend_dev}" -l "${lbaf_4k}" -f 2>/dev/null && \
            log "Format completed" || \
            log "Format failed, continuing..."
    fi

    sleep 2
}

# Prefill: Cache 디바이스 (CACHE_BDF)를 전체 sequential write로 채움
# SPDK 바인딩 전에 kernel driver로 수행
PREFILL=${PREFILL:-0}

prefill_cache() {
    if [[ "${PREFILL}" != "1" ]]; then
        return 0
    fi

    local cache_ctrl=$(ls -d /sys/bus/pci/devices/${CACHE_BDF}/nvme/nvme* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)
    local cache_dev=""
    if [[ -n "$cache_ctrl" ]]; then
        cache_dev=$(find_nvme_ns "$cache_ctrl" || true)
    fi

    if [[ -z "$cache_dev" ]] || [[ ! -e "/dev/${cache_dev}" ]]; then
        log "Cache device not found at ${CACHE_BDF}, skipping prefill"
        return 0
    fi

    local device="/dev/${cache_dev}"
    local device_size=$(sudo blockdev --getsize64 "$device")
    local device_size_gb=$((device_size / 1024 / 1024 / 1024))

    echo ""
    echo "=========================================="
    echo "  Prefill: Sequential write to cache"
    echo "  Device: ${device} (${CACHE_BDF})"
    echo "  Size: ${device_size_gb} GB"
    echo "=========================================="
    echo "예상 시간: ~$((device_size_gb / 2000))-$((device_size_gb / 1500))분 (1.5-2 GB/s 기준)"
    echo ""

    sudo fio --name=prefill \
        --filename="${device}" \
        --ioengine=libaio \
        --direct=1 \
        --bs=1M \
        --rw=write \
        --iodepth=32 \
        --numjobs=1 \
        --group_reporting \
        --status-interval=10

    if [[ $? -ne 0 ]]; then
        log "Prefill failed!"
        exit 1
    fi

    echo ""
    log "Prefill completed!"
    echo ""
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

    # Enable uring zerocopy for better TCP performance (kernel 6.0+)
    # Must be called before framework_start_init
    sudo "$ROOT_DIR/scripts/rpc.py" -s "$RPC_SOCKET" sock_impl_set_options -i uring --enable-zerocopy-send-server --enable-zerocopy-send-client 2>/dev/null || true
    log "uring zerocopy enabled"

    # Start the SPDK framework (required when using --wait-for-rpc)
    sudo "$ROOT_DIR/scripts/rpc.py" -s "$RPC_SOCKET" framework_start_init
    log "framework started"
}

create_tier() {
    # Set NVMF/UBLK enable based on EXPORT_MODE
    if [[ "${EXPORT_MODE}" == "ublk" ]]; then
        export NVMF_ENABLE=0
        export UBLK_ENABLE=1
    else
        export NVMF_ENABLE=1
        export UBLK_ENABLE=0
    fi

    log "Running create_tier_fdp.sh (NVMF_ENABLE=${NVMF_ENABLE}, UBLK_ENABLE=${UBLK_ENABLE})"
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

# Wait for ublk device (already created by create_tier_fdp.sh)
setup_ublk() {
    # ublk device is already created by create_tier_fdp.sh when UBLK_ENABLE=1
    # Just wait for it to appear
    local waited=0
    while [[ ! -e "/dev/ublkb${UBLK_DEV_ID}" ]]; do
        if (( waited >= 10 )); then
            log "ERROR: /dev/ublkb${UBLK_DEV_ID} not found after 10s"
            return 1
        fi
        sleep 1
        ((waited++))
    done

    log "ublk device ready: /dev/ublkb${UBLK_DEV_ID}"
    echo ""
    echo "=========================================="
    echo "  ublk Device Ready"
    echo "  /dev/ublkb${UBLK_DEV_ID}"
    echo "=========================================="
    echo ""
}

# Connect via NVMe-oF (for remote or when ublk not available)
connect_nvmeof() {
    if ! command -v nvme >/dev/null 2>&1; then
        log "'nvme' CLI not found; skipping connect step"
        return 0
    fi

    log "Connecting host NVMe controller (trtype=${NVMF_TRTYPE}, addr=${NVMF_TRADDR}, port=${NVMF_TRSVCID}, nqn=${NVMF_SUBSYSTEM})"
    if sudo nvme connect -t "${NVMF_TRTYPE}" -a "${NVMF_TRADDR}" -s "${NVMF_TRSVCID}" -n "${NVMF_SUBSYSTEM}" -k 120 --ctrl-loss-tmo=120; then
        log "nvme connect succeeded"
        # Set I/O timeout to 120 seconds (default 30s)
        sleep 1
        for dev in /sys/class/nvme/nvme*/io_timeout; do
            if [[ -w "$dev" ]]; then
                echo 120 | sudo tee "$dev" > /dev/null 2>&1 || true
            fi
        done
        log "I/O timeout set to 120 seconds"
    else
        log "nvme connect failed (might already be connected); continuing"
    fi
}

# Export bdev to host (ublk or NVMe-oF based on EXPORT_MODE)
export_to_host() {
    case "${EXPORT_MODE}" in
        ublk)
            setup_ublk
            ;;
        nvmeof)
            connect_nvmeof
            ;;
        *)
            log "Unknown EXPORT_MODE: ${EXPORT_MODE}, using ublk"
            setup_ublk
            ;;
    esac
}

# Reset SPDK binding so kernel driver can see devices for format
log "Resetting SPDK binding (scripts/setup.sh reset)..."
sudo "${ROOT_DIR}/scripts/setup.sh" reset

pre_format_devices
prefill_cache

# Bind devices to SPDK (uio_pci_generic) so spdk_tgt can use them
# HUGEMEM=12288 allocates 6144 x 2MB hugepages = 12GB for DMA buffers
log "Compacting memory for hugepage allocation..."
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
sudo sh -c 'echo 1 > /proc/sys/vm/compact_memory'
sleep 2

log "Binding devices to SPDK (scripts/setup.sh) with HUGEMEM=10000 and uio_pci_generic..."
sudo HUGEMEM=8192 SHRINK_HUGE=yes "${ROOT_DIR}/scripts/setup.sh"

start_spdk_tgt
create_tier
set_qos_limit
export_to_host

log "=== Setup complete (EXPORT_MODE=${EXPORT_MODE}) ==="
