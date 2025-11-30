#!/bin/bash

sudo python3 ./blk_trace_analysis.py --trace_format csv --rw_policy write-only --cache_policy LRU /mnt/nvme2n1/alibaba_block_traces_2020/modified.trace > result_0316_LRU
