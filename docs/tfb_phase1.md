# TFB Phase 1: Methodology Report and Results

Status: this is a TFB-compatible standalone report. It is not a
submission to the official TechEmpower Benchmark (TFB). TFB runs on
TechEmpower hardware with a fixed Docker pipeline. We use the TFB test
definitions on our own EC2 hardware and publish the full method here.

The plan for this work is the Fossil wiki page `Plan-TFB-Phase1`. The
ticket for this report is `[Ticket 8c3327ee16]`. All raw data is in the
Fossil unversioned store under `benchmarks/2026-11-18/`.

## 1. Goal and test types

Show the performance of the Unikraft-ENA stack in a form outside
readers can check. Phase 1 ports two TFB test types to
`samples/httpreply-mc` and measures them on EC2 against Linux
baselines.

### Test type 6: plaintext

The settled spec literals, from the TFB "Framework Tests Overview"
wiki, checked 2026-11-17:

1. URI: `/plaintext`.
2. Body: `Hello, World!` with no trailing newline. The body is 13
   bytes. Content-Length equals the body length.
3. Content-Type: bare `text/plain`, no charset parameter.
4. Headers `Server` and `Date` are present. The Date value is
   re-rendered once per second.
5. The response is composed per request. Headers plus body are not
   stored as one pre-rendered buffer.
6. Keep-alive works. Pipelined requests return correct in-order
   responses.
7. No gzip. The listen port has four digits:
   `CONFIG_APPHTTPREPLYMC_LISTEN_PORT`, default 8080.
8. TFB plaintext concurrency levels: 256, 1024, 4096, 16384.

### Test type 1: JSON serialization

1. URI: `/json`.
2. Body: `{"message":"Hello, World!"}`. The key case is exactly
   `message`. The serialized string is not cached across requests.
   The body is 27 bytes.
3. Content-Type: bare `application/json`, no charset parameter.
4. The header, keep-alive, pipelining, and port rules match the
   plaintext test.
5. TFB JSON concurrency levels run from 16 to 512. The sweep used
   16, 64, 128, 256, and 512. It did not use the TFB level 32.

Both endpoints sit on the routed path in `tfb_http.h`. The per-worker
response queue caps at 128 pending responses (`TFB_RESPQ_CAP`). Nodes
come from a fixed per-worker pool of 2048 slots
(`TFB_RESPQ_SLOTS`). The routed path performs no malloc
(`[Ticket 0f377016a9]`).

The sample also keeps the legacy `/` reply (14-byte body) for older
harnesses. The validator checks it only against the unikernel.

## 2. Methodology disclosure

### Build identity

1. The measured kernels were built from check-in `86600f855a`. The
   trunk tip at publication time is `48931be8b9`. No driver change
   sits between the two for the measured paths.
2. The c6i.large kernel is a 2-worker build. SHA-256
   `c5490475a7896004ec0ee3b65f438a5e32a4dd8d3782e27a094b8eca358d16ee`.
3. The c6i.xlarge kernel is a 4-worker build. SHA-256
   `4f5cab678d580850b2d10c47833f733a8c89c15933bd1e0295f5eb81aa494a32`.
4. The worker count follows the per-size shapes recorded in
   `samples/httpreply-mc/README.md`. The c6i types report two vCPUs
   per physical core. Four workers on two physical cores would
   over-subscribe the xlarge target.

### Runs, AMIs, and instances

All runs ran in `us-east-1`. The target used the fixed private IP
172.31.16.153. The client used 172.31.16.161 in the same subnet. The
security group allows TCP 8080 from the subnet only.

| Run | Date (UTC) | Target | Instance | AMI | Target instance ID | Client instance ID |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| runA-large-uk | 2026-11-18 | unikraft | c6i.large | ami-09b8bd55dc2f4e7b8 | i-0b428404751aefe58 | i-0b19cfb4508713bf5 |
| runB-large-nginx | 2026-11-19 | nginx | c6i.large | ami-0045d7fc2ad003464 | i-000e62be4c901c21a | i-0c6c809d28fcce92c |
| runC-xlarge-uk | 2026-11-19 | unikraft | c6i.xlarge | ami-0ca1811e306f79a6e | i-0ee4e9721162fcf13 | i-0794f84200ed46c28 |
| runD-xlarge-nginx | 2026-11-19 | nginx | c6i.xlarge | ami-0045d7fc2ad003464 | i-0613338d5172760b7 | i-0091de8b348e7828b |

