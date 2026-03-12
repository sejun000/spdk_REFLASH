#!/usr/bin/env python3
"""
Graph plotting script for various performance metrics.
Supports multi-workload grouped bar graphs and single-workload timeseries.
"""

import argparse
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import re
import os
from plot_config import (
    CONFIGS, CSV_COLUMNS, QLC_COST_MULTIPLIERS, NORMALIZATION_BASE,
    NEAROPT_COSTS_TB,
    HISTOGRAM_CONFIGS, ALL_CONFIGS, GRAPH_B2_CONFIGS, GRAPH_C_CONFIGS,
    GRAPH_C2_CONFIGS, GRAPH_G_CONFIGS, GRAPH_H_CONFIGS, GRAPH_I_CONFIGS,
    OUTPUT_FILES, WORKLOADS, DEFAULT_WORKLOAD, COST_START_HOST_WRITE_GB
)

# Set style
plt.style.use('seaborn-v0_8-whitegrid')
plt.rcParams['figure.figsize'] = (14, 10)
plt.rcParams['font.size'] = 30
plt.rcParams['axes.labelsize'] = 31
plt.rcParams['axes.titlesize'] = 34
plt.rcParams['xtick.labelsize'] = 26
plt.rcParams['ytick.labelsize'] = 26
plt.rcParams['legend.fontsize'] = 23
plt.rcParams['axes.linewidth'] = 2.5
plt.rcParams['axes.edgecolor'] = 'black'
plt.rcParams['xtick.major.width'] = 2.0
plt.rcParams['ytick.major.width'] = 2.0
plt.rcParams['xtick.minor.width'] = 1.5
plt.rcParams['ytick.minor.width'] = 1.5
plt.rcParams['grid.color'] = 'black'
plt.rcParams['grid.linestyle'] = ':'
plt.rcParams['grid.alpha'] = 0.4
plt.rcParams['grid.linewidth'] = 1.0

# Consistent color mapping for configs
# REFlash and NearOpt: bold/vivid colors, others: medium pastel tones
CONFIG_COLORS = {
    "SepBIT": "#F0C070",            # medium pastel amber/yellow
    "REFlash_COLD_FIXED": "#F0A870",# medium pastel orange
    "REFlash_80": "#80C080",        # medium pastel green
    "REFlash": "#C41E3A",           # bold crimson red
    "REFlash_2.88": "#E85050",      # bold medium red
    "REFlash_8.64": "#C41E3A",      # bold dark red
    "CSAL+GC": "#E090B0",          # medium pastel pink
    "REFlash_Beta_Control": "#C09870", # medium pastel brown
    "CSAL": "#A880C0",             # medium pastel purple
    "OpenCAS": "#7BAFD4",          # medium pastel blue
    "QLC only": "#A8A8A8",         # medium pastel gray
    "NearOpt": "#1B4F9B",          # bold dark blue
}


def has_workload(config_name, workload):
    """Check if a config has data for the given workload."""
    return workload in CONFIGS.get(config_name, {})


def get_config(config_name, workload=None, run_index=0):
    """Get config dict for a feature and workload.
    If the workload entry is a list (REFlash multi-run), returns the entry at run_index.
    """
    if workload is None:
        workload = DEFAULT_WORKLOAD
    entry = CONFIGS[config_name][workload]
    if isinstance(entry, list):
        return entry[run_index]
    return entry


def get_run_count(config_name, workload=None):
    """Get number of runs for a config/workload combo."""
    if workload is None:
        workload = DEFAULT_WORKLOAD
    entry = CONFIGS.get(config_name, {}).get(workload)
    if entry is None:
        return 0
    if isinstance(entry, list):
        return len(entry)
    return 1


def get_active_workloads(config_list):
    """Get workloads that have at least one config with data, from WORKLOADS order."""
    return [w for w in WORKLOADS
            if any(has_workload(c, w) for c in config_list)]


def load_csv_data(config_name, workload=None, run_index=0):
    """Load CSV data and extract relevant columns.
    All cumulative values (TLC, QLC, FDP) are calculated as delta from
    the point where host_write starts (first non-zero host_write).
    """
    config = get_config(config_name, workload, run_index)
    csv_path = config["csv"]
    csv_type = config["csv_type"]
    columns = CSV_COLUMNS[csv_type]

    df = pd.read_csv(csv_path)

    # Convert MB to GB
    host_write_gb = df[columns["host_write"]].values / 1024
    tlc_write_gb = df[columns["tlc_write"]].values / 1024
    qlc_write_gb = df[columns["qlc_write"]].values / 1024
    fdp_host_write_gb = df[columns["fdp_host_write"]].values / 1024
    fdp_media_write_gb = df[columns["fdp_media_write"]].values / 1024

    # Find the first index where host_write > 0 (host write starts)
    start_idx = 0
    for i in range(len(host_write_gb)):
        if host_write_gb[i] > 0:
            start_idx = i
            break

    # Calculate all cumulative values as delta from the starting point
    tlc_start = tlc_write_gb[start_idx] if start_idx > 0 else 0
    qlc_start = qlc_write_gb[start_idx] if start_idx > 0 else 0

    # FDP SMART values may be 0 at row 0 (not yet read), find first non-zero as baseline
    fdp_baseline_idx = 0
    for i in range(len(fdp_host_write_gb)):
        if fdp_host_write_gb[i] > 0:
            fdp_baseline_idx = i
            break
    fdp_host_start = fdp_host_write_gb[fdp_baseline_idx]
    fdp_media_start = fdp_media_write_gb[fdp_baseline_idx]

    return {
        "host_write_gb": host_write_gb,
        "tlc_write_gb": tlc_write_gb - tlc_start,
        "qlc_write_gb": qlc_write_gb - qlc_start,
        "fdp_host_write_gb": fdp_host_write_gb - fdp_host_start,
        "fdp_media_write_gb": fdp_media_write_gb - fdp_media_start,
    }


