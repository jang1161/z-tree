#!/usr/bin/env bash
# Run the full YCSB sweep: 8 workloads × 3 source modes
#   ztree, ilayer-buffered, ilayer-O_DIRECT.
#
# Usage: scripts/run_ycsb_all.sh [recordcount] [opcount] [threads]
#   defaults: 10000000 / 10000000 / 64
#
# Existing logs under logs/ycsb/{ztree,ilayer_buffered,ilayer_odirect}/ and
# any loose root-level *_workload*.log are moved to logs/ycsb/archive_<TS>/
# before the sweep.  At the end, prints LOAD-throughput average per source.

set -e

RECCOUNT="${1:-10000000}"
OPCOUNT="${2:-10000000}"
THREADS="${3:-64}"

WORKLOADS=(a b b2 b3 c d f g)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_DIR="$(cd "$SCRIPT_DIR/.." && pwd)/logs/ycsb"
mkdir -p "$LOG_DIR"

# ── Archive existing logs ─────────────────────────────────────────────────
TS="$(date +%Y%m%d_%H%M%S)"
ARCHIVE="$LOG_DIR/archive_${TS}"
moved_any=0
mkdir -p "$ARCHIVE"
for d in ztree ilayer_buffered ilayer_odirect; do
    if [ -d "$LOG_DIR/$d" ] && [ -n "$(ls -A "$LOG_DIR/$d" 2>/dev/null)" ]; then
        mv "$LOG_DIR/$d" "$ARCHIVE/$d"
        moved_any=1
    fi
done
shopt -s nullglob
loose=("$LOG_DIR"/*_workload*.log)
shopt -u nullglob
if [ ${#loose[@]} -gt 0 ]; then
    mv "${loose[@]}" "$ARCHIVE/"
    moved_any=1
fi
if [ $moved_any -eq 1 ]; then
    echo "[archive] previous logs moved to $ARCHIVE"
else
    rmdir "$ARCHIVE"
fi

# ── Sweep helper ──────────────────────────────────────────────────────────
run_sweep() {
    local variant=$1
    local odirect=$2
    local label=$3
    for wl in "${WORKLOADS[@]}"; do
        echo
        echo "================================================"
        echo "  $label  Workload $wl  records=$RECCOUNT  ops=$OPCOUNT  threads=$THREADS"
        echo "================================================"
        CNS_ODIRECT="$odirect" "$SCRIPT_DIR/run_ycsb.sh" \
            "$variant" "$wl" "$RECCOUNT" "$OPCOUNT" "$THREADS"
    done
}

run_sweep ztree  0 "ztree"
run_sweep ilayer 0 "ilayer-buffered"
run_sweep ilayer 1 "ilayer-O_DIRECT"

# ── LOAD throughput averages ──────────────────────────────────────────────
echo
echo "================================================"
echo "  LOAD throughput averages"
echo "================================================"
for d in ztree ilayer_buffered ilayer_odirect; do
    total=0
    n=0
    for log in "$LOG_DIR/$d"/*.log; do
        [ -e "$log" ] || continue
        val=$(grep -oE 'Load throughput\(ops/sec\):[[:space:]]+[0-9.]+' "$log" \
              | awk '{print $NF}' | head -1)
        [ -n "$val" ] || continue
        total=$(awk -v t="$total" -v v="$val" 'BEGIN{printf "%.4f", t+v}')
        n=$((n+1))
    done
    if [ "$n" -gt 0 ]; then
        avg=$(awk -v t="$total" -v n="$n" 'BEGIN{printf "%.1f", t/n/1000}')
        printf "  %-18s %s Kops/sec  (avg over %d workloads)\n" "$d:" "$avg" "$n"
    fi
done

echo
echo "All done.  Logs in logs/ycsb/{ztree,ilayer_buffered,ilayer_odirect}/"
