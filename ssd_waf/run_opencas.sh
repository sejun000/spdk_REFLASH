#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

# Device BDFs
# FDP SSD as cache (200GB partition)
CACHE_BDF=${CACHE_BDF:-0001:10:00.0}
BACKEND_BDF=${BACKEND_BDF:-0000:01:00.0}

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

# Limit NVMe IRQ affinity to specific cores
NVME_CPU_MASK=${NVME_CPU_MASK:-0x3}  # Core 0-1 (2 cores)

set_nvme_irq_affinity() {
    log "Setting NVMe IRQ affinity to CPU mask ${NVME_CPU_MASK}..."

    # Find all NVMe IRQs for cache and backend devices
    for bdf in ${CACHE_BDF} ${BACKEND_BDF}; do
        local irqs=$(grep "PCI-MSIX-${bdf}" /proc/interrupts | awk -F: '{print $1}' | tr -d ' ')
        for irq in $irqs; do
            if [[ -f "/proc/irq/${irq}/smp_affinity" ]]; then
                echo "${NVME_CPU_MASK}" | sudo tee "/proc/irq/${irq}/smp_affinity" > /dev/null 2>&1 || true
            fi
        done
        local count=$(echo "$irqs" | wc -w)
        log "Set affinity for ${count} IRQs on ${bdf}"
    done
}

# Setup persistent NVMe device names via udev rules
setup_udev_rules() {
    local rules_file="/etc/udev/rules.d/99-nvme-persistent.rules"

    if [[ -f "$rules_file" ]]; then
        return 0
    fi

    log "Setting up persistent NVMe device names (udev rules)..."

    sudo tee "$rules_file" > /dev/null << EOF
# Persistent NVMe device names based on PCI BDF
# Cache device (FDP SSD) - ${CACHE_BDF}
SUBSYSTEM=="block", KERNEL=="nvme*n1", KERNELS=="${CACHE_BDF}", SYMLINK+="nvme_cache"
SUBSYSTEM=="block", KERNEL=="nvme*n1p*", KERNELS=="${CACHE_BDF}", SYMLINK+="nvme_cache_part%n"

# Backend device (regular SSD) - ${BACKEND_BDF}
SUBSYSTEM=="block", KERNEL=="nvme*n1", KERNELS=="${BACKEND_BDF}", SYMLINK+="nvme_backend"
SUBSYSTEM=="block", KERNEL=="nvme*n1p*", KERNELS=="${BACKEND_BDF}", SYMLINK+="nvme_backend_part%n"
EOF

    sudo udevadm control --reload-rules
    sudo udevadm trigger --subsystem-match=block
    sudo udevadm settle --timeout=5

    log "udev rules created: $rules_file"
    log "Persistent symlinks: /dev/nvme_cache, /dev/nvme_backend"
}

# List available NVMe devices with BDF
list_nvme_devices() {
    echo ""
    echo "Available NVMe devices:"
    echo "========================"
    for nvme in /sys/class/nvme/nvme*; do
        if [[ -d "$nvme" ]]; then
            local name=$(basename "$nvme")
            local bdf=$(basename $(readlink -f "$nvme/device"))
            local model=$(cat "$nvme/model" 2>/dev/null | tr -d '\n' | xargs)
            # Find first available namespace
            local ns_dev=""
            for ns in n1 n2 n3 n4; do
                if [[ -e "/sys/block/${name}${ns}" ]]; then
                    ns_dev="${name}${ns}"
                    break
                fi
            done
            if [[ -n "$ns_dev" ]]; then
                local size_bytes=$(cat "/sys/block/${ns_dev}/size" 2>/dev/null || echo 0)
                local size_gb=$((size_bytes * 512 / 1024 / 1024 / 1024))
                echo "  ${bdf}  /dev/${ns_dev}  ${size_gb}GB  ${model}"
            fi
        fi
    done
    echo ""
}

# Verify BDF exists and return device path
verify_bdf() {
    local bdf=$1
    local role=$2  # "cache" or "backend"
    local dev=$(get_device_from_bdf "$bdf")

    if [[ -z "$dev" ]]; then
        echo ""
        echo "ERROR: ${role} device not found at BDF ${bdf}"
        list_nvme_devices
        echo "Set correct BDF with: ${role^^}_BDF=0000:XX:00.0 $0 --start"
        echo ""
        exit 1
    fi
    echo "$dev"
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
    local ctrl=$(ls -d /sys/bus/pci/devices/${bdf}/nvme/nvme* 2>/dev/null | head -1 | xargs basename 2>/dev/null || true)
    if [[ -n "$ctrl" ]]; then
        # Find first available namespace (n1, n2, etc.)
        for ns in n1 n2 n3 n4; do
            if [[ -e "/dev/${ctrl}${ns}" ]]; then
                echo "/dev/${ctrl}${ns}"
                return 0
            fi
        done
    fi
}

