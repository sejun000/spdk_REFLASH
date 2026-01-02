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

//==============================================================================
// End of FDP Configuration
//==============================================================================
