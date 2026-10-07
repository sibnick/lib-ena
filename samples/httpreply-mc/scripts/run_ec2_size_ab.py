#!/usr/bin/env python3
"""
Instance-size A/B for [Ticket cc07b8b438].

One image and one AMI serve every target size, so the guest build is
identical across the arms. The script measures closed-loop wrk at the
given concurrency levels, N repeats per size, and writes raw wrk
output plus CSV and JSON to the output directory.

The client instance type is fixed for all arms. Only the target size
changes. Target and client stay in one subnet, so one availability
zone. The script reads vCPU, physical-core, and thread-per-core counts
from aws ec2 describe-instance-types and puts them in the output.

This script does not build the image and does not change any config.
Build the image first. For the 2-worker shape set
CONFIG_UKPLAT_CPU_MAXCOUNT and CONFIG_LIBUKNETDEV_MAXNBQUEUES to the
same value. Keep CONFIG_APPHTTPREPLYMC_CONSOLE_STATS and
CONFIG_APPHTTPREPLYMC_NETLOG off: a stats build changes throughput.

Usage:
  python3 scripts/run_ec2_size_ab.py --kernel PATH
      [--instances c6i.large,c6i.xlarge] [--client-instance TYPE]
      [--concurrency 25] [--repeat 3] [--variant LABEL]
      [--outdir DIR] [--build-rev REV] [--dry-run]
"""

import argparse
import csv
import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_ec2_verification import (  # noqa: E402
    AWS_REGION,
    CLIENT_PRIVATE_IP,
    SG_ID,
    SUBNET_ID,
    TARGET_PRIVATE_IP,
    UBUNTU_AMI,
    create_bootable_disk,
    register_ami,
    run_cmd,
    upload_ebs_snapshot,
)
from run_ec2_lowconc_probe import (  # noqa: E402
    DURATION,
    THREADS,
    client_user_data,
    launch_instance,
    parse_step_times,
    phase_cpu,
    phase_file_tag,
)
from run_ec2_4cpu_benchmark import http_get, parse_wrk_closed_loop  # noqa: E402

SAMPLE_DIR = Path(__file__).resolve().parent.parent
DEFAULT_INSTANCES = ["c6i.large", "c6i.xlarge"]
DEFAULT_CONCS = [25]
DEFAULT_KERNEL = SAMPLE_DIR / "build/httpreply-mc_qemu-x86_64"
DEFAULT_OUTDIR = "/tmp/opencode/cc07b8b438"
DATE_STR = time.strftime("%Y-%m-%d")
SWEEP_TIMEOUT = 1800

CSV_FIELDS = [
    "instance_type",
    "vcpus",
    "physical_cores",
    "threads_per_core",
    "availability_zone",
    "concurrency",
    "repeat",
    "requests_sec",
    "latency_avg_ms",
    "latency_stdev_ms",
    "latency_max_ms",
    "p50_ms",
    "p90_ms",
    "p99_ms",
    "transfer_kb_sec",
    "total_requests",
    "socket_errors",
    "cpu_avg_pct",
    "cpu_max_pct",
    "cpu_datapoints",
    "target_instance_id",
    "client_instance_id",
    "ami_id",
    "snapshot_id",
    "build_rev",
    "kernel_sha256",
]


