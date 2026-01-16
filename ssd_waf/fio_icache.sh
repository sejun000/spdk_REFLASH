#!/bin/bash

# icache 디바이스를 찾아 fio 실행 + 검증
# ublk (/dev/ublkb0), NVMe-oF (ICACHE), 또는 OpenCAS (/dev/cas1-1) 사용
#
# Usage:
#   ./fio_icache.sh                    # 자동 디바이스 탐지 (ublk > nvmeof > opencas)
#   DEVICE_TYPE=opencas ./fio_icache.sh  # OpenCAS 디바이스만 사용
#   DEVICE_TYPE=ublk ./fio_icache.sh     # ublk 디바이스만 사용
#   DEVICE_TYPE=nvmeof ./fio_icache.sh   # NVMe-oF 디바이스만 사용

# Device type: auto (default), ublk, nvmeof, opencas
DEVICE_TYPE=${DEVICE_TYPE:-auto}

UBLK_DEV_ID=${UBLK_DEV_ID:-0}
UBLK_DEVICE="/dev/ublkb${UBLK_DEV_ID}"

# OpenCAS device settings
CAS_CACHE_ID=${CAS_CACHE_ID:-1}
CAS_CORE_ID=${CAS_CORE_ID:-1}
CAS_DEVICE="/dev/cas${CAS_CACHE_ID}-${CAS_CORE_ID}"

RUNTIME=${RUNTIME:-200}  # Default 2 minutes
TEST_SIZE=${TEST_SIZE:-1700G}  # 검증용 테스트 크기
VERIFY_ONLY=${VERIFY_ONLY:-0}  # 1이면 검증만 수행
SKIP_VERIFY=${SKIP_VERIFY:-0}  # 1이면 검증 스킵
LOG_PREFIX="fio_bw_$(date +%Y%m%d_%H%M%S)"

# Workload type: uniform (default), zipf, hotcold
# uniform: 균등 분포 random write
# zipf: Zipf 분포 (theta=1.2, 일부 영역에 집중)
# hotcold: 90% writes → 10% 영역 (hot), 10% writes → 90% 영역 (cold)
WORKLOAD=${WORKLOAD:-uniform}

# Zipf theta parameter (higher = more skewed, 1.2 is typical)
ZIPF_THETA=${ZIPF_THETA:-1.2}

# Get random distribution option based on workload type
get_random_distribution() {
    case "$WORKLOAD" in
        zipf)
            echo "--random_distribution=zipf:${ZIPF_THETA}"
            ;;
        hotcold)
            # 90% of IO goes to first 10% of space, 10% of IO goes to remaining 90%
            echo "--random_distribution=zoned:90/10:10/90"
            ;;
        uniform|*)
            echo ""  # Default uniform distribution
            ;;
    esac
}

RANDOM_DIST=$(get_random_distribution)
echo "Workload type: ${WORKLOAD}"
if [ -n "$RANDOM_DIST" ]; then
    echo "Random distribution: ${RANDOM_DIST}"
fi

# Verify 모드에서는 single thread 사용 (multi-job race condition 방지)
# multi-job으로 같은 offset에 쓰면 header는 A job, data는 B job 데이터가 될 수 있음
if [ "${SKIP_VERIFY}" != "1" ] || [ "${VERIFY_ONLY}" == "1" ]; then
    NUMJOBS=1
    IODEPTH=128  # single thread라서 iodepth 높임
    echo "Verify mode: numjobs=1, iodepth=128 (single thread for data integrity)"
else
    NUMJOBS=4
    IODEPTH=32
fi

# 디바이스 검증 함수
verify_device() {
    local dev=$1
    local dev_type=$2

    if [ ! -e "$dev" ]; then
        echo "ERROR: ${dev_type} 디바이스가 존재하지 않습니다: $dev"
        return 1
    fi

    # 블록 디바이스인지 확인
    if [ ! -b "$dev" ]; then
        echo "ERROR: ${dev} 는 블록 디바이스가 아닙니다"
        return 1
    fi

    # 디바이스 크기 확인 (0이면 문제)
    local size=$(sudo blockdev --getsize64 "$dev" 2>/dev/null)
    if [ -z "$size" ] || [ "$size" -eq 0 ]; then
        echo "ERROR: ${dev} 디바이스 크기를 읽을 수 없거나 0입니다"
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
                echo ""
                echo "OpenCAS가 실행중인지 확인하세요:"
                echo "  casadm -L"
                echo "  ./run_opencas.sh --start"
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
                    echo ""
                    echo "디바이스 타입을 지정하세요:"
                    echo "  DEVICE_TYPE=opencas $0"
                    echo "  DEVICE_TYPE=ublk $0"
                    exit 1
                fi
            fi
            ;;
    esac
}

find_device
echo ""
echo "테스트 대상 디바이스: $DEVICE"
echo ""

# 검증만 수행 모드
if [ "${VERIFY_ONLY}" == "1" ]; then
    echo "=========================================="
    echo "  검증만 수행 (이전 write 데이터 확인)"
    echo "=========================================="
    sudo fio --name=verify_only \
        --filename="$DEVICE" \
        --ioengine=libaio \
        --direct=1 \
        --bs=4k \
        --rw=randread \
        --size=${TEST_SIZE} \
        --numjobs=${NUMJOBS} \
        --iodepth=${IODEPTH} \
        --verify=crc32c \
        --verify_only \
        --group_reporting

    if [ $? -eq 0 ]; then
        echo "검증 성공!"
    else
        echo "검증 실패!"
        exit 1
    fi
    exit 0
