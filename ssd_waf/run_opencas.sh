#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

# Device BDFs
# FDP SSD as cache (200GB partition)
CACHE_BDF=${CACHE_BDF:-0000:06:00.0}
# Regular SSD as backend (core device)
BACKEND_BDF=${BACKEND_BDF:-0000:07:00.0}

# Cache configuration
CACHE_SIZE_GB=${CACHE_SIZE_GB:-200}
CACHE_MODE=${CACHE_MODE:-wb}  # wb=write-back, wt=write-through, wa=write-around, pt=pass-through
CACHE_LINE_SIZE=${CACHE_LINE_SIZE:-4}  # 4KB cache line size
CACHE_ID=${CACHE_ID:-1}
CORE_ID=${CORE_ID:-1}

# Skip options
SKIP_PRE_FORMAT=${SKIP_PRE_FORMAT:-0}
PREFILL=${PREFILL:-0}

log() {
    echo "[run_opencas] $*"
}

check_prerequisites() {
    log "Checking prerequisites..."

    # Check casadm
    if ! command -v casadm &>/dev/null; then
        echo ""
        echo "ERROR: casadm not found!"
        echo ""
        echo "Please install OpenCAS v25.12 (supports kernel up to 6.16):"
        echo "  wget https://github.com/Open-CAS/open-cas-linux/releases/download/v25.12/open-cas-linux-25.12.0.0997.release.tar.gz"
        echo "  tar xzf open-cas-linux-25.12.0.0997.release.tar.gz"
        echo "  cd open-cas-linux-25.12.0.0997.release"
        echo "  ./configure"
        echo "  make -j\$(nproc)"
        echo "  sudo make install"
        echo "  sudo modprobe cas_cache"
        echo ""
        exit 1
    fi

    # Check if cas kernel module is loaded
    if ! lsmod | grep -q "^cas_cache"; then
        log "Loading cas_cache kernel module..."
        sudo modprobe cas_cache || {
            echo ""
            echo "ERROR: Failed to load cas_cache module!"
            echo "Make sure OpenCAS is properly installed."
            echo ""
            exit 1
        }
    fi

    log "Prerequisites OK (casadm found, cas_cache loaded)"
}

get_device_from_bdf() {
    local bdf=$1
    local dev=$(ls -d /sys/bus/pci/devices/${bdf}/nvme/nvme* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)
    if [[ -n "$dev" ]] && [[ -e "/dev/${dev}n1" ]]; then
        echo "/dev/${dev}n1"
    fi
}

# Pre-format devices with 4K block size
pre_format_devices() {
    if [[ "${SKIP_PRE_FORMAT}" == "1" ]]; then
        log "Skipping pre-format (SKIP_PRE_FORMAT=1)"
        return 0
    fi

    local cache_dev=$(get_device_from_bdf ${CACHE_BDF})
    local backend_dev=$(get_device_from_bdf ${BACKEND_BDF})

    echo ""
    echo "========================================"
    echo "  Device Format Confirmation (OpenCAS)"
    echo "========================================"
    echo ""
    echo "The following devices will be formatted:"
    echo ""
    if [[ -n "$cache_dev" ]]; then
        echo "  Cache:   ${cache_dev}  [${CACHE_SIZE_GB}GB partition will be used]"
    else
        echo "  Cache:   Not found at ${CACHE_BDF}"
    fi
    if [[ -n "$backend_dev" ]]; then
        echo "  Backend: ${backend_dev}  [Full capacity as core device]"
    else
        echo "  Backend: Not found at ${BACKEND_BDF}"
    fi
    echo ""
    echo "Cache mode: ${CACHE_MODE}"
    echo "Cache line size: ${CACHE_LINE_SIZE}KB"
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

    # Cache device - format with 4K block size
    if [[ -n "$cache_dev" ]]; then
        local lbaf_4k=$(sudo nvme id-ns "$cache_dev" 2>/dev/null | \
            grep -E "^lbaf\s+[0-9]+.*lbads:12" | head -1 | \
            sed -E 's/^lbaf\s+([0-9]+).*/\1/')
        if [[ -z "$lbaf_4k" ]]; then
            log "WARNING: No 4K LBA format found for cache, using default (lbaf 0)"
            lbaf_4k=0
        fi
        log "Formatting cache ${cache_dev} with lbaf=${lbaf_4k} (4K block size)"
        sudo nvme format "$cache_dev" -l "${lbaf_4k}" -f 2>/dev/null && \
            log "Cache format completed" || \
            log "Cache format failed, continuing..."
    fi

    # Backend device - format with 4K block size
    if [[ -n "$backend_dev" ]]; then
        local lbaf_4k=$(sudo nvme id-ns "$backend_dev" 2>/dev/null | \
            grep -E "^lbaf\s+[0-9]+.*lbads:12" | head -1 | \
            sed -E 's/^lbaf\s+([0-9]+).*/\1/')
        if [[ -z "$lbaf_4k" ]]; then
            log "WARNING: No 4K LBA format found for backend, using default (lbaf 0)"
            lbaf_4k=0
        fi
        log "Formatting backend ${backend_dev} with lbaf=${lbaf_4k} (4K block size)"
        sudo nvme format "$backend_dev" -l "${lbaf_4k}" -f 2>/dev/null && \
            log "Backend format completed" || \
            log "Backend format failed, continuing..."
    fi

    sleep 2
}

