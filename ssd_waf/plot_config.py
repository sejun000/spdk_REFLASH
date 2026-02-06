#!/usr/bin/env python3
"""
Configuration file for graph plotting.
All input file paths and settings are centralized here.
"""

import os

BASE_DIR = "/home/sejun000/spdk_REFLASH/ssd_waf"
LOGGING_DIR = os.path.join(BASE_DIR, "logging")
OUTPUT_DIR = BASE_DIR

# Config definitions
CONFIGS = {
    "SepBIT": {
        "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260203_122659.csv"),
        "stat_log": os.path.join(BASE_DIR, "stat.log.20260126_120618"),
        "replay_trace": os.path.join(BASE_DIR, "replay_trace_sepbit.log"),
        "csv_type": "icache",  # icache or ftl or ocf
        "cache_size_gb": 580,
    },
    "REFlash_COLD_FIXED": {
        "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_COLD_20260127_000704.csv"),
        "stat_log": os.path.join(BASE_DIR, "stat.log.20260127_000703"),
        "replay_trace": os.path.join(BASE_DIR, "replay_trace_cold.log"),
        "csv_type": "icache",
        "cache_size_gb": 580,
    },
    "REFlash_WARM_FIXED": {
        "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_WARM_20260204_044917.csv"),
        "stat_log": os.path.join(BASE_DIR, "stat.log.20260126_135916"),
        "replay_trace": os.path.join(BASE_DIR, "replay_trace_warm_fixed.log"),
        "csv_type": "icache",
        "cache_size_gb": 580,
    },
    "REFlash": {
        "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260203_182203.csv"),
        "stat_log": os.path.join(BASE_DIR, "stat.log.20260130_124016"),
        "replay_trace": os.path.join(BASE_DIR, "replay_trace_ghost.log"),
        "csv_type": "icache",
        "cache_size_gb": 580,
    },
    "REFlash_Beta_Control": {
        "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260205_035041.csv"),
        "stat_log": os.path.join(BASE_DIR, "stat.log.20260130_124016"),
        "replay_trace": None,
        "csv_type": "icache",
        "cache_size_gb": 580,
    },
    "CSAL": {
        "csv": os.path.join(LOGGING_DIR, "ftl0_20260126_110949.csv"),
        "stat_log": None,  # No histogram for CSAL
        "replay_trace": os.path.join(BASE_DIR, "replay_trace_ftl.log"),
        "csv_type": "ftl",
        "cache_size_gb": 580,
    },
    "OpenCAS": {
        "csv": os.path.join(LOGGING_DIR, "ocf0_20260126_082458.csv"),
        "stat_log": None,  # No histogram for OpenCAS
        "replay_trace": os.path.join(BASE_DIR, "replay_trace_ocf.log"),
        "csv_type": "ocf",
    },
}

# Column mappings for different CSV types
# Columns: TLC writes (nvme_media_written), QLC writes (backend/evicted), Host writes
CSV_COLUMNS = {
    "icache": {
        "host_write": "host_write_MB",
        "tlc_write": "nvme_media_written_MB",
        "qlc_write": "backend_write_MB",
        "fdp_host_write": "nvme_host_written_MB",
        "fdp_media_write": "nvme_media_written_MB",
    },
    "ftl": {
        "host_write": "host_write_MB",
        "tlc_write": "nvme_media_written_MB",
        "qlc_write": "backend_write_MB",
        "fdp_host_write": "nvme_host_written_MB",
        "fdp_media_write": "nvme_media_written_MB",
    },
    "ocf": {
        "host_write": "host_wr_MB",
        "tlc_write": "nvme_media_MB",
        "qlc_write": "core_wr_MB",
        "fdp_host_write": "nvme_host_MB",
        "fdp_media_write": "nvme_media_MB",
    },
}

# Cost formula: TLC writes + 2.8 * QLC writes
QLC_COST_MULTIPLIER = 2.8

# Normalization base for Graph D
NORMALIZATION_BASE = "CSAL"

# Configs that have histogram data (for Graphs E, F)
HISTOGRAM_CONFIGS = ["SepBIT", "REFlash_COLD_FIXED", "REFlash_WARM_FIXED", "REFlash_Beta_Control", "REFlash"]

# All configs for Graphs A, B, C, D, G
ALL_CONFIGS = ["SepBIT", "REFlash_COLD_FIXED", "REFlash_WARM_FIXED", "REFlash_Beta_Control", "REFlash", "CSAL", "OpenCAS"]

# Configs for Graph H (valid block rate) - must have valid_blocks column and cache_size_gb
GRAPH_H_CONFIGS = ["REFlash_WARM_FIXED", "REFlash", "REFlash_Beta_Control", "CSAL"]

# Output file names
OUTPUT_FILES = {
    "graph_a": os.path.join(OUTPUT_DIR, "graph_A_scatter.png"),
    "graph_b": os.path.join(OUTPUT_DIR, "graph_B_timeseries_tlc.png"),
    "graph_c": os.path.join(OUTPUT_DIR, "graph_C_timeseries_qlc.png"),
    "graph_d": os.path.join(OUTPUT_DIR, "graph_D_normalized_cost.png"),
    "graph_e": os.path.join(OUTPUT_DIR, "graph_E_evicted_histogram.png"),
    "graph_f": os.path.join(OUTPUT_DIR, "graph_F_compacted_histogram.png"),
    "graph_g": os.path.join(OUTPUT_DIR, "graph_G_throughput.png"),
    "graph_d2": os.path.join(OUTPUT_DIR, "graph_D2_actual_cost.png"),
    "graph_h": os.path.join(OUTPUT_DIR, "graph_H_valid_block_rate.png"),
}
