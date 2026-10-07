# Run Note: 2-worker instance-size A/B (httpreply-mc)

## Purpose

Measure plan step 7 of the ca72834ec7 investigation plan, tracked as
Ticket cc07b8b438. The question: does the target instance size change
throughput for the 2-worker build? Earlier runs gave 74,366 req/s on
c6i.xlarge and 103,629 req/s on c6i.large. Those runs used different
trunk revisions, so instance size and trunk drift were mixed.

This run changes one thing: the target instance size. One build, one
AMI, one client, one subnet.

## Build

Built from check-in `0ed9f3ffb7` on branch `fix/tests-rss-skew-bench`.
That check-in differs from trunk `774142188a` only in
`tests/test_rss_skew.c`, which is not part of the unikernel. The
measured image is current trunk.

```bash
cp defconfig .config
make olddefconfig && make -j"$(nproc)"
```

`defconfig` is unchanged. It sets the 2-worker shape and keeps the
stats off:

- `CONFIG_UKPLAT_CPU_MAXCOUNT=2`
- `CONFIG_LIBUKNETDEV_MAXNBQUEUES=2`
- `CONFIG_APPHTTPREPLYMC_CONSOLE_STATS` off
- `CONFIG_APPHTTPREPLYMC_NETLOG` off

Image: `build/httpreply-mc_qemu-x86_64`,
SHA-256 `0834d95f97883e326b76c261ae29061c9dea68f01dc4e70fd0be007ba7441b0d`.

## Method

Script: `scripts/run_ec2_size_ab.py`. It uploads one snapshot, registers
one AMI, and runs both target sizes against that AMI.

| Item | Value |
| :--- | :--- |
| Region | `us-east-1` |
| Subnet / AZ | `subnet-0e6ba9e8b1e7dcdf0` / `us-east-1a` |
| Security group | `sg-0ac75f6a09207fcfe` (pre-existing, not created here) |
| Snapshot | `snap-05dc0f512d8119e05` |
| AMI | `ami-08a09cf8ea063eb35` |
| Target | `172.31.16.153`, `c6i.large` then `c6i.xlarge` |
| Client | `172.31.16.161`, `c6i.xlarge` for both arms |
| Load | `wrk -t4 -c25 -d60s --latency`, keep-alive |
| Warm-up | `wrk -t4 -c50 -d10s` before the measured phases |
| Repeats | 3 per size, back to back in one client run |

The client size is fixed at `c6i.xlarge` for both arms. The client is
then the same machine in both runs, and only the target size changes.

Guest shape check, from the target console of the `c6i.xlarge` arm
(`2worker-size_c6i.xlarge_target_console_2026-10-07.txt`):

```
Configured maximum number of cores (2) reached.
ena: rings: 2 tx / 2 rx queues created (msix tx: q0=1 q1=2 rx: q0=1 q1=2)
ena: rss: indirection table (128 entries) configured across 2 queues
httpreply-mc: detected 2 worker cores
```

The 4-vCPU instance ran the same 2-worker, 2-queue shape as the
2-vCPU instance.

## Results

`aws ec2 describe-instance-types` reports the core counts. req/s and
latency are per repeat. p50 and p99 are the mean of the three repeats.
All six phases reported zero socket errors.

| Instance | vCPUs | Physical cores | Threads per core | req/s r1 | req/s r2 | req/s r3 | Mean req/s | p50 (ms) | p99 (ms) | Socket errors |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| c6i.large | 2 | 1 | 2 | 64,419.73 | 66,689.20 | 64,615.06 | 65,241 | 0.358 | 0.505 | 0 |
| c6i.xlarge | 4 | 2 | 2 | 65,127.20 | 65,486.52 | 64,667.57 | 65,094 | 0.362 | 0.505 | 0 |

Spread inside one size: 3.5 percent on c6i.large, 1.3 percent on
c6i.xlarge. Difference between the two means: 147 req/s, or 0.2
percent.

The third c6i.xlarge repeat had latency stalls: max 209.8 ms and a
10.65 ms standard deviation. Its throughput, 64,668 req/s, stayed
inside the other repeats.

CloudWatch CPUUtilization is not usable here. A 60 s phase window
leaves fewer than two full 60 s datapoints, so the probe reports no
CPU figure.

## Findings

- The instance-size effect does not reproduce. On one build, c6i.large
  and c6i.xlarge serve the same number of requests at c=25. The gap
  between the means is 0.2 percent.
- The repeat spread is larger than that gap. The c6i.large spread is
  3.5 percent. This run cannot resolve an effect below about 4 percent.
- The 74,366 versus 103,629 gap in the ticket is not an instance-size
  effect. The two figures came from different trunk revisions. The
  103,629 figure comes from trunk `cf895d8cec` and is not comparable
  with this run.
- Current trunk at c=25 on c6i.large is 65,241 req/s. That is 37
  percent below the 103,629 figure in README table 1. That table is a
  dated record of trunk `cf895d8cec`, so this run does not change it.
  The gap between the two trunks is a separate question.
- This run did not measure c6i.2xlarge, so it says nothing about the
  61,543 req/s figure in plan step 10.

## Conclusion

Keep Ticket cc07b8b438 open. The measured size effect between
c6i.large and c6i.xlarge is smaller than the run-to-run spread, so
this run rules out a large size effect at these two sizes but does not
close the ticket.

Next step: run the same script with `--instances c6i.2xlarge` on the
same build. Plan step 10 reported 61,543 req/s there against 74,366 on
c6i.xlarge. If c6i.2xlarge lands near 65k on current trunk, the size
effect is gone and the ticket can close.

## Artifacts

Raw tool output, CSV, and JSON are in the Fossil unversioned store
under `reports/httpreply-mc_2cpu_size_2026-10-07/`:

- `2worker-size_size_ab_2026-10-07.csv`, `2worker-size_size_ab_2026-10-07.json`
- `2worker-size_instance_types_2026-10-07.json`
- `2worker-size_<instance>_wrk_c25_r<1..3>_2026-10-07.txt`
- `2worker-size_<instance>_wrk_sweep.log_2026-10-07.txt`
- `2worker-size_<instance>_step_times.txt_2026-10-07.txt`
- `2worker-size_<instance>_diag.txt_2026-10-07.txt`
- `2worker-size_<instance>_target_console_2026-10-07.txt`
- `run.log`

## Cleanup

Terminated both targets and both clients. Deregistered
`ami-08a09cf8ea063eb35`. Deleted `snap-05dc0f512d8119e05`. The run
created no security group and no key pair, so it removed none.

Verified with:

```bash
aws ec2 describe-instances --region us-east-1 \
  --filters "Name=instance-state-name,Values=pending,running,stopping,stopped"
aws ec2 describe-volumes --region us-east-1 \
  --filters "Name=status,Values=available,in-use"
```

Both returned empty. `aws ec2 describe-images --owners self` and
`aws ec2 describe-snapshots --owner self` returned no entries.
