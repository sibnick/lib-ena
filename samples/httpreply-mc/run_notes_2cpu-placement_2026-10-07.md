# Run Note: 2-worker placement-controlled instance-size test (httpreply-mc)

## Purpose

Settle Ticket cc07b8b438. The ticket reports that the same 2-worker,
2-queue build serves 74,366 req/s on c6i.xlarge and 103,629 req/s on
c6i.large. Plan step 10 reported a second size effect in the other
direction: 74,366 req/s on c6i.xlarge and 61,543 req/s on c6i.2xlarge.

An earlier run on 2026-10-07 (see
`run_notes_2cpu-instance-size_2026-10-07.md`) found no difference
between c6i.large and c6i.xlarge, but it left the ticket open. The
repeat spread inside one size was larger than the difference between
sizes, so that run could not resolve the effect it was looking for.

This run changes the method. Each measurement gets a fresh target
instance and a fresh client instance, so each one lands on a new host
placement. That measures placement noise directly.

## Method change

`scripts/run_ec2_size_ab.py` gained a `--placements N` option.

- `--repeat N` runs N phases against one target instance. The spread it
  reports is phase-to-phase noise. The instance keeps one host
  placement for all N phases.
- `--placements N` launches a fresh target and a fresh client per
  measurement, and forces `--repeat` to 1. Each measurement gets a new
  host placement.

Comparing two instance types needs the second form. The guest does not
control which physical core a vCPU runs on, and that choice changes the
result more than any setting in this test.

## Build

Built from source at check-in `774142188a`. This is the same source the
earlier size run measured, so the two runs are comparable. The current
trunk carries three later driver fixes, and this run does not measure
them.

```bash
cp defconfig .config
make olddefconfig && make -j"$(nproc)"
```

`defconfig` is unchanged:

- `CONFIG_UKPLAT_CPU_MAXCOUNT=2`
- `CONFIG_LIBUKNETDEV_MAXNBQUEUES=2`
- `CONFIG_APPHTTPREPLYMC_CONSOLE_STATS` off
- `CONFIG_APPHTTPREPLYMC_NETLOG` off

Image: `build/httpreply-mc_qemu-x86_64`,
SHA-256 `2b5b3e37f3105f8925050ca82ff324ec3309a2b1b3dad63fe2988c298a0eb613`.
The same source built from the same path gives the same image hash.

## Setup

| Item | Value |
| :--- | :--- |
| Script | `scripts/run_ec2_size_ab.py --placements 3` |
| Region / AZ | `us-east-1` / `us-east-1a` |
| Subnet / SG | `subnet-0e6ba9e8b1e7dcdf0` / `sg-0ac75f6a09207fcfe` |
| Snapshot / AMI | `snap-0da268ba1c13549f1` / `ami-0be5a94ef76d5426a` |
| Targets | `c6i.xlarge` and `c6i.2xlarge`, three fresh instances each |
| Client | `c6i.xlarge`, a fresh instance per measurement |
| Load | `wrk -t4 -c25 -d60s --latency`, keep-alive |
| Warm-up | `wrk -t4 -c50 -d10s` before the measured phase |
| Measurements | one per placement, six in total |

Guest shape check. All six target consoles report the same shape:

```
Configured maximum number of cores (2) reached.
ena: rings: 2 tx / 2 rx queues created (msix tx: q0=1 q1=2 rx: q0=1 q1=2)
ena: rss: indirection table (128 entries) configured across 2 queues
httpreply-mc: detected 2 worker cores
```

The 4-vCPU and 8-vCPU targets both ran the 2-worker, 2-queue shape.

## Results

Core counts come from `aws ec2 describe-instance-types`. Socket errors
were zero in all six phases.

| Instance | vCPUs | Physical cores | p1 req/s | p2 req/s | p3 req/s | Mean req/s | SD | Spread |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| c6i.xlarge | 4 | 2 | 67,237 | 76,984 | 63,613 | 69,278 | 6,915 | 19.3% |
| c6i.2xlarge | 8 | 4 | 90,667 | 59,332 | 61,382 | 70,460 | 17,529 | 44.5% |

| Instance | Placement | p50 (ms) | p99 (ms) | Max (ms) |
| :--- | :--- | ---: | ---: | ---: |
| c6i.xlarge | p1 | 0.345 | 146.250 | 1450.0 |
| c6i.xlarge | p2 | 0.307 | 0.383 | 3.2 |
| c6i.xlarge | p3 | 0.373 | 0.448 | 9.4 |
| c6i.2xlarge | p1 | 0.248 | 0.361 | 2.2 |
| c6i.2xlarge | p2 | 0.398 | 0.541 | 3.1 |
| c6i.2xlarge | p3 | 0.386 | 0.512 | 2.4 |