# Create partition on cache device for OpenCAS (200GB)
create_cache_partition() {
    local cache_dev=$(get_device_from_bdf ${CACHE_BDF})

    if [[ -z "$cache_dev" ]]; then
        log "ERROR: Cache device not found at ${CACHE_BDF}"
        exit 1
    fi

    # Remove existing partition table
    log "Creating ${CACHE_SIZE_GB}GB partition on ${cache_dev} for cache..."

    # Delete all partitions first
    sudo wipefs -a "${cache_dev}" 2>/dev/null || true

    # Create GPT partition table and partition
    # Partition 1: Cache (CACHE_SIZE_GB)
    echo "g
n
1

+${CACHE_SIZE_GB}G
w" | sudo fdisk "${cache_dev}" 2>/dev/null || true

    sleep 2

    # Verify partition was created
    if [[ ! -e "${cache_dev}p1" ]] && [[ ! -e "${cache_dev}1" ]]; then
        log "ERROR: Failed to create partition on ${cache_dev}"
        exit 1
    fi

    # Return partition path
    if [[ -e "${cache_dev}p1" ]]; then
        echo "${cache_dev}p1"
    else
        echo "${cache_dev}1"
    fi
}

# Prefill cache partition
prefill_cache() {
    if [[ "${PREFILL}" != "1" ]]; then
        return 0
    fi

    local cache_dev=$(get_device_from_bdf ${CACHE_BDF})

    if [[ -z "$cache_dev" ]]; then
        log "Cache device not found at ${CACHE_BDF}, skipping prefill"
        return 0
    fi

    local device_size=$(sudo blockdev --getsize64 "$cache_dev")
    local device_size_gb=$((device_size / 1024 / 1024 / 1024))

    echo ""
    echo "=========================================="
    echo "  Prefill: Sequential write to cache"
    echo "  Device: ${cache_dev} (${CACHE_BDF})"
    echo "  Size: ${device_size_gb} GB"
    echo "=========================================="
    echo ""

    sudo fio --name=prefill \
        --filename="${cache_dev}" \
        --ioengine=libaio \
        --direct=1 \
        --bs=1M \
        --rw=write \
        --iodepth=32 \
        --numjobs=1 \
        --group_reporting \
        --status-interval=10

    log "Prefill completed!"
    echo ""
}

# Stop existing OpenCAS configuration
stop_opencas() {
    log "Stopping existing OpenCAS configuration (if any)..."

    # List and stop all caches
    if casadm -L 2>/dev/null | grep -q "cache"; then
        log "Found existing cache configuration, stopping..."

        # Remove cores first
        for cache_id in $(casadm -L -o csv 2>/dev/null | grep "^cache" | cut -d',' -f2); do
            for core_id in $(casadm -L -C -i "$cache_id" -o csv 2>/dev/null | grep "^core" | cut -d',' -f2); do
                log "Removing core ${core_id} from cache ${cache_id}"
                sudo casadm -R -i "$cache_id" -j "$core_id" 2>/dev/null || true
            done
            log "Stopping cache ${cache_id}"
            sudo casadm -T -i "$cache_id" 2>/dev/null || true
        done
    fi

    sleep 1
}

