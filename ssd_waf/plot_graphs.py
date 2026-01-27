#!/usr/bin/env python3
"""
Graph plotting script for various performance metrics.
"""

import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import re
import os
from plot_config import (
    CONFIGS, CSV_COLUMNS, QLC_COST_MULTIPLIER, NORMALIZATION_BASE,
    HISTOGRAM_CONFIGS, ALL_CONFIGS, OUTPUT_FILES
)

# Set style
plt.style.use('seaborn-v0_8-whitegrid')
plt.rcParams['figure.figsize'] = (14, 10)
plt.rcParams['font.size'] = 23
plt.rcParams['axes.labelsize'] = 24
plt.rcParams['axes.titlesize'] = 26
plt.rcParams['xtick.labelsize'] = 20
plt.rcParams['ytick.labelsize'] = 20
plt.rcParams['legend.fontsize'] = 18

# Consistent color mapping for configs
CONFIG_COLORS = {
    "SepBIT": "#1f77b4",           # blue
    "REFLASH_COLD_FIXED": "#ff7f0e", # orange
    "REFLASH_WARM_FIXED": "#2ca02c", # green
    "REFLASH": "#d62728",           # red
    "CSAL": "#9467bd",              # purple
    "OpenCAS": "#17becf",           # cyan
}


def load_csv_data(config_name):
    """Load CSV data and extract relevant columns.
    All cumulative values (TLC, QLC, FDP) are calculated as delta from
    the point where host_write starts (first non-zero host_write).
    """
    config = CONFIGS[config_name]
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
    fdp_host_start = fdp_host_write_gb[start_idx] if start_idx > 0 else 0
    fdp_media_start = fdp_media_write_gb[start_idx] if start_idx > 0 else 0

    return {
        "host_write_gb": host_write_gb,
        "tlc_write_gb": tlc_write_gb - tlc_start,
        "qlc_write_gb": qlc_write_gb - qlc_start,
        "fdp_host_write_gb": fdp_host_write_gb - fdp_host_start,
        "fdp_media_write_gb": fdp_media_write_gb - fdp_media_start,
    }


def get_final_values(config_name):
    """Get final values for a config.
    Find the first point where host_write reaches its final value
    (to exclude flush operations after host writes stop).
    """
    data = load_csv_data(config_name)
    final_host_write = data["host_write_gb"][-1]

    # Traverse backwards to find the first index where host_write equals final value
    first_idx = len(data["host_write_gb"]) - 1
    for i in range(len(data["host_write_gb"]) - 1, -1, -1):
        if data["host_write_gb"][i] == final_host_write:
            first_idx = i
        else:
            break

    return {
        "host_write_gb": data["host_write_gb"][first_idx],
        "tlc_write_gb": data["tlc_write_gb"][first_idx],
        "qlc_write_gb": data["qlc_write_gb"][first_idx],
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
    """Graph A: Scatter plot - x: TLC writes, y: QLC (evict) writes (GB)"""
    fig, ax = plt.subplots(figsize=(14, 10))

    for config_name in ALL_CONFIGS:
        final = get_final_values(config_name)
        ax.scatter(final["tlc_write_gb"], final["qlc_write_gb"],
                   s=250, c=CONFIG_COLORS[config_name], label=config_name, edgecolors='black', linewidths=1.5)

    ax.set_xlabel("TLC Writes (GB)")
    ax.set_ylabel("QLC (Evict) Writes (GB)")
    ax.set_title("Graph A: TLC vs QLC Writes")
    ax.legend(loc='best')
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_a"], dpi=150)
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_a']}")


def plot_graph_b():
    """Graph B: Time series - x: Host writes, y: WAF (FDP media written / FDP host written)"""
    fig, ax = plt.subplots(figsize=(14, 10))

    for config_name in ALL_CONFIGS:
        data = load_csv_data(config_name)
        # Filter out zero/initial values (fdp_host_write > 0)
        mask = data["fdp_host_write_gb"] > 0
        waf = data["fdp_media_write_gb"][mask] / data["fdp_host_write_gb"][mask]
        ax.plot(data["host_write_gb"][mask], waf,
                label=config_name, linewidth=3, color=CONFIG_COLORS[config_name])

    ax.set_xlabel("Host Writes (GB)")
    ax.set_ylabel("WAF (FDP Media / FDP Host)")
    ax.set_title("Graph B: Host Writes vs WAF")
    ax.legend(loc='best')
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=500)
    ax.set_ylim(bottom=0, top=7)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_b"], dpi=150)
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_b']}")