The runC AMI came from snapshot `snap-0b969a4a3df153521`. The
runA snapshot ID is null in the run JSON. The client ran on
`c6i.xlarge` from the same Ubuntu AMI in every run. Teardown removed
every sweep-owned resource. See `teardown_verification.txt` in the
archive.

### Target shapes

The console log for each unikernel boot records the device shapes.

1. c6i.large: 2 worker cores, 2 TX plus 2 RX queue pairs, ring depth
   1024 per queue, 6 MSI-X vectors active, RSS indirection table of
   128 entries across 2 queues.
2. c6i.xlarge: 4 worker cores, 4 TX plus 4 RX queue pairs, ring depth
   1024 per queue, 8 MSI-X vectors active, RSS table across 4 queues.
3. The device reports `max_tx_q=2 max_rx_q=2` on c6i.large and
   `max_tx_q=4 max_rx_q=4` on c6i.xlarge. The build uses the full
   device queue count.

### lwIP notes

1. lwIP runs in no-thread mode with per-core state. Each worker core
   owns its PCBs, timers, and pools.
2. Each core opens its own listener on port 8080 with `SO_REUSEPORT`.
   Hardware RSS steers flows to the matching queue pair.
3. `CONFIG_LWIP_NUM_TCPCON=1024` and
   `CONFIG_LWIP_NUM_TCPLISTENERS=256`.
4. `CONFIG_LWIP_TCP_MSS=1460`. Window scaling, receive buffering, and
   linger are on.
5. Hardware checksum offload is active. The boot console shows
   checksum check and generate for IP, UDP, TCP, and ICMP.
6. `CONFIG_LIBPOSIX_FDTAB_MAXFDS=1024`. This sets the observed
   target socket ceiling. Section 4 explains the effect.
7. The checked-in build configs
   `samples/httpreply-mc/configs/2q-stats.config` and
   `4q-stats.config` carry these values.

### Client and load method

1. One client instance (`c6i.xlarge`, Ubuntu 24.04) ran wrk 4.1.0
   (package 4.1.0-4build2, epoll). The client set `ulimit -n 150000`,
   `ip_local_port_range` 1024 to 65535, and `somaxconn` 65535.
2. Each point ran `wrk -t4 -c<c> -d20s --latency` against
   `http://172.31.16.153:8080/<endpoint>`.
3. Each point ran 3 reps. The report uses the median of reps. The CSV
   also records the per-rep minimum and maximum req/s.
4. A short warmup run (`-t4 -c64 -d10s`) preceded each sweep.
5. The compliance validator gated every target boot. The sweep did not
   start until it passed. See section 3.
6. Client CPU busy percent per rep comes from `/proc/stat` deltas on
   the client. Target and client CloudWatch `CPUUtilization`
   (1-minute detailed datapoints) is attributed per point from the
   recorded rep wall windows.
7. The first runA attempt aborted at client launch. The client
   user-data script passed the AWS 16 KiB inline limit. The harness
   now embeds the validator as compressed base64. The aborted attempt
   produced no measured data. Its log is archived.

### Baseline target

The nginx baseline ran nginx 1.24.0 on Ubuntu 24.04.5 LTS on the same
instance types and the same port 8080. Tuning follows TFB nginx
practice: `worker_processes auto` with CPU affinity, epoll, reuseport,
backlog 65535, `worker_connections 20000`, nofile 200000, keepalive
75s, sendfile, tcp_nopush, access logging off, gzip off, and
open_file_cache. nginx serves the exact TFB bodies from static files,
because `return 200` appends CRLF and breaks byte equality.

Earlier archived baselines used port 80. The sweep re-measured every
baseline on port 8080 on the same VPC paths as the unikernel runs.
This document uses only the 2026-11-18/19 re-measured numbers.

## 3. Compliance gate

The local validator `tfb_validate.sh` checks a live target against the
spec literals: exact body bytes, bare Content-Type, exact
Content-Length, Server and Date headers, Date freshness, the `message`
key case, 404 on unknown paths, keep-alive, a 100-request pipeline, a
10-request mixed pipeline, `Connection: close` handling, split
requests, and HTTP/1.0.

The validator is not the official TFB verifier. It is our own tool at
`samples/httpreply-mc/scripts/tfb_validate.sh`.

The archived per-run logs `tfb_validate.txt` show the outcome for
each boot. The validator runs 23 compliance checks. It adds the
legacy `/` check only for the unikernel.

