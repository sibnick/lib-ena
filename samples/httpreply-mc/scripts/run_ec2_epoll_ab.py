#!/usr/bin/env python3
"""
A/B benchmark for Ticket 20d68e6796 (epoll ready-list fix).

Builds are done separately. This script takes two prebuilt unikernel images,
deploys each on c6i.large with an in-subnet wrk client, runs the same
high-concurrency wrk sweep against each, then tears down all cloud
resources. It writes labeled CSV/JSON and prints a base-vs-fixed
comparison of throughput and p99.

Usage:
  python3 scripts/run_ec2_epoll_ab.py BASE_KERNEL FIXED_KERNEL

The only intended difference between the two images is the epoll.c
ready-list change. Keep every other build input identical.
"""

import json
import sys
import time
import csv
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_ec2_verification import (  # noqa: E402
    AWS_REGION, INSTANCE_TYPE, SUBNET_ID, SG_ID,
    TARGET_PRIVATE_IP, CLIENT_PRIVATE_IP, UBUNTU_AMI,
    run_cmd, create_bootable_disk, upload_ebs_snapshot,
    register_ami, launch_target_instance, parse_wrk_file,
)

SAMPLE_DIR = Path(__file__).resolve().parent.parent
DATE_STR = time.strftime("%Y-%m-%d")

# Concurrency levels where the O(N) walk matters. Low levels are omitted:
# the ticket states the cost is negligible below c=50.
CONCS = [50, 100, 200, 300]
THREADS = 4
DURATION = "20s"


def launch_ab_client():
    user_data = f"""#!/bin/bash
set -e
exec > >(tee -a /root/ab_diag.txt) 2>&1
echo "=== AB_START $(date) target={TARGET_PRIVATE_IP} ==="

export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y wrk python3
echo "WRK_READY $(date)"

cd /root
python3 -m http.server 80 &

echo "GATE: waiting for target http://{TARGET_PRIVATE_IP}/ ..."
OK=0
for i in $(seq 1 120); do
    CODE=$(curl -s -o /dev/null --max-time 5 -w "%{{http_code}}" http://{TARGET_PRIVATE_IP}/ || echo 000)
    if [ "$CODE" = "200" ]; then OK=1; echo "GATE_OK tries=$i $(date)"; break; fi
    sleep 2
done
if [ "$OK" != "1" ]; then
    echo "GATE_FAIL" > /root/ab_sweep.log
    echo "ALL_DONE $(date)" >> /root/ab_sweep.log
    exit 0
fi

echo "WARMUP c50 10s $(date)" | tee -a /root/ab_sweep.log
timeout --kill-after=5s 20s wrk -t{THREADS} -c50 -d10s http://{TARGET_PRIVATE_IP}/ > /root/wrk_warm.txt 2>&1 || true
sleep 5

for c in {" ".join(str(c) for c in CONCS)}; do
    echo "=== STEP c=$c start $(date) ===" | tee -a /root/ab_sweep.log
    timeout --kill-after=5s 30s wrk -t{THREADS} -c$c -d{DURATION} --latency http://{TARGET_PRIVATE_IP}/ > /root/wrk_c$c.txt 2>&1 || true
    echo "=== STEP c=$c end $(date) ===" | tee -a /root/ab_sweep.log
    sleep 5
done

echo "ALL_DONE $(date)" | tee -a /root/ab_sweep.log
"""
    launch_out = run_cmd([
        "aws", "ec2", "run-instances",
        "--image-id", UBUNTU_AMI,
        "--instance-type", INSTANCE_TYPE,
        "--security-group-ids", SG_ID,
        "--subnet-id", SUBNET_ID,
        "--private-ip-address", CLIENT_PRIVATE_IP,
        "--user-data", user_data,
        "--count", "1",
        "--tag-specifications",
        "ResourceType=instance,Tags=[{Key=Name,Value=wrk-mc-epoll-ab}]",
        "--region", AWS_REGION, "--output", "json",
    ])
    instance_id = json.loads(launch_out)["Instances"][0]["InstanceId"]
    print(f"[SUCCESS] Launched A/B client: {instance_id}")
    while True:
        d = run_cmd([
            "aws", "ec2", "describe-instances",
            "--instance-ids", instance_id,
            "--region", AWS_REGION, "--output", "json",
        ])
        inst = json.loads(d)["Reservations"][0]["Instances"][0]
        if inst["State"]["Name"] == "running" and inst.get("PublicIpAddress"):
            print(f"[SUCCESS] Client RUNNING. Public IP: {inst['PublicIpAddress']}")
            return instance_id, inst["PublicIpAddress"]
        time.sleep(3)