def plot_graph_c():
    """Graph C: Time series - x: Host writes, y: QLC (evict) writes (GB)"""
    fig, ax = plt.subplots(figsize=(14, 10))

    for config_name in ALL_CONFIGS:
        data = load_csv_data(config_name)
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
        # Filter out zero/initial values
        mask = plot_data_host > 0
        ax.plot(plot_data_host[mask], plot_data_qlc[mask],
                label=config_name, linewidth=3, color=CONFIG_COLORS[config_name])

    ax.set_xlabel("Host Writes (GB)")
    ax.set_ylabel("QLC (Evict) Writes (GB)")
    ax.set_title("Graph C: Host Writes vs QLC Writes")
    ax.legend(loc='best')
    ax.grid(True, alpha=0.3)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_c"], dpi=150)
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_c']}")


def plot_graph_d():
    """Graph D: Bar chart - Normalized cost (normalized to CSAL)
    Cost = TLC writes + 6.73 * QLC writes
    """
    fig, ax = plt.subplots(figsize=(14, 10))

    costs = {}
    for config_name in ALL_CONFIGS:
        final = get_final_values(config_name)
        cost = final["tlc_write_gb"] + QLC_COST_MULTIPLIER * final["qlc_write_gb"]
        costs[config_name] = cost

    # Normalize to CSAL
    base_cost = costs[NORMALIZATION_BASE]
    normalized_costs = {k: v / base_cost for k, v in costs.items()}

    x = np.arange(len(ALL_CONFIGS))
    bars = ax.bar(x, [normalized_costs[c] for c in ALL_CONFIGS],
                  color=[CONFIG_COLORS[c] for c in ALL_CONFIGS],
                  edgecolor='black', linewidth=1.5)

    ax.set_xlabel("Config")
    ax.set_ylabel("Normalized Cost (CSAL = 1.0)")
    ax.set_title("Graph D: Normalized Cost (TLC + 6.73 * QLC)")
    ax.set_xticks(x)
    ax.set_xticklabels(ALL_CONFIGS, rotation=45, ha='right')
    ax.axhline(y=1.0, color='red', linestyle='--', linewidth=2, label='CSAL baseline')
    ax.legend()
    ax.grid(True, alpha=0.3, axis='y')
    ax.set_ylim(bottom=0)

    # Add value labels on bars
    for bar, config in zip(bars, ALL_CONFIGS):
        height = bar.get_height()
        ax.annotate(f'{height:.2f}',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 5), textcoords="offset points",
                    ha='center', va='bottom', fontsize=18)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_d"], dpi=150)
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_d']}")


def plot_graph_d2():
    """Graph D-2: Bar chart - Actual cost (not normalized)
    Cost = TLC writes + 6.73 * QLC writes
    """
    fig, ax = plt.subplots(figsize=(14, 10))

    costs = {}
    for config_name in ALL_CONFIGS:
        final = get_final_values(config_name)
        cost = final["tlc_write_gb"] + QLC_COST_MULTIPLIER * final["qlc_write_gb"]
        costs[config_name] = cost

    x = np.arange(len(ALL_CONFIGS))
    bars = ax.bar(x, [costs[c] for c in ALL_CONFIGS],
                  color=[CONFIG_COLORS[c] for c in ALL_CONFIGS],
                  edgecolor='black', linewidth=1.5)

    ax.set_ylabel("Cost (GB)")
    ax.set_title("Graph D-2: Actual Cost (TLC + 6.73 * QLC)")
    ax.set_xticks(x)
    ax.set_xticklabels(ALL_CONFIGS, rotation=45, ha='right')
    ax.grid(True, alpha=0.3, axis='y')
    ax.set_ylim(bottom=0)

    # Add value labels on bars
    for bar, config in zip(bars, ALL_CONFIGS):
        height = bar.get_height()
        ax.annotate(f'{height:.1f}',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 5), textcoords="offset points",
                    ha='center', va='bottom', fontsize=18)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_d2"], dpi=150)
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_d2']}")


