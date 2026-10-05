#!/usr/bin/env python3
"""
4-vCPU EC2 benchmark for httpreply-mc vs an nginx baseline.

Runs both variants on c6i.xlarge (4 vCPUs) in the same subnet, with an
in-subnet client. Per concurrency level (25, 50, 100, 200, 300, 500):

1. Closed-loop wrk 4.1.0, 30 s: throughput, p50, p90, p99. Same tool
   and method as the 2-vCPU tables in README.md.
2. giltene/wrk2 for 30 s at a fixed rate of 90% of the measured
   closed-loop throughput. wrk2 reports the coordinated-omission-
   corrected 99.900% percentile (p999). A fixed rate must stay below
   capacity, or wrk2's rate scheduler skews its histograms.

CPU load comes from CloudWatch CPUUtilization with detailed
monitoring (1-minute datapoints). The client records per-phase epoch
windows; the script attributes datapoints to each level.

Usage:
  python3 scripts/run_ec2_4cpu_benchmark.py [unikraft|nginx|both]
"""

import json
import re
import sys
import time
import csv
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_ec2_verification import (  # noqa: E402
    AWS_REGION, SUBNET_ID, SG_ID,
    TARGET_PRIVATE_IP, CLIENT_PRIVATE_IP, UBUNTU_AMI,
    run_cmd, create_bootable_disk, upload_ebs_snapshot,
    register_ami, wait_for_ip_free,
)

SAMPLE_DIR = Path(__file__).resolve().parent.parent
INSTANCE_TYPE = "c6i.xlarge"
DATE_STR = time.strftime("%Y-%m-%d")
CONCS = [25, 50, 100, 200, 300, 500]
THREADS = 4
DURATION = "30s"


def to_ms(v, u):
    v = float(v)
    if u == "us": return v / 1000.0
    if u == "ms": return v
    if u == "s": return v * 1000.0
    if u == "m": return v * 60000.0
    return v


def parse_wrk_closed_loop(content):
    """Parse wrk 4.1.0 output: percentiles print as '     50%'."""
    data = {"requests_sec": 0.0, "transfer_kb_sec": 0.0,
            "latency_avg_ms": 0.0, "latency_stdev_ms": 0.0,
            "latency_max_ms": 0.0, "total_requests": 0,
            "socket_errors": 0,
            "p50_ms": 0.0, "p90_ms": 0.0, "p99_ms": 0.0}
    m = re.search(r"Requests/sec:\s+([\d\.]+)", content)
    if m: data["requests_sec"] = float(m.group(1))
    m = re.search(r"Transfer/sec:\s+([\d\.]+)\s*([KMG]?B)", content)
    if m:
        val = float(m.group(1))
        if m.group(2) == "MB": val *= 1024.0
        elif m.group(2) == "GB": val *= 1024.0 * 1024.0
        data["transfer_kb_sec"] = val
    m = re.search(r"Latency\s+([\d\.]+)(\w+)\s+([\d\.]+)(\w+)\s+([\d\.]+)(\w+)", content)
    if m:
        data["latency_avg_ms"] = to_ms(m.group(1), m.group(2))
        data["latency_stdev_ms"] = to_ms(m.group(3), m.group(4))
        data["latency_max_ms"] = to_ms(m.group(5), m.group(6))
    for key, pat in [("p50_ms", r"\s+50%\s+([\d\.]+)(us|ms|s|m)"),
                     ("p90_ms", r"\s+90%\s+([\d\.]+)(us|ms|s|m)"),
                     ("p99_ms", r"\s+99%\s+([\d\.]+)(us|ms|s|m)")]:
        m = re.search(pat, content)
        if m: data[key] = to_ms(m.group(1), m.group(2))
    m = re.search(r"(\d+)\s+requests in", content)
    if m: data["total_requests"] = int(m.group(1))
    m = re.search(r"Socket errors:\s+connect\s+(\d+),\s+read\s+(\d+),\s+write\s+(\d+),\s+timeout\s+(\d+)", content)
    if m: data["socket_errors"] = sum(int(m.group(i)) for i in range(1, 5))
    return data


def parse_wrk2_p999(content):
    """wrk2 output: percentiles print as ' 99.900%'. Return p99/p999 ms."""
    out = {"p99_ms_wrk2": None, "p999_ms": None}
    m = re.search(r"\s*99\.000%\s+([\d\.]+)(us|ms|s|m)", content)
    if m: out["p99_ms_wrk2"] = to_ms(m.group(1), m.group(2))
    m = re.search(r"\s*99\.900%\s+([\d\.]+)(us|ms|s|m)", content)
    if m: out["p999_ms"] = to_ms(m.group(1), m.group(2))
    return out


