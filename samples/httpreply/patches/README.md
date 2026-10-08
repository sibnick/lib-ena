# Submodule Patches (`httpreply`)

This directory holds local patches for the submodules that the single-core HTTP
reply sample builds against. Each entry below describes the problem a patch
solves and the approach it uses. The multi-core sample keeps a larger set in
[samples/httpreply-mc/patches/README.md](../../httpreply-mc/patches/README.md).

## 1. How These Patches Work

| File | Submodule | Base commit | Size |
| :--- | :--- | :--- | ---: |
| `lib-lwip-ec55ae17.patch` | `.libs/lib-lwip` | lwIP `ec55ae17` | 1226 lines |
| `unikraft-eb8fa236.patch` | `.unikraft/unikraft` | unikraft `eb8fa236` | 413 lines |

The file name encodes the submodule base commit. `scripts/apply_patches.sh`
maps the prefix to a submodule path, skips a patch that is already applied,
applies a patch that fits, and stops with an error on a conflict. The build runs
this script before compilation.

Regenerate a patch after you change a submodule checkout:

```bash
git -C <submodule> diff > patches/<name>-$(git -C <submodule> rev-parse --short=8 HEAD).patch
```

## 2. `lib-lwip-ec55ae17.patch` (lwIP Port)

The patch turns the lwIP port into a single-threaded stack that still offers a
POSIX socket API and drives its own network interface. Five groups make up the
patch.

### 2.1 Socket API in `NO_SYS` Mode

**Problem.** lwIP refuses the build. It stops with an `#error` when `LWIP_SOCKET`
or `LWIP_NETCONN` meets `NO_SYS=1`. This sample needs sockets and `epoll`, but it
must not run a stack thread. `lwip/sys.h` also replaces the semaphore, mutex, and
mailbox types with stubs under `NO_SYS`, so the netconn code cannot compile.

**Approach.** A nested patch, `patches/0015-nosys-sequential-api.patch`, edits
upstream lwIP:

- Removes the `#error` that blocks the sequential API under `NO_SYS`.
- Keeps the real `sys_*` declarations in both modes instead of the stub block.
- Runs `tcpip_api_call()`, `tcpip_callback()`, `tcpip_try_callback()`, and
  `tcpip_send_msg_wait_sem()` inline in the caller context. No stack thread
  exists, so no hand-off is needed.
- Empties `TCPIP_APIMSG_ACK()` under `NO_SYS`, and builds `err_to_errno()` when
  `LWIP_SOCKET` is on.

The port side of this group:

- `Config.uk`: `LWIP_SOCKET` no longer depends on `LWIP_THREADS`. It selects
  `LIBUKLOCK`, `LIBUKLOCK_SEMAPHORE`, `LIBUKLOCK_MUTEX`, `LIBUKMPI`, and
  `LIBUKMPI_MBOX`.
- `Makefile.uk`: builds `mutex.c`, `semaphore.c`, and `mailbox.c` in every mode,
  not only under `CONFIG_LWIP_THREADS`.
- `lwipopts.h`: sets `LWIP_NETCONN 1` in mainloop mode when the Socket API is
  enabled. Sets `SYS_LIGHTWEIGHT_PROT 0`, because one thread holds the stack.
  Raises mailbox sizes to cover a full receive window and the listen backlog:
  `TCPIP_MBOX_SIZE 128`, `DEFAULT_TCP_RECVMBOX_SIZE 256`,
  `DEFAULT_ACCEPTMBOX_SIZE 1024`. Adds `SO_REUSEPORT 15`.
- `mailbox.c`, `semaphore.c`: add `CONFIG_LWIP_NOTHREADS` variants of
  `sys_arch_mbox_fetch()` and `sys_arch_sem_wait()`. No other context can post,
  so they try once and return `SYS_ARCH_TIMEOUT`.

### 2.2 Per-Core Stack State

**Problem.** lwIP keeps PCB lists, timer heads, memp free lists, the ARP cache,
and the socket table in file-scope globals. Two cores that run `tcp_input()` at
the same time corrupt those lists.

**Approach.** New `include/lwip_percore.h` and `percore.c` define
`struct lwip_core_state` and the array `uk_lwip_core_states[LWIP_CORE_MAX]`. The
index comes from `uk_pcpuvar_cpu_idx`. A second nested patch,
`patches/0016-per-core-stack-state.patch`, redefines the lwIP globals as macros
into that state:

- `tcp_priv.h`, `tcp.c`: PCB list heads, `tcp_ticks`, and timer counters.
- `timeouts.c`: the timeout list and due-time counters.
- `etharp.c`: the ARP cache and the cached entry index.
- `memp.c`: per-core free-list heads in `memp_tabs[]`. The pool is split evenly
  across cores at init.
- `api/sockets.c`: a per-core socket table. A socket fd encodes its owner:
  `core * NUM_SOCKETS + idx + LWIP_SOCKET_OFFSET`.

`Config.uk` adds `CONFIG_LWIP_PERCORE`, default y under `LWIP_NOTHREADS`.
`init.c` calls `lwip_core_state_init_all()` before lwIP init. This sample runs on
 one core, so the extra state costs nothing here. The multi-core sample needs it.

### 2.3 Link State and Gratuitous ARP

