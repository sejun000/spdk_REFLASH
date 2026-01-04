# Zone Striping Implementation

## Date: 2025-12-29

## Overview
Implemented N-zone striping for LogCache to achieve full ZNS bandwidth by writing to multiple zones in parallel.

## Problem
- Single zone write doesn't utilize full SSD bandwidth
- ZNS SSDs have internal parallelism across channels/dies
- Writing to one zone only uses subset of internal resources

## Solution: Zone Striping
1 segment = STRIPE_WIDTH zones (default: 2)

### Write Pattern (128KB chunk-based striping)
```
idx 0-31   (128KB chunk 0) → Zone 0, offset 0-128KB
idx 32-63  (128KB chunk 1) → Zone 1, offset 0-128KB
idx 64-95  (128KB chunk 2) → Zone 0, offset 128KB-256KB
idx 96-127 (128KB chunk 3) → Zone 1, offset 128KB-256KB
...
```

## Files Modified

### 1. log_cache_segment.h
- Added `STRIPE_WIDTH` define (number of zones per segment)
- Added `STRIPE_CHUNK_BLOCKS` define (32 blocks = 128KB)
- Added `physical_bases[]` vector (N zone base addresses)
- Added `get_block_offset()` - calculates striped offset
- Added `get_zone_idx()` - returns zone index for block

### 2. log_cache.h
- Added `zone_capacity_bytes` to Config
- Added `stripe_width` to Config
- Added `dst_idx` to GcBlockInfo struct

### 3. log_cache.cpp
- Modified segment initialization to create striped segments
- `segment_size_blocks = zone_capacity * stripe_width`
- `total_segments = total_zones / stripe_width`
- Each segment gets N physical_bases
- `block_offset()` uses striped calculation
- Reset operations reset all N zones in segment
- **2-tier GC threshold**: GC trigger at <=10, block at <=5

### 4. log_cache_wrapper.cpp
- Updated Config with `zone_capacity_bytes` and `stripe_width`
- Updated `block_offset()` to use striped method
- **Parallel evict**: 256 chunks processed in parallel
- **SPDK logging**: printf -> SPDK_NOTICELOG/WARNLOG/ERRLOG
- **Zone reset timing**: logs elapsed time per zone reset
- **Blocking stats**: logs total blocked time when unblocked
- **Skip flush during GC**: flush_write_buffer skips if gc/evict in progress

### 5. vbdev_icache.c
- Added cache size limit (100GB hardcoded, TODO: RPC param)
- Aligned cache size to zone_size * STRIPE_WIDTH boundary

## Key Constants
```cpp
#define STRIPE_WIDTH 2              // Number of zones per segment
#define STRIPE_CHUNK_BLOCKS 32      // 128KB chunks (32 * 4KB)
PARALLEL_CHUNKS = 256               // Evict 256 chunks in parallel (1MB)
ASYNC_GC_TRIGGER_SEGMENTS = 10      // Start GC at <= 10 free segments
ASYNC_RESERVE_SEGMENTS = 5          // Block host IO at <= 5 free segments
cache_size_limit_gb = 100           // Cache size limit
```

## GC/Evict Improvements

### Parallel Evict (256 chunks)
- Before: 1 chunk at a time (52 seconds for 2.1GB)
- After: 256 chunks in parallel (~7-11 seconds)
- Uses atomic counters for completion tracking

### 2-Tier GC Threshold
- GC_TRIGGER (<=10): Start GC, host IO continues
- BLOCK (<=5): Block host IO until free segments available
- Gives GC time to complete before blocking

### Zone Reset Timing
```
Zone reset complete: 2 zones, 20445 us (10.22 ms/zone)
zone_reset_next: zone_idx=4 (block=2097152), end_zone_idx=5
```

### Blocking Stats
```
BLOCKED: flush_write_buffer waiting for GC (blocked 339 times)
UNBLOCKED: flush_write_buffer succeeded after 339 blocks, 8498820 us blocked
```

## Performance Results
- Zone reset: ~10ms per zone
- Evict 2.1GB: 52s -> 7-11s (with 256 parallel chunks)
- Blocking time significantly reduced with 2-tier threshold

## Debug Logging (SPDK API)
- SPDK_NOTICELOG: init params, GC trigger, zone reset, evict progress
- SPDK_WARNLOG: GC blocked, host write blocked
- SPDK_ERRLOG: errors

## TODO
- Make STRIPE_WIDTH configurable via RPC
- Make cache_size_limit configurable via RPC
- Consider parallel GC for multiple segments
- Coalesce consecutive LBAs for larger backend writes
- Tune PARALLEL_CHUNKS and thresholds

## Testing
- STRIPE_WIDTH=2 with 100GB cache limit
- 256 parallel evict chunks
- GC triggers at <=10, blocks at <=5 segments
