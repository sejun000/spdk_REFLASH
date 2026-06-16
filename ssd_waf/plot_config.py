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
WORKLOADS = ["FIO", "YCSB-A", "Alibaba1", "Alibaba2", "Alibaba3", "Varmail"]

# Default workload for single-workload graphs (B, C, H, I, E, F, F2)
DEFAULT_WORKLOAD = "Alibaba1"

# Config definitions - nested: feature -> workload -> config_dict
# Each feature can have data for one or more workloads.
# Graphs will gracefully skip missing workloads.
CONFIGS = {
    # SepBIT: re-ran 2026-06-03 ~ 2026-06-04 (6 workloads).
    # Old 2-3월 entries kept as "# old:" comments for traceability.
    "SepBIT": {
        "Alibaba1": {
            # old: LOG_SEPBIT_FIFO_20260228_061648.csv (stat.log.20260228_061646, replay sepbit_20260228_062758)
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260603_163035.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260603_163032"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_alibaba_dwpd2_5x_20260603_164144.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Alibaba2": {
            # old: LOG_SEPBIT_FIFO_20260226_081216.csv (stat.log.20260226_081214, replay sepbit_20260226_082326)
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260603_103453.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260603_103451"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_alibaba_dwpd1to2_4x_20260603_104603.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Alibaba3": {
            # old: LOG_SEPBIT_FIFO_20260303_002046.csv (stat.log.20260303_002043, replay sepbit_20260303_003155)
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260603_211558.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260603_211555"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_alibaba_dwpd01to1_20260603_212707.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "YCSB-A": {
            # old: LOG_SEPBIT_FIFO_20260302_032038.csv (stat.log.20260302_032036, replay sepbit_20260302_033148)
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260604_014136.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260604_014134"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_ssdtrace_scaled_4x_20260604_015246.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Varmail": {
            # old: LOG_SEPBIT_FIFO_20260309_124312.csv (stat.log.20260309_124309, replay sepbit_varmail_2tb_6x_16t_20260309_125421)
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260604_071607.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260604_071605"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_varmail_2tb_6x_16t_20260604_072717.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "FIO": {
            # old: LOG_SEPBIT_FIFO_20260311_151222.csv (stat.log.20260311_151220, replay sepbit_fio_zipf0.9_20260311_152332)
            "csv": os.path.join(LOGGING_DIR, "LOG_SEPBIT_FIFO_20260604_120248.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260604_120246"),
            "replay_trace": os.path.join(BASE_DIR, "sepbit_fio_zipf0.9_20260604_121358.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
    },
    "REFlash_COLD_FIXED": {
        "Alibaba1": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_COST_BENEFIT_COLD_20260209_214756.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260209_214755"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_cold_fixed.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Alibaba2": {
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
        # REFLASH_80 build (target_valid_rate=0.80) re-run on 6/07-6/08 with current binary
        # (invrate + sqrt(age) + waf_w, sepbit-age compactor). All 6 workloads complete (FIO finished 2026-06-09 00:33).
        "Alibaba1": {
            # old (5/24 GS_SUM build): REFLASH_80_20260524_014022.csv (stat.log.20260524_014020, replay reflash_alibaba_dwpd2_5x_20260524_015132)
            "csv": os.path.join(LOGGING_DIR, "REFLASH_80_20260608_054739.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260608_054737"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_alibaba_dwpd2_5x_20260608_055849.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Alibaba2": {
            # old (5/23 GS_SUM build): REFLASH_80_20260523_211121.csv (stat.log.20260523_211119, replay reflash_alibaba_dwpd1to2_4x_20260523_212231)
            "csv": os.path.join(LOGGING_DIR, "REFLASH_80_20260608_011929.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260608_011926"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_alibaba_dwpd1to2_4x_20260608_013038.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Alibaba3": {
            # old (5/24 GS_SUM build): REFLASH_80_20260524_055415.csv (stat.log.20260524_055413, replay reflash_alibaba_dwpd01to1_20260524_060525)
            "csv": os.path.join(LOGGING_DIR, "REFLASH_80_20260607_204502.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260607_204500"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_alibaba_dwpd01to1_20260607_205612.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "YCSB-A": {
            # old (6/02 invrate build): REFLASH_80_20260602_140323.csv (stat.log.20260602_140320, replay reflash_ssdtrace_scaled_4x_20260602_141432)
            "csv": os.path.join(LOGGING_DIR, "REFLASH_80_20260608_094512.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260608_094510"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_ssdtrace_scaled_4x_20260608_095622.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Varmail": {
            # old (6/02 invrate build): REFLASH_80_20260602_191320.csv (stat.log.20260602_191317, replay reflash_varmail_2tb_6x_16t_20260602_192429)
            "csv": os.path.join(LOGGING_DIR, "REFLASH_80_20260608_144121.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260608_144119"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_varmail_2tb_6x_16t_20260608_145231.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "FIO": {
            # old (6/02 invrate build): REFLASH_80_20260602_232415.csv (stat.log.20260602_232413, replay reflash_fio_zipf0.9_20260602_233525)
            # Completed 2026-06-09 00:33 (14336 GiB host writes, 10255 rows, ~5h46m wall-clock).
            # Note: fixed target_valid_rate=0.80 mismatches FIO Zipf 0.9 → TLC_WAF ~2.3 (vs REFlash dynamic ~1.5).
            "csv": os.path.join(LOGGING_DIR, "REFLASH_80_20260608_184754.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260608_184752"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_fio_zipf0.9_20260608_185904.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
    },
    # REFlash: list format - each workload has a list of run configs
    # Index [0] -> r=8.64 (REFLASH_R864), index [1] -> r=2.88 (REFLASH_R288)
    # Current build: invrate + sqrt(age) in score_warm_first (sepbit-age compactor) + waf_w restored.
    #                Run dates: 2026-06-05 ~ 2026-06-07. All 12 entries (6 workloads × 2 ratios) complete.
    # Predecessor: invrate-only (no sqrt-age, no waf_w), 2026-05-27~29, kept as "# old invrate:".
    # Earlier predecessor: PrGh D=1, 2026-05-20~22, kept as "# old D1:".
    "REFlash": {
        "Alibaba1": [
            # old D1:      REFLASH_R864_20260520_224545.csv
            # old invrate: REFLASH_R864_20260528_023034.csv (stat.log.20260528_023031, replay reflash_alibaba_dwpd2_5x_20260528_024143)
            {  # r=8.64 (sepbit-age + waf_w, 6/06)
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R864_20260606_135113.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260606_135110"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_alibaba_dwpd2_5x_20260606_140222.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            # old D1:      REFLASH_R288_20260521_023535.csv
            # old invrate: REFLASH_R288_20260527_225042.csv (stat.log.20260527_225040, replay reflash_alibaba_dwpd2_5x_20260527_230152)
            {  # r=2.88 (sepbit-age + waf_w, 6/06)
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R288_20260606_173705.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260606_173703"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_alibaba_dwpd2_5x_20260606_174815.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
        "Alibaba2": [
            # old D1:      REFLASH_R864_20260520_151345.csv
            # old invrate: REFLASH_R864_20260527_185115.csv (stat.log.20260527_185113, replay reflash_alibaba_dwpd1to2_4x_20260527_190225)
            {  # r=8.64 (sepbit-age + waf_w, 6/06)
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R864_20260606_060808.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260606_060806"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_alibaba_dwpd1to2_4x_20260606_061918.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            # old D1:      REFLASH_R288_20260520_185921.csv
            # old invrate: REFLASH_R288_20260527_150715.csv (stat.log.20260527_150713, replay reflash_alibaba_dwpd1to2_4x_20260527_151825)
            {  # r=2.88 (sepbit-age + waf_w, 6/06)
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R288_20260606_100315.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260606_100313"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_alibaba_dwpd1to2_4x_20260606_101425.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
        "Alibaba3": [
            # old D1:      REFLASH_R864_20260521_062123.csv
            # old invrate: REFLASH_R864_20260528_100435.csv (stat.log.20260528_100433, replay reflash_alibaba_dwpd01to1_20260528_101545)
            {  # r=8.64 (sepbit-age + waf_w, 6/05)
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R864_20260605_222657.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260605_222654"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_alibaba_dwpd01to1_20260605_223806.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            # old D1:      REFLASH_R288_20260521_101427.csv
            # old invrate: REFLASH_R288_20260528_061646.csv (stat.log.20260528_061644, replay reflash_alibaba_dwpd01to1_20260528_062756)
            {  # r=2.88 (sepbit-age + waf_w, 6/06)
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R288_20260606_021859.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260606_021857"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_alibaba_dwpd01to1_20260606_023009.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
        "YCSB-A": [
            # old D1:      REFLASH_R864_20260521_140329.csv
            # old invrate: REFLASH_R864_20260528_173922.csv (stat.log.20260528_173920, replay reflash_ssdtrace_scaled_4x_20260528_175031)
            {  # r=8.64 (sepbit-age + waf_w, 6/06)
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R864_20260606_212157.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260606_212155"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_ssdtrace_scaled_4x_20260606_213307.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            # old D1:      REFLASH_R288_20260521_175155.csv
            # old invrate: REFLASH_R288_20260528_135915.csv (stat.log.20260528_135913, replay reflash_ssdtrace_scaled_4x_20260528_141025)
            {  # r=2.88 (sepbit-age + waf_w, 6/07)
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R288_20260607_012027.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260607_012025"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_ssdtrace_scaled_4x_20260607_013136.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
        "Varmail": [
            # old D1:      REFLASH_R864_20260521_213621.csv
            # old invrate: REFLASH_R864_20260529_013350.csv (stat.log.20260529_013348, replay reflash_varmail_2tb_6x_16t_20260529_014500)
            {  # r=8.64 (sepbit-age + waf_w, 6/07)
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R864_20260607_050633.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260607_050631"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_varmail_2tb_6x_16t_20260607_051743.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            # old D1:      REFLASH_R288_20260522_012354.csv
            # old invrate: REFLASH_R288_20260528_214020.csv (stat.log.20260528_214018, replay reflash_varmail_2tb_6x_16t_20260528_215130)
            {  # r=2.88 (sepbit-age + waf_w, 6/07)
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R288_20260607_085932.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260607_085929"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_varmail_2tb_6x_16t_20260607_091041.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
        "FIO": [
            # old D1: REFLASH_R864_20260522_051751.csv
            # old invrate: REFLASH_R864_20260529_090747.csv (stat.log.20260529_090744, replay reflash_fio_zipf0.9_20260529_091856)
            {  # r=8.64
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R864_20260607_124731.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260607_124729"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_fio_zipf0.9_20260607_125841.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
            # old D1: REFLASH_R288_20260522_090813.csv
            # old invrate: REFLASH_R288_20260529_052627.csv (stat.log.20260529_052625, replay reflash_fio_zipf0.9_20260529_053737)
            {  # r=2.88
                "csv": os.path.join(LOGGING_DIR, "REFLASH_R288_20260607_164430.csv"),
                "stat_log": os.path.join(BASE_DIR, "stat.log.20260607_164428"),
                "replay_trace": os.path.join(BASE_DIR, "reflash_fio_zipf0.9_20260607_165540.replay"),
                "csv_type": "icache",
                "cache_size_gb": 1880,
            },
        ],
    },
    "Greedy": {
        "Alibaba1": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_80_WARM_20260306_170505.csv.dwpd2"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260306_170503"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_80_warm_20260306_171615.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Alibaba2": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_80_WARM_20260307_155706.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260307_155703"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_80_warm_20260307_075522.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
        "Alibaba3": {
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
        "FIO": {
            "csv": os.path.join(LOGGING_DIR, "LOG_GREEDY_80_WARM_20260311_233030.csv"),
            "stat_log": os.path.join(BASE_DIR, "stat.log.20260311_233027"),
            "replay_trace": os.path.join(BASE_DIR, "reflash_80_warm_fio_zipf0.9_20260311_234139.replay"),
            "csv_type": "icache",
            "cache_size_gb": 1880,
        },
    },
    "CSAL": {
        "Alibaba1": {
            "csv": os.path.join(LOGGING_DIR, "ftl0_20260227_181546.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ftl_20260227_182656.replay"),
            "csv_type": "ftl",
            "cache_size_gb": 1880,
        },
        "Alibaba2": {
            "csv": os.path.join(LOGGING_DIR, "ftl0_20260225_082514.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ftl_20260225_083623.replay"),
            "csv_type": "ftl",
            "cache_size_gb": 1880,
        },
        "Alibaba3": {
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
        "FIO": {
            "csv": os.path.join(LOGGING_DIR, "ftl0_20260313_235735.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ftl_fio_zipf0.9_20260314_000844.replay"),
            "csv_type": "ftl",
            "cache_size_gb": 1880,
        },
    },
    "OpenCAS": {
        "Alibaba1": {
            "csv": os.path.join(LOGGING_DIR, "ocf0_20260227_225127.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ocf_20260227_230240.replay"),
            "csv_type": "ocf",
        },
        "Alibaba2": {
            "csv": os.path.join(LOGGING_DIR, "ocf0_20260225_122214.csv"),
            "stat_log": None,
            "replay_trace": os.path.join(BASE_DIR, "ocf_20260225_123326.replay"),
            "csv_type": "ocf",
        },
         "Alibaba3": {
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
        "FIO": {
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

# Start measuring TLC/QLC writes from this host write point (GiB) for cost graphs (A, D, D2)
# Set to 0 to use the full trace from the beginning
COST_START_HOST_WRITE_GB = 6 * 1024  # 6 TiB (skip cache-fill warmup, deep steady-state)

# Normalization base for Graph D
NORMALIZATION_BASE = "CSAL"

# NearOpt (lower bound) cost values in TB, per (workload, QLC_COST_MULTIPLIER)
NEAROPT_COSTS_TB = {
    ("Alibaba1", 2.88): 14.35,
    ("Alibaba1", 8.64): 23.24,
    ("Alibaba2", 2.88): 18.61,
    ("Alibaba2", 8.64): 31.36,
    ("Alibaba3", 2.88): 16.85,
    ("Alibaba3", 8.64): 32.15,
    ("YCSB-A", 2.88): 18.09,
    ("YCSB-A", 8.64): 32.11,
    ("Varmail", 2.88): 22.60,   # TODO: temporary placeholder
    ("Varmail", 8.64): 47.85,   # TODO: temporary placeholder
    ("FIO", 2.88): 14.23,
    ("FIO", 8.64): 22.31,
}

# Configs that have histogram data (for Graphs E, F)
HISTOGRAM_CONFIGS = ["SepBIT", "REFlash_COLD_FIXED", "REFlash_80", "REFlash"]

# All configs for Graphs A, B, C, D, etc. (histogram 제외)
ALL_CONFIGS = ["OpenCAS", "CSAL", "Greedy", "SepBIT", "REFlash_80", "REFlash"]

# Configs for Graph I (utilization scatter) - ordered for comparison
GRAPH_I_CONFIGS = ["OpenCAS", "REFlash", "CSAL"]
# Configs for Graph B2 (WAF bar) - ordered for comparison
GRAPH_B2_CONFIGS = ["OpenCAS", "Greedy", "CSAL"]
# Configs for Graph C (QLC writes timeseries) - tiering configs only
GRAPH_C_CONFIGS = ["OpenCAS", "CSAL", "SepBIT", "REFlash_80", "REFlash"]
# Configs for Graph C2 (QLC writes bar) - ordered for comparison
GRAPH_C2_CONFIGS = ["OpenCAS", "CSAL", "Greedy", "SepBIT", "REFlash_80", "REFlash"]
# Configs for Graph G (throughput)
GRAPH_G_CONFIGS = ["OpenCAS", "CSAL", "Greedy", "SepBIT", "REFlash_80", "REFlash"]
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
    "graph_j2": os.path.join(OUTPUT_DIR, "A_graph_J2_c2_utilization.pdf"),
}