| Run | PASS | FAIL | Exit |
| :--- | ---: | ---: | ---: |
| runA-large-uk | 24 (includes legacy `/`) | 0 | 0 |
| runB-large-nginx | 23 | 0 | 0 |
| runC-xlarge-uk | 24 (includes legacy `/`) | 0 | 0 |
| runD-xlarge-nginx | 23 | 0 | 0 |

Note: the archived `METHODOLOGY_DEVIATIONS.md` states "23 PASS lines"
for the unikernel and "22" for nginx. The archived validator logs show
24 and 23. The logs are the evidence.

## 4. Results

Each table shows the median of 3 reps. Errors are socket errors summed
over the reps. `LB` marks a lower-bound point. The harness flags a
point `lower_bound` and `client_saturated` when the reps logged socket
errors or the client busy median reached 92 percent. The uk/nginx
column is the unikernel median req/s divided by the nginx median
req/s.

Numbers come from `benchmark_tfb_medians.csv`. The req/s values are
rounded to integers, as in `summary_table.md`.

### c6i.large, JSON (test type 1), 2 workers / 2 queues

| c | uk req/s | uk p50 ms | uk p99 ms | uk errs | nginx req/s | nginx p50 ms | nginx p99 ms | nginx errs | uk/nginx |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 45,831 | 0.343 | 0.412 | 0 | 115,520 | 0.137 | 0.209 | 0 | 0.40 |
| 64 | 142,377 | 0.441 | 0.531 | 0 | 142,332 | 0.441 | 0.56 | 0 | 1.00 |
| 128 | 226,221 | 0.541 | 0.696 | 0 | 141,294 | 0.9 | 1.1 | 0 | 1.60 |
| 256 | 234,853 | 1.04 | 1.3 | 0 | 139,185 | 1.82 | 2.31 | 0 | 1.69 |
| 512 | 230,089 | 2.19 | 2.5 | 0 | 137,891 | 3.7 | 4.32 | 0 | 1.67 |

### c6i.large, plaintext (test type 6), 2 workers / 2 queues

| c | uk req/s | uk p50 ms | uk p99 ms | uk errs | nginx req/s | nginx p50 ms | nginx p99 ms | nginx errs | uk/nginx |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | 233,353 | 1.06 | 1.29 | 0 | 138,152 | 1.84 | 2.28 | 0 | 1.69 |
| 1024 | 233,447 LB | 4.32 | 4.99 | 75 | 133,931 | 7.59 | 10.86 | 0 | 1.74 |
| 4096 | 231,609 LB | 4.29 | 58.93 | 8470 | 123,191 | 33.06 | 37.47 | 0 | 1.88 |

### c6i.xlarge, JSON (test type 1), 4 workers / 4 queues

| c | uk req/s | uk p50 ms | uk p99 ms | uk errs | nginx req/s | nginx p50 ms | nginx p99 ms | nginx errs | uk/nginx |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 | 48,604 | 0.322 | 0.441 | 0 | 78,731 | 0.196 | 0.261 | 0 | 0.62 |
| 64 | 154,857 | 0.397 | 0.586 | 0 | 234,570 | 0.255 | 0.386 | 0 | 0.66 |
| 128 | 248,403 | 0.49 | 0.747 | 0 | 271,613 | 0.467 | 0.657 | 0 | 0.91 |
| 256 | 339,126 | 0.707 | 1.21 | 0 | 271,574 | 0.92 | 1.38 | 0 | 1.25 |
| 512 | 341,095 | 1.43 | 2.23 | 0 | 270,143 | 1.87 | 2.6 | 0 | 1.26 |

### c6i.xlarge, plaintext (test type 6), 4 workers / 4 queues

| c | uk req/s | uk p50 ms | uk p99 ms | uk errs | nginx req/s | nginx p50 ms | nginx p99 ms | nginx errs | uk/nginx |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | 339,043 | 0.712 | 1.19 | 0 | 270,492 | 0.92 | 1.34 | 0 | 1.25 |
| 1024 | 340,046 LB | 2.84 | 4.45 | 4559 | 266,544 | 3.74 | 5.4 | 0 | 1.28 |
| 4096 | 339,177 LB | 2.82 | 4.77 | 16293 | 254,550 | 15.35 | 23.47 | 0 | 1.33 |
| 16384 | 329,903 LB | 2.82 | 5.06 | 18507 | not run | - | - | - | - |

The 16384 point is the stretch point. The nginx plan did not include
it.

### How to read the results

1. The unikernel leads from the mid concurrency range up. The lead is
   1.60x to 1.88x on c6i.large. It is 1.25x to 1.33x on c6i.xlarge.
   The same-binary Linux baseline and the c=16 gap point to an
   application-side cause, not a driver cause.
