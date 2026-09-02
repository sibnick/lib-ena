# Official Unikraft HTTP Reply Benchmark (`app-httpreply`)

This sample provides the official Unikraft HTTP reply benchmark server. It runs as a lightweight unikernel on AWS EC2 instances with the native AWS ENA driver (`lib-ena`) and the lwIP network stack.

The lwIP stack runs in single-threaded (`NO_SYS`) mode: one thread polls the network device (`uknetdev_poll_all()`), drives the stack timers (`sys_check_timeouts()`), and multiplexes sockets with level-triggered `epoll`. There are no worker threads, no mailboxes, and no context switches between device, stack, and application.

Use this sample to benchmark network throughput, latency percentiles, and connection scalability on AWS EC2, and compare against Linux.

## Directory Layout

| Path | Purpose |
| :--- | :--- |
| `main.c` | Single-threaded HTTP echo server (NO_SYS lwIP, epoll-driven) |
| `Config.uk` | Kconfig dependencies (`nolibc`, `lwip`, `lib-ena`, `uknetdev`) |
| `Makefile.uk` | Build definitions for Unikraft build system |
| `Kraftfile` | KraftKit specification referencing `lib-ena` from `../..` |
| `Makefile` | Top-level build entry point with automated patch application |
| `patches/` | KVM x86 linker script multiboot fix |
| `scripts/apply_patches.sh` | Idempotent patch applier |
| `scripts/build_disk.sh` | Builds raw bootable disk image with GRUB |
| `scripts/deploy_aws.py` | Deploys image to AWS EBS and launches EC2 instance |
| `scripts/benchmark_wrk.sh` | Automated `wrk` benchmark harness for latency and throughput |

## Build Instructions

### Option 1: Build with KraftKit

```bash
kraft build --target aws-t3-x86_64
```

### Option 2: Build with Make and Submodules

1. Initialize submodules:
   ```bash
   git submodule update --init --recursive
   ```
2. Build the unikernel image:
   ```bash
   make
   ```

Output binaries:
- `build/httpreply-ena_qemu-x86_64`: Multiboot ELF unikernel loaded by GRUB.
- `build/httpreply-ena_qemu-x86_64.bootinfo`: UKBI blob.

## Deploy to Amazon EC2

Deploy the unikernel to an AWS EC2 instance:

```bash
SUBNET_ID=subnet-xxxxxx \
PRIVATE_IP=172.31.x.y \
GATEWAY_IP=172.31.x.1 \
NETMASK=255.255.240.0 \
python3 scripts/deploy_aws.py
```

The script builds the unikernel, creates the disk image, registers an AMI, and launches an EC2 instance.

## Run Benchmarks on AWS

After launching the instance, test connectivity:

```bash
curl -i http://<instance-public-ip>/
```

Run the automated `wrk` benchmark harness from a client machine in the same VPC or region:

```bash
./scripts/benchmark_wrk.sh <instance-private-or-public-ip> 30s 4
```

The script measures:
- Requests per second (req/s)
- Network transfer rate (MB/s)
- Latency percentiles (`p50`, `p75`, `p90`, `p99`) across concurrency levels (10, 50, 100, 200, 500 connections).

Results are saved to `benchmark-results/<timestamp>/benchmark_summary.csv`.

## Compare with Linux

To compare performance against Linux on the same instance type (e.g. `t3.nano` or `c6i.large`):

1. Launch an Ubuntu 24.04 EC2 instance in the same subnet.
2. Run Nginx or a C socket HTTP server on the Linux instance.
3. Run `./scripts/benchmark_wrk.sh <linux-ip> 30s 4` from the same benchmark client.
4. Compare requests per second, p99 latency, and CPU usage.
