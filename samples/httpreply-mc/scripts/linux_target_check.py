#!/usr/bin/env python3
"""Discriminator for ticket 1152cbcacdcc.

Runs the same 6x c=50 idle-gate wrk soak against a stock Ubuntu target
instead of the Unikraft kernel. If the stock Linux target also stalls,
the loss is environmental (Nitro/VPC), not in the Unikraft ENA driver.
"""

import json
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

AWS_REGION = "us-east-1"
INSTANCE_TYPE = "c6i.large"
SUBNET_ID = "subnet-0e6ba9e8b1e7dcdf0"
SG_ID = "sg-0ac75f6a09207fcfe"
UBUNTU_AMI = "ami-0045d7fc2ad003464"
TARGET_PRIVATE_IP = "172.31.16.153"
CLIENT_PRIVATE_IP = "172.31.16.161"


def run_cmd(cmd, check=True):
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if check and proc.returncode != 0:
        print(f"[ERR] Command failed: {' '.join(cmd)}")
        print(proc.stdout)
        print(proc.stderr)
        sys.exit(1)
    return proc.stdout


def wait_for_ip_free(ip):
    while True:
        out = run_cmd([
            "aws", "ec2", "describe-network-interfaces",
            "--filters", f"Name=private-ip-address,Values={ip}",
            "--region", AWS_REGION, "--output", "json",
        ])
        if not json.loads(out)["NetworkInterfaces"]:
            return
        time.sleep(5)


def launch_target():
    wait_for_ip_free(TARGET_PRIVATE_IP)
    user_data = """#!/bin/bash
set -e
cat > /root/srv.py <<'PY'
import http.server, socketserver
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"OK\\n"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a):
        pass
class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
S(("0.0.0.0", 80), H).serve_forever()
PY
nohup python3 /root/srv.py > /root/srv.log 2>&1 &
echo "LINUX_TARGET_HTTPD_STARTED $(date)"
"""
    out = run_cmd([
        "aws", "ec2", "run-instances",
        "--image-id", UBUNTU_AMI,
        "--instance-type", INSTANCE_TYPE,
        "--security-group-ids", SG_ID,
        "--subnet-id", SUBNET_ID,
        "--private-ip-address", TARGET_PRIVATE_IP,
        "--user-data", user_data,
        "--count", "1",
        "--tag-specifications", "ResourceType=instance,Tags=[{Key=Name,Value=linux-target-check}]",
        "--region", AWS_REGION, "--output", "json",
    ])
    return json.loads(out)["Instances"][0]["InstanceId"]


def launch_client():
    wait_for_ip_free(CLIENT_PRIVATE_IP)
    user_data = f"""#!/bin/bash
set -e
exec > >(tee -a /root/diag.txt) 2>&1
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y wrk python3

cd /root
python3 -m http.server 80 &

echo "Curl gate: waiting for target http://{TARGET_PRIVATE_IP}/ ..."
OK=0
for i in $(seq 1 120); do
    CODE=$(curl -s -o /dev/null --max-time 5 -w "%{{http_code}}" http://{TARGET_PRIVATE_IP}/ || echo "000")
    if [ "$CODE" = "200" ]; then OK=1; echo "TARGET_HEALTH 200 after $i tries $(date)"; break; fi
    sleep 2
done
if [ "$OK" != "1" ]; then echo "GATE_FAIL" > /root/wrk_sweep.log; echo "ALL_DONE $(date)" >> /root/wrk_sweep.log; exit 0; fi

CONCS="10 25 50 100 200"
for c in $CONCS; do
    CURL_CODE=$(curl -s -o /dev/null -w "code=%{{http_code}} t=%{{time_total}}s" --max-time 3 http://{TARGET_PRIVATE_IP}/ || echo FAIL)
    echo "=== STEP c=$c start (ping=$CURL_CODE) $(date) ===" | tee -a /root/wrk_sweep.log
    RC=0
    timeout --kill-after=5s 45s wrk -t2 -c$c -d30s --latency http://{TARGET_PRIVATE_IP}/ > /root/wrk_out.txt 2>&1 || RC=$?
    echo "=== STEP c=$c end rc=$RC $(date) ===" | tee -a /root/wrk_sweep.log
    sleep 5
done
echo "ALL_DONE $(date)" | tee -a /root/wrk_sweep.log
"""
    out = run_cmd([
        "aws", "ec2", "run-instances",
        "--image-id", UBUNTU_AMI,
        "--instance-type", INSTANCE_TYPE,
        "--security-group-ids", SG_ID,
        "--subnet-id", SUBNET_ID,
        "--private-ip-address", CLIENT_PRIVATE_IP,
        "--user-data", user_data,
        "--count", "1",
        "--tag-specifications", "ResourceType=instance,Tags=[{Key=Name,Value=wrk-mc-client}]",
        "--region", AWS_REGION, "--output", "json",
    ])
    iid = json.loads(out)["Instances"][0]["InstanceId"]
    while True:
        desc = json.loads(run_cmd([
            "aws", "ec2", "describe-instances", "--instance-ids", iid,
            "--region", AWS_REGION, "--output", "json",
        ]))
        inst = desc["Reservations"][0]["Instances"][0]
        if inst["State"]["Name"] == "running" and inst.get("PublicIpAddress"):
            return iid, inst["PublicIpAddress"]
        time.sleep(3)


def main():
    target_id = client_id = None
    try:
        target_id = launch_target()
        print(f"[INFO] Linux target: {target_id}")
        client_id, client_pub = launch_client()
        print(f"[INFO] Client: {client_id} pub={client_pub}")

        done = False
        for _ in range(120):
            time.sleep(10)
            try:
                with urllib.request.urlopen(f"http://{client_pub}/wrk_sweep.log", timeout=5) as r:
                    txt = r.read().decode("utf-8", errors="replace")
                for line in txt.splitlines():
                    if "STEP" in line or "GATE" in line:
                        print(line)
                if "ALL_DONE" in txt:
                    done = True
                    break
            except Exception:
                pass
        print(f"[RESULT] sweep_done={done}")
    finally:
        ids = [i for i in (target_id, client_id) if i]
        if ids:
            run_cmd(["aws", "ec2", "terminate-instances", "--instance-ids", *ids,
                     "--region", AWS_REGION], check=False)
        print("[INFO] Instances terminated; ENIs/volumes auto-release.")


if __name__ == "__main__":
    main()
