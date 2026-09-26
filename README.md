# AWS ENA Native Driver for Unikraft

This repository contains the native AWS Elastic Network Adapter (ENA) driver for the Unikraft unikernel.

## Overview

The ENA driver provides high-performance networking for Unikraft unikernels running on Amazon EC2 instances. It interacts directly with the ENA PCI hardware without Linux kernel dependencies.

## Feature Matrix

| Feature | Status | Description |
| :--- | :--- | :--- |
| **PCI Probe & Reset** | Supported | Device identification, MMIO BAR0 mapping, and controller lifecycle. |
| **Admin Queue (AQ/ACQ)** | Supported | Synchronous device configuration and capability discovery. |
| **AENQ Engine** | Supported | The driver polls the AENQ ring on every RX pass. A fatal error resets the device. A link change updates the link state. |
| **TX / RX Rings** | Supported | Multi-queue circular descriptor rings with hardware checksum offload. |
| **Low Latency Queue (LLQ)** | Supported | Direct push of packet headers and descriptors to BAR2 MMIO. |
| **RSS (Receive Side Scaling)** | Supported | Hardware Toeplitz hash on the TCP/IPv4 4-tuple steers incoming flows across per-core RX queues. The driver clamps the indirection table size to the range the device reports. |
| **LRO / TSO** | Advertised | The netdev info advertises the `UK_NETDEV_F_LRO` and `UK_NETDEV_F_TSO4` offloads to the stack. The RX path reassembles multi-descriptor (LRO/jumbo) frames. |
| **Interrupts / MSI-X** | Partial | The default mode is software polling. The driver allocates MSI-X vectors at probe time when the platform provides them. |
| **Jumbo Frames** | Partial | TX supports MTU up to 9000 bytes. RX offers one 2048-byte buffer per descriptor. The driver drops received frames longer than 2048 bytes. |

## Supported EC2 Instance Types

The driver supports x86_64 AWS EC2 instance types equipped with ENA hardware:

- **General Purpose**: `t3`, `m5`, `m6i`, `m6a`, `m7i`, `m7a`
- **Compute Optimized**: `c5`, `c6i`, `c6a`, `c7i`
- **Memory Optimized**: `r5`, `r6i`, `r6a`, `r7i`

ARM64 (Graviton) instances are not supported at this time. The driver reads
PCI config space with x86 port I/O (0xCF8/0xCFC) and cannot build for
ARM64. Porting config space access to the UK ECAM API (available on aarch64
since UK 0.19) is future work.

## Kconfig Configuration Options

The driver exposes the following Kconfig options in `Config.uk`:

- `CONFIG_LIBENA`: Enable the AWS ENA native network driver.
- `CONFIG_LIBENA_LLQ`: Enable Low Latency Queue (LLQ) direct MMIO push mode (default: `y`).
- `CONFIG_LIBENA_MAX_QUEUES`: Maximum number of IO queue pairs per device (default: `8`).
- `CONFIG_LIBENA_RSS`: Enable hardware RSS steering of incoming flows across RX queues (default: `y`).

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

## Samples and Examples

This repository includes working samples and test applications:

- **[samples/httpreply](samples/httpreply/)**: High-performance Unikraft HTTP reply benchmark server (`app-httpreply`). Uses single-threaded lwIP sockets (`NO_SYS` mode, `epoll`) and native ENA networking on AWS EC2. Includes automated `wrk` benchmark scripts and AWS deployment tools.
- **[samples/httpreply-mc](samples/httpreply-mc/)**: Multi-core HTTP reply benchmark server. Pins one run-to-completion worker per CPU core. Each core owns a dedicated ENA queue pair, hardware RSS (Toeplitz 4-tuple hash) steers flows between the queues, and sockets bind with `SO_REUSEPORT`. On a 2-vCPU `c6i.large` it sustains 123,620 requests per second at 200 concurrent connections with zero socket errors.
- **[samples/low-latency-hft](samples/low-latency-hft/)**: Low-latency UDP echo server and latency benchmarking client in C. Demonstrates zero-copy datagram echo loops, socket buffer tuning, and CPU core pinning. Runs on Linux and as a Unikraft unikernel with the native ENA driver.
- **[examples/ci-app](examples/ci-app/)**: A minimal Unikraft application stub for CI build verification.

## AWS EC2 Deployment

Deploy Unikraft images with ENA support to Amazon EC2:

1. Build the KVM image with KraftKit:
   ```bash
   kraft build --target kvm --plat qemu --arch x86_64
   ```
2. Convert the image to a raw disk and upload it to Amazon S3.
3. Import the snapshot and register an AMI with the `--ena-support` flag.
4. Launch an ENA-enabled instance (such as `t3.nano` or `c6i.large`).

See [docs/ec2_deployment.md](docs/ec2_deployment.md) for complete deployment guidelines.

## Security Considerations and Audit

The driver operates under a strict threat model where hardware device input is untrusted:

- **MMIO Boundary Checks**: Validates doorbell offsets against BAR0 boundaries and 4-byte alignment before MMIO access.
- **Buffer Safety**: Checks RX completion lengths against allocated buffer capacity to prevent heap overflow.
- **In-Flight Request Tracking**: Tracks active request IDs to stop use-after-free and duplicate descriptor recycling.
- **DMA Isolation**: Uses per-queue bounce buffers for non-DMA-safe memory addresses.
- **Bounded Iteration**: Clamps queue counts and depths to device and specification limits.

All 18 security audit findings are resolved. See [docs/security_audit.md](docs/security_audit.md) for full audit records.

## Performance Benchmarking

Benchmark measurements for the HTTP reply server on AWS EC2 `c6i.large` instances are documented in [samples/httpreply/README.md](samples/httpreply/README.md).

Store raw benchmark reports outside version control, for example in the Fossil unversioned store (`fossil uv`). Do not store unmeasured numbers in this repository.

## License

This project is licensed under the BSD-3-Clause License. See [COPYING.md](COPYING.md) for details.

