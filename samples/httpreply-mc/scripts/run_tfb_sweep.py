#!/usr/bin/env python3
"""
TFB-style EC2 sweep harness for httpreply-mc.

Runs the TechEmpower-style plaintext and JSON sweeps defined by the
Plan-TFB-Phase1 wiki page against two target modes:

  unikraft  the httpreply-mc unikernel AMI built from a kernel image.
  nginx     Ubuntu 24.04 with nginx tuned as a TFB-compliant baseline.
            nginx serves the exact TFB body bytes from static files,
            because nginx 'return' appends CRLF and breaks byte
            equality. The validator gates every sweep either way.

Sweep plan (TFB concurrency levels, 3 repetitions per point, 20 s per
rep, report medians):

  /json       concurrency 16, 64, 128, 256, 512
  /plaintext  concurrency 256, 1024, 4096
  stretch     one plaintext point at 16384 with --stretch

The client runs standard wrk 4.x (keep-alive, no client-side
pipelining). TFB plaintext uses pipelined load, so every number here
is a lower bound versus TFB methodology. The script records this and
the other deviations in the results JSON.

Method (same as run_ec2_verification.py):

  1. Build a bootable disk, upload an EBS snapshot, register an AMI
     (unikraft mode only; --ami-id reuses an already-registered AMI).
  2. Launch the target on the fixed private IP, capture boot console.
  3. Launch a c6i.xlarge wrk client on its fixed private IP.
  4. The client waits for HTTP 200 on /plaintext, runs tfb_validate.sh
     as a hard gate, warms up, then runs the sweep. A failed validator
     stops the sweep before any wrk point.
  5. Collect raw wrk files, compute medians and CloudWatch CPU per
     point, save CSV + JSON + raw outputs under --outdir.
  6. Terminate instances. Deregister the AMI and delete the snapshot
     only when this run created them and --keep-ami is absent.

Usage examples:

  python3 scripts/run_tfb_sweep.py --target unikraft \
      --instance-type c6i.large --workers 2 --keep-ami --stretch
  python3 scripts/run_tfb_sweep.py --target nginx \
      --instance-type c6i.large
  python3 scripts/run_tfb_sweep.py --target unikraft \
      --instance-type c6i.xlarge --workers 4 --only plaintext \
      --stretch   # plaintext re-run only, plus the stretch point

--print-plans DIR writes the generated cloud-init scripts to DIR and
exits, so they can be reviewed or shell-checked without touching AWS.

[Ticket 0a2a42d225]
"""

import argparse
import base64
import csv
import gzip
import hashlib
import json
import re
import statistics
import sys
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_ec2_verification import (  # noqa: E402
    AWS_REGION,
    SUBNET_ID,
    SG_ID,
    TARGET_PRIVATE_IP,
    CLIENT_PRIVATE_IP,
    UBUNTU_AMI,
    run_cmd,
    create_bootable_disk,
    upload_ebs_snapshot,
    register_ami,
    wait_for_ip_free,
    parse_wrk_file,
)

SAMPLE_DIR = Path(__file__).resolve().parent.parent

# Sweep plan. TFB JSON test runs at 16..512; plaintext at 256/1024/4096
# plus the 16384 stretch point.
JSON_CONCS = [16, 64, 128, 256, 512]
PLAIN_CONCS = [256, 1024, 4096]
STRETCH_CONC = 16384
DURATION_S = 20
REPS = 3
COOLDOWN_S = 4  # between reps
POINT_COOLDOWN_S = 6  # between points

CSV_FIELDS = [
    "target",
    "instance_type",
    "endpoint",
    "concurrency",
    "rep",
    "requests_sec",
    "transfer_kb_sec",
    "total_requests",
    "latency_avg_ms",
    "latency_stdev_ms",
    "latency_max_ms",
    "p50_ms",
    "p75_ms",
    "p90_ms",
    "p99_ms",
    "socket_errors",
    "client_cpu_pct",
    "status",
]
MED_FIELDS = [
    "target",
    "instance_type",
    "endpoint",
    "concurrency",
    "reps_ok",
    "median_requests_sec",
    "min_requests_sec",
    "max_requests_sec",
    "median_p50_ms",
    "median_p99_ms",
    "median_max_ms",
    "total_socket_errors",
    "median_client_cpu_pct",
    "lower_bound",
    "client_saturated",
    "status",
]


