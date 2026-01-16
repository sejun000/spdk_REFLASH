#!/usr/bin/env python3
"""
Saturated Trace Replay - 4 threads, 64 QD per thread
Replays write operations from Alibaba trace as fast as possible.
Streaming mode: reads trace line-by-line, doesn't load entire file into memory.

Trace format (CSV):
  Column 0: timestamp (ignored in saturated mode)
  Column 1: operation (W, WS = write)
  Column 2: offset (bytes)
  Column 3: size (bytes)
"""

import os
import sys
import threading
import time
import argparse
import queue
import signal

NUM_THREADS = 4
QUEUE_DEPTH = 64
QUEUE_SIZE = 1024  # Max items per thread queue (backpressure)
REPORT_INTERVAL = 100000  # Report every 100k IOs
DEFAULT_MAX_WRITE_TB = 4  # 4TB default limit
DEFAULT_BLOCK_SIZE = 4096  # LBA mapping unit
stop_flag = threading.Event()  # Global stop flag
max_write_bytes = DEFAULT_MAX_WRITE_TB * 1024 * 1024 * 1024 * 1024

def signal_handler(sig, frame):
    print("\n[Ctrl+C] Exiting...")
    os._exit(0)

signal.signal(signal.SIGINT, signal_handler)


class LbaMapper:
    """Maps trace LBAs to sequential device LBAs (0, 1, 2, ...)."""

    def __init__(self, device_size_bytes, block_size=DEFAULT_BLOCK_SIZE):
        self.block_size = block_size
        self.max_lba = device_size_bytes // block_size
        self.lba_map = {}  # trace_lba -> device_lba
        self.next_device_lba = 0
        self.skipped_ios = 0
        self.skipped_bytes = 0

    def map(self, trace_offset, size):
        """
        Map trace offset to device offset.
        Returns (device_offset, size) or None if no more device LBAs available.
        """
        trace_lba = trace_offset // self.block_size

        if trace_lba in self.lba_map:
            # Already seen this LBA, use existing mapping
            device_lba = self.lba_map[trace_lba]
            device_offset = device_lba * self.block_size
            return (device_offset, size)
        else:
            # New LBA, assign next sequential device LBA
            if self.next_device_lba >= self.max_lba:
                # No more device LBAs available, skip this IO
                self.skipped_ios += 1
                self.skipped_bytes += size
                return None

            device_lba = self.next_device_lba
            self.lba_map[trace_lba] = device_lba
            self.next_device_lba += 1
            device_offset = device_lba * self.block_size
            return (device_offset, size)

    def get_stats(self):
        return {
            'mapped_lbas': len(self.lba_map),
            'skipped_ios': self.skipped_ios,
            'skipped_bytes': self.skipped_bytes,
        }


class ReplayStats:
    def __init__(self):
        self.lock = threading.Lock()
        self.total_ios = 0
        self.total_bytes = 0
        self.start_time = None
        self.last_report_ios = 0
        self.last_report_time = None

    def add(self, ios, bytes_written):
        with self.lock:
            if self.start_time is None:
                self.start_time = time.time()
                self.last_report_time = self.start_time
            self.total_ios += ios
            self.total_bytes += bytes_written
            # Check write limit - exit immediately
            if self.total_bytes >= max_write_bytes:
                print(f"\n[Limit] {max_write_bytes / (1024**4):.1f} TB reached, exiting...")
                self._print_final()
                os._exit(0)

    def _print_final(self):
        """Print final stats (called with lock held)."""
        if self.start_time is None:
            return
        elapsed = time.time() - self.start_time
        if elapsed < 0.001:
            elapsed = 0.001
        iops = self.total_ios / elapsed
        throughput_mb = (self.total_bytes / (1024*1024)) / elapsed
        throughput_gb = self.total_bytes / (1024**3)
        print(f"  Total IOs: {self.total_ios:,}, Data: {throughput_gb:.2f} GB, "
              f"IOPS: {iops:,.0f}, BW: {throughput_mb:.1f} MB/s")

    def report(self, force=False):
        with self.lock:
            if self.start_time is None:
                return
            now = time.time()
            elapsed = now - self.start_time
            if elapsed < 0.001:
                return

            ios_since_last = self.total_ios - self.last_report_ios
            if ios_since_last >= REPORT_INTERVAL or force:
                interval = now - self.last_report_time
                if interval > 0:
                    iops = ios_since_last / interval
                    throughput_mb = (self.total_bytes / (1024*1024)) / elapsed
                    throughput_gb = self.total_bytes / (1024**3)
                    print(f"[{elapsed:7.1f}s] IOs: {self.total_ios:>12,}  "
                          f"IOPS: {iops:>8,.0f}  "
                          f"Throughput: {throughput_mb:>7.1f} MB/s  "
                          f"Total: {throughput_gb:>7.2f} GB")
                self.last_report_ios = self.total_ios
                self.last_report_time = now

    def final_report(self):
        with self.lock:
            if self.start_time is None:
                print("No IOs completed")
                return
            elapsed = time.time() - self.start_time
            if elapsed < 0.001:
                elapsed = 0.001
            iops = self.total_ios / elapsed
            throughput_mb = (self.total_bytes / (1024*1024)) / elapsed
            throughput_gb = self.total_bytes / (1024**3)
            throughput_tb = self.total_bytes / (1024**4)

            print("\n" + "=" * 60)
            print("  Final Results (Saturated Replay)")
            print("=" * 60)
            print(f"  Total IOs:       {self.total_ios:,}")
            print(f"  Total Data:      {throughput_gb:.2f} GB ({throughput_tb:.4f} TB)")
            print(f"  Elapsed Time:    {elapsed:.2f} seconds")
            print(f"  Average IOPS:    {iops:,.0f}")
            print(f"  Average BW:      {throughput_mb:.1f} MB/s")
            print("=" * 60)


