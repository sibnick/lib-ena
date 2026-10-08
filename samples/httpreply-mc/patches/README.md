# Submodule Patches (`httpreply-mc`)

This directory holds local patches for the submodules that the multi-core HTTP
reply sample builds against. Each entry below describes the problem a patch
solves and the approach it uses. The single-core sample keeps a smaller set in
[samples/httpreply/patches/README.md](../../httpreply/patches/README.md). Read
that file first for the groups that both samples share.

## 1. How These Patches Work

| File | Submodule | Base commit | Size |
| :--- | :--- | :--- | ---: |
| `lib-lwip-ec55ae17.patch` | `.libs/lib-lwip` | lwIP `ec55ae17` | 2664 lines |
| `unikraft-e31b2c44.patch` | `.unikraft/unikraft` | unikraft `e31b2c44` | 2494 lines |

The file name encodes the submodule base commit. `scripts/apply_patches.sh` maps
the prefix to a submodule path, skips a patch that is already applied, applies a
patch that fits, and stops with an error on a conflict. The build runs this
script before compilation.

Regenerate a patch after you change a submodule checkout:

```bash
git -C <submodule> diff > patches/<name>-$(git -C <submodule> rev-parse --short=8 HEAD).patch
```

## 2. `lib-lwip-ec55ae17.patch` (lwIP Port)

### 2.1 Groups Shared with the Single-Core Sample

These five groups are identical in both patches. Section 2 of the single-core
file describes each problem and approach.

| Group | What it does |
| :--- | :--- |
| Socket API in `NO_SYS` mode | Builds netconn and the Socket API with `NO_SYS=1`, and runs every `tcpip_*` call inline. Nested `patches/0015-nosys-sequential-api.patch`. |
| Per-core stack state | Moves PCB lists, timers, memp pools, the ARP cache, and the socket table into `struct lwip_core_state`. Nested `patches/0016-per-core-stack-state.patch`. |
| Link state and gratuitous ARP | Follows the driver link state, and announces the IPv4 address once per change. |
| On-demand socket polling | Reports `epoll` events from live lwIP state instead of a stored level. |

### 2.2 Per-Core Timer Pool Sizing for Cyclic Timers

**Problem.** The per-core split divides every memp pool by `LWIP_CORE_MAX`.
`lwip_init()` calls `sys_timeouts_init()`, which registers all enabled cyclic
timers on the boot core at once. The lwIP default `MEMP_NUM_SYS_TIMEOUT` sizes one
global pool, so one core runs out of slots at boot and crashes with
`pool MEMP_SYS_TIMEOUT is empty`.

**Approach.** `include/lwipopts.h` sets `MEMP_NUM_SYS_TIMEOUT` to
`16 * CONFIG_UKPLAT_CPU_MAXCOUNT` when `LWIP_PERCORE` is on. The value covers the
cyclic timers plus the on-demand TCP and DHCP timers.

### 2.3 Per-Core `ip_data` and TCP Input State

**Problem.** Two more global blocks of lwIP state stayed outside the first per-core patch:

- `ip.c` keeps `ip_data`, which holds the source address and netif choice for the
  packet in flight. Two cores overwrite each other's value.
- `tcp_in.c` keeps the parsed header state (`inseg`, `tcphdr`, `seqno`, `ackno`,
  `recv_flags`, `recv_data`) in file-scope statics. Concurrent `tcp_input()`
  calls destroy each other's parse.

**Approach.** Nested patch 0016 now also moves these:

- `ip.c`, `ip.h`: `struct ip_globals uk_lwip_ip_data[LWIP_CORE_MAX]`, with
  `#define ip_data (uk_lwip_ip_data[lwip_current_core_id()])`.
- `tcp_in.c`: a `static struct tcp_in_state tcp_in_states[LWIP_CORE_MAX]` and a
  `tcp_in_curr` pointer for the running core. The patch renames the fields inside
  the file (`tcphdr_in`, `tcp_seqno`, `tcp_ackno`, `tcp_flags`) so no name clashes with
  the macros that per-core state needs.

### 2.5 Multi-Queue NIC Polling and Transmit

**Problem.** The port polls RX queue 0, allocates netbufs from one shared
allocator, and transmits on TX queue 0. The run-to-completion design gives each
core its own ENA queue pair, so this sharing one queue per device cannot work.

