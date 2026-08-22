# ENA Unikraft Driver: Roadmap and Milestones

## 1. Project Objective

Develop and validate a native AWS Elastic Network Adapter (ENA) driver in Unikraft OS.
The driver must integrate with `ukbus_pci` and `uknetdev` to provide high network performance on AWS EC2.

---

## 2. Milestones and Phases (10 Phases)

```mermaid
gantt
    title ENA Driver Development Timeline
    dateFormat  YYYY-MM-DD
    section Phase 1
    PCI Driver Scaffold & Device Discovery       :p1, 2026-09-01, 7d
    section Phase 2
    Admin Queue & AENQ Communication Subsystem   :p2, after p1, 10d
    section Phase 3
    Device Init & Capability Negotiation         :p3, after p2, 7d
    section Phase 4
    DMA Memory & Circular Ring Buffers           :p4, after p3, 7d
    section Phase 5
    Transmit (TX) Datapath Implementation        :p5, after p4, 10d
    section Phase 6
    Receive (RX) Datapath & Buffer Replenishment :p6, after p5, 10d
    section Phase 7
    Unikraft uknetdev Interface Integration      :p7, after p6, 7d
    section Phase 8
    Interrupt Handling & Polling Engine          :p8, after p7, 7d
    section Phase 9
    Low Latency Queue (LLQ) Optimization Mode    :p9, after p8, 7d
    section Phase 10
    EC2 Deployment & Performance Benchmarking    :p10, after p9, 10d
```

### Phase 1: PCI Driver Scaffold and Device Discovery
- Register ENA PCI Vendor ID (`0x1D0F`) and Device IDs (`0x0EC2`, `0xEC20`, `0x1EC2`) with `ukbus_pci`.
- Map BAR0 MMIO space and verify controller reset/status registers.
- Provide standalone mock MMIO test harness for offline unit testing.

### Phase 2: Admin Queue & AENQ Communication Subsystem
- Allocate DMA memory for Admin Submission (AQ) and Completion (ACQ) Queues.
- Implement synchronous command execution loop with timeouts and phase bit tracking.
- Implement AENQ listener for asynchronous device health events.

### Phase 3: Device Initialization and Capability Negotiation
- Read device attributes (`ENA_ADMIN_DEVICE_ATTRIBUTES`).
- Set host info and driver version.
- Negotiate MTU, queue limits, and MAC address.

### Phase 4: DMA Memory Management and Circular Ring Buffers
- Implement page-aligned circular DMA rings for IO SQ and CQ.
- Create tracking arrays for outstanding `uk_netbuf` packets and request IDs.

### Phase 5: Transmit (TX) Datapath Implementation
- Serialize `ena_eth_io_tx_desc` into TX SQ.
- Ring TX doorbells.
- Poll TX CQ completions and recycle transmitted `uk_netbuf` structures.
- Support L3/L4 checksum offload flags.

### Phase 6: Receive (RX) Datapath and Buffer Management
- Pre-allocate and populate RX SQ with `uk_netbuf` buffers.
- Poll RX CQ phase bits.
- Parse packet metadata (checksum status, packet length, hash) and hand off to `uknetdev`.
- Replenish consumed RX descriptors.

### Phase 7: Unikraft uknetdev Interface Integration
- Implement `struct uk_netdev_ops` callbacks (`dev_configure`, `rxq_configure`, `txq_configure`, `dev_start`, `dev_stop`, `rxq_recv`, `txq_xmit`).
- Register device instance with `uk_netdev_register`.

### Phase 8: Interrupt Handling and Polling Engine
- Implement MSI-X vector allocation for RX and AENQ.
- Implement polling worker thread mode for high throughput.

### Phase 9: Low Latency Queue (LLQ) Optimization Mode
- Detect BAR2 memory region.
- Implement direct MMIO push for TX descriptors and packet headers.

### Phase 10: Validation, EC2 Deployment, and Performance Benchmarking
- Validate on AWS EC2 instances (e.g., c5/c6i/m5).
- Benchmark throughput and latency using iperf3 and netperf.
- Store benchmark reports in Fossil unversioned store (`fossil uv`).
