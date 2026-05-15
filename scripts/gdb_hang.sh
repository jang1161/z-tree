#!/usr/bin/env bash
# gdb_hang.sh — snapshot all thread backtraces of a hung ycsb_ctree.
#
# Usage:  sudo bash scripts/gdb_hang.sh
#
# Attaches gdb non-interactively, dumps every thread's backtrace to
# /tmp/hang_bt.txt, then detaches (the process keeps running so you can
# snapshot again or kill it yourself).

set -e
# -x matches the executable name exactly (comm), so the `sudo ... ycsb_ctree`
# wrapper process (comm=sudo) is skipped — we want the real binary.
PID=$(pgrep -x ycsb_ctree | head -1)
if [ -z "$PID" ]; then
    echo "no ycsb_ctree process found"
    exit 1
fi
OUT=/tmp/hang_bt.txt
echo "attaching gdb to pid $PID ..."
gdb -p "$PID" -batch \
    -ex "set pagination off" \
    -ex "info threads" \
    -ex "thread apply all bt" \
    > "$OUT" 2>&1
echo "backtraces written to $OUT  ($(wc -l < "$OUT") lines)"
echo "--- thread summary ---"
grep -E "^Thread|#0 " "$OUT" | head -40