def comma_list(text):
    """Parse 'c6i.large,c6i.xlarge' into a list."""
    vals = [x.strip() for x in text.split(",") if x.strip()]
    if not vals:
        raise argparse.ArgumentTypeError("no values given")
    return vals


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Instance-size A/B for [Ticket cc07b8b438]."
    )
    p.add_argument(
        "--instances",
        type=comma_list,
        default=DEFAULT_INSTANCES,
        help="comma-separated target instance types (default: %(default)s)",
    )
    p.add_argument(
        "--client-instance",
        default=None,
        help="instance type for the wrk client, the same for every arm "
        "(default: the largest type in --instances)",
    )
    p.add_argument(
        "--concurrency",
        type=comma_list,
        default=[str(c) for c in DEFAULT_CONCS],
        help="comma-separated wrk concurrency levels (default: 25)",
    )
    p.add_argument(
        "--repeat",
        type=int,
        metavar="N",
        default=3,
        help="measurement(s) per concurrency level per size (default: %(default)s)",
    )
    p.add_argument(
        "--kernel",
        type=Path,
        default=DEFAULT_KERNEL,
        help="path to a built unikernel image (default: %(default)s)",
    )
    p.add_argument(
        "--variant",
        default="sizeab",
        help="label for output names and instance Name tags (default: %(default)s)",
    )
    p.add_argument(
        "--outdir",
        type=Path,
        default=DEFAULT_OUTDIR,
        help="directory for all output files (default: %(default)s)",
    )
    p.add_argument(
        "--build-rev",
        default="",
        help="source revision the image was built from, recorded in the output",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="print the plan, do not call aws",
    )
    args = p.parse_args(argv)
    args.concurrency = [int(c) for c in args.concurrency]
    if args.repeat < 1:
        p.error("--repeat must be 1 or more")
    if args.client_instance is None:
        args.client_instance = max(args.instances, key=instance_vcpus)
    return args


def instance_vcpus(instance_type):
    """Guess the vCPU count from the size suffix, for the client default."""
    suffix = instance_type.rsplit(".", 1)[-1]
    base = {
        "nano": 2,
        "micro": 2,
        "small": 2,
        "medium": 2,
        "large": 2,
        "xlarge": 4,
        "2xlarge": 8,
        "4xlarge": 16,
        "8xlarge": 32,
        "16xlarge": 64,
        "metal": 96,
    }
    digits = ""
    for ch in suffix:
        if ch.isdigit():
            digits += ch
        else:
            break
    mult = int(digits) if digits else 1
    name = suffix[len(digits) :] if digits else suffix
    return base.get(name, 2) * mult


def describe_instance_types(types):
    """Return {type: {vcpus, physical_cores, threads_per_core}}."""
    out = run_cmd(
        ["aws", "ec2", "describe-instance-types", "--instance-types"]
        + list(types)
        + ["--region", AWS_REGION, "--output", "json"]
    )
    info = {}
    for it in json.loads(out)["InstanceTypes"]:
        t = it["InstanceType"]
        info[t] = {
            "vcpus": it["VCpuInfo"]["DefaultVCpus"],
            "physical_cores": it["VCpuInfo"]["DefaultCores"],
            "threads_per_core": it["VCpuInfo"]["DefaultThreadsPerCore"],
        }
    return info


def describe_subnet_az(subnet_id):
    """Return the availability zone of a subnet."""
    out = run_cmd(
        [
            "aws",
            "ec2",
            "describe-subnets",
            "--subnet-ids",
            subnet_id,
            "--region",
            AWS_REGION,
            "--output",
            "text",
            "--query",
            "Subnets[0].AvailabilityZone",
        ]
    )
    return out.strip()


def sha256_file(path):
    out = run_cmd(["sha256sum", str(path)])
    return out.split()[0]


