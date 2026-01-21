#pragma once

//==============================================================================
// FDP (Flexible Data Placement) Configuration
//==============================================================================
// Set FDP to 1 to enable FDP mode:
// - Skips ZNS zone open commands (open_zone_zrwa)
// - Skips ZRWA WP flush commands (flush_zrwa)
// - Skips zone reset commands
// The rest of the code (read/write/evict) operates normally.
//
// Set FDP to 0 (or comment out) for ZNS mode.
//==============================================================================

#define FDP 1

// Number of FDP placement handles available (0 ~ FDP_NUM_PLACEMENT_HANDLES-1)
// stream_id % FDP_NUM_PLACEMENT_HANDLES = placement handle
#define FDP_NUM_PLACEMENT_HANDLES 7

// FDP_TRIM: When enabled, sends TRIM/UNMAP command on segment reset in FDP mode
// Set to 1 to enable TRIM on segment reset, 0 to skip
#define FDP_TRIM 1

// FDP_PLACEMENT_ENABLED: When enabled, uses actual placement handles for data separation
// Set to 1 to enable placement handles (default), 0 to force all writes to placement_handle=0
#define FDP_PLACEMENT_ENABLED 1

//==============================================================================
// End of FDP Configuration
//==============================================================================