def plot_graph_e():
    """Graph E: Histogram subplots - evicted_ages_with_segment for each config
    x-axis: bucket (0-79), y-axis: count
    """
    fig, axes = plt.subplots(2, 2, figsize=(18, 14))
    axes = axes.flatten()

    for i, config_name in enumerate(HISTOGRAM_CONFIGS):
        ax = axes[i]
        config = CONFIGS[config_name]

        hist_data = parse_histogram_from_stat_log(
            config["stat_log"], "evicted_ages_with_segment"
        )

        if hist_data:
            # Exclude last bucket (overflow bucket)
            buckets = hist_data["buckets"][:-1]
            counts = hist_data["counts"][:-1]
            ax.bar(buckets, counts,
                   color='steelblue', edgecolor='black', linewidth=0.5)
            ax.set_xlabel("Bucket (0-78)")
            ax.set_ylabel("Count")
            ax.set_title(f"{config_name}")
            ax.grid(True, alpha=0.3, axis='y')
            ax.set_ylim(bottom=0)

    fig.suptitle("Graph E: Evicted Ages with Segment Histogram", fontsize=28, y=1.02)
    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_e"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_e']}")


def plot_graph_f():
    """Graph F: Histogram subplots - compacted_ages_with_segment for each config
    x-axis: bucket (0-79), y-axis: count
    """
    fig, axes = plt.subplots(2, 2, figsize=(18, 14))
    axes = axes.flatten()

    for i, config_name in enumerate(HISTOGRAM_CONFIGS):
        ax = axes[i]
        config = CONFIGS[config_name]

        hist_data = parse_histogram_from_stat_log(
            config["stat_log"], "compacted_ages_with_segment"
        )

        if hist_data:
            # Exclude last bucket (overflow bucket)
            buckets = hist_data["buckets"][:-1]
            counts = hist_data["counts"][:-1]
            ax.bar(buckets, counts,
                   color='coral', edgecolor='black', linewidth=0.5)
            ax.set_xlabel("Bucket (0-78)")
            ax.set_ylabel("Count")
            ax.set_title(f"{config_name}")
            ax.grid(True, alpha=0.3, axis='y')
            ax.set_ylim(bottom=0)

    fig.suptitle("Graph F: Compacted Ages with Segment Histogram", fontsize=28, y=1.02)
    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_f"], dpi=150, bbox_inches='tight')
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_f']}")


def plot_graph_g():
    """Graph G: Bar chart - Throughput (MB/s) for each config"""
    fig, ax = plt.subplots(figsize=(14, 10))

    throughputs = {}
    for config_name in ALL_CONFIGS:
        config = CONFIGS[config_name]
        tp = parse_throughput_from_replay_trace(config["replay_trace"])
        if tp:
            throughputs[config_name] = tp

    x = np.arange(len(throughputs))
    configs = list(throughputs.keys())
    values = [throughputs[c] for c in configs]

    bars = ax.bar(x, values,
                  color=[CONFIG_COLORS[c] for c in configs],
                  edgecolor='black', linewidth=1.5)

    ax.set_ylabel("Throughput (MB/s)")
    ax.set_title("Graph G: Throughput Comparison")
    ax.set_xticks(x)
    ax.set_xticklabels(configs, rotation=45, ha='right')
    ax.grid(True, alpha=0.3, axis='y')
    ax.set_ylim(bottom=0)

    # Add value labels on bars
    for bar, val in zip(bars, values):
        height = bar.get_height()
        ax.annotate(f'{val:.1f}',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 5), textcoords="offset points",
                    ha='center', va='bottom', fontsize=18)

    plt.tight_layout()
    plt.savefig(OUTPUT_FILES["graph_g"], dpi=150)
    plt.close()
    print(f"Saved: {OUTPUT_FILES['graph_g']}")


def main():
    print("Generating graphs...")

    plot_graph_a()
    plot_graph_b()
    plot_graph_c()
    plot_graph_d()
    plot_graph_d2()
    plot_graph_e()
    plot_graph_f()
    plot_graph_g()

    print("\nAll graphs generated successfully!")
    print("\nOutput files:")
    for name, path in OUTPUT_FILES.items():
        print(f"  {name}: {path}")


if __name__ == "__main__":
    main()
