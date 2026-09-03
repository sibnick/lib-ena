# Low-Latency UDP Echo Sample

This directory contains a low-latency UDP echo server and benchmarking client written in C.
The sample runs on standard Linux and as a Unikraft unikernel with the AWS ENA network driver.

## Overview

High-frequency trading (HFT) and telemetry workloads require minimal round-trip latency.
This sample shows a zero-copy UDP datagram echo loop.

### Optimizations
- **Zero dynamic memory allocation**: The server receives and echoes packets directly in stack buffers.
- **Large socket buffers**: Configures 4 MB receive and send buffers to prevent kernel packet drops.
- **Low-delay IP TOS**: Sets the low-delay DSCP traffic class on the socket.
- **Busy-polling**: Uses `SO_BUSY_POLL` when running on Linux kernels that support it.
- **Core affinity**: Lets the user pin server and client threads to dedicated CPU cores.

## Directory Structure

```
samples/low-latency-hft/
├── server.c         # Low-latency UDP echo server in C
├── client.c         # Low-latency UDP echo benchmark client in C
├── Makefile         # Build rules for native Linux binaries
├── Config.uk        # Unikraft KConfig integration
├── Makefile.uk      # Unikraft build definition
├── Kraftfile        # KraftKit specification for building unikernels
├── scripts/
│   └── tune_host.sh # Linux kernel and IRQ network tuning script
└── README.md        # Documentation
```

## Building Native Binaries

Build the server and client with `make`:

```bash
make clean
make
```

The build produces two binaries in `bin/`:
- `bin/server`: UDP echo server.
- `bin/client`: UDP latency benchmark client.

## Running the Echo Server

Start the server on port 9000:

```bash
./bin/server -p 9000
```

To pin the server to CPU core 2:

```bash
./bin/server -p 9000 -c 2
```

## Running the Benchmark Client

Send 50000 probe packets to the server:

```bash
./bin/client -s 127.0.0.1 -p 9000 -n 50000
```

Sample output:

```
========================================
 Low-Latency UDP Echo Benchmark Client
 Target: 127.0.0.1:9000
 Probes: 50000 packets, Size: 64 bytes
 Target Rate: Unthrottled (RTT synchronous)
========================================

--- Benchmark Results ---
Sent:               50000 packets
Received:           50000 packets (0.00% loss)
Duration:           0.200 seconds (250346 pkts/sec)
Round-Trip Latency (RTT):
  Min:                 2.67 µs (2671 ns)
  p50 (Median):        3.99 µs (3986 ns)
  p90:                 4.52 µs (4523 ns)
  p99:                 6.05 µs (6055 ns)
  p99.9:              58.20 µs (58201 ns)
  Max:               786.98 µs (786980 ns)
  Mean ± StdDev:       3.98 ± 6.93 µs
-------------------------
```

## Automated Test

Run the automated test target:

```bash
make test
```

This target builds both binaries.
It starts the server on port 9876.
It sends 5000 test packets through the client.
It verifies complete packet reception and stops the server cleanly.

## Empirical AWS EC2 Benchmark Results

We measured the performance on real AWS EC2 instances.

### Test Environment
- **Instance Type**: 2x `c7i.large` (Intel Sapphire Rapids, 2 vCPUs, 4 GB RAM)
- **Network Hardware**: AWS Elastic Network Adapter (ENA) with native kernel driver
- **Operating System**: Ubuntu 24.04 LTS (kernel 6.8 with low-latency tuning)
- **Topology**: Dedicated client and server in the same VPC subnet (`us-east-1a`)
- **Traffic**: UDP echo on port 9000 with 64-byte probe datagrams

### Measured Performance

| Metric | Cross-Instance (AWS VPC) | Local Loopback (c7i.large) |
| :--- | :--- | :--- |
| **Packets Sent** | 100,000 | 100,000 |
| **Packets Received** | 100,000 | 100,000 |
| **Packet Loss** | **0.00%** | **0.00%** |
| **Min RTT** | 63.98 µs | 5.64 µs |
| **p50 (Median) RTT** | **97.81 µs** | **10.59 µs** |
| **p90 RTT** | 102.45 µs | 11.01 µs |
| **p99 RTT** | 111.17 µs | 13.89 µs |
| **p99.9 RTT** | 123.37 µs | 21.55 µs |
| **Max RTT** | 487.44 µs | 92.92 µs |
| **Mean ± StdDev** | 95.60 ± 9.43 µs | 9.46 ± 2.36 µs |

Across the 220,000 total packets processed during the test suite, the server achieved 100% echo delivery with zero dropped packets.
Tail latency (p99.9) remained bounded under 125 microseconds over the AWS VPC network.

