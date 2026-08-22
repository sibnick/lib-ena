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
