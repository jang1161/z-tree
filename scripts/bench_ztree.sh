#!/bin/bash
# bench_ztree.sh — ZTree benchmark
#
# Usage: sudo bash scripts/bench_ztree.sh /dev/nvme3n2
#
# ── Configure below ──────────────────────────────────────
KEYS=10000000
RUNS=3
THREADS=(8 16 32 64)
OUTDIR=logs/ztree/10M_3runs
BIN=build/ztree
# ─────────────────────────────────────────────────────────

set -e
DEV=${1:?Usage: $0 <device>}
mkdir -p "$OUTDIR"
TS=$(date +%Y%m%d_%H%M%S)

echo "=== ZTree (${KEYS} keys, ${RUNS} runs, threads=${THREADS[*]}) ==="
echo "Device: $DEV  Output: $OUTDIR"
echo ""

for t in "${THREADS[@]}"; do
    for r in $(seq 1 $RUNS); do
        log="${OUTDIR}/ztree_${t}T_run${r}_${TS}.log"
        echo "[ztree] ${t}T run ${r}/${RUNS} ..."
        ./${BIN} ${KEYS} ${t} ${DEV} > "$log" 2>&1
        tput=$(grep 'Average throughput' "$log" | awk '{print $3}')
        echo "  -> ${tput} ops/sec"
    done
done

find "$OUTDIR" -name "*.log" -newer "$OUTDIR/ztree_${THREADS[0]}T_run1_${TS}.log" -exec sed -i '/Progress:/d' {} + 2>/dev/null
sed -i '/Progress:/d' "$OUTDIR/ztree_${THREADS[0]}T_run1_${TS}.log" 2>/dev/null

echo ""
echo "Done. Logs in ${OUTDIR}/"