def parse_args():
    ap = argparse.ArgumentParser(
        description="Run TFB plaintext/json sweeps on EC2 against a "
        "unikraft or nginx target."
    )
    ap.add_argument(
        "--target",
        choices=["unikraft", "nginx"],
        required=True,
        help="Target mode to sweep.",
    )
    ap.add_argument(
        "--instance-type",
        default="c6i.large",
        help="Target instance type (default c6i.large).",
    )
    ap.add_argument(
        "--client-type",
        default="c6i.xlarge",
        help="wrk client instance type (default c6i.xlarge, "
        "4 vCPUs, so the client is not the ceiling at "
        "c=4096).",
    )
    ap.add_argument(
        "--client-threads",
        type=int,
        default=4,
        help="wrk -t value (default 4, matches c6i.xlarge).",
    )
    ap.add_argument(
        "--workers",
        type=int,
        default=0,
        help="Unikernel worker count actually built into the "
        "kernel image. Recorded in the results.",
    )
    ap.add_argument(
        "--kernel",
        default=str(SAMPLE_DIR / "build/httpreply-mc_qemu-x86_64"),
        help="Kernel image for the unikraft target.",
    )
    ap.add_argument(
        "--ami-id",
        default=None,
        help="Reuse this already-registered AMI. Skip the "
        "disk/snapshot/register steps.",
    )
    ap.add_argument(
        "--keep-ami",
        action="store_true",
        help="Do not deregister the AMI or delete the "
        "snapshot on teardown. The run prints both IDs "
        "and records them in the JSON. Only applies to "
        "an AMI this run created.",
    )
    ap.add_argument(
        "--stretch",
        action="store_true",
        help="Add one plaintext point at concurrency 16384 "
        "after the planned points. Best effort: the "
        "first failed rep ends the point and the exact "
        "error stays in the raw file.",
    )
    ap.add_argument(
        "--outdir",
        default=str(SAMPLE_DIR / "tfb_out"),
        help="Directory for this run's artifacts.",
    )
    ap.add_argument(
        "--build-rev",
        default="unknown",
        help="Source revision of the kernel image. Recorded in the results.",
    )
    ap.add_argument(
        "--only",
        choices=["json", "plaintext", "all"],
        default="all",
        help="Restrict the sweep to one endpoint. The stretch "
        "point stays a plaintext point and needs --only "
        "plaintext. Use this for re-runs of a subset of the "
        "plan. [Ticket 68b37524bb]",
    )
    ap.add_argument(
        "--print-plans",
        metavar="DIR",
        default=None,
        help="Write the generated cloud-init scripts to DIR "
        "and exit without touching AWS.",
    )
    return ap.parse_args()


def sweep_plan(only):
    plan = []
    if only in ("json", "all"):
        plan += [("json", c) for c in JSON_CONCS]
    if only in ("plaintext", "all"):
        plan += [("plaintext", c) for c in PLAIN_CONCS]
    return plan


def stretch_point():
    return ("plaintext", STRETCH_CONC)


def deviations(args, plan):
    """Methodology deviations from the official TFB method."""
    dev = [
        "Client-side pipelining: standard wrk 4.x uses one request in "
        "flight per keep-alive connection. TFB plaintext drives use "
        "pipelined clients. Numbers are lower bounds vs TFB "
        "methodology. No pipelined client exists in the Ubuntu client "
        "image, and faking one with a patched wrk was rejected.",
        "One client instance, not the multi-machine load generator pool "
        "TFB uses at 256/1024/4096/16384 concurrency.",
        f"{DURATION_S} s per rep with {REPS} reps per point, not the TFB "
        "single long test window. The median of reps is reported.",
        "Single target instance per run; TFB distributes load across a "
        "cluster of machines for the final score.",
        "TFB warmup (setup/verify) is replaced by tfb_validate.sh as the "
        "compliance gate plus one short warmup run.",
    ]
    if args.target == "unikraft":
        dev.append(
            f"Unikernel worker count {args.workers} on {args.instance_type}. "
            "c6i types report 2 vCPUs per physical core; see the Worker "
            "Count and Physical Cores section in samples/httpreply-mc/"
            "README.md. The config follows the established per-size "
            "shapes in this repo."
        )
    return dev


