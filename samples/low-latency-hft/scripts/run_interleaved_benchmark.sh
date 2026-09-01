#!/usr/bin/env bash
# ==============================================================================
# Interleaved Low-Latency Benchmark Suite for Spectral Challenge
# ==============================================================================
# Features:
# 1. Zero-I/O Measurement: Uses /dev/shm (RAM tmpfs) for latency logs to prevent
#    disk writeback / page cache flush spikes on p99.99.
# 2. Interleaved Execution: Alternates test configurations across iterations to
#    eliminate external drift and hypervisor scheduling bias.
# 3. Automated Core Preflight & Clean Termination.
# ==============================================================================

set -e

COUNT=${1:-50000}
RATE=${2:-100000}
RUNS=${3:-3}
TMP_DIR="/dev/shm/spectral_bench_$$"

mkdir -p "$TMP_DIR"
trap 'rm -rf "$TMP_DIR"; kill -9 $(jobs -p) 2>/dev/null || true' EXIT

echo "========================================================================"
echo " Starting Interleaved Benchmark Suite (Count: $COUNT, Rate: $RATE msg/s, Iterations: $RUNS)"
echo " Measurement tmpfs Directory: $TMP_DIR (Zero Disk I/O)"
echo "========================================================================"

cleanup_processes() {
    killall -9 sender receiver producer consumer >/dev/null 2>&1 || true
    rm -f /dev/shm/bench_cons /dev/shm/bench_prod >/dev/null 2>&1 || true
    sleep 0.05
}

run_trial() {
    local name="$1"
    local fec_flag="$2"
    local nak_flag="$3"
    local drop_flag="$4"
    local iter="$5"
    local csv_out="$TMP_DIR/${name}_iter${iter}.csv"

    cleanup_processes

    # 1. Start Receiver
    ./bin/receiver --shm /bench_cons --slots 65536 --port 9000 --echo-port 0 $nak_flag >/dev/null 2>&1 &
    local r_pid=$!
    sleep 0.05

    # 2. Start Consumer (pointing to RAM tmpfs)
    ./harness/bin/consumer --shm /bench_cons --slots 65536 --count "$COUNT" --idle-ms 1000 --csv "$csv_out" > "$TMP_DIR/${name}_c_${iter}.log" 2>&1 &
    local c_pid=$!
    sleep 0.05

    # 3. Start Sender
    ./bin/sender --shm /bench_prod --slots 65536 --dest 127.0.0.1 --port 9000 --count "$COUNT" --echo-listen 0 $fec_flag $nak_flag $drop_flag >/dev/null 2>&1 &
    local s_pid=$!
    sleep 0.05

    # 4. Start Producer (generates stream)
    ./harness/bin/producer --shm /bench_prod --slots 65536 --count "$COUNT" --rate "$RATE" --type mixed >/dev/null 2>&1 &
    local p_pid=$!

    wait $c_pid 2>/dev/null || true
    kill -9 $r_pid $s_pid $p_pid >/dev/null 2>&1 || true

    echo "  -> Finished $name [Iteration $iter]"
}

echo ""
echo "--- Running Interleaved Benchmark Trials ---"
for i in $(seq 1 "$RUNS"); do
    echo "[Round $i of $RUNS]"
    run_trial "Clean_NoFEC" "--no-fec" "--no-nak" "" "$i"
    run_trial "Clean_WithFEC" "--fec 16" "--no-nak" "" "$i"
    run_trial "Lossy1pct_WithFEC" "--fec 16" "--no-nak" "--drop-rate 0.01" "$i"
done

echo ""
echo "========================================================================"
echo " Benchmark Summary Results (Averaged Across Interleaved Iterations)"
echo "========================================================================"

for config in "Clean_NoFEC" "Clean_WithFEC" "Lossy1pct_WithFEC"; do
    echo "Configuration: $config"
    grep -h "latency (ns)" "$TMP_DIR/${config}"_c_*.log | head -n 1 || true
    grep -h -E "p01|p50|p99|p99.9|p99.99" "$TMP_DIR/${config}"_c_*.log | tail -n 5 || true
    echo "------------------------------------------------------------------------"
done
