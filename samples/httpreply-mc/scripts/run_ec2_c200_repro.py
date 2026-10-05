#!/usr/bin/env python3
"""
Targeted c=200 stall reproduction for Ticket d4c2a6bd2a.

Deploys the current httpreply-mc image and an in-subnet wrk client,
runs wrk -t4 -c200 -d20s three times with warm-up, and polls / and
/__log from the client during each run. Saves the wrk outputs and the
netlog captures locally, then tears down all cloud resources.

Build the image first with CONFIG_APPHTTPREPLYMC_NETLOG=y and
CONFIG_APPHTTPREPLYMC_CONSOLE_STATS=y (diagnostic run, not a
latency measurement).
"""

import json
import sys
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_ec2_verification import (  # noqa: E402
    AWS_REGION, INSTANCE_TYPE, SUBNET_ID, SG_ID,
    TARGET_PRIVATE_IP, CLIENT_PRIVATE_IP, UBUNTU_AMI,
    run_cmd, create_bootable_disk, upload_ebs_snapshot,
    register_ami, wait_for_ip_free, launch_target_instance,
)

DATE_STR = time.strftime("%Y-%m-%d")
SAMPLE_DIR = Path(__file__).resolve().parent.parent


def launch_repro_client():
    print(f"Step 5: Launching repro wrk client ({INSTANCE_TYPE})...")
    wait_for_ip_free(CLIENT_PRIVATE_IP)
    user_data = f"""#!/bin/bash
exec > >(tee -a /root/diag.txt) 2>&1
echo "=== REPRO_START $(date) target={TARGET_PRIVATE_IP} ==="

export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y wrk python3

cd /root
python3 -m http.server 80 &

echo "GATE: waiting for target http://{TARGET_PRIVATE_IP}/ ..."
UK_OK=0
for i in $(seq 1 120); do
    CODE=$(curl -s -o /dev/null --max-time 5 -w "%{{http_code}}" http://{TARGET_PRIVATE_IP}/ || echo 000)
    if [ "$CODE" = "200" ]; then UK_OK=1; echo "GATE_OK tries=$i $(date)"; break; fi
    sleep 2
done
if [ "$UK_OK" != "1" ]; then
    echo "GATE_FAIL" > /root/repro.log
    echo "ALL_DONE $(date)" >> /root/repro.log
    exit 0
fi

echo "WARMUP c50 10s $(date)" | tee -a /root/repro.log
timeout --kill-after=5s 20s wrk -t4 -c50 -d10s http://{TARGET_PRIVATE_IP}/ > /root/wrk_warm_c50.txt 2>&1 || true
sleep 5

for r in 1 2 3; do
    echo "=== ROUND $r start $(date) ===" | tee -a /root/repro.log
    timeout --kill-after=5s 30s wrk -t4 -c200 -d20s http://{TARGET_PRIVATE_IP}/ > /root/wrk_r${{r}}_c200.txt 2>&1 &
    WRK_PID=$!
    for p in 1 2 3 4 5 6; do
        sleep 3
        R=$(curl -s -o /dev/null --max-time 15 -w "%{{http_code}} t=%{{time_total}}s" http://{TARGET_PRIVATE_IP}/ || echo "FAIL")
        L=$(curl -s --max-time 15 -o /root/netlog_r${{r}}_p${{p}}.txt -w "%{{http_code}} bytes=%{{size_download}} t=%{{time_total}}s" http://{TARGET_PRIVATE_IP}/__log || echo "FAIL")
        echo "POLL r=$r p=$p root=[$R] log=[$L] $(date +%T)" | tee -a /root/repro.log
    done
    wait $WRK_PID
    echo "WRK_END r=$r rc=$? $(date)" | tee -a /root/repro.log
    curl -s --max-time 15 -o /root/netlog_r${{r}}_final.txt http://{TARGET_PRIVATE_IP}/__log || true
    echo "=== ROUND $r end $(date) ===" | tee -a /root/repro.log
    sleep 10
done

echo "ALL_DONE $(date)" | tee -a /root/repro.log
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
        "ResourceType=instance,Tags=[{Key=Name,Value=wrk-mc-repro-client}]",
        "--region", AWS_REGION,
        "--output", "json",
    ])
    instance_id = json.loads(launch_out)["Instances"][0]["InstanceId"]
    print(f"[SUCCESS] Launched repro client: {instance_id}")
    while True:
        inst_desc = run_cmd([
            "aws", "ec2", "describe-instances",
            "--instance-ids", instance_id,
            "--region", AWS_REGION, "--output", "json",
        ])
        inst = json.loads(inst_desc)["Reservations"][0]["Instances"][0]
        if inst["State"]["Name"] == "running" and inst.get("PublicIpAddress"):
            print(f"[SUCCESS] Client RUNNING. Public IP: {inst['PublicIpAddress']}")
            return instance_id, inst["PublicIpAddress"]
        time.sleep(3)


def fetch(client_pub_ip, name, dest_dir):
    try:
        with urllib.request.urlopen(
                f"http://{client_pub_ip}/{name}", timeout=10) as resp:
            data = resp.read()
        (dest_dir / name).write_bytes(data)
        print(f"[INFO] saved {name} ({len(data)} bytes)")
        return data.decode("utf-8", "replace")
    except Exception as exc:
        print(f"[WARN] fetch {name}: {exc}")
        return ""


def main():
    kernel_path = SAMPLE_DIR / "build/httpreply-mc_qemu-x86_64"
    if not kernel_path.exists():
        print(f"[ERR] Kernel not found: {kernel_path}")
        sys.exit(1)

    out_dir = SAMPLE_DIR / f"repro_d4c2a6bd2a_{DATE_STR}"
    out_dir.mkdir(exist_ok=True)

    target_id = client_id = ami_id = snapshot_id = None
    try:
        disk_raw = create_bootable_disk(kernel_path, SAMPLE_DIR)
        snapshot_id = upload_ebs_snapshot(disk_raw, SAMPLE_DIR)
        ami_id = register_ami(snapshot_id)
        target_id, target_pub = launch_target_instance(ami_id)
        print(f"[INFO] Target public IP (diagnostic only): {target_pub}")
        client_id, client_pub = launch_repro_client()

        print("Waiting for repro run to finish (polling client repro.log)...")
        done = False
        start = time.time()
        last = ""
        while time.time() - start < 1800:
            try:
                with urllib.request.urlopen(
                        f"http://{client_pub}/repro.log", timeout=5) as resp:
                    cur = resp.read().decode("utf-8", "replace")
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
            print("[WARN] Repro did not finish within 30 min.")

        for name in (["repro.log", "diag.txt", "wrk_warm_c50.txt"] +
                     [f"wrk_r{r}_c200.txt" for r in (1, 2, 3)] +
                     [f"netlog_r{r}_p{p}.txt" for r in (1, 2, 3)
                      for p in range(1, 7)] +
                     [f"netlog_r{r}_final.txt" for r in (1, 2, 3)]):
            fetch(client_pub, name, out_dir)

        time.sleep(10)
        console_out = run_cmd([
            "aws", "ec2", "get-console-output",
            "--instance-id", target_id, "--latest",
            "--region", AWS_REGION, "--output", "json",
        ])
        console_text = json.loads(console_out).get("Output", "")
        (out_dir / "target_console.txt").write_text(console_text)
        print(f"[SUCCESS] Saved {len(console_text)} bytes console output")
        print(f"[SUCCESS] Repro artifacts in {out_dir}")
    finally:
        print("Teardown...")
        ids = [i for i in (target_id, client_id) if i]
        if ids:
            run_cmd(["aws", "ec2", "terminate-instances",
                     "--instance-ids"] + ids + ["--region", AWS_REGION])
            while True:
                d = run_cmd(["aws", "ec2", "describe-instances",
                             "--instance-ids"] + ids + [
                             "--query",
                             "Reservations[].Instances[].State.Name",
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
        print("[SUCCESS] Teardown complete.")


if __name__ == "__main__":
    main()