def client_user_data(args, validator_src, plan, stretch):
    """Cloud-init for the wrk client. It gates on the validator, then
    runs every point and writes the markers the monitor reads. The
    validator is embedded as gzip + base64 because the AWS CLI caps
    user data at 16384 bytes and the plain validator is 13 KB.
    """
    validator_b64 = base64.b64encode(gzip.compress(validator_src.encode(), 9)).decode(
        "ascii"
    )
    points = list(plan)
    check_legacy = 1 if args.target == "unikraft" else 0
    loop_lines = []
    for ep, c in points:
        loop_lines.append(f"run_point {ep} {c} {REPS} 0")
    if stretch:
        ep, c = stretch_point()
        loop_lines.append(f"run_point {ep} {c} {REPS} 1")
    loop = "\n".join(loop_lines)
    return f"""#!/bin/bash
set -e
exec > >(tee -a /root/diag.txt) 2>&1

echo "=== TFB_SWEEP_START $(date -u) ==="
export DEBIAN_FRONTEND=noninteractive
# unattended-upgrades can hold the dpkg lock early in boot. Retry.
for i in $(seq 1 8); do apt-get update -y && break; echo "apt retry $i"; sleep 15; done
for i in $(seq 1 8); do apt-get install -y curl python3 wrk && break; echo "apt-install retry $i"; sleep 15; done

ulimit -n 150000 || ulimit -n 65535 || true
sysctl -w net.ipv4.ip_local_port_range="1024 65535" || true
sysctl -w net.core.somaxconn=65535 || true

nproc
wrk --version 2>&1 | head -1 || true
cat /proc/sys/net/ipv4/ip_local_port_range
echo "WRK_READY $(date -u)"

cd /root
python3 -m http.server 80 &

echo "GATE: waiting for target http://{TARGET_PRIVATE_IP}:8080/plaintext ..."
OK=0
for i in $(seq 1 120); do
    CODE=$(curl -s -o /dev/null --max-time 5 -w "%{{http_code}}" http://{TARGET_PRIVATE_IP}:8080/plaintext || echo 000)
    if [ "$CODE" = "200" ]; then OK=1; echo "GATE_OK tries=$i $(date -u)"; break; fi
    sleep 2
done
if [ "$OK" != "1" ]; then
    echo "GATE_FAIL: target never returned HTTP 200 on /plaintext" > /root/wrk_sweep.log
    echo "ALL_DONE $(date -u)" >> /root/wrk_sweep.log
    exit 0
fi

echo '{validator_b64}' | base64 -d | gunzip > /root/tfb_validate.sh
echo "TFB_VALIDATE_START $(date -u)"
set +e
CHECK_LEGACY={check_legacy} bash /root/tfb_validate.sh {TARGET_PRIVATE_IP} 8080 > /root/tfb_validate.txt 2>&1
VRC=$?
set -e
echo "TFB_VALIDATE_RC=$VRC"
cat /root/tfb_validate.txt
if [ "$VRC" != "0" ]; then
    echo "VALIDATE_FAIL rc=$VRC; sweep NOT started" > /root/wrk_sweep.log
    echo "ALL_DONE $(date -u)" >> /root/wrk_sweep.log
    exit 0
fi

echo "WARMUP plaintext c=64 10s $(date -u)" | tee -a /root/wrk_sweep.log
set +e
timeout --kill-after=5s 40s wrk -t{args.client_threads} -c64 -d10s --latency http://{TARGET_PRIVATE_IP}:8080/plaintext > /root/wrk_warmup.txt 2>&1
echo "WARMUP_RC=$?"
set -e
sleep 5

# Per-rep runner. Records the wall window for CloudWatch attribution
# and the client CPU busy percent from /proc/stat deltas.
cpu_snap() {{ awk '/^cpu /{{ t=0; for (i=2; i<=NF; i++) t+=$i; print t, $5+$6 }}' /proc/stat; }}

run_point() {{
    ep=$1; c=$2; reps=$3; stop_on_fail=$4
    r=1
    while [ $r -le $reps ]; do
        f=/root/w_${{ep}}_c${{c}}_r${{r}}.txt
        T0=$(date +%s); S1=$(cpu_snap)
        RC=0
        timeout --kill-after=10s $((DUR+40))s wrk -t{args.client_threads} -c$c -d${{DUR}}s --latency http://{TARGET_PRIVATE_IP}:8080/$ep > $f 2>&1 || RC=$?
        S2=$(cpu_snap); T1=$(date +%s)
        BUSY=$(awk -v s1="$S1" -v s2="$S2" 'BEGIN {{
            split(s1, a, " "); split(s2, b, " ")
            dt = b[1]-a[1]; di = b[2]-a[2]
            if (dt <= 0) {{ print 0; exit }}
            printf "%.1f", 100 * (1 - di / dt)
        }}')
        echo "STEP_WINDOW ep=$ep c=$c r=$r start=$T0 end=$T1" >> /root/step_times.txt
        echo "CPUSAMP ep=$ep c=$c r=$r busy=$BUSY rc=$RC"
        echo "=== DONE ep=$ep c=$c r=$r rc=$RC busy=$BUSY $(date -u) ===" | tee -a /root/wrk_sweep.log
        if [ $RC -ne 0 ] || [ ! -s $f ]; then
            echo "POINT_FAIL ep=$ep c=$c r=$r rc=$RC" | tee -a /root/wrk_sweep.log
            if [ "$stop_on_fail" = "1" ]; then break; fi
        fi
        r=$((r + 1))
        sleep {COOLDOWN_S}
    done
    sleep {POINT_COOLDOWN_S}
}}

DUR={DURATION_S}
echo "SWEEP_START dur=${{DUR}}s reps={REPS} threads={args.client_threads} $(date -u)" | tee -a /root/wrk_sweep.log
set +e
{loop}
set -e

echo "SWEEP_DONE $(date -u)" | tee -a /root/wrk_sweep.log
echo "ALL_DONE $(date -u)" | tee -a /root/wrk_sweep.log
"""


