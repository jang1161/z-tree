#!/usr/bin/env bash
# Usage: scripts/run_ycsb_dynamic.sh <variant> <records> <opcount> <threads>
#   variant : dynamic | ilayer | ztree
# Env:
#   CNS_ODIRECT=1   bypass CNS page cache  (dynamic/ilayer only; ztree ignores)
#   RUN_TAG=...     override the timestamp suffix in the output filename
#
# Runs:  load + workloadA + chain(C,D,G).
#   - dynamic: CNS sharded + GC envs + trace + plot.
#   - ilayer : same chain (ilayer has no CNS GC; Maintenance is no-op leaf-wise).
#   - ztree  : no CNS, no dynamic envs, no trace/plot.
#
# Logs:  logs/ycsb_multi/<variant>/<mode>/...

set -e

VARIANT="${1:?variant required (dynamic|ilayer|ztree)}"
RECCOUNT="${2:-1000000}"
OPCOUNT="${3:-200000}"
THREADS="${4:-64}"

CNS_ODIRECT_VAL="${CNS_ODIRECT:-0}"

case "$VARIANT" in
  dynamic|ilayer)
    BIN=ycsb_ctree; DB=ctree
    PROPS_REL=ctree/ctree.properties
    DEV_PROP=ctree.device
    MODE_TAG=$([ "$CNS_ODIRECT_VAL" = "1" ] && echo "odirect" || echo "buffered")
    USE_CNS=1
    ;;
  ztree)
    BIN=ycsb_ztree; DB=ztree
    PROPS_REL=ztree/ztree.properties
    DEV_PROP=ztree.device
    MODE_TAG=zns
    USE_CNS=0
    ;;
  *)
    echo "unknown variant: $VARIANT (use dynamic|ilayer|ztree)" >&2
    exit 1
    ;;
esac

USE_TRACE=1  # all variants emit trace now
ZNS_GC_ON="${CTREE_DYNAMIC_ZNS_GC:-1}"
ZNS_GC_INT="${CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS:-10000}"
if [ "$VARIANT" = "dynamic" ]; then
  DYN_ENVS=(
    CTREE_DYNAMIC_GC_INTERVAL_MS=0
    CNS_ODIRECT="$CNS_ODIRECT_VAL"
    CTREE_DYNAMIC_ZNS_GC="$ZNS_GC_ON"
    CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS="$ZNS_GC_INT"
  )
elif [ "$VARIANT" = "ilayer" ]; then
  DYN_ENVS=(
    CNS_ODIRECT="$CNS_ODIRECT_VAL"
    CTREE_DYNAMIC_ZNS_GC="$ZNS_GC_ON"
    CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS="$ZNS_GC_INT"
  )
else
  DYN_ENVS=(
    CTREE_DYNAMIC_ZNS_GC="$ZNS_GC_ON"
    CTREE_DYNAMIC_ZNS_GC_INTERVAL_MS="$ZNS_GC_INT"
  )
fi

ZNS=/dev/nvme3n2
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
YCSB_DIR="$(cd "$REPO_ROOT/../YCSB-cpp" && pwd)"
LOG_DIR="$REPO_ROOT/logs/ycsb_multi/${VARIANT}/$MODE_TAG"
mkdir -p "$LOG_DIR"

fmt_count() {
  case "$1" in
    *000000) echo "$(($1 / 1000000))M" ;;
    *000)    echo "$(($1 / 1000))K"    ;;
    *)       echo "$1"                 ;;
  esac
}
KEYS_TAG=$(fmt_count "$RECCOUNT")
OPS_TAG=$(fmt_count "$OPCOUNT")
RUN_TAG="${RUN_TAG:-$(date +%Y%m%d_%H%M%S)}"
RUN_NAME="K${KEYS_TAG}_O${OPS_TAG}_T${THREADS}_${MODE_TAG}_${RUN_TAG}"
TRACE_CSV="$LOG_DIR/${RUN_NAME}.csv"
RUN_LOG="$LOG_DIR/${RUN_NAME}.log"

# ── Ensure the binary is built against the right variant ────────────────
if [ "$VARIANT" = "dynamic" ] || [ "$VARIANT" = "ilayer" ]; then
  CTREE_VARIANT_REL="../z-tree/ctree/variants/$VARIANT/ctree_main.c"
  echo "[build] $BIN  (CTREE_VARIANT=$VARIANT)"
  ( cd "$YCSB_DIR" && rm -f "$BIN" && \
    make BIND_CTREE=1 CTREE_VARIANT="$CTREE_VARIANT_REL" -j4 >/dev/null )
else
  echo "[build] $BIN  (BIND_ZTREE=1)"
  ( cd "$YCSB_DIR" && rm -f "$BIN" && make BIND_ZTREE=1 -j4 >/dev/null )
fi

# ── Reset device(s) ─────────────────────────────────────────────────────
echo "[reset] $ZNS"
sudo nvme zns reset-zone -a "$ZNS" >/dev/null
if [ "$USE_CNS" = "1" ]; then
  echo "[reset] /mnt/cns/nodes.*.dat"
  sudo rm -f /mnt/cns/nodes.dat /mnt/cns/nodes.*.dat
fi

echo "[run]   variant=$VARIANT  records=$RECCOUNT  ops/phase=$OPCOUNT  threads=$THREADS  mode=$MODE_TAG"
echo "[log]   $RUN_LOG"

RUN_CMD=( "$YCSB_DIR/$BIN" -load -run
          -db "$DB"
          -P "$YCSB_DIR/workloads/workloada"
          -P "$YCSB_DIR/$PROPS_REL"
          -p "$DEV_PROP=$ZNS"
          -p recordcount="$RECCOUNT"
          -p operationcount="$OPCOUNT"
          -p chain_opcount="$OPCOUNT"
          -p status.interval=20
          -threads "$THREADS"
          -chain "$YCSB_DIR/workloads/workloadc,$YCSB_DIR/workloads/workloadd,$YCSB_DIR/workloads/workloadg"
          -s )

if [ "$USE_TRACE" = "1" ]; then
  sudo "${DYN_ENVS[@]}" CTREE_DYNAMIC_TRACE_PATH="$TRACE_CSV" \
       "${RUN_CMD[@]}" 2>&1 | tee "$RUN_LOG"
else
  sudo "${DYN_ENVS[@]}" "${RUN_CMD[@]}" 2>&1 | tee "$RUN_LOG"
fi

# ── Plot (all variants now emit the trace CSV) ──────────────────────────
echo
echo "[plot] ${RUN_NAME}.png  (variant=$VARIANT)"
if [ -n "$SUDO_USER" ]; then
  sudo -u "$SUDO_USER" \
    CTREE_DYNAMIC_TRACE_PATH="$TRACE_CSV" \
    python3 "$REPO_ROOT/graphs/plot_dynamic_zns_cns.py" \
      "$RECCOUNT" "$THREADS" "$MODE_TAG" "$RUN_NAME" "$VARIANT"
else
  CTREE_DYNAMIC_TRACE_PATH="$TRACE_CSV" \
    python3 "$REPO_ROOT/graphs/plot_dynamic_zns_cns.py" \
      "$RECCOUNT" "$THREADS" "$MODE_TAG" "$RUN_NAME" "$VARIANT"
fi
