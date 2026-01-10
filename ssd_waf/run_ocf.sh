#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SPDK_TGT_SCRIPT=${SPDK_TGT_SCRIPT:-"$ROOT_DIR/ssd_waf/spdk_tgt.sh"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
RPC="$ROOT_DIR/scripts/rpc.py -s $RPC_SOCKET"

# Device BDFs
CACHE_BDF=${CACHE_BDF:-0000:06:00.0}
BACKEND_BDF=${BACKEND_BDF:-0000:07:00.0}

# OCF configuration
OCF_NAME=${OCF_NAME:-ocf0}
OCF_MODE=${OCF_MODE:-wb}  # wb, wt, pt, wa, wi, wo
OCF_CACHE_LINE_SIZE=${OCF_CACHE_LINE_SIZE:-4}  # 4, 8, 16, 32, 64 KiB

# Export mode: nvmeof (default, stable) or ublk
EXPORT_MODE=${EXPORT_MODE:-nvmeof}

# ublk settings
UBLK_DEV_ID=${UBLK_DEV_ID:-0}
UBLK_CPUMASK=${UBLK_CPUMASK:-0xff}  # Core 0-7 for ublk
UBLK_NUM_QUEUES=${UBLK_NUM_QUEUES:-8}
UBLK_QUEUE_DEPTH=${UBLK_QUEUE_DEPTH:-512}

# NVMe-oF settings
NVMF_TRTYPE=${NVMF_TRTYPE:-tcp}
NVMF_TRADDR=${NVMF_TRADDR:-127.0.0.1}
NVMF_TRSVCID=${NVMF_TRSVCID:-4420}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:${OCF_NAME}}

# Skip options
SKIP_PRE_FORMAT=${SKIP_PRE_FORMAT:-0}

log() {
    echo "[run_ocf] $*"
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
        echo "  Backend: /dev/${backend_dev}"
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
        log "User cancelled. Exiting."
        exit 0
    fi
    echo ""

    log "Pre-formatting devices..."

    for dev_info in "cache:$cache_dev" "backend:$backend_dev"; do
        local role=${dev_info%%:*}
        local dev=${dev_info#*:}
        if [[ -n "$dev" ]] && [[ -e "/dev/${dev}" ]]; then
            local lbaf_4k=$(sudo nvme id-ns "/dev/${dev}" 2>/dev/null | \
                grep -E "^lbaf\s+[0-9]+.*lbads:12" | head -1 | \
                sed -E 's/^lbaf\s+([0-9]+).*/\1/')
            if [[ -z "$lbaf_4k" ]]; then
                lbaf_4k=0
            fi
            log "Formatting ${role} /dev/${dev} with lbaf=${lbaf_4k}"
            sudo nvme format "/dev/${dev}" -l "${lbaf_4k}" -f 2>/dev/null || true
        fi
    done

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
        log "Waiting for RPC socket failed"
        sudo kill "$tgt_pid" >/dev/null 2>&1 || true
        exit 1
    fi
    log "spdk_tgt is ready (RPC socket ${RPC_SOCKET})"
}

create_ocf() {
    log "Creating OCF bdev..."

    # Attach NVMe controllers
    local cache_ctrl="cache_ctrl"
    local backend_ctrl="backend_ctrl"

    log "Attaching cache controller at ${CACHE_BDF}"
    sudo $RPC bdev_nvme_attach_controller -b "$cache_ctrl" -t pcie -a "$CACHE_BDF" || true
    sleep 1

    log "Attaching backend controller at ${BACKEND_BDF}"
    sudo $RPC bdev_nvme_attach_controller -b "$backend_ctrl" -t pcie -a "$BACKEND_BDF" || true
    sleep 1

    local cache_ns="${cache_ctrl}n1"
    local backend_ns="${backend_ctrl}n1"

    log "Creating OCF bdev: ${OCF_NAME}"
    log "  Cache bdev:  ${cache_ns}"
    log "  Core bdev:   ${backend_ns}"
    log "  Mode:        ${OCF_MODE}"
    log "  Line size:   ${OCF_CACHE_LINE_SIZE}KB"

    sudo $RPC bdev_ocf_create "${OCF_NAME}" "${OCF_MODE}" "${cache_ns}" "${backend_ns}" \
        --cache-line-size "${OCF_CACHE_LINE_SIZE}"

    log "OCF bdev created: ${OCF_NAME}"
}

setup_nvmf() {
    log "Setting up NVMe-oF target..."

    # Create transport
    sudo $RPC nvmf_create_transport -t "${NVMF_TRTYPE}" || true
    sleep 1

    # Create subsystem
    sudo $RPC nvmf_create_subsystem "${NVMF_SUBSYSTEM}" -a -s "OCF0001"
    sleep 1

    # Add namespace
    sudo $RPC nvmf_subsystem_add_ns "${NVMF_SUBSYSTEM}" "${OCF_NAME}"
    sleep 1

    # Add listener
    sudo $RPC nvmf_subsystem_add_listener "${NVMF_SUBSYSTEM}" \
        -t "${NVMF_TRTYPE}" -f ipv4 -a "${NVMF_TRADDR}" -s "${NVMF_TRSVCID}"

    log "NVMe-oF ready: ${NVMF_SUBSYSTEM} @ ${NVMF_TRADDR}:${NVMF_TRSVCID}"
}

connect_nvmf() {
    if ! command -v nvme >/dev/null 2>&1; then
        log "'nvme' CLI not found; skipping connect"
        return 0
    fi

    log "Connecting to NVMe-oF target..."
    sudo nvme connect -t "${NVMF_TRTYPE}" -a "${NVMF_TRADDR}" -s "${NVMF_TRSVCID}" -n "${NVMF_SUBSYSTEM}" || true

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

    # Create ublk target
    sudo $RPC ublk_create_target -m "${UBLK_CPUMASK}" || {
        log "ublk_create_target failed (might already exist)"
    }
    sleep 1

    # Start ublk device
    log "Starting ublk device /dev/ublkb${UBLK_DEV_ID} (queues=${UBLK_NUM_QUEUES}, depth=${UBLK_QUEUE_DEPTH})"
    sudo $RPC ublk_start_disk "${OCF_NAME}" "${UBLK_DEV_ID}" -q "${UBLK_NUM_QUEUES}" -d "${UBLK_QUEUE_DEPTH}" || {
        log "ERROR: Failed to start ublk disk"
        return 1
    }

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
    echo "  sudo $RPC bdev_ocf_get_stats ${OCF_NAME}"
    echo ""
    echo "=========================================="
}

# Main
log "Resetting SPDK binding..."
sudo "${ROOT_DIR}/scripts/setup.sh" reset

pre_format_devices

log "Binding devices to SPDK..."
sudo HUGEMEM=8192 "${ROOT_DIR}/scripts/setup.sh"

start_spdk_tgt
create_ocf
export_to_host
show_status

log "=== OCF Setup complete ==="
