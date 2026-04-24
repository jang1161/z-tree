#!/bin/bash
# ZNS iodepth × numjobs scaling test
# Run as: sudo bash scripts/fio_iodepth_jobs_test.sh
set -euo pipefail

DEV="/dev/nvme3n2"
RUNTIME=5

echo "=== ZNS iodepth × numjobs test (scheduler: $(cat /sys/block/nvme3n2/queue/scheduler)) ==="

for jobs in 1 2 4 8; do
    for depth in 1 2 4 8; do
        echo ""
        echo "--- numjobs=$jobs  iodepth=$depth  (total_qd=$((jobs * depth))) ---"
        nvme zns reset-zone -a "$DEV" 2>/dev/null || true
        sleep 1

        if [ "$depth" -eq 1 ]; then
            ENGINE="psync"
        else
            ENGINE="libaio"
        fi

        fio --name=test --filename="$DEV" --ioengine="$ENGINE" --direct=1 --rw=write --bs=4k --numjobs="$jobs" --zonemode=zbd --iodepth="$depth" --runtime=$RUNTIME --time_based --group_reporting 2>&1 | grep -E "WRITE:|error|err="
    done
done

echo ""
echo "=== Done ==="
