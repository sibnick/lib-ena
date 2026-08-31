# ENA Driver Security Audit

Date: 2026-08-29. Scope: all driver sources under `src/` and `include/`,
reviewed against the upstream ABI definitions in `reference/`.

## Threat Model

The driver runs inside a Unikraft unikernel and talks directly to the ENA
PCI device. The audit treats the device as untrusted input. A faulty,
malicious, or compromised device can return any values in admin responses
and completion descriptors. The driver must never act on those values in
a way that corrupts memory, leaks data, or crashes the kernel.

## Findings Summary

| ID | Severity | Area | Ticket |
| :--- | :--- | :--- | :--- |
| H1 | High | datapath | 7b74d68625 |
| H2 | High | datapath | d26028408f |
| H3 | High | datapath | 071fac0e5c |
| H4 | High | netdev | 9a8732b869 |
| H5 | High | interrupts | 8bb0c952b5 |
| M1 | Medium | netdev | abe4742408 |
| M2 | Medium | init | 44ba1a615e |
| M3 | Medium | netdev | bef0fdc7fd |
| M4 | Medium | netdev | 34958a0e34 |
| M5 | Medium | netdev | d588f9bb33 |
| M6 | Medium | llq | 3c4ea85d18 |
| M7 | Medium | datapath | 5a48b1835e |
| L1 | Low | pci | d6af84a6de |
| L2 | Low | admin | 756f3d7629 |
| L3 | Low | interrupts | fbd9aef6ca |
| L4 | Low | pci | 3f7eed5c45 |
| L5 | Low | admin | cea0c20033 |
| L6 | Low | netdev | 2147b78aca |

## High Severity

### H1: Doorbell Offsets Not Range-Checked

`src/ena_datapath.c:345-348, 360-363` forms doorbell pointers as
`bar0_base + cq_db_offset` and `bar0_base + sq_db_offset`. The offsets
come from the device's CREATE_CQ and CREATE_SQ responses. The code does
not check them against `adapter->bar0_size` (16 KB) or 4-byte alignment.
The pointers are written from `src/ena_tx.c:113`, `src/ena_rx.c:187`, and
`src/ena_intr.c:89-90`. A device that returns an out-of-window offset
redirects 32-bit MMIO writes outside BAR0.

Fix: before forming each doorbell pointer, require
`0 < offset < bar0_size` and `(offset & 3) == 0`. On a violation, log an
error, tear down the partially created queue, and return an error.

### H2: RX Completion Length Not Validated

`src/ena_rx.c:164` copies `cdesc->length` (a 16-bit value, up to 65535)
into the packet record. The descriptor was offered with at most
`buf_len` (2048 bytes by default, `src/ena_netdev.c:164`). The netdev
layer sets `nb->len` to that value at `src/ena_netdev.c:217, 507`. The
stack then reads `nb->data[0..len)` past the end of the heap object.

Fix: in `ena_rx_poll`, compare the completion length against the tracked
buffer capacity (`rx_bufs[req_id].data_len`). On overflow, log an error,
drop the packet, and re-offer the buffer to the device. Never pass the
packet to the caller.

### H3: Completion req_id Lacks In-Flight Validation

`src/ena_tx.c:143-163` and `src/ena_rx.c:156-176` only range-check the
completion `req_id` against `sq_depth`. The device can echo an id that
is already free or was never submitted. `ena_ring_req_id_free`
(`src/ena_datapath.c:191-209`) then pushes the id back into the free
pool a second time. Two in-flight descriptors then share one id. On the
TX path, `uk_netbuf_free` (`src/ena_tx.c:156-157`) runs on a netbuf that
is still referenced. The later real completion frees it again. This is a
use-after-free.

Fix: keep a per-ring in-flight bit for each req_id. Set it in the
submit path. In the completion path, check the bit. If it is not set,
log an error and drop the completion. If it is set, clear it and free
the req_id and buffer as today.

### H4: Shared Static TX Bounce Buffer

`src/ena_netdev.c:225-251` uses one static 4 KB buffer as the DMA source
for every TX packet whose data sits below 1 MB. The descriptor keeps
pointing at the buffer. The next TX from any queue or thread overwrites
it before the device finished reading the previous packet. The result is
silent packet corruption and cross-queue data leak.

