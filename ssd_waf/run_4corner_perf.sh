#!/usr/bin/env bash
set -euo pipefail

# Random RW Performance Test
# - Random Write (4k), Random Read (4k)
# - 2 configurations: FTL (baseline), FDP (LOG_GREEDY_COST_BENEFIT_10)

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
SCRIPT_DIR="$ROOT_DIR/ssd_waf"
RESULT_DIR="${RESULT_DIR:-$SCRIPT_DIR/4corner_results_$(date +%Y%m%d_%H%M%S)}"

# Test parameters
TEST_SIZE=${TEST_SIZE:-800G}
RUNTIME_WRITE=${RUNTIME_WRITE:-400}
RUNTIME_READ=${RUNTIME_READ:-100}
NUMJOBS=${NUMJOBS:-4}
IODEPTH=${IODEPTH:-32}

log() {
    echo "[4corner] $(date '+%Y-%m-%d %H:%M:%S') $*" >&2
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

# Function to find device (same as fio_icache.sh)
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

# Function to run fio test
run_fio_test() {
    local device=$1
    local test_name=$2
    local rw=$3
    local bs=$4
    local runtime=$5
    local output_file=$6
    local extra_opts=${7:-}

    log "Running ${test_name} (bs=${bs}, runtime=${runtime}s)..."
    sudo fio --name="${test_name}" \
        --filename="$device" \
        --ioengine=libaio \
        --direct=1 \
        --bs=${bs} \
        --rw=${rw} \
        --size=${TEST_SIZE} \
        --runtime=${runtime} \
        --time_based \
        --numjobs=${NUMJOBS} \
        --iodepth=${IODEPTH} \
        --group_reporting \
        --output-format=json \
        --output="${output_file}" \
        ${extra_opts}

    log "${test_name} complete"
}

# Function to save syslog from "reactor" marker
save_syslog_from_reactor() {
    local output_file=$1
    log "Saving syslog from 'reactor' marker to: $output_file"

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
    local rw_type=$2  # "read" or "write"
    if [ -f "$json_file" ]; then
        local bw=$(python3 -c "import json; d=json.load(open('$json_file')); print(f\"{d['jobs'][0]['${rw_type}']['bw']/1024:.1f}\")" 2>/dev/null || echo "N/A")
        local iops=$(python3 -c "import json; d=json.load(open('$json_file')); print(f\"{d['jobs'][0]['${rw_type}']['iops']:.0f}\")" 2>/dev/null || echo "N/A")
        echo "${bw} MB/s, ${iops} IOPS"
    else
        echo "N/A"
    fi
}

#######################################
# Main benchmark execution
#######################################

log "=========================================="
log "  Random RW Performance Test"
log "  TEST_SIZE: ${TEST_SIZE}"
log "  RUNTIME_WRITE: ${RUNTIME_WRITE}s"
log "  RUNTIME_READ: ${RUNTIME_READ}s"
log "  NUMJOBS: ${NUMJOBS}, IODEPTH: ${IODEPTH}"
log "=========================================="

# Function to start FTL
start_ftl() {
    stop_spdk
    log "Starting FTL (run_ftl.sh)..."
    cd "$SCRIPT_DIR"
    SKIP_PRE_FORMAT=1 ./run_ftl.sh <<< "y" || {
        log "FTL startup failed, trying with format..."
        SKIP_PRE_FORMAT=0 ./run_ftl.sh <<< "y"
    }
    wait_for_device 60 || {
        log "ERROR: Device not available for FTL"
        return 1
    }
}

# Function to start FDP
start_fdp() {
    stop_spdk
    log "Starting FDP tier (run_tier_fdp.sh with LOG_GREEDY_COST_BENEFIT_10)..."
    cd "$SCRIPT_DIR"
    SKIP_PRE_FORMAT=1 sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_10 ./run_tier_fdp.sh <<< "y" || {
        log "FDP startup failed, trying with format..."
        SKIP_PRE_FORMAT=0 sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_10 ./run_tier_fdp.sh <<< "y"
    }
    wait_for_device 60 || {
        log "ERROR: Device not available for FDP"
        return 1
    }
}

# Function to run a single test with restart
run_ftl_single_test() {
    local test_name=$1
    local rw=$2
    local bs=$3
    local runtime=$4
    local output_file=$5
    local extra_opts=${6:-}

    log "--- FTL: ${test_name} (restart) ---"
    start_ftl || exit 1
    local device=$(find_device)
    run_fio_test "$device" "$test_name" "$rw" "$bs" "$runtime" "$output_file" "$extra_opts"
    stop_ftl
}

run_fdp_single_test() {
    local test_name=$1
    local rw=$2
    local bs=$3
    local runtime=$4
    local output_file=$5
    local extra_opts=${6:-}

    log "--- FDP: ${test_name} (restart) ---"
    start_fdp || exit 1
    local device=$(find_device)
    run_fio_test "$device" "$test_name" "$rw" "$bs" "$runtime" "$output_file" "$extra_opts"
    save_syslog_from_reactor "${output_file%.json}_syslog.txt"
    stop_fdp
}

# ==========================================
# Part 1: FTL Baseline Tests (each test restarts FTL)
# ==========================================
log ""
log "########## PART 1: FTL BASELINE ##########"
log ""

# Random Write - Uniform (4k)
run_ftl_single_test "randwrite_uniform" "randwrite" "4k" "${RUNTIME_WRITE}" "${RESULT_DIR}/ftl_randwrite_uniform.json"

# Random Write - Zipf 1.3 (4k)
run_ftl_single_test "randwrite_zipf" "randwrite" "4k" "${RUNTIME_WRITE}" "${RESULT_DIR}/ftl_randwrite_zipf.json" "--random_distribution=zipf:1.3"

# Random Write - HotCold 90/10 (4k)
run_ftl_single_test "randwrite_hotcold" "randwrite" "4k" "${RUNTIME_WRITE}" "${RESULT_DIR}/ftl_randwrite_hotcold.json" "--random_distribution=zoned:90/10:10/90"

# Random Read (4k)
run_ftl_single_test "randread_4k" "randread" "4k" "${RUNTIME_READ}" "${RESULT_DIR}/ftl_randread_4k.json"

log "FTL tests complete"

# ==========================================
# Part 2: FDP (LOG_GREEDY_COST_BENEFIT_10) Tests (each test restarts FDP)
# ==========================================
log ""
log "########## PART 2: FDP TIER (ICACHE) ##########"
log ""

# Random Write - Uniform (4k)
run_fdp_single_test "randwrite_uniform" "randwrite" "4k" "${RUNTIME_WRITE}" "${RESULT_DIR}/fdp_randwrite_uniform.json"

# Random Write - Zipf 1.3 (4k)
run_fdp_single_test "randwrite_zipf" "randwrite" "4k" "${RUNTIME_WRITE}" "${RESULT_DIR}/fdp_randwrite_zipf.json" "--random_distribution=zipf:1.3"

# Random Write - HotCold 90/10 (4k)
run_fdp_single_test "randwrite_hotcold" "randwrite" "4k" "${RUNTIME_WRITE}" "${RESULT_DIR}/fdp_randwrite_hotcold.json" "--random_distribution=zoned:90/10:10/90"

# Random Read (4k)
run_fdp_single_test "randread_4k" "randread" "4k" "${RUNTIME_READ}" "${RESULT_DIR}/fdp_randread_4k.json"

log "FDP tests complete"

# ==========================================
# Summary
# ==========================================
log ""
log "=========================================="
log "  4-Corner Performance Test Complete!"
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
printf "%-12s %-20s %-30s\n" "Config" "Test" "Performance"
printf "%-12s %-20s %-30s\n" "------" "----" "-----------"

for config in "ftl" "fdp"; do
    # Random Write - Uniform
    json_file="${RESULT_DIR}/${config}_randwrite_uniform.json"
    perf=$(extract_perf "$json_file" "write")
    printf "%-12s %-20s %-30s\n" "$config" "randwrite_uniform" "$perf"

    # Random Write - Zipf
    json_file="${RESULT_DIR}/${config}_randwrite_zipf.json"
    perf=$(extract_perf "$json_file" "write")
    printf "%-12s %-20s %-30s\n" "$config" "randwrite_zipf" "$perf"

    # Random Write - HotCold
    json_file="${RESULT_DIR}/${config}_randwrite_hotcold.json"
    perf=$(extract_perf "$json_file" "write")
    printf "%-12s %-20s %-30s\n" "$config" "randwrite_hotcold" "$perf"

    # Random Read
    json_file="${RESULT_DIR}/${config}_randread_4k.json"
    perf=$(extract_perf "$json_file" "read")
    printf "%-12s %-20s %-30s\n" "$config" "randread_4k" "$perf"
    echo ""
done

log "Done!"
