#!/bin/bash

# Benchmark automation script
# Usage: ./run_benchmark.sh [config_numbers...]
# Example: ./run_benchmark.sh 1 3 5  (run only config 1, 3, 5)
# Example: ./run_benchmark.sh        (run all configs)
#
# Trace file selection via TRACE_NUMS (space-separated):
#   TRACE_NUMS="1 3 5" ./run_benchmark.sh 1 2
#   Available traces:
#     1) alibaba_dwpd0.3.trace
#     2) alibaba_dwpd01to1.trace
#     3) alibaba_dwpd1to2_4x.trace   (default)
#     4) alibaba_dwpd2_5x.trace
#     5) ssdtrace_scaled_4x.trace
#     7) fio_zipf_1.0 (50% read / 50% write)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NVMEV_DIR="/home/sejun000/csd-virt/CSD-Virt"
LOG_FILE="$SCRIPT_DIR/test.log"

# Trace file definitions (number -> path)
TRACE_BASE="../../"
declare -a TRACE_FILES=(
    [1]="alibaba_dwpd0.3.trace"
    [2]="alibaba_dwpd01to1.trace"
    [3]="alibaba_dwpd1to2_4x.trace"
    [4]="alibaba_dwpd2_5x.trace"
    [5]="ssdtrace_scaled_4x.trace"
    [7]="fio_zipf_1.0"
)
# Default trace file (used when TRACE_NUMS is not set)
TRACE_FILE="${TRACE_FILE:-${TRACE_BASE}alibaba_dwpd1to2_4x.trace}"

# fio zipf parameters
FIO_ZIPF_THETA="0.9"
FIO_ZIPF_SIZE="3300g"      # working set (3300 GiB ≈ 3.3 TiB)
FIO_ZIPF_RANDSEED="12345"
# Current trace number (set in multi-trace loop)
CURRENT_TRACE_NUM=""

# Configurable via environment variables
CACHE_SPLIT_GB="${CACHE_SPLIT_GB:-1800}"
BACKEND_SPLIT_GB="${BACKEND_SPLIT_GB:-14400}"
MAX_TB="${MAX_TB:-14}"
IO_SCALE="${IO_SCALE:-2}"
REPLAY_EXTRA_ARGS="${REPLAY_EXTRA_ARGS:-}"
PREFILL="${PREFILL:-1}"

# Initialize log file with timestamp
echo "========================================" > "$LOG_FILE"
echo "Benchmark started at: $(date '+%Y-%m-%d %H:%M:%S')" >> "$LOG_FILE"
echo "========================================" >> "$LOG_FILE"

# Logging function that writes to both console and file
log_to_file() {
    echo "$1" >> "$LOG_FILE"
}

