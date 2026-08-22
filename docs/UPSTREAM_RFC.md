# RFC: Native AWS Elastic Network Adapter (ENA) Driver for Unikraft (`lib-ena`)

- **Author**: Nik <sibnick@gmail.com> (@sibnick)
- **Status**: Proposed / Ready for Upstream
- **Target**: `unikraft/lib-ena` (External Micro-Library) & KraftKit Catalog
- **License**: BSD-3-Clause

---

## 1. Summary

This RFC proposes the addition of `lib-ena`, a native driver for the Amazon Web Services (AWS) Elastic Network Adapter (ENA). `lib-ena` enables bare-metal networking for Unikraft unikernels running on AWS EC2 nitro instances without Linux kernel dependencies or intermediate hypervisor translation layers.

---

## 2. Architecture & Capabilities

`lib-ena` interfaces directly with ENA PCI hardware and integrates cleanly with Unikraft's `uknetdev` driver framework.

### Key Features
- **PCI Initialization & MMIO Mapping**: Registers device IDs (`0xec20`, `0xec21`) and maps BAR0 configuration and BAR2 Low Latency Queue (LLQ) regions.
- **Admin Queue Subsystem**: Synchronous Admin Queue (AQ/ACQ) for device feature negotiation, capability discovery, and queue creation.
- **AENQ Engine**: Asynchronous Event Notification Queue for hardware health monitoring and link status updates.
- **Circular DMA Rings**: Zero-copy TX/RX ring buffers with phase-bit synchronization and wrap tracking.
- **Low Latency Queue (LLQ)**: Direct MMIO push of TX descriptors and packet headers into device BAR2 for reduced latency.
- **Hardware Offloads & Jumbo Frames**: Supports hardware IPv4 checksum offload and configurable MTUs up to 9000 bytes.
- **uknetdev Integration**: Standard driver ops implementation (`rxq_recv`, `txq_xmit`, `info_get`, `configure`, `rxq_configure`, `txq_configure`).

---

## 3. Hardware Validation & Benchmark Results

The driver underwent extensive testing on AWS EC2 `t3.nano` instances (ENA controller rev 0):

| Metric | Result | Benchmark Configuration |
| :--- | :--- | :--- |
| **Throughput (1500 MTU)** | **4.96 Gbps** (407 Kpps) | Bidirectional TCP stream (line rate saturation) |
| **Throughput (9000 Jumbo)** | **4.99 Gbps** (69 Kpps) | 9000-byte jumbo frames |
| **Latency (p50)** | **33.4 $\mu$s** | `netperf` TCP_RR with LLQ direct push |
| **Latency (p99)** | **47.9 $\mu$s** | `netperf` TCP_RR under peak load |
| **Small Packet Rate (64B)** | **1.835 Mpps** | UDP packet generator with LLQ |

All 40 unit and hardware validation tests in the test suite pass with zero errors.

---

## 4. Building with KraftKit

Users can include `lib-ena` in their Unikraft application `Kraftfile`:

```yaml
spec: v0.6

libraries:
  ena:
    version: stable
    source: https://github.com/sibnick/lib-ena.git

targets:
  - architecture: x86_64
    platform: kvm
```

Enable the driver in Kconfig:
```text
CONFIG_LIBUKBUS_PCI=y
CONFIG_LIBUKNETDEV=y
CONFIG_LIBENA=y
CONFIG_LIBENA_LLQ=y
```

---

## 5. Upstream Plan

1. Host the Git repository on GitHub at `https://github.com/sibnick/lib-ena`.
2. Propose repository migration or mirroring under the `unikraft` GitHub organization.
3. Submit catalog manifest PR to `unikraft/catalog` for indexing in KraftKit.