**Approach.**

- `include/netif/uknetdev.h`: new public API `uknetdev_poll_queue(nf, queue_id)`
  and `uknetdev_poll_rxqueue(queue_id)`. `uknetdev_poll()` and
  `uknetdev_poll_all()` stay as wrappers for queue 0.
- `uknetdev.c`: `uknetdev_input()` passes a real queue id to `uk_netdev_rx_one()`.
  New statics `uknetdev_nb_rxq`, `uknetdev_nb_txq`, and `uknetdev_txq_rr` record
  the queue counts.
- `uknetdev_output()`: under `NO_SYS` plus `LWIP_PERCORE` it transmits on queue
  `lwip_current_core_id() % uknetdev_nb_txq`. Other modes round-robin over
  `uknetdev_txq_rr`.
- `netif_alloc_rxpkts()` and `uknetdev_output()` allocate netbufs from
  `uk_alloc_get_current()`, the per-core heap, instead of one shared allocator.
- `uknetdev_init()`: requests `CONFIG_LIBUKNETDEV_MAXNBQUEUES` queue pairs, caps
  them at `dev_info.max_rx_queues` and `max_tx_queues`, and configures every RX and
  TX queue in a loop.
- New `uknetdev_lcpu_sched(idx)` walks `uk_sched_head` to find the scheduler that
  owns vCPU idx, so each RX dispatcher thread binds to its own core.
- `uknetdev_updown()`: enables or disables RX interrupts on every queue when
  `CONFIG_LIBUKNETDEV_DISPATCHERTHREADS` is on.

### 2.6 EPOLLOUT from the Live Send Buffer

**Problem.** The netconn layer sets `sendevent` on a low-water edge and clears it
on the opposite edge. A small write that never fills the send buffer leaves the
latch at 1, so `epoll` reports the socket as writable forever.

**Approach.** `sockets.c` computes writability for TCP sockets from live state:
`tcp_sndbuf(pcb) > 0` and `tcp_sndqueuelen(pcb)` below the `tcp_write()` limit.
UDP and raw sockets have no flow control, so they keep the `sendevent` flag.

## 3. `unikraft-e31b2c44.patch` (Unikraft Core)

### 3.1 Groups Shared with the Single-Core Sample

Sections 3.1 to 3.5 of the single-core file describe these. The multi-core patch
carries the same work: MSI-X vector allocation for xpic, forced xAPIC MMIO mode,
8259 guards for high irq numbers, and the `link_state_get` driver operation in uknetdev.
The multi-core version also adds an optional `stop` operation to
`struct uk_netdev_ops`.

### 3.2 SMP Bring-Up on KVM x86_64

**Problem.** The KVM platform has no path for a secondary CPU. It selects the
native platform LCPU power-management operations, which do not fit KVM. Application
processors never enter the kernel, so an SMP workloads run on one core.

**Approach.** New `plat/kvm/x86/lcpu.c` (350 lines):

- `apic_mode_detect()` reads the APIC base MSR and picks xAPIC MMIO or x2APIC MSR
  access for ICR writes.
- `boot_memregion_alloc_sipi_vect()` reserves a buffer below `0xA0000`, because the
  startup trampoline must live in the first 1 MiB. The image itself links at 1 MiB.
- `start16_reloc_mp_init()` copies the real-mode trampoline from `lcpu_start.S`
  and fixes its internal references (`gdt32_ptr`, `gdt32`, `lcpu_start16`,
  `jump_to32`, `lcpu_start32`).
- `kvm_x86_lcpu_start()` sends an INIT IPI. `kvm_x86_lcpu_post_start()` waits
  10 ms and sends two SIPIs per AP with 200 µs gaps.
- `kvm_x86_lcpu_halt()` and `kvm_x86_lcpu_halt_irq()` implement idle halt with the
  `sti; hlt; cli` no-gap idiom. `UK_BOOT_EARLYTAB_ENTRY` registers these as
  `kvm_x86_pm_ops`.

Supporting changes:

- `plat/kvm/Config.uk`: stops selecting `LIBUKPLAT_NATIVE_LCPU_PM` for x86_64 SMP,
  so the new operations take over.
