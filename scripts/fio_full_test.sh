#!/bin/bash
# ZNS device fio full characterization
# Run as: sudo bash scripts/fio_full_test.sh
set -euo pipefail

DEV="/dev/nvme3n2"
OUTDIR="logs/fio_detail"
RUNTIME=10

mkdir -p "$OUTDIR"

reset_dev() {
    nvme zns reset-zone -a "$DEV" 2>/dev/null || true
    sleep 1
}

echo "=========================================="
echo " ZNS fio Full Characterization"
echo " Device: $DEV"
echo " Output: $OUTDIR/"
echo "=========================================="

# ── Test 1: Block size sweep (single job) ──
echo ""
echo "[Test 1/4] Block size sweep (1 job, varying bs)"
for bs in 4k 16k 64k 128k; do
    echo "  bs=$bs ..."
    reset_dev
    fio --name=test --filename="$DEV" \
        --ioengine=psync --direct=1 --rw=write --bs="$bs" \
        --numjobs=1 --zonemode=zbd \
        --runtime=$RUNTIME --time_based \
        --output="$OUTDIR/blocksize_${bs}.log"
done
echo "  Done."

# ── Test 2: Read-only bandwidth ──
echo ""
echo "[Test 2/4] Read-only bandwidth (fill then read)"
reset_dev
echo "  Filling 2GB ..."
fio --name=fill --filename="$DEV" \
    --ioengine=psync --direct=1 --rw=write --bs=128k \
    --numjobs=1 --zonemode=zbd --size=2g \
    --output="$OUTDIR/read_fill.log"

for jobs in 1 2 4 8 16; do
    echo "  read numjobs=$jobs ..."
    fio --name=test --filename="$DEV" \
        --ioengine=psync --direct=1 --rw=read --bs=4k \
        --numjobs=$jobs --size=2g \
        --runtime=$RUNTIME --time_based \
        --output="$OUTDIR/read_jobs${jobs}.log"
done
echo "  Done."

# ── Test 3: Write scaling (numjobs sweep, 4KB) ──
echo ""
echo "[Test 3/4] Write scaling (4KB, numjobs sweep)"
for jobs in 1 2 4 8 16 32; do
    echo "  write numjobs=$jobs ..."
    reset_dev
    fio --name=test --filename="$DEV" \
        --ioengine=psync --direct=1 --rw=write --bs=4k \
        --numjobs=$jobs --zonemode=zbd \
        --runtime=$RUNTIME --time_based \
        --group_reporting=0 \
        --output="$OUTDIR/write_jobs${jobs}.log"
done
echo "  Done."

# ── Test 4: Per-job latency distribution (8 jobs) ──
echo ""
echo "[Test 4/4] Per-job latency distribution (8 jobs, 4KB write)"
reset_dev
fio --name=test --filename="$DEV" \
    --ioengine=psync --direct=1 --rw=write --bs=4k \
    --numjobs=8 --zonemode=zbd \
    --runtime=$RUNTIME --time_based \
    --lat_percentiles=1 --group_reporting=0 \
    --output="$OUTDIR/latency_8jobs.log"
echo "  Done."

echo ""
echo "=========================================="
echo " All tests complete. Results in: $OUTDIR/"
echo "=========================================="
ls -la "$OUTDIR/"