# Start OpenCAS with write-back cache configuration
start_opencas() {
    local cache_dev=$(get_device_from_bdf ${CACHE_BDF})
    local backend_dev=$(get_device_from_bdf ${BACKEND_BDF})

    if [[ -z "$cache_dev" ]]; then
        log "ERROR: Cache device not found at ${CACHE_BDF}"
        exit 1
    fi

    if [[ -z "$backend_dev" ]]; then
        log "ERROR: Backend device not found at ${BACKEND_BDF}"
        exit 1
    fi

    # Create partition for cache
    local cache_partition=$(create_cache_partition)
    log "Cache partition: ${cache_partition}"

    log "Starting OpenCAS configuration..."
    log "  Cache device: ${cache_partition} (${CACHE_SIZE_GB}GB)"
    log "  Core device: ${backend_dev}"
    log "  Cache mode: ${CACHE_MODE}"
    log "  Cache line size: ${CACHE_LINE_SIZE}KB"

    # Start cache
    log "Creating cache instance (ID=${CACHE_ID})..."
    sudo casadm -S -d "${cache_partition}" -i ${CACHE_ID} -c ${CACHE_MODE} --cache-line-size ${CACHE_LINE_SIZE} || {
        log "ERROR: Failed to start cache"
        exit 1
    }

    # Add core device
    log "Adding core device (ID=${CORE_ID})..."
    sudo casadm -A -d "${backend_dev}" -i ${CACHE_ID} -j ${CORE_ID} || {
        log "ERROR: Failed to add core device"
        sudo casadm -T -i ${CACHE_ID}
        exit 1
    }

    log "OpenCAS configuration complete!"
    echo ""

    # Show configuration
    echo "=========================================="
    echo "  OpenCAS Configuration"
    echo "=========================================="
    casadm -L
    echo ""
    echo "Exported device: /dev/cas${CACHE_ID}-${CORE_ID}"
    echo ""
    echo "=========================================="
}

# Show status
show_status() {
    echo ""
    log "OpenCAS Status:"
    casadm -L
    echo ""
    log "Cache statistics:"
    casadm -P -i ${CACHE_ID} 2>/dev/null || true
    echo ""
}

# Usage help
usage() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --start       Start OpenCAS (default)"
    echo "  --stop        Stop OpenCAS"
    echo "  --status      Show OpenCAS status"
    echo "  --help        Show this help"
    echo ""
    echo "Environment variables:"
    echo "  CACHE_BDF        Cache device BDF (default: 0000:06:00.0)"
    echo "  BACKEND_BDF      Backend device BDF (default: 0000:07:00.0)"
    echo "  CACHE_SIZE_GB    Cache partition size in GB (default: 200)"
    echo "  CACHE_MODE       Cache mode: wb, wt, wa, pt (default: wb)"
    echo "  SKIP_PRE_FORMAT  Skip device format (default: 0)"
    echo "  PREFILL          Prefill cache device before setup (default: 0)"
    echo ""
    echo "Example:"
    echo "  PREFILL=1 $0 --start"
    echo "  $0 --status"
    echo "  $0 --stop"
}

# Main
main() {
    local action="start"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --start)
                action="start"
                shift
                ;;
            --stop)
                action="stop"
                shift
                ;;
            --status)
                action="status"
                shift
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            *)
                echo "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done

    case "$action" in
        start)
            check_prerequisites
            stop_opencas
            pre_format_devices
            prefill_cache
            start_opencas
            show_status
            ;;
        stop)
            check_prerequisites
            stop_opencas
            log "OpenCAS stopped"
            ;;
        status)
            check_prerequisites
            show_status
            ;;
    esac
}

main "$@"