def http_get(ip, name, timeout=10):
    with urllib.request.urlopen(f"http://{ip}/{name}", timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def run_variant(label, kernel_path, out_dir):
    print("\n" + "#" * 60)
    print(f"# VARIANT: {label}  kernel={kernel_path}")
    print("#" * 60)
    target_id = client_id = ami_id = snapshot_id = None
    rows = []
    try:
        disk_raw = create_bootable_disk(kernel_path, SAMPLE_DIR)
        snapshot_id = upload_ebs_snapshot(disk_raw, SAMPLE_DIR)
        ami_id = register_ami(snapshot_id)
        target_id, _ = launch_target_instance(ami_id)
        client_id, client_pub = launch_ab_client()

        start = time.time()
        last = ""
        done = False
        while time.time() - start < 1500:
            try:
                cur = http_get(client_pub, "ab_sweep.log", timeout=5)
                if cur != last:
                    sys.stdout.write(cur[len(last):])
                    sys.stdout.flush()
                    last = cur
                if "ALL_DONE" in cur or "GATE_FAIL" in cur:
                    done = True
                    break
            except Exception:
                pass
            time.sleep(5)
        if not done:
            print(f"[WARN] {label}: sweep did not finish within 25 min.")

        for c in CONCS:
            try:
                content = http_get(client_pub, f"wrk_c{c}.txt", timeout=15)
            except Exception as exc:
                print(f"[WARN] {label}: fetch wrk_c{c}.txt failed: {exc}")
                content = ""
            (out_dir / f"{label}_wrk_c{c}.txt").write_text(content)
            parsed = parse_wrk_file(content)
            rows.append({"variant": label, "concurrency": c, **parsed})
    finally:
        print(f"[{label}] Teardown...")
        ids = [i for i in (target_id, client_id) if i]
        if ids:
            run_cmd(["aws", "ec2", "terminate-instances",
                     "--instance-ids"] + ids + ["--region", AWS_REGION])
            while True:
                d = run_cmd(["aws", "ec2", "describe-instances",
                             "--instance-ids"] + ids + [
                             "--query", "Reservations[].Instances[].State.Name",
                             "--region", AWS_REGION, "--output", "json"])
                if all(s in ("terminated", "shutting-down")
                       for s in json.loads(d)):
                    print("[SUCCESS] Instances terminated.")
                    break
                time.sleep(3)
        if ami_id:
            run_cmd(["aws", "ec2", "deregister-image",
                     "--image-id", ami_id, "--region", AWS_REGION])
        if snapshot_id:
            run_cmd(["aws", "ec2", "delete-snapshot",
                     "--snapshot-id", snapshot_id, "--region", AWS_REGION])
        print(f"[{label}] Teardown complete.")
    return rows


def print_compare(rows):
    by = {}
    for r in rows:
        by.setdefault(r["concurrency"], {})[r["variant"]] = r
    print("\n" + "=" * 78)
    print("EPOLL A/B  (req/s and p99 ms; base = pre-merge, fixed = ready-list)")
    print("=" * 78)
    print(f"{'c':>5} | {'base req/s':>11} {'fixed req/s':>12} {'d%':>7} | "
          f"{'base p99':>9} {'fixed p99':>10} {'d%':>7}")
    print("-" * 78)
    for c in CONCS:
        b = by.get(c, {}).get("base")
        f = by.get(c, {}).get("fixed")
        if not b or not f:
            continue
        dreq = (f["requests_sec"] - b["requests_sec"]) / b["requests_sec"] * 100 if b["requests_sec"] else 0
        dp99 = (f["p99_ms"] - b["p99_ms"]) / b["p99_ms"] * 100 if b["p99_ms"] else 0
        print(f"{c:>5} | {b['requests_sec']:>11.1f} {f['requests_sec']:>12.1f} {dreq:>+6.1f}% | "
              f"{b['p99_ms']:>9.2f} {f['p99_ms']:>10.2f} {dp99:>+6.1f}%")
    print("=" * 78)


def main():
    if len(sys.argv) != 3:
        print("usage: run_ec2_epoll_ab.py BASE_KERNEL FIXED_KERNEL")
        sys.exit(1)
    base_kernel = Path(sys.argv[1]).resolve()
    fixed_kernel = Path(sys.argv[2]).resolve()
    for k in (base_kernel, fixed_kernel):
        if not k.exists():
            print(f"[ERR] kernel not found: {k}")
            sys.exit(1)

    out_dir = SAMPLE_DIR / f"epoll_ab_{DATE_STR}"
    out_dir.mkdir(exist_ok=True)

    rows = []
    rows += run_variant("base", base_kernel, out_dir)
    time.sleep(15)
    rows += run_variant("fixed", fixed_kernel, out_dir)

    with (out_dir / "epoll_ab.json").open("w") as f:
        json.dump(rows, f, indent=2)
    with (out_dir / "epoll_ab.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=[
            "variant", "concurrency", "requests_sec", "latency_avg_ms",
            "latency_stdev_ms", "latency_max_ms", "p50_ms", "p75_ms",
            "p90_ms", "p99_ms", "transfer_kb_sec", "total_requests",
            "socket_errors"], extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print(f"[SUCCESS] wrote {out_dir}/epoll_ab.csv and .json")
    print_compare(rows)


if __name__ == "__main__":
    main()