Fix: give each TX queue its own bounce buffer. Reuse happens only after
the queue's own completion for the borrowed buffer is observed.

### H5: Poll Step Iterates Past Ring Array Bounds

`src/ena_intr.c:149, 163` loops to `adapter->max_tx_queues` and
`adapter->max_rx_queues`. The ring arrays were allocated with the
application's `nb_*_queues` values (`src/ena_netdev.c:313-324`). The
maxima come from the device and are truncated from u32 to u16
(`src/ena_init.c:93-105`), so they can be as large as 65535. A hostile
or faulty device makes the loop read garbage pointers past the array and
then dereference them.

Fix: loop to `num_tx_rings` and `num_rx_rings`. See also M2 for clamping
the device-reported limits at parse time.

## Medium Severity

### M1: RX Queue Configure Has No queue_id Bound

`src/ena_netdev.c:93-121` (Unikraft branch) indexes
`edev->rx_queues[queue_id]` with no bound check. The TX twin checks
`queue_id >= 8` at `src/ena_netdev.c:132`. The Unikraft `configure`
(lines 71-91) also skips the `nb_*_queues` validation that the host
branch has at lines 306-310.

Fix: add the same `queue_id` bound for RX as for TX. Add the
`nb_rx_queues` / `nb_tx_queues` validation to the Unikraft `configure`.

### M2: Device-Reported Limits Not Clamped

`src/ena_init.c:93-113` truncates device u32 values to u16 and stores
them without an upper bound. `max_sq_num` and `max_sq_depth` can be up to
65535. These values feed ring sizing (`src/ena_netdev.c:37, 48, 348,
381`) and the loop bound in H5.

Fix: at parse time, clamp to the ENA spec maxima (at most 256 queues,
depth at most 4096, depth a power of two). Log when a clamp applies.

### M3: nb_desc Is Unbounded in Queue Configure

Both branches (`src/ena_netdev.c:104-107` Unikraft, `347-356` host) pass
the application-supplied `nb_desc` straight to `ena_ring_alloc`, which
only checks power-of-two and a minimum of 4 (`src/ena_datapath.c:17-20`).
A depth of 65535 allocates about 2.5 MB of DMA memory plus tracking per
ring.

Fix: cap `nb_desc` against the device `max_*_ring_size` and a hard
ceiling of 4096.

### M4: RX Buffer DMA Address Not Validated

`src/ena_netdev.c:153-175` passes `(uintptr_t)nb->data` to the device as
a DMA address with no check. The low-memory reservation in
`src/ena_plat.c:98-107` only protects driver-internal allocations, not
buffers from the application's `alloc_rxpkts` pool. The TX path has a
below-1 MB bounce guard (`src/ena_netdev.c:244-251`). The RX path has
none.

Fix: apply the same DMA-safety check on the RX path. If a buffer is not
DMA-safe, use a driver-allocated DMA-safe bounce buffer for that slot
and copy data on completion.

### M5: No dev_stop in the Unikraft Ops Table

`src/ena_netdev.c:267-278` registers `start` but no stop op. On
teardown, the hardware queues stay enabled and the netbufs are freed
while the device can still DMA into them.

Fix: add a `dev_stop` op that quiesces the device, drains pending TX
completions, and destroys the hardware queues. Mirror the host branch
`ena_netdev_stop` (`src/ena_netdev.c:450-479`).

### M6: LLQ Fallback Copies Header Before Length Check

`src/ena_llq.c:86-90` copies `hdr_data` into `nb->data` before the
`hdr_len > 96` check at line 97. A large `hdr_len` is an out-of-bounds
heap write on the fallback path. `src/ena_llq.c:153-154` writes 128
bytes at `push_buf_virt + sq_tail * 128` with no check against the push
buffer size. Lines 157-159 advance `sq_tail` with a masked increment,
which conflicts with the monotonic convention used in
`src/ena_tx.c` and `src/ena_rx.c`. The path is latent today (BAR2 is not
mapped by the probe), but it is armed for re-enablement.

Fix: move the `hdr_len` check before the copy. Bound the push write
against `push_buf_size`. Align the tail convention with the rest of the
datapath (monotonic counter, phase flip on wrap).

