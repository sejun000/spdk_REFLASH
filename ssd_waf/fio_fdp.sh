#!/bin/bash

# FDP Test Script
# Usage: FDP_PLI=0 ./fio_fdp.sh   (placement handle 0 - expected full BW)
#        FDP_PLI=1 ./fio_fdp.sh   (placement handle 1 - expected lower BW)

TARGET_SN="S77UNG0TC00116"
FDP_PLI=${FDP_PLI:-0}  # Default to placement handle 0

# Find device by serial number
DEVICE=$(nvme list -o json 2>/dev/null | jq -r ".Devices[] | select(.SerialNumber==\"$TARGET_SN\") | .DevicePath" | head -1)

if [ -z "$DEVICE" ] || [ "$DEVICE" == "null" ]; then
    echo "Error: Device with SN=$TARGET_SN not found"
    echo "Available devices:"
    nvme list
    exit 1
fi

echo "============================================"
echo "FDP Write Test"
echo "Target SN: $TARGET_SN"
echo "Device: $DEVICE"
echo "Placement Handle Index: $FDP_PLI"
echo "============================================"

# Get character device (ng*) for io_uring_cmd
CHAR_DEV=$(echo $DEVICE | sed 's/nvme\([0-9]*\)n\([0-9]*\)/ng\1n\2/')
if [ ! -e "$CHAR_DEV" ]; then
    # Fallback: try /dev/ngXnY format
    CHAR_DEV="/dev/ng${DEVICE#/dev/nvme}"
    CHAR_DEV=$(echo $CHAR_DEV | sed 's/nvme//')
    CHAR_DEV="/dev/ng$(echo $DEVICE | grep -o '[0-9]*n[0-9]*')"
fi

echo "Character device: $CHAR_DEV"

# Run fio with FDP using io_uring_cmd (NVMe passthrough)
fio --name=fdp_test \
    --filename=$CHAR_DEV \
    --ioengine=io_uring_cmd \
    --cmd_type=nvme \
    --rw=write \
    --bs=128k \
    --iodepth=64 \
    --numjobs=4 \
    --fdp=1 \
    --fdp_pli=$FDP_PLI \
    --size=10G \
    --runtime=30 \
    --time_based \
    --group_reporting \
    --output-format=normal

echo ""
echo "Test completed with FDP PLI=$FDP_PLI"