def reader_thread(filepath, work_queues, num_threads, lba_mapper):
    """Read trace file line by line and distribute to worker queues (round-robin)."""
    line_num = 0
    distributed = 0

    try:
        with open(filepath, 'r') as f:
            for line in f:
                # Check stop flag (write limit reached or Ctrl+C)
                if stop_flag.is_set():
                    print(f"[Reader] Stop signal received, stopping...")
                    break

                parts = line.strip().split(',')
                if len(parts) >= 4:
                    op = parts[1].strip()
                    if op in ('W', 'WS'):
                        try:
                            offset = int(parts[2].strip())
                            size = int(parts[3].strip())
                            if size > 0:
                                # Map trace LBA to device LBA
                                mapped = lba_mapper.map(offset, size)
                                if mapped is not None:
                                    device_offset, size = mapped
                                    # Round-robin distribution
                                    thread_id = distributed % num_threads
                                    work_queues[thread_id].put((device_offset, size))
                                    distributed += 1
                        except ValueError:
                            pass
                line_num += 1

                # Progress for large files
                if line_num % 1000000 == 0:
                    mapper_stats = lba_mapper.get_stats()
                    print(f"[Reader] Parsed {line_num:,} lines, distributed {distributed:,} writes, "
                          f"skipped {mapper_stats['skipped_ios']:,} (device full)")

    finally:
        # Signal end to all workers
        for q in work_queues:
            q.put(None)

    mapper_stats = lba_mapper.get_stats()
    print(f"[Reader] Done: {line_num:,} lines, {distributed:,} writes distributed, "
          f"{mapper_stats['skipped_ios']:,} skipped (device full)")


