#!/bin/bash

# Saturated Trace Replay - Alibaba trace를 최대 속도로 replay
# fio_icache.sh와 동일한 방식으로 디바이스 탐지
#
# Usage:
#   ./replay_trace.sh                              # 자동 디바이스 탐지
#   ./replay_trace.sh /path/to/trace.csv           # trace 파일 지정
#   DEVICE_TYPE=nvmeof ./replay_trace.sh           # 디바이스 타입 지정

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPLAY_PY="${SCRIPT_DIR}/replay_trace.py"

# Default trace file
TRACE_FILE=${TRACE_FILE:-/home/sejun000/alibaba_dwpd1.trace.head30p}

# Override with command line argument if provided
if [ -n "$1" ]; then
    TRACE_FILE="$1"
fi

# Device type: auto (default), ublk, nvmeof, opencas
DEVICE_TYPE=${DEVICE_TYPE:-auto}

UBLK_DEV_ID=${UBLK_DEV_ID:-0}
UBLK_DEVICE="/dev/ublkb${UBLK_DEV_ID}"

# OpenCAS device settings
CAS_CACHE_ID=${CAS_CACHE_ID:-1}
CAS_CORE_ID=${CAS_CORE_ID:-1}
CAS_DEVICE="/dev/cas${CAS_CACHE_ID}-${CAS_CORE_ID}"

# Replay settings
NUM_THREADS=${NUM_THREADS:-4}
QUEUE_DEPTH=${QUEUE_DEPTH:-64}

# 디바이스 검증 함수
verify_device() {
    local dev=$1
    local dev_type=$2

    if [ ! -e "$dev" ]; then
        return 1
    fi

    if [ ! -b "$dev" ]; then
        return 1
    fi

    local size=$(sudo blockdev --getsize64 "$dev" 2>/dev/null)
    if [ -z "$size" ] || [ "$size" -eq 0 ]; then
        return 1
    fi

    local size_gb=$((size / 1024 / 1024 / 1024))
    echo "${dev_type} 디바이스 확인됨: $dev (${size_gb}GB)"
    return 0
}

# 디바이스 탐지
find_device() {
    case "$DEVICE_TYPE" in
        ublk)
            if verify_device "$UBLK_DEVICE" "ublk"; then
                DEVICE="$UBLK_DEVICE"
            else
                echo "ERROR: ublk 디바이스를 찾을 수 없습니다: $UBLK_DEVICE"
                exit 1
            fi
            ;;
        nvmeof)
            DEVICE=$(sudo nvme list 2>/dev/null | grep -iE "ICACHE|FTLBDEV|NULL|OCF" | awk '{print $1}')
            if [ -z "$DEVICE" ]; then
                echo "ERROR: NVMe-oF 디바이스를 찾을 수 없습니다 (ICACHE/FTLBDEV/NULL/OCF)"
                exit 1
            fi
            if ! verify_device "$DEVICE" "NVMe-oF"; then
                exit 1
            fi
            ;;
        opencas)
            if ! verify_device "$CAS_DEVICE" "OpenCAS"; then
                echo "ERROR: OpenCAS 디바이스를 찾을 수 없습니다: $CAS_DEVICE"
                exit 1
            fi
            DEVICE="$CAS_DEVICE"
            ;;
        auto|*)
            # 자동 탐지: ublk > nvmeof > opencas 순서
            if [ -e "$UBLK_DEVICE" ] && verify_device "$UBLK_DEVICE" "ublk" 2>/dev/null; then
                DEVICE="$UBLK_DEVICE"
                echo "자동 탐지: ublk 디바이스 사용"
            else
                DEVICE=$(sudo nvme list 2>/dev/null | grep -iE "ICACHE|FTLBDEV|NULL|OCF" | awk '{print $1}')
                if [ -n "$DEVICE" ] && verify_device "$DEVICE" "NVMe-oF" 2>/dev/null; then
                    echo "자동 탐지: NVMe-oF 디바이스 사용"
                elif [ -e "$CAS_DEVICE" ] && verify_device "$CAS_DEVICE" "OpenCAS" 2>/dev/null; then
                    DEVICE="$CAS_DEVICE"
                    echo "자동 탐지: OpenCAS 디바이스 사용"
                else
                    echo "ERROR: 사용 가능한 디바이스를 찾을 수 없습니다"
                    echo ""
                    echo "확인된 위치:"
                    echo "  ublk:    $UBLK_DEVICE ($([ -e "$UBLK_DEVICE" ] && echo '존재' || echo '없음'))"
                    echo "  NVMe-oF: ICACHE/FTLBDEV/NULL/OCF (없음)"
                    echo "  OpenCAS: $CAS_DEVICE ($([ -e "$CAS_DEVICE" ] && echo '존재' || echo '없음'))"
                    exit 1
                fi
            fi
            ;;
    esac
}

# Check trace file
if [ ! -f "$TRACE_FILE" ]; then
    echo "ERROR: Trace 파일을 찾을 수 없습니다: $TRACE_FILE"
    exit 1
fi

find_device

echo ""
echo "=========================================="
echo "  Saturated Trace Replay"
echo "=========================================="
echo "  Device:    $DEVICE"
echo "  Trace:     $TRACE_FILE"
echo "  Threads:   $NUM_THREADS"
echo "  QD/thread: $QUEUE_DEPTH"
echo "=========================================="
echo ""

# Run replay
sudo python3 "$REPLAY_PY" "$DEVICE" \
    --trace "$TRACE_FILE" \
    --threads "$NUM_THREADS" \
    --qd "$QUEUE_DEPTH"
