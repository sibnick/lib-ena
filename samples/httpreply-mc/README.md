# Multi-Core Unikraft HTTP Reply Benchmark (`httpreply-mc`)

## 1. Overview

This sample provides a multi-core HTTP reply benchmark server for Unikraft on AWS EC2. It uses the native AWS ENA driver (`lib-ena`) and the lwIP network stack in `NO_SYS` mode.

The sample implements a shared-nothing run-to-completion architecture:
- Each CPU core binds to an independent ENA hardware queue pair.
- Hardware Receive Side Scaling (RSS) steers incoming TCP traffic across RX queues.
- Each core runs an isolated polling loop with local lwIP stack state.
- Sockets bind to port 80 with `SO_REUSEPORT` on each core.
- Workers transmit packets directly on their dedicated hardware TX queues.
- There are no inter-core locks, queues, or context switches.

## 2. Architecture

The server pins one run-to-completion worker to each CPU core:

```mermaid
graph TD
    NIC["AWS ENA Hardware RSS"]

    subgraph Core_0["CPU Core 0"]
        RX0["ENA RX Queue 0"]
        TX0["ENA TX Queue 0"]
        LWIP0["lwIP State (Core 0)"]
        LISTEN0["Listener (Port 80, SO_REUSEPORT)"]
        WORKER0["Worker 0 (epoll 0)"]
    end

    subgraph Core_1["CPU Core 1"]
        RX1["ENA RX Queue 1"]
        TX1["ENA TX Queue 1"]
        LWIP1["lwIP State (Core 1)"]
        LISTEN1["Listener (Port 80, SO_REUSEPORT)"]
        WORKER1["Worker 1 (epoll 1)"]
    end

    NIC -->|Flow Hash Queue 0| RX0
    NIC -->|Flow Hash Queue 1| RX1

    RX0 --> WORKER0
    WORKER0 --> LWIP0
    LWIP0 --> LISTEN0
    WORKER0 --> TX0

    RX1 --> WORKER1
    WORKER1 --> LWIP1
    LWIP1 --> LISTEN1
    WORKER1 --> TX1
```

## 3. Directory Layout

| Path | Purpose |
| :--- | :--- |
| `main.c` | Run-to-completion multi-worker HTTP echo server |
| `spsc.h` | Standalone lock-free single-producer single-consumer ring buffer |
| `Config.uk` | Application Kconfig options |
| `defconfig` | Target configuration for multi-core KVM and multi-queue ENA |
| `Makefile` | Top-level build entry point with automated patch application |
| `Makefile.uk` | Build definitions for Unikraft build system |
| `Kraftfile` | KraftKit specification referencing `lib-ena` from repository root |
| `patches/` | Submodule patches for lwIP and Unikraft core |
| `scripts/apply_patches.sh` | Idempotent patch application script |
| `scripts/build_disk.sh` | Builds raw bootable disk image with GRUB |
| `scripts/deploy_aws.py` | Deploys image to AWS EBS and launches EC2 instance |
| `scripts/run_ec2_benchmark.py` | Automated multi-concurrency benchmark runner |
| `benchmark_results.csv` | Measured benchmark metrics in CSV format |
| `benchmark_results.json` | Measured benchmark metrics in JSON format |

## 4. Build Instructions

### Option 1: Build with Make and Submodules

1. Initialize git submodules:
   ```bash
   git submodule update --init --recursive
   ```
2. Generate configuration file from defconfig:
   ```bash
   cp defconfig .config
   make olddefconfig
   ```
3. Build unikernel image:
   ```bash
   make
   ```

Output binaries:
- `build/httpreply-mc_qemu-x86_64`: Multiboot ELF unikernel.
- `build/httpreply-mc_qemu-x86_64.bootinfo`: UKBI bootinfo blob.

### Option 2: Build with KraftKit

```bash
kraft build --target aws-t3-x86_64
```

## 5. Deploy to Amazon EC2

Deploy the unikernel to an AWS EC2 instance. This sample requires an instance type with at least two vCPUs, such as `c6i.large`.

```bash
SUBNET_ID=subnet-xxxxxx \
PRIVATE_IP=172.31.x.y \
GATEWAY_IP=172.31.x.1 \
NETMASK=255.255.240.0 \
INSTANCE_TYPE=c6i.large \
python3 scripts/deploy_aws.py
```

