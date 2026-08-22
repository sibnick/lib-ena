# ENA Hardware and Admin Queue Specification

## 1. PCI Register Space & BARs

The ENA device exposes Memory Mapped I/O (MMIO) BARs:

| BAR Index | Type | Purpose |
| :--- | :--- | :--- |
| **BAR0** | MMIO (64-bit) | Device control registers, AQ/ACQ/AENQ registers, Doorbell registers. |
| **BAR2** | MMIO (64-bit, Optional) | Low Latency Queue (LLQ) push memory region. |

### 1.1 Authoritative BAR0 Register Layout (from `reference/ena_regs_defs.h`)

| Register Name | Offset | Purpose |
| :--- | :--- | :--- |
| `ENA_REGS_VERSION_OFF` | `0x00` | Hardware version and capability bitmask. |
| `ENA_REGS_CONTROLLER_VERSION_OFF` | `0x04` | Controller version and implementation ID. |
| `ENA_REGS_CAPS_OFF` | `0x08` | Capabilities (DMA address width, timeouts). |
| `ENA_REGS_CAPS_EXT_OFF` | `0x0C` | Extended capabilities register. |
| `ENA_REGS_AQ_BASE_LO_OFF` | `0x10` | Low 32 bits of Admin SQ physical address. |
| `ENA_REGS_AQ_BASE_HI_OFF` | `0x14` | High 32 bits of Admin SQ physical address. |
| `ENA_REGS_AQ_CAPS_OFF` | `0x18` | Admin SQ depth and entry size. |
| `ENA_REGS_ACQ_BASE_LO_OFF` | `0x20` | Low 32 bits of Admin CQ physical address. |
| `ENA_REGS_ACQ_BASE_HI_OFF` | `0x24` | High 32 bits of Admin CQ physical address. |
| `ENA_REGS_ACQ_CAPS_OFF` | `0x28` | Admin CQ depth and entry size. |
| `ENA_REGS_AQ_DB_OFF` | `0x2C` | Admin SQ Doorbell register. |
| `ENA_REGS_ACQ_TAIL_OFF` | `0x30` | Admin CQ Tail pointer register. |
| `ENA_REGS_AENQ_CAPS_OFF` | `0x34` | AENQ depth and entry size. |
| `ENA_REGS_AENQ_BASE_LO_OFF` | `0x38` | Low 32 bits of AENQ physical address. |
| `ENA_REGS_AENQ_BASE_HI_OFF` | `0x3C` | High 32 bits of AENQ physical address. |
| `ENA_REGS_AENQ_HEAD_DB_OFF` | `0x40` | AENQ Head Doorbell register. |
| `ENA_REGS_AENQ_TAIL_OFF` | `0x44` | AENQ Tail pointer register. |
| `ENA_REGS_INTR_MASK_OFF` | `0x4C` | Interrupt mask register. |
| `ENA_REGS_DEV_CTL_OFF` | `0x54` | Device control (Reset trigger, AQ restart). |
| `ENA_REGS_DEV_STS_OFF` | `0x58` | Device status (Ready, Reset in progress). |
| `ENA_REGS_MMIO_REG_READ_OFF` | `0x5C` | Asynchronous MMIO register read request. |
| `ENA_REGS_MMIO_RESP_LO_OFF` | `0x60` | Low 32 bits of MMIO response address. |
| `ENA_REGS_MMIO_RESP_HI_OFF` | `0x64` | High 32 bits of MMIO response address. |
| `ENA_REGS_RSS_IND_ENTRY_UPDATE_OFF` | `0x68` | RSS indirection entry update register. |
| `ENA_REGS_PHC_DB_OFF` | `0x100` | PHC Doorbell register. |

---

## 2. Admin Queue Protocol

### 2.1 Admin Command Execution Sequence
1. Prepare `struct ena_admin_aq_entry` in the circular Admin Submission Queue (AQ).
2. Increment the local AQ tail index.
3. Write the tail index to `ENA_REGS_AQ_DB_OFF` (`0x2C`).
4. Poll the `phase` bit of `struct ena_admin_acq_entry` in Admin Completion Queue (ACQ).
5. Check return status in `acq_entry.status`.
6. Write updated consumer index to `ENA_REGS_ACQ_TAIL_OFF` (`0x30`).

### 2.2 Core Admin Commands
- `ENA_ADMIN_DEVICE_ATTRIBUTES` (`0x1`): Retrieves MAC address, max MTU, max queues, and capabilities.
- `ENA_ADMIN_CREATE_SQ` (`0x2`): Creates a TX or RX Submission Queue.
- `ENA_ADMIN_DESTROY_SQ` (`0x3`): Destroys a Submission Queue.
- `ENA_ADMIN_CREATE_CQ` (`0x4`): Creates a Completion Queue.
- `ENA_ADMIN_DESTROY_CQ` (`0x5`): Destroys a Completion Queue.
- `ENA_ADMIN_GET_FEATURE` (`0x6`): Reads feature parameters (RSS, MTU, offload capabilities).
- `ENA_ADMIN_SET_FEATURE` (`0x7`): Writes feature settings to the controller.

---

## 3. Asynchronous Event Notification Queue (AENQ)

- AENQ uses a host memory ring populated by the hardware.
- Each entry has a `phase` bit to detect new events.
- Groups of events:
  - `ENA_ADMIN_LINK_CHANGE`: Link state up / down notification.
  - `ENA_ADMIN_FATAL_ERROR`: Unrecoverable hardware or reset condition.
  - `ENA_ADMIN_WARNING`: Resource exhaustion or temperature alert.
- Driver acknowledges handled events via `ENA_REGS_AENQ_HEAD_DB_OFF` (`0x40`).
