#!/bin/bash
# ZNS 4KB write scaling — each job pinned to its own zone via offset_increment
# Run as: sudo bash scripts/fio_write_scaling_isolated.sh
set -euo pipefail

DEV="/dev/nvme3n2"
OUTDIR="logs/fio_detail"
RUNTIME=10

mkdir -p "$OUTDIR"

echo "=== 4KB Write Scaling (zone-isolated, psync, iodepth=1) ==="
for jobs in 1 2 4 8 16; do
    echo "  numjobs=$jobs ..."
    nvme zns reset-zone -a "$DEV" 2>/dev/null || true
    sleep 1
    fio --name=test --filename="$DEV" \
        --ioengine=psync --direct=1 --rw=write --bs=16k \
        --numjobs=$jobs --zonemode=zbd \
        --offset_increment=2g \
        --runtime=$RUNTIME --time_based \
        --group_reporting=0 \
        --output="$OUTDIR/write_isolated_jobs${jobs}.log"
done

echo ""
echo "=== Results ==="
for jobs in 1 2 4 8 16; do
    echo "--- jobs=$jobs ---"
    grep "WRITE:" "$OUTDIR/write_isolated_jobs${jobs}.log" | tail -1
done
