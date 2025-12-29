#!/bin/bash

# icache 디바이스를 찾아 fio 실행
# ublk (/dev/ublkb0) 또는 NVMe-oF (ICACHE) 사용

UBLK_DEV_ID=${UBLK_DEV_ID:-0}
UBLK_DEVICE="/dev/ublkb${UBLK_DEV_ID}"

# ublk 디바이스 우선 확인
if [ -e "$UBLK_DEVICE" ]; then
    DEVICE="$UBLK_DEVICE"
    echo "ublk 디바이스 발견: $DEVICE"
else
    # Fallback: NVMe-oF 디바이스 검색 (ICACHE 또는 NULL)
    DEVICE=$(sudo nvme list 2>/dev/null | grep -iE "ICACHE|NULL" | awk '{print $1}')
    if [ -z "$DEVICE" ]; then
        echo "디바이스를 찾을 수 없습니다. (ublk: $UBLK_DEVICE, NVMe: ICACHE/NULL)"
        exit 1
    fi
    echo "NVMe-oF 디바이스 발견: $DEVICE"
fi

echo "fio random 4k write 200GB 시작..."

sudo fio --name=random_test \
    --filename="$DEVICE" \
    --ioengine=libaio \
    --direct=1 \
    --bs=4k \
    --rw=randwrite \
    --io_size=200g \
    --numjobs=8 \
    --iodepth=128 
