#!/bin/bash
# bench_random.sh — 5-way random-insert comparison
#
# Runs:
#   1. ztree              (build/ztree)
#   2. ctree buffered     (build/ctree, CNS_ODIRECT=0)
#   3. ctree O_DIRECT     (build/ctree, CNS_ODIRECT=1)
#   4. ilayer buffered    (build/ctree_ilayer, CNS_ODIRECT=0)
#   5. ilayer O_DIRECT    (build/ctree_ilayer, CNS_ODIRECT=1)
#
# Each variant inserts $KEYS keys in shuffled (Fisher-Yates) order — the
# bench main's default when BENCH_SEQUENTIAL is unset.  ZNS is reset and
# the CNS file is wiped between every run so each variant starts fresh.
#
# Usage:
#   sudo bash scripts/bench_random.sh [keys] [threads]
# Defaults: 10M keys, 64 threads.

set -e

KEYS="${1:-10000000}"
THREADS="${2:-64}"
ZNS=/dev/nvme3n2
CNS_FILE=/mnt/cns/nodes.dat

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TS=$(date +%Y%m%d_%H%M%S)
OUTDIR="$REPO_ROOT/logs/random/${KEYS}_T${THREADS}_${TS}"
mkdir -p "$OUTDIR"

# Variant table: label  binary                          extra_env_for_run
VARIANTS=(
    "ztree              build/ztree                     "
    "ctree_buffered     build/ctree                     CNS_ODIRECT=0"
    "ctree_odirect      build/ctree                     CNS_ODIRECT=1"
    "ilayer_buffered    build/ctree_ilayer              CNS_ODIRECT=0"
    "ilayer_odirect     build/ctree_ilayer              CNS_ODIRECT=1"
)

echo "=== Random insert ($KEYS keys, $THREADS threads) ==="
echo "Device: $ZNS"
echo "Output: $OUTDIR"
echo ""

for v in "${VARIANTS[@]}"; do
    set -- $v
    LABEL=$1; BIN=$2; EXTRA_ENV=${3:-}

    LOG="$OUTDIR/${LABEL}.log"

    echo "[reset] ZNS + CNS file for ${LABEL}"
    sudo nvme zns reset-zone -a "$ZNS" >/dev/null
    sudo rm -f "$CNS_FILE"

    echo "[run]   ${LABEL}  (BIN=$BIN  $EXTRA_ENV)"
    sudo $EXTRA_ENV "$REPO_ROOT/$BIN" \
        "$KEYS" "$THREADS" "$ZNS" > "$LOG" 2>&1

    # Strip noisy progress lines that bench_main prints during the run.
    sed -i '/Progress:/d' "$LOG" 2>/dev/null || true

    TPUT=$(grep -E 'Average throughput|throughput' "$LOG" | head -1 | awk '{print $3}')
    echo "  -> ${TPUT:-?} ops/sec"
    echo ""
done

echo "Done.  Per-variant logs in: $OUTDIR/"
echo ""
echo "Summary:"
for v in "${VARIANTS[@]}"; do
    set -- $v
    LABEL=$1
    LOG="$OUTDIR/${LABEL}.log"
    TPUT=$(grep -E 'Average throughput|throughput' "$LOG" 2>/dev/null | head -1 | awk '{print $3}')
    printf "  %-20s %s ops/sec\n" "$LABEL" "${TPUT:-?}"
done
