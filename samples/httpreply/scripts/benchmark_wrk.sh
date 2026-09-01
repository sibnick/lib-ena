#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
#
# Automated HTTP benchmark harness using wrk
# Measures requests/second, throughput, and latency percentiles on AWS EC2.

set -euo pipefail

TARGET_IP="${1:-}"
DURATION="${2:-30s}"
THREADS="${3:-4}"

if [ -z "$TARGET_IP" ]; then
    echo "Usage: $0 <target-ip> [duration] [threads]"
    echo "Example: $0 54.210.10.20 30s 4"
    exit 1
fi

if ! command -v wrk &> /dev/null; then
    echo "[ERR] wrk not found. Install with: sudo apt install wrk"
    exit 1
fi

URL="http://${TARGET_IP}/"
CONCURRENCY_LEVELS=(10 50 100 200 500)
OUT_DIR="benchmark-results/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"

echo "========================================================="
echo " Starting Unikraft ENA HTTP Benchmark (app-httpreply)"
echo " Target:     $URL"
echo " Duration:   $DURATION per concurrency step"
echo " Threads:    $THREADS"
echo " Results in: $OUT_DIR"
echo "========================================================="

# Summary CSV header
CSV_FILE="$OUT_DIR/benchmark_summary.csv"
echo "concurrency,requests_per_sec,transfer_mb_per_sec,p50_ms,p75_ms,p90_ms,p99_ms" > "$CSV_FILE"

for c in "${CONCURRENCY_LEVELS[@]}"; do
    echo ""
    echo ">>> Running test at Concurrency = $c (threads: $THREADS, duration: $DURATION)..."
    LOG_FILE="$OUT_DIR/wrk_c${c}.log"

    wrk -t"$THREADS" -c"$c" -d"$DURATION" --latency "$URL" | tee "$LOG_FILE"

    # Extract metrics from wrk log
    RPS=$(grep "Requests/sec:" "$LOG_FILE" | awk '{print $2}')
    TRANSFER=$(grep "Transfer/sec:" "$LOG_FILE" | awk '{print $2}')
    P50=$(grep "50%" "$LOG_FILE" | awk '{print $2}')
    P75=$(grep "75%" "$LOG_FILE" | awk '{print $2}')
    P90=$(grep "90%" "$LOG_FILE" | awk '{print $2}')
    P99=$(grep "99%" "$LOG_FILE" | awk '{print $2}')

    echo "$c,$RPS,$TRANSFER,$P50,$P75,$P90,$P99" >> "$CSV_FILE"
done

echo ""
echo "========================================================="
echo " Benchmark completed successfully!"
echo " Summary CSV: $CSV_FILE"
echo "========================================================="
cat "$CSV_FILE"
