# Benchmarks pending after the high 200 TB run

The previous queue was intentionally cancelled on 2026-08-11 to prioritize
the copied high-DWPD trace. Run these after the high 200 TB experiments:

- OCF (config 3)
  - Trace 2: `alibaba_dwpd01to1.trace` (the 2026-08-11 replay was interrupted)
  - Trace 5: `ssdtrace_scaled_4x.trace`
- REFlash R864 (config 6)
  - Trace 3: `alibaba_dwpd1to2_4x.trace`
  - Trace 5: `ssdtrace_scaled_4x.trace`
- REFlash R288 (config 7)
  - Trace 2: `alibaba_dwpd01to1.trace`
  - Trace 3: `alibaba_dwpd1to2_4x.trace`
  - Trace 5: `ssdtrace_scaled_4x.trace`
  - Trace 7: `fio_zipf_1.0`

Suggested resume commands:

```bash
TRACE_NUMS="2 5" ./run_benchmark.sh 3
TRACE_NUMS="3 5" ./run_benchmark.sh 6
TRACE_NUMS="2 3 5 7" ./run_benchmark.sh 7
```

## CSAL rerun correctness requirements (2026-08-16)

Do not treat the existing raw-device prefill in `run_ftl.sh` as an FTL
steady-state prefill.  It conditions the physical SSDs, but `ftl0` is created
afterward in fresh-create mode, so its L2P and valid-band state still start
empty.  This delays host-level FTL GC and makes the old CSAL result unsuitable
as the final steady-state comparison.

Before the next CSAL measurement:

1. Raw-prefill **only the buffer/cache SSD**.  Do not raw-prefill the backend
   SSD for CSAL.
2. Create and expose `ftl0`.
3. Sequentially prefill the complete exposed **FTL bdev**.  This FTL-level
   prefill must be the operation that populates the backend SSD.
4. After the full sequential FTL-bdev prefill completes, wait a fixed
   **10 minutes** before starting the measured trace.  This settling interval
   allows cache-to-backend compaction to progress before measurement.
5. Reset or record fresh logger/vendor-counter baselines after prefill so the
   measured CSV excludes prefill writes.
6. Use flat L2P (`SPDK_FTL_L2P_FLAT`) so measured CSAL runs have no L2P
   page-in/page-out traffic.  Clean-rebuild and verify that `ftl_l2p.o`
   references `ftl_l2p_flat_*` before launching CSAL.
7. Log backend compaction data writes, host-level GC relocation writes, and
   backend metadata writes separately; preserve the combined backend total as
   well.
8. Keep the intended 7% FTL overprovisioning and apply the planned DRAM limit
   consistently.  Confirm the full flat-map allocation fits before the long
   run.

Keep the existing CSAL result as a historical result that raw-prefilled both
devices, but rerun it with buffer-only raw prefill followed by FTL-bdev prefill
for the paper comparison.
