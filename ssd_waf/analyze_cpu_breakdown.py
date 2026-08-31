#!/usr/bin/env python3
"""Report SPDK CPU-cost categories in single-core-equivalent units.

The perf profiles used here sample ``cycles:u``.  A perf symbol percentage is
therefore converted to CPU cores with the user-mode CPU usage measured during
the same profile window:

    category_core_equivalents = perf_self_pct / 100 * user_cpu_cores

The reactor idle value is a separate SPDK poller metric.  It is not OS idle
time and must not be stacked with the perf categories to make 100%.

With no arguments, this script analyzes the 2026-08-30/31 Alibaba-DWPD2 runs
for REFlash R864 N_gc={3,4} and CSAL.  Additional runs can be supplied with
``--run``; see ``--help`` for the field format.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import re
import sys


SCRIPT_DIR = Path(__file__).resolve().parent


@dataclass(frozen=True)
class RunSpec:
    label: str
    policy: str
    report: Path
    thread_report: Path | None
    metadata: Path
    reactor_csv: Path
    n_gc: int


DEFAULT_RUNS = (
    RunSpec(
        label="N_gc=3",
        policy="reflash",
        report=SCRIPT_DIR / "perf/r864_ngc34_dwpd2_nobeprefill/REFLASH_R864_NGC3_20260830T122550Z.report.txt",
        thread_report=SCRIPT_DIR / "perf/r864_ngc34_dwpd2_nobeprefill/REFLASH_R864_NGC3_20260830T122550Z.threads.report.txt",
        metadata=SCRIPT_DIR / "perf/r864_ngc34_dwpd2_nobeprefill/REFLASH_R864_NGC3_20260830T122550Z.metadata.json",
        reactor_csv=SCRIPT_DIR / "logging/spdk_reactor_util_R864_NGC34_dwpd2_nobeprefill_20260830.csv",
        n_gc=3,
    ),
    RunSpec(
        label="N_gc=4",
        policy="reflash",
        report=SCRIPT_DIR / "perf/r864_ngc34_dwpd2_nobeprefill/REFLASH_R864_NGC4_20260830T162046Z.report.txt",
        thread_report=SCRIPT_DIR / "perf/r864_ngc34_dwpd2_nobeprefill/REFLASH_R864_NGC4_20260830T162046Z.threads.report.txt",
        metadata=SCRIPT_DIR / "perf/r864_ngc34_dwpd2_nobeprefill/REFLASH_R864_NGC4_20260830T162046Z.metadata.json",
        reactor_csv=SCRIPT_DIR / "logging/spdk_reactor_util_R864_NGC34_dwpd2_nobeprefill_20260830.csv",
        n_gc=4,
    ),
    RunSpec(
        label="CSAL",
        policy="csal",
        report=SCRIPT_DIR / "perf/csal_dwpd2_nobeprefill/CSAL_dwpd2_NGC5_20260830T203759Z.report.txt",
        thread_report=None,
        metadata=SCRIPT_DIR / "perf/csal_dwpd2_nobeprefill/CSAL_dwpd2_NGC5_20260830T203759Z.metadata.json",
        reactor_csv=SCRIPT_DIR / "logging/spdk_reactor_util_CSAL_dwpd2_nobeprefill_20260830.csv",
        n_gc=5,
    ),
)


REPORT_LINE = re.compile(
    r"^\s*([0-9.]+)%\s+\[\.\]\s+(.+?)\s{2,}(?:-|[0-9])"
)
THREAD_REPORT_LINE = re.compile(
    r"^\s*([0-9.]+)%\s+(\d+):reactor_(\d+)\s+\[\.\]\s+(.+?)\s{2,}"
)

ACTIVE_STREAM = re.compile(
    r"MultiHotCold::|get_segment_to_active_stream|"
    r"get_segment_with_stream_policy|compute_stream_interval|"
    r"get_multi_hot_cold_streams"
)
ADAPTIVE_BALANCING = re.compile(
    r"LogCache::periodic|periodic_ghost|update_ghost|sum_invalidate_rate|"
    r"CbEvictPolicy::|score_(warm|hot|cold|greedy|age|sepbit)|throttle_"
)
HOST_LEVEL_GC = re.compile(
    r"(^|[^A-Za-z])gc_|GcIo|LogCache::(prepare_gc|finalize_gc|abort_gc|"
    r"evict_and_compaction|copy_block)|start_gc_or_evict"
)
REFLASH_EVICTION = re.compile(
    r"(^|[^A-Za-z])evict_|EvictIo|LogCache::(prepare_evict|finalize_evict|"
    r"abort_evict|evict_segment|flush_block_to_backend)"
)
CSAL_BACKEND_FLUSH = re.compile(
    r"compaction_|chunk_compaction|compact_user_maps|ftl_reloc|ftl_band|"
    r"ftl_nv_cache_(process|throttle)"
)
REFLASH_CACHE_LOOKUP = re.compile(
    r"GhostCache::access|LogCache::.*(lookup|find)|"
    r"_Hashtable<long, std::pair<long const, LogCache::Loc>.*::find"
)
CSAL_CACHE_LOOKUP = re.compile(r"ftl_l2p")
DEVICE_SUBMIT_COMPLETION = re.compile(
    r"nvme|bdev_io|spdk_bdev|"
    r"SpdkCacheDevice::(submit|zone_async_io_completion)|"
    r"zone_write_done|write_buffer_flush_done|"
    r"ftl_io_channel_poll|spdk_ftl_writev|bdev_ftl_submit_request|"
    r"ftl_io_(init|complete)|ftl_stats_bdev_io_completed"
)
HOST_INTERFACE = re.compile(
    r"nvmf|iscsi|sock|epoll|sendmsg|readv|connect_poller"
)


CATEGORY_LABELS = (
    ("active_stream", "Active-stream classification"),
    ("adaptive_balancing", "Adaptive balancing + periodic decision"),
    ("host_level_gc", "Host-level GC"),
    ("eviction_backend_flush", "Eviction / backend flush"),
    ("cache_lookup", "Cache / map lookup"),
    ("device_submit_completion", "Device submit / completion (buffer + backend)"),
    ("host_interface_one_core", "Host interface (one of N cores)"),
    ("reactor_idle_one_core", "Reactor idle (1 - categorized usage)"),
    ("total_one_core", "Total"),
)

ADDITIVE_CATEGORY_KEYS = (
    "active_stream",
    "adaptive_balancing",
    "host_level_gc",
    "eviction_backend_flush",
    "cache_lookup",
    "device_submit_completion",
    "host_interface_one_core",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--run",
        action="append",
        default=[],
        metavar="LABEL|POLICY|REPORT|METADATA|REACTOR_CSV|N_GC[|THREAD_REPORT]",
        help=(
            "Analyze a custom run. POLICY must be 'reflash' or 'csal'. "
            "THREAD_REPORT is required for REFlash device accounting. "
            "May be specified repeatedly; when present, default runs are omitted."
        ),
    )
    parser.add_argument(
        "--host-interface-cores",
        type=int,
        default=8,
        help="Divide aggregate host-interface cost by this many cores (default: 8)",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "csv", "json"),
        default="markdown",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Write the report to this file in addition to stdout",
    )
    args = parser.parse_args()
    if args.host_interface_cores <= 0:
        parser.error("--host-interface-cores must be positive")
    return args


def parse_custom_run(value: str) -> RunSpec:
    fields = value.split("|")
    if len(fields) not in {6, 7}:
        raise ValueError("expected six or seven '|' separated fields")
    label, policy, report, metadata, reactor_csv, n_gc = fields[:6]
    thread_report = fields[6] if len(fields) == 7 else None
    policy = policy.lower()
    if policy not in {"reflash", "csal"}:
        raise ValueError("POLICY must be 'reflash' or 'csal'")
    return RunSpec(
        label=label,
        policy=policy,
        report=Path(report).expanduser().resolve(),
        thread_report=(Path(thread_report).expanduser().resolve() if thread_report else None),
        metadata=Path(metadata).expanduser().resolve(),
        reactor_csv=Path(reactor_csv).expanduser().resolve(),
        n_gc=int(n_gc),
    )


def load_perf_symbols(path: Path) -> list[tuple[float, str]]:
    values: list[tuple[float, str]] = []
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = REPORT_LINE.match(line)
            if match:
                values.append((float(match.group(1)), match.group(2).strip()))
    if not values:
        raise ValueError(f"no perf symbol rows found in {path}")
    return values


def symbol_pct(values: list[tuple[float, str]], pattern: re.Pattern[str]) -> float:
    return sum(percent for percent, symbol in values if pattern.search(symbol))


def symbol_pct_excluding(
    values: list[tuple[float, str]],
    pattern: re.Pattern[str],
    excluded: tuple[re.Pattern[str], ...],
) -> float:
    """Sum a category after removing symbols owned by earlier categories."""
    return sum(
        percent
        for percent, symbol in values
        if pattern.search(symbol) and not any(item.search(symbol) for item in excluded)
    )


def thread_symbol_pct(
    path: Path, lcore: int, pattern: re.Pattern[str]
) -> float:
    """Return process-global perf self-percent attributed to one reactor."""
    total = 0.0
    matched_rows = 0
    with path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            match = THREAD_REPORT_LINE.match(line)
            if not match or int(match.group(3)) != lcore:
                continue
            matched_rows += 1
            if pattern.search(match.group(4).strip()):
                total += float(match.group(1))
    if not matched_rows:
        raise ValueError(f"no reactor_{lcore} perf rows found in {path}")
    return total


def parse_timestamp(value: str) -> datetime:
    if value.endswith("Z") and "T" in value and "-" not in value:
        return datetime.strptime(value, "%Y%m%dT%H%M%SZ").replace(tzinfo=timezone.utc)
    parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return parsed.astimezone(timezone.utc)


def reactor_window(
    path: Path, n_gc: int, start: datetime, end: datetime
) -> dict[str, float]:
    rows: list[dict[str, float]] = []
    with path.open("r", encoding="utf-8", newline="") as stream:
        for row in csv.DictReader(stream):
            try:
                timestamp = parse_timestamp(row["timestamp_utc"])
                row_n_gc = int(row["n_gc"])
                replay_active = int(row["replay_active"])
            except (KeyError, TypeError, ValueError):
                continue
            if (
                row.get("scope") != "all"
                or replay_active != 1
                or row_n_gc != n_gc
                or timestamp < start
                or timestamp > end
            ):
                continue
            try:
                rows.append(
                    {
                        "interval": float(row["interval_sec"]),
                        "busy": float(row["busy_ticks_delta"]),
                        "idle": float(row["idle_ticks_delta"]),
                        "usr_pct": float(row["usr_cpu_pct"]),
                        "sys_pct": float(row["sys_cpu_pct"]),
                        "irq_pct": float(row["irq_cpu_pct"]),
                    }
                )
            except (KeyError, TypeError, ValueError):
                continue
    if not rows:
        raise ValueError(f"no matching reactor rows in {path}")

    interval_total = sum(row["interval"] for row in rows)
    busy_total = sum(row["busy"] for row in rows)
    idle_total = sum(row["idle"] for row in rows)

    def weighted(field: str) -> float:
        return sum(row[field] * row["interval"] for row in rows) / interval_total

    return {
        "samples": float(len(rows)),
        "usr_cores": weighted("usr_pct") / 100.0,
        "sys_cores": weighted("sys_pct") / 100.0,
        "irq_cores": weighted("irq_pct") / 100.0,
        "busy_pct": 100.0 * busy_total / (busy_total + idle_total),
        "idle_pct": 100.0 * idle_total / (busy_total + idle_total),
    }


def analyze(spec: RunSpec, host_interface_cores: int) -> dict[str, object]:
    for path in (spec.report, spec.metadata, spec.reactor_csv):
        if not path.is_file():
            raise FileNotFoundError(path)

    metadata = json.loads(spec.metadata.read_text(encoding="utf-8"))
    start = parse_timestamp(str(metadata["timestamp_utc"]))
    end = start + timedelta(seconds=float(metadata["duration_sec"]))
    reactor = reactor_window(spec.reactor_csv, spec.n_gc, start, end)
    symbols = load_perf_symbols(spec.report)

    if spec.policy == "reflash":
        active_pct = symbol_pct(symbols, ACTIVE_STREAM)
        adaptive_pct = symbol_pct(symbols, ADAPTIVE_BALANCING)
        host_gc_pct = symbol_pct_excluding(
            symbols, HOST_LEVEL_GC, (ACTIVE_STREAM, ADAPTIVE_BALANCING)
        )
        eviction_pct = symbol_pct_excluding(
            symbols,
            REFLASH_EVICTION,
            (ACTIVE_STREAM, ADAPTIVE_BALANCING, HOST_LEVEL_GC),
        )
        lookup_pct = symbol_pct(symbols, REFLASH_CACHE_LOOKUP)
    else:
        active_pct = 0.0
        adaptive_pct = 0.0
        # Per the comparison definition, CSAL relocation/compaction is assigned
        # to eviction/backend flush rather than host-level GC.
        host_gc_pct = 0.0
        eviction_pct = symbol_pct(symbols, CSAL_BACKEND_FLUSH)
        lookup_pct = symbol_pct(symbols, CSAL_CACHE_LOOKUP)

    if spec.policy == "reflash":
        if spec.thread_report is None or not spec.thread_report.is_file():
            raise FileNotFoundError(
                f"REFlash requires a per-thread perf report: {spec.thread_report}"
            )
        # vbdev_icache pins its only log_worker to lcore 8.  Counting device
        # pollers process-wide incorrectly includes the other SPDK reactors.
        device_pct = thread_symbol_pct(
            spec.thread_report, 8, DEVICE_SUBMIT_COMPLETION
        )
        device_scope = "reactor_8/log_worker only"
    else:
        device_pct = symbol_pct(symbols, DEVICE_SUBMIT_COMPLETION)
        device_scope = "process-wide (CSAL has distributed completion pollers)"
    host_pct = symbol_pct(symbols, HOST_INTERFACE)
    user_cores = float(reactor["usr_cores"])

    def core_equivalent(perf_pct: float) -> float:
        return perf_pct / 100.0 * user_cores

    values: dict[str, float] = {
        "active_stream": core_equivalent(active_pct),
        "adaptive_balancing": core_equivalent(adaptive_pct),
        "host_level_gc": core_equivalent(host_gc_pct),
        "eviction_backend_flush": core_equivalent(eviction_pct),
        "cache_lookup": core_equivalent(lookup_pct),
        "device_submit_completion": core_equivalent(device_pct),
        "host_interface_one_core": core_equivalent(host_pct) / host_interface_cores,
    }
    categorized_usage = sum(values[key] for key in ADDITIVE_CATEGORY_KEYS)
    values["reactor_idle_one_core"] = 1.0 - categorized_usage
    values["total_one_core"] = categorized_usage + values["reactor_idle_one_core"]
    return {
        "label": spec.label,
        "policy": spec.policy,
        "profile_start_utc": start.isoformat(),
        "profile_end_utc": end.isoformat(),
        "profile_start_host_write_tib": metadata.get("profile_start_host_write_tib"),
        "profile_end_host_write_tib": metadata.get("profile_end_host_write_tib"),
        "reactor_samples": int(reactor["samples"]),
        "user_cpu_cores": user_cores,
        "system_cpu_cores": reactor["sys_cores"],
        "irq_cpu_cores": reactor["irq_cores"],
        "reactor_busy_pct": reactor["busy_pct"],
        "reactor_idle_pct": reactor["idle_pct"],
        "host_interface_cores_divisor": host_interface_cores,
        "device_scope": device_scope,
        "device_perf_self_pct": device_pct,
        "values_core_equivalent": values,
    }


def render_markdown(results: list[dict[str, object]]) -> str:
    labels = [str(result["label"]) for result in results]
    lines = [
        "# SPDK CPU breakdown",
        "",
        "Each cell shows CPU cores used and utilization relative to one CPU core.",
        "",
        "| Category | " + " | ".join(labels) + " |",
        "|---|" + "---:|" * len(labels),
    ]
    for key, display in CATEGORY_LABELS:
        values = [
            float(result["values_core_equivalent"][key]) * 100.0  # type: ignore[index]
            for result in results
        ]
        lines.append(
            f"| {display} | "
            + " | ".join(f"{value / 100.0:.3f} core ({value:.3f}%)" for value in values)
            + " |"
        )

    lines.extend(
        [
            "",
            "## Profile-window diagnostics",
            "",
            "| Metric | " + " | ".join(labels) + " |",
            "|---|" + "---:|" * len(labels),
            "| User CPU | "
            + " | ".join(f"{float(result['user_cpu_cores']):.3f} cores" for result in results)
            + " |",
            "| System CPU | "
            + " | ".join(f"{float(result['system_cpu_cores']):.3f} cores" for result in results)
            + " |",
            "| IRQ CPU | "
            + " | ".join(f"{float(result['irq_cpu_cores']):.3f} cores" for result in results)
            + " |",
            "| Device perf self | "
            + " | ".join(f"{float(result['device_perf_self_pct']):.3f}%" for result in results)
            + " |",
            "| Device measurement scope | "
            + " | ".join(str(result["device_scope"]) for result in results)
            + " |",
            "| Framework busy | "
            + " | ".join(f"{float(result['reactor_busy_pct']):.3f}%" for result in results)
            + " |",
            "| Framework idle | "
            + " | ".join(f"{float(result['reactor_idle_pct']):.3f}%" for result in results)
            + " |",
            "",
            "Notes:",
            "",
            "- Main-table idle is 1 core minus all preceding additive categories; the Total row therefore equals exactly 1 core.",
            "- REFlash device submit/completion counts reactor 8/log_worker only; it does not sum NVMe polling on the other reactors.",
            "- CSAL device submit/completion remains process-wide because its completion pollers are distributed across reactors.",
            "- Device core usage = process-global device perf self percentage for the selected scope x profile-window user CPU cores.",
            "- Device symbols combine buffer and backend devices because perf symbols do not identify the controller.",
            "- Host-interface cost is divided by the requested number of host cores.",
            "- Framework idle is the independent SPDK poller metric; it is retained only in diagnostics and is not used for the main-table residual.",
            "- Categories use direct self-cycle samples from the perf report; asynchronous I/O wait is excluded.",
        ]
    )
    return "\n".join(lines) + "\n"


def render_csv(results: list[dict[str, object]]) -> str:
    lines = ["category," + ",".join(str(result["label"]) for result in results)]
    for key, display in CATEGORY_LABELS:
        values = [
            float(result["values_core_equivalent"][key]) * 100.0  # type: ignore[index]
            for result in results
        ]
        lines.append(display + "," + ",".join(f"{value:.6f}" for value in values))
    return "\n".join(lines) + "\n"


def main() -> int:
    args = parse_args()
    try:
        runs = (
            [parse_custom_run(value) for value in args.run]
            if args.run
            else list(DEFAULT_RUNS)
        )
        results = [analyze(run, args.host_interface_cores) for run in runs]
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if args.format == "markdown":
        output = render_markdown(results)
    elif args.format == "csv":
        output = render_csv(results)
    else:
        output = json.dumps(results, indent=2) + "\n"

    sys.stdout.write(output)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
