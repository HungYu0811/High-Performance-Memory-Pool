#!/bin/bash
# interleaved_bench.sh
#
# Runs the v1.7 and v1.8 benchmark binaries in strict alternation (v1.7, v1.8,
# v1.7, v1.8, ...) instead of back-to-back batches. This spreads any VM-level
# noise (host contention, CPU frequency scaling, etc.) evenly across both
# versions, so a difference between them is more likely to reflect the actual
# code change rather than "which batch happened to run when the VM was busy."
#
# Usage:
#   chmod +x interleaved_bench.sh
#   ./interleaved_bench.sh <v1.7_binary> <v1.8_binary> <num_rounds>
#
# Example:
#   ./interleaved_bench.sh ./benchmark_bk ./benchmark 10
#
# Output: raw output from each run, prefixed with [ROUND n / v1.7] or
# [ROUND n / v1.8] so you can grep out just the numbers afterward, e.g.:
#   ./interleaved_bench.sh ./benchmark_bk ./benchmark 10 | tee bench_log.txt
#   grep "single] pool" bench_log.txt

set -e

V17_BIN="$1"
V18_BIN="$2"
ROUNDS="${3:-10}"

if [[ -z "$V17_BIN" || -z "$V18_BIN" ]]; then
    echo "Usage: $0 <v1.7_binary> <v1.8_binary> [num_rounds]"
    exit 1
fi

for ((i = 1; i <= ROUNDS; i++)); do
    echo "===== ROUND $i / v1.7 ====="
    "$V17_BIN"
    echo "===== ROUND $i / v1.8 ====="
    "$V18_BIN"
done
