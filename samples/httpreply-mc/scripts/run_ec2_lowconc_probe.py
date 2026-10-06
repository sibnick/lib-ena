#!/usr/bin/env python3
"""
Low-concurrency probe for [Ticket ca72834ec7].

Runs the current httpreply-mc build on c6i.xlarge and measures only
c=25 and c=50 with closed-loop wrk. Compare the numbers against the
4-core run (reports/httpreply-mc_2026-10-06) to see whether the core
and queue count causes the low-concurrency regression.

Usage: python3 scripts/run_ec2_lowconc_probe.py
"""

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_ec2_verification import (  # noqa: E402
    AWS_REGION, SG_ID, SUBNET_ID,
    TARGET_PRIVATE_IP, CLIENT_PRIVATE_IP, UBUNTU_AMI,
    run_cmd, create_bootable_disk, upload_ebs_snapshot,
    register_ami, wait_for_ip_free,
)
from run_ec2_4cpu_benchmark import parse_wrk_closed_loop  # noqa: E402

SAMPLE_DIR = Path(__file__).resolve().parent.parent
INSTANCE_TYPE = "c6i.xlarge"
DATE_STR = time.strftime("%Y-%m-%d")
CONCS = [25, 50]
THREADS = 4
DURATION = "60s"


def launch_instance(ami_id, ip, user_data=None, tag=""):
    wait_for_ip_free(ip)
    cmd = [
        "aws", "ec2", "run-instances",
        "--image-id", ami_id,
        "--instance-type", INSTANCE_TYPE,
        "--security-group-ids", SG_ID,
        "--subnet-id", SUBNET_ID,
        "--private-ip-address", ip,
        "--count", "1",
        "--tag-specifications", f"ResourceType=instance,Tags=[{{Key=Name,Value={tag}}}]",
        "--region", AWS_REGION, "--output", "json",
    ]
    if user_data:
        cmd += ["--user-data", user_data]
    out = run_cmd(cmd)
    instance_id = json.loads(out)["Instances"][0]["InstanceId"]
    print(f"[SUCCESS] Launched {tag}: {instance_id}")
    while True:
        d = run_cmd([
            "aws", "ec2", "describe-instances",
            "--instance-ids", instance_id,
            "--region", AWS_REGION, "--output", "json",
        ])
        inst = json.loads(d)["Reservations"][0]["Instances"][0]
        if inst["State"]["Name"] == "running" and inst.get("PublicIpAddress"):
            print(f"[SUCCESS] {tag} RUNNING. Public IP: {inst['PublicIpAddress']}")
            return instance_id, inst["PublicIpAddress"]
        time.sleep(3)


def client_user_data():
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
for c in {" ".join(str(c) for c in CONCS)}; do
    echo "=== STEP c=$c $(date) ===" | tee -a /root/wrk_sweep.log
    timeout --kill-after=5s 90s wrk -t{THREADS} -c$c -d{DURATION} --latency http://{TARGET_PRIVATE_IP}/ > /root/wrk_c$c.txt 2>&1 || true
    sleep 5
done
echo "ALL_DONE $(date)" | tee -a /root/wrk_sweep.log
"""


def main():
    kernel = SAMPLE_DIR / "build/httpreply-mc_qemu-x86_64"
    if not kernel.exists():
        print(f"[ERR] kernel not found: {kernel}")
        sys.exit(1)
    target_id = client_id = ami_id = snapshot_id = None
    try:
        disk_raw = create_bootable_disk(kernel, SAMPLE_DIR)
        snapshot_id = upload_ebs_snapshot(disk_raw, SAMPLE_DIR)
        ami_id = register_ami(snapshot_id)
        target_id, _ = launch_instance(
            ami_id, TARGET_PRIVATE_IP, tag="unikraft-mc-lowconc-target")
        client_id, client_pub = launch_instance(
            UBUNTU_AMI, CLIENT_PRIVATE_IP,
            user_data=client_user_data(), tag="wrk-mc-lowconc-client")

        start = time.time()
        last = ""
        while time.time() - start < 1200:
            try:
                import urllib.request
                cur = urllib.request.urlopen(
                    f"http://{client_pub}/wrk_sweep.log", timeout=5
                ).read().decode("utf-8", "replace")
                if cur != last:
                    sys.stdout.write(cur[len(last):])
                    sys.stdout.flush()
                    last = cur
                if "ALL_DONE" in cur or "GATE_FAIL" in cur:
                    break
            except Exception:
                pass
            time.sleep(5)

        for c in CONCS:
            import urllib.request
            try:
                content = urllib.request.urlopen(
                    f"http://{client_pub}/wrk_c{c}.txt", timeout=15
                ).read().decode("utf-8", "replace")
            except Exception as e:
                print(f"[WARN] fetch wrk_c{c}.txt failed: {e}")
                content = ""
            (SAMPLE_DIR / f"lowconc_wrk_c{c}_{DATE_STR}.txt").write_text(content)
            p = parse_wrk_closed_loop(content)
            print(f"[probe] c={c}: {p['requests_sec']:.0f} req/s, "
                  f"avg={p['latency_avg_ms']:.2f}ms p50={p['p50_ms']:.2f}ms "
                  f"p99={p['p99_ms']:.2f}ms errors={p['socket_errors']}")

        time.sleep(5)
        out = run_cmd(["aws", "ec2", "get-console-output",
                       "--instance-id", target_id,
                       "--region", AWS_REGION, "--output", "text"])
        (SAMPLE_DIR / f"lowconc_target_console_{DATE_STR}.txt").write_text(out)
        print("[probe] target console saved")
    finally:
        ids = [i for i in (target_id, client_id) if i]
        if ids:
            run_cmd(["aws", "ec2", "terminate-instances",
                     "--instance-ids"] + ids + ["--region", AWS_REGION])
        if ami_id:
            run_cmd(["aws", "ec2", "deregister-image",
                     "--image-id", ami_id, "--region", AWS_REGION])
        if snapshot_id:
            run_cmd(["aws", "ec2", "delete-snapshot",
                     "--snapshot-id", snapshot_id, "--region", AWS_REGION])
        print("[probe] teardown issued")


if __name__ == "__main__":
    main()