def get_final_values(config_name, workload=None, run_index=0):
    """Get final values for a config.
    If COST_START_HOST_WRITE_GB > 0, TLC/QLC writes are measured
    from that host write point (delta from start point to end).
    For ocf type, QLC write is taken at the point where host_write stops
    changing (excludes post-workload period where host_write is flat).
    """
    config = get_config(config_name, workload, run_index)
    data = load_csv_data(config_name, workload, run_index)

    # For ocf type, use QLC at the point where host_write stops changing
    if config["csv_type"] == "ocf":
        final_host_write = data["host_write_gb"][-1]
        qlc_end_idx = len(data["host_write_gb"]) - 1
        for i in range(len(data["host_write_gb"]) - 1, -1, -1):
            if data["host_write_gb"][i] == final_host_write:
                qlc_end_idx = i
            else:
                break
    else:
        qlc_end_idx = len(data["host_write_gb"]) - 1

    tlc_start = 0
    qlc_start = 0
    if COST_START_HOST_WRITE_GB > 0:
        # Find the first index where host_write_gb >= COST_START_HOST_WRITE_GB
        for i in range(len(data["host_write_gb"])):
            if data["host_write_gb"][i] >= COST_START_HOST_WRITE_GB:
                tlc_start = data["tlc_write_gb"][i]
                qlc_start = data["qlc_write_gb"][i]
                break

    return {
        "host_write_gb": data["host_write_gb"][-1],
        "tlc_write_gb": data["tlc_write_gb"][-1] - tlc_start,
        "qlc_write_gb": data["qlc_write_gb"][qlc_end_idx] - qlc_start,
    }


def parse_histogram_from_stat_log(stat_log_path, histogram_name):
    """Parse histogram data from stat.log file (last occurrence)."""
    with open(stat_log_path, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()

    # Find all occurrences of the histogram
    pattern = rf"histogram,{histogram_name}\nbucket,age_min,age_max,count\n((?:\d+,[^,]+,[^,]+,\d+\n?)+)"
    matches = list(re.finditer(pattern, content))

    if not matches:
        return None

    # Get the last match
    last_match = matches[-1]
    histogram_data = last_match.group(1).strip()

    buckets = []
    counts = []
    for line in histogram_data.split('\n'):
        parts = line.split(',')
        if len(parts) >= 4:
            bucket = int(parts[0])
            count = int(parts[3])
            buckets.append(bucket)
            counts.append(count)

    return {"buckets": buckets, "counts": counts}


def parse_throughput_from_replay_trace(replay_trace_path):
    """Parse average throughput (MB/s) from replay trace log."""
    with open(replay_trace_path, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()

    # Look for "Average BW:" in the final results section
    match = re.search(r"Average BW:\s+([\d.]+)\s*MB/s", content)
    if match:
        return float(match.group(1))
    return None


def plot_graph_a():
    """Graph A: Scatter plot subplots - one per workload, horizontally arranged"""
    active_workloads = get_active_workloads(ALL_CONFIGS)
    n_workloads = len(active_workloads)
    if n_workloads == 0:
        print("Graph A: No workloads with data, skipping.")
        return

    fig, axes = plt.subplots(1, n_workloads, figsize=(12 * n_workloads, 10))
    if n_workloads == 1:
        axes = [axes]

    for i, workload in enumerate(active_workloads):
        ax = axes[i]
        for config_name in ALL_CONFIGS:
            if not has_workload(config_name, workload):
                continue
            final = get_final_values(config_name, workload)
            ax.scatter(final["tlc_write_gb"], final["qlc_write_gb"],
                       s=250, c=CONFIG_COLORS[config_name], label=config_name,
                       edgecolors='black', linewidths=1.5)

        ax.set_xlabel("TLC writes (GB)")
        ax.set_ylabel("QLC (evict) writes (GB)")
        ax.set_title(f"{workload}")
        ax.grid(True)
        ax.set_xlim(left=0)
        ax.set_ylim(bottom=0)

    # Single shared legend at top center, one row
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper center', bbox_to_anchor=(0.5, 1.05),
               ncol=len(labels), frameon=False)
    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_a"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_a']}")


def plot_graph_b():
    """Graph B: Time series - x: Host writes, y: WAF (FDP media written / FDP host written)
    Uses DEFAULT_WORKLOAD."""
    fig, ax = plt.subplots(figsize=(14, 10))

    for config_name in ALL_CONFIGS:
        if not has_workload(config_name, DEFAULT_WORKLOAD):
            continue
        data = load_csv_data(config_name, DEFAULT_WORKLOAD)
        mask = data["fdp_host_write_gb"] > 0
        waf = data["fdp_media_write_gb"][mask] / data["fdp_host_write_gb"][mask]
        ax.plot(data["host_write_gb"][mask], waf,
                label=config_name, linewidth=3, color=CONFIG_COLORS[config_name])

    ax.set_xlabel("Host writes (GB)")
    ax.set_ylabel("WAF (FDP media / FDP host)")
    ax.legend(loc='upper center', bbox_to_anchor=(0.5, 1.08), ncol=3, frameon=False)
    ax.grid(True)
    ax.set_xlim(left=500)
    ax.set_ylim(bottom=0, top=7)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_b"], dpi=150)
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_b']}")


def plot_graph_c():
    """Graph C: Time series - x: Host writes, y: QLC (evict) writes (GB)
    Uses DEFAULT_WORKLOAD."""
    fig, ax = plt.subplots(figsize=(14, 10))

    for config_name in GRAPH_C_CONFIGS:
        if not has_workload(config_name, DEFAULT_WORKLOAD):
            continue
        data = load_csv_data(config_name, DEFAULT_WORKLOAD)
        # Find the first index where host_write reaches its final value (exclude flush)
        final_host_write = data["host_write_gb"][-1]
        first_final_idx = len(data["host_write_gb"])
        for i in range(len(data["host_write_gb"]) - 1, -1, -1):
            if data["host_write_gb"][i] == final_host_write:
                first_final_idx = i
            else:
                break
        # Only plot up to first_final_idx (inclusive)
        plot_data_host = data["host_write_gb"][:first_final_idx + 1]
        plot_data_qlc = data["qlc_write_gb"][:first_final_idx + 1]
        # Apply COST_START delta if set
        if COST_START_HOST_WRITE_GB > 0:
            mask = plot_data_host >= COST_START_HOST_WRITE_GB
            qlc_start = plot_data_qlc[mask][0] if mask.any() else 0
            ax.plot(plot_data_host[mask], plot_data_qlc[mask] - qlc_start,
                    label=config_name, linewidth=3, color=CONFIG_COLORS[config_name])
        else:
            mask = plot_data_host > 0
            ax.plot(plot_data_host[mask], plot_data_qlc[mask],
                    label=config_name, linewidth=3, color=CONFIG_COLORS[config_name])

    ax.set_xlabel("Host writes (GB)")
    ax.set_ylabel("QLC (evict) writes (GB)")
    ax.legend(loc='upper center', bbox_to_anchor=(0.5, 1.08), ncol=3, frameon=False)
    ax.grid(True)
    ax.set_xlim(left=COST_START_HOST_WRITE_GB if COST_START_HOST_WRITE_GB > 0 else 0)
    ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_c"], dpi=150)
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_c']}")


