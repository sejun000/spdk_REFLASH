#!/bin/bash

NVME_VIRT_DIR=/home/sejun000/nvmevirt
TRACE_DIR=/mnt/nvme2n1
#RATIOS=(0.05618 0.075 0.085 0.095 0.105)
RATIOS=(0.105)
./get_waf.sh &
GET_WAF_PID=$!

for RATIO in "${RATIOS[@]}"
do
    cd $NVME_VIRT_DIR/tests
    sudo ./rmmod_array.sh
    sudo ./insmod_array.sh
    cd -
    echo "[Trace Replay Start - RATIO: $RATIO] $(date '+%Y-%m-%d %H:%M:%S')"
    sudo ./trace_replayer $TRACE_DIR/LOG_FIFO_${RATIO}.trace /dev/nvme3n1 csv
    echo "[Trace Replay End - RATIO: $RATIO] $(date '+%Y-%m-%d %H:%M:%S')"
done

kill $GET_WAF_PID
wait $GET_WAF_PID 2>/dev/null
