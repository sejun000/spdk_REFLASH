#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SPDK_TGT_SCRIPT=${SPDK_TGT_SCRIPT:-"$ROOT_DIR/ssd_waf/spdk_tgt.sh"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
RPC_BIN=${RPC_BIN:-"$ROOT_DIR/scripts/rpc.py"}
RPC=("$RPC_BIN" "-s" "$RPC_SOCKET" "-t" "300")

# Device BDFs
CACHE_BDF=${CACHE_BDF:-0000:06:00.0}
BACKEND_BDF=${BACKEND_BDF:-0000:07:00.0}

# OCF configuration
OCF_NAME=${OCF_NAME:-ocf0}
OCF_MODE=${OCF_MODE:-wo}  # wb, wt, pt, wa, wi, wo
OCF_CACHE_LINE_SIZE=${OCF_CACHE_LINE_SIZE:-4}  # 4, 8, 16, 32, 64 KiB
CACHE_SPLIT_GB=${CACHE_SPLIT_GB:-256}  # Split cache device to 256GB
BACKEND_SPLIT_GB=${BACKEND_SPLIT_GB:-0}  # Split backend device (0 = full capacity)
OCF_STAT_LOG=${OCF_STAT_LOG:-$ROOT_DIR/ssd_waf/logging}  # Directory for stats CSV log

# Export mode: nvmeof (default, stable) or ublk
EXPORT_MODE=${EXPORT_MODE:-nvmeof}

# ublk settings
UBLK_DEV_ID=${UBLK_DEV_ID:-0}
UBLK_CPUMASK=${UBLK_CPUMASK:-0xff}  # Core 0-7 for ublk
UBLK_NUM_QUEUES=${UBLK_NUM_QUEUES:-8}
UBLK_QUEUE_DEPTH=${UBLK_QUEUE_DEPTH:-512}

# NVMe-oF settings
NVMF_TRTYPE=${NVMF_TRTYPE:-tcp}
NVMF_ADRFAM=${NVMF_ADRFAM:-ipv4}
NVMF_TRADDR=${NVMF_TRADDR:-127.0.0.1}
NVMF_TRSVCID=${NVMF_TRSVCID:-4420}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:${OCF_NAME}}
NVMF_SERIAL=${NVMF_SERIAL:-OCF0001}

# Skip options
SKIP_PRE_FORMAT=${SKIP_PRE_FORMAT:-0}

# Prefill: Sequential write to cache device (skipping first CACHE_SPLIT_GB)
PREFILL=${PREFILL:-0}
BACKEND_PREFILL=${BACKEND_PREFILL:-${PREFILL}}

log() {
    echo "[run_ocf] $*"
}

rpc_call() {
    local desc=$1
    shift
    log "RPC start: ${desc}"
    if sudo "${RPC[@]}" "$@"; then
        log "RPC success: ${desc}"
        return 0
    else
        log "RPC FAILED: ${desc}"
        return 1
    fi
}

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