def plot_graph_b2():
    """Graph B2: Grouped bar - Final WAF per workload, grouped by feature"""
    configs = GRAPH_B2_CONFIGS
    active_workloads = get_active_workloads(configs)
    n_workloads = len(active_workloads)
    n_configs = len(configs)
    if n_workloads == 0:
        print("Graph B2: No workloads with data, skipping.")
        return

    fig, ax = plt.subplots(figsize=(14, 10))
    x = np.arange(n_workloads)
    width = 0.8 / n_configs

    for i, config_name in enumerate(configs):
        vals = []
        for workload in active_workloads:
            if not has_workload(config_name, workload):
                vals.append(0)
                continue
            data = load_csv_data(config_name, workload)
            final_host_write = data["host_write_gb"][-1]
            first_idx = len(data["host_write_gb"]) - 1
            for j in range(len(data["host_write_gb"]) - 1, -1, -1):
                if data["host_write_gb"][j] == final_host_write:
                    first_idx = j
                else:
                    break
            fdp_host = data["fdp_host_write_gb"][first_idx]
            fdp_media = data["fdp_media_write_gb"][first_idx]
            vals.append(fdp_media / fdp_host if fdp_host > 0 else 0)

        offset = (i - (n_configs - 1) / 2) * width
        bars = ax.bar(x + offset, vals, width, label=config_name,
                      color=CONFIG_COLORS[config_name], edgecolor='black', linewidth=1.5)

        for bar in bars:
            height = bar.get_height()
            if height > 0:
                ax.annotate(f'{height:.2f}',
                            xy=(bar.get_x() + bar.get_width() / 2, height),
                            xytext=(0, 5), textcoords="offset points",
                            ha='center', va='bottom', fontsize=14)

    ax.set_ylabel("Device WA")
    ax.set_xticks(x)
    ax.set_xticklabels(active_workloads)
    ax.axhline(y=1.0, color='red', linestyle='--', linewidth=2)
    ax.legend(loc='upper center', bbox_to_anchor=(0.5, 1.08), ncol=3, frameon=False)
    ax.grid(True, axis='y')
    ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_b2"], dpi=150)
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_b2']}")


def plot_graph_c2():
    """Graph C2: Grouped bar - Final QLC (evict) writes per workload, grouped by feature"""
    configs = ["OpenCAS", "CSAL+GC", "CSAL"]
    workload = "Ali1"
    n_configs = len(configs)

    fig, ax = plt.subplots(figsize=(14, 10))
    x = np.arange(n_configs)

    vals = []
    for config_name in configs:
        if not has_workload(config_name, workload):
            vals.append(0)
            continue
        final = get_final_values(config_name, workload)
        vals.append(final["qlc_write_gb"])

    bars = ax.bar(x, vals, 0.6, color=[CONFIG_COLORS[c] for c in configs],
                  edgecolor='black', linewidth=1.5)

    for bar in bars:
        height = bar.get_height()
        if height > 0:
            ax.annotate(f'{height:.0f}',
                        xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 5), textcoords="offset points",
                        ha='center', va='bottom', fontsize=14)

    ax.set_ylabel("Capacity-tier writes (GB)")
    ax.set_xticks(x)
    ax.set_xticklabels(configs)
    ax.grid(True, axis='y')
    ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_c2"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_c2']}")


def _get_avg_utilization(config_name, workload):
    """Get average cache utilization for a config/workload (after host writes start)."""
    config = get_config(config_name, workload)
    csv_path = config["csv"]
    df = pd.read_csv(csv_path)
    csv_type = config["csv_type"]
    columns = CSV_COLUMNS[csv_type]
    host_write_gb = df[columns["host_write"]].values / 1024

    if csv_type == "ocf":
        occupancy = df["occupancy_blocks"].values.astype(float)
        free = df["free_blocks"].values.astype(float)
        total = occupancy + free
        utilization = np.where(total > 0, occupancy / total, 0)
    else:
        cache_size_gb = config.get("cache_size_gb")
        if not cache_size_gb:
            return 0
        valid_blocks = df["valid_blocks"].values.astype(float)
        utilization = (valid_blocks * 4096 / 1000 / 1000 / 1000) / cache_size_gb

    mask = host_write_gb >= COST_START_HOST_WRITE_GB
    if mask.sum() == 0:
        return 0
    return float(np.mean(utilization[mask]))


