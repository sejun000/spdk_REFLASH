#!/bin/bash

# Kill all benchmark-related processes

echo "Killing benchmark processes..."

# Kill replay_trace
sudo pkill -9 -f replay_trace 2>/dev/null && echo "  killed replay_trace" || echo "  no replay_trace found"

# Kill fio
sudo pkill -9 -f fio 2>/dev/null && echo "  killed fio" || echo "  no fio found"

# Kill run_*.sh scripts
sudo pkill -9 -f 'run_.*\.sh' 2>/dev/null && echo "  killed run_*.sh" || echo "  no run_*.sh found"

# Kill run_benchmark.sh itself (other instances)
sudo pkill -9 -f 'run_benchmark\.sh' 2>/dev/null && echo "  killed run_benchmark.sh" || echo "  no run_benchmark.sh found"

echo "Done."
