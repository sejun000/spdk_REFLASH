#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
RPC_BIN=${RPC_BIN:-"$ROOT_DIR/scripts/rpc.py"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
RPC_TIMEOUT=${RPC_TIMEOUT:-3600}
RPC=("$RPC_BIN" "-s" "$RPC_SOCKET" "-t" "$RPC_TIMEOUT")
export PYTHONPATH="${PYTHONPATH:-}:$ROOT_DIR/python"

# FDP mode: same as icache tier
# Cache: 06:00.0 (FDP SSD)
# Backend (base device): 07:00.0 (regular SSD)
CACHE_BDF=${CACHE_BDF:-0000:06:00.0}
BACKEND_BDF=${BACKEND_BDF:-0000:07:00.0}
CACHE_CTRL=${CACHE_CTRL:-ftl_cache_ctrl}
BACKEND_CTRL=${BACKEND_CTRL:-ftl_backend_ctrl}
CACHE_NS=${CACHE_NS:-${CACHE_CTRL}n1}
BACKEND_NS=${BACKEND_NS:-${BACKEND_CTRL}n1}
FTL_NAME=${FTL_NAME:-ftl0}
# Cache split size in GB (100GB default)
CACHE_SPLIT_GB=${CACHE_SPLIT_GB:-200}
# Expose 93% of the backend capacity by default (7% FTL overprovisioning).
FTL_OVERPROV=${FTL_OVERPROV:-7}
FTL_L2P_DRAM=${FTL_L2P_DRAM:-8192}

# NVMe-oF TCP
NVMF_ENABLE=${NVMF_ENABLE:-1}
UBLK_ENABLE=${UBLK_ENABLE:-0}
UBLK_DEV_ID=${UBLK_DEV_ID:-0}
NVMF_TRTYPE=${NVMF_TRTYPE:-tcp}
NVMF_ADRFAM=${NVMF_ADRFAM:-ipv4}
NVMF_TRADDR=${NVMF_TRADDR:-127.0.0.1}
NVMF_TRSVCID=${NVMF_TRSVCID:-4420}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:${FTL_NAME}}
NVMF_SERIAL=${NVMF_SERIAL:-FTLBDEV0001}

log() {
    echo "[create_ftl] $*"
}

rpc_call() {
    local desc=$1
    shift
    log "RPC start: ${desc}"
    if "${RPC[@]}" "$@"; then
        log "RPC success: ${desc}"
        return 0
    else
        log "RPC FAILED: ${desc}"
        return 1
    fi
}

sleep 2

# Attach cache controller (FDP SSD)
log "Attaching cache controller ${CACHE_CTRL} at ${CACHE_BDF}"
if ! rpc_call "attach cache controller ${CACHE_CTRL}" \
    bdev_nvme_attach_controller -b "$CACHE_CTRL" -t pcie -a "$CACHE_BDF"; then
    log "Ignoring attach failure (controller may already exist)"
fi
sleep 2

# Split cache device if enabled (disabled by default)
if [[ "${CACHE_SPLIT_ENABLE:-0}" == "1" ]]; then
    CACHE_SPLIT_MB=$((CACHE_SPLIT_GB * 1024))
    CACHE_BDEV="${CACHE_NS}p0"
    log "Splitting cache ${CACHE_NS} to ${CACHE_SPLIT_GB}GB"
    if ! rpc_call "split cache bdev" \
        bdev_split_create "${CACHE_NS}" 1 -s "${CACHE_SPLIT_MB}"; then
        log "Split failed, using full namespace"
        CACHE_BDEV="${CACHE_NS}"
    fi
else
    CACHE_BDEV="${CACHE_NS}"
    log "Using full namespace (CACHE_SPLIT_ENABLE=0)"
fi

# Attach backend controller (regular SSD)
log "Attaching backend controller ${BACKEND_CTRL} at ${BACKEND_BDF}"
if ! rpc_call "attach backend controller ${BACKEND_CTRL}" \
    bdev_nvme_attach_controller -b "$BACKEND_CTRL" -t pcie -a "$BACKEND_BDF"; then
    log "Ignoring attach failure (controller may already exist)"
fi
sleep 2