# Config definitions: "name|command|replay_file"
declare -a CONFIGS=(
    "LOG_SEPBIT_FIFO|sudo ICACHE_CACHE_TYPE=LOG_SEPBIT_FIFO ./run_tier_fdp.sh|sepbit.replay"
    "LOG_GREEDY_COST_BENEFIT_10|sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_10 ./run_tier_fdp.sh|reflash.replay"
    "LOG_GREEDY_COST_BENEFIT_10_WARM|sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_10_WARM ./run_tier_fdp.sh|reflash_fixed.replay"
    "LOG_GREEDY_COST_BENEFIT_COLD|sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_COLD ./run_tier_fdp.sh|reflash_fixed.replay"
    "FTL|sudo ./run_ftl.sh|ftl.replay"
    "OCF|sudo ./run_ocf.sh|ocf.replay"
    "LOG_GREEDY_80_WARM|sudo ICACHE_CACHE_TYPE=LOG_GREEDY_80_WARM ./run_tier_fdp.sh|reflash_80_warm.replay"
    "LOG_GREEDY_60_WARM|sudo ICACHE_CACHE_TYPE=LOG_GREEDY_60_WARM ./run_tier_fdp.sh|reflash_60_warm.replay"
    "LOG_GREEDY_40_WARM|sudo ICACHE_CACHE_TYPE=LOG_GREEDY_40_WARM ./run_tier_fdp.sh|reflash_40_warm.replay"
    "REFLASH|sudo ICACHE_CACHE_TYPE=REFLASH ./run_tier_fdp.sh|reflash.replay"
    "REFLASH_80|sudo ICACHE_CACHE_TYPE=REFLASH_80 ./run_tier_fdp.sh|reflash.replay"
    "REFLASH_R864|sudo ICACHE_CACHE_TYPE=REFLASH_R864 ./run_tier_fdp.sh|reflash.replay"
    "REFLASH_R288|sudo ICACHE_CACHE_TYPE=REFLASH_R288 ./run_tier_fdp.sh|reflash.replay"
)
# Split configs (use with CACHE_SPLIT_ENABLE=1):
#    "LOG_SEPBIT_FIFO|sudo ICACHE_CACHE_TYPE=LOG_SEPBIT_FIFO CACHE_SPLIT_GB=$CACHE_SPLIT_GB CACHE_SPLIT_ENABLE=1 BACKEND_SPLIT_GB=$BACKEND_SPLIT_GB ./run_tier_fdp.sh|sepbit.replay"
#    "LOG_GREEDY_COST_BENEFIT_10|sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_10 CACHE_SPLIT_GB=$CACHE_SPLIT_GB CACHE_SPLIT_ENABLE=1 BACKEND_SPLIT_GB=$BACKEND_SPLIT_GB ./run_tier_fdp.sh|reflash.replay"
#    "LOG_GREEDY_COST_BENEFIT_10_WARM|sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_10_WARM CACHE_SPLIT_GB=$CACHE_SPLIT_GB CACHE_SPLIT_ENABLE=1 BACKEND_SPLIT_GB=$BACKEND_SPLIT_GB ./run_tier_fdp.sh|reflash_fixed.replay"
#    "LOG_GREEDY_COST_BENEFIT_COLD|sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_COLD CACHE_SPLIT_GB=$CACHE_SPLIT_GB CACHE_SPLIT_ENABLE=1 BACKEND_SPLIT_GB=$BACKEND_SPLIT_GB ./run_tier_fdp.sh|reflash_cold_fixed.replay"
#    "FTL|sudo CACHE_SPLIT_GB=$CACHE_SPLIT_GB CACHE_SPLIT_ENABLE=1 BACKEND_SPLIT_GB=$BACKEND_SPLIT_GB ./run_ftl.sh|ftl.replay"
#    "OCF|sudo CACHE_SPLIT_GB=$CACHE_SPLIT_GB CACHE_SPLIT_ENABLE=1 BACKEND_SPLIT_GB=$BACKEND_SPLIT_GB ./run_ocf.sh|ocf.replay"
#    "LOG_GREEDY_COST_BENEFIT_HOT|sudo PREFILL=1 ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_HOT CACHE_SPLIT_GB=720 CACHE_SPLIT_ENABLE=1 BACKEND_SPLIT_GB=2880 ./run_tier_fdp.sh|reflash_hot_fixed.replay"
#  "LOG_GREEDY_COST_BENEFIT_COLD|sudo ICACHE_CACHE_TYPE=LOG_GREEDY_COST_BENEFIT_COLD ./run_tier_fdp.sh|reflash_cold_fixed.replay"    

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo -e "${BLUE}[INFO]${NC} $1"
    echo "[$timestamp] [INFO] $1" >> "$LOG_FILE"
}

log_success() {
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo -e "${GREEN}[SUCCESS]${NC} $1"
    echo "[$timestamp] [SUCCESS] $1" >> "$LOG_FILE"
}

log_warn() {
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo -e "${YELLOW}[WARN]${NC} $1"
    echo "[$timestamp] [WARN] $1" >> "$LOG_FILE"
}

log_error() {
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo -e "${RED}[ERROR]${NC} $1"
    echo "[$timestamp] [ERROR] $1" >> "$LOG_FILE"
}