def plot_graph_c3():
    """Graph C3: Left - Final QLC writes bar, Right - Avg cache utilization bar"""
    configs = GRAPH_C2_CONFIGS
    active_workloads = get_active_workloads(configs)
    n_workloads = len(active_workloads)
    n_configs = len(configs)
    if n_workloads == 0:
        print("Graph C3: No workloads with data, skipping.")
        return

    fig, (ax_left, ax_right) = plt.subplots(1, 2, figsize=(24, 5.25))
    x = np.arange(n_workloads)
    width = 0.8 / n_configs

    # --- Left: QLC writes bar ---
    for i, config_name in enumerate(configs):
        vals = []
        for workload in active_workloads:
            if not has_workload(config_name, workload):
                vals.append(0)
                continue
            final = get_final_values(config_name, workload)
            vals.append(final["qlc_write_gb"] / 1000)

        offset = (i - (n_configs - 1) / 2) * width
        bars = ax_left.bar(x + offset, vals, width, label=config_name,
                           color=CONFIG_COLORS[config_name], edgecolor='black', linewidth=1.5)
        for bar in bars:
            height = bar.get_height()
            if height > 0:
                ax_left.annotate(f'{height:.1f}',
                                 xy=(bar.get_x() + bar.get_width() / 2, height),
                                 xytext=(0, 5), textcoords="offset points",
                                 ha='center', va='bottom', fontsize=14)

    ax_left.set_ylabel("Capacity-tier writes (TB)", fontsize=24)
    ax_left.set_xticks(x)
    ax_left.set_xticklabels(active_workloads, fontsize=23)
    ax_left.tick_params(axis='y', labelsize=23)
    ax_left.grid(True, axis='y')
    ax_left.set_ylim(bottom=0, top=5.5)
    ax_left.set_xlabel("(a) Capacity-tier writes", fontsize=28)

    # --- Right: Avg utilization bar ---
    for i, config_name in enumerate(configs):
        vals = []
        for workload in active_workloads:
            if not has_workload(config_name, workload):
                vals.append(0)
                continue
            v = _get_avg_utilization(config_name, workload)
            if config_name in ("REFlash_80", "SepBIT", "CSAL+GC"):
                v = round(v, 1)
            vals.append(v)

        offset = (i - (n_configs - 1) / 2) * width
        bars = ax_right.bar(x + offset, vals, width, label=config_name,
                            color=CONFIG_COLORS[config_name], edgecolor='black', linewidth=1.5)
        fmt = '.1f' if config_name in ("REFlash_80", "SepBIT", "CSAL+GC") else '.2f'
        for bar in bars:
            height = bar.get_height()
            if height > 0:
                ax_right.annotate(f'{height:{fmt}}',
                                  xy=(bar.get_x() + bar.get_width() / 2, height),
                                  xytext=(0, 5), textcoords="offset points",
                                  ha='center', va='bottom', fontsize=14)

    ax_right.set_ylabel("BUtil", fontsize=24)
    ax_right.set_xticks(x)
    ax_right.set_xticklabels(active_workloads, fontsize=23)
    ax_right.tick_params(axis='y', labelsize=23)
    ax_right.grid(True, axis='y')
    ax_right.set_ylim(bottom=0, top=1.2)
    ax_right.set_xlabel("(b) BUtil", fontsize=28)

    handles, labels = ax_left.get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper center', bbox_to_anchor=(0.5, 1.03),
               ncol=n_configs, frameon=False, fontsize=20)
    plt.tight_layout()
    plt.subplots_adjust(top=0.90)
    plt.savefig(OUTPUT_FILES["graph_c3"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_c3']}")


def plot_graph_c4():
    """Graph C4: Grouped bar - Host WA, Device WA, Total WA per workload
    Subplots per workload. x-axis: configs, 3 bars per config.
    Host WA = fdp_host_write / host_write
    Device WA = fdp_media_write / fdp_host_write
    Total WA = fdp_media_write / host_write
    """
    configs = GRAPH_C2_CONFIGS
    metrics = ["Host WA", "Device WA"]
    metric_colors = {"Host WA": "#4c72b0", "Device WA": "#dd8452"}
    active_workloads = get_active_workloads(configs)
    n_workloads = len(active_workloads)
    if n_workloads == 0:
        print("Graph C4: No workloads with data, skipping.")
        return

    fig, axes = plt.subplots(1, n_workloads, figsize=(12 * n_workloads, 10))
    if n_workloads == 1:
        axes = [axes]

    for wi, workload in enumerate(active_workloads):
        ax = axes[wi]
        n_configs = len(configs)
        n_metrics = len(metrics)
        x = np.arange(n_configs)
        width = 0.8 / n_metrics

        total_wa_vals = []
        for mi, metric in enumerate(metrics):
            vals = []
            for config_name in configs:
                if not has_workload(config_name, workload):
                    vals.append(0)
                    continue
                data = load_csv_data(config_name, workload)
                # Find start index at COST_START_HOST_WRITE_GB
                start_si = 0
                for i in range(len(data["host_write_gb"])):
                    if data["host_write_gb"][i] >= COST_START_HOST_WRITE_GB:
                        start_si = i
                        break

                hw = data["host_write_gb"][-1] - data["host_write_gb"][start_si]
                fdp_host = data["fdp_host_write_gb"][-1] - data["fdp_host_write_gb"][start_si]
                fdp_media = data["fdp_media_write_gb"][-1] - data["fdp_media_write_gb"][start_si]

                if metric == "Host WA":
                    vals.append(fdp_host / hw if hw > 0 else 0)
                    total_wa_vals.append(fdp_media / hw if hw > 0 else 0)
                elif metric == "Device WA":
                    vals.append(fdp_media / fdp_host if fdp_host > 0 else 0)

            offset = (mi - (n_metrics - 1) / 2) * width
            bars = ax.bar(x + offset, vals, width, label=metric,
                          color=metric_colors[metric], edgecolor='black', linewidth=1.5)

        # Total WA as red * marker above each config's bars
        for ci in range(n_configs):
            if total_wa_vals[ci] > 0:
                ax.plot(x[ci], total_wa_vals[ci], marker='*', color='red',
                        markersize=27, zorder=5,
                        label="Total WA" if ci == 0 else None)
                ax.annotate(f'{total_wa_vals[ci]:.2f}',
                            xy=(x[ci], total_wa_vals[ci]),
                            xytext=(0, 8), textcoords="offset points",
                            ha='center', va='bottom', fontsize=36, fontweight='bold', color='red')

        ax.set_ylabel("Write amplification", fontsize=40)
        ax.set_xticks(x)
        ax.set_xticklabels(configs, rotation=15, ha='right', fontsize=36)
        labels_abc = "abcdefghijklmnopqrstuvwxyz"
        ax.set_xlabel(f"({labels_abc[wi]}) {workload}", fontsize=48)
        ax.tick_params(axis='y', labelsize=36)
        ax.grid(True, axis='y')
        ax.set_ylim(bottom=0)

    # Single shared legend
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper center', bbox_to_anchor=(0.5, 1.05),
               ncol=len(labels), frameon=False, fontsize=40)
    plt.subplots_adjust(top=0.88)
    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_c4"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_c4']}")


