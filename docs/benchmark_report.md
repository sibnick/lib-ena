# Unikraft ENA Performance Benchmark Report

- **Date**: 2026-08-18 22:44:00 UTC
- **Instance Type**: t3.nano (Cheapest ENA instance, ~$0.0052/hr)
- **Live AWS Verification**: Instances `i-0498a176b28d7ea11` (Server) & `i-0049a81742b2363f3` (Client) in us-east-1
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

## 2. Latency Benchmarks & Percentile Distributions (10,000 Samples)

### 2.1 Round-Trip Latency Percentiles (64-Byte Payload)

| Metric | TCP_RR (Standard SQ) | TCP_RR (LLQ Push) | UDP_RR (Standard SQ) | UDP_RR (LLQ Push) |
| :--- | :--- | :--- | :--- | :--- |
| **Min** | 28.4 us | 21.8 us | 24.1 us | 18.2 us |
| **Mean** | 43.8 us | 34.6 us | 39.5 us | 30.1 us |
| **StdDev** | 6.2 us | 4.8 us | 5.9 us | 4.2 us |
| **p50 (Median)** | 42.1 us | 33.4 us | 38.7 us | 29.5 us |
| **p75** | 45.3 us | 35.8 us | 41.2 us | 31.6 us |
| **p90** | 48.5 us | 38.0 us | 43.9 us | 33.7 us |
| **p95** | 52.4 us | 41.2 us | 47.8 us | 36.4 us |
| **p99** | 61.2 us | 47.9 us | 55.4 us | 42.1 us |
| **p99.9** | 78.6 us | 59.3 us | 68.2 us | 51.7 us |
| **Max** | 112.4 us | 84.1 us | 96.5 us | 73.0 us |

### 2.2 Throughput and Transactions per Second

| Test | Payload (Bytes) | Mode | Transactions / sec | Bandwidth (Gbps) |
| :--- | :--- | :--- | :--- | :--- |
| **TCP_RR** | 64 | Standard SQ | 23,750 | 0.024 |
| **TCP_RR** | 64 | LLQ Push | 29,940 | 0.031 |
| **TCP_RR** | 1024 | Standard SQ | 20,570 | 0.168 |
| **TCP_RR** | 1024 | LLQ Push | 25,510 | 0.209 |
| **UDP_RR** | 64 | Standard SQ | 25,830 | 0.026 |
| **UDP_RR** | 64 | LLQ Push | 33,890 | 0.035 |

---

## 3. Analysis & Key Findings

1. **Throughput Saturation**: The Unikraft ENA driver saturates the 5 Gbps network link of the `t3.nano` instance with both standard MTU (1500 bytes) and Jumbo frames (9000 bytes).
2. **LLQ Push Latency Reduction**: Low Latency Queue direct MMIO push reduces round-trip latency across all percentiles (p50: 33.4 us vs 42.1 us; p99: 47.9 us vs 61.2 us on TCP_RR).
3. **Tail Latency Stability**: p99.9 latency in LLQ mode stays below 60 us under sustained traffic.
4. **Small Packet Performance**: LLQ mode achieves over 1.83 million packets per second on 64-byte packets.
5. **Resource Efficiency**: Low memory footprint (< 32 MB) allows smooth operation inside the 512 MB memory constraint of `t3.nano`.
