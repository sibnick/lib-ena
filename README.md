# AWS ENA Native Driver for Unikraft

This repository contains the native AWS Elastic Network Adapter (ENA) driver for the Unikraft unikernel.

## Overview

The ENA driver provides high-performance networking for Unikraft unikernels running on Amazon EC2 instances. It interacts directly with the ENA PCI hardware without Linux kernel dependencies.

## Feature Matrix

| Feature | Status | Description |
| :--- | :--- | :--- |
| **PCI Probe & Reset** | Supported | Device identification, MMIO BAR0 mapping, and controller lifecycle. |
| **Admin Queue (AQ/ACQ)** | Supported | Synchronous device configuration and capability discovery. |
| **AENQ Engine** | Supported | Asynchronous event notifications and health monitoring. |
| **TX / RX Rings** | Supported | Multi-queue circular descriptor rings with hardware checksum offload. |
| **Low Latency Queue (LLQ)** | Supported | Direct push of packet headers and descriptors to BAR2 MMIO. |
| **Interrupt / MSI-X Polling** | Supported | Per-queue interrupt masking and high-frequency polling engine. |
| **Jumbo Frames** | Supported | Configurable MTU up to 9000 bytes. |

## Supported EC2 Instance Types

The driver supports all AWS EC2 instance types equipped with ENA hardware:

- **General Purpose**: `t3`, `m5`, `m6i`, `m6a`, `m7i`, `m7a`
- **Compute Optimized**: `c5`, `c6i`, `c6a`, `c7i`
- **Memory Optimized**: `r5`, `r6i`, `r6a`, `r7i`
- **Graviton (ARM64)**: `c6g`, `m6g`, `r6g`, `c7g`, `m7g`, `r7g`

## Kconfig Configuration Options

The driver exposes the following Kconfig options in `Config.uk`:

- `CONFIG_LIBENA`: Enable the AWS ENA native network driver.
- `CONFIG_LIBENA_LLQ`: Enable Low Latency Queue (LLQ) direct MMIO push mode (default: `y`).
- `CONFIG_LIBENA_MAX_QUEUES`: Maximum number of IO queue pairs per device (default: `8`).

## Build Instructions

### Unikraft Integration Build

Build your Unikraft image using KraftKit:

```bash
kraft build --target kvm --plat qemu --arch x86_64
```

### Standalone Test Suite

Build and run the standalone host unit tests and validation harness:

```bash
make clean
make test
```

## Performance Benchmark Summary

The driver was validated on an AWS `t3.nano` instance (`i-04ac6e142c9989dc4`):

- **Throughput (1500 MTU)**: Line rate saturation at 4.96 Gbps (407 Kpps).
- **Throughput (9000 Jumbo MTU)**: Line rate saturation at 4.99 Gbps (69 Kpps).
- **Round-Trip Latency (TCP_RR)**: 33.4 us median (p50) and 47.9 us 99th percentile (p99) with LLQ.
- **Small Packet Rate (64B)**: 1.835 Mpps under LLQ direct push mode.

See [docs/benchmark_report.md](docs/benchmark_report.md) for full benchmark details.

## License

This project is licensed under the BSD-3-Clause License. See [COPYING.md](COPYING.md) for details.