def client_user_data():
    return f"""#!/bin/bash
set -e
exec > >(tee -a /root/diag.txt) 2>&1
echo "=== 4CPU_RUN_START $(date) target={TARGET_PRIVATE_IP} ==="

export DEBIAN_FRONTEND=noninteractive
# unattended-upgrades can hold the dpkg lock early in boot. Retry.
for i in $(seq 1 8); do apt-get update -y && break; echo "apt retry $i"; sleep 15; done
for i in $(seq 1 8); do apt-get install -y curl python3 wrk build-essential libssl-dev zlib1g-dev git unzip && break; echo "apt-install retry $i"; sleep 15; done
test -d /root/wrk2-src || git clone https://github.com/giltene/wrk2 /root/wrk2-src
make -C /root/wrk2-src -j4
WRK2=/root/wrk2-src/wrk
wrk --version 2>&1 | head -1 || true
$WRK2 --version 2>&1 | head -1 || true
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
    echo "GATE_FAIL" > /root/wrk_sweep.log
    echo "ALL_DONE" >> /root/wrk_sweep.log
    exit 0
fi

echo "WARMUP c50 10s $(date)" | tee -a /root/wrk_sweep.log
timeout --kill-after=5s 20s wrk -t{THREADS} -c50 -d10s http://{TARGET_PRIVATE_IP}/ > /root/wrk_warm.txt 2>&1 || true
sleep 5

for c in {" ".join(str(c) for c in CONCS)}; do
    T0=$(date +%s)
    echo "=== STEP c=$c phase=wrk start=$T0 $(date) ===" | tee -a /root/wrk_sweep.log
    timeout --kill-after=5s 45s wrk -t{THREADS} -c$c -d{DURATION} --latency http://{TARGET_PRIVATE_IP}/ > /root/wrk_c$c.txt 2>&1 || true
    T1=$(date +%s)
    echo "STEP_WINDOW c=$c phase=wrk start=$T0 end=$T1" >> /root/step_times.txt
    echo "=== STEP c=$c phase=wrk end=$T1 $(date) ===" | tee -a /root/wrk_sweep.log
    sleep 5

    R=$(awk '/Requests\\/sec/ {{printf "%d", $2*0.9}}' /root/wrk_c$c.txt)
    if [ -n "$R" ] && [ "$R" != "0" ]; then
        T2=$(date +%s)
        echo "=== STEP c=$c phase=wrk2 rate=$R start=$T2 $(date) ===" | tee -a /root/wrk_sweep.log
        timeout --kill-after=5s 45s $WRK2 -t{THREADS} -c$c -R $R -d{DURATION} --latency http://{TARGET_PRIVATE_IP}/ > /root/wrk2_c$c.txt 2>&1 || true
        T3=$(date +%s)
        echo "STEP_WINDOW c=$c phase=wrk2 start=$T2 end=$T3" >> /root/step_times.txt
        echo "=== STEP c=$c phase=wrk2 end=$T3 $(date) ===" | tee -a /root/wrk_sweep.log
    else
        echo "=== STEP c=$c phase=wrk2 SKIPPED (no closed-loop rate) ===" | tee -a /root/wrk_sweep.log
    fi
    sleep 5
done

echo "ALL_DONE $(date)" | tee -a /root/wrk_sweep.log
"""