# Convert device path to by-id path (required by OpenCAS)
get_by_id_path() {
    local dev=$1
    local dev_name=$(basename "$dev")

    # Find by-id symlink that points to this device
    local by_id=$(ls -la /dev/disk/by-id/ 2>/dev/null | grep -E "nvme-.*-> \.\./\.\./${dev_name}$" | grep -v "eui\." | head -1 | awk '{print $9}')

    if [[ -n "$by_id" ]]; then
        echo "/dev/disk/by-id/${by_id}"
    else
        # Fallback to original path if by-id not found
        echo "$dev"
    fi
}

# Safety check: ensure device is not boot/root device
check_not_boot_device() {
    local dev=$1
    local role=$2

    # Check if any partition is mounted
    if mount | grep -q "^${dev}"; then
        echo ""
        echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
        echo "  CRITICAL ERROR: ${role} device is MOUNTED!"
        echo "  Device: ${dev}"
        echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
        echo ""
        echo "Mounted partitions:"
        mount | grep "^${dev}" || true
        echo ""
        echo "This could be your BOOT or ROOT device!"
        echo "Aborting to prevent data loss."
        echo ""
        exit 1
    fi

    # Check if device contains root filesystem
    local root_dev=$(findmnt -n -o SOURCE / 2>/dev/null | sed 's/p[0-9]*$//' | sed 's/[0-9]*$//')
    if [[ "${dev}" == "${root_dev}"* ]]; then
        echo ""
        echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
        echo "  CRITICAL ERROR: ${role} device contains ROOT filesystem!"
        echo "  Device: ${dev}"
        echo "  Root: ${root_dev}"
        echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
        echo ""
        exit 1
    fi

    # Check /etc/fstab for this device
    if grep -q "${dev}" /etc/fstab 2>/dev/null; then
        echo ""
        echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
        echo "  WARNING: ${role} device found in /etc/fstab!"
        echo "  Device: ${dev}"
        echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
        grep "${dev}" /etc/fstab
        echo ""
        read -p "Are you SURE this is not a system device? [yes/NO]: " confirm
        if [[ "${confirm}" != "yes" ]]; then
            echo "Aborting."
            exit 1
        fi
    fi

    log "Safety check passed for ${role}: ${dev}"
}

# Pre-format devices with 4K block size
pre_format_devices() {
    if [[ "${SKIP_PRE_FORMAT}" == "1" ]]; then
        log "Skipping pre-format (SKIP_PRE_FORMAT=1)"
        return 0
    fi

    local cache_dev=$(get_device_from_bdf ${CACHE_BDF})
    local backend_dev=$(get_device_from_bdf ${BACKEND_BDF})

    # SAFETY CHECK: Ensure devices are not boot/root devices
    log "Running safety checks..."
    if [[ -n "$cache_dev" ]]; then
        check_not_boot_device "$cache_dev" "Cache"
    fi
    if [[ -n "$backend_dev" ]]; then
        check_not_boot_device "$backend_dev" "Backend"
    fi

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
    log "Creating ${CACHE_SIZE_GB}GB partition on ${cache_dev} for cache..." >&2

    # Delete all partitions first
    sudo wipefs -a "${cache_dev}" &>/dev/null || true

    # Create GPT partition table and partition
    # Partition 1: Cache (CACHE_SIZE_GB)
    echo "g
n
1

+${CACHE_SIZE_GB}G
w" | sudo fdisk "${cache_dev}" &>/dev/null || true

    # Wait for partition to appear
    sleep 2
    sudo partprobe "${cache_dev}" 2>/dev/null || true
    sleep 1

    # Verify partition was created
    if [[ ! -e "${cache_dev}p1" ]] && [[ ! -e "${cache_dev}1" ]]; then
        log "ERROR: Failed to create partition on ${cache_dev}" >&2
        exit 1
    fi

    # Return partition path (only this goes to stdout)
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
    if casadm --list-caches 2>/dev/null | grep -q "cache"; then
        log "Found existing cache configuration, stopping..."

        # Remove cores first
        for cache_id in $(casadm --list-caches -o csv 2>/dev/null | grep "^cache" | cut -d',' -f2); do
            for core_id in $(casadm --list-caches -o csv 2>/dev/null | grep "^core" | grep ",$cache_id," | cut -d',' -f2); do
                log "Removing core ${core_id} from cache ${cache_id}"
                sudo casadm --remove-core --cache-id "$cache_id" --core-id "$core_id" --force 2>/dev/null || true
            done
            log "Stopping cache ${cache_id}"
            sudo casadm --stop-cache --cache-id "$cache_id" --no-data-flush 2>/dev/null || true
        done
    fi

    sleep 1

    # Trigger udev to restore persistent device names
    log "Triggering udev to restore device names..."
    sudo udevadm trigger --subsystem-match=block
    sudo udevadm settle --timeout=5
    sleep 1
}