fi

echo "=========================================="
echo "  Phase 1: Write with verification pattern"
echo "  Workload: ${WORKLOAD}"
echo "=========================================="

# SKIP_VERIFY일 때만 runtime/time_based 사용 (verify 시에는 size만큼 한번만 write)
if [ "${SKIP_VERIFY}" == "1" ]; then
    TIME_OPTS="--runtime=${RUNTIME} --time_based"
    echo "fio random 4k write ${RUNTIME}s (${WORKLOAD} distribution)..."
else
    TIME_OPTS=""
    echo "fio random 4k write ${TEST_SIZE} (${WORKLOAD} distribution, no time limit for verify)..."
fi
echo "BW log: ${LOG_PREFIX}_bw.*.log"

sudo fio --name=random_test \
    --filename="$DEVICE" \
    --ioengine=libaio \
    --direct=1 \
    --bs=4k \
    --rw=randwrite \
    --size=${TEST_SIZE} \
    ${TIME_OPTS} \
    --numjobs=${NUMJOBS} \
    --iodepth=${IODEPTH} \
    --verify=crc32c \
    --do_verify=0 \
    --group_reporting \
    --write_bw_log=${LOG_PREFIX} \
    --log_avg_msec=1000 \
    ${RANDOM_DIST}

WRITE_STATUS=$?

if [ ${WRITE_STATUS} -ne 0 ]; then
    echo "Write 실패!"
    exit 1
fi

# 검증 스킵 옵션
if [ "${SKIP_VERIFY}" == "1" ]; then
    echo "검증 스킵 (SKIP_VERIFY=1)"
else
    echo ""
    echo "=========================================="
    echo "  Phase 2: Read and Verify"
    echo "=========================================="
    echo "쓴 데이터 읽어서 검증 중..."

    sudo fio --name=verify_read \
        --filename="$DEVICE" \
        --ioengine=libaio \
        --direct=1 \
        --bs=4k \
        --rw=randread \
        --size=${TEST_SIZE} \
        --numjobs=${NUMJOBS} \
        --iodepth=${IODEPTH} \
        --verify=crc32c \
        --verify_only \
        --group_reporting

    VERIFY_STATUS=$?

    if [ ${VERIFY_STATUS} -eq 0 ]; then
        echo ""
        echo "=========================================="
        echo "  검증 성공! 데이터 정합성 확인됨"
        echo "=========================================="
    else
        echo ""
        echo "=========================================="
        echo "  검증 실패! 데이터 불일치 발생"
        echo "=========================================="
        exit 1
    fi
fi

# Generate graph if python available
if command -v python3 &> /dev/null; then
    echo "Generating bandwidth graph..."
    python3 << 'PYTHON_SCRIPT'
import glob
import sys

# Find the log file
log_files = glob.glob("fio_bw_*_bw.*.log")
if not log_files:
    print("No bw log files found")
    sys.exit(1)

# Parse and aggregate all job logs
times = {}
for log_file in log_files:
    with open(log_file, 'r') as f:
        for line in f:
            parts = line.strip().split(',')
            if len(parts) >= 2:
                time_ms = int(parts[0])
                bw_kb = int(parts[1])
                time_sec = time_ms // 1000
                if time_sec not in times:
                    times[time_sec] = 0
                times[time_sec] += bw_kb

if not times:
    print("No data in log files")
    sys.exit(1)

# Convert KiB/s to MiB/s (matches fio display)
sorted_times = sorted(times.keys())
bw_mbs = [times[t] / 1024 for t in sorted_times]

# Print stats
avg_bw = sum(bw_mbs) / len(bw_mbs)
max_bw = max(bw_mbs)
min_bw = min(bw_mbs)
print(f"\nBandwidth Statistics:")
print(f"  Average: {avg_bw:.1f} MB/s")
print(f"  Max: {max_bw:.1f} MB/s")
print(f"  Min: {min_bw:.1f} MB/s")

# Try to plot with matplotlib
try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import os

    # Get timestamp from log filename
    timestamp = log_files[0].split('_bw')[0].replace('fio_bw_', '')

    plt.figure(figsize=(12, 6))
    plt.plot(sorted_times, bw_mbs, 'b-', linewidth=0.8)
    plt.xlabel('Time (seconds)')
    plt.ylabel('Bandwidth (MB/s)')
    plt.title(f'FIO Random 4K Write Bandwidth - {timestamp}')
    plt.grid(True, alpha=0.3)

    # Save plot with timestamp
    plot_file = f"fio_bw_graph_{timestamp}.png"
    plt.savefig(plot_file, dpi=150, bbox_inches='tight')
    print(f"\nGraph saved: {plot_file}")
    plt.close()
except ImportError:
    print("\nmatplotlib not available - printing ASCII graph instead")
    # Simple ASCII graph
    width = 60
    height = 20
    max_val = max(bw_mbs) if bw_mbs else 1

    print(f"\n{'='*width}")
    print(f"Bandwidth over time (MB/s)")
    print(f"{'='*width}")

    # Sample data if too many points
    step = max(1, len(bw_mbs) // width)
    sampled = bw_mbs[::step][:width]

    for row in range(height, 0, -1):
        threshold = max_val * row / height
        line = ""
        for val in sampled:
            if val >= threshold:
                line += "█"
            else:
                line += " "
        print(f"{threshold:6.0f} |{line}|")
    print(f"       +{'-'*len(sampled)}+")
    print(f"        0{' '*(len(sampled)-6)}time->{len(sorted_times)}s")

PYTHON_SCRIPT
fi

echo "Done!"