pre_format_devices() {
    if [[ "${SKIP_PRE_FORMAT}" == "1" ]]; then
        log "Skipping pre-format (SKIP_PRE_FORMAT=1)"
        return 0
    fi

    local cache_ctrl=$(ls -d /sys/bus/pci/devices/${CACHE_BDF}/nvme/nvme* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)
    local backend_ctrl=$(ls -d /sys/bus/pci/devices/${BACKEND_BDF}/nvme/nvme* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)

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
    echo "  Device Format Confirmation (OCF Mode)"
    echo "========================================"
    echo ""
    echo "The following devices will be formatted:"
    echo ""
    if [[ -n "$cache_dev" ]] && [[ -e "/dev/${cache_dev}" ]]; then
        echo "  Cache:   /dev/${cache_dev}"
    else
        echo "  Cache:   Not found at ${CACHE_BDF}"
    fi
    if [[ -n "$backend_dev" ]] && [[ -e "/dev/${backend_dev}" ]]; then
        if [[ "${BACKEND_SPLIT_GB}" -gt 0 ]]; then
            echo "  Backend: /dev/${backend_dev}  [${BACKEND_SPLIT_GB}GB limit]"
        else
            echo "  Backend: /dev/${backend_dev}  [Full capacity]"
        fi
    else
        echo "  Backend: Not found at ${BACKEND_BDF}"
    fi
    echo ""
    echo "OCF mode: ${OCF_MODE}"
    echo "Cache line size: ${OCF_CACHE_LINE_SIZE}KB"
    echo ""
    echo "========================================"
    echo ""
    read -p "Proceed with format? [y/N]: " confirm
    if [[ "${confirm,,}" != "y" ]]; then
        log "Skipping format, continuing with bringup..."
        return 0
    fi
    echo ""

    log "Pre-formatting devices..."

    for dev_info in "cache:$cache_dev" "backend:$backend_dev"; do
        local role=${dev_info%%:*}
        local dev=${dev_info#*:}
        if [[ -n "$dev" ]] && [[ -e "/dev/${dev}" ]]; then
            local lbaf_4k=$(sudo nvme id-ns "/dev/${dev}" 2>/dev/null | \
                grep -E "^lbaf\s+[0-9]+.*ms:0\s+lbads:12" | head -1 | \
                sed -E 's/^lbaf\s+([0-9]+).*/\1/')
            if [[ -z "$lbaf_4k" ]]; then
                lbaf_4k=0
            fi
            log "Formatting ${role} /dev/${dev} with lbaf=${lbaf_4k} (4K, ms:0)"
            sudo nvme format "/dev/${dev}" -l "${lbaf_4k}" -f 2>/dev/null || true
        fi
    done

    sleep 2
}

# Prefill: Sequential write to cache device (skipping first CACHE_SPLIT_GB for cache)
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
    echo "  Offset: ${CACHE_SPLIT_GB} GB (skip cache area)"
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

prefill_backend() {
    if [[ "${BACKEND_PREFILL}" != "1" ]]; then
        return 0
    fi

    local backend_ctrl=$(ls -d /sys/bus/pci/devices/${BACKEND_BDF}/nvme/nvme* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)
    local backend_dev=""
    if [[ -n "$backend_ctrl" ]]; then
        backend_dev=$(find_nvme_ns "$backend_ctrl" || true)
    fi

    if [[ -z "$backend_dev" ]] || [[ ! -e "/dev/${backend_dev}" ]]; then
        log "Backend device not found at ${BACKEND_BDF}, skipping backend prefill"
        return 0
    fi

    local device="/dev/${backend_dev}"
    local device_size=$(sudo blockdev --getsize64 "$device")
    local device_size_gb=$((device_size / 1024 / 1024 / 1024))

    echo ""
    echo "=========================================="
    echo "  Prefill: Sequential write to backend"
    echo "  Device: ${device} (${BACKEND_BDF})"
    echo "  Size: ${device_size_gb} GB"
    echo "=========================================="
    echo "예상 시간: ~$((device_size_gb / 1500))-$((device_size_gb / 1000))분 (1-1.5 GB/s 기준, QLC)"
    echo ""

    sudo fio --name=prefill_backend \
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
        log "Backend prefill failed!"
        exit 1
    fi

    echo ""
    log "Backend prefill completed!"
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
        log "Waiting for RPC socket failed"
        sudo kill "$tgt_pid" >/dev/null 2>&1 || true
        exit 1
    fi
    log "spdk_tgt is ready (RPC socket ${RPC_SOCKET})"

    # Enable uring zerocopy for better TCP performance (kernel 6.0+)
    # Must be called before framework_start_init
    "${RPC[@]}" sock_impl_set_options -i uring --enable-zerocopy-send-server --enable-zerocopy-send-client 2>/dev/null || true
    log "uring zerocopy enabled"

    # Start the SPDK framework (required when using --wait-for-rpc)
    "${RPC[@]}" framework_start_init
    log "framework started"
}

create_ocf() {
    log "Creating OCF bdev..."

    # Attach NVMe controllers
    local cache_ctrl="cache_ctrl"
    local backend_ctrl="backend_ctrl"

    log "Attaching cache controller at ${CACHE_BDF}"
    if ! rpc_call "attach cache controller ${cache_ctrl}" \
        bdev_nvme_attach_controller -b "$cache_ctrl" -t pcie -a "$CACHE_BDF"; then
        log "Ignoring attach failure (controller may already exist)"
    fi
    sleep 2

    log "Attaching backend controller at ${BACKEND_BDF}"
    if ! rpc_call "attach backend controller ${backend_ctrl}" \
        bdev_nvme_attach_controller -b "$backend_ctrl" -t pcie -a "$BACKEND_BDF"; then
        log "Ignoring attach failure (controller may already exist)"
    fi
    sleep 2

    local cache_ns="${cache_ctrl}n1"
    local backend_ns="${backend_ctrl}n1"

    # Split cache device if enabled (disabled by default)
    local cache_bdev
    if [[ "${CACHE_SPLIT_ENABLE:-0}" == "1" ]]; then
        local cache_split_mb=$((CACHE_SPLIT_GB * 1024))
        cache_bdev="${cache_ns}p0"
        log "Splitting cache device ${cache_ns} to ${CACHE_SPLIT_GB}GB (${cache_split_mb} MB)"
        rpc_call "split cache bdev ${cache_ns}" \
            bdev_split_create "${cache_ns}" 1 -s "${cache_split_mb}"
        sleep 1
    else
        cache_bdev="${cache_ns}"
        log "Using full cache namespace (CACHE_SPLIT_ENABLE=0)"
    fi

    # Split backend device if enabled
    local backend_bdev
    if [[ "${BACKEND_SPLIT_GB}" -gt 0 ]]; then
        local backend_split_mb=$((BACKEND_SPLIT_GB * 1024))
        backend_bdev="${backend_ns}p0"
        log "Splitting backend device ${backend_ns} to ${BACKEND_SPLIT_GB}GB (${backend_split_mb} MB)"
        rpc_call "split backend bdev ${backend_ns}" \
            bdev_split_create "${backend_ns}" 1 -s "${backend_split_mb}"
        sleep 1
    else
        backend_bdev="${backend_ns}"
        log "Using full backend namespace"
    fi

    log "Creating OCF bdev: ${OCF_NAME}"
    log "  Cache bdev:  ${cache_bdev}"
    log "  Core bdev:   ${backend_bdev}"
    log "  Mode:        ${OCF_MODE}"
    log "  Line size:   ${OCF_CACHE_LINE_SIZE}KB"
    log "  Stats log:   ${OCF_STAT_LOG}"

    mkdir -p "${OCF_STAT_LOG}"
    rpc_call "create OCF ${OCF_NAME}" \
        bdev_ocf_create "${OCF_NAME}" "${OCF_MODE}" "${cache_bdev}" "${backend_bdev}" \
        --cache-line-size "${OCF_CACHE_LINE_SIZE}" \
        --stat-log-path "${OCF_STAT_LOG}"

    log "OCF bdev created: ${OCF_NAME}"

    # Disable sequential cutoff to prevent hot sequential LBAs from bypassing cache
    # (Zipf distribution accesses low LBAs sequentially, which triggers seq cutoff)
    sleep 1
    rpc_call "disable sequential cutoff for ${OCF_NAME}" \
        bdev_ocf_set_seqcutoff "${OCF_NAME}" -p never
    log "Sequential cutoff disabled for ${OCF_NAME}"
}

setup_nvmf() {
    log "Setting up NVMe-oF target..."

    sleep 2
    NVMF_OK=1

    # Create transport
    if ! rpc_call "create NVMe-oF transport ${NVMF_TRTYPE}" \
        nvmf_create_transport -t "${NVMF_TRTYPE}"; then
        log "Transport ${NVMF_TRTYPE} may already exist, continuing"
    fi
    sleep 2

    # Create subsystem
    if ! rpc_call "create subsystem ${NVMF_SUBSYSTEM}" \
        nvmf_create_subsystem "${NVMF_SUBSYSTEM}" -a -s "${NVMF_SERIAL}"; then
        NVMF_OK=0
    fi

    if [[ "${NVMF_OK}" == "1" ]]; then
        sleep 2
        # Add namespace
        if ! rpc_call "add namespace ${OCF_NAME} to ${NVMF_SUBSYSTEM}" \
            nvmf_subsystem_add_ns "${NVMF_SUBSYSTEM}" "${OCF_NAME}"; then
            NVMF_OK=0
        fi
    fi

    if [[ "${NVMF_OK}" == "1" ]]; then
        sleep 2
        # Add listener
        if ! rpc_call "add listener ${NVMF_SUBSYSTEM}@${NVMF_TRADDR}:${NVMF_TRSVCID}" \
            nvmf_subsystem_add_listener "${NVMF_SUBSYSTEM}" \
            -t "${NVMF_TRTYPE}" -f "${NVMF_ADRFAM}" -a "${NVMF_TRADDR}" -s "${NVMF_TRSVCID}"; then
            NVMF_OK=0
        fi
    fi

    if [[ "${NVMF_OK}" == "1" ]]; then
        log "NVMe-oF TCP ready: ${NVMF_SUBSYSTEM} @ ${NVMF_TRADDR}:${NVMF_TRSVCID}"
    else
        log "NVMe-oF TCP setup failed"
        return 1
    fi
}

connect_nvmf() {
    if ! command -v nvme >/dev/null 2>&1; then
        log "'nvme' CLI not found; skipping connect"
        return 0
    fi

    log "Connecting to NVMe-oF target..."
    if sudo nvme connect -t "${NVMF_TRTYPE}" -a "${NVMF_TRADDR}" -s "${NVMF_TRSVCID}" -n "${NVMF_SUBSYSTEM}" -k 120 --ctrl-loss-tmo=120; then
        # Set I/O timeout to 120 seconds (default 30s)
        sleep 1
        for dev in /sys/class/nvme/nvme*/io_timeout; do
            if [[ -w "$dev" ]]; then
                echo 120 | sudo tee "$dev" > /dev/null 2>&1 || true
            fi
        done
        log "I/O timeout set to 120 seconds"
    fi

    sleep 2

    # Find connected device
    local dev=$(sudo nvme list 2>/dev/null | grep -i "OCF" | awk '{print $1}' | head -1)
    if [[ -n "$dev" ]]; then
        log "Connected device: $dev"
    else
        log "Device connected (check 'nvme list')"
    fi
}

setup_ublk() {
    log "Setting up ublk target..."

    # Load ublk kernel module
    if ! lsmod | grep -q "^ublk_drv"; then
        sudo modprobe ublk_drv || {
            log "ERROR: Failed to load ublk_drv module"
            return 1
        }
    fi

    sleep 2
    # Create ublk target
    if ! rpc_call "create ublk target (cpumask=${UBLK_CPUMASK})" \
        ublk_create_target -m "${UBLK_CPUMASK}"; then
        log "ublk_create_target failed (may already exist), continuing"
    fi
    sleep 1

    # Start ublk device
    log "Starting ublk device /dev/ublkb${UBLK_DEV_ID} (queues=${UBLK_NUM_QUEUES}, depth=${UBLK_QUEUE_DEPTH})"
    if ! rpc_call "start ublk for ${OCF_NAME}" \
        ublk_start_disk "${OCF_NAME}" "${UBLK_DEV_ID}" -q "${UBLK_NUM_QUEUES}" -d "${UBLK_QUEUE_DEPTH}"; then
        log "ERROR: Failed to start ublk disk"
        return 1
    fi

    # Wait for device
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
}

export_to_host() {
    case "${EXPORT_MODE}" in
        ublk)
            setup_ublk
            ;;
        nvmeof)
            setup_nvmf
            connect_nvmf
            ;;
        *)
            log "Unknown EXPORT_MODE: ${EXPORT_MODE}, using nvmeof"
            setup_nvmf
            connect_nvmf
            ;;
    esac
}

