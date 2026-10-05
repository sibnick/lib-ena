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

The first guarded run (2026-10-04) did not boot. The bbuddy freelist sanity
check had an upstream bug at unikraft eb8fa236: its back-link test
`(*c->link.pprev)->next != &c->link` is always true for a valid hlist, so
it printed "Bad backlink" for every node of every freelist at boot. The
serial flood stalled boot and the target never bound port 80. Fixed in
`patches/unikraft-eb8fa236.patch`: the test is now
`*c->link.pprev != &c->link`, the real hlist invariant.

The check does not assert. It prints `uk_pr_err` lines on every allocation
and free, the first of which names the corrupt freelist and chunk pointer:

```
ERR: [libukallocbbuddy] <bbuddy.c @ ...> Bad backlink @ <ptr> (free_head[<order>](<head>) + <off>): got <p>, expected <p>
```

Other strings it can print: "Invalid chunk pointer", "Unaligned chunk",
"Bad page level", "Bad backlink". Read the FIRST one after the warm-up;
it is close to the corrupting operation.

Build with the check on. `defconfig` is a fragment, so expand it first,
then set the flag, then expand again:

```bash
cp defconfig .config
make olddefconfig
sed -i 's/^# CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY is not set/CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY=y/' .config
make olddefconfig && make
```

Confirm the flag is on before building:

```bash
grep CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY=y .config
```

Then deploy and run the repro after a c=50 warm-up:

```bash
wrk -t4 -c100 -d30s http://<target>/
```

Poll the console with `aws ec2 get-console-output`. The check adds large
overhead, so throughput is low and the fault can take a while to appear.
Use this build only to localize, not to measure.


## Reading the new fd-lock warning

The worker now prints one line when a core waits on the fd lock for more
than 2 seconds:

```
httpreply-mc: [WARN] core 1 waiting on fd lock held by core 0 for 2003 ms (holder may be stuck)
```

That names the stuck core. A stuck holder points at a fault or an infinite
loop on that core, which the guarded build then localizes.

## Network log sink (/__log)

The EC2 serial console is lossy and lags, so it drops the guard and
fd-lock lines we need. The app can mirror every log line into an
in-memory ring and serve the last bytes at GET /__log on the listen
port. Enable it with CONFIG_APPHTTPREPLYMC_NETLOG=y. Read it during a
run with:

    curl -s http://<PUBLIC_IP>/__log

The ring holds the most recent CONFIG_APPHTTPREPLYMC_NETLOG_SIZE bytes
(default 0x40000). One fetch near the end of a run returns that run's
guard and fd-lock lines. Keep the flag off for clean latency numbers.

Guarded + netlog build:

    cp defconfig .config
    make olddefconfig
    sed -i 's/^# CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY is not set/CONFIG_LIBUKALLOCBBUDDY_FREELIST_SANITY=y/' .config
    sed -i 's/^# CONFIG_APPHTTPREPLYMC_NETLOG is not set/CONFIG_APPHTTPREPLYMC_NETLOG=y/' .config
    make olddefconfig && make

## CORRECTION (2026-10-05): the heap corruption was a false positive

The ukalloc heap canary was buggy: uk_posix_memalign_ifpages (the path
uk_netbuf_alloc_buf uses) calls uk_palloc directly and never wrote a
canary, and the check read at metadata->base+METADATA+size, which is not
the user region for a memalign allocation (the user pointer is
alignment-offset). So every ENA RX netbuf was flagged at boot with no
real fault. A follow-up bug (canary write past the block) even crashed at
boot with rax = the canary pattern.

Fixed: write the canary at userptr+size in the memalign path, reserve
UKALLOC_CANARY_BYTES in its realsize, and check at userptr+size in the
free path and uk_alloc_canary_check. Verified clean on local QEMU (both
cores' alloc self-test passes, no canary lines).

With the trustworthy canary on EC2: ZERO real heap overruns under load
(0 ukalloc-canary, 0 rx-canary, 0 Bad backlink, 0 crash). The heap was
never the cause. The bbuddy freelist "corruption" reports were also
artifacts of the same broken canary.

## Real failure mode: cross-core fd-lock deadlock

The live failure is the app's fd lock (mc_fd_lock, main.c:799) held by a
stuck core: "core X waiting on fd lock held by core Y for 21923..112518
ms (holder may be stuck)". The lock guards fd lifecycle only (accept4 at
main.c:1051, close at main.c:843). A holder stuck for >100s is blocked
inside accept4/close. There is also a flat ~116ms latency floor at every
concurrency level. The hang is intermittent (struck at c50 in one run;
another survived c50/c100/c200).

Leading hypothesis: close()/send() blocks on a stalled ENA TX completion
while holding mc_fd_lock, so the holder never releases and the other core
deadlocks. Next: instrument the fd-lock holder (what it is doing while
holding the lock) and the ENA TX completion latency.
