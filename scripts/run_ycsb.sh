#!/usr/bin/env bash
# Usage: scripts/run_ycsb.sh <variant> <workload_letter> [recordcount] [opcount] [threads]
#   variant         : ilayer | ztree
#   workload_letter : a|b|c|d|e|f
#   recordcount     : default 1000000
#   opcount         : default 1000000
#   threads         : default 64
#
# Resets ZNS+CNS, then runs YCSB-cpp -load -run with the chosen binding.

set -e

VARIANT="${1:?variant required (ilayer|ztree)}"
WL="${2:?workload letter required (a|b|c|d|e|f)}"
RECCOUNT="${3:-1000000}"
OPCOUNT="${4:-1000000}"
THREADS="${5:-64}"

ZNS=/dev/nvme3n2
CNS=/dev/nvme3n1

case "$VARIANT" in
  ilayer)
    BIN=ycsb_ctree
    DB=ctree
    PROPS=ctree/ctree.properties
    DEV_PROP=ctree.device
    ;;
  ztree)
    BIN=ycsb_ztree
    DB=ztree
    PROPS=ztree/ztree.properties
    DEV_PROP=ztree.device
    ;;
  *)
    echo "Unknown variant: $VARIANT (expected ilayer|ztree)" >&2
    exit 1
    ;;
esac

YCSB_DIR="$(cd "$(dirname "$0")/../../YCSB-cpp" && pwd)"
LOG_DIR="$(cd "$(dirname "$0")/.." && pwd)/logs/ycsb"

# Route to per-variant subdir.  ztree → ztree/, ilayer → ilayer_{buffered,odirect}/
if [ "$VARIANT" = "ztree" ]; then
    SUBDIR="ztree"
elif [ "${CNS_ODIRECT:-0}" = "1" ]; then
    SUBDIR="ilayer_odirect"
else
    SUBDIR="ilayer_buffered"
fi
mkdir -p "$LOG_DIR/$SUBDIR"

LOG="$LOG_DIR/$SUBDIR/${VARIANT}_workload${WL}_R${RECCOUNT}_O${OPCOUNT}_T${THREADS}.log"

echo "[reset] $ZNS + $CNS"
sudo nvme zns reset-zone -a "$ZNS" >/dev/null
sudo blkdiscard "$CNS"

echo "[run]   variant=$VARIANT  workload$WL  records=$RECCOUNT  ops=$OPCOUNT  threads=$THREADS"
echo "[log]   $LOG"

sudo CNS_ODIRECT="${CNS_ODIRECT:-0}" "$YCSB_DIR/$BIN" -load -run \
  -db "$DB" \
  -P "$YCSB_DIR/workloads/workload$WL" \
  -P "$YCSB_DIR/$PROPS" \
  -p "$DEV_PROP=$ZNS" \
  -p recordcount="$RECCOUNT" \
  -p operationcount="$OPCOUNT" \
  -p status.interval=20 \
  -threads "$THREADS" \
  -s 2>&1 | tee "$LOG"