- `plat/kvm/Makefile.uk`: builds `x86/lcpu.c` only under `CONFIG_HAVE_SMP`.
- `plat/kvm/x86/lcpu_start.S`: an AP enters with `RSP = 0`. The code scans the
  pcpuvar slots up to `CONFIG_UKPLAT_CPU_MAXCOUNT`, matches its APIC ID against
  `uk_pcpuvar_cpu_id`, and sets `RSP` to that slot. An unmatched CPU halts with a
  message.
- `drivers/firmware/ukacpi/arch/x86_64/madt.c`: reads the BSP CPU ID from
  `CPUID(1).EBX` instead of an unset pcpuvar, and fills the CPU id map before any
  AP starts. The scan in `lcpu_start.S` depends on that map.
- `lib/uksched/exportsyms.uk`: exports `uk_sched_head`, which the lwIP port uses to
  bind a queue to a core.

### 3.3 Per-Core Heap Partitions

**Problem.** One shared heap serves all cores. Concurrent `bbuddy_palloc()` calls
corrupted the free lists (Ticket `d4c2a6bd2a`), and a TX completion freed a netbuf
on a different core from the one that allocated it (Ticket `f47bdd0ed1`).

**Approach.** New Kconfig `CONFIG_LIBUKBOOT_PERCORE_HEAP`. `percore_heap_init()`
in `lib/ukboot/boot.c` finds the largest free memregion and carves N-1
page-aligned partitions before the default allocator claims memory. It exposes them
through `uk_percore_heap_count()` and `uk_percore_heap_get(i, &base, &len)`. Each
core then builds its own bbuddy instance on its partition. The default heap becomes
`uk_allocbbuddy_init_shared()`, which is lock-protected.

### 3.4 Per-Core Allocator Binding and Buddy Safety

**Problem.** `uk_alloc_get_default()` returns one global instance, so every
allocation goes to the shared heap. Corruption from a bad free also went
unnoticed: the upstream free-list sanity test `(*c->link.pprev)->next != &c->link`
is always true for a valid hlist, so it never reported nothing.

**Approach.**

- `lib/ukalloc`: adds the per-CPU slot `uk_pcpuvar_percore_alloc`, the accessor
  `uk_alloc_get_current()`, and makes `uk_alloc_get_default()` prefer that slot.
- `lib/ukallocbbuddy`: splits `bbuddy_palloc()`, `bbuddy_pfree()`, and
  `bbuddy_addmem()` into `_nolock` cores plus locked wrappers, guarded by a CAS
  spinlock in `struct uk_bballoc`. `uk_allocbbuddy_init_shared()` marks the shared
  default heap.
- Free-path guards print `free of unallocated or wild obj` and `bad-size free`
  with a 12-frame backtrace (Ticket `d4c2a6bd2a`).
- Heap canaries add 16 guard bytes per allocation under
  `CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY`, checked at free (Ticket `a9c6945c21`).
- Fixes the hlist back-link test to `*c->link.pprev != &c->link`.

### 3.5 Per-CPU Pending Event Flags

**Problem.** `sched_have_pending_events` is one slot for all CPUs. A wake that belongs to
core 1 can be consumed by core 0, so core 1 keeps sleeping and its device queue
stays unpolled.

**Approach.** `lib/ukintctlr/ukintctlr.c` adds `uk_intctlr_pending_events[16]`,
indexed by CPU id. `uk_intctlr_irq_handle()` sets the current CPU's bit for any
non-timer interrupt. The clock checks its own slot before and after halting.

### 3.6 epoll Ready List

**Problem.** Level-triggered `epoll` re-checks every registered file descriptor on each
wake. The per-request event cost grows with the number of live connections, which
dominates at 100 connections and more connections (Ticket `20d68e6796`).

**Approach.** `lib/posix-poll/epoll.c` adds a ready path:

- `epoll_ready_enqueue()` pushes an entry onto a lock-free Treiber stack
  (`al->inbox`) from the poll callback.
- `epoll_ready_drain()` splices the inbox into a `ready` list under the file write
  lock.
- `uk_sys_epoll_pwait2()` now takes the write lock, drains the inbox, and walks
  only the entries on the ready list. Level-triggered entries stay queued; a
  consumed entry is re-queued if new events arrived meanwhile.
- `epoll_ready_remove()` unlinks an entry on `EPOLL_CTL_DEL`.

