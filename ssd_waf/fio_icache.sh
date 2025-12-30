#!/bin/bash

# icache 디바이스를 찾아 fio 실행
# ublk (/dev/ublkb0) 또는 NVMe-oF (ICACHE) 사용

UBLK_DEV_ID=${UBLK_DEV_ID:-0}
UBLK_DEVICE="/dev/ublkb${UBLK_DEV_ID}"
RUNTIME=${RUNTIME:-120}  # Default 2 minutes
LOG_PREFIX="fio_bw_$(date +%Y%m%d_%H%M%S)"

# ublk 디바이스 우선 확인
if [ -e "$UBLK_DEVICE" ]; then
    DEVICE="$UBLK_DEVICE"
    echo "ublk 디바이스 발견: $DEVICE"
else
    # Fallback: NVMe-oF 디바이스 검색 (ICACHE 또는 NULL)
    DEVICE=$(sudo nvme list 2>/dev/null | grep -iE "ICACHE|NULL" | awk '{print $1}')
    if [ -z "$DEVICE" ]; then
        echo "디바이스를 찾을 수 없습니다. (ublk: $UBLK_DEVICE, NVMe: ICACHE/NULL)"
        exit 1
    fi
    echo "NVMe-oF 디바이스 발견: $DEVICE"
fi

echo "fio random 4k write ${RUNTIME}s 시작..."
echo "BW log: ${LOG_PREFIX}_bw.*.log"

sudo fio --name=random_test \
    --filename="$DEVICE" \
    --ioengine=libaio \
    --direct=1 \
    --bs=4k \
    --rw=randwrite \
    --runtime=${RUNTIME} \
    --time_based \
    --numjobs=4 \
    --iodepth=32 \
    --group_reporting \
    --write_bw_log=${LOG_PREFIX} \
    --log_avg_msec=1000

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
bw_mibs = [times[t] / 1024 for t in sorted_times]

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