def nginx_user_data():
    """Cloud-init for the TFB-compliant nginx baseline.

    nginx `return 200` appends CRLF, which breaks the exact-byte TFB
    bodies, so the baseline serves the bodies from static files with
    exact bytes. Tuning follows TFB nginx practices: auto workers with
    CPU affinity, epoll, high connection ceilings, keepalive, sendfile,
    tcp_nopush, no access log, gzip off. The setup transcript is also
    teed to the serial console, so the EC2 console output carries it
    as evidence: the target runs no HTTP server of its own.
    """
    return """#!/bin/bash
set -e
exec > >(tee -a /root/nginx_tfb_setup.txt /dev/console) 2>&1
echo "=== NGINX_TFB_START $(date -u) ==="
export DEBIAN_FRONTEND=noninteractive
for i in $(seq 1 8); do apt-get update -y && break; echo "apt retry $i"; sleep 15; done
for i in $(seq 1 8); do apt-get install -y nginx && break; echo "apt-install retry $i"; sleep 15; done

mkdir -p /var/www/tfb
printf 'Hello, World!' > /var/www/tfb/plaintext
printf '{"message":"Hello, World!"}' > /var/www/tfb/json
wc -c /var/www/tfb/plaintext /var/www/tfb/json

cat > /etc/nginx/nginx.conf <<'NGINX_CONF_EOF'
worker_processes auto;
worker_cpu_affinity auto;
worker_rlimit_nofile 200000;
pid /run/nginx.pid;
events {
    use epoll;
    multi_accept on;
    worker_connections 20000;
}
http {
    include /etc/nginx/mime.types;
    default_type application/octet-stream;
    sendfile on;
    tcp_nopush on;
    tcp_nodelay on;
    keepalive_timeout 75s;
    keepalive_requests 1000000;
    types_hash_max_size 2048;
    access_log off;
    gzip off;
    open_file_cache max=1000 inactive=60s;
    open_file_cache_valid 120s;
    open_file_cache_min_uses 1;
    server {
        listen 8080 reuseport backlog=65535 default_server;
        server_name _;
        location = /plaintext {
            gzip off;
            default_type text/plain;
            alias /var/www/tfb/plaintext;
        }
        location = /json {
            gzip off;
            default_type application/json;
            alias /var/www/tfb/json;
        }
    }
}
NGINX_CONF_EOF
rm -f /etc/nginx/sites-enabled/default

mkdir -p /etc/systemd/system/nginx.service.d
printf '[Service]\\nLimitNOFILE=200000\\n' > /etc/systemd/system/nginx.service.d/limits.conf
systemctl daemon-reload
nginx -V 2>&1 | head -1
nginx -t
systemctl restart nginx
systemctl is-active nginx
ss -ltn | grep 8080 || true
echo "=== NGINX_TFB_READY $(date -u) ==="
"""