# Start OpenCAS with write-back cache configuration
start_opencas() {
    # Verify BDFs exist before proceeding
    log "Verifying device BDFs..."
    local cache_dev=$(verify_bdf ${CACHE_BDF} "cache")
    local backend_dev=$(verify_bdf ${BACKEND_BDF} "backend")

    # SAFETY CHECK (even if pre_format was skipped)
    check_not_boot_device "$cache_dev" "Cache"
    check_not_boot_device "$backend_dev" "Backend"

    log "Cache device:   ${cache_dev} (${CACHE_BDF})"
    log "Backend device: ${backend_dev} (${BACKEND_BDF})"

    # Create partition for cache
    local cache_partition=$(create_cache_partition)
    log "Cache partition: ${cache_partition}"

    # Verify partition exists
    if [[ ! -b "${cache_partition}" ]]; then
        log "ERROR: Cache partition not found: ${cache_partition}"
        log "Checking available partitions..."
        ls -la ${cache_dev}* 2>/dev/null || true
        exit 1
    fi

    # Convert to by-id paths (required by OpenCAS)
    local cache_by_id=$(get_by_id_path "${cache_partition}")
    local backend_by_id=$(get_by_id_path "${backend_dev}")
    log "Cache by-id:   ${cache_by_id}"
    log "Backend by-id: ${backend_by_id}"

    log "Starting OpenCAS configuration..."
    log "  Cache device: ${cache_by_id} (${CACHE_SIZE_GB}GB)"
    log "  Core device: ${backend_by_id}"
    log "  Cache mode: ${CACHE_MODE}"
    log "  Cache line size: ${CACHE_LINE_SIZE}KB"

    # Clear any existing metadata on cache device
    log "Clearing old metadata from cache device..."
    sudo casadm --zero-metadata --device "${cache_by_id}" --force 2>/dev/null || true

    # Start cache with --force flag
    log "Creating cache instance (ID=${CACHE_ID})..."
    sudo casadm --start-cache \
        --cache-device "${cache_by_id}" \
        --cache-id ${CACHE_ID} \
        --cache-mode ${CACHE_MODE} \
        --cache-line-size ${CACHE_LINE_SIZE} \
        --force || {
        log "ERROR: Failed to start cache"
        log "Check: casadm --start-cache --help"
        exit 1
    }

    # Add core device
    log "Adding core device (ID=${CORE_ID})..."
    sudo casadm --add-core \
        --cache-id ${CACHE_ID} \
        --core-device "${backend_by_id}" \
        --core-id ${CORE_ID} || {
        log "ERROR: Failed to add core device"
        sudo casadm --stop-cache --cache-id ${CACHE_ID} --no-data-flush 2>/dev/null || true
        exit 1
    }

    log "OpenCAS configuration complete!"
    echo ""

    # Show configuration
    echo "=========================================="
    echo "  OpenCAS Configuration"
    echo "=========================================="
    casadm --list-caches
    echo ""
    echo "Exported device: /dev/cas${CACHE_ID}-${CORE_ID}"
    echo ""
    echo "=========================================="
}

# Show status
show_status() {
    echo ""
    log "OpenCAS Status:"
    casadm --list-caches
    echo ""
    log "Cache statistics:"
    casadm --stats --cache-id ${CACHE_ID} 2>/dev/null || true
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
    echo "  CACHE_BDF        Cache device BDF (default: 0000:)"
    echo "  BACKEND_BDF      Backend device BDF (default: 0000:)"
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
            setup_udev_rules
            stop_opencas
            pre_format_devices
            prefill_cache
            start_opencas
            set_nvme_irq_affinity
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