The difference between the two means is 1,182 req/s, or 1.7 percent.
The Welch t statistic for the two samples is 0.11. The two ranges
overlap over most of their span.

## Findings

- **The instance-size effect does not exist at these sizes.** On one
  image, c6i.xlarge and c6i.2xlarge serve the same number of requests
  at c=25. The gap between the means is 1.7 percent, and the spread
  inside each size is 19.3 and 44.5 percent.
- **Host placement is the dominant noise source.** Three fresh
  c6i.xlarge instances gave 63,613 to 76,984 req/s. That is a 19.3
  percent range on identical code in the same subnet within ten
  minutes.
- **The old method hid this noise and produced false confidence.** The
  same image, three phases on one c6i.xlarge instance, gave 55,639 /
  56,703 / 56,615 req/s: a 1.9 percent spread. That is about ten
  times tighter than the placement spread. A between-size comparison
  built on those numbers reports a Welch t of 15.1 and looks decisive.
  It is not: the placement offset is a constant inside each arm, so it
  never enters the within-arm spread.
- **Every earlier figure falls inside the placement range.** The
  c6i.xlarge values measured on 2026-10-07 were 65,094 and 56,319
  req/s, and a single-placement c6i.2xlarge run gave 64,365 req/s.
  All three sit inside the 63,613 to 76,984 band from this run. The
  14.3 percent c6i.2xlarge advantage seen in that single-placement
  run was placement noise.
- **One placement stalled.** c6i.xlarge p1 reported p99 146 ms and a
  1,450 ms maximum, while its throughput, 67,237 req/s, stayed inside
  the other placements. That is the host stall signature the ticket
  already describes.

## Limits

Three placements per size is a small sample. This run rules out an
instance-size effect larger than about 20 percent at c=25 on these two
sizes. It cannot resolve a small effect. It says nothing about other
instance families, other concurrency levels, or other trunk revisions.

## Conclusion

Close Ticket cc07b8b438 with the resolution "Works As Intended". The
reported instance-size effect does not reproduce. The figures in the
ticket and in plan step 10 came from single placements on different
trunk revisions, and the run-to-run placement noise in this test is
larger than every difference those runs reported.

Keep `--placements` in the tooling. Any future between-configuration
benchmark must use it, or its error bars are wrong by an order of
magnitude.

## Artifacts

Raw tool output, CSV, and JSON are in the Fossil unversioned store
under `reports/httpreply-mc_2cpu_place_2026-10-07/`:

- `2worker-place_size_ab_2026-10-07.csv`, `2worker-place_size_ab_2026-10-07.json`
- `2worker-place_instance_types_2026-10-07.json`
- `2worker-place-p<N>_<instance>_wrk_c25_2026-10-07.txt` (N = 1..3)
- `2worker-place-p<N>_<instance>_wrk_sweep.log_2026-10-07.txt`
- `2worker-place-p<N>_<instance>_step_times.txt_2026-10-07.txt`
- `2worker-place-p<N>_<instance>_diag.txt_2026-10-07.txt`
- `2worker-place-p<N>_<instance>_target_console_2026-10-07.txt`

The earlier single-placement run for the same ticket is under
`reports/httpreply-mc_2cpu_size_2026-10-07/`.

## Cleanup

All six targets and all six clients were terminated during the run.
Deregistered `ami-0be5a94ef76d5426a`. Deleted `snap-0da268ba1c13549f1`.
The run created no security group and no key pair, so it removed none.

Verified after teardown:

```bash
aws ec2 describe-instances --region us-east-1 \
  --filters "Name=instance-state-name,Values=pending,running,stopping,stopped"
aws ec2 describe-volumes --region us-east-1 \
  --filters "Name=status,Values=available,in-use"
aws ec2 describe-images --owners self --region us-east-1
aws ec2 describe-snapshots --owner self --region us-east-1
```

All four returned empty.

## Script fixes made during this work

- `run_ec2_lowconc_probe.py`: the poll after `run-instances` now
  tolerates a transient AWS CLI failure and retries. It also
  terminates the instance it launched if that instance never reaches
  the running state. Before this change, one failed poll ended a
  benchmark run and left the peer instance running with no owner to
  clean it up.
- `run_ec2_size_ab.py`: the target console fetch now retries an empty
  response body. AWS exposes the console body only after the instance
  terminates, so the fetch that ran before termination saved nothing.
