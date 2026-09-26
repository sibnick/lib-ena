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
- **PCI Initialization & MMIO Mapping**: Registers PCI Vendor ID `0x1D0F` and five Device IDs (`0x0051` reserved, `0x0EC2` PF, `0x1EC2` LLQ PF, `0xEC20` VF, `0xEC21` LLQ VF). Maps BAR0 configuration and BAR2 Low Latency Queue (LLQ) regions.
- **Admin Queue Subsystem**: Synchronous Admin Queue (AQ/ACQ) for device feature negotiation, capability discovery, and queue creation.
- **AENQ Engine**: The driver polls the Asynchronous Event Notification Queue on every RX pass. A fatal error event resets the device. A link change event updates the link state.
- **Circular DMA Rings**: Zero-copy TX/RX ring buffers with phase-bit synchronization and wrap tracking.
- **Low Latency Queue (LLQ)**: Direct MMIO push of TX descriptors and packet headers into device BAR2 for reduced latency.
- **Hardware RSS**: Toeplitz hash over the TCP/IPv4 4-tuple steers incoming flows across per-core RX queues. The driver queries the device for the supported indirection table size range (`GET_FEATURE`) and clamps its own table into that range before configuration.
- **Interrupts**: The driver runs in software polling mode by default. It allocates MSI-X vectors at probe time when the platform provides them. The pinned Unikraft platform (>=0.17.0) exposes no interrupt delivery API, so full MSI-X interrupt runtime support is in progress.
- **Hardware Offloads & Jumbo Frames**: The netdev info advertises the `UK_NETDEV_F_LRO` and `UK_NETDEV_F_TSO4` offloads. The RX path reassembles multi-descriptor (LRO/jumbo) frames. MTU up to 9000 bytes works on TX. RX uses 2048-byte single-descriptor buffers, so the driver drops received frames longer than 2048 bytes.
- **uknetdev Integration**: Standard driver ops implementation (`info_get`, `configure`, `rxq_configure`, `txq_configure`, `start`, `stop`, `rxq_recv`, `txq_xmit`).

---

## 3. Validation and Benchmarking

The driver has measured benchmark results on AWS EC2 `c6i.large` instances. The benchmark application is `app-httpreply` with lwIP in single-threaded mode.

The measurements compare Unikraft with Ubuntu 24.04 in the same VPC subnet using the `wrk` HTTP benchmark tool. At concurrency 200, Unikraft reached 53,404 requests per second. This result exceeds Linux by 12.4 percent. The complete dataset and reproduction steps are in `samples/httpreply/README.md`.

A second benchmark application, `httpreply-mc`, pins one run-to-completion worker per CPU core and assigns each core a dedicated ENA queue pair with hardware RSS steering between them. Measured on a 2-vCPU `c6i.large` against an Ubuntu 24.04 `wrk` client across the private VPC, the multi-core server reached 115,844 requests per second at 100 concurrent connections and 123,620 requests per second at 200 concurrent connections, with zero socket errors over 12.5 million requests. Nginx on the same instance type plateaus near 72,000 requests per second, so the multi-core Unikraft build exceeds the Linux baseline by up to 72 percent. The complete dataset is in `samples/httpreply-mc/README.md`.

The standalone test suite (`make test`) runs against a mock ENA controller. It validates driver logic across 11 test runners (10 driver phases plus the SPSC ring suite) and 122 test cases in total.

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
CONFIG_LIBENA_RSS=y
```

---

## 5. Upstream Plan

1. Host the Git repository on GitHub at `https://github.com/sibnick/lib-ena`.
2. Propose repository migration or mirroring under the `unikraft` GitHub organization.
3. Submit catalog manifest PR to `unikraft/catalog` for indexing in KraftKit.
