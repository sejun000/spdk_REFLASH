import sys
from collections import defaultdict

TRACES = [
    "/home/sejun000/alibaba_dwpd01to1.trace",
    "/home/sejun000/alibaba_dwpd1to2_4x.trace",
    "/home/sejun000/alibaba_dwpd2_5x.trace",
    "/home/sejun000/ssdtrace_scaled_4x.trace",
]

LIMIT_BYTES = 8 * 1024**4  # 8 TB
PAGE_SIZE = 4096

for trace_path in TRACES:
    print(f"\n{'='*60}")
    print(f"Trace: {trace_path.split('/')[-1]}")
    print(f"{'='*60}")

    write_count_per_lba = defaultdict(int)  # lba_page -> write_count
    total_write_bytes = 0
    total_write_ios = 0

    with open(trace_path, 'r') as f:
        for line in f:
            parts = line.strip().split(',')
            if len(parts) < 4:
                continue
            op = parts[1].strip()
            if op != 'W':
                continue
            offset = int(parts[2].strip())
            size = int(parts[3].strip())

            # Track each 4KB page touched
            start_page = offset // PAGE_SIZE
            num_pages = (size + PAGE_SIZE - 1) // PAGE_SIZE
            for p in range(num_pages):
                write_count_per_lba[start_page + p] += 1

            total_write_bytes += size
            total_write_ios += 1

            if total_write_bytes >= LIMIT_BYTES:
                break

    total_write_tb = total_write_bytes / (1024**4)
    wss_pages = len(write_count_per_lba)
    wss_gb = wss_pages * PAGE_SIZE / (1024**3)

    # Sort LBAs by write count descending
    sorted_counts = sorted(write_count_per_lba.values(), reverse=True)
    total_page_writes = sum(sorted_counts)

    # Hot 20% of unique LBAs
    hot_count = max(1, int(wss_pages * 0.20))
    hot_writes = sum(sorted_counts[:hot_count])
    hot_ratio = hot_writes / total_page_writes * 100.0

    print(f"  Total write volume : {total_write_tb:.2f} TB  ({total_write_ios:,} IOs)")
    print(f"  WSS (unique pages) : {wss_pages:,} pages = {wss_gb:.2f} GB")
    print(f"  Total page writes  : {total_page_writes:,}")
    print(f"  Hot 20% LBAs       : {hot_count:,} pages")
    print(f"  Hot 20% writes     : {hot_writes:,} / {total_page_writes:,} = {hot_ratio:.1f}%")

print()