def launch_instance(ami_id, instance_type, ip, tag, user_data=None):
    wait_for_ip_free(ip)
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
        "--monitoring",
        "Enabled=true",
        "--count",
        "1",
        "--tag-specifications",
        f"ResourceType=instance,Tags=[{{Key=Name,Value={tag}}}]",
        "--region",
        AWS_REGION,
        "--output",
        "json",
    ]
    if user_data:
        cmd += ["--user-data", user_data]
    out = run_cmd(cmd)
    instance_id = json.loads(out)["Instances"][0]["InstanceId"]
    print(f"[SUCCESS] Launched {tag}: {instance_id}")
    while True:
        try:
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
            state = inst["State"]["Name"]
            pub = inst.get("PublicIpAddress", "")
        except Exception as exc:
            print(f"[WARN] describe-instances failed ({exc}); retrying in 3s")
            time.sleep(3)
            continue
        if state == "running" and pub:
            print(f"[SUCCESS] {tag} RUNNING. Public IP: {pub}")
            return instance_id, pub
        time.sleep(3)


def http_get(ip, name, timeout=10):
    with urllib.request.urlopen(f"http://{ip}/{name}", timeout=timeout) as r:
        return r.read().decode("utf-8", "replace")


def get_console(instance_id):
    out = run_cmd(
        [
            "aws",
            "ec2",
            "get-console-output",
            "--instance-id",
            instance_id,
            "--latest",
            "--region",
            AWS_REGION,
            "--output",
            "json",
        ]
    )
    return json.loads(out).get("Output", "")


