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
        "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260226_081216.csv"),
        "stat_log": os.path.join(BASE_DIR, "stat.log.20260224_232851"),
        "replay_trace": os.path.join(BASE_DIR, "sepbit_20260224_094450.replay"),
        "csv_type": "icache",  # icache or ftl or ocf
        "cache_size_gb": 1880,
    },
    "REFlash_COLD_FIXED": {
        "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_COLD_20260209_214756.csv"),
        "stat_log": os.path.join(BASE_DIR, "stat.log.20260209_214755"),
        "replay_trace": os.path.join(BASE_DIR, "reflash_cold_fixed.replay"),
        "csv_type": "icache",
        "cache_size_gb": 1880,
    },
    "REFlash_WARM_FIXED": {
        "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_WARM_20260225_231144.csv"),
        "stat_log": os.path.join(BASE_DIR, "stat.log.20260225_231142"),
        "replay_trace": os.path.join(BASE_DIR, "reflash_fixed_20260225_232254.replay"),
        "csv_type": "icache",
        "cache_size_gb": 1880,
    },
    "REFlash": {
        "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260226_033138.csv"),
        "stat_log": os.path.join(BASE_DIR, "stat.log.20260226_033136"),
        "replay_trace": os.path.join(BASE_DIR, "reflash_20260224_180916.replay"),
        "csv_type": "icache",
        "cache_size_gb": 1880,
    },
    "CSAL": {
        "csv": os.path.join(LOGGING_DIR, "ftl0_20260225_082514.csv"),
        "stat_log": None,  # No histogram for CSAL
        "replay_trace": os.path.join(BASE_DIR, "ftl_20260223_200336.replay"),
        "csv_type": "ftl",
        "cache_size_gb": 1880,
    },
    "OpenCAS": {
        "csv": os.path.join(LOGGING_DIR, "ocf0_20260225_122214.csv"),
        "stat_log": None,  # No histogram for OpenCAS
        "replay_trace": os.path.join(BASE_DIR, "ocf_20260223_141636.replay"),
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

# Cost formula: TLC writes + QLC_COST_MULTIPLIER * QLC writes
QLC_COST_MULTIPLIER = 8.64

# Normalization base for Graph D
NORMALIZATION_BASE = "CSAL"

# Configs that have histogram data (for Graphs E, F)
HISTOGRAM_CONFIGS = ["SepBIT", "REFlash_COLD_FIXED", "REFlash_WARM_FIXED", "REFlash"]

# All configs for Graphs A, B, C, D, etc. (histogram 제외)
ALL_CONFIGS = ["SepBIT", "REFlash_WARM_FIXED", "REFlash", "CSAL", "OpenCAS"]

# Configs for Graph I (utilization scatter) - ordered for comparison
GRAPH_I_CONFIGS = ["OpenCAS", "REFlash", "CSAL"]
# Configs for Graph B2 (WAF bar) - ordered for comparison
GRAPH_B2_CONFIGS = ["OpenCAS", "REFlash", "CSAL"]
# Configs for Graph C (QLC writes timeseries) - tiering configs only
GRAPH_C_CONFIGS = ["REFlash", "CSAL", "OpenCAS"]
# Configs for Graph C2 (QLC writes bar) - ordered for comparison
GRAPH_C2_CONFIGS = ["OpenCAS", "REFlash", "CSAL"]
# Configs for Graph G (throughput)
GRAPH_G_CONFIGS = ["SepBIT", "REFlash", "CSAL", "OpenCAS"]
# Configs for Graph H (valid block rate) - must have valid_blocks column and cache_size_gb
GRAPH_H_CONFIGS = ["SepBIT",  "REFlash_WARM_FIXED", "REFlash", "CSAL", "OpenCAS"]

# Output file names
OUTPUT_FILES = {
    "graph_a": os.path.join(OUTPUT_DIR, "graph_A_scatter.png"),
    "graph_b": os.path.join(OUTPUT_DIR, "graph_B_timeseries_tlc.png"),
    "graph_c": os.path.join(OUTPUT_DIR, "graph_C_timeseries_qlc.png"),
    "graph_d": os.path.join(OUTPUT_DIR, "graph_D_normalized_cost.png"),
    "graph_e": os.path.join(OUTPUT_DIR, "graph_E_evicted_histogram.png"),
    "graph_f": os.path.join(OUTPUT_DIR, "graph_F_compacted_histogram.png"),
    "graph_g": os.path.join(OUTPUT_DIR, "graph_G_throughput.png"),
    "graph_b2": os.path.join(OUTPUT_DIR, "graph_B2_waf_bar.png"),
    "graph_c2": os.path.join(OUTPUT_DIR, "graph_C2_qlc_bar.png"),
    "graph_d2": os.path.join(OUTPUT_DIR, "graph_D2_actual_cost.png"),
    "graph_f2": os.path.join(OUTPUT_DIR, "graph_F2_gc_copied_lifetime.png"),
    "graph_h": os.path.join(OUTPUT_DIR, "graph_H_valid_block_rate.png"),
    "graph_i": os.path.join(OUTPUT_DIR, "graph_I_utilization.png"),
}