def run_arm(ami_id, instance_type, meta, args, client_meta, az):
    """Run one target size and return one row per repeat."""
    print("\n" + "#" * 60)
    print(f"# ARM: target={instance_type} client={args.client_instance}")
    print(
        f"# vcpus={meta['vcpus']} physical_cores={meta['physical_cores']} "
        f"threads_per_core={meta['threads_per_core']}"
    )
    print("#" * 60)

    probe_args = argparse.Namespace(
        concurrency=args.concurrency,
        repeat=args.repeat,
        stats=False,
    )
    target_id = client_id = None
    rows = []
    try:
        target_id, _ = launch_instance(
            ami_id,
            TARGET_PRIVATE_IP,
            instance_type,
            f"unikraft-mc-{args.variant}-{instance_type}-target",
            monitoring=True,
        )
        client_id, client_pub = launch_instance(
            UBUNTU_AMI,
            CLIENT_PRIVATE_IP,
            args.client_instance,
            f"wrk-mc-{args.variant}-client",
            user_data=client_user_data(probe_args),
        )

        start = time.time()
        last = ""
        done = False
        while time.time() - start < SWEEP_TIMEOUT:
            try:
                cur = http_get(client_pub, "wrk_sweep.log", timeout=5)
                if cur != last:
                    sys.stdout.write(cur[len(last) :])
                    sys.stdout.flush()
                    last = cur
                if "ALL_DONE" in cur or "GATE_FAIL" in cur:
                    done = True
                    break
            except Exception:
                pass
            time.sleep(5)
        if not done:
            print(f"[WARN] {instance_type}: sweep did not finish in {SWEEP_TIMEOUT} s")

        windows = {}
        for name in ("wrk_sweep.log", "step_times.txt", "diag.txt"):
            try:
                text = http_get(client_pub, name, timeout=15)
            except Exception as e:
                print(f"[WARN] fetch {name} failed: {e}")
                text = ""
            (
                args.outdir / f"{args.variant}_{instance_type}_{name}_{DATE_STR}.txt"
            ).write_text(text)
            if name == "step_times.txt":
                windows = parse_step_times(text)

        for c in args.concurrency:
            for r in range(1, args.repeat + 1):
                tag = phase_file_tag(c, r, args.repeat)
                try:
                    content = http_get(client_pub, f"wrk_{tag}.txt", timeout=15)
                except Exception as e:
                    print(f"[WARN] fetch wrk_{tag}.txt failed: {e}")
                    content = ""
                (
                    args.outdir / f"{args.variant}_{instance_type}_wrk_{tag}_"
                    f"{DATE_STR}.txt"
                ).write_text(content)
                p = parse_wrk_closed_loop(content)

                avg = mx = None
                npts = 0
                win = windows.get((c, r))
                if win:
                    avg, mx, npts = phase_cpu(target_id, win[0], win[1])
                else:
                    print(f"[size-ab] c={c} r={r}: no phase window recorded")

                rows.append(
                    {
                        "instance_type": instance_type,
                        "vcpus": meta["vcpus"],
                        "physical_cores": meta["physical_cores"],
                        "threads_per_core": meta["threads_per_core"],
                        "availability_zone": az,
                        "concurrency": c,
                        "repeat": r,
                        "cpu_avg_pct": avg,
                        "cpu_max_pct": mx,
                        "cpu_datapoints": npts,
                        "target_instance_id": target_id,
                        "client_instance_id": client_id,
                        "ami_id": ami_id,
                        "build_rev": args.build_rev,
                        "kernel_sha256": args.kernel_sha256,
                        **p,
                    }
                )
                print(
                    f"[size-ab] {instance_type} c={c} r={r}: "
                    f"{p['requests_sec']:.0f} req/s "
                    f"p50={p['p50_ms']:.3f}ms p99={p['p99_ms']:.3f}ms "
                    f"errors={p['socket_errors']} cpu_avg={avg}%"
                )

        time.sleep(5)
        out = run_cmd(
            [
                "aws",
                "ec2",
                "get-console-output",
                "--instance-id",
                target_id,
                "--region",
                AWS_REGION,
                "--output",
                "text",
            ]
        )
        (
            args.outdir / f"{args.variant}_{instance_type}_target_console_"
            f"{DATE_STR}.txt"
        ).write_text(out)
        print(f"[size-ab] {instance_type} target console saved")
    finally:
        ids = [i for i in (target_id, client_id) if i]
        if ids:
            run_cmd(
                ["aws", "ec2", "terminate-instances", "--instance-ids"]
                + ids
                + ["--region", AWS_REGION]
            )
            while True:
                d = run_cmd(
                    ["aws", "ec2", "describe-instances", "--instance-ids"]
                    + ids
                    + [
                        "--query",
                        "Reservations[].Instances[].State.Name",
                        "--region",
                        AWS_REGION,
                        "--output",
                        "json",
                    ]
                )
                if all(s in ("terminated", "shutting-down") for s in json.loads(d)):
                    print("[SUCCESS] instances terminated")
                    break
                time.sleep(5)
    return rows


