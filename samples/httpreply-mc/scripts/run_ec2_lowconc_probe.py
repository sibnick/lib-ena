#!/usr/bin/env python3
"""
Low-concurrency probe for [Ticket ca72834ec7].

Runs a prebuilt httpreply-mc image on a chosen instance type and
measures closed-loop wrk at chosen concurrency levels. Compare the
numbers across runs to see whether the core and queue count causes
the low-concurrency regression.

This script does not build or edit any defconfig. Build the images
first. For the A/B test, the two images must differ only in
CONFIG_UKPLAT_CPU_MAXCOUNT and CONFIG_LIBUKNETDEV_MAXNBQUEUES. The
second one sets nb_rx_queues and nb_tx_queues in the netdev config, so
it is the switch that changes the active queue count.
CONFIG_LIBENA_MAX_QUEUES only sizes the driver ring arrays, and must
stay at or above the queue count. Pass one image per run with
--kernel, and label each run with --variant.

With --stats, the client fetches GET /__log from the target after the
wrk phases, and this script saves the log unchanged. That needs
CONFIG_APPHTTPREPLYMC_NETLOG=y on the target build. The heartbeat
lines need CONFIG_APPHTTPREPLYMC_CONSOLE_STATS=y. Both default to n
in samples/httpreply-mc/Config.uk. The heartbeat prints CUMULATIVE
counters, not rates. This script does not parse them. Difference
consecutive heartbeats yourself.

CPU usage comes from CloudWatch CPUUtilization with detailed
monitoring. The script uses only 60 s datapoints that fall fully
inside a phase window. It prints how many datapoints it used. Fewer
than two datapoints is not usable.

Use --repeat N to measure each concurrency level N times in one
launch. Each repeat writes its own wrk output file and its own
STEP_WINDOW line. This keeps each CPU window matched to its phase.
It also avoids a second snapshot upload and AMI registration.

Usage:
  python3 scripts/run_ec2_lowconc_probe.py [--instance TYPE]
      [--concurrency 25,50] [--repeat N] [--kernel PATH]
      [--variant LABEL] [--outdir DIR] [--stats] [--dry-run]
"""

import argparse
import json
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_ec2_verification import (  # noqa: E402
    AWS_REGION,
    SG_ID,
    SUBNET_ID,
    TARGET_PRIVATE_IP,
    CLIENT_PRIVATE_IP,
    UBUNTU_AMI,
    run_cmd,
    create_bootable_disk,
    upload_ebs_snapshot,
    register_ami,
    wait_for_ip_free,
)
from run_ec2_4cpu_benchmark import parse_wrk_closed_loop, http_get  # noqa: E402

SAMPLE_DIR = Path(__file__).resolve().parent.parent
DEFAULT_INSTANCE = "c6i.xlarge"
DEFAULT_CONCS = [25, 50]
DEFAULT_KERNEL = SAMPLE_DIR / "build/httpreply-mc_qemu-x86_64"
DEFAULT_OUTDIR = "/tmp/opencode/ca72834ec7"
DATE_STR = time.strftime("%Y-%m-%d")
THREADS = 4
DURATION = "60s"


def comma_ints(text):
    """Parse '25,50' into [25, 50]."""
    vals = [int(x) for x in text.split(",") if x.strip()]
    if not vals:
        raise argparse.ArgumentTypeError("no concurrency levels given")
    return vals


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Low-concurrency probe for [Ticket ca72834ec7]."
    )
    p.add_argument(
        "--instance",
        default=DEFAULT_INSTANCE,
        help="EC2 instance type (default: %(default)s)",
    )
    p.add_argument(
        "--concurrency",
        type=comma_ints,
        default=DEFAULT_CONCS,
        help="comma-separated wrk concurrency levels (default: 25,50)",
    )
    p.add_argument(
        "--repeat",
        type=int,
        metavar="N",
        default=1,
        help="measure each concurrency level N times in one launch "
        "(default: %(default)s)",
    )
    p.add_argument(
        "--kernel",
        type=Path,
        default=DEFAULT_KERNEL,
        help="path to a built unikernel image (default: %(default)s)",
    )
    p.add_argument(
        "--variant",
        default="lowconc",
        help="label for output names and instance Name tags (default: %(default)s)",
    )
    p.add_argument(
        "--outdir",
        type=Path,
        default=DEFAULT_OUTDIR,
        help="directory for all output files (default: %(default)s)",
    )
    p.add_argument(
        "--stats",
        action="store_true",
        help="fetch GET /__log from the target after the wrk "
        "phases and save it unchanged",
    )
    p.add_argument(
        "--dry-run",
        action="store_true",
        help="print the plan and the aws commands, do not call aws",
    )
    args = p.parse_args(argv)
    if args.repeat < 1:
        p.error("--repeat must be 1 or more")
    return args


