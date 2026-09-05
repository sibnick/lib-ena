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

## Measured Performance Results (AWS EC2 `c6i.large`)

The following measurements compare Unikraft (`lib-ena` and lwIP) with Ubuntu 24.04 on AWS EC2 `c6i.large` instances. Both instances ran in the same subnet (`us-east-1a`). The benchmark client ran `wrk` with two threads for 10 seconds per concurrency step.

| Concurrency | Target | Requests/sec | Avg Latency (ms) | Max Latency (ms) | Transfer (MB/s) | Socket Errors |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| 1 | Unikraft | 4,190.53 | 0.24 | 0.39 | 0.60 | 0 |
| 1 | Linux | 6,296.08 | 0.16 | 0.29 | 0.90 | 0 |
| 5 | Unikraft | 5,417.39 | 105.66 | 1510.00 | 0.77 | 1 |
| 5 | Linux | 21,774.27 | 0.18 | 4.34 | 3.11 | 0 |
| 10 | Unikraft | 10,008.30 | 115.47 | 1560.00 | 1.43 | 0 |
| 10 | Linux | 48,480.60 | 0.20 | 2.50 | 6.94 | 0 |
| 25 | Unikraft | 13,987.78 | 83.39 | 1260.00 | 2.00 | 8 |
| 25 | Linux | 45,981.79 | 0.55 | 13.42 | 6.58 | 0 |
| 50 | Unikraft | 17,356.70 | 92.68 | 1760.00 | 2.48 | 11 |
| 50 | Linux | 49,136.29 | 1.13 | 50.80 | 7.03 | 0 |
| 100 | Unikraft | 25,803.36 | 108.19 | 1980.00 | 3.69 | 38 |
| 100 | Linux | 48,808.80 | 2.65 | 208.15 | 6.98 | 0 |
| 200 | Unikraft | 53,404.49 | 67.48 | 1730.00 | 7.64 | 0 |
| 200 | Linux | 47,495.98 | 9.99 | 849.03 | 6.79 | 0 |

At concurrency 200, Unikraft reached 53,404.49 requests per second, exceeding the Linux baseline by 12.4%. Machine-readable results are stored in `benchmark_results.csv` and `benchmark_results.json`.