The script builds the unikernel, creates the disk image, registers an AMI, and launches the instance.

## 6. Run Benchmarks on AWS

Test connectivity to the instance:

```bash
curl -i http://<instance-public-or-private-ip>/
```

Run the automated benchmark suite with `wrk`:

```bash
python3 scripts/run_ec2_benchmark.py
```

Or run `wrk` manually with keep-alive connections:

```bash
wrk -t4 -c200 -d10s --latency http://<instance-ip>/
```

## 7. Verified AWS EC2 Performance Results

These results come from a same-day A/B run on 2026-10-05, on trunk
revision `cf895d8cec`. This trunk includes the TX reclaim at completion
[Ticket 292e049bf4] and the compiler optimization changes
[Ticket 483416560f]. Both servers are AWS EC2 `c6i.large` (2 vCPUs) in
subnet 172.31.16.0/20 (us-east-1a), and the `wrk` client (Ubuntu 24.04)
sits in the same subnet. Both servers return a 14-byte HTTP body. Each
level ran one 30 s `wrk` sweep with 2 threads.

**What the levels mean.** c=10 and c=25 show throughput at low load.
c=100 is the pass/fail goal of the multi-core design: it must reach at
least 80,000 req/s, and it reached 218,506. c=200 is a stress probe of
the saturation limit.

### Table 1. Unikraft multi-core (`httpreply-mc`)

| Concurrency (`-c`) | Req/s | Avg Latency (ms) | P50 (µs) | P99 (ms) | Socket Errors |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 10 | 51,406.18 | 0.19 | 192.00 | 0.26 | 0 |
| 25 | 103,629.18 | 0.22 | 225.00 | 0.30 | 0 |
| 50 | 154,850.15 | 0.30 | 295.00 | 0.42 | 0 |
| 100 | 218,506.21 | 0.43 | 399.00 | 1.70 | 0 |
| 200 | 223,780.04 | 0.76 | 717.00 | 2.28 | 0 |

### Table 2. Linux Nginx baseline (Ubuntu 24.04, default configuration)

| Concurrency (`-c`) | Req/s | Avg Latency (ms) | P50 (µs) | P99 (ms) | Socket Errors |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 10 | 29,926.21 | 0.34 | 337.00 | 0.43 | 0 |
| 25 | 62,676.13 | 0.38 | 386.00 | 0.49 | 0 |
| 50 | 74,290.73 | 0.67 | 671.00 | 0.88 | 0 |
| 100 | 74,409.55 | 1.34 | 1,330.00 | 1.72 | 0 |
| 200 | 74,014.64 | 2.70 | 2,680.00 | 3.66 | 0 |

### Comparison

The table compares the two servers row by row at equal concurrency.
The last column is the relative difference (Unikraft minus Nginx, over Nginx).

| Concurrency (`-c`) | Nginx Req/s | Unikraft Req/s | Unikraft vs Nginx |
| :--- | ---: | ---: | ---: |
| 10 | 29,926.21 | 51,406.18 | +71.8% |
| 25 | 62,676.13 | 103,629.18 | +65.3% |
| 50 | 74,290.73 | 154,850.15 | +108.4% |
| 100 | 74,409.55 | 218,506.21 | +193.7% |
| 200 | 74,014.64 | 223,780.04 | +202.3% |

Nginx plateaus at about 74k req/s from c=50 up, because its two worker
processes saturate there. The multi-core Unikraft server keeps scaling, and
it now leads Nginx at every measured level, from +72% at c=10 to +202% at
c=200.

These numbers double the throughput of the 2026-09-27 run at the same
levels. Two changes on trunk account for the gain: the driver reclaims TX
bounce slots at completion instead of walking all 256 map entries per send
[Ticket 292e049bf4], and the build now compiles the driver at `-O3` with
LTO [Ticket 483416560f].

The P99 tail also dropped. Earlier runs showed P99 near one second, caused
by the driver heartbeat and console I/O pauses. In this run P99 stays under
2.3 ms, and the average latency stays under 0.8 ms at c=200. Both stacks
served all requests with zero socket errors (32,081,621 requests across
10 `wrk` runs).

