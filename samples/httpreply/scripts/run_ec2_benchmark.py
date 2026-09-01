#!/usr/bin/env python3
"""
Automated wrk benchmark execution script comparing Unikraft ENA and Linux baseline.
Runs multi-concurrency wrk benchmarks and records throughput and latency metrics.
"""

import subprocess
import re
import json
import csv
import sys
import time

CONCURRENCIES = [1, 5, 10, 25, 50, 100, 200]
DURATION = "10s"
THREADS = 2

TARGETS = {
    "Unikraft (Optimized ENA + lwIP)": "http://172.31.16.153/",
    "Linux (Ubuntu 24.04 ENA)": "http://172.31.16.160/"
}

def parse_wrk_output(output):
    data = {
        "requests_sec": 0.0,
        "transfer_kb_sec": 0.0,
        "latency_avg_ms": 0.0,
        "latency_stdev_ms": 0.0,
        "latency_max_ms": 0.0,
        "total_requests": 0,
        "socket_errors": 0
    }
    
    # Requests/sec: 1234.56
    req_match = re.search(r"Requests/sec:\s+([\d\.]+)", output)
    if req_match:
        data["requests_sec"] = float(req_match.group(1))
        
    # Transfer/sec: 123.45KB or 1.23MB
    tx_match = re.search(r"Transfer/sec:\s+([\d\.]+)\s*([KMG]?B)", output)
    if tx_match:
        val = float(tx_match.group(1))
        unit = tx_match.group(2)
        if unit == "MB":
            val *= 1024.0
        elif unit == "GB":
            val *= 1024.0 * 1024.0
        data["transfer_kb_sec"] = val

    # Thread Stats Latency
    lat_match = re.search(r"Latency\s+([\d\.]+)(\w+)\s+([\d\.]+)(\w+)\s+([\d\.]+)(\w+)", output)
    if lat_match:
        def to_ms(v, u):
            v = float(v)
            if u == "us": return v / 1000.0
            if u == "ms": return v
            if u == "s": return v * 1000.0
            if u == "m": return v * 60000.0
            return v
        data["latency_avg_ms"] = to_ms(lat_match.group(1), lat_match.group(2))
        data["latency_stdev_ms"] = to_ms(lat_match.group(3), lat_match.group(4))
        data["latency_max_ms"] = to_ms(lat_match.group(5), lat_match.group(6))

    # Total requests
    tot_match = re.search(r"(\d+)\s+requests in", output)
    if tot_match:
        data["total_requests"] = int(tot_match.group(1))

    # Socket errors
    err_match = re.search(r"Socket errors:\s+connect\s+(\d+),\s+read\s+(\d+),\s+write\s+(\d+),\s+timeout\s+(\d+)", output)
    if err_match:
        data["socket_errors"] = sum(int(err_match.group(i)) for i in range(1, 5))

    return data

def main():
    results = []
    print("=" * 60)
    print("Starting Automated AWS EC2 Network Benchmark")
    print(f"Duration per run: {DURATION}, Threads: {THREADS}")
    print("=" * 60)

    for c in CONCURRENCIES:
        t = min(THREADS, c)
        print(f"\n--- Testing Concurrency Level: {c} (Threads: {t}) ---")
        
        for name, url in TARGETS.items():
            print(f"Running wrk against {name} ({url})...")
            cmd = ["wrk", f"-t{t}", f"-c{c}", f"-d{DURATION}", "--latency", url]
            try:
                res = subprocess.run(cmd, capture_output=True, text=True, check=True)
                parsed = parse_wrk_output(res.stdout)
                entry = {
                    "concurrency": c,
                    "target": name,
                    "url": url,
                    **parsed
                }
                results.append(entry)
                print(f"  -> {parsed['requests_sec']:.2f} req/s, avg latency: {parsed['latency_avg_ms']:.2f}ms, max latency: {parsed['latency_max_ms']:.2f}ms, errors: {parsed['socket_errors']}")
            except Exception as e:
                print(f"  -> Error running wrk: {e}")
            time.sleep(1)

    # Save to JSON
    with open("benchmark_results.json", "w") as f:
        json.dump(results, f, indent=2)
    print("\n[SUCCESS] Results saved to benchmark_results.json")

    # Save to CSV
    with open("benchmark_results.csv", "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=[
            "concurrency", "target", "requests_sec", "latency_avg_ms", "latency_stdev_ms", "latency_max_ms", "transfer_kb_sec", "total_requests", "socket_errors"
        ], extrasaction='ignore')
        writer.writeheader()
        for r in results:
            writer.writerow(r)
    print("[SUCCESS] Results saved to benchmark_results.csv")

if __name__ == "__main__":
    main()
