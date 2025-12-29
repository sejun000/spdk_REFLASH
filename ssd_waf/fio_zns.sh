#!/bin/bash

# ZNS 디바이스에 직접 fio 실행 (모델: WZS4C8T4TDSP303)

MODEL="WZS4C8T4TDSP303"

# 모델명으로 디바이스 찾기
DEVICE=$(sudo nvme list 2>/dev/null | grep "$MODEL" | awk '{print $1}')

if [ -z "$DEVICE" ]; then
    echo "ZNS 디바이스를 찾을 수 없습니다. (모델: $MODEL)"
    exit 1
fi

echo "ZNS 디바이스 발견: $DEVICE (모델: $MODEL)"

# Zone 크기 가져오기 (bytes)
ZONE_SIZE_SECTORS=$(sudo blkzone report "$DEVICE" | head -1 | awk '{print $4}' | tr -d ',')
ZONE_SIZE_BYTES=$((ZONE_SIZE_SECTORS * 512))
echo "Zone 크기: $((ZONE_SIZE_BYTES / 1024 / 1024))MB"

# Zone reset first
echo "Zone reset all..."
sudo nvme format "$DEVICE" -s 2 --force

echo "fio zone 0 write 시작..."

sudo fio --name=zns_test \
    --filename="$DEVICE" \
    --ioengine=libaio \
    --direct=1 \
    --bs=4k \
    --rw=randwrite \
    --offset=0 \
    --numjobs=2 \
    --iodepth=64 \
    --zonemode=zbd 
    #--max_open_zones=14
