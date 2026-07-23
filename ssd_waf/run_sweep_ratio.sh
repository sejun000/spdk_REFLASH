#!/bin/bash
# Sweep QLC_TLC_COST_RATIO in log_cache.h across multiple values,
# recompile SPDK, and run benchmark (config 2 = LOG_GREEDY_COST_BENEFIT_10)
# with TRACE_NUMS=3 (alibaba_dwpd1to2_4x.trace) for each.
#
# Usage: nohup ./run_sweep_ratio.sh > sweep.log 2>&1 &

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPDK_DIR="$(dirname "$SCRIPT_DIR")"
LOG_CACHE_H="$SPDK_DIR/module/bdev/icache/logcache/port/log_cache.h"

RATIOS=(2 4 6 8 10)

echo "========================================="
echo "QLC_TLC_COST_RATIO sweep: ${RATIOS[*]}"
echo "Started at: $(date '+%Y-%m-%d %H:%M:%S')"
echo "========================================="

for ratio in "${RATIOS[@]}"; do
    echo ""
    echo "########################################"
    echo "  Ratio = $ratio  ($(date '+%Y-%m-%d %H:%M:%S'))"
    echo "########################################"

    # 1. Modify log_cache.h: replace QLC_TLC_COST_RATIO value
    sed -i "s/static constexpr double QLC_TLC_COST_RATIO = [^;]*/static constexpr double QLC_TLC_COST_RATIO = ${ratio}.0/" "$LOG_CACHE_H"
    echo "[INFO] Set QLC_TLC_COST_RATIO = ${ratio}.0"
    grep "QLC_TLC_COST_RATIO" "$LOG_CACHE_H"

    # 2. Recompile SPDK
    echo "[INFO] Building SPDK..."
    cd "$SPDK_DIR"
    make -j4
    echo "[INFO] Build complete"

    # 3. Run benchmark: config 2 (LOG_GREEDY_COST_BENEFIT_10), trace 3
    echo "[INFO] Running benchmark with ratio=$ratio ..."
    cd "$SCRIPT_DIR"
    export TRACE_NUMS="3"
    sudo -E ./run_benchmark.sh 2

    echo "[INFO] Ratio $ratio done at $(date '+%Y-%m-%d %H:%M:%S')"
done

echo ""
echo "========================================="
echo "Sweep complete at: $(date '+%Y-%m-%d %H:%M:%S')"
echo "========================================="