### 3.7 Per-vCPU LAPIC One-Shot Wake

**Problem.** The i8254 one-shot output is wired to the 8259, and that interrupt
routes only to the boot CPU. A halted secondary core has no wake source, so its
device queue stays unpolled and a run-to-completion worker cannot idle (Tickets
`a5b91e7993`, `597c1b9731`).

**Approach.** `plat/kvm/x86/tscclock.c` adds a per-vCPU LAPIC one-shot timer:

- `tscclock_lapic_prepare()` runs per core. It rejects x2APIC mode, probes the
  one-shot path with a masked timer, calibrates `lapic_mult[]` (ticks per
  nanosecond) over a 1 ms window, and registers irq 16 with
  (`APIC_TIMER_VECTOR` 48) with `lapic_timer_handler()`.
- `tscclock_cpu_block_lapic()` programs LVT and ICR, then calls
  `uk_lcpu_halt_irq()`. It clamps the delta to 2 µs .. 200 ms.
- `time_block_until()` also checks `uk_intctlr_pending_events[id]` before and
  after halting, so a wake events that no event is lost in the gap.
- The code falls back to the i8254 path when the probe fails.

### 3.8 IPI Destination Fix on the Native Platform

**Problem.** `apic_send_ipi()` wrote the destination to ICR-high and set the SELF
bit, so an IPI reached only the sender. The x2APIC MSR access also faults on this
hypervisor (Ticket `aa0fc35743`).

**Approach.** `plat/native/arch/x86_64/except.c` forces xAPIC MMIO mode and
writes ICR-low at base + `0x300` with the destination in bits 31:24. The same
destination placement appears in the MSI message builder (see 4.1).

### 3.9 Small Fixes

- `lib/posix-fdio/fdctl.c`: `uk_sys_ioctl()` no longer returns early for
  `FIONBIO`. It falls through to the normal `_SHOULD_LOCK()` path, so a concurrent
  change of `O_NONBLOCK` honors the descriptor lock.
- `plat/native/arch/x86_64/sysctx_auxsp.c`: `uk_plat_native_set_auxsp()` returns
  early when `auxsp` is NULL, before it dereferences the control block during core
  bring-up.

## 4. Differences Between the Two Core Patches

The two core patches were generated at different bases and have drifted. Check these points
before you copy a change from one to the other.

| Item | `unikraft-eb8fa236.patch` | `unikraft-e31b2c44.patch` |
| :--- | :--- | :--- |
| Base commit | `eb8fa236` | `e31b2c44` |
| Allocatable irq pool | 20..222 | 20..207, which keeps irq 16 for the LAPIC timer and moves the wake IPI to vector `0xF0` |
| MSI destination field | bits 55:48 (`<< 56`) | bits 31:24 (`<< 12`) |
| 8259 EOI guard | `irq <= 16` | `irq <= 15` |
| `stop` driver op | absent | present |

The `<< 56` form breaks the `0xFEE` prefix in xAPIC physical destination mode, and
the device then drops the message. The `<< 12` form is correct. Regenerate the
single-core patch from a current base and carry this fix into it.

## 5. Patch Hygiene

- `lib-lwip-ec55ae17.patch` adds `src/api/sockets.c.orig`. That file is a backup
  that the nested patch left behind. Drop it when you regenerate the patch.
- `samples/httpreply-mc/run_notes_heap-corruption-guard.md` says the hlist
  back-link fix lives in `patches/unikraft-eb8fa236.patch`. It does not. That fix
  is only in this directory's `unikraft-e31b2c44.patch`.

## 6. Related Records

| Ticket | Subject |
| :--- | :--- |
| `a5b91e7993` | per-vCPU LAPIC one-shot wake replaces the shared i8254 idle halt |
| `aa0fc35743` | one-shot wake misfires, then the guest hangs on EC2 |
| `d4c2a6bd2a` | intermittent stall at c200 from concurrent buddy allocation |
| `a9c6945c21` | target dies at about 100 connections (`uk_hlist_del` crash) |
| `f47bdd0ed1` | two per-core heaps corrupt because TX completion frees elsewhere |
| `20d68e6796` | level-triggered epoll walks every registered fd on each wake |
| `597c1b9731` | worker loop burns 100% CPU at zero load |

All tickets are closed. Read the full history with `fossil ticket history <uuid>`.
