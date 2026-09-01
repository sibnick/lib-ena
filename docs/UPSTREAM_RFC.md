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
- **AENQ Engine**: The driver polls the Asynchronous Event Notification Queue on every RX pass. A fatal error event resets the device. A link change event updates the link state.
- **Circular DMA Rings**: Zero-copy TX/RX ring buffers with phase-bit synchronization and wrap tracking.
- **Low Latency Queue (LLQ)**: Direct MMIO push of TX descriptors and packet headers into device BAR2 for reduced latency.
- **Interrupts**: The driver runs in software polling mode by default. It allocates MSI-X vectors at probe time when the platform provides them. The pinned Unikraft platform (>=0.17.0) exposes no interrupt delivery API, so full MSI-X interrupt runtime support is in progress.
- **Hardware Offloads & Jumbo Frames**: Supports hardware IPv4 checksum offload. MTU up to 9000 bytes works on TX. RX uses 2048-byte single-descriptor buffers, so the driver drops received frames longer than 2048 bytes.
- **uknetdev Integration**: Standard driver ops implementation (`rxq_recv`, `txq_xmit`, `info_get`, `configure`, `rxq_configure`, `txq_configure`).

---

## 3. Validation and Benchmarking

No published benchmark results. The driver has not been measured on real EC2 hardware in this repository.

The standalone test suite (`make test`) runs against a mock ENA controller. It checks driver logic. It is not a hardware validation.

A measurement method is described in [scripts/ec2_benchmark.sh](scripts/ec2_benchmark.sh). Store real measurements outside version control, for example in the Fossil unversioned store (`fossil uv`).

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
