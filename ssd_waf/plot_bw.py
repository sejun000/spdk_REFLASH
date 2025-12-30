#!/usr/bin/env python3
import sys
import glob
import os

if len(sys.argv) < 2:
    print("Usage: python3 plot_bw.py <timestamp>")
    print("Example: python3 plot_bw.py 20251230_140117")
    sys.exit(1)

timestamp = sys.argv[1]
log_pattern = f"fio_bw_{timestamp}_bw.*.log"
log_files = sorted(glob.glob(log_pattern))

if not log_files:
    print(f"No log files found matching: {log_pattern}")
    sys.exit(1)

print(f"Found {len(log_files)} log files:")
for f in log_files:
    print(f"  {f}")

# Parse and sum 2nd column from all job logs
times = {}
for log_file in log_files:
    with open(log_file, 'r') as f:
        for line in f:
            parts = line.strip().split(',')
            if len(parts) >= 2:
                time_ms = int(parts[0].strip())
                bw_kib = int(parts[1].strip())
                time_sec = time_ms // 1000
                if time_sec not in times:
                    times[time_sec] = 0
                times[time_sec] += bw_kib

if not times:
    print("No data found")
    sys.exit(1)

# Convert KiB/s to MiB/s (matches fio display)
sorted_times = sorted(times.keys())
bw_mibs = [times[t] / 1024 for t in sorted_times]

print(f"\nData points: {len(bw_mibs)}")
print(f"Average: {sum(bw_mibs)/len(bw_mibs):.1f} MiB/s")
print(f"Max: {max(bw_mibs):.1f} MiB/s")
print(f"Min: {min(bw_mibs):.1f} MiB/s")

# Plot
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

plt.figure(figsize=(12, 6))
plt.plot(sorted_times, bw_mibs, 'b-', linewidth=0.8)
plt.xlabel('Time (seconds)')
plt.ylabel('Bandwidth (MiB/s)')
plt.title(f'FIO Random 4K Write - {timestamp}')
plt.grid(True, alpha=0.3)
plt.ylim(bottom=0)

plot_file = f"fio_bw_graph_{timestamp}.png"
plt.savefig(plot_file, dpi=150, bbox_inches='tight')
print(f"\nGraph saved: {plot_file}")
