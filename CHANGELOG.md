# Changelog

This file records what each release of `lib-ena` contains. Release names are
tags in this repository and follow semantic versioning. Each entry lists what
the release adds, the limits a user must know, and how to verify a checkout.

## 0.1.0 - 2026-10-11

First public release of `lib-ena`, a native AWS Elastic Network Adapter (ENA)
driver for Unikraft.

### Added

- **PCI bring-up.** Probe for vendor `0x1D0F` and devices `0x0EC2`, `0x1EC2`,
  `0xEC20`, and `0xEC21`. Maps the BAR0 register bank and the BAR2 Low Latency
  Queue region. Handles device reset with a bounded wait and a fatal-error read.
- **Admin queue.** Synchronous AQ and ACQ with command-id wrap handling, command
  timeout handling, and a shared path that resets the device and rebuilds the IO
  queues after a failure.
- **Asynchronous events (AENQ).** The driver drains the AENQ ring on every RX
  pass. A fatal error triggers a device reset and recovery. A link change updates
  the link state.
- **TX and RX rings.** Circular descriptor rings with phase-bit synchronisation,
  multiple queue pairs up to `CONFIG_LIBENA_MAX_QUEUES`, a request-id pool, and a
  TX bounce pool that applies backpressure instead of releasing live buffers.
- **Low Latency Queue (LLQ).** Direct MMIO push of descriptors and packet headers
  into BAR2, behind `CONFIG_LIBENA_LLQ`.
- **Hardware RSS.** Toeplitz hash over the TCP/IPv4 4-tuple. The driver reads the
  indirection table limits from the device, clamps its own table into that range,
  and checks the result with a readback.
- **libuknetdev adapter.** Implements `info_get`, `configure`, `rxq_configure`,
  `txq_configure`, `start`, `stop`, `rxq_recv`, and `txq_xmit`. Advertises
  `UK_NETDEV_F_LRO` and `UK_NETDEV_F_TSO4`.
- **Host test suite.** 14 programs that run the driver against a mock ENA device
  with fault injection. See [docs/testing.md](docs/testing.md).
- **Samples and tooling.** `samples/httpreply` (single core), `samples/httpreply-mc`
  (one worker per core with RSS steering), `samples/low-latency-hft`, and
  `samples/nginx`. Deploy, benchmark, and verification scripts for EC2.

### Known limits

- **Architecture:** x86_64 only. The driver reads PCI config space with port I/O
  (`0xCF8` and `0xCFC`). ARM64 and AWS Graviton need the ECAM path. That work is
  open, and [docs/roadmap.md](docs/roadmap.md) Phase 15 lists the order.
- **Interrupts:** partial. The default mode is software polling. The driver
  allocates and arms MSI-X vectors at probe when the platform provides them, but
  the pinned Unikraft platform exposes no interrupt delivery API for the runtime
  path.
- **Jumbo frames:** TX supports MTU up to 9000 bytes. RX offers one 2048-byte
  buffer per descriptor, so the driver drops received frames above 2048 bytes.
- **Multi-core samples** build against a pinned Unikraft revision with local
  patches. See `samples/*/patches/README.md`.

### Verified with

- `make test` (14 host programs), `make sanitize` (the same programs under
  AddressSanitizer and UndefinedBehaviourSanitizer), `make format-check`, and
  `make license-check`.
- Continuous integration: host tests, the sanitizer build, and two x86_64
  Unikraft builds in `.github/workflows/ci.yaml`.
- Real hardware on AWS `c6i.large`: probe of a Nitro ENA device, admin queue
  bring-up, AENQ groups enabled with keep-alive, six MSI-X vectors armed, two TX
  and two RX queue pairs created, RSS table readback, and HTTP responses with no
  errors. The short path for this check is
  `samples/httpreply-mc/scripts/run_ec2_verification.py --smoke`.

### Measured performance

Both datasets, the instance setup, and the reproduction steps are in the sample
README files.

- `httpreply` on one core, `c6i.large`: 53,404 requests per second at concurrency
  200, which is 12.4 percent above an Ubuntu 24.04 baseline in the same subnet.
- `httpreply-mc` with two cores and one queue pair each: 115,844 requests per
  second at concurrency 100 and 123,620 at concurrency 200, with no socket errors
  over 12.5 million requests. Nginx on the same instance type plateaus near
  72,000.