print_configs() {
    echo "" | tee -a "$LOG_FILE"
    echo "Available configurations:" | tee -a "$LOG_FILE"
    echo "=========================" | tee -a "$LOG_FILE"
    for i in "${!CONFIGS[@]}"; do
        IFS='|' read -r name cmd replay <<< "${CONFIGS[$i]}"
        echo "  $((i+1)). $name -> $replay" | tee -a "$LOG_FILE"
    done
    echo "" | tee -a "$LOG_FILE"
    echo "Available trace files:" | tee -a "$LOG_FILE"
    echo "=========================" | tee -a "$LOG_FILE"
    for i in "${!TRACE_FILES[@]}"; do
        local marker=""
        [ "${TRACE_FILES[$i]}" = "alibaba_dwpd1to2_4x.trace" ] && marker=" (default)"
        echo "  $i) ${TRACE_FILES[$i]}${marker}" | tee -a "$LOG_FILE"
    done
    echo "" | tee -a "$LOG_FILE"
}

# Step 1: nvmev rmmod && init
nvmev_reinit() {
    log_info "Step 1: Reinitializing nvmev..."
    cd "$NVMEV_DIR"
    sudo rmmod nvmev || log_warn "nvmev module not loaded, skipping rmmod"
    sudo ./init_nvmev.sh
    cd "$SCRIPT_DIR"
    log_success "nvmev reinitialized"
}

# Step 2: Exit all tgt processes
exit_all_tgt() {
    log_info "Step 2: Exiting all tgt processes..."
    cd "$SCRIPT_DIR"
    ./exit_tgt.sh || true
    ./exit_ocf.sh || true
    ./exit_ftl.sh || true
    log_success "All tgt processes exited"
}

# Step 3: Run tgt with specific config
run_tgt() {
    local cmd="$1"
    # Inject PREFILL if enabled
    if [ "$PREFILL" = "1" ]; then
        cmd="${cmd/sudo /sudo PREFILL=1 }"
    fi
    log_info "Step 3: Running tgt..."
    log_info "Command: $cmd"
    cd "$SCRIPT_DIR"
    # Auto-answer 'y' for format confirmation prompts
    yes | eval "$cmd"
    log_success "tgt started"
}

# Step 4: Run replay_trace and wait for completion
run_replay_trace() {
    local base_replay="$1"
    local timestamp=$(date '+%Y%m%d_%H%M%S')
    # Include trace name in replay filename for identification
    local trace_name=$(basename "$TRACE_FILE" | sed 's/\.\(trace\|csv\)$//')
    local replay_file="${base_replay%.replay}_${trace_name}_${timestamp}.replay"
    log_info "Step 4: Running replay_trace -> $replay_file (trace: $TRACE_FILE)"
    cd "$SCRIPT_DIR"

    # Build io-scale argument (empty when IO_SCALE is unset/empty)
    local io_scale_arg=""
    [ -n "$IO_SCALE" ] && io_scale_arg="--io-scale $IO_SCALE"

    # Run replay_trace in background with nohup
    nohup bash -c "sudo taskset -c 14-18 ./replay_trace --remap-lba --trace $TRACE_FILE --max-tb $MAX_TB $io_scale_arg $REPLAY_EXTRA_ARGS" > "$replay_file" 2>&1 &
    local pid=$!

    log_info "replay_trace started with PID: $pid"
    log_info "Waiting for completion (or kill the process to skip)..."

    # Wait for process to complete
    # If killed externally, this will return and we continue
    wait $pid 2>/dev/null || log_warn "replay_trace process terminated (PID: $pid)"

    log_success "replay_trace completed -> $replay_file"
}

# Device detection (mirrors replay_trace.cpp find_device())
detect_device() {
    # 1. Check ublk device
    if [ -b /dev/ublkb0 ]; then
        local size=$(sudo blockdev --getsize64 /dev/ublkb0 2>/dev/null || echo 0)
        if [ "$size" -gt 0 ] 2>/dev/null; then
            echo "/dev/ublkb0"
            return 0
        fi
    fi

    # 2. Check NVMe-oF device (ICACHE/FTLBDEV/NULL/OCF)
    local nvme_dev=$(nvme list 2>/dev/null | grep -iE 'ICACHE|FTLBDEV|NULL|OCF' | awk '{print $1}')
    if [ -n "$nvme_dev" ]; then
        echo "$nvme_dev"
        return 0
    fi

    # 3. Check OpenCAS device
    if [ -b /dev/cas1-1 ]; then
        local size=$(sudo blockdev --getsize64 /dev/cas1-1 2>/dev/null || echo 0)
        if [ "$size" -gt 0 ] 2>/dev/null; then
            echo "/dev/cas1-1"
            return 0
        fi
    fi

    return 1
}

