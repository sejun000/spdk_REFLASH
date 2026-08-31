#!/usr/bin/env python3
"""Capture one low-overhead perf profile per SPDK process during replay."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from datetime import datetime, timezone


CATEGORIES = [
    (
        "active_stream_classification",
        re.compile(
            r"MultiHotCold::|get_segment_to_active_stream|get_segment_with_stream_policy|"
            r"compute_stream_interval|get_multi_hot_cold_streams"
        ),
    ),
    (
        "adaptive_background_balancing",
        re.compile(
            r"LogCache::periodic|periodic_ghost|update_ghost|sum_invalidate_rate|"
            r"CbEvictPolicy::|score_(warm|hot|cold|greedy|age|sepbit)|throttle_"
        ),
    ),
    (
        "host_level_gc",
        re.compile(
            r"(^|[^A-Za-z])gc_|GcIo|LogCache::(prepare_gc|finalize_gc|abort_gc|"
            r"evict_and_compaction|copy_block)|start_gc_or_evict"
        ),
    ),
    (
        "csal_ftl_compaction_gc",
        re.compile(
            r"compaction_|chunk_compaction|compact_user_maps|ftl_reloc|"
            r"ftl_band|ftl_nv_cache_(process|throttle)"
        ),
    ),
    (
        "csal_l2p_metadata",
        re.compile(r"ftl_l2p|ftl_mngt|ftl_md|l2p_(cache|page|pin|unpin)"),
    ),
    (
        "eviction_backend_flush",
        re.compile(
            r"(^|[^A-Za-z])evict_|EvictIo|LogCache::(prepare_evict|finalize_evict|"
            r"abort_evict|evict_segment|flush_block_to_backend)"
        ),
    ),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    repo = Path(__file__).resolve().parents[1]
    parser.add_argument("--socket", default="/var/tmp/spdk.sock")
    parser.add_argument("--rpc", default=str(repo / "scripts" / "rpc.py"))
    parser.add_argument(
        "--perf", default="/usr/lib/linux-tools/6.8.0-138-generic/perf"
    )
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--stats-dir", default=str(repo / "ssd_waf" / "logging"))
    parser.add_argument("--stats-glob", default="REFLASH_R864_*.csv")
    parser.add_argument("--profile-label", default="REFLASH_R864")
    parser.add_argument("--trigger-host-write-tib", type=float, default=8.0)
    parser.add_argument("--duration-sec", type=int, default=180)
    parser.add_argument("--frequency", type=int, default=99)
    parser.add_argument("--poll-sec", type=float, default=10.0)
    parser.add_argument("--watch-unit")
    parser.add_argument("--runner-pattern", default="run_r864_ngc34_dwpd2.sh")
    return parser.parse_args()


def unit_is_running(unit: str | None) -> bool | None:
    if not unit:
        return None
    try:
        result = subprocess.run(
            ["systemctl", "is-active", "--quiet", unit],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return None
    return result.returncode == 0


def proc_cmdlines() -> list[tuple[int, str]]:
    processes: list[tuple[int, str]] = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            command = (entry / "cmdline").read_bytes().replace(b"\0", b" ").decode(
                "utf-8", errors="replace"
            )
        except (OSError, PermissionError):
            continue
        if command:
            processes.append((int(entry.name), command))
    return processes


def runner_is_running(pattern: str) -> bool:
    own_pid = os.getpid()
    return any(pid != own_pid and pattern in command for pid, command in proc_cmdlines())


def replay_is_active() -> bool:
    return any(
        "replay_trace.real" in command or "replay_trace.release" in command
        for _, command in proc_cmdlines()
    )


def read_ngc(pid: int) -> str:
    try:
        environment = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
    except (OSError, PermissionError):
        return "unknown"
    for item in environment:
        if item.startswith(b"ICACHE_N_GC="):
            return item.split(b"=", 1)[1].decode("ascii", errors="replace")
    return "unknown"


def get_spdk_pid(rpc: str, socket: str) -> int | None:
    try:
        result = subprocess.run(
            [sys.executable, rpc, "-s", socket, "framework_get_reactors"],
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
        )
        return int(json.loads(result.stdout)["pid"])
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError, KeyError, ValueError):
        return None


def process_start_epoch(pid: int) -> float:
    try:
        stat_fields = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8").split()
        start_ticks = int(stat_fields[21])
        boot_time = next(
            int(line.split()[1])
            for line in Path("/proc/stat").read_text(encoding="utf-8").splitlines()
            if line.startswith("btime ")
        )
        return boot_time + start_ticks / os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError, StopIteration):
        return time.time()


def read_last_host_write_mib(
    stats_dir: str, stats_glob: str, min_mtime: float
) -> tuple[float, Path] | None:
    candidates = [
        path
        for path in Path(stats_dir).glob(stats_glob)
        if "_NGC" not in path.name and path.stat().st_mtime >= min_mtime
    ]
    if not candidates:
        return None
    path = max(candidates, key=lambda item: item.stat().st_mtime_ns)
    try:
        with path.open("rb") as stream:
            header = stream.readline().decode("utf-8", errors="replace").strip().split(",")
            column = header.index("host_write_MB")
            stream.seek(0, os.SEEK_END)
            end = stream.tell()
            if end == 0:
                return None
            position = end - 1
            while position > 0:
                stream.seek(position)
                if stream.read(1) == b"\n" and position < end - 1:
                    break
                position -= 1
            stream.seek(position + 1 if position else 0)
            row = stream.readline().decode("utf-8", errors="replace").strip().split(",")
        return float(row[column]), path
    except (OSError, ValueError, IndexError):
        return None


def make_report(perf: str, data_path: Path, *, per_thread: bool = False) -> str:
    sort_keys = "pid,symbol" if per_thread else "symbol"
    fields = "overhead,pid,symbol" if per_thread else "overhead,symbol"
    command = [
        perf,
        "report",
        "--input",
        str(data_path),
        "--stdio",
        "--no-children",
    ]
    if per_thread:
        # Keep percentages relative to the entire SPDK process so multiplying
        # by process user-CPU cores gives the selected reactor's core usage.
        command.extend(["--percentage", "absolute"])
    command.extend(
        [
            "--sort", sort_keys,
            "--percent-limit", "0.00" if per_thread else "0.01",
            "--fields", fields,
        ]
    )
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=300,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "perf report failed")
    return result.stdout


def summarize_report(report: str, metadata: dict) -> str:
    totals = {name: 0.0 for name, _ in CATEGORIES}
    matched = {name: [] for name, _ in CATEGORIES}
    parsed_total = 0.0
    line_pattern = re.compile(r"^\s*([0-9]+(?:\.[0-9]+)?)%\s+(.+?)\s*$")

    for line in report.splitlines():
        match = line_pattern.match(line)
        if not match:
            continue
        percent = float(match.group(1))
        symbol = match.group(2)
        parsed_total += percent
        for name, pattern in CATEGORIES:
            if pattern.search(symbol):
                totals[name] += percent
                matched[name].append((percent, symbol))
                break

    lines = [
        "SPDK perf category summary (exclusive self-sample percentages)",
        f"pid={metadata['pid']} N_gc={metadata['n_gc']} duration_sec={metadata['duration_sec']}",
        "",
    ]
    for name, _ in CATEGORIES:
        lines.append(f"{name}: {totals[name]:.3f}%")
        for percent, symbol in sorted(matched[name], reverse=True)[:20]:
            lines.append(f"  {percent:8.3f}%  {symbol}")
    categorized = sum(totals.values())
    lines.extend(
        [
            "",
            f"categorized_self_samples: {categorized:.3f}%",
            f"all_reported_self_samples: {parsed_total:.3f}%",
            "Note: categories are mutually exclusive and report direct CPU samples, not I/O wait.",
        ]
    )
    return "\n".join(lines) + "\n"


def capture_profile(
    args: argparse.Namespace, pid: int, trigger_host_write_tib: float, stats_path: Path
) -> None:
    n_gc = read_ngc(pid)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    label = args.profile_label.replace("/", "_").replace(" ", "_")
    ngc_suffix = f"_NGC{n_gc}" if n_gc != "unknown" else ""
    base = Path(args.output_dir).resolve() / f"{label}{ngc_suffix}_{timestamp}"
    data_path = base.with_suffix(".data")
    report_path = base.with_suffix(".report.txt")
    thread_report_path = base.with_suffix(".threads.report.txt")
    summary_path = base.with_suffix(".categories.txt")
    metadata_path = base.with_suffix(".metadata.json")
    metadata = {
        "timestamp_utc": timestamp,
        "pid": pid,
        "n_gc": n_gc,
        "duration_sec": args.duration_sec,
        "frequency_hz": args.frequency,
        "event": "cycles:u",
        "call_graph": "frame-pointer",
        "profile_start_host_write_tib": trigger_host_write_tib,
        "stats_csv": str(stats_path),
    }
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")

    command = [
        args.perf,
        "record",
        "--output",
        str(data_path),
        "--event",
        "cycles:u",
        "--freq",
        str(args.frequency),
        "--call-graph",
        "fp",
        "--pid",
        str(pid),
        "--",
        "sleep",
        str(args.duration_sec),
    ]
    print(f"starting perf profile: pid={pid}, N_gc={n_gc}, output={data_path}", flush=True)
    result = subprocess.run(command, check=False, text=True)
    ending = read_last_host_write_mib(
        args.stats_dir, args.stats_glob, process_start_epoch(pid) - 60.0
    )
    if ending is not None:
        metadata["profile_end_host_write_tib"] = ending[0] / (1024.0 * 1024.0)
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    if result.returncode != 0:
        print(f"perf record failed with status {result.returncode}", flush=True)
        return

    try:
        report = make_report(args.perf, data_path)
        thread_report = make_report(args.perf, data_path, per_thread=True)
        report_path.write_text(report, encoding="utf-8")
        thread_report_path.write_text(thread_report, encoding="utf-8")
        summary_path.write_text(summarize_report(report, metadata), encoding="utf-8")
        print(f"perf report complete: {summary_path}", flush=True)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"could not generate perf report: {error}", flush=True)


def main() -> int:
    args = parse_args()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    profiled: set[int] = set()
    last_progress_log: dict[int, float] = {}
    print(
        f"perf watcher waiting for replay and {args.trigger_host_write_tib:.3f} TiB host writes; "
        f"duration={args.duration_sec}s, output={output_dir}",
        flush=True,
    )

    while True:
        running = unit_is_running(args.watch_unit)
        if running is None:
            running = runner_is_running(args.runner_pattern)
        if not running:
            print("benchmark runner stopped; perf watcher exiting", flush=True)
            return 0

        pid = get_spdk_pid(args.rpc, args.socket)
        active = replay_is_active()
        if pid is None or not active:
            time.sleep(args.poll_sec)
            continue

        progress = read_last_host_write_mib(
            args.stats_dir, args.stats_glob, process_start_epoch(pid) - 60.0
        )
        if progress is None:
            time.sleep(args.poll_sec)
            continue
        host_write_mib, stats_path = progress
        host_write_tib = host_write_mib / (1024.0 * 1024.0)
        now = time.monotonic()
        if now - last_progress_log.get(pid, 0.0) >= 300.0:
            print(
                f"pid={pid} N_gc={read_ngc(pid)} host_write={host_write_tib:.3f} TiB "
                f"stats={stats_path.name}",
                flush=True,
            )
            last_progress_log[pid] = now
        if pid not in profiled and host_write_tib >= args.trigger_host_write_tib:
            profiled.add(pid)
            capture_profile(args, pid, host_write_tib, stats_path)
        time.sleep(args.poll_sec)


if __name__ == "__main__":
    raise SystemExit(main())
