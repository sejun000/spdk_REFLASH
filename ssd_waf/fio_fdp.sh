#!/bin/bash

# FDP Sequential Write Test - 2 threads, split range, FDP PLI 0/1
# Device: nvme3n1 (SN: 0123456789ABCDEF0000)

TARGET_SN="0123456789ABCDEF0000"

# Find device by serial number
DEV=""
for d in /sys/block/nvme*; do
    sn=$(cat "$d/device/serial" 2>/dev/null | tr -d ' ')
    if [ "$sn" == "$TARGET_SN" ]; then
        DEV="/dev/$(basename $d)"
        break
    fi
done

if [ -z "$DEV" ]; then
    echo "Error: Device with SN=$TARGET_SN not found"
    lsblk -o NAME,SERIAL | grep nvme
    exit 1
fi

# Get character device for io_uring_cmd (ng device)
CHAR_DEV=$(echo "$DEV" | sed 's|/dev/nvme|/dev/ng|')

if [ ! -e "$CHAR_DEV" ]; then
    echo "Error: Character device $CHAR_DEV not found"
    exit 1
fi

TOTAL_SIZE=$(lsblk -b -o SIZE -n "$DEV" | tr -d ' ')
HALF_SIZE=$((TOTAL_SIZE / 2))

echo "============================================"
echo "FDP Sequential Write Test"
echo "  SN:          $TARGET_SN"
echo "  Block dev:   $DEV"
echo "  Char dev:    $CHAR_DEV"
echo "  Total size:  $((TOTAL_SIZE / 1024 / 1024 / 1024)) GiB"
echo "  Half size:   $((HALF_SIZE / 1024 / 1024 / 1024)) GiB"
echo "  Job 0:       offset=0, size=$((HALF_SIZE / 1024 / 1024 / 1024)) GiB, fdp_pli=0"
echo "  Job 1:       offset=$((HALF_SIZE / 1024 / 1024 / 1024)) GiB, size=$((HALF_SIZE / 1024 / 1024 / 1024)) GiB, fdp_pli=1"
echo "============================================"

fio \
    --name=fdp_seq_pli0 \
    --filename="$CHAR_DEV" \
    --ioengine=io_uring_cmd \
    --cmd_type=nvme \
    --rw=write \
    --bs=128k \
    --iodepth=32 \
    --offset=0 \
    --size="$HALF_SIZE" \
    --fdp=1 \
    --fdp_pli=0 \
    --numjobs=1 \
    --name=fdp_seq_pli1 \
    --filename="$CHAR_DEV" \
    --ioengine=io_uring_cmd \
    --cmd_type=nvme \
    --rw=write \
    --bs=128k \
    --iodepth=32 \
    --offset="$HALF_SIZE" \
    --size="$HALF_SIZE" \
    --fdp=1 \
    --fdp_pli=1 \
    --numjobs=1 \
    --group_reporting=0

echo ""
echo "Done."
