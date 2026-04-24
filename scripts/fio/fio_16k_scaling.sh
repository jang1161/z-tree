#!/bin/bash
# 16KB write scaling test
# Run as: sudo bash scripts/fio_16k_scaling.sh
set -euo pipefail

DEV="/dev/nvme3n2"
OUTDIR="logs/fio_detail"
RUNTIME=10

mkdir -p "$OUTDIR"

echo "=== 16KB Write Scaling ==="
for jobs in 1 2 4 8 16 32; do
    echo "  numjobs=$jobs ..."
    nvme zns reset-zone -a "$DEV" 2>/dev/null || true
    sleep 1
    fio --name=test --filename="$DEV" \
        --ioengine=psync --direct=1 --rw=write --bs=16k \
        --numjobs=$jobs --zonemode=zbd \
        --runtime=$RUNTIME --time_based \
        --group_reporting=0 \
        --output="$OUTDIR/write_16k_jobs${jobs}.log"
done

echo ""
echo "=== Results ==="
for jobs in 1 2 4 8 16 32; do
    echo "--- jobs=$jobs ---"
    grep "WRITE:" "$OUTDIR/write_16k_jobs${jobs}.log" | tail -1
done
