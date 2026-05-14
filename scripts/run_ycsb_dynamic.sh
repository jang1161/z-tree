#!/usr/bin/env bash
# Sawtooth experiment: load → A → C → D → G(delete), with cow_evict_cns_leaves
# + cow_gc_cns between each phase.  Uses the modified `ycsb_ctree -chain ...`
# binary built against the `dynamic` ctree variant.
#
# Usage:
#   scripts/run_ycsb_dynamic.sh [recordcount] [operationcount] [threads]
# Defaults: 1M records, 200K ops per phase, 64 threads.

set -e

RECCOUNT="${1:-1000000}"
OPCOUNT="${2:-200000}"
THREADS="${3:-64}"

# CNS_ODIRECT=1 to bypass kernel page cache for CNS writes/reads.
# Default 0 (buffered).  Toggling this is the primary knob for measuring
# how much of the sawtooth's drop comes from F2FS reclaiming page-cached
# blocks vs actual on-device physical reclamation.
CNS_ODIRECT_VAL="${CNS_ODIRECT:-0}"
MODE_TAG=$([ "$CNS_ODIRECT_VAL" = "1" ] && echo "odirect" || echo "buffered")

ZNS=/dev/nvme3n2

YCSB_DIR="$(cd "$(dirname "$0")/../../YCSB-cpp" && pwd)"
LOG_DIR="$(cd "$(dirname "$0")/.." && pwd)/logs/dynamic_multi-workload/$MODE_TAG"
mkdir -p "$LOG_DIR"

# Timestamp suffix prevents overwriting previous runs.  Set RUN_TAG="latest"
# (or any fixed string) to keep a stable path that does overwrite.
RUN_TAG="${RUN_TAG:-$(date +%Y%m%d_%H%M%S)}"
TRACE_CSV="$LOG_DIR/R${RECCOUNT}_O${OPCOUNT}_T${THREADS}_${RUN_TAG}.csv"
RUN_LOG="$LOG_DIR/R${RECCOUNT}_O${OPCOUNT}_T${THREADS}_${RUN_TAG}.log"

# Wipe disks so we start from a fresh tree.
echo "[reset] $ZNS  + remount /mnt/cns"
sudo nvme zns reset-zone -a "$ZNS" >/dev/null

# F2FS sparse file lives at /mnt/cns/nodes.dat — wiping the file is
# sufficient (no need to mkfs.f2fs each run); ctree_dynamic ftruncates
# on open if cns_open_flags include O_CREAT|O_TRUNC.
sudo rm -f /mnt/cns/nodes.dat

echo "[run]   records=$RECCOUNT  ops/phase=$OPCOUNT  threads=$THREADS  mode=$MODE_TAG"
echo "[trace] $TRACE_CSV"
echo "[log]   $RUN_LOG"

# Primary phase: load + workload A.  Then chain C, D, G.
# CTREE_DYNAMIC_GC_INTERVAL_MS=0 disables the background GC thread so the
# only evict+GC events are the explicit Maintenance() calls between phases.
sudo CTREE_DYNAMIC_GC_INTERVAL_MS=0 \
     CNS_ODIRECT="$CNS_ODIRECT_VAL" \
     CTREE_DYNAMIC_TRACE_PATH="$TRACE_CSV" \
     "$YCSB_DIR/ycsb_ctree" -load -run \
  -db ctree \
  -P "$YCSB_DIR/workloads/workloada" \
  -P "$YCSB_DIR/ctree/ctree.properties" \
  -p ctree.device="$ZNS" \
  -p recordcount="$RECCOUNT" \
  -p operationcount="$OPCOUNT" \
  -p chain_opcount="$OPCOUNT" \
  -p status.interval=10 \
  -threads "$THREADS" \
  -chain "$YCSB_DIR/workloads/workloadc,$YCSB_DIR/workloads/workloadd,$YCSB_DIR/workloads/workloadg" \
  -s 2>&1 | tee "$RUN_LOG"

echo
echo "[plot] generating graphs/ctree_dynamic/zns_cns_K..._T${THREADS}_${MODE_TAG}.png"

# Format records count for filename suffix (1M, 100K, etc.) to match plotter convention.
case "$RECCOUNT" in
  *000000) KEYS_TAG="$((RECCOUNT / 1000000))M" ;;
  *000)    KEYS_TAG="$((RECCOUNT / 1000))K"    ;;
  *)       KEYS_TAG="$RECCOUNT"                ;;
esac

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Drop privileges for plot step so matplotlib (installed in user env) is found.
# When invoked via `sudo bash run_...`, $SUDO_USER is set; otherwise we're
# already running as the user.
PLOT_TAG="${MODE_TAG}_${RUN_TAG}"
if [ -n "$SUDO_USER" ]; then
  sudo -u "$SUDO_USER" \
    CTREE_DYNAMIC_TRACE_PATH="$TRACE_CSV" \
    python3 "$REPO_ROOT/graphs/plot_dynamic_zns_cns.py" \
      "$RECCOUNT" "$THREADS" "$PLOT_TAG"
else
  CTREE_DYNAMIC_TRACE_PATH="$TRACE_CSV" \
    python3 "$REPO_ROOT/graphs/plot_dynamic_zns_cns.py" \
      "$RECCOUNT" "$THREADS" "$PLOT_TAG"
fi
