# GC/Evict Critical Bug Fixes (2026-01-05)

## Summary
Fixed multiple critical bugs in GC and Evict paths that caused data corruption (verify failures) and I/O stuck issues.

## Bugs Fixed

### 1. GC dst_seg Mapping Bug (Critical - Data Corruption)

**Root Cause:**
In `prepare_gc()`, when iterating through victim segment blocks, if the target segment becomes full, a new segment is allocated. However, `GcPrepareResult::target_seg` only stores the LAST target segment, not per-block.

```cpp
// Before: result.target_seg changes mid-loop but blocks_to_copy has old dst_idx values
if (result.target_seg->full()) {
    result.target_seg = get_segment_to_active_stream(true, ...);  // New segment
}
// info.dst_idx was from OLD segment, but finalize uses NEW segment
```

In `finalize_gc_async()`:
```cpp
mapping[info.key] = {target, dst_idx};  // target = LAST segment only!
```

**Result:** Blocks written to SegmentA get mapped to SegmentB → Read returns wrong data → Verify failure

**Fix:**
1. Added `dst_seg` field to `GcBlockInfo` struct
2. `prepare_gc()` sets `info.dst_seg = result.target_seg` for each block
3. `finalize_gc()` and `finalize_gc_async()` use `info.dst_seg` instead of `result.target_seg`

### 2. GC Sequential Mode Use-After-Free Bug

**Root Cause:**
In `gc_start_seq_batch()`, after submitting async reads:
```cpp
if (io->seq_writes_done.load() >= io->seq_total_writes) {  // 0 >= 0 when no valid blocks
    io->seq_current_chunk += io->seq_parallel_count;
    gc_start_seq_batch(io);  // Moves to next batch, frees staging buffer!
}
```

But reads are still in flight! When read callbacks fire, they access freed staging buffer.

**Fix:**
Added check for `seq_reads_done >= seq_parallel_count` before moving to next batch.

### 3. Evict Same Use-After-Free Bug

**Root Cause:**
Same pattern in `evict_start_chunk()`:
```cpp
if (io->coalesced_writes_total == 0 || io->parallel_writes_done.load() >= io->coalesced_writes_total) {
    // Moved to next batch without checking if reads done
}
```

**Fix:**
Added check for `parallel_reads_done >= parallel_batch_count`.

### 4. GC/Evict Completion Path Missing

**Root Cause:**
When all reads complete but `pending_gc_writes` / `pending_evict_writes` is empty (no valid blocks):
- `gc_dispatch_writes()` / `evict_dispatch_writes()` does nothing
- No next batch is triggered
- GC/Evict stuck

**Fix:**
Added explicit check in read completion callbacks:
```cpp
if (io->seq_total_writes == 0 || io->pending_gc_writes.empty()) {
    // Skip directly to next batch
    io->seq_current_chunk += io->seq_parallel_count;
    gc_start_seq_batch(io);
    return;
}
```

## Files Modified

### log_cache.h
- Added `LogCacheSegment *dst_seg` to `GcBlockInfo` struct

### log_cache.cpp
- `prepare_gc()`: Set `info.dst_seg = result.target_seg` for each block
- `finalize_gc()`: Use `info.dst_seg` instead of `result.target_seg`
- `finalize_gc_async()`: Same fix + added validation logging
- `get_cache_location()`: Added validation for mapping consistency

### log_cache_wrapper.cpp
- `gc_start_seq_batch()`: Added `seq_reads_done` check before moving to next batch
- `gc_seq_read_done()`: Added check for empty pending_gc_writes to skip to next batch
- `evict_start_chunk()`: Added `parallel_reads_done` check
- `coalesced_read_done()`: Added check for empty pending_evict_writes
- Added debug logging throughout

## Testing
- fio verify with crc32c: PASSED
- Performance improved (no more stuck I/O)

## Related Issues
- NVMe-oF keep_alive timeout was caused by GC monopolizing I/O (separate issue, not fixed here)