### M7: No Synchronization on Datapath Ring State

The SQ and CQ tails, phase bits, and req-id free pool are read and
written without any lock. The admin path has a spin lock
(`src/ena_admin.c:39-48`). The datapath has none. uknetdev does not
guarantee a single thread per queue. Concurrent `rx_one` / `tx_one`
calls on the same queue are a data race that can corrupt the req-id
pool (see H3).

Fix: add a per-ring lock around submit and completion sections. Use the
same `__sync_lock_test_and_set` / `__sync_lock_release` pattern as the
admin path so the code stays portable under Unikraft.

## Low Severity

### L1: Probe Failure Paths Leak DMA Memory

`src/ena_pci.c:137-160` frees only the `ena_uk_device` struct on failure
after `ena_admin_init` or `ena_init_run`. The AQ/ACQ/AENQ rings and the
4 KB host-info buffer are not released.

Fix: call `ena_admin_fini` (and the init cleanup, if separate) on each
failure path after admin init succeeded.

### L2: No Recovery After Admin Timeout

`src/ena_admin.c:290-294` sets `ENA_STATE_ERROR` and resets the device
on a poll timeout. The AQ/ACQ base registers and ring state are not
invalidated or re-initialized. `ena_admin_exec_cmd` returns `-ENODEV`
while the state is ERROR (line 354), and no API re-runs admin
initialization. A single timeout kills the netdev permanently.

Fix: after the reset, re-initialize the admin queues (re-create
AQ/ACQ/AENQ via the existing init path) and restore a usable state.

### L3: INTR_MASK Polarity Unverified

`src/ena_intr.c:66` writes 0 to `ENA_REGS_INTR_MASK_OFF` to mask vector
0. Line 84 writes 1 to unmask it. Verify the bit polarity against the
ENA ABI before relying on it.

### L4: BAR0 Size Hardcoded

`src/ena_pci.c:99` hardcodes `bar0_size` to 0x4000 without the standard
write-all-ones / read-back size determination.

Fix: determine the BAR size from the PCI size readback and validate it
is at least the minimum needed (0x104, `src/ena_com.c:13`).

### L5: 12-bit Command-id Wrap

`src/ena_admin.c:246-250` wraps command ids at 4096
(`ENA_ADMIN_COMMAND_ID_MASK`). If a completion is lost or arrives late,
a later command with the same wrapped id can match a stale ACQ entry.
The phase bit mitigates this in practice.

Fix: on a wrap, invalidate the ACQ (write the ACQ head register to the
current tail, reset the phase expectation) before issuing the next
command.

### L6: Features Field Inconsistent

`src/ena_netdev.c:29` (Unikraft) sets `info->features = 0`. The host
branch at line 293 advertises `UK_NETDEV_F_RX_CSUM | UK_NETDEV_F_TX_CSUM`.
The driver does enable checksum offloads on TX
(`src/ena_netdev.c:548-549`), so the host value is the accurate one.

Fix: make both branches report the same feature set.

## Done Right

- All device-driven loops are bounded by time-based budgets.
- Admin response copies are capped by `resp_cap` (`src/ena_admin.c:324-330`).
- Ring depths are validated as power-of-two and at least 4
  (`src/ena_datapath.c:17-20`).
- All DMA descriptor memory is zeroed before use.
- `DEV_CTL` updates use read-modify-write.
- Feature command layouts match `reference/ena_admin_defs.h:1010-1020`.
  The leading `control_buffer` field is correct for upstream.
- PCI device ID table matches `reference/ena_pci_id_tbl.h` verbatim.
- The code builds clean under `-Wall -Wextra -Werror -pedantic` and
  passes ASan (`make sanitize`).

## Recommended Fix Order

1. H1, H2 (direct out-of-bounds access from device input)
2. H3 (in-flight bitmap; also underpins M7)
3. H4, H5 (corruption and out-of-bounds read)
4. M1 through M7
5. L1 through L6

After each group, re-run `make clean && make test` and `make sanitize`.
Re-run the EC2 benchmark after the datapath group. The H2 clamp should
not change throughput on healthy hardware.
