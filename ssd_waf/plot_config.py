#!/usr/bin/env python3
"""
Configuration file for graph plotting.
All input file paths and settings are centralized here.
Supports multiple workloads per feature for grouped bar graphs.
"""

import os

BASE_DIR = "/home/sejun000/spdk_REFLASH/ssd_waf"
LOGGING_DIR = os.path.join(BASE_DIR, "logging")
OUTPUT_DIR = BASE_DIR

# Workload names for multi-workload graphs (A, B2, C2, D, D2, G)
WORKLOADS = ["FIO-0.9", "YCSB-A", "Ali1", "Ali2", "Ali3", "Varmail"]

# Default workload for single-workload graphs (B, C, H, I, E, F, F2)
DEFAULT_WORKLOAD = "Ali1"

# Config definitions - nested: feature -> workload -> config_dict
# Each feature can have data for one or more workloads.
# Graphs will gracefully skip missing workloads.
CONFIGS = {
    "SepBIT": {
        "Ali1": {
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260228_061648.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260228_061646"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_20260228_062758.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Ali2": {
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260226_081216.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260226_081214"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_20260226_082326.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Ali3": {
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260303_002046.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260303_002043"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_20260303_003155.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "YCSB-A": {
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260302_032038.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260302_032036"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_20260302_033148.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Varmail": {
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260309_124312.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260309_124309"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_varmail_2tb_6x_16t_20260309_125421.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "FIO-0.9": {
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260311_151222.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260311_151220"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_fio_zipf0.9_20260311_152332.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
    },
    "REFlash_COLD_FIXED": {
        "Ali1": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_COLD_20260209_214756.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260209_214755"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_cold_fixed.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Ali2": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_COLD_20260209_214756.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260209_214755"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_cold_fixed.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },  # REFlash_COLD_FIXED: Ali1/Ali2 same data (no separate runs)
        "YCSB-A": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_COLD_20260209_214756.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260209_214755"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_cold_fixed.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
    },
    "REFlash_80": {
        "Ali1": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_WARM_20260228_133035.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260228_133033"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_fixed_20260228_134145.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Ali2": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_WARM_20260226_153249.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260226_153246"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_fixed_20260226_154358.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Ali3": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_WARM_20260303_044509.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260303_044507"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_fixed_20260303_045619.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "YCSB-A": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_WARM_20260301_222901.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260301_222858"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_fixed_20260301_224010.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Varmail": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_WARM_20260309_221410.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260309_221408"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_fixed_varmail_2tb_6x_16t_20260309_222520.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "FIO-0.9": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_WARM_20260312_141111.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260312_141109"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_fixed_fio_zipf0.9_20260312_142221.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
    },
    # REFlash: list format - each workload has a list of run configs (multiple csv/stat_log/replay_trace)
    "REFlash": {
        "Ali1": [
            {
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260227_141502.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260227_141500"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_20260227_142612.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            {
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260303_230256.ali2.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260226_033136"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_20260226_034248.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
        "Ali2": [
            {
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260226_033138.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260226_033136"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_20260226_034248.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            {
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260303_190521.ali1.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260226_033136"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_20260226_034248.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
        "Ali3": [
            {
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260302_095057.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260302_095055"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_20260302_100206.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            {
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260303_112411.alilow.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260302_095055"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_20260302_100206.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
        "YCSB-A": [
            {
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260228_210650.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260228_210648"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_20260228_211800.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            {
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260304_081151.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260228_210648"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_20260228_211800.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
        "Varmail": [
            {  # r=8.64
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260312_220439.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260309_083114"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_varmail_2tb_6x_16t_20260309_084226.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            {  # r=2.88
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260309_083117.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260312_220437"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_varmail_2tb_6x_16t_20260312_221549.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
        "FIO-0.9": [
            {  # r=8.64
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260313_024813.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260313_024811"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_fio_zipf0.9_20260313_025922.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            {  # r=2.88
                "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_10_20260310_204648.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260310_204645"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_fio_zipf0.9_20260310_205757.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
    },
    "CSAL+GC": {
        "Ali1": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_80_WARM_20260306_170505.csv.dwpd2"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260306_170503"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_80_warm_20260306_171615.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Ali2": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_80_WARM_20260307_155706.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260307_155703"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_80_warm_20260307_075522.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Ali3": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_80_WARM_20260306_222303.csv.dwpd01"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260306_222301"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_80_warm_20260306_223413.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "YCSB-A": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_80_WARM_20260307_074413.csv.ssdtrace"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260307_074410"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_80_warm_20260307_075522.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Varmail": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_80_WARM_20260309_174006.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260309_174004"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_80_warm_varmail_2tb_6x_16t_20260309_175116.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "FIO-0.9": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_80_WARM_20260311_233030.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260311_233027"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_80_warm_fio_zipf0.9_20260311_234139.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
    },
    "CSAL": {
        "Ali1": {
            "csv": os.path.join(LOGGING_DIR, "ftl0_20260227_181546.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ftl_20260227_182656.replay"),
            "csv_type": "ftl",
            "cache_size_gb": 1880,
        },
        "Ali2": {
            "csv": os.path.join(LOGGING_DIR, "ftl0_20260225_082514.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ftl_20260225_083623.replay"),
            "csv_type": "ftl",
            "cache_size_gb": 1880,
        },
        "Ali3": {
            "csv": os.path.join(LOGGING_DIR, "ftl0_20260302_140526.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ftl_20260302_141636.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "YCSB-A": {
            "csv": os.path.join(LOGGING_DIR, "ftl0_20260301_010833.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ftl_20260301_011942.replay"),
            "csv_type": "ftl",
            "cache_size_gb": 1880,
        },
        "Varmail": {
            "csv": os.path.join(LOGGING_DIR, "ftl0_20260308_183828.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ftl_varmail_2tb_6x_16t_20260308_184937.replay"),
            "csv_type": "ftl",
            "cache_size_gb": 1880,
        },
        "FIO-0.9": {
            "csv": os.path.join(LOGGING_DIR, "ftl0_20260313_235735.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ftl_fio_zipf0.9_20260314_000844.replay"),
            "csv_type": "ftl",
            "cache_size_gb": 1880,
        },
    },
    "OpenCAS": {
        "Ali1": {
            "csv": os.path.join(LOGGING_DIR, "ocf0_20260227_225127.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ocf_20260227_230240.replay"),
            "csv_type": "ocf",
        },
        "Ali2": {
            "csv": os.path.join(LOGGING_DIR, "ocf0_20260225_122214.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ocf_20260225_123326.replay"),
            "csv_type": "ocf",
        },
         "Ali3": {
            "csv": os.path.join(LOGGING_DIR, "ocf0_20260302_182507.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ocf_20260302_183620.replay"),
            "csv_type": "ocf",
        },
        "YCSB-A": {
            "csv": os.path.join(LOGGING_DIR, "ocf0_20260301_050151.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ocf_20260301_051304.replay"),
            "csv_type": "ocf",
        },
        "Varmail": {
            "csv": os.path.join(LOGGING_DIR, "ocf0_20260308_225125.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ocf_varmail_2tb_6x_16t_20260308_230238.replay"),
            "csv_type": "ocf",
        },
        "FIO-0.9": {
            "csv": os.path.join(LOGGING_DIR, "ocf0_20260311_051835.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ocf_fio_zipf0.9_20260311_052948.replay"),
            "csv_type": "ocf",
        },
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

# Cost formula: TLC writes + r * QLC writes
# Multiple r values for Graph D subplots: each value creates a subplot titled "(a) r=x"
QLC_COST_MULTIPLIERS = [8.64, 2.88]

# Start measuring TLC/QLC writes from this host write point (GB) for cost graphs (A, D, D2)
# Set to 0 to use the full trace from the beginning
COST_START_HOST_WRITE_GB = 6072 # 4TB

# Normalization base for Graph D
NORMALIZATION_BASE = "NearOpt"

# NearOpt (lower bound) cost values in TB, per (workload, QLC_COST_MULTIPLIER)
NEAROPT_COSTS_TB = {
    ("Ali1", 2.88): 14.35,
    ("Ali1", 8.64): 23.24,
    ("Ali2", 2.88): 18.61,
    ("Ali2", 8.64): 31.36,
    ("Ali3", 2.88): 16.85,
    ("Ali3", 8.64): 32.15,
    ("YCSB-A", 2.88): 18.09,
    ("YCSB-A", 8.64): 32.11,
    ("Varmail", 2.88): 22.60,   # TODO: temporary placeholder
    ("Varmail", 8.64): 47.85,   # TODO: temporary placeholder
    ("FIO-0.9", 2.88): 14.23,
    ("FIO-0.9", 8.64): 22.31,
}

# Configs that have histogram data (for Graphs E, F)
HISTOGRAM_CONFIGS = ["SepBIT", "REFlash_COLD_FIXED", "REFlash_80", "REFlash"]

# All configs for Graphs A, B, C, D, etc. (histogram 제외)
ALL_CONFIGS = ["OpenCAS", "CSAL", "CSAL+GC", "SepBIT", "REFlash_80", "REFlash"]

# Configs for Graph I (utilization scatter) - ordered for comparison
GRAPH_I_CONFIGS = ["OpenCAS", "REFlash", "CSAL"]
# Configs for Graph B2 (WAF bar) - ordered for comparison
GRAPH_B2_CONFIGS = ["OpenCAS", "CSAL+GC", "CSAL"]
# Configs for Graph C (QLC writes timeseries) - tiering configs only
GRAPH_C_CONFIGS = ["OpenCAS", "CSAL", "SepBIT", "REFlash_80", "REFlash"]
# Configs for Graph C2 (QLC writes bar) - ordered for comparison
GRAPH_C2_CONFIGS = ["OpenCAS", "CSAL", "CSAL+GC", "SepBIT", "REFlash_80", "REFlash"]
# Configs for Graph G (throughput)
GRAPH_G_CONFIGS = ["OpenCAS", "CSAL", "CSAL+GC", "SepBIT", "REFlash_80", "REFlash"]
# Configs for Graph H (valid block rate) - must have valid_blocks column and cache_size_gb
GRAPH_H_CONFIGS = ["SepBIT",  "REFlash_80", "REFlash", "CSAL", "OpenCAS"]

# Output file names
OUTPUT_FILES = {
    "graph_a": os.path.join(OUTPUT_DIR, "graph_A_scatter.png"),
    "graph_b": os.path.join(OUTPUT_DIR, "graph_B_timeseries_tlc.png"),
    "graph_c": os.path.join(OUTPUT_DIR, "graph_C_timeseries_qlc.png"),
    "graph_d": os.path.join(OUTPUT_DIR, "A_graph_D_normalized_cost.pdf"),
    "graph_e": os.path.join(OUTPUT_DIR, "graph_E_evicted_histogram.png"),
    "graph_f": os.path.join(OUTPUT_DIR, "graph_F_compacted_histogram.png"),
    "graph_g": os.path.join(OUTPUT_DIR, "A_graph_G_throughput.pdf"),
    "graph_b2": os.path.join(OUTPUT_DIR, "graph_B2_waf_bar.png"),
    "graph_c2": os.path.join(OUTPUT_DIR, "graph_C2_qlc_bar.png"),
    "graph_c3": os.path.join(OUTPUT_DIR, "A_graph_C3_capacity_tier.pdf"),
    "graph_c4": os.path.join(OUTPUT_DIR, "A_graph_C4_buffer_tier.pdf"),
    "graph_d2": os.path.join(OUTPUT_DIR, "graph_D2_actual_cost.png"),
    "graph_f2": os.path.join(OUTPUT_DIR, "graph_F2_gc_copied_lifetime.png"),
    "graph_h": os.path.join(OUTPUT_DIR, "graph_H_valid_block_rate.png"),
    "graph_i": os.path.join(OUTPUT_DIR, "graph_I_utilization.png"),
    "graph_j": os.path.join(OUTPUT_DIR, "A_graph_J_c2_utilization.pdf"),
}
