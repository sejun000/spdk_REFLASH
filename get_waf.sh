#!/bin/bash
LOGFILE="nvme_counts_0506.log"

while true; do
    echo "========== $(date) ==========" >> "$LOGFILE"
    echo "nand_write_count: $(cat /proc/nvmev0/nand_write_count)" >> "$LOGFILE"
    echo "write_count: $(cat /proc/nvmev0/write_count)" >> "$LOGFILE"
    echo "" >> "$LOGFILE"
    sleep 10
done
