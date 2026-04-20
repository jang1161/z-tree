#!/bin/bash
# ZNS iodepth scaling test (mq-deadline scheduler)
# Run as: sudo bash scripts/fio_iodepth_test.sh
set -euo pipefail

DEV="/dev/nvme3n2"
RUNTIME=5

echo "=== ZNS iodepth test (scheduler: $(cat /sys/block/nvme3n2/queue/scheduler)) ==="

for depth in 1 2 4 8 16; do
    echo ""
    echo "--- iodepth=$depth ---"
    nvme zns reset-zone -a "$DEV" 2>/dev/null || true
    sleep 1

    if [ "$depth" -eq 1 ]; then
        ENGINE="psync"
    else
        ENGINE="libaio"
    fi

    fio --name=test --filename="$DEV" --ioengine="$ENGINE" --direct=1 --rw=write --bs=4k --numjobs=1 --zonemode=zbd --iodepth="$depth" --runtime=$RUNTIME --time_based 2>&1 | grep -E "WRITE:|error|err="
done

echo ""
echo "=== Done ==="