def cloudwatch_cpu(instance_id, windows):
    """Return (avg, max) CPUUtilization over a list of (t0, t1) windows."""
    if not windows:
        return None, None
    t0 = min(w[0] for w in windows)
    t1 = max(w[1] for w in windows)
    start = datetime.fromtimestamp(t0 - 60, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    end = datetime.fromtimestamp(t1 + 60, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
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


def parse_step_windows(text):
    """STEP_WINDOW ep=<ep> c=<n> r=<n> start=<epoch> end=<epoch>"""
    windows = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 6 and parts[0] == "STEP_WINDOW":
            ep = parts[1].split("=")[1]
            c = int(parts[2].split("=")[1])
            t0 = int(parts[4].split("=")[1])
            t1 = int(parts[5].split("=")[1])
            windows.setdefault((ep, c), []).append((t0, t1))
    return windows


def parse_cpusamps(text):
    """CPUSAMP ep=<ep> c=<n> r=<n> busy=<pct> rc=<n>"""
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 6 and parts[0] == "CPUSAMP":
            ep = parts[1].split("=")[1]
            c = int(parts[2].split("=")[1])
            r = int(parts[3].split("=")[1])
            busy = float(parts[4].split("=")[1])
            rc = int(parts[5].split("=")[1])
            out[(ep, c, r)] = (busy, rc)
    return out


def collect_rows(client_pub, plan, stretch, outdir, args, cpusamps):
    """Download raw wrk files, parse each rep into a row."""
    points = list(plan)
    if stretch:
        points.append(stretch_point())
    rows = []
    for ep, c in points:
        for r in range(1, REPS + 1):
            fname = f"w_{ep}_c{c}_r{r}.txt"
            raw = None
            try:
                raw = http_get(client_pub, fname, timeout=15)
            except Exception as exc:
                raw = None
                print(f"[WARN] fetch {fname} failed: {exc}")
            row = parse_wrk_file(raw) if raw else parse_wrk_file("")
            row["target"] = args.target
            row["instance_type"] = args.instance_type
            row["endpoint"] = ep
            row["concurrency"] = c
            row["rep"] = r
            if raw is None:
                row["status"] = "missing"
            elif row["requests_sec"] <= 0:
                row["status"] = "failed"
            else:
                row["status"] = "ok"
            busy, rc = cpusamps.get((ep, c, r), (None, None))
            row["client_cpu_pct"] = busy
            if rc not in (None, 0) and row["status"] == "ok":
                row["status"] = "ok_timeout"
            if raw is not None:
                (outdir / "raw" / fname).write_text(raw)
            rows.append(row)
    return rows


def compute_medians(rows, args, cpu_by_point):
    """Median per point from the recorded ok reps. Medians are
    computed, never eyeballed. A point with zero ok reps reports
    status failed with no medians."""
    by_point = {}
    for row in rows:
        key = (row["endpoint"], row["concurrency"])
        by_point.setdefault(key, []).append(row)
    medians = []
    for (ep, c), reps in by_point.items():
        ok = [r for r in reps if r["status"] in ("ok", "ok_timeout")]
        entry = {
            "target": args.target,
            "instance_type": args.instance_type,
            "endpoint": ep,
            "concurrency": c,
            "reps_ok": len(ok),
            "total_socket_errors": sum(r["socket_errors"] for r in reps),
            "status": "ok" if ok else "failed",
        }
        if ok:

            def med(field):
                vals = [r[field] for r in ok if r.get(field) is not None]
                return round(statistics.median(vals), 3) if vals else None

            entry["median_requests_sec"] = med("requests_sec")
            entry["min_requests_sec"] = round(min(r["requests_sec"] for r in ok), 1)
            entry["max_requests_sec"] = round(max(r["requests_sec"] for r in ok), 1)
            entry["median_p50_ms"] = med("p50_ms")
            entry["median_p99_ms"] = med("p99_ms")
            entry["median_max_ms"] = med("latency_max_ms")
            entry["median_client_cpu_pct"] = med("client_cpu_pct")
            errors = any(r["socket_errors"] > 0 for r in ok)
            busy_high = (
                entry["median_client_cpu_pct"] is not None
                and entry["median_client_cpu_pct"] >= 92.0
            )
            entry["client_saturated"] = bool(errors or busy_high)
            entry["lower_bound"] = bool(errors or busy_high)
        else:
            entry.update(
                {
                    "median_requests_sec": None,
                    "min_requests_sec": None,
                    "max_requests_sec": None,
                    "median_p50_ms": None,
                    "median_p99_ms": None,
                    "median_max_ms": None,
                    "median_client_cpu_pct": None,
                    "client_saturated": False,
                    "lower_bound": False,
                }
            )
        tw = cpu_by_point.get((ep, c), {})
        entry["target_cpu_avg_pct"] = tw.get("target_avg")
        entry["target_cpu_max_pct"] = tw.get("target_max")
        entry["client_cpu_avg_pct"] = tw.get("client_avg")
        entry["client_cpu_max_pct"] = tw.get("client_max")
        medians.append(entry)
    return medians


def main():
    args = parse_args()
    plan = sweep_plan(args.only)
    validator_src = (Path(__file__).resolve().parent / "tfb_validate.sh").read_text()

    if args.print_plans:
        out = Path(args.print_plans)
        out.mkdir(parents=True, exist_ok=True)
        (out / "client_user_data.sh").write_text(
            client_user_data(args, validator_src, plan, args.stretch)
        )
        (out / "nginx_user_data.sh").write_text(nginx_user_data())
        print(f"[SUCCESS] wrote plans to {out}")
        return 0

    if args.target == "unikraft" and not args.ami_id:
        kernel = Path(args.kernel)
        if not kernel.exists():
            print(f"[ERR] kernel not found: {kernel}")
            return 1

    outdir = Path(args.outdir)
    (outdir / "raw").mkdir(parents=True, exist_ok=True)
    date_str = time.strftime("%Y-%m-%d")
    tag_base = f"tfb-{args.target}-{args.instance_type}"
    kernel_sha = None
    if args.target == "unikraft":
        kernel_sha = hashlib.sha256(Path(args.kernel).read_bytes()).hexdigest()

    print(f"[CONFIG] {json.dumps(vars(args), default=str)}")

    target_id = client_id = None
    created_ami_id = created_snap_id = None
    gate_failed = False
    validate_rc = None
    bench_done = False

    try:
        if args.target == "unikraft":
            if args.ami_id:
                ami_id = args.ami_id
                print(f"[INFO] Reusing registered AMI {ami_id}")
            else:
                disk_raw = create_bootable_disk(Path(args.kernel), SAMPLE_DIR)
                created_snap_id = upload_ebs_snapshot(disk_raw, SAMPLE_DIR)
                created_ami_id = register_ami(created_snap_id)
                ami_id = created_ami_id
            target_id, _ = launch_instance(
                ami_id, args.instance_type, TARGET_PRIVATE_IP, f"{tag_base}-target"
            )
        else:
            target_id, _ = launch_instance(
                UBUNTU_AMI,
                args.instance_type,
                TARGET_PRIVATE_IP,
                f"{tag_base}-target",
                nginx_user_data(),
            )

        print(
            "[INFO] Waiting 45s for boot logs (console API lag), "
            "capturing boot console..."
        )
        time.sleep(45)
        try:
            boot = get_console(target_id)
            (outdir / "target_console_boot.txt").write_text(boot)
            print(f"[SUCCESS] Saved {len(boot)} bytes of boot console")
        except Exception as exc:
            print(f"[WARN] boot console capture failed: {exc}")

        client_id, client_pub = launch_instance(
            UBUNTU_AMI,
            args.client_type,
            CLIENT_PRIVATE_IP,
            f"{tag_base}-client",
            client_user_data(args, validator_src, plan, args.stretch),
        )

        n_points = len(plan) + (1 if args.stretch else 0)
        monitor_budget = 900 + n_points * REPS * (DURATION_S + COOLDOWN_S + 12)
        if args.stretch:
            monitor_budget += 3 * (DURATION_S + 90)
        print(f"[INFO] Monitor budget: {monitor_budget}s")

        client_ready = False
        for _ in range(60):
            try:
                with urllib.request.urlopen(
                    f"http://{client_pub}/diag.txt", timeout=3
                ) as resp:
                    if resp.status == 200:
                        client_ready = True
                        print("[INFO] Client HTTP server is accessible!")
                        break
            except Exception:
                time.sleep(3)
        if not client_ready:
            print("[ERR] client HTTP server did not become ready.")

        last_log = ""
        console_polls = 0
        start_wait = time.time()
        while time.time() - start_wait < monitor_budget:
            try:
                cur = http_get(client_pub, "wrk_sweep.log", timeout=3)
                if cur != last_log:
                    sys.stdout.write(cur[len(last_log) :])
                    sys.stdout.flush()
                    last_log = cur
            except Exception:
                pass
            try:
                diag = http_get(client_pub, "diag.txt", timeout=3)
                if "ALL_DONE" in diag:
                    bench_done = True
                    if "GATE_FAIL" in diag:
                        gate_failed = True
                    m = re.search(r"TFB_VALIDATE_RC=(\d+)", diag)
                    if m:
                        validate_rc = int(m.group(1))
                    if "VALIDATE_FAIL" in diag:
                        validate_rc = validate_rc if validate_rc else 1
                    break
            except Exception:
                pass
            console_polls += 1
            if console_polls % 12 == 1:
                n = console_polls // 12
                try:
                    ctext = get_console(target_id)
                    (outdir / f"target_console_poll{n}.txt").write_text(ctext)
                    last_line = (ctext.rstrip().splitlines() or ["<empty>"])[-1]
                    print(f"[CONSOLE] poll{n}: {len(ctext)} B; last: {last_line[:160]}")
                except Exception as ce:
                    print(f"[CONSOLE] poll{n} failed: {ce}")
            time.sleep(5)

        if not bench_done:
            print(f"[WARN] client did not report ALL_DONE within {monitor_budget}s.")

        print("==================================================")
        print("Step 7: Collecting Artifacts...")
        print("==================================================")
        diag = ""
        try:
            diag = http_get(client_pub, "diag.txt", timeout=10)
            (outdir / "client_diag.txt").write_text(diag)
            print("[INFO] downloaded diag.txt")
        except Exception as e:
            print(f"[WARN] diag.txt fetch failed: {e}")
        try:
            (outdir / "client_sweep_log.txt").write_text(
                http_get(client_pub, "wrk_sweep.log", timeout=10)
            )
        except Exception as e:
            print(f"[WARN] sweep log fetch failed: {e}")
        try:
            (outdir / "tfb_validate.txt").write_text(
                http_get(client_pub, "tfb_validate.txt", timeout=10)
            )
        except Exception as e:
            print(f"[WARN] validator output fetch failed: {e}")
        step_txt = ""
        try:
            step_txt = http_get(client_pub, "step_times.txt", timeout=10)
            (outdir / "step_times.txt").write_text(step_txt)
        except Exception as e:
            print(f"[WARN] step_times fetch failed: {e}")

        cpusamps = parse_cpusamps(diag)
        windows = parse_step_windows(step_txt)
        rows = collect_rows(client_pub, plan, args.stretch, outdir, args, cpusamps)

        # CloudWatch CPU per point, target and client.
        cpu_by_point = {}
        for key, wins in windows.items():
            t_avg, t_max = cloudwatch_cpu(target_id, wins)
            c_avg, c_max = cloudwatch_cpu(client_id, wins)
            cpu_by_point[key] = {
                "target_avg": t_avg,
                "target_max": t_max,
                "client_avg": c_avg,
                "client_max": c_max,
            }
            time.sleep(1)
        medians = compute_medians(rows, args, cpu_by_point)

        stamp = f"{date_str}-{args.target}-{args.instance_type}"
        csv_path = outdir / f"tfb_reps_{stamp}.csv"
        with csv_path.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=CSV_FIELDS, extrasaction="ignore")
            w.writeheader()
            for row in rows:
                w.writerow(row)
        med_path = outdir / f"tfb_medians_{stamp}.csv"
        with med_path.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=MED_FIELDS, extrasaction="ignore")
            w.writeheader()
            for row in medians:
                w.writerow(row)
        result = {
            "meta": {
                "run_date": date_str,
                "target": args.target,
                "instance_type": args.instance_type,
                "client_type": args.client_type,
                "client_threads": args.client_threads,
                "workers": args.workers if args.target == "unikraft" else None,
                "build_rev": args.build_rev if args.target == "unikraft" else None,
                "kernel_sha256": kernel_sha,
                "ami_id": (args.ami_id or created_ami_id)
                if args.target == "unikraft"
                else UBUNTU_AMI,
                "snapshot_id": created_snap_id,
                "target_instance_id": target_id,
                "client_instance_id": client_id,
                "duration_s": DURATION_S,
                "reps": REPS,
                "stretch": args.stretch,
                "gate_failed": gate_failed,
                "validator_rc": validate_rc,
                "deviations": deviations(args, plan),
            },
            "rows": rows,
            "medians": medians,
        }
        json_path = outdir / f"tfb_sweep_{stamp}.json"
        json_path.write_text(json.dumps(result, indent=2))
        print(f"[SUCCESS] wrote {csv_path}")
        print(f"[SUCCESS] wrote {med_path}")
        print(f"[SUCCESS] wrote {json_path}")

        for m in medians:
            reqs = m["median_requests_sec"]
            flag = " [LOWER BOUND]" if m["lower_bound"] else ""
            print(
                f"  [{m['endpoint']} c={m['concurrency']}] "
                f"{reqs} req/s median ({m['reps_ok']}/{REPS} reps ok) "
                f"p50={m['median_p50_ms']}ms p99={m['median_p99_ms']}ms"
                f"{flag}"
            )

        # The nginx target runs no HTTP server, so its setup
        # transcript is captured from the serial console: the user-data
        # tees to /dev/console and target_console_final.txt carries it.

    finally:
        print("==================================================")
        print("Step 8: Teardown...")
        print("==================================================")
        if target_id:
            try:
                time.sleep(10)
                ctext = get_console(target_id)
                (outdir / "target_console_final.txt").write_text(ctext)
                print(f"[SUCCESS] saved {len(ctext)} B final console")
            except Exception as e:
                print(f"[WARN] final console fetch failed: {e}")
        ids = [i for i in (target_id, client_id) if i]
        if ids:
            print(f"[INFO] terminating {ids}")
            try:
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
                        print("[SUCCESS] instances terminated.")
                        break
                    time.sleep(3)
            except Exception as e:
                print(f"[WARN] terminate loop failed: {e}")
        if created_ami_id and not args.keep_ami:
            print(f"[INFO] deregistering AMI {created_ami_id}")
            run_cmd(
                [
                    "aws",
                    "ec2",
                    "deregister-image",
                    "--image-id",
                    created_ami_id,
                    "--region",
                    AWS_REGION,
                ]
            )
        elif created_ami_id:
            print(f"[KEEP-AMI] {created_ami_id}")
        if created_snap_id and not args.keep_ami:
            print(f"[INFO] deleting snapshot {created_snap_id}")
            run_cmd(
                [
                    "aws",
                    "ec2",
                    "delete-snapshot",
                    "--snapshot-id",
                    created_snap_id,
                    "--region",
                    AWS_REGION,
                ]
            )
        elif created_snap_id:
            print(f"[KEEP-SNAP] {created_snap_id}")

    if gate_failed:
        print("[RESULT] GATE_FAIL")
        return 2
    if validate_rc not in (None, 0):
        print(f"[RESULT] VALIDATE_FAIL rc={validate_rc}")
        return 3
    if not bench_done:
        return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