def phase_file_tag(c, r, repeat):
    """Return the wrk file tag for one phase.

    A single run keeps the old names. A repeated run adds _r<N>. The
    client user-data script builds the same names in bash.
    """
    if repeat > 1:
        return f"c{c}_r{r}"
    return f"c{c}"


def run_instances_cmd(ami_id, ip, instance_type, tag, user_data=None, monitoring=False):
    cmd = [
        "aws",
        "ec2",
        "run-instances",
        "--image-id",
        ami_id,
        "--instance-type",
        instance_type,
        "--security-group-ids",
        SG_ID,
        "--subnet-id",
        SUBNET_ID,
        "--private-ip-address",
        ip,
        "--count",
        "1",
        "--tag-specifications",
        f"ResourceType=instance,Tags=[{{Key=Name,Value={tag}}}]",
        "--region",
        AWS_REGION,
        "--output",
        "json",
    ]
    if monitoring:
        # Detailed monitoring gives 1-minute CPUUtilization datapoints.
        cmd += ["--monitoring", "Enabled=true"]
    if user_data:
        cmd += ["--user-data", user_data]
    return cmd


def launch_instance(ami_id, ip, instance_type, tag, user_data=None, monitoring=False):
    wait_for_ip_free(ip)
    out = run_cmd(
        run_instances_cmd(ami_id, ip, instance_type, tag, user_data, monitoring)
    )
    instance_id = json.loads(out)["Instances"][0]["InstanceId"]
    print(f"[SUCCESS] Launched {tag}: {instance_id}")
    while True:
        d = run_cmd(
            [
                "aws",
                "ec2",
                "describe-instances",
                "--instance-ids",
                instance_id,
                "--region",
                AWS_REGION,
                "--output",
                "json",
            ]
        )
        inst = json.loads(d)["Reservations"][0]["Instances"][0]
        if inst["State"]["Name"] == "running" and inst.get("PublicIpAddress"):
            print(f"[SUCCESS] {tag} RUNNING. Public IP: {inst['PublicIpAddress']}")
            return instance_id, inst["PublicIpAddress"]
        time.sleep(3)


def client_user_data(args):
    stats_block = ""
    if args.stats:
        stats_block = f"""echo "NETLOG: fetching /__log from target $(date)" | tee -a /root/wrk_sweep.log
curl -s --max-time 30 http://{TARGET_PRIVATE_IP}/__log > /root/netlog.txt || echo "NETLOG_FETCH_FAILED" > /root/netlog.txt
echo "NETLOG_SAVED $(wc -c < /root/netlog.txt) bytes" | tee -a /root/wrk_sweep.log
"""
    repeat = args.repeat
    if repeat > 1:
        # These bash names must match phase_file_tag(). Use ${c} and
        # ${r} so bash does not read $c_r as one variable name.
        r_label = " r=$r"
        file_suffix = "_r${r}"
    else:
        r_label = ""
        file_suffix = ""
    return f"""#!/bin/bash
set -e
exec > >(tee -a /root/diag.txt) 2>&1
export DEBIAN_FRONTEND=noninteractive
for i in 1 2 3 4 5 6 7 8; do
    apt-get update -y && break || sleep 15
done
for i in 1 2 3 4 5 6 7 8; do
    apt-get install -y curl wrk && break || sleep 15
done
echo "WRK_READY $(date)"
cd /root
python3 -m http.server 80 &
echo "GATE: waiting for target http://{TARGET_PRIVATE_IP}/ ..."
OK=0
for i in $(seq 1 120); do
    CODE=$(curl -s -o /dev/null --max-time 5 -w "%{{http_code}}" http://{TARGET_PRIVATE_IP}/ || echo 000)
    if [ "$CODE" = "200" ]; then OK=1; echo "GATE_OK tries=$i"; break; fi
    sleep 2
done
if [ "$OK" != "1" ]; then
    echo "GATE_FAIL" > /root/wrk_sweep.log
    echo "ALL_DONE" >> /root/wrk_sweep.log
    exit 0
fi
echo "WARMUP c50 10s" | tee -a /root/wrk_sweep.log
timeout --kill-after=5s 20s wrk -t{THREADS} -c50 -d10s http://{TARGET_PRIVATE_IP}/ > /root/wrk_warm.txt 2>&1 || true
sleep 5
for c in {" ".join(str(c) for c in args.concurrency)}; do
    for r in $(seq 1 {repeat}); do
        T0=$(date +%s)
        echo "=== STEP c=$c{r_label} start=$T0 $(date) ===" | tee -a /root/wrk_sweep.log
        timeout --kill-after=5s 90s wrk -t{THREADS} -c$c -d{DURATION} --latency http://{TARGET_PRIVATE_IP}/ > /root/wrk_c${{c}}{file_suffix}.txt 2>&1 || true
        T1=$(date +%s)
        echo "STEP_WINDOW c=$c{r_label} start=$T0 end=$T1" >> /root/step_times.txt
        echo "=== STEP c=$c{r_label} end=$T1 $(date) ===" | tee -a /root/wrk_sweep.log
        sleep 5
    done
done
{stats_block}echo "ALL_DONE $(date)" | tee -a /root/wrk_sweep.log
"""