def summarize(rows):
    """Print the per-size mean, min and max of req/s."""
    by_size = {}
    for r in rows:
        by_size.setdefault(r["instance_type"], []).append(r)
    print("\n=== summary, req/s per size ===")
    for t, rs in by_size.items():
        vals = [r["requests_sec"] for r in rs]
        if not vals:
            continue
        print(
            f"{t}: n={len(vals)} cores={rs[0]['physical_cores']} "
            f"vcpus={rs[0]['vcpus']} "
            f"mean={statistics.mean(vals):.0f} "
            f"min={min(vals):.0f} max={max(vals):.0f} "
            f"spread={(max(vals) - min(vals)) / statistics.mean(vals) * 100:.1f}%"
        )


def print_dry_run(args, meta, client_meta, az):
    print("[DRY-RUN] plan (no aws calls, no instances):")
    print(f"  kernel : {args.kernel}")
    print(f"  outdir : {args.outdir}")
    print(
        f"  client : {args.client_instance} at {CLIENT_PRIVATE_IP}, same for every arm"
    )
    print(f"  subnet : {SUBNET_ID} az={az} sg={SG_ID}")
    print(
        f"  repeat : {args.repeat}, concs {args.concurrency}, "
        f"wrk -t{THREADS} -d{DURATION} --latency, stats off"
    )
    for t in args.instances:
        m = meta[t]
        print(
            f"  target : {t} at {TARGET_PRIVATE_IP} "
            f"vcpus={m['vcpus']} cores={m['physical_cores']} "
            f"threads_per_core={m['threads_per_core']}"
        )
    print("  one snapshot upload and one AMI registration serve all arms")


def main():
    args = parse_args()
    if not args.kernel.exists() and not args.dry_run:
        print(f"[ERR] kernel not found: {args.kernel}")
        sys.exit(1)
    args.kernel_sha256 = sha256_file(args.kernel) if args.kernel.exists() else ""
    args.outdir.mkdir(parents=True, exist_ok=True)

    meta = describe_instance_types(args.instances + [args.client_instance])
    az = describe_subnet_az(SUBNET_ID)
    client_meta = meta[args.client_instance]
    if args.dry_run:
        print_dry_run(args, meta, client_meta, az)
        return

    (args.outdir / f"{args.variant}_instance_types_{DATE_STR}.json").write_text(
        json.dumps(meta, indent=2)
    )

    disk_raw = create_bootable_disk(args.kernel, SAMPLE_DIR)
    snapshot_id = upload_ebs_snapshot(disk_raw, SAMPLE_DIR)
    ami_id = register_ami(snapshot_id)

    rows = []
    try:
        for t in args.instances:
            rows += run_arm(ami_id, t, meta[t], args, client_meta, az)
            time.sleep(15)
    finally:
        run_cmd(
            [
                "aws",
                "ec2",
                "deregister-image",
                "--image-id",
                ami_id,
                "--region",
                AWS_REGION,
            ]
        )
        run_cmd(
            [
                "aws",
                "ec2",
                "delete-snapshot",
                "--snapshot-id",
                snapshot_id,
                "--region",
                AWS_REGION,
            ]
        )
        print("[size-ab] AMI deregistered and snapshot deleted")

    for r in rows:
        r["snapshot_id"] = snapshot_id

    jpath = args.outdir / f"{args.variant}_size_ab_{DATE_STR}.json"
    cpath = args.outdir / f"{args.variant}_size_ab_{DATE_STR}.csv"
    jpath.write_text(
        json.dumps(
            {
                "ticket": "cc07b8b438",
                "date": DATE_STR,
                "region": AWS_REGION,
                "subnet": SUBNET_ID,
                "availability_zone": az,
                "security_group": SG_ID,
                "ami_id": ami_id,
                "snapshot_id": snapshot_id,
                "kernel": str(args.kernel),
                "kernel_sha256": args.kernel_sha256,
                "build_rev": args.build_rev,
                "client_instance": args.client_instance,
                "client_meta": client_meta,
                "instance_meta": meta,
                "concurrency": args.concurrency,
                "repeat": args.repeat,
                "wrk_threads": THREADS,
                "wrk_duration": DURATION,
                "rows": rows,
            },
            indent=2,
        )
    )
    with cpath.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print(f"[SUCCESS] wrote {cpath} and {jpath}")
    summarize(rows)


if __name__ == "__main__":
    main()