def launch_instance(ami_id, ip, user_data=None, tag=""):
    wait_for_ip_free(ip)
    cmd = [
        "aws", "ec2", "run-instances",
        "--image-id", ami_id,
        "--instance-type", INSTANCE_TYPE,
        "--security-group-ids", SG_ID,
        "--subnet-id", SUBNET_ID,
        "--private-ip-address", ip,
        "--monitoring", "Enabled=true",
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


def http_get(ip, name, timeout=10):
    with urllib.request.urlopen(f"http://{ip}/{name}", timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def cloudwatch_cpu(instance_id, windows):
    """Return (avg, max) CPUUtilization over a list of (t0, t1) windows."""
    if not windows:
        return None, None
    t0 = min(w[0] for w in windows)
    t1 = max(w[1] for w in windows)
    start = datetime.fromtimestamp(t0 - 60, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    end = datetime.fromtimestamp(t1 + 60, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    try:
        out = run_cmd([
            "aws", "cloudwatch", "get-metric-statistics",
            "--namespace", "AWS/EC2",
            "--metric-name", "CPUUtilization",
            "--dimensions", f"Name=InstanceId,Value={instance_id}",
            "--start-time", start, "--end-time", end,
            "--period", "60",
            "--statistics", "Average", "Maximum",
            "--region", AWS_REGION, "--output", "json",
        ])
        dps = json.loads(out).get("Datapoints", [])
        in_win = []
        for d in dps:
            ts_str = d["Timestamp"]
            if hasattr(ts_str, "timestamp"):
                ts = ts_str.timestamp()
            else:
                ts = datetime.fromisoformat(str(ts_str).replace("Z", "+00:00")).timestamp()
            if any(w0 - 60 <= ts <= w1 + 60 for w0, w1 in windows):
                in_win.append(d)
        if not in_win:
            return None, None
        avg = sum(d["Average"] for d in in_win) / len(in_win)
        mx = max(d["Maximum"] for d in in_win)
        return round(avg, 1), round(mx, 1)
    except Exception as e:
        print(f"[WARN] cloudwatch query failed: {e}")
        return None, None


def run_variant(label, kernel_path=None):
    print("\n" + "#" * 60)
    print(f"# VARIANT: {label} ({INSTANCE_TYPE})")
    print("#" * 60)
    target_id = client_id = ami_id = snapshot_id = None
    rows = []
    try:
        if kernel_path:
            disk_raw = create_bootable_disk(kernel_path, SAMPLE_DIR)
            snapshot_id = upload_ebs_snapshot(disk_raw, SAMPLE_DIR)
            ami_id = register_ami(snapshot_id)
            target_id, _ = launch_instance(ami_id, TARGET_PRIVATE_IP,
                                           tag="unikraft-mc-4cpu-target")
        else:
            ud = """#!/bin/bash
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y nginx
printf 'Hello, World!\\n' > /var/www/html/index.html
systemctl enable --now nginx
"""
            target_id, _ = launch_instance(UBUNTU_AMI, TARGET_PRIVATE_IP, ud,
                                           tag="nginx-mc-4cpu-baseline")

        client_id, client_pub = launch_instance(
            UBUNTU_AMI, CLIENT_PRIVATE_IP, client_user_data(),
            tag="wrk-mc-4cpu-client")

        start = time.time()
        last = ""
        done = False
        while time.time() - start < 3000:
            try:
                cur = http_get(client_pub, "wrk_sweep.log", timeout=5)
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
            print(f"[WARN] {label}: sweep did not finish within 50 min.")

        try:
            step_txt = http_get(client_pub, "step_times.txt", timeout=10)
        except Exception:
            step_txt = ""
        windows = {}
        for line in step_txt.splitlines():
            parts = line.split()
            if len(parts) == 5 and parts[0] == "STEP_WINDOW":
                c = int(parts[1].split("=")[1])
                windows.setdefault(c, []).append(
                    (int(parts[3].split("=")[1]), int(parts[4].split("=")[1])))

        for c in CONCS:
            try:
                content = http_get(client_pub, f"wrk_c{c}.txt", timeout=15)
            except Exception as e:
                print(f"[WARN] {label}: fetch wrk_c{c}.txt failed: {e}")
                content = ""
            (SAMPLE_DIR / f"4cpu_{label}_wrk_c{c}_{DATE_STR}.txt").write_text(content)
            parsed = parse_wrk_closed_loop(content)

            try:
                content2 = http_get(client_pub, f"wrk2_c{c}.txt", timeout=15)
            except Exception:
                content2 = ""
            (SAMPLE_DIR / f"4cpu_{label}_wrk2_c{c}_{DATE_STR}.txt").write_text(content2)
            parsed.update(parse_wrk2_p999(content2))

            cpu_avg, cpu_max = cloudwatch_cpu(target_id, windows.get(c, []))
            row = {"variant": label, "concurrency": c,
                   "cpu_avg_pct": cpu_avg, "cpu_max_pct": cpu_max, **parsed}
            rows.append(row)
            print(f"[{label}] c={c}: {parsed['requests_sec']:.0f} req/s, "
                  f"p99={parsed['p99_ms']:.2f}ms p99.9={parsed['p999_ms']}ms, "
                  f"cpu avg={cpu_avg}% max={cpu_max}%")
    finally:
        ids = [i for i in (target_id, client_id) if i]
        if ids:
            run_cmd(["aws", "ec2", "terminate-instances",
                     "--instance-ids"] + ids + ["--region", AWS_REGION])
            while True:
                d = run_cmd(["aws", "ec2", "describe-instances",
                             "--instance-ids"] + ids + [
                             "--query", "Reservations[].Instances[].State.Name",
                             "--region", AWS_REGION, "--output", "json"])
                if all(s in ("terminated", "shutting-down") for s in json.loads(d)):
                    print("[SUCCESS] Instances terminated.")
                    break
                time.sleep(3)
        if ami_id:
            run_cmd(["aws", "ec2", "deregister-image",
                     "--image-id", ami_id, "--region", AWS_REGION])
        if snapshot_id:
            run_cmd(["aws", "ec2", "delete-snapshot",
                     "--snapshot-id", snapshot_id, "--region", AWS_REGION])
    return rows


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "both"
    rows = []
    if what in ("unikraft", "both"):
        kernel = SAMPLE_DIR / "build/httpreply-mc_qemu-x86_64"
        if not kernel.exists():
            print(f"[ERR] kernel not found: {kernel}")
            sys.exit(1)
        rows += run_variant("unikraft", kernel)
        time.sleep(15)
    if what in ("nginx", "both"):
        rows += run_variant("nginx")

    jpath = SAMPLE_DIR / f"benchmark_4cpu_{DATE_STR}.json"
    cpath = SAMPLE_DIR / f"benchmark_4cpu_{DATE_STR}.csv"
    jpath.write_text(json.dumps(rows, indent=2))
    with cpath.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=[
            "variant", "concurrency", "requests_sec", "latency_avg_ms",
            "latency_stdev_ms", "latency_max_ms", "p50_ms", "p90_ms",
            "p99_ms", "p999_ms", "transfer_kb_sec", "total_requests",
            "socket_errors", "cpu_avg_pct", "cpu_max_pct"], extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print(f"[SUCCESS] wrote {cpath} and {jpath}")


if __name__ == "__main__":
    main()
