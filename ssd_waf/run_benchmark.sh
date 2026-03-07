#!/bin/bash

# Benchmark automation script
# Usage: ./run_benchmark.sh [config_numbers...]
# Example: ./run_benchmark.sh 1 3 5  (run only config 1, 3, 5)
# Example: ./run_benchmark.sh        (run all configs)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NVMEV_DIR="/home/sejun000/csd-virt/CSD-Virt"
TRACE_FILE="${TRACE_FILE:-../../ssdtrace_scaled_4x.trace}"
LOG_FILE="$SCRIPT_DIR/test.log"

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
    local replay_file="${base_replay%.replay}_${timestamp}.replay"
    log_info "Step 4: Running replay_trace -> $replay_file"
    cd "$SCRIPT_DIR"

    # Run replay_trace in background with nohup
    nohup bash -c "sudo taskset -c 14-18 ./replay_trace --remap-lba --trace $TRACE_FILE --max-tb $MAX_TB --io-scale $IO_SCALE $REPLAY_EXTRA_ARGS" > "$replay_file" 2>&1 &
    local pid=$!

    log_info "replay_trace started with PID: $pid"
    log_info "Waiting for completion (or kill the process to skip)..."

    # Wait for process to complete
    # If killed externally, this will return and we continue
    wait $pid 2>/dev/null || log_warn "replay_trace process terminated (PID: $pid)"

    log_success "replay_trace completed -> $replay_file"
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

    # Step 4: Run replay_trace
    run_replay_trace "$replay"

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

    # Run selected configs
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

    echo ""
    echo "========================================" | tee -a "$LOG_FILE"
    log_success "All selected benchmarks completed!"
    echo "Benchmark ended at: $(date '+%Y-%m-%d %H:%M:%S')" >> "$LOG_FILE"
    echo "========================================" | tee -a "$LOG_FILE"
}

main "$@"