def parse_step_times(text):
    """Return {(concurrency, repeat): (start_epoch, end_epoch)}.

    A line without r= is a single-run phase. This parser reads it as
    repeat 1.
    """
    windows = {}
    for line in text.splitlines():
        m = re.match(
            r"STEP_WINDOW c=(\d+)(?: r=(\d+))? start=(\d+) end=(\d+)",
            line,
        )
        if m:
            r = int(m.group(2)) if m.group(2) else 1
            windows[(int(m.group(1)), r)] = (int(m.group(3)), int(m.group(4)))
    return windows


def phase_cpu(instance_id, t0, t1):
    """Return (avg, max, count) CPUUtilization from 60 s datapoints.

    Only datapoints whose whole 60 s bucket falls inside [t0, t1]
    count. avg is None when fewer than two datapoints qualify.
    """
    start = datetime.fromtimestamp(t0, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    end = datetime.fromtimestamp(t1, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    try:
        out = run_cmd(
            [
                "aws",
                "cloudwatch",
                "get-metric-statistics",
                "--namespace",
                "AWS/EC2",
                "--metric-name",
                "CPUUtilization",
                "--dimensions",
                f"Name=InstanceId,Value={instance_id}",
                "--start-time",
                start,
                "--end-time",
                end,
                "--period",
                "60",
                "--statistics",
                "Average",
                "Maximum",
                "--region",
                AWS_REGION,
                "--output",
                "json",
            ]
        )
        dps = json.loads(out).get("Datapoints", [])
        in_win = []
        for d in dps:
            ts_str = d["Timestamp"]
            if hasattr(ts_str, "timestamp"):
                ts = ts_str.timestamp()
            else:
                ts = datetime.fromisoformat(
                    str(ts_str).replace("Z", "+00:00")
                ).timestamp()
            if t0 <= ts and ts + 60 <= t1:
                in_win.append(d)
        if len(in_win) < 2:
            return None, None, len(in_win)
        avg = sum(d["Average"] for d in in_win) / len(in_win)
        mx = max(d["Maximum"] for d in in_win)
        return round(avg, 1), round(mx, 1), len(in_win)
    except Exception as e:
        print(f"[WARN] cloudwatch query failed: {e}")
        return None, None, 0


def print_dry_run(args, kernel):
    print("[DRY-RUN] plan (no aws calls, no instances):")
    print(
        f"  target : {args.instance} at {TARGET_PRIVATE_IP}, "
        f"tag unikraft-mc-{args.variant}-target"
    )
    print(
        f"  client : {args.instance} at {CLIENT_PRIVATE_IP}, "
        f"tag wrk-mc-{args.variant}-client"
    )
    print(f"  kernel : {kernel}")
    print(f"  outdir : {args.outdir}")
    print(f"  concs  : {', '.join(str(c) for c in args.concurrency)}")
    print(f"  repeat : {args.repeat} measurement(s) per concurrency level")
    if args.repeat > 1:
        print("  files  : one wrk file per repeat, wrk_c<c>_r<r>.txt on the client")
    print(
        f"  phase  : wrk -t{THREADS} -c<c> -d{DURATION} --latency, "
        "plus a c50 10s warmup"
    )
    print(
        f"  stats  : {'on: fetch GET /__log after the wrk phases' if args.stats else 'off'}"
    )
    print("[DRY-RUN] aws commands this script would run:")
    print(
        "  "
        + " ".join(
            run_instances_cmd(
                "<new-ami>",
                TARGET_PRIVATE_IP,
                args.instance,
                f"unikraft-mc-{args.variant}-target",
                monitoring=True,
            )
        )
    )
    print(
        "  "
        + " ".join(
            run_instances_cmd(
                UBUNTU_AMI,
                CLIENT_PRIVATE_IP,
                args.instance,
                f"wrk-mc-{args.variant}-client",
                user_data="<client script>",
            )
        )
    )
    print(
        "  aws cloudwatch get-metric-statistics --namespace AWS/EC2 "
        f"--metric-name CPUUtilization --dimensions Name=InstanceId,Value=<target> "
        f"--period 60 --statistics Average Maximum --region {AWS_REGION} "
        "(once per phase window)"
    )
    if args.stats:
        print(
            f"  (on client) curl http://{TARGET_PRIVATE_IP}/__log -> /root/netlog.txt"
        )
    print(
        f"  aws ec2 get-console-output --instance-id <target> --region {AWS_REGION} --output text"
    )
    print(
        "  aws ec2 terminate-instances --instance-ids <target> <client> "
        f"--region {AWS_REGION}"
    )
    print(f"  aws ec2 deregister-image --image-id <ami> --region {AWS_REGION}")
    print(f"  aws ec2 delete-snapshot --snapshot-id <snapshot> --region {AWS_REGION}")
    print(
        "[DRY-RUN] upload_ebs_snapshot and register_ami would also issue "
        "aws ebs and aws ec2 commands."
    )


def main():
    args = parse_args()
    kernel = args.kernel
    if args.dry_run:
        print_dry_run(args, kernel)
        return
    if not kernel.exists():
        print(f"[ERR] kernel not found: {kernel}")
        sys.exit(1)
    outdir = args.outdir
    outdir.mkdir(parents=True, exist_ok=True)
    target_id = client_id = ami_id = snapshot_id = None
    try:
        disk_raw = create_bootable_disk(kernel, SAMPLE_DIR)
        snapshot_id = upload_ebs_snapshot(disk_raw, SAMPLE_DIR)
        ami_id = register_ami(snapshot_id)
        target_id, _ = launch_instance(
            ami_id,
            TARGET_PRIVATE_IP,
            args.instance,
            f"unikraft-mc-{args.variant}-target",
            monitoring=True,
        )
        client_id, client_pub = launch_instance(
            UBUNTU_AMI,
            CLIENT_PRIVATE_IP,
            args.instance,
            f"wrk-mc-{args.variant}-client",
            user_data=client_user_data(args),
        )

        start = time.time()
        last = ""
        while time.time() - start < 1200:
            try:
                cur = http_get(client_pub, "wrk_sweep.log", timeout=5)
                if cur != last:
                    sys.stdout.write(cur[len(last) :])
                    sys.stdout.flush()
                    last = cur
                if "ALL_DONE" in cur or "GATE_FAIL" in cur:
                    break
            except Exception:
                pass
            time.sleep(5)

        try:
            step_txt = http_get(client_pub, "step_times.txt", timeout=10)
        except Exception as e:
            print(f"[WARN] fetch step_times.txt failed: {e}")
            step_txt = ""
        (outdir / f"{args.variant}_step_times_{DATE_STR}.txt").write_text(step_txt)
        windows = parse_step_times(step_txt)

        for c in args.concurrency:
            for r in range(1, args.repeat + 1):
                tag = phase_file_tag(c, r, args.repeat)
                if args.repeat > 1:
                    label = f"c={c} r={r}"
                else:
                    label = f"c={c}"
                try:
                    content = http_get(client_pub, f"wrk_{tag}.txt", timeout=15)
                except Exception as e:
                    print(f"[WARN] fetch wrk_{tag}.txt failed: {e}")
                    content = ""
                (outdir / f"{args.variant}_wrk_{tag}_{DATE_STR}.txt").write_text(
                    content
                )
                p = parse_wrk_closed_loop(content)
                print(
                    f"[probe] {label}: {p['requests_sec']:.0f} req/s, "
                    f"avg={p['latency_avg_ms']:.2f}ms p50={p['p50_ms']:.2f}ms "
                    f"p99={p['p99_ms']:.2f}ms errors={p['socket_errors']}"
                )

                win = windows.get((c, r))
                if not win:
                    print(f"[probe] {label}: no phase window recorded, CPU not usable")
                    continue
                avg, mx, n = phase_cpu(target_id, win[0], win[1])
                if avg is None:
                    print(
                        f"[probe] {label}: only {n} datapoint(s) fall fully "
                        "inside the phase window, CPU not usable"
                    )
                else:
                    print(
                        f"[probe] {label}: cpu avg={avg}% max={mx}% "
                        f"from {n} full 60s datapoints"
                    )

        if args.stats:
            try:
                netlog = http_get(client_pub, "netlog.txt", timeout=30)
            except Exception as e:
                print(f"[WARN] fetch netlog.txt failed: {e}")
                netlog = ""
            netlog_path = outdir / f"{args.variant}_netlog_{DATE_STR}.txt"
            netlog_path.write_text(netlog)
            print(
                f"[probe] saved {len(netlog)} bytes of /__log to "
                f"{netlog_path} (raw, counters not parsed)"
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
        (outdir / f"{args.variant}_target_console_{DATE_STR}.txt").write_text(out)
        print("[probe] target console saved")
    finally:
        ids = [i for i in (target_id, client_id) if i]
        if ids:
            run_cmd(
                ["aws", "ec2", "terminate-instances", "--instance-ids"]
                + ids
                + ["--region", AWS_REGION]
            )
        if ami_id:
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
        if snapshot_id:
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
        print("[probe] teardown issued")


if __name__ == "__main__":
    main()