2. nginx wins at low concurrency. It wins 2.52x at c=16 on
   c6i.large and 1.62x at c=16 on c6i.xlarge. It also wins the JSON
   points at c=64 (1.51x) and c=128 (1.09x) on c6i.xlarge. At JSON
   c=64 on c6i.large the two targets tie.
3. The low-concurrency gap is an open question on the application and
   stack side. The leading explanation is the per-request poll cost
   of the run-to-completion worker loop, against nginx event-driven
   reuse. Phase 1 did not settle it.
4. Every unikernel point at c>=1024 is a lower bound. The fd table
   (`CONFIG_LIBPOSIX_FDTAB_MAXFDS=1024`) caps the target at about 1000
   established sockets. The points show roughly 930 to 1000 requests
   in flight at the median latency. The rest of the connections queue
   outside the target and produce the socket errors. These points
   measure queue behaviour at the fd-table limit. They do not measure
   1024, 4096, or 16384 served connections.
5. The nginx baseline served every requested connection with zero
   socket errors, including c=4096.
6. The 16384 stretch point held at 329,903 req/s with p50 2.82 ms.
   The target did not crash. All 3 reps completed. The median maximum
   latency was 218.71 ms.
7. Client CPU stayed a ceiling risk, not a proven ceiling. CloudWatch
   `CPUUtilization` for the client never exceeded 82.6 percent max at
   any point. The harness `/proc/stat` busy sampler on the client
   reached 93.3 percent median during the c=1024 reps on xlarge. The
   1-minute CloudWatch datapoints smooth short bursts. Target
   CloudWatch CPU never exceeded 86.7 percent max at any point.
8. No unikernel point carried client-side pipelining, so every
   plaintext number is also a lower bound versus the TFB method. See
   section 5.

## 5. Deviations from official TFB methodology

These deviations apply to every number in this report. They restate
the archived `METHODOLOGY_DEVIATIONS.md`.

### Client and load method

1. The client sent no pipelined requests. TFB plaintext drives use
   pipelined clients. Standard wrk 4.x sends one request in flight
   per keep-alive connection. The Ubuntu 24.04 client image ships
   only standard wrk. A patched or Lua-pipelined client was rejected
   as faking methodology. Every plaintext number is therefore a lower
   bound versus TFB methodology.
2. One client instance served every point. TFB uses a multi-machine
   load-generator pool at 256, 1024, 4096, and 16384 concurrency.
   Points where client CPU or socket errors show a ceiling are
   flagged `lower_bound` and `client_saturated` in the CSV and JSON.
3. Each rep ran 20 seconds, with 3 reps per point and the median
   reported. TFB runs one long window. Medians of short reps carry
   more spread than a TFB run. The per-rep minimum and maximum are
   recorded per point.

### Target and hardware

4. One target instance served each run. TFB distributes the workload
   across a cluster of machines for its final score.
5. The instance models are `c6i.large` and `c6i.xlarge` (dedicated
   compute, Intel 8375C). TFB hardware differs. Numbers are never
   comparable across platforms.
6. The compliance check is the local `tfb_validate.sh`. It gates
   every sweep. The official TFB verifier was not used. Section 3
   records the archived per-run PASS counts and one count mismatch in
   the deviation file.
7. Warmup is one short wrk run plus the validator. TFB performs a
   setup and verification phase per framework.

### Unikraft target specifics

8. The worker count follows the established per-size shapes of the
   repo: 2 workers on c6i.large, 4 workers on c6i.xlarge. See
   "Worker Count and Physical Cores" in `samples/httpreply-mc/README.md`.
   The JSON meta records the count each run used.
9. The build under test is check-in `86600f855a`. The kernel SHA-256
   is recorded in each run JSON. No driver change was made for the
   sweep.

### nginx baseline specifics

10. nginx serves the exact TFB bodies from static files (13 bytes and
    27 bytes). Tuning follows TFB nginx practice, as section 2 lists.
11. The validator ran against nginx with the legacy `/` check
    disabled (`CHECK_LEGACY=0`). Only httpreply-mc serves `/`.

### Recorded environment facts

12. Client CPU busy percent per rep comes from `/proc/stat` deltas.
    Target and client CloudWatch `CPUUtilization` (1-minute detailed
    datapoints) is attributed per point from the client-side wall
    windows and recorded in the medians CSV.
13. Availability-zone placement is host-level noise. Fresh placements
    dominate phase repeats, as the project benchmarking-methodology
    note records. Single-placement runs carry that variance.
