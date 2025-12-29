#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
RPC_BIN=${RPC_BIN:-"$ROOT_DIR/scripts/rpc.py"}
RPC_SOCKET=${SPDK_RPC_SOCKET:-/var/tmp/spdk.sock}
RPC=("$RPC_BIN" "-s" "$RPC_SOCKET")
export PYTHONPATH="${PYTHONPATH:-}:$ROOT_DIR/python"

# Null bdev settings (same size as icache for fair comparison)
NULL_BDEV_NAME=${NULL_BDEV_NAME:-icache0}
NULL_BDEV_SIZE_MB=${NULL_BDEV_SIZE_MB:-102400}  # 100GB
NULL_BLOCK_SIZE=${NULL_BLOCK_SIZE:-4096}

# NVMe-oF TCP settings
NVMF_TRTYPE=${NVMF_TRTYPE:-tcp}
NVMF_ADRFAM=${NVMF_ADRFAM:-ipv4}
NVMF_TRADDR=${NVMF_TRADDR:-127.0.0.1}
NVMF_TRSVCID=${NVMF_TRSVCID:-4420}
NVMF_SUBSYSTEM=${NVMF_SUBSYSTEM:-nqn.2024-11.io.spdk:icache0}
NVMF_SERIAL=${NVMF_SERIAL:-NULL0001}

log() {
    echo "[create_null] $*"
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

# Create null bdev (no icache, pure null device for baseline)
log "Creating null bdev ${NULL_BDEV_NAME} (${NULL_BDEV_SIZE_MB}MB, ${NULL_BLOCK_SIZE}B blocks)"
if ! rpc_call "create null bdev ${NULL_BDEV_NAME}" \
	bdev_null_create "${NULL_BDEV_NAME}" "${NULL_BDEV_SIZE_MB}" "${NULL_BLOCK_SIZE}"; then
	log "Ignoring null bdev creation failure (may already exist)"
fi
sleep 2

cat <<MSG
[create_null] Done.
 Bdev name   : ${NULL_BDEV_NAME} (NULL - no actual IO)
 Size        : ${NULL_BDEV_SIZE_MB}MB
 Block size  : ${NULL_BLOCK_SIZE}B
MSG

# Expose null bdev via NVMe-oF TCP
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
	if ! rpc_call "add namespace ${NULL_BDEV_NAME} to ${NVMF_SUBSYSTEM}" \
		nvmf_subsystem_add_ns "${NVMF_SUBSYSTEM}" "${NULL_BDEV_NAME}"; then
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
else
	log "ERROR: NVMe-oF TCP setup failed!"
	exit 1
fi