show_status() {
    echo ""
    echo "=========================================="
    echo "  OCF Setup Complete (EXPORT_MODE=${EXPORT_MODE})"
    echo "=========================================="
    echo ""
    echo "OCF bdev: ${OCF_NAME}"
    echo "Mode: ${OCF_MODE}"
    echo "Cache line size: ${OCF_CACHE_LINE_SIZE}KB"
    echo ""
    if [[ "${EXPORT_MODE}" == "ublk" ]]; then
        echo "Device: /dev/ublkb${UBLK_DEV_ID}"
        echo ""
        echo "To run fio:"
        echo "  DEVICE_TYPE=ublk ./ssd_waf/fio_icache.sh"
    else
        echo "NVMe-oF: ${NVMF_SUBSYSTEM}"
        echo "Address: ${NVMF_TRADDR}:${NVMF_TRSVCID}"
        echo ""
        echo "To run fio:"
        echo "  DEVICE_TYPE=nvmeof ./ssd_waf/fio_icache.sh"
    fi
    echo ""
    echo "To get OCF stats:"
    echo "  sudo ${RPC_BIN} -s ${RPC_SOCKET} bdev_ocf_get_stats ${OCF_NAME}"
    echo ""
    echo "=========================================="
}

# Main
log "Resetting SPDK binding..."
sudo "${ROOT_DIR}/scripts/setup.sh" reset

pre_format_devices
prefill_cache
prefill_backend

log "Compacting memory for hugepage allocation..."
sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
sudo sh -c 'echo 1 > /proc/sys/vm/compact_memory'
sleep 2

log "Binding devices to SPDK with uio_pci_generic..."
sudo HUGEMEM=30720 SHRINK_HUGE=yes "${ROOT_DIR}/scripts/setup.sh"

start_spdk_tgt
create_ocf
export_to_host
show_status

log "=== OCF Setup complete ==="