14. The unikernel holds about 1000 established sockets at once
    (`CONFIG_LIBPOSIX_FDTAB_MAXFDS=1024`). The c=1024, 4096, and
    16384 points show the queue behaviour at that limit, not full
    offered-concurrency service. nginx serves all requested
    connections with zero socket errors.

## 6. External context: published TFB numbers

The official TechEmpower Round 23 results (published 2025-02-24) give
two reference points. Values are per-interval averages computed as
total requests divided by the 15-second interval, taken from the
official Round 23 raw data file
`https://www.techempower.com/benchmarks/results/round23/ph.json`
(run "Continuous Benchmarking Run 2025-01-30", environment
"Citrine").

| TFB Round 23 entry | Test | Concurrency | req/s |
| :--- | :--- | ---: | ---: |
| nginx (C) | plaintext | 256 | 4,852,268 |
| nginx (C) | plaintext | 1024 | 4,008,024 |
| netty (Java) | plaintext | 1024 | 12,242,035 |
| netty (Java) | JSON | 512 | 2,709,587 |

> **Hardware and method differ. These numbers are context only.**
> TFB Round 23 ran on the Citrine testbed with 40-gigabit networking,
> a multi-machine pipelined client pool, and a cluster per platform.
> Our sweep ran one small EC2 instance per target with one
> non-pipelined wrk client. The TFB nginx plaintext figure is about
> 18x our own c6i.xlarge nginx median. That gap measures hardware and
> method, not code. Do not place any number from section 4 beside a
> published TFB number without this disclaimer.

## 7. Open follow-ups before external quoting

Quote this report externally only with its lower-bound flags. Before
any public claim, close these items:

1. Raise the fd table and per-socket memory, then re-measure the
   c>=1024 points. The current 1024-fd cap makes those points lower
   bounds.
2. Investigate the low-concurrency gap (c=16 to 128). Measure the
   per-request poll cost of the run-to-completion loop against an
   event-driven baseline.
3. Run a pipelined client for plaintext, or accept the lower-bound
   label permanently.
4. Cross-check the endpoints with the official TFB verifier, not only
   `tfb_validate.sh`.
5. Strengthen the client side (multiple client instances) so high
   concurrency points stop flagging `client_saturated`.

## 8. Data sources

All artifacts live in the Fossil unversioned store. List them with
`fossil uv ls | grep benchmarks/2026-11-18`. Export one with
`fossil uv export benchmarks/2026-11-18/<path> <dest>`.

1. `benchmarks/2026-11-18/benchmark_tfb_medians.csv` and `.json`:
   machine-readable medians, per-run metadata, and flags.
2. `benchmarks/2026-11-18/summary_table.md`: the full per-point table.
3. `benchmarks/2026-11-18/METHODOLOGY_DEVIATIONS.md`: the deviation
   list.
4. `benchmarks/2026-11-18/teardown_verification.txt`: cleanup proof.
5. `benchmarks/2026-11-18/runA-large-uk/`,
   `runB-large-nginx/`, `runC-xlarge-uk/`, `runD-xlarge-nginx/`:
   raw wrk outputs, validator outputs, per-rep CSVs, step wall
   windows, client diagnostics, boot and final console captures, and
   the per-run sweep JSON with kernel SHA-256 and AMI IDs.
6. `benchmarks/2026-11-18/run*_monitor.log` and
   `runA_attempt1_aborted_userdata_16k_limit.log`: harness-side
   monitor logs.

Related material:

- Plan: Fossil wiki page `Plan-TFB-Phase1`.
- Tickets: `[Ticket e1e4844702]` (plaintext endpoint),
  `[Ticket 42d43d501d]` (JSON endpoint), `[Ticket c3dd68d3d3]`
  (validator), `[Ticket 0a2a42d225]` (EC2 harness),
  `[Ticket 8caafb5454]` (sweep and archive), `[Ticket 8c3327ee16]`
  (this report).
- Audit follow-ups: `[Ticket 21214f6a58]` (Content-Type literals),
  `[Ticket 1478df5bd5]` (listen port 8080), `[Ticket 0f377016a9]`
  (bounded response queue), `[Ticket 2212657476]` (over-the-wire
  validation).
- Harness: `samples/httpreply-mc/scripts/run_tfb_sweep.py`.
- Validator: `samples/httpreply-mc/scripts/tfb_validate.sh`.
- QEMU harness: `samples/httpreply-mc/scripts/qemu_tfb.sh`.

This report is a TFB-compatible standalone report. It is not an
official TFB submission.