## 8. Configuration Reference

Key Kconfig options used in `defconfig`:

| Option | Value | Description |
| :--- | :--- | :--- |
| `CONFIG_UKPLAT_CPU_MAXCOUNT` | `2` | Maximum number of CPU cores configured for KVM |
| `CONFIG_HAVE_SMP` | `y` | Enables Symmetric Multi-Processing support |
| `CONFIG_LIBUKPCPUVAR` | `y` | Enables per-CPU storage variables |
| `CONFIG_LWIP_NOTHREADS` | `y` | Runs lwIP in non-threaded NO_SYS mode |
| `CONFIG_LWIP_PERCORE` | `y` | Isolates lwIP stack state per CPU core |
| `CONFIG_LIBUKNETDEV_MAXNBQUEUES` | `2` | Configures two hardware network queue pairs |
| `CONFIG_LIBENA` | `y` | Enables native AWS ENA driver |
| `CONFIG_LIBENA_LLQ` | `y` | Enables ENA Low Latency Queue support |
| `CONFIG_LIBENA_MAX_QUEUES` | `8` | Maximum queue pairs supported by ENA driver |
| `CONFIG_LIBENA_RSS` | `y` | Enables ENA hardware Receive Side Scaling |
| `CONFIG_LIBENA_VERBOSE_STATS` | `y` | Prints periodic datapath counters to console |

## 8. Design Decisions

### Run-to-Completion Execution
Each worker core polls its assigned ENA RX queue. The same core processes stack timers, accepts incoming connections, and writes HTTP responses. This design eliminates inter-core synchronization.

### Hardware RSS Steering
The AWS ENA device hashes TCP/IP 4-tuples and distributes incoming connections across hardware RX queues. Each core processes its own traffic partition.

### Per-Core Stack State and SO_REUSEPORT
The lwIP stack runs in `NO_SYS` mode with per-core state encapsulation. Each core owns independent TCP control blocks, timers, and socket tables. Sockets bind with `SO_REUSEPORT` to accept connections on port 80 without global locks.

### Per-CPU Heap Partitions
At boot, the heap is split into N-1 partitions, where N is the CPU max count (`CONFIG_UKPLAT_CPU_MAXCOUNT`). Each non-boot core gets one partition. The boot core keeps the remaining region as the default allocator.

Each core creates a standalone `ukalloc` (bbuddy) instance on its partition and binds it to a per-CPU slot (`uk_pcpuvar_percore_alloc`). All runtime allocations from that core (lwIP netbufs, sockets, ENA buffers) go to its own instance. This prevents two cores from touching the same allocator, which was the root cause of the concurrent `bbuddy_palloc` corruption.

### Idle Backoff
The worker loop was an unthrottled spin: at zero traffic it ran the loop
1.3-2.3 M no-work iterations per second per core, each paying a fixed
TSC, ARP, epoll, and queue-peek cost, so both vCPUs read 100% at all
loads. The loop now backs off when it has no work
(`idlebackoff.h`, [Ticket 597c1b9731]):

- An iteration counts as work when an epoll event fires or a packet
  arrives at or leaves the core's hardware queue (cumulative ENA
  counters). After `IB_IDLE_STREAK_LIMIT` (8) consecutive no-work
  iterations, the core sleeps.
- The sleep is a real vCPU halt on the KVM platform: the guest arms a
  one-shot timer for the wake and executes `sti; hlt`, so the vCPU
  leaves the run queue for the whole sleep.
- While traffic flows (work within the last `IB_ACTIVE_WINDOW_NS`,
  2 ms), the sleep is a fixed `IB_ACTIVE_SLEEP_NS` (20 us). After
  2 ms without work the budget grows `IB_LONG_BASE_NS` (1 ms) to
  `IB_LONG_MAX_NS` (8 ms) in doubling steps until the next packet
  wakes the core.

The added wake latency is bounded: 20 us while traffic flows, and at
most 8 ms once, for the first request after a long idle. At zero
load both vCPUs are halted nearly all the time, so the instance shows
idle vCPU usage instead of a permanent 100%. All thresholds are
`#define`s in `idlebackoff.h`; the policy is covered by a host unit
test (`tests/test_idlebackoff.c`).
