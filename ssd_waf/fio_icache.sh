#!/bin/bash

# ICACHE001 모델/시리얼 번호를 가진 NVMe 디바이스를 찾아 fio 실행

DEVICE=$(sudo nvme list | grep -i "ICACHE" | awk '{print $1}')

if [ -z "$DEVICE" ]; then
    echo "ICACHE001 디바이스를 찾을 수 없습니다."
    exit 1
fi

echo "ICACHE 디바이스 발견: $DEVICE"
echo "fio random 4k write 200GB 시작..."

sudo fio --name=random_test \
    --filename="$DEVICE" \
    --ioengine=libaio \
    --direct=1 \
    --bs=4k \
    --rw=randwrite \
    --io_size=10g \
    --numjobs=1 \
    --iodepth=64 \
    --verify=md5
