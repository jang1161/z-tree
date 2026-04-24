#!/bin/bash
# bench_both.sh — ZTree + CTree benchmark (same session, fair comparison)
#
# Usage: sudo bash scripts/bench_both.sh /dev/nvme3n2
#
# ── Configure below ──────────────────────────────────────
KEYS=10000000
RUNS=1
THREADS=(8 16 32 64)
OUTDIR=logs/compare/10M
ZTREE_BIN=build/ztree
CTREE_BIN=build/ctree
# ─────────────────────────────────────────────────────────

set -e
DEV=${1:?Usage: $0 <device>}
mkdir -p "$OUTDIR"
TS=$(date +%Y%m%d_%H%M%S)

echo "=== ZTree vs CTree (${KEYS} keys, ${RUNS} runs, threads=${THREADS[*]}) ==="
echo "Device: $DEV  Output: $OUTDIR"
echo ""

for bin_label in ztree ctree; do
    if [ "$bin_label" = "ztree" ]; then BIN=$ZTREE_BIN; else BIN=$CTREE_BIN; fi
    for t in "${THREADS[@]}"; do
        for r in $(seq 1 $RUNS); do
            log="${OUTDIR}/${bin_label}_${t}T_run${r}_${TS}.log"
            echo "[${bin_label}] ${t}T run ${r}/${RUNS} ..."
            ./${BIN} ${KEYS} ${t} ${DEV} > "$log" 2>&1
            tput=$(grep 'Average throughput' "$log" | awk '{print $3}')
            echo "  -> ${tput} ops/sec"
        done
    done
done

find "$OUTDIR" -name "*.log" -exec sed -i '/Progress:/d' {} +

echo ""
echo "Done. Logs in ${OUTDIR}/"
