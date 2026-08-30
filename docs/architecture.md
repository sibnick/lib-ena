# ENA Native Driver for Unikraft OS: Architecture Document

## 1. Overview

This document describes the architectural design of the native Amazon Elastic Network Adapter (ENA) driver for Unikraft OS.
The driver enables high-throughput and low-latency networking for Unikraft unikernels running on AWS EC2 instances.

The driver interacts directly with the Unikraft architecture:
- `ukbus_pci`: Discovers PCI devices and maps memory bars.
- `uknetdev`: Exposes network device abstractions to the network stack.
- `ukalloc` and page allocators: Allocates contiguous DMA memory for rings and packet buffers.

```
+-------------------------------------------------------------+
|                 Unikraft Network Stack (lwIP)               |
+-------------------------------------------------------------+
                              |
+-------------------------------------------------------------+
|                    libuknetdev Interface                    |
|       (uk_netdev, uk_netdev_ops, uk_netdev_event_handler)    |
+-------------------------------------------------------------+
                              |
+-------------------------------------------------------------+
|                  Unikraft ENA Native Driver                 |
|  +---------------------+  +-------------------------------+ |
|  |   Control Plane     |  |          Data Plane           | |
|  | - Admin Queue (AQ)  |  | - TX Ring (SQ/CQ)             | |
|  | - AENQ Poller       |  | - RX Ring (SQ/CQ)             | |
|  | - Feature Config    |  | - LLQ Push Mode (BAR2)        | |
|  +---------------------+  +-------------------------------+ |
+-------------------------------------------------------------+
          |                                       |
+----------------------+               +----------------------+
|     ukbus_pci        |               |   ukalloc / DMA Mem  |
+----------------------+               +----------------------+
          |                                       |
+-------------------------------------------------------------+
|                 AWS ENA Virtual Hardware (EC2)              |
+-------------------------------------------------------------+
```

---

## 2. Core Components

### 2.1 Driver Lifecycle & PCI Attachment
1. `ukbus_pci` probes the bus and matches the ENA PCI IDs:
   - Vendor ID: `0x1D0F` (Amazon)
   - Device IDs: `0x0EC2` (ENA PF), `0xEC20` (ENA VF), `0x1EC2` (ENA LLQ PF).
2. The driver initializes MMIO registers via BAR0.
3. The driver reads device capabilities and negotiates version compatibility.

### 2.2 Control Path: Admin Queue (AQ) and AENQ
- **Admin Queue (AQ)**:
  - Synchronous request-response ring for device management.
  - Used for configuring features, creating IO queues, and setting RSS.
- **Asynchronous Event Notification Queue (AENQ)**:
  - Device-to-host notification ring for out-of-band events and fatal errors.

### 2.3 Data Path: IO Queues
- **Submission Queue (SQ)**:
  - Contains transmit descriptors (TX) or empty receive buffers (RX).
- **Completion Queue (CQ)**:
  - Contains completion descriptors written by the ENA hardware after TX or RX completion.
- **LLQ Mode (Low Latency Queue)**:
  - Writes TX descriptors and packet headers directly into device BAR space.
  - Reduces latency by removing host-memory descriptor reads by device.

### 2.4 Unikraft `uknetdev` Integration
- Implements `struct uk_netdev_ops`:
  - `dev_configure`: Validates ring sizes and queue counts.
  - `rxq_configure` / `txq_configure`: Allocates hardware descriptors and helper buffers.
  - `dev_start`: Enables interrupts or polling threads and starts queues.
  - `dev_stop`: Stops datapath and flushes pending rings.
  - `rxq_recv` / `txq_xmit`: Fast-path packet intake and submission.

---

## 3. Memory & DMA Management

1. **DMA Contiguity**:
   - Descriptor rings require physically contiguous memory buffers.
   - Allocate ring memory with page alignment via `uk_palloc` / `ukalloc`.
2. **Cache Coherency & Barriers**:
   - Insert memory barriers (`uk_pci_dma_sync` / `rmb` / `wmb`) before ringing doorbells.
3. **Netbuf Mapping**:
   - Incoming and outgoing network buffers (`struct uk_netbuf`) map to physical addresses for ENA DMA.

---

## 4. Concurrency and Interrupt Models

- **Polling Mode**:
  - Main worker thread polls CQ rings for zero-copy high throughput.
- **Interrupt Mode**:
  - MSI-X vectors handle RX packet events and AENQ notifications.
  - Fallback to polling mode for minimal interrupt overhead under load.

---

## 5. Security Architecture & Threat Model

The driver treats the hardware device as an untrusted input source.
A faulty or compromised device can return invalid descriptor fields or register offsets.
The driver implements defensive controls across all control and data paths:

1. **Doorbell Bounds Validation**:
   - The driver validates doorbell offsets against BAR0 size before MMIO access.
   - The driver enforces 4-byte alignment on all MMIO offsets.

2. **RX Length Clamping**:
   - The driver validates RX packet completion lengths against buffer capacity.
   - Malformed packets exceeding buffer capacity are dropped immediately.

3. **In-Flight Request Tracking**:
   - Each ring tracks active request IDs with an in-flight bitmap.
   - The driver drops stale or duplicate completion IDs to prevent use-after-free conditions.

4. **DMA Address Isolation**:
   - The driver checks buffer physical addresses against safe memory regions.
   - Per-queue bounce buffers isolate memory when buffers reside outside DMA-safe ranges.

5. **Loop and Queue Bounds**:
   - Queue counts and depths are clamped to specification limits at configuration time.
   - Polling loops iterate only over configured queues.

See [docs/security_audit.md](security_audit.md) for the complete security audit report.