def plot_graph_d():
    """Graph D: Subplots per QLC cost multiplier - Normalized cost
    Each subplot: grouped bars of normalized cost for all configs per workload.
    Cost = TLC writes + r * QLC writes, normalized to NORMALIZATION_BASE.
    Subplot titles: (a) r=X, (b) r=Y, etc.
    """
    configs = ALL_CONFIGS
    all_labels = ["QLC only"] + list(configs) + ["NearOpt"]
    active_workloads = get_active_workloads(configs)
    n_workloads = len(active_workloads)
    n_bars = len(all_labels)
    n_multipliers = len(QLC_COST_MULTIPLIERS)
    if n_workloads == 0:
        print("Graph D: No workloads with data, skipping.")
        return

    fig, axes = plt.subplots(1, n_multipliers, figsize=(14 * n_multipliers, 7))
    if n_multipliers == 1:
        axes = [axes]

    # Pre-compute final values for non-REFlash configs (independent of multiplier)
    final_vals_cache = {}
    for config_name in configs:
        if config_name == "REFlash":
            continue
        for workload in active_workloads:
            if has_workload(config_name, workload):
                final_vals_cache[(config_name, workload)] = get_final_values(config_name, workload)

    # Get host_write for QLC only
    host_writes = {}
    for workload in active_workloads:
        for config_name in configs:
            if (config_name, workload) in final_vals_cache:
                host_writes[workload] = final_vals_cache[(config_name, workload)]["host_write_gb"]
                break

    labels_abc = "abcdefghijklmnopqrstuvwxyz"

    for mi, multiplier in enumerate(QLC_COST_MULTIPLIERS):
        ax = axes[mi]
        x = np.arange(n_workloads)
        width = 0.8 / n_bars

        # For REFlash, use run_index=mi (different data per multiplier subplot)
        for workload in active_workloads:
            if has_workload("REFlash", workload) and get_run_count("REFlash", workload) > mi:
                final_vals_cache[("REFlash", workload, mi)] = get_final_values("REFlash", workload, run_index=mi)

        # Compute raw costs with this multiplier
        raw_costs = {}
        for config_name in configs:
            for workload in active_workloads:
                if config_name == "REFlash":
                    key = ("REFlash", workload, mi)
                    if key in final_vals_cache:
                        fv = final_vals_cache[key]
                        raw_costs[(config_name, workload)] = fv["tlc_write_gb"] + multiplier * fv["qlc_write_gb"]
                elif (config_name, workload) in final_vals_cache:
                    fv = final_vals_cache[(config_name, workload)]
                    raw_costs[(config_name, workload)] = fv["tlc_write_gb"] + multiplier * fv["qlc_write_gb"]

        # QLC only
        for workload in active_workloads:
            if workload in host_writes:
                raw_costs[("QLC only", workload)] = host_writes[workload] * multiplier

        # NearOpt (TB -> GB)
        for workload in active_workloads:
            nearopt_tb = NEAROPT_COSTS_TB.get((workload, multiplier))
            if nearopt_tb is not None:
                raw_costs[("NearOpt", workload)] = nearopt_tb * 1000

        # Normalization base per workload
        base_costs = {}
        for workload in active_workloads:
            base_costs[workload] = raw_costs.get((NORMALIZATION_BASE, workload), 1.0)

        for i, config_name in enumerate(all_labels):
            vals = []
            for workload in active_workloads:
                raw = raw_costs.get((config_name, workload), 0)
                base = base_costs[workload]
                vals.append(raw / base if base > 0 else 0)

            offset = (i - (n_bars - 1) / 2) * width
            bars = ax.bar(x + offset, vals, width, label=config_name,
                          color=CONFIG_COLORS.get(config_name, "#333333"),
                          edgecolor='black', linewidth=1.5)

            for bar in bars:
                height = bar.get_height()
                if height > 0:
                    if config_name == "QLC only":
                        # Always show value above graph area for QLC only
                        ax.annotate(f'{height:.2f}',
                                    xy=(bar.get_x() + bar.get_width() / 2, 2.0),
                                    xytext=(0, 3), textcoords="offset points",
                                    ha='center', va='bottom', fontsize=12, fontweight='bold',
                                    annotation_clip=False)
                    elif height > 2.0:
                        ax.annotate(f'{height:.2f}',
                                    xy=(bar.get_x() + bar.get_width() / 2, 1.97),
                                    ha='center', va='top', fontsize=12, fontweight='bold')
                    else:
                        ax.annotate(f'{height:.2f}',
                                    xy=(bar.get_x() + bar.get_width() / 2, height),
                                    xytext=(0, 5), textcoords="offset points",
                                    ha='center', va='bottom', fontsize=14)

        if mi == 0:
            ax.set_ylabel("Normalized cost")
        ax.set_xticks(x)
        ax.set_xticklabels(active_workloads)
        ax.grid(True, axis='y')
        ax.set_ylim(bottom=0, top=2.0)
        # Title below subplot
        ax.set_xlabel(f"({labels_abc[mi]}) r={multiplier}")

    # Shared legend at top - single row
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper center', bbox_to_anchor=(0.5, 1.05),
               ncol=len(labels), frameon=False)
    plt.tight_layout()
    plt.subplots_adjust(top=0.90)
    plt.savefig(OUTPUT_FILES["graph_d"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_d']}")


def plot_graph_d2():
    """Graph D2: Subplots per QLC cost multiplier - Actual cost
    Each subplot: grouped bars of actual cost for all configs per workload.
    Cost = TLC writes + r * QLC writes.
    REFlash uses run_index=mi (different data per multiplier subplot).
    Subplot titles: (a) r=X, (b) r=Y, etc.
    """
    configs = ALL_CONFIGS
    all_d2_labels = list(configs) + ["NearOpt"]
    active_workloads = get_active_workloads(configs)
    n_workloads = len(active_workloads)
    n_bars = len(all_d2_labels)
    n_multipliers = len(QLC_COST_MULTIPLIERS)
    if n_workloads == 0:
        print("Graph D2: No workloads with data, skipping.")
        return

    fig, axes = plt.subplots(1, n_multipliers, figsize=(14 * n_multipliers, 7))
    if n_multipliers == 1:
        axes = [axes]

    # Pre-compute final values for non-REFlash configs
    final_vals_cache = {}
    for config_name in configs:
        if config_name == "REFlash":
            continue
        for workload in active_workloads:
            if has_workload(config_name, workload):
                final_vals_cache[(config_name, workload)] = get_final_values(config_name, workload)

    labels_abc = "abcdefghijklmnopqrstuvwxyz"

    for mi, multiplier in enumerate(QLC_COST_MULTIPLIERS):
        ax = axes[mi]
        x = np.arange(n_workloads)
        width = 0.8 / n_bars

        # For REFlash, use run_index=mi
        for workload in active_workloads:
            if has_workload("REFlash", workload) and get_run_count("REFlash", workload) > mi:
                final_vals_cache[("REFlash", workload, mi)] = get_final_values("REFlash", workload, run_index=mi)

        for i, config_name in enumerate(all_d2_labels):
            vals = []
            for workload in active_workloads:
                if config_name == "NearOpt":
                    nearopt_tb = NEAROPT_COSTS_TB.get((workload, multiplier))
                    vals.append(nearopt_tb * 1000 if nearopt_tb else 0)
                elif config_name == "REFlash":
                    key = ("REFlash", workload, mi)
                    if key in final_vals_cache:
                        fv = final_vals_cache[key]
                        vals.append(fv["tlc_write_gb"] + multiplier * fv["qlc_write_gb"])
                    else:
                        vals.append(0)
                elif has_workload(config_name, workload):
                    fv = final_vals_cache[(config_name, workload)]
                    vals.append(fv["tlc_write_gb"] + multiplier * fv["qlc_write_gb"])
                else:
                    vals.append(0)

            offset = (i - (n_bars - 1) / 2) * width
            bars = ax.bar(x + offset, vals, width, label=config_name,
                          color=CONFIG_COLORS.get(config_name, "#333333"),
                          edgecolor='black', linewidth=1.5)

            for bar in bars:
                height = bar.get_height()
                if height > 0:
                    ax.annotate(f'{height:.1f}',
                                xy=(bar.get_x() + bar.get_width() / 2, height),
                                xytext=(0, 5), textcoords="offset points",
                                ha='center', va='bottom', fontsize=14)

        if mi == 0:
            ax.set_ylabel("Cost (GB)")
        ax.set_xticks(x)
        ax.set_xticklabels(active_workloads)
        ax.grid(True, axis='y')
        ax.set_ylim(bottom=0)
        ax.set_xlabel(f"({labels_abc[mi]}) r={multiplier}")

    # Shared legend at top
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper center', bbox_to_anchor=(0.5, 1.08),
               ncol=len(labels), frameon=False)
    plt.tight_layout()
    plt.subplots_adjust(top=0.85)
    plt.savefig(OUTPUT_FILES["graph_d2"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_d2']}")


def plot_graph_e():
    """Graph E: Histogram subplots - evicted_ages_with_segment for each config
    Uses DEFAULT_WORKLOAD. x-axis: bucket (0-79), y-axis: count
    """
    fig, axes = plt.subplots(2, 2, figsize=(18, 14))
    axes = axes.flatten()

    for i, config_name in enumerate(HISTOGRAM_CONFIGS):
        ax = axes[i]
        if not has_workload(config_name, DEFAULT_WORKLOAD):
            continue
        config = get_config(config_name, DEFAULT_WORKLOAD)

        hist_data = parse_histogram_from_stat_log(
            config["stat_log"], "evicted_ages_with_segment"
        )

        if hist_data:
            # Exclude last bucket (overflow bucket)
            buckets = hist_data["buckets"][:-1]
            counts = [c / 1e6 for c in hist_data["counts"][:-1]]
            ax.bar(buckets, counts,
                   color='steelblue', edgecolor='black', linewidth=0.5)
            ax.set_xlabel("Bucket (0-78)")
            ax.set_ylabel("Count (M)")
            ax.set_title(f"{config_name}")
            ax.grid(True, axis='y')
            ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_e"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_e']}")


def plot_graph_f():
    """Graph F: Histogram subplots - compacted_ages_with_segment for each config
    Uses DEFAULT_WORKLOAD. x-axis: bucket (0-79), y-axis: count
    """
    fig, axes = plt.subplots(2, 2, figsize=(18, 14))
    axes = axes.flatten()

    for i, config_name in enumerate(HISTOGRAM_CONFIGS):
        ax = axes[i]
        if not has_workload(config_name, DEFAULT_WORKLOAD):
            continue
        config = get_config(config_name, DEFAULT_WORKLOAD)

        hist_data = parse_histogram_from_stat_log(
            config["stat_log"], "compacted_ages_with_segment"
        )

        if hist_data:
            # Exclude last bucket (overflow bucket)
            buckets = hist_data["buckets"][:-1]
            counts = [c / 1e6 for c in hist_data["counts"][:-1]]
            ax.bar(buckets, counts,
                   color='coral', edgecolor='black', linewidth=0.5)
            ax.set_xlabel("Bucket (0-78)")
            ax.set_ylabel("Count (M)")
            ax.set_title(f"{config_name}")
            ax.grid(True, axis='y')
            ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_f"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_f']}")


def plot_graph_f2():
    """Graph F2: Histogram subplots - gc_copied_lifetime for each config
    Uses DEFAULT_WORKLOAD. x-axis: bucket (0-79), y-axis: count
    """
    fig, axes = plt.subplots(2, 2, figsize=(18, 14))
    axes = axes.flatten()

    for i, config_name in enumerate(HISTOGRAM_CONFIGS):
        ax = axes[i]
        if not has_workload(config_name, DEFAULT_WORKLOAD):
            continue
        config = get_config(config_name, DEFAULT_WORKLOAD)

        hist_data = parse_histogram_from_stat_log(
            config["stat_log"], "gc_copied_lifetime"
        )

        if hist_data:
            # Exclude last bucket (overflow bucket)
            buckets = hist_data["buckets"][:-1]
            counts = [c / 1e6 for c in hist_data["counts"][:-1]]
            ax.bar(buckets, counts,
                   color='mediumpurple', edgecolor='black', linewidth=0.5)
            ax.set_xlabel("Bucket (0-78)")
            ax.set_ylabel("Count (M)")
            ax.set_title(f"{config_name}")
            ax.grid(True, axis='y')
            ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_f2"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_f2']}")