# Step 4 (alt): Run fio with zipf distribution instead of replay_trace
run_fio_zipf() {
    local base_replay="$1"
    local timestamp=$(date '+%Y%m%d_%H%M%S')
    local replay_file="${base_replay%.replay}_fio_zipf${FIO_ZIPF_THETA}_${timestamp}.replay"

    local device
    device=$(detect_device)
    if [ -z "$device" ]; then
        log_error "No device detected for fio"
        return 1
    fi

    log_info "Step 4: Running fio zipf:${FIO_ZIPF_THETA} on $device -> $replay_file"
    cd "$SCRIPT_DIR"

    local fio_numjobs=4
    # randrw is 50:50, so issue 2x total I/O to reach MAX_TB expected writes.
    # With billions of 4K I/Os, the random mix converges extremely closely to 50:50.
    local total_io_gib=$(( MAX_TB * 2 * 1024 ))
    local per_job_io_gib=$(( total_io_gib / fio_numjobs ))

    log_info "  Working set: ${FIO_ZIPF_SIZE}, mix: 50% read / 50% write"
    log_info "  io_size/job: ${per_job_io_gib}g (total I/O $((MAX_TB * 2))TB, expected writes ${MAX_TB}TB), seeds: ${FIO_ZIPF_RANDSEED}..$(( FIO_ZIPF_RANDSEED + fio_numjobs - 1 ))"

    # Generate temporary jobfile with per-job seeds
    local jobfile=$(mktemp /tmp/fio_zipf_XXXXXX.fio)
    cat > "$jobfile" <<FIOEOF
[global]
ioengine=libaio
direct=1
rw=randrw
rwmixread=50
bs=4k
iodepth=64
random_distribution=zipf:${FIO_ZIPF_THETA}
norandommap
size=${FIO_ZIPF_SIZE}
io_size=${per_job_io_gib}g
filename=${device}
group_reporting
cpus_allowed=14-18
clat_percentiles=1
percentile_list=90:99

FIOEOF
    for i in $(seq 0 $((fio_numjobs - 1))); do
        cat >> "$jobfile" <<FIOEOF
[job${i}]
randseed=$(( FIO_ZIPF_RANDSEED + i ))

FIOEOF
    done

    nohup sudo fio "$jobfile" > "$replay_file" 2>&1 &
    local pid=$!

    log_info "fio started with PID: $pid"
    log_info "Waiting for completion (or kill the process to skip)..."

    wait $pid 2>/dev/null || log_warn "fio process terminated (PID: $pid)"

    rm -f "$jobfile"
    log_success "fio completed -> $replay_file"
}

# Step 5: Exit all tgt after replay
cleanup_after_replay() {
    log_info "Step 5: Cleaning up after replay..."
    exit_all_tgt
}

# Run single config
run_single_config() {
    local config_idx="$1"
    local config="${CONFIGS[$config_idx]}"

    IFS='|' read -r name cmd replay <<< "$config"

    echo ""
    echo "========================================" | tee -a "$LOG_FILE"
    log_info "Running config $((config_idx+1)): $name"
    echo "========================================" | tee -a "$LOG_FILE"

    # Step 1: nvmev reinit
    #nvmev_reinit

    # Step 2: Exit all tgt
    exit_all_tgt

    # Step 3: Run tgt
    run_tgt "$cmd"

    # Wait 11 minutes after bringup before replay
    log_info "Sleeping 11 minutes after bringup..."
    sleep 660

    # Step 4: Run replay_trace (or fio zipf for trace 7)
    if [ "$CURRENT_TRACE_NUM" = "7" ]; then
        run_fio_zipf "$replay"
    else
        run_replay_trace "$replay"
    fi

    # Wait 11 minutes after replay before cleanup
    log_info "Sleeping 11 minutes after replay_trace..."
    sleep 660

    # Step 5: Cleanup
    cleanup_after_replay

    log_success "Config $((config_idx+1)): $name completed!"
}

