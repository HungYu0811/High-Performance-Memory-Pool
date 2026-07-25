#!/bin/bash
# interleaved_bench.sh
#
# Runs two benchmark binaries in strict alternation (A, B, A, B, ...) instead of
# back-to-back batches. This spreads any VM-level noise (host contention, CPU
# frequency scaling, etc.) evenly across both versions, so a difference between
# them is more likely to reflect the actual code change rather than "which batch
# happened to run when the VM was busy."
#
# Generalized from the original v1.7-vs-v1.8 version: takes free-form labels now,
# instead of "v1.7"/"v1.8" being hardcoded, so the same script works for any
# A/B comparison (this round: v1.8 baseline vs v1.9) without editing the file.
#
# Usage:
#   chmod +x interleaved_bench.sh
#   ./interleaved_bench.sh <bin_A> <bin_B> <num_rounds> [label_A] [label_B]
#
# Example (this round — v1.8 vector<bool> baseline vs v1.9 vector<uint8_t>):
#   ./interleaved_bench.sh ./benchmark_v1_8 ./benchmark_v1_9 8 v1.8 v1.9
#
# Output: raw output from each run, prefixed with [ROUND n / <label>] so you can
# grep out just the numbers afterward, e.g.:
#   ./interleaved_bench.sh ./benchmark_v1_8 ./benchmark_v1_9 8 v1.8 v1.9 | tee bench_log.txt
#   grep "single] pool" bench_log.txt
# NOTE on ordering bias (found the hard way, v1.8-vs-v1.9 test): running "A then B" every
# single round, even though rounds themselves alternate, still gives B a systematic advantage
# from CPU frequency ramp-up / cache warm-up, since B is always the *second* thing to run in
# that round. The unmodified `system` control (identical code in both binaries) showed this
# directly: it was higher in binary B in 8/8 rounds, which cannot be a code effect since that
# code never changed — it's the warm-up-from-running-second effect. Fix: flip which binary
# goes first on alternating rounds (odd rounds: A then B; even rounds: B then A), so the
# warm-up advantage is split evenly instead of always favoring the second argument.
set -e
BIN_A="$1"
BIN_B="$2"
ROUNDS="${3:-10}"
LABEL_A="${4:-A}"
LABEL_B="${5:-B}"
if [[ -z "$BIN_A" || -z "$BIN_B" ]]; then
    echo "Usage: $0 <bin_A> <bin_B> [num_rounds] [label_A] [label_B]"
    exit 1
fi
for ((i = 1; i <= ROUNDS; i++)); do
    if (( i % 2 == 1 )); then
        echo "===== ROUND $i / $LABEL_A ====="
        "$BIN_A"
        echo "===== ROUND $i / $LABEL_B ====="
        "$BIN_B"
    else
        echo "===== ROUND $i / $LABEL_B ====="
        "$BIN_B"
        echo "===== ROUND $i / $LABEL_A ====="
        "$BIN_A"
    fi
done