def plot_graph_g():
    """Graph G: Grouped bar - Throughput (MB/s) per workload, grouped by feature"""
    configs = GRAPH_G_CONFIGS
    active_workloads = get_active_workloads(configs)
    n_workloads = len(active_workloads)
    n_configs = len(configs)
    if n_workloads == 0:
        print("Graph G: No workloads with data, skipping.")
        return

    fig, ax = plt.subplots(figsize=(14, 5.6))
    x = np.arange(n_workloads)
    width = 0.8 / n_configs

    for i, config_name in enumerate(configs):
        vals = []
        for workload in active_workloads:
            if not has_workload(config_name, workload):
                vals.append(0)
                continue
            config = get_config(config_name, workload)
            tp = parse_throughput_from_replay_trace(config["replay_trace"])
            vals.append(tp if tp else 0)

        offset = (i - (n_configs - 1) / 2) * width
        bars = ax.bar(x + offset, vals, width, label=config_name,
                      color=CONFIG_COLORS[config_name], edgecolor='black', linewidth=1.5)


    ax.set_ylabel("Throughput (MB/s)")
    ax.set_xticks(x)
    ax.set_xticklabels(active_workloads)
    ax.legend(loc='upper center', bbox_to_anchor=(0.5, 1.38), ncol=3, frameon=False)
    ax.grid(True, axis='y')
    ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.subplots_adjust(top=0.68)
    plt.savefig(OUTPUT_FILES["graph_g"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_g']}")


def plot_graph_h():
    """Graph H: Time series - x: Host writes, y: Valid block rate
    Uses DEFAULT_WORKLOAD."""
    fig, ax = plt.subplots(figsize=(14, 10))

    for config_name in GRAPH_H_CONFIGS:
        if not has_workload(config_name, DEFAULT_WORKLOAD):
            continue
        config = get_config(config_name, DEFAULT_WORKLOAD)
        cache_size_gb = config.get("cache_size_gb")
        if not cache_size_gb:
            print(f"  Skipping {config_name}: no cache_size_gb")
            continue

        csv_path = config["csv"]
        df = pd.read_csv(csv_path)

        csv_type = config["csv_type"]
        columns = CSV_COLUMNS[csv_type]
        host_write_gb = df[columns["host_write"]].values / 1024

        # valid_blocks * 4KB in GB, then divide by cache_size_gb
        valid_blocks = df["valid_blocks"].values.astype(float)
        valid_block_rate = (valid_blocks * 4096 / 1000 / 1000 / 1000) / cache_size_gb

        # Filter where host_write > 0
        mask = host_write_gb > 0
        ax.plot(host_write_gb[mask], valid_block_rate[mask],
                label=config_name, linewidth=3, color=CONFIG_COLORS[config_name])

    ax.set_xlabel("Host writes (GB)")
    ax.set_ylabel("Valid block rate")
    ax.legend(loc='upper center', bbox_to_anchor=(0.5, 1.08), ncol=3, frameon=False)
    ax.grid(True)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_h"], dpi=150)
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_h']}")


def _plot_utilization_for_config(ax, config_name, workload, label=None, run_index=0):
    """Helper: plot utilization scatter for a single config/run on the given axis."""
    config = get_config(config_name, workload, run_index)
    csv_path = config["csv"]
    df = pd.read_csv(csv_path)
    csv_type = config["csv_type"]
    columns = CSV_COLUMNS[csv_type]
    host_write_gb = df[columns["host_write"]].values / 1024

    if csv_type == "ocf":
        occupancy = df["occupancy_blocks"].values.astype(float)
        free = df["free_blocks"].values.astype(float)
        total = occupancy + free
        utilization = np.where(total > 0, occupancy / total, 0)
    else:
        cache_size_gb = config.get("cache_size_gb")
        if not cache_size_gb:
            print(f"  Skipping {config_name}: no cache_size_gb")
            return
        valid_blocks = df["valid_blocks"].values.astype(float)
        utilization = (valid_blocks * 4096 / 1000 / 1000 / 1000) / cache_size_gb

    display_label = label or config_name
    mask = host_write_gb > 0
    ax.scatter(host_write_gb[mask], utilization[mask],
               label=display_label, color=CONFIG_COLORS.get(display_label, CONFIG_COLORS.get(config_name, "#333333")),
               s=10, alpha=0.6)


def plot_graph_i():
    """Graph I: Scatter subplots - one per workload, horizontally arranged
    x: Host writes, y: Cache utilization rate
    REFlash is expanded into per-multiplier entries (REFlash_2.88, REFlash_8.64, ...)
    """
    active_workloads = get_active_workloads(GRAPH_I_CONFIGS)
    n_workloads = len(active_workloads)
    if n_workloads == 0:
        print("Graph I: No workloads with data, skipping.")
        return

    fig, axes = plt.subplots(1, n_workloads, figsize=(12 * n_workloads, 10))
    if n_workloads == 1:
        axes = [axes]

    for idx, workload in enumerate(active_workloads):
        ax = axes[idx]
        for config_name in GRAPH_I_CONFIGS:
            if not has_workload(config_name, workload):
                continue
            if config_name == "REFlash":
                # Expand REFlash into per-multiplier entries
                n_runs = get_run_count(config_name, workload)
                for mi, mult in enumerate(QLC_COST_MULTIPLIERS):
                    if mi >= n_runs:
                        break
                    label = f"REFlash_{mult}"
                    _plot_utilization_for_config(ax, config_name, workload, label=label, run_index=mi)
            else:
                _plot_utilization_for_config(ax, config_name, workload)

        ax.set_xlabel("Host writes (GB)")
        ax.set_ylabel("Cache utilization")
        ax.set_title(f"{workload}")
        ax.legend(loc='upper center', bbox_to_anchor=(0.5, 1.15), ncol=4, frameon=False)
        ax.grid(True)
        ax.set_xlim(left=0)
        ax.set_ylim(bottom=0)
    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_i"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_i']}")


def plot_graph_j():
    """Graph J: 3 subplots - (a) FDP WA bar, (b) QLC writes bar, (c) normalized TEC bar
    Shared legend at top center, one row. Ali2."""
    configs = ["OpenCAS", "CSAL", "REFlash"]
    workload = "Ali2"
    r = QLC_COST_MULTIPLIERS[0]  # 8.64

    fig, (ax_a, ax_b, ax_c) = plt.subplots(1, 3, figsize=(24, 6))

    n_configs = len(configs)
    x = np.arange(n_configs)

    # --- (a) Buffer-Tier Device WA (delta from COST_START_HOST_WRITE_GB) ---
    for i, config_name in enumerate(configs):
        val = 0
        if has_workload(config_name, workload):
            data = load_csv_data(config_name, workload)
            # Find start index at COST_START_HOST_WRITE_GB
            fdp_host_start = 0
            fdp_media_start = 0
            for si in range(len(data["host_write_gb"])):
                if data["host_write_gb"][si] >= COST_START_HOST_WRITE_GB:
                    fdp_host_start = data["fdp_host_write_gb"][si]
                    fdp_media_start = data["fdp_media_write_gb"][si]
                    break
            fdp_host = data["fdp_host_write_gb"][-1] - fdp_host_start
            fdp_media = data["fdp_media_write_gb"][-1] - fdp_media_start
            val = fdp_media / fdp_host if fdp_host > 0 else 0
        ax_a.bar(x[i], val, 0.6, label=config_name,
                 color=CONFIG_COLORS[config_name], edgecolor='black', linewidth=1.5)
        if val > 0:
            ax_a.annotate(f'{val:.2f}',
                          xy=(x[i], val),
                          xytext=(0, 5), textcoords="offset points",
                          ha='center', va='bottom', fontsize=14)

    ax_a.set_ylabel("WA")
    ax_a.set_xlabel("(a) Buffer-tier device WA")
    ax_a.set_xticks(x)
    ax_a.set_xticklabels(configs)
    ax_a.grid(True, axis='y')
    ax_a.set_ylim(bottom=0, top=4.5)

    # --- (b) Capacity-Tier Writes ---
    for i, config_name in enumerate(configs):
        val = 0
        if has_workload(config_name, workload):
            run_idx = 0
            final = get_final_values(config_name, workload, run_index=run_idx)
            val = final["qlc_write_gb"] / 1000  # GB to TB
        ax_b.bar(x[i], val, 0.6, label=config_name,
                 color=CONFIG_COLORS[config_name], edgecolor='black', linewidth=1.5)
        if val > 0:
            ax_b.annotate(f'{val:.2f}',
                          xy=(x[i], val),
                          xytext=(0, 5), textcoords="offset points",
                          ha='center', va='bottom', fontsize=14)

    ax_b.set_ylabel("Capacity-tier writes (TB)")
    ax_b.set_xlabel("(b) Capacity-tier writes")
    ax_b.set_xticks(x)
    ax_b.set_xticklabels(configs)
    ax_b.grid(True, axis='y')
    ax_b.set_ylim(bottom=0, top=5.5)

    # --- (c) Normalized TEC (stacked: buffer-tier + capacity-tier) ---
    # Compute raw cost components, normalize to OpenCAS
    raw_tlc = {}
    raw_qlc = {}
    raw_total = {}
    for config_name in configs:
        if has_workload(config_name, workload):
            fv = get_final_values(config_name, workload, run_index=0)
            raw_tlc[config_name] = fv["tlc_write_gb"]
            raw_qlc[config_name] = r * fv["qlc_write_gb"]
            raw_total[config_name] = raw_tlc[config_name] + raw_qlc[config_name]

    base_cost = raw_total.get("OpenCAS", 1.0)

    for i, config_name in enumerate(configs):
        tlc_val = raw_tlc.get(config_name, 0) / base_cost if base_cost > 0 else 0
        qlc_val = raw_qlc.get(config_name, 0) / base_cost if base_cost > 0 else 0
        total_val = tlc_val + qlc_val
        # Bottom: buffer-tier (solid)
        ax_c.bar(x[i], tlc_val, 0.6,
                 color=CONFIG_COLORS[config_name], edgecolor='black', linewidth=1.5)
        # Top: capacity-tier (hatched)
        ax_c.bar(x[i], qlc_val, 0.6, bottom=tlc_val,
                 color=CONFIG_COLORS[config_name], edgecolor='black', linewidth=1.5,
                 hatch='xx')
        if total_val > 0:
            ax_c.annotate(f'{total_val:.2f}',
                          xy=(x[i], total_val),
                          xytext=(0, 5), textcoords="offset points",
                          ha='center', va='bottom', fontsize=14)

    ax_c.set_ylabel("Normalized TEC")
    ax_c.set_xlabel("(c) Total endurance cost")
    ax_c.set_xticks(x)
    ax_c.set_xticklabels(configs)
    ax_c.grid(True, axis='y')
    ax_c.set_ylim(bottom=0, top=1.15)
    # Stacked legend for (c) subplot - horizontal, inside top-left
    from matplotlib.patches import Patch
    ax_c.legend(handles=[
        Patch(facecolor='#E0E0E0', edgecolor='black', label='Buffer-tier'),
        Patch(facecolor='#E0E0E0', edgecolor='black', hatch='xx', label='Capacity-tier'),
    ], loc='upper right', ncol=1, fontsize=20,
       frameon=True, edgecolor='black', fancybox=False, handlelength=0.8)

    # Shared legend - one row, top center
    handles, labels = ax_a.get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper center', bbox_to_anchor=(0.5, 1.08),
               ncol=len(labels), frameon=False, fontsize=27)
    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_j"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_j']}")


GRAPH_FUNCTIONS = {
    "a": plot_graph_a,
    "b2": plot_graph_b2,
    "c2": plot_graph_c2,
    "c3": plot_graph_c3,
    "c4": plot_graph_c4,
    "d": plot_graph_d,
    "d2": plot_graph_d2,
    "e": plot_graph_e,
    "f": plot_graph_f,
    "f2": plot_graph_f2,
    "g": plot_graph_g,
    "i": plot_graph_i,
    "j": plot_graph_j,
}


def main():
    parser = argparse.ArgumentParser(description="Plot performance graphs")
    parser.add_argument("--graph", nargs="+", choices=list(GRAPH_FUNCTIONS.keys()),
                        help="Specific graph(s) to plot (e.g. --graph h or --graph a b h). Default: all")
    args = parser.parse_args()

    graphs = args.graph if args.graph else list(GRAPH_FUNCTIONS.keys())

    print(f"Generating graphs: {', '.join(g.upper() for g in graphs)}...")
    for g in graphs:
        GRAPH_FUNCTIONS[g]()

    print("\nDone!")


if __name__ == "__main__":
    main()
