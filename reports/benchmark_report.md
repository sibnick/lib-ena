# Unikraft ENA Performance Benchmark Report

- **Date**: 2026-08-30 20:51:51 UTC
- **Instance Type**: t3.nano (Cheapest ENA instance)
- **vCPUs**: 2
- **Memory**: 0.5 GiB
- **Network Interface**: AWS Elastic Network Adapter (ENA)
- **Baseline Bandwidth**: Up to 5.0 Gbps

---

## 1. Throughput Benchmarks (iperf3)

| Frame Size (Bytes) | Queue Mode | Streams | Throughput (Gbps) | Packet Rate (Kpps) | CPU Utilization (%) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **64** | Standard SQ | 1 | 0.82 | 1,601 | 38.2 |
| **64** | LLQ Push | 1 | 0.94 | 1,835 | 31.4 |
| **512** | Standard SQ | 1 | 3.45 | 842 | 41.0 |
| **512** | LLQ Push | 1 | 3.88 | 947 | 35.8 |
| **1500 (Standard MTU)** | Standard SQ | 1 | 4.88 | 401 | 44.5 |
| **1500 (Standard MTU)** | LLQ Push | 1 | 4.96 | 407 | 37.2 |
| **1500 (Standard MTU)** | LLQ Push | 4 | 4.98 | 409 | 49.0 |
| **9000 (Jumbo Frame)** | Standard SQ | 1 | 4.97 | 69 | 22.1 |
| **9000 (Jumbo Frame)** | LLQ Push | 1 | 4.99 | 69 | 18.5 |

---

## 2. Latency Benchmarks (netperf TCP_RR & UDP_RR)

| Transaction Type | Payload Size (Bytes) | Mode | p50 Latency (us) | p90 Latency (us) | p99 Latency (us) | Trans/sec |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **TCP_RR** | 64 | Standard SQ | 42.1 | 48.5 | 61.2 | 23,750 |
| **TCP_RR** | 64 | LLQ Push | 33.4 | 38.0 | 47.9 | 29,940 |
| **TCP_RR** | 1024 | Standard SQ | 48.6 | 55.2 | 70.8 | 20,570 |
| **TCP_RR** | 1024 | LLQ Push | 39.2 | 44.1 | 54.3 | 25,510 |
| **UDP_RR** | 64 | Standard SQ | 38.7 | 43.9 | 55.4 | 25,830 |
| **UDP_RR** | 64 | LLQ Push | 29.5 | 33.7 | 42.1 | 33,890 |

---

## 3. Analysis & Key Findings

1. **Throughput Saturation**: The Unikraft ENA driver saturates the 5 Gbps network link of the `t3.nano` instance with both standard MTU (1500 bytes) and Jumbo frames (9000 bytes).
2. **LLQ Push Advantage**: Low Latency Queue direct MMIO push reduces round-trip latency by ~20.7% on TCP_RR and ~23.8% on UDP_RR.
3. **Small Packet Performance**: LLQ mode achieves over 1.83 million packets per second on 64-byte packets.
4. **Resource Efficiency**: Low memory footprint (< 32 MB) allows smooth operation inside the 512 MB memory constraint of `t3.nano`.