def worker_thread(thread_id, device_path, work_queue, stats, block_size=4096):
    """Worker thread: consume from queue and write to device."""
    try:
        fd = os.open(device_path, os.O_WRONLY | os.O_DIRECT)
    except OSError as e:
        print(f"Thread {thread_id}: Failed to open device: {e}")
        return

    # Pre-allocate aligned buffer (1MB max)
    max_buf_size = 1024 * 1024
    try:
        import mmap
        buf = mmap.mmap(-1, max_buf_size)
        buf.write(b'\x00' * max_buf_size)
    except:
        buf = bytearray(max_buf_size)

    completed_ios = 0
    completed_bytes = 0
    batch_ios = 0
    batch_bytes = 0

    while True:
        # Check stop flag
        if stop_flag.is_set():
            break

        try:
            item = work_queue.get(timeout=0.1)
        except queue.Empty:
            continue

        if item is None:  # End signal
            break

        offset, size = item

        # Align offset and size to block_size for O_DIRECT
        aligned_offset = (offset // block_size) * block_size
        aligned_size = ((size + block_size - 1) // block_size) * block_size

        # Cap at buffer size
        if aligned_size > max_buf_size:
            aligned_size = max_buf_size

        try:
            os.lseek(fd, aligned_offset, os.SEEK_SET)
            written = os.write(fd, buf[:aligned_size])
            if written > 0:
                completed_ios += 1
                completed_bytes += size
                batch_ios += 1
                batch_bytes += size
        except OSError:
            pass  # Skip errors (offset out of range, etc.)

        # Report periodically
        if batch_ios >= 1000:
            stats.add(batch_ios, batch_bytes)
            stats.report()
            batch_ios = 0
            batch_bytes = 0

    # Final batch
    if batch_ios > 0:
        stats.add(batch_ios, batch_bytes)

    os.close(fd)
    print(f"Thread {thread_id}: Completed {completed_ios:,} IOs, {completed_bytes/(1024**3):.2f} GB")


def main():
    parser = argparse.ArgumentParser(description='Saturated Trace Replay (Streaming)')
    parser.add_argument('device', help='Target device (e.g., /dev/nvme0n1)')
    parser.add_argument('--trace', default='/home/sejun000/alibaba_dwpd1.trace.head30p',
                        help='Trace file path')
    parser.add_argument('--threads', type=int, default=NUM_THREADS,
                        help=f'Number of threads (default: {NUM_THREADS})')
    parser.add_argument('--qd', type=int, default=QUEUE_DEPTH,
                        help=f'Queue depth per thread (default: {QUEUE_DEPTH})')
    parser.add_argument('--max-tb', type=float, default=DEFAULT_MAX_WRITE_TB,
                        help=f'Max write amount in TB (default: {DEFAULT_MAX_WRITE_TB})')
    args = parser.parse_args()

    # Set global max write bytes
    global max_write_bytes
    max_write_bytes = int(args.max_tb * 1024 * 1024 * 1024 * 1024)

    print("=" * 60)
    print("  Saturated Trace Replay (Streaming Mode)")
    print("=" * 60)
    print(f"  Device:     {args.device}")
    print(f"  Trace:      {args.trace}")
    print(f"  Threads:    {args.threads}")
    print(f"  QD/thread:  {args.qd}")
    print(f"  Max write:  {args.max_tb} TB")
    print("=" * 60)
    print()

    # Check device exists
    if not os.path.exists(args.device):
        print(f"ERROR: Device not found: {args.device}")
        sys.exit(1)

    # Check trace exists
    if not os.path.exists(args.trace):
        print(f"ERROR: Trace file not found: {args.trace}")
        sys.exit(1)

    # Get device size
    try:
        fd = os.open(args.device, os.O_RDONLY)
        device_size = os.lseek(fd, 0, os.SEEK_END)
        os.close(fd)
    except OSError as e:
        print(f"ERROR: Cannot get device size: {e}")
        sys.exit(1)

    print(f"  Device size: {device_size / (1024**3):.2f} GB ({device_size // DEFAULT_BLOCK_SIZE:,} LBAs)")
    print()

    # Create LBA mapper for sequential LBA remapping
    lba_mapper = LbaMapper(device_size, DEFAULT_BLOCK_SIZE)

    # Create work queues (bounded to apply backpressure)
    work_queues = [queue.Queue(maxsize=QUEUE_SIZE) for _ in range(args.threads)]

    stats = ReplayStats()

    # Start worker threads
    workers = []
    for t in range(args.threads):
        worker = threading.Thread(
            target=worker_thread,
            args=(t, args.device, work_queues[t], stats)
        )
        workers.append(worker)
        worker.start()

    # Start reader thread
    reader = threading.Thread(
        target=reader_thread,
        args=(args.trace, work_queues, args.threads, lba_mapper)
    )
    reader.start()

    print("Replay started...")
    print()

    # Wait for reader to finish
    reader.join()

    # Wait for all workers to finish
    for worker in workers:
        worker.join()

    stats.final_report()

    # Print LBA mapper stats
    mapper_stats = lba_mapper.get_stats()
    print(f"\n  LBA Mapping Stats:")
    print(f"    Unique LBAs mapped: {mapper_stats['mapped_lbas']:,}")
    print(f"    Skipped IOs:        {mapper_stats['skipped_ios']:,}")
    print(f"    Skipped bytes:      {mapper_stats['skipped_bytes'] / (1024**3):.2f} GB")


if __name__ == '__main__':
    main()