**Problem.** The port set `NETIF_FLAG_LINK_UP` at attach time. On EC2 the ENA
link comes up after a device reset or an AENQ link change, and lwIP kept sending
into a device that was down. Separately, a reused private IP left stale ARP
entries on other hosts, so traffic went to an old MAC address.

**Approach.** `uknetdev.c` queries the driver through
`uk_netdev_link_state_get()` (see 3.4). `uknetdev_poll()` calls
`netif_set_link_up()` or `netif_set_link_down()` when the state changes, and
`uknetdev_init()` sets the initial flag from the device. A new
`uknetdev_send_gratuitous_arp()` calls `etharp_gratuitous()` once per address,
when the interface comes up or the address changes. The last announced address
lives in `struct lwip_netdev_data` in the netdev scratch pad.

### 2.4 On-Demand Socket Polling

**Problem.** With `CONFIG_LIBPOSIX_SOCKET_POLLED`, the file descriptor queue
stores event levels that callbacks push. A stored level goes stale, and `epoll`
reports a readiness that no longer exists.

**Approach.** `sockets.c` adds `lwip_posix_socket_poll()`. It reads the live
lwIP socket state through `get_lwip_socket_events()` and returns the masked
result. The port registers it as the `.poll` operation, so the queue stores no
levels of its own.

## 3. `unikraft-eb8fa236.patch` (Unikraft Core)

This patch adds the interrupt and link-state primitives that the ENA driver and
the lwIP port need. It touches no scheduler or SMP code.

### 3.1 MSI-X Vector Allocation for xpic

**Problem.** The ENA driver needs one MSI-X vector per queue, each targeted at a
chosen vCPU. The x86 interrupt controller (`xpic`) exposes only a raw irq pool and
no MSI message builder. The pool range 16..224 also overlaps the IPI vectors
48..51, so an allocated vector could collide with an IPI.

**Approach.** New `drivers/ukintctlr/xpic/msix.c` and
`include/uk/intctlr/msix.h` export two functions:

- `uk_intctlr_msix_alloc(lcpu, &irq, &addr, &data)` takes one irq from the pool,
  computes the hardware vector as `32 + irq`, and builds the MSI message. The
  address is the xAPIC MMIO base `0xFEE00000` with the destination APIC ID in the
  top byte, bits 55:48. The APIC ID comes from `uk_pcpuvar_cpu_id`, and falls
  back to the vCPU index, which KVM and QEMU use as the APIC ID.
- `uk_intctlr_msix_free(irq)` returns the irq to the pool.

`include/uk/intctlr/limits.h` moves the allocatable pool to irq 20..222. That
keeps vectors 48..51 free for IPIs and keeps the spurious vector 255 out of the
pool. It also fixes an off-by-one in `UK_INTCTLR_ALLOCABLE_IRQ_COUNT`.
`Makefile.uk` builds `msix.c` under `CONFIG_LIBUKINTCTLR_APIC`, and
`exportsyms.uk` exports both functions.

### 3.2 Force xAPIC (MMIO) Mode

**Problem.** Upstream `xpic` switches the local APIC to x2APIC when CPUID reports
support. On this hypervisor the x2APIC TSC-deadline timer is broken, and a core
that executes `hlt` never wakes. The upstream path also leaves the
spurious-interrupt vector unconfigured.

**Approach.** `ukintctlr.c` replaces `x2apic_enable()` with `xapic_enable()`. It
reads the APIC base MSR, records the MMIO base, sets the enable bit, clears the
`EXTD` bit, and writes the SVR at offset `0xF0` with the software-enable bit and
spurious vector `0xFF`. EOI now writes the MMIO register at base + `0x0B0`.

### 3.3 Guard the 8259 Against High IRQ Numbers

**Problem.** `pic_mask_irq()` and `pic_clear_irq()` compute an 8259 port for any
irq number. The 8259 decodes only irq 0..15, so higher numbers wrote to unrelated
ports.

**Approach.** `pic.c` returns early when `irq >= 16`. Such irqs are APIC-only,
and their masking lives in the device PBA.

### 3.4 Link State Query in uknetdev

**Problem.** The lwIP port needs to know whether the device link is up. No driver
operation existed.

**Approach.** `lib/uknetdev/include/uk/netdev_core.h` adds an optional
`link_state_get` operation to `struct uk_netdev_ops`.
`include/uk/netdev.h` adds `uk_netdev_link_state_get()`, which returns 1 when a
driver does not implement the op.

### 3.5 Drop the TLS Program Headers

**Problem.** The KVM linker script asks the host linker for one program header per
section group. The TLS group has no content, so the linker emits an empty
`PT_LOAD` at the data segment address. GRUB reads load chunks in address order
and rejects two chunks that share an address: `error: overlap detected`.

**Approach.** `plat/kvm/x86/link64.lds.S` removes the `tls PT_TLS` and
`tls_load PT_LOAD` entries from `PHDRS`. The unikernel does not read its own
program header table, so the removal is safe. The nginx sample solves the same
problem after the link with `scripts/elf_drop_empty_segments.py`.

## 4. Related Records

| Ticket | Subject |
| :--- | :--- |
| `ae97b87da5` | MSI-X capability discovery and arm path |
| `aa0fc35743` | one-shot wake misfires after the xAPIC-forcing change |

Both tickets are closed. Read the full history with
`fossil ticket history <uuid>`.
