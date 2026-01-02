#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
RPC_BIN=${RPC_BIN:-"$ROOT_DIR/scripts/rpc.py"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
RPC=("$RPC_BIN" "-s" "$RPC_SOCKET")
export PYTHONPATH="${PYTHONPATH:-}:$ROOT_DIR/python"

# FDP SSD as cache, regular SSD as backend
CACHE_BDF=${CACHE_BDF:-0000:06:00.0}
BACKEND_BDF=${BACKEND_BDF:-0000:07:00.0}
CACHE_CTRL=${CACHE_CTRL:-cache_ctrl}
BACKEND_CTRL=${BACKEND_CTRL:-backend_ctrl}
CACHE_NS=${CACHE_NS:-${CACHE_CTRL}n1}
BACKEND_NS=${BACKEND_NS:-${BACKEND_CTRL}n1}

# FDP cache size: 200GB (passed from run_tier_fdp.sh)
CACHE_SPLIT_GB=${CACHE_SPLIT_GB:-200}

MAX_PENDING_IO=${MAX_PENDING_IO:-64}
ICACHE_NAME=${ICACHE_NAME:-icache0}
# NVMe-oF TCP is primary (lower latency for local testing)
NVMF_ENABLE=${NVMF_ENABLE:-1}
# ublk is fallback if NVMF fails
UBLK_ENABLE=${UBLK_ENABLE:-0}
UBLK_DEV_ID=${UBLK_DEV_ID:-0}
NVMF_TRTYPE=${NVMF_TRTYPE:-tcp}
NVMF_ADRFAM=${NVMF_ADRFAM:-ipv4}
NVMF_TRADDR=${NVMF_TRADDR:-127.0.0.1}
NVMF_TRSVCID=${NVMF_TRSVCID:-4420}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:${ICACHE_NAME}}
NVMF_SERIAL=${NVMF_SERIAL:-ICACHE0001}
ICACHE_CACHE_TYPE=${ICACHE_CACHE_TYPE:-LOG_GREEDY}
ICACHE_WAF_LOG=${ICACHE_WAF_LOG:-$ROOT_DIR/ssd_waf/icache_waf.log}
ICACHE_STAT_LOG=${ICACHE_STAT_LOG:-}
ICACHE_VALID_RATE_THRESHOLD=${ICACHE_VALID_RATE_THRESHOLD:-0.9}

if [[ -n "$ICACHE_WAF_LOG" ]]; then
    mkdir -p "$(dirname "$ICACHE_WAF_LOG")"
    : > "$ICACHE_WAF_LOG"
fi

if [[ -n "$ICACHE_STAT_LOG" ]]; then
    mkdir -p "$(dirname "$ICACHE_STAT_LOG")"
fi

log() {
    echo "[create_tier_fdp] $*"
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
CACHE_SPLIT_MB=$((CACHE_SPLIT_GB * 1024))
if (( CACHE_SPLIT_MB <= 0 )); then
    echo "Invalid CACHE_SPLIT_GB ($CACHE_SPLIT_GB)" >&2
    exit 1
fi

log "FDP Mode: Cache ${CACHE_SPLIT_GB}GB, Backend full capacity"

log "Attaching cache controller ${CACHE_CTRL} at ${CACHE_BDF}"
if ! rpc_call "attach cache controller ${CACHE_CTRL}" \
	bdev_nvme_attach_controller -b "$CACHE_CTRL" -t pcie -a "$CACHE_BDF"; then
	log "Ignoring attach failure (controller may already exist)"
fi
sleep 2

log "Attaching backend controller ${BACKEND_CTRL} at ${BACKEND_BDF}"
if ! rpc_call "attach backend controller ${BACKEND_CTRL}" \
	bdev_nvme_attach_controller -b "$BACKEND_CTRL" -t pcie -a "$BACKEND_BDF"; then
	log "Ignoring attach failure (controller may already exist)"
fi
sleep 2

# Use full namespace (FDP doesn't use zones, cache size enforced in icache module)
CACHE_DEVICE=${CACHE_NS}
log "Using FDP cache namespace ${CACHE_DEVICE} (limit ${CACHE_SPLIT_GB}GB enforced in icache)"

cat <<MSG
[create_tier_fdp] Done.
 Cache bdev (FDP) : ${CACHE_DEVICE}
 Backend bdev     : ${BACKEND_NS}
 Max pending IO   : ${MAX_PENDING_IO}
 Cache policy     : ${ICACHE_CACHE_TYPE}
 WAF log path     : ${ICACHE_WAF_LOG}
Creating icache bdev "${ICACHE_NAME}" (cache=${CACHE_DEVICE}, backend=${BACKEND_NS})
MSG

ICACHE_RPC_ARGS=(
	bdev_icache_create
	--name "${ICACHE_NAME}"
	--cache-bdev "${CACHE_DEVICE}"
	--backend-bdev "${BACKEND_NS}"
	--max-pending-io "${MAX_PENDING_IO}"
	--cache-type "${ICACHE_CACHE_TYPE}"
	--waf-log-path "${ICACHE_WAF_LOG}"
	--valid-rate-threshold "${ICACHE_VALID_RATE_THRESHOLD}"
)

if [[ -n "${ICACHE_STAT_LOG}" ]]; then
	ICACHE_RPC_ARGS+=(--stat-log-path "${ICACHE_STAT_LOG}")
fi

rpc_call "create icache ${ICACHE_NAME}" "${ICACHE_RPC_ARGS[@]}"

# Track if device exposure succeeded
DEVICE_EXPOSED=0

# Primary: Expose icache bdev via NVMe-oF TCP
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
		if ! rpc_call "add namespace ${ICACHE_NAME} to ${NVMF_SUBSYSTEM}" \
			nvmf_subsystem_add_ns "${NVMF_SUBSYSTEM}" "${ICACHE_NAME}"; then
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

# Fallback: Expose icache bdev via ublk (if NVMF failed or UBLK_ENABLE=1)
UBLK_CPUMASK=${UBLK_CPUMASK:-0x1}  # Core 0 only for ublk

if [[ "${DEVICE_EXPOSED}" == "0" ]] || [[ "${UBLK_ENABLE}" != "0" ]]; then
	sleep 2
	if ! rpc_call "create ublk target on core 0" ublk_create_target -m "${UBLK_CPUMASK}"; then
		log "ublk_create_target failed (may already exist), continuing"
	fi
	sleep 1
	log "Starting ublk target for ${ICACHE_NAME} as /dev/ublkb${UBLK_DEV_ID}"
	if rpc_call "start ublk for ${ICACHE_NAME}" \
		ublk_start_disk "${ICACHE_NAME}" "${UBLK_DEV_ID}"; then
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
