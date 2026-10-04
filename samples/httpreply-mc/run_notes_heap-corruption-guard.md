# Run Note: heap corruption at c=100 (httpreply-mc)

Ticket: a9c6945c21.

## Corrected root cause

The crash report names `uk_hlist_del` at `uk/list.h:302`. That function is
not used by lwIP. lwIP links TCP control blocks with its own `pcb->next`
pointers. In this build `uk_hlist_del` is reached only from the bbuddy page
allocator (`bbuddy_pfree` / `bbuddy_merge`), which frees a `chunk_head` whose
`link.pprev` is corrupt. So the fault is heap-metadata corruption, not a
per-core lwIP list bug. The earlier hypothesis in the ticket was wrong.

The "hang with no banner" variant is a side effect of the shared fd lock. If
one core faults or spins inside a corrupted allocator while it holds
`mc_fd_lock`, the other core spins in `mc_fd_lock_acquire` and prints
nothing.

## Defect found and fixed

The TX "stuck bounce" recovery path (`ena_netdev_release_stuck_tx_bounce`)
force-releases a request ID that the device may still complete later. The
late completion then releases the same ID a second time. The request-ID free
pool is a FIFO with no double-release guard, so the ID is pushed twice. A
later pair of allocations can then hand one ID to two requests, and their
completions free the same netbuf twice. That double free corrupts the
per-core bbuddy heap and crashes in `uk_hlist_del`.

This path only runs when the TX bounce pool is exhausted, which happens at
high connection counts. It matches the report: c=10 and c=50 pass, c=100
fails, and both cores are affected because each core runs its own TX ring.

Fix: `ena_ring_req_id_free` is now idempotent. A request ID is tracked as
allocated or free (`req_allocated`), and a second release is a no-op instead
of a second push. Unit test `test_req_id_double_free` covers it.

## Guarded rebuild (localization, if the crash persists)

If a guarded run still faults, bbuddy can assert at the exact corrupting
free instead of crashing later. Build with the freelist sanity check on:

```bash
cp defconfig .config
sed -i 's/^# CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY is not set/CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY=y/' .config
make olddefconfig && make
```

Then deploy and run the same sweep:

```bash
wrk -t4 -c100 -d15s http://<target>/   # after a c=50 warm-up
```

With the check on, a corrupt freelist trips a `UK_ASSERT` inside
`freelist_sanitycheck` at the call that caused it. That points at the
freeing code path, not at a later victim. The check adds large overhead, so
use it only to localize, not to measure throughput.

## Reading the new fd-lock warning

The worker now prints one line when a core waits on the fd lock for more
than 2 seconds:

```
httpreply-mc: [WARN] core 1 waiting on fd lock held by core 0 for 2003 ms (holder may be stuck)
```

That names the stuck core. A stuck holder points at a fault or an infinite
loop on that core, which the guarded build then localizes.
