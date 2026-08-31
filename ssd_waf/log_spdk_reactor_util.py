#!/usr/bin/env python3
"""Periodically record interval SPDK reactor busy/idle utilization.

SPDK's framework_get_reactors RPC reports cumulative busy and idle ticks.
This logger converts consecutive samples into per-interval utilization and
writes one row per reactor plus an aggregate row.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from datetime import datetime, timezone


CSV_FIELDS = [
    "timestamp_utc",
    "interval_sec",
    "spdk_pid",
    "n_gc",
    "replay_active",
    "scope",
    "lcore",
    "tid",
    "thread_names",
    "spdk_tick_rate_hz",
    "core_freq_mhz",
    "in_interrupt",
    "busy_ticks_delta",
    "idle_ticks_delta",
    "busy_pct",
    "idle_pct",
    "usr_jiffies_delta",
    "sys_jiffies_delta",
    "irq_jiffies_delta",
    "usr_cpu_pct",
    "sys_cpu_pct",
    "irq_cpu_pct",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    repo = Path(__file__).resolve().parents[1]
    parser.add_argument("--socket", default="/var/tmp/spdk.sock")
    parser.add_argument("--rpc", default=str(repo / "scripts" / "rpc.py"))
    parser.add_argument("--output", required=True)
    parser.add_argument("--interval", type=float, default=30.0)
    parser.add_argument("--watch-unit", help="Exit after this systemd unit stops")
    parser.add_argument(
        "--runner-pattern",
        default="run_r864_ngc34_dwpd2.sh",
        help="Fallback process pattern used when systemd is unavailable",
    )
    args = parser.parse_args()
    if args.interval <= 0:
        parser.error("--interval must be positive")
    return args


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
        return ""
    prefix = b"ICACHE_N_GC="
    for item in environment:
        if item.startswith(prefix):
            return item[len(prefix) :].decode("ascii", errors="replace")
    return ""


def get_reactors(rpc: str, socket: str) -> dict:
    result = subprocess.run(
        [sys.executable, rpc, "-s", socket, "framework_get_reactors"],
        check=True,
        capture_output=True,
        text=True,
        timeout=15,
    )
    return json.loads(result.stdout)


def thread_names(reactor: dict) -> str:
    names = [str(thread.get("name", "")) for thread in reactor.get("lw_threads", [])]
    return ";".join(name for name in names if name)


def write_interval(
    writer: csv.DictWriter,
    current: dict,
    previous: dict,
    interval_sec: float,
) -> int:
    pid = int(current["pid"])
    n_gc = read_ngc(pid)
    replay_active = int(replay_is_active())
    timestamp = datetime.now(timezone.utc).isoformat(timespec="seconds")
    previous_by_lcore = {int(r["lcore"]): r for r in previous["reactors"]}
    aggregate_busy = 0
    aggregate_idle = 0
    aggregate_usr = 0
    aggregate_sys = 0
    aggregate_irq = 0
    rows = 0
    user_hz = os.sysconf("SC_CLK_TCK")

    for reactor in sorted(current["reactors"], key=lambda item: int(item["lcore"])):
        lcore = int(reactor["lcore"])
        old = previous_by_lcore.get(lcore)
        if old is None:
            continue
        busy_delta = int(reactor["busy"]) - int(old["busy"])
        idle_delta = int(reactor["idle"]) - int(old["idle"])
        usr_delta = int(reactor.get("usr", 0)) - int(old.get("usr", 0))
        sys_delta = int(reactor.get("sys", 0)) - int(old.get("sys", 0))
        irq_delta = int(reactor.get("irq", 0)) - int(old.get("irq", 0))
        if busy_delta < 0 or idle_delta < 0:
            continue
        total = busy_delta + idle_delta
        busy_pct = 100.0 * busy_delta / total if total else 0.0
        idle_pct = 100.0 - busy_pct if total else 0.0
        aggregate_busy += busy_delta
        aggregate_idle += idle_delta
        aggregate_usr += max(0, usr_delta)
        aggregate_sys += max(0, sys_delta)
        aggregate_irq += max(0, irq_delta)
        writer.writerow(
            {
                "timestamp_utc": timestamp,
                "interval_sec": f"{interval_sec:.3f}",
                "spdk_pid": pid,
                "n_gc": n_gc,
                "replay_active": replay_active,
                "scope": "core",
                "lcore": lcore,
                "tid": reactor.get("tid", ""),
                "thread_names": thread_names(reactor),
                "spdk_tick_rate_hz": current.get("tick_rate", ""),
                "core_freq_mhz": reactor.get("core_freq", ""),
                "in_interrupt": int(bool(reactor.get("in_interrupt", False))),
                "busy_ticks_delta": busy_delta,
                "idle_ticks_delta": idle_delta,
                "busy_pct": f"{busy_pct:.4f}",
                "idle_pct": f"{idle_pct:.4f}",
                "usr_jiffies_delta": max(0, usr_delta),
                "sys_jiffies_delta": max(0, sys_delta),
                "irq_jiffies_delta": max(0, irq_delta),
                "usr_cpu_pct": f"{100.0 * max(0, usr_delta) / user_hz / interval_sec:.4f}",
                "sys_cpu_pct": f"{100.0 * max(0, sys_delta) / user_hz / interval_sec:.4f}",
                "irq_cpu_pct": f"{100.0 * max(0, irq_delta) / user_hz / interval_sec:.4f}",
            }
        )
        rows += 1

    total = aggregate_busy + aggregate_idle
    if rows and total:
        busy_pct = 100.0 * aggregate_busy / total
        writer.writerow(
            {
                "timestamp_utc": timestamp,
                "interval_sec": f"{interval_sec:.3f}",
                "spdk_pid": pid,
                "n_gc": n_gc,
                "replay_active": replay_active,
                "scope": "all",
                "lcore": "ALL",
                "tid": "",
                "thread_names": "",
                "spdk_tick_rate_hz": current.get("tick_rate", ""),
                "core_freq_mhz": "",
                "in_interrupt": "",
                "busy_ticks_delta": aggregate_busy,
                "idle_ticks_delta": aggregate_idle,
                "busy_pct": f"{busy_pct:.4f}",
                "idle_pct": f"{100.0 - busy_pct:.4f}",
                "usr_jiffies_delta": aggregate_usr,
                "sys_jiffies_delta": aggregate_sys,
                "irq_jiffies_delta": aggregate_irq,
                "usr_cpu_pct": f"{100.0 * aggregate_usr / user_hz / interval_sec:.4f}",
                "sys_cpu_pct": f"{100.0 * aggregate_sys / user_hz / interval_sec:.4f}",
                "irq_cpu_pct": f"{100.0 * aggregate_irq / user_hz / interval_sec:.4f}",
            }
        )
        rows += 1
    return rows


def main() -> int:
    args = parse_args()
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    needs_header = not output.exists() or output.stat().st_size == 0
    previous: dict | None = None
    previous_time = 0.0
    seen_spdk = False

    print(f"reactor logger waiting for SPDK RPC socket: {args.socket}", flush=True)
    print(f"CSV output: {output}", flush=True)

    with output.open("a", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
        if needs_header:
            writer.writeheader()
            stream.flush()

        while True:
            running = unit_is_running(args.watch_unit)
            if running is None:
                running = runner_is_running(args.runner_pattern)
            if not running:
                print("benchmark runner stopped; reactor logger exiting", flush=True)
                return 0

            sample_time = time.monotonic()
            try:
                current = get_reactors(args.rpc, args.socket)
            except (OSError, subprocess.SubprocessError, json.JSONDecodeError, KeyError) as error:
                if seen_spdk:
                    print(f"RPC temporarily unavailable: {error}", flush=True)
                previous = None
                time.sleep(args.interval)
                continue

            seen_spdk = True
            current_pid = int(current["pid"])
            if previous is None or int(previous["pid"]) != current_pid:
                previous = current
                previous_time = sample_time
                print(
                    f"SPDK pid={current_pid}, N_gc={read_ngc(current_pid) or 'unknown'}; "
                    "captured utilization baseline",
                    flush=True,
                )
                time.sleep(args.interval)
                continue

            elapsed = sample_time - previous_time
            rows = write_interval(writer, current, previous, elapsed)
            stream.flush()
            print(
                f"wrote {rows} rows for SPDK pid={current_pid} "
                f"(replay_active={int(replay_is_active())})",
                flush=True,
            )
            previous = current
            previous_time = sample_time
            time.sleep(args.interval)


if __name__ == "__main__":
    raise SystemExit(main())