cat <<MSG
[create_ftl] Configuration:
  Cache bdev:                ${CACHE_BDEV}
  Backend bdev (base):       ${BACKEND_NS}
  FTL name:                  ${FTL_NAME}
  Overprovisioning:          ${FTL_OVERPROV}%
  L2P DRAM limit:            ${FTL_L2P_DRAM} MiB
Creating FTL bdev "${FTL_NAME}"...
MSG

# Create FTL bdev (no --uuid → SPDK_FTL_MODE_CREATE → always fresh init)
# --base-bdev: base device (regular SSD)
# --cache: cache device (FDP SSD)
rpc_call "create FTL ${FTL_NAME}" \
    bdev_ftl_create \
    --name "${FTL_NAME}" \
    --base-bdev "${BACKEND_NS}" \
    --cache "${CACHE_BDEV}" \
    --overprovisioning "${FTL_OVERPROV}" \
    --l2p-dram-limit "${FTL_L2P_DRAM}"

# Track if device exposure succeeded
DEVICE_EXPOSED=0

# Primary: Expose FTL bdev via NVMe-oF TCP
if [[ "${NVMF_ENABLE}" != "0" ]]; then
    sleep 2
    NVMF_OK=1
    if ! rpc_call "create NVMe-oF transport ${NVMF_TRTYPE}" \
        nvmf_create_transport -t "${NVMF_TRTYPE}"; then
        log "Transport ${NVMF_TRTYPE} may already exist, continuing"
    fi
    sleep 2
    if ! rpc_call "create subsystem ${NVMF_SUBSYSTEM}" \
        nvmf_create_subsystem "${NVMF_SUBSYSTEM}" -a -s "${NVMF_SERIAL}"; then
        NVMF_OK=0
    fi
    if [[ "${NVMF_OK}" == "1" ]]; then
        sleep 2
        if ! rpc_call "add namespace ${FTL_NAME} to ${NVMF_SUBSYSTEM}" \
            nvmf_subsystem_add_ns "${NVMF_SUBSYSTEM}" "${FTL_NAME}"; then
            NVMF_OK=0
        fi
    fi
    if [[ "${NVMF_OK}" == "1" ]]; then
        sleep 2
        if ! rpc_call "add listener ${NVMF_SUBSYSTEM}@${NVMF_TRADDR}:${NVMF_TRSVCID}" \
            nvmf_subsystem_add_listener "${NVMF_SUBSYSTEM}" \
            -t "${NVMF_TRTYPE}" -f "${NVMF_ADRFAM}" -a "${NVMF_TRADDR}" -s "${NVMF_TRSVCID}"; then
            NVMF_OK=0
        fi
    fi
    if [[ "${NVMF_OK}" == "1" ]]; then
        log "NVMe-oF TCP ready: ${NVMF_SUBSYSTEM} @ ${NVMF_TRADDR}:${NVMF_TRSVCID}"
        DEVICE_EXPOSED=1
    else
        log "NVMe-oF TCP setup failed, will try ublk fallback"
    fi
fi

# Fallback: Expose FTL bdev via ublk
UBLK_CPUMASK=${UBLK_CPUMASK:-0x1}

if [[ "${DEVICE_EXPOSED}" == "0" ]] || [[ "${UBLK_ENABLE}" != "0" ]]; then
    sleep 2
    if ! rpc_call "create ublk target on core 0" ublk_create_target -m "${UBLK_CPUMASK}"; then
        log "ublk_create_target failed (may already exist), continuing"
    fi
    sleep 1
    log "Starting ublk target for ${FTL_NAME} as /dev/ublkb${UBLK_DEV_ID}"
    if rpc_call "start ublk for ${FTL_NAME}" \
        ublk_start_disk "${FTL_NAME}" "${UBLK_DEV_ID}"; then
        log "ublk device ready: /dev/ublkb${UBLK_DEV_ID}"
        DEVICE_EXPOSED=1
    else
        log "ublk_start_disk failed"
        if [[ "${DEVICE_EXPOSED}" == "0" ]]; then
            log "ERROR: No device exposure method succeeded!"
            exit 1
        fi
    fi
fi

log "FTL bdev setup complete"