# Main
main() {
    sudo modprobe nvme_tcp

    # Clean up stale hugepage and SHM files from previous runs
    sudo rm -f /dev/hugepages/ftl_* /dev/hugepages/spdk_*
    sudo rm -f /dev/shm/spdk_tgt_trace.*
    log_info "Cleaned up stale hugepage and SHM files"

    echo "========================================" | tee -a "$LOG_FILE"
    echo "    Benchmark Automation Script" | tee -a "$LOG_FILE"
    echo "========================================" | tee -a "$LOG_FILE"

    print_configs

    # Determine which configs to run
    declare -a selected_configs=()

    if [ $# -eq 0 ]; then
        # No arguments: run all configs
        log_info "No config specified, running ALL configs"
        for i in "${!CONFIGS[@]}"; do
            selected_configs+=("$i")
        done
    else
        # Arguments provided: run specified configs
        for arg in "$@"; do
            if [[ "$arg" =~ ^[0-9]+$ ]] && [ "$arg" -ge 1 ] && [ "$arg" -le ${#CONFIGS[@]} ]; then
                selected_configs+=("$((arg-1))")
            else
                log_error "Invalid config number: $arg (must be 1-${#CONFIGS[@]})"
                exit 1
            fi
        done
    fi

    log_info "Selected configs: ${selected_configs[*]}"
    echo ""

    # Determine which trace files to run
    declare -a selected_traces=()

    if [ -n "${TRACE_NUMS:-}" ]; then
        for tnum in $TRACE_NUMS; do
            if [[ "$tnum" =~ ^[0-9]+$ ]] && [ -n "${TRACE_FILES[$tnum]+x}" ]; then
                selected_traces+=("$tnum")
            else
                log_error "Invalid trace number: $tnum (must be one of: ${!TRACE_FILES[*]})"
                exit 1
            fi
        done
        log_info "Selected traces: ${selected_traces[*]}"
    fi

    # Run selected configs (with optional multi-trace loop)
    if [ ${#selected_traces[@]} -gt 0 ]; then
        # Multi-trace mode
        local trace_total=${#selected_traces[@]}
        local trace_current=0

        local io_scale_orig="$IO_SCALE"
        for tnum in "${selected_traces[@]}"; do
            trace_current=$((trace_current+1))
            CURRENT_TRACE_NUM="$tnum"
            TRACE_FILE="${TRACE_BASE}${TRACE_FILES[$tnum]}"

            # Trace 7 (fio zipf) doesn't need --io-scale
            if [ "$tnum" -eq 7 ]; then
                IO_SCALE=""
            else
                IO_SCALE="$io_scale_orig"
            fi

            echo ""
            echo "========================================" | tee -a "$LOG_FILE"
            log_info "Trace $trace_current / $trace_total: ${TRACE_FILES[$tnum]}"
            echo "========================================" | tee -a "$LOG_FILE"

            local total=${#selected_configs[@]}
            local current=0

            for idx in "${selected_configs[@]}"; do
                current=$((current+1))
                echo ""
                echo "########################################" | tee -a "$LOG_FILE"
                echo "  Trace [$trace_current/$trace_total] Config [$current/$total]" | tee -a "$LOG_FILE"
                echo "########################################" | tee -a "$LOG_FILE"

                run_single_config "$idx"
            done
        done
    else
        # Single trace mode (default)
        local total=${#selected_configs[@]}
        local current=0

        for idx in "${selected_configs[@]}"; do
            current=$((current+1))
            echo ""
            echo "########################################" | tee -a "$LOG_FILE"
            echo "  Progress: $current / $total" | tee -a "$LOG_FILE"
            echo "########################################" | tee -a "$LOG_FILE"

            run_single_config "$idx"
        done
    fi

    echo ""
    echo "========================================" | tee -a "$LOG_FILE"
    log_success "All selected benchmarks completed!"
    echo "Benchmark ended at: $(date '+%Y-%m-%d %H:%M:%S')" >> "$LOG_FILE"
    echo "========================================" | tee -a "$LOG_FILE"
}

main "$@"
