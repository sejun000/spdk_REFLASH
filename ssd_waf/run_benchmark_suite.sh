#!/usr/bin/env bash
set -euo pipefail

# Comprehensive Benchmark Suite
# - 3 workloads: uniform, zipf, hotcold
# - 2 configurations: FTL (baseline), FDP (LOG_GREEDY_COST_BENEFIT_10)
# - Sequential fill + random write/read performance
# - Output: 12 JSON files + syslog for FDP runs

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SCRIPT_DIR="$ROOT_DIR/ssd_waf"
RESULT_DIR="${RESULT_DIR:-$SCRIPT_DIR/benchmark_results_$(date +%Y%m%d_%H%M%S)}"

# Test parameters
TEST_SIZE=${TEST_SIZE:-200G}       # Sequential fill size
RUNTIME=${RUNTIME:-300}            # Random workload runtime (seconds)
WORKLOADS=("uniform" "zipf" "hotcold")

log() {
    echo "[benchmark] $(date '+%Y-%m-%d %H:%M:%S') $*"
}

mkdir -p "$RESULT_DIR"
log "Results will be saved to: $RESULT_DIR"

# Function to stop FTL
stop_ftl() {
    log "Stopping FTL..."
    cd "$SCRIPT_DIR"
    ./exit_ftl.sh || true
    sleep 2
}

# Function to stop FDP (icache)
stop_fdp() {
    log "Stopping FDP (icache)..."
    cd "$SCRIPT_DIR"
    ./exit_tgt.sh || true
    sleep 2
}

# Function to stop any running SPDK (fallback)
stop_spdk() {
    log "Stopping any running SPDK processes..."
    sudo pkill -f spdk_tgt || true
    sudo nvme disconnect-all 2>/dev/null || true
    sleep 3
}

# Function to find device
find_device() {
    local ublk_dev="/dev/ublkb0"
    if [ -e "$ublk_dev" ]; then
        echo "$ublk_dev"
        return 0
    fi
    local nvme_dev=$(sudo nvme list 2>/dev/null | grep -iE "ICACHE|FTLBDEV|NULL" | awk '{print $1}' | head -1)
    if [ -n "$nvme_dev" ]; then
        echo "$nvme_dev"
        return 0
    fi
    return 1
}

# Function to wait for device
wait_for_device() {
    local timeout=${1:-60}
    local waited=0
    log "Waiting for device..."
    while ! find_device >/dev/null 2>&1; do
        if (( waited >= timeout )); then
            log "ERROR: Device not found after ${timeout}s"
            return 1
        fi
        sleep 2
        ((waited+=2))
    done
    local dev=$(find_device)
    log "Device found: $dev"
    echo "$dev"
}

# Function to run sequential fill
run_seq_fill() {
    local device=$1
    local output_prefix=$2

    log "Running sequential fill (${TEST_SIZE})..."
    sudo fio --name=seq_fill \
        --filename="$device" \
        --ioengine=libaio \
        --direct=1 \
        --bs=128k \
        --rw=write \
        --size=${TEST_SIZE} \
        --numjobs=1 \
        --iodepth=32 \
        --group_reporting \
        --output-format=json \
        --output="${output_prefix}_seqfill.json"

    log "Sequential fill complete"
}

# Function to run random write test
run_random_write() {
    local device=$1
    local workload=$2
    local output_prefix=$3

    local random_dist=""
    case "$workload" in
        zipf)
            random_dist="--random_distribution=zipf:1.2"
            ;;
        hotcold)
            random_dist="--random_distribution=zoned:90/10:10/90"
            ;;
        uniform|*)
            random_dist=""
            ;;
    esac

    log "Running random write test (workload=${workload}, runtime=${RUNTIME}s)..."
    sudo fio --name=random_write \
        --filename="$device" \
        --ioengine=libaio \
        --direct=1 \
        --bs=4k \
        --rw=randwrite \
        --size=${TEST_SIZE} \
        --runtime=${RUNTIME} \
        --time_based \
        --numjobs=4 \
        --iodepth=32 \
        --group_reporting \
        --output-format=json \
        --output="${output_prefix}_randwrite.json" \
        ${random_dist}

    log "Random write test complete"
}

# Function to run random read test
run_random_read() {
    local device=$1
    local workload=$2
    local output_prefix=$3

    local random_dist=""
    case "$workload" in
        zipf)
            random_dist="--random_distribution=zipf:1.2"
            ;;
        hotcold)
            random_dist="--random_distribution=zoned:90/10:10/90"
            ;;
        uniform|*)
            random_dist=""
            ;;
    esac

    log "Running random read test (workload=${workload}, runtime=${RUNTIME}s)..."
    sudo fio --name=random_read \
        --filename="$device" \
        --ioengine=libaio \
        --direct=1 \
        --bs=4k \
        --rw=randread \
        --size=${TEST_SIZE} \
        --runtime=${RUNTIME} \
        --time_based \
        --numjobs=4 \
        --iodepth=32 \
        --group_reporting \
        --output-format=json \
        --output="${output_prefix}_randread.json" \
        ${random_dist}

    log "Random read test complete"
}

