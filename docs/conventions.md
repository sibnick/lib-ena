# Code Style and Project Conventions

## 1. Language and Compiler Standard
- **Standard**: C99 (`-std=c99` or `-std=gnu99`).
- **Target Compilers**: GCC 10+ and Clang 12+.
- **Warnings**: `-Wall -Wextra -Werror -Wno-unused-parameter -pedantic`.

---

## 2. File Organization and Directory Layout

```
Unikraft-ENA/
├── Config.uk             # Unikraft KConfig menu and options
├── Makefile.uk           # Unikraft build system rules
├── Makefile              # Standalone host test and build runner
├── AGENTS.md             # OpenCode agent instructions
├── docs/                 # Architecture and specifications
├── reference/            # Authoritative Linux kernel ENA headers
├── include/              # Public driver headers
│   ├── ena.h             # Master driver declarations
│   ├── ena_regs.h        # MMIO register definitions and accessors
│   ├── ena_admin.h       # Admin Queue structures and commands
│   ├── ena_datapath.h    # TX/RX ring structures and descriptor types
│   └── ena_plat.h        # Platform abstraction layer (Unikraft vs Mock)
├── src/                  # Implementation source files
│   ├── ena_pci.c         # PCI driver probe, BAR mapping, and cleanup
│   ├── ena_com.c         # Admin Queue and AENQ engine
│   ├── ena_netdev.c      # libuknetdev adapter operations
│   ├── ena_tx.c          # Transmit ring processing and doorbell push
│   └── ena_rx.c          # Receive ring replenishment and packet intake
└── tests/                # Standalone unit test suite
    ├── mock_pci.h        # Mock PCI device and BAR MMIO simulation
    ├── mock_pci.c        # Simulated ENA hardware register bank
    └── test_pci_scaffold.c # Phase 1 verification tests
```

---

## 3. Coding Style Rules

1. **License Header**: Every `.c` and `.h` file must start with the standard BSD-3-Clause license header:
```c
/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Unikraft ENA Driver Maintainers
 * Copyright (c) 2026, Unikraft ENA Contributors. All rights reserved.
 */
```

2. **Types**: Use standard fixed-width integer types from `<stdint.h>` (`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`).
3. **Hardware Endianness & Memory Access**:
   - MMIO reads/writes must use platform barrier accessors (`ena_reg_read32`, `ena_reg_write32`).
   - Hardware descriptors must use little-endian byte ordering.
4. **Error Handling**: Return standard negative errno values (`-ENODEV`, `-ENOMEM`, `-EINVAL`, `-ETIMEDOUT`, `-EIO`).
5. **Naming Conventions**:
   - Functions: `ena_<subsystem>_<action>()` (e.g., `ena_pci_probe()`, `ena_aq_submit_cmd()`).
   - Structures: `struct ena_<name>` (e.g., `struct ena_adapter`, `struct ena_ring`).
   - Macros & Enums: `ENA_<NAME>` (e.g., `ENA_REG_DEV_STS`, `ENA_STATE_READY`).
6. **No Dynamic Memory in Fast-Path**: Ring descriptors and buffer pointer tables must be pre-allocated during queue setup.