# Function to save syslog from "reactor" marker
save_syslog_from_reactor() {
    local output_file=$1
    log "Saving syslog from 'reactor' marker to: $output_file"

    # Find line number of most recent "reactor" occurrence
    local reactor_line=$(grep -n "reactor" /var/log/syslog | tail -1 | cut -d: -f1)

    if [ -n "$reactor_line" ]; then
        sudo tail -n +${reactor_line} /var/log/syslog > "$output_file"
        log "Saved $(wc -l < "$output_file") lines of syslog"
    else
        log "WARNING: 'reactor' not found in syslog, saving last 10000 lines"
        sudo tail -n 10000 /var/log/syslog > "$output_file"
    fi
}

# Function to extract performance from JSON
extract_perf() {
    local json_file=$1
    if [ -f "$json_file" ]; then
        local bw=$(python3 -c "import json; d=json.load(open('$json_file')); print(d['jobs'][0]['write']['bw']/1024 if 'write' in d['jobs'][0] and d['jobs'][0]['write']['bw'] > 0 else d['jobs'][0]['read']['bw']/1024)" 2>/dev/null || echo "N/A")
        local iops=$(python3 -c "import json; d=json.load(open('$json_file')); print(d['jobs'][0]['write']['iops'] if 'write' in d['jobs'][0] and d['jobs'][0]['write']['iops'] > 0 else d['jobs'][0]['read']['iops'])" 2>/dev/null || echo "N/A")
        echo "BW: ${bw} MB/s, IOPS: ${iops}"
    else
        echo "N/A"
    fi
}

#######################################
# Main benchmark execution
#######################################

log "=========================================="
log "  Benchmark Suite Starting"
log "  TEST_SIZE: ${TEST_SIZE}"
log "  RUNTIME: ${RUNTIME}s"
log "  WORKLOADS: ${WORKLOADS[*]}"
log "=========================================="

# ==========================================
# Part 1: FTL Baseline Tests
# ==========================================
log ""
log "########## PART 1: FTL BASELINE ##########"
log ""

for workload in "${WORKLOADS[@]}"; do
    log "=========================================="
    log "  FTL - Workload: ${workload}"
    log "=========================================="

    # Stop any existing SPDK
    stop_spdk

    # Start FTL
    log "Starting FTL (run_ftl.sh)..."
    cd "$SCRIPT_DIR"
    SKIP_PRE_FORMAT=1 ./run_ftl.sh <<< "y" || {
        log "FTL startup failed, trying with format..."
        SKIP_PRE_FORMAT=0 ./run_ftl.sh <<< "y"
    }

    # Wait for device
    device=$(wait_for_device 60) || {
        log "ERROR: Device not available for FTL-${workload}"
        continue
    }

    output_prefix="${RESULT_DIR}/ftl_${workload}"

    # Sequential fill
    run_seq_fill "$device" "$output_prefix"

    # Random write
    run_random_write "$device" "$workload" "$output_prefix"

    # Random read
    run_random_read "$device" "$workload" "$output_prefix"

    log "FTL-${workload} complete"

    # Stop FTL for next test
    stop_ftl
done

# ==========================================
# Part 2: FDP (LOG_GREEDY_COST_BENEFIT_10) Tests
# ==========================================
log ""
log "########## PART 2: FDP TIER (ICACHE) ##########"
log ""

for workload in "${WORKLOADS[@]}"; do
    log "=========================================="
    log "  FDP - Workload: ${workload}"
    log "=========================================="

    # Stop any existing SPDK
    stop_spdk

    # Start FDP tier with LOG_GREEDY_COST_BENEFIT_10
    log "Starting FDP tier (run_tier_fdp.sh with LOG_GREEDY_COST_BENEFIT_10)..."
    cd "$SCRIPT_DIR"
    SKIP_PRE_FORMAT=1 sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_10 ./run_tier_fdp.sh <<< "y" || {
        log "FDP startup failed, trying with format..."
        SKIP_PRE_FORMAT=0 sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_10 ./run_tier_fdp.sh <<< "y"
    }

    # Wait for device
    device=$(wait_for_device 60) || {
        log "ERROR: Device not available for FDP-${workload}"
        continue
    }

    output_prefix="${RESULT_DIR}/fdp_${workload}"

    # Sequential fill
    run_seq_fill "$device" "$output_prefix"

    # Random write
    run_random_write "$device" "$workload" "$output_prefix"

    # Random read
    run_random_read "$device" "$workload" "$output_prefix"

    # Save syslog for FDP run
    save_syslog_from_reactor "${RESULT_DIR}/fdp_${workload}_syslog.txt"

    log "FDP-${workload} complete"

    # Stop FDP for next test
    stop_fdp
done

# ==========================================
# Summary
# ==========================================
log ""
log "=========================================="
log "  Benchmark Suite Complete!"
log "=========================================="
log ""
log "Results saved to: $RESULT_DIR"
log ""
log "Files generated:"
ls -la "$RESULT_DIR"
log ""

# Print summary table
log "Performance Summary:"
log "===================="
printf "%-20s %-15s %-30s %-30s\n" "Config" "Workload" "Write Performance" "Read Performance"
printf "%-20s %-15s %-30s %-30s\n" "------" "--------" "-----------------" "----------------"

for config in "ftl" "fdp"; do
    for workload in "${WORKLOADS[@]}"; do
        write_json="${RESULT_DIR}/${config}_${workload}_randwrite.json"
        read_json="${RESULT_DIR}/${config}_${workload}_randread.json"

        write_perf=$(extract_perf "$write_json")
        read_perf=$(extract_perf "$read_json")

        printf "%-20s %-15s %-30s %-30s\n" "$config" "$workload" "$write_perf" "$read_perf"
    done
done

log ""
log "Done!"
