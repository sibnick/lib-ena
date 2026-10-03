#!/usr/bin/env python3
"""
Automated End-to-End EC2 Performance and Verification Benchmark.
Deploys Unikraft httpreply-mc and an Ubuntu 24.04 wrk client into AWS EC2 (c6i.large),
executes the full concurrency benchmark sweep, captures console logs and diagnostics,
stores results, and cleans up all cloud resources.
"""

import os
import sys
import json
import time
import base64
import hashlib
import struct
import subprocess
import urllib.request
import urllib.error
import re
import csv
from pathlib import Path

AWS_REGION = "us-east-1"
INSTANCE_TYPE = "c6i.large"
SUBNET_ID = "subnet-0e6ba9e8b1e7dcdf0"
SG_ID = "sg-0ac75f6a09207fcfe"
GATEWAY_IP = "172.31.16.1"
NETMASK = "255.255.240.0"
TARGET_PRIVATE_IP = "172.31.16.153"
CLIENT_PRIVATE_IP = "172.31.16.161"
UBUNTU_AMI = "ami-0045d7fc2ad003464"
BLOCK_SIZE = 524288  # 512 KiB EBS direct block size

def run_cmd(cmd, check=True, capture=True):
    cmd_str = ' '.join(cmd) if isinstance(cmd, list) else cmd
    print(f"[CMD] {cmd_str}")
    res = subprocess.run(
        cmd,
        shell=isinstance(cmd, str),
        check=check,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True
    )
    return res.stdout.strip() if capture and res.stdout else ""

def create_bootable_disk(kernel_path, sample_dir):
    print("==================================================")
    print("Step 1: Creating Bootable Disk Image with GRUB...")
    print("==================================================")
    build_dir = sample_dir / "build"
    staging_dir = build_dir / "staging"
    boot_dir = staging_dir / "boot"
    grub_dir = boot_dir / "grub" / "i386-pc"
    grub_dir.mkdir(parents=True, exist_ok=True)

    import shutil
    shutil.copy(kernel_path, boot_dir / "unikraft.bin")

    for f in Path("/usr/lib/grub/i386-pc").glob("*.mod"):
        shutil.copy(f, grub_dir)
    for f in Path("/usr/lib/grub/i386-pc").glob("*.lst"):
        shutil.copy(f, grub_dir)

    cidr = sum(bin(int(x)).count('1') for x in NETMASK.split('.'))
    grub_cfg = f"""set default=0
set timeout=0

serial --unit=0 --speed=115200
terminal_input serial console
terminal_output serial console

menuentry "Unikraft HTTP Hello World (AWS ENA)" {{
    multiboot /boot/unikraft.bin unikraft netdev.ip={TARGET_PRIVATE_IP}/{cidr}:{GATEWAY_IP} --
    boot
}}
"""
    (boot_dir / "grub" / "grub.cfg").write_text(grub_cfg)

    part1_img = build_dir / "part1.img"
    if part1_img.exists():
        part1_img.unlink()
    run_cmd(f"mkfs.ext4 -F -L rootfs -d {staging_dir} {part1_img} 63M")

    disk_raw = build_dir / "disk.raw"
    if disk_raw.exists():
        disk_raw.unlink()
    with open(disk_raw, "wb") as f:
        f.seek(64 * 1024 * 1024 - 1)
        f.write(b"\0")

    run_cmd(f"parted -s {disk_raw} mklabel msdos")
    run_cmd(f"parted -s {disk_raw} mkpart primary ext4 1MiB 100%")
    run_cmd(f"parted -s {disk_raw} set 1 boot on")

    with open(part1_img, "rb") as src, open(disk_raw, "r+b") as dst:
        dst.seek(1024 * 1024)
        while chunk := src.read(1024 * 1024):
            dst.write(chunk)

    core_img = build_dir / "core.img"
    run_cmd([
        "grub-mkimage", "-O", "i386-pc", "-o", str(core_img),
        "-p", "(hd0,msdos1)/boot/grub",
        "biosdisk", "part_msdos", "ext2", "multiboot", "normal", "configfile", "serial", "terminfo"
    ])

    with open("/usr/lib/grub/i386-pc/boot.img", "rb") as f:
        boot = bytearray(f.read())
    with open(core_img, "rb") as f:
        core = f.read()

    boot[0x1f4:0x1f8] = struct.pack("<I", 1)

    with open(disk_raw, "r+b") as dst:
        dst.seek(0)
        dst.write(boot[:440])
        dst.seek(512)
        dst.write(core)

    print(f"[SUCCESS] Bootable disk image ready at {disk_raw} ({disk_raw.stat().st_size} bytes)")
    return disk_raw

def upload_ebs_snapshot(disk_raw, sample_dir):
    print("==================================================")
    print("Step 2: Uploading Disk to EBS Snapshot via EBS Direct API...")
    print("==================================================")
    desc = f"Unikraft-ENA-httpreply-mc-{int(time.time())}"
    start_out = run_cmd([
        "aws", "ebs", "start-snapshot",
        "--volume-size", "1",
        "--description", desc,
        "--region", AWS_REGION,
        "--output", "json"
    ])
    start_data = json.loads(start_out)
    snapshot_id = start_data["SnapshotId"]
    print(f"[INFO] Started EBS Snapshot: {snapshot_id}")

    disk_size = disk_raw.stat().st_size
    num_blocks = (disk_size + BLOCK_SIZE - 1) // BLOCK_SIZE
    changed_blocks = 0

    with open(disk_raw, "rb") as f:
        for block_idx in range(num_blocks):
            f.seek(block_idx * BLOCK_SIZE)
            data = f.read(BLOCK_SIZE)
            if not data:
                break
            if len(data) < BLOCK_SIZE:
                data = data.ljust(BLOCK_SIZE, b"\0")

            if not any(data):
                continue

            sha = hashlib.sha256(data).digest()
            csum_b64 = base64.b64encode(sha).decode("ascii")

            block_file = sample_dir / f"build/block_{block_idx}.bin"
            block_file.write_bytes(data)

            run_cmd([
                "aws", "ebs", "put-snapshot-block",
                "--snapshot-id", snapshot_id,
                "--block-index", str(block_idx),
                "--block-data", str(block_file),
                "--data-length", str(BLOCK_SIZE),
                "--checksum", csum_b64,
                "--checksum-algorithm", "SHA256",
                "--region", AWS_REGION,
                "--output", "json"
            ])
            block_file.unlink()
            changed_blocks += 1

    print(f"[INFO] Completing EBS snapshot {snapshot_id} (total changed blocks: {changed_blocks})...")
    run_cmd([
        "aws", "ebs", "complete-snapshot",
        "--snapshot-id", snapshot_id,
        "--changed-blocks-count", str(changed_blocks),
        "--region", AWS_REGION,
        "--output", "json"
    ])

    print("[INFO] Waiting for snapshot state to become 'completed'...")
    while True:
        snap_out = run_cmd([
            "aws", "ec2", "describe-snapshots",
            "--snapshot-ids", snapshot_id,
            "--region", AWS_REGION,
            "--query", "Snapshots[0].State",
            "--output", "text"
        ])
        if snap_out == "completed":
            print(f"[SUCCESS] Snapshot {snapshot_id} is completed!")
            break
        time.sleep(3)

    return snapshot_id

def register_ami(snapshot_id):
    print("==================================================")
    print("Step 3: Registering ENA-Enabled AMI...")
    print("==================================================")
    ami_name = f"unikraft-httpreply-mc-{int(time.time())}"
    block_device_mapping = [
        {
            "DeviceName": "/dev/sda1",
            "Ebs": {
                "SnapshotId": snapshot_id,
                "VolumeSize": 1,
                "VolumeType": "gp3",
                "DeleteOnTermination": True
            }
        }
    ]

    reg_out = run_cmd([
        "aws", "ec2", "register-image",
        "--name", ami_name,
        "--description", "Unikraft httpreply-mc Server with Native AWS ENA Driver",
        "--architecture", "x86_64",
        "--virtualization-type", "hvm",
        "--root-device-name", "/dev/sda1",
        "--ena-support",
        "--block-device-mappings", json.dumps(block_device_mapping),
        "--region", AWS_REGION,
        "--output", "json"
    ])
    ami_id = json.loads(reg_out)["ImageId"]
    print(f"[SUCCESS] AMI Registered: {ami_id} ({ami_name})")

    print("[INFO] Waiting for AMI state to become 'available'...")
    while True:
        state = run_cmd([
            "aws", "ec2", "describe-images",
            "--image-ids", ami_id,
            "--region", AWS_REGION,
            "--query", "Images[0].State",
            "--output", "text"
        ])
        if state == "available":
            print(f"[SUCCESS] AMI {ami_id} is available!")
            break
        time.sleep(3)

    return ami_id

def wait_for_ip_free(ip):
    while True:
        enis = run_cmd([
            "aws", "ec2", "describe-network-interfaces",
            "--filters", f"Name=addresses.private-ip-address,Values={ip}",
            "--query", "NetworkInterfaces[].NetworkInterfaceId",
            "--region", AWS_REGION,
            "--output", "json"
        ])
        eni_list = json.loads(enis) if enis else []
        if not eni_list:
            print(f"[SUCCESS] IP {ip} is free!")
            break
        print(f"[INFO] IP {ip} is still bound to ENIs {eni_list}, waiting 5s...")
        time.sleep(5)

def launch_target_instance(ami_id):
    print("==================================================")
    print(f"Step 4: Launching Unikraft Target Instance ({INSTANCE_TYPE})...")
    print("==================================================")
    wait_for_ip_free(TARGET_PRIVATE_IP)
    launch_out = run_cmd([
        "aws", "ec2", "run-instances",
        "--image-id", ami_id,
        "--instance-type", INSTANCE_TYPE,
        "--security-group-ids", SG_ID,
        "--subnet-id", SUBNET_ID,
        "--private-ip-address", TARGET_PRIVATE_IP,
        "--count", "1",
        "--tag-specifications", "ResourceType=instance,Tags=[{Key=Name,Value=unikraft-mc-target}]",
        "--region", AWS_REGION,
        "--output", "json"
    ])
    instance_id = json.loads(launch_out)["Instances"][0]["InstanceId"]
    print(f"[SUCCESS] Launched Unikraft Target: {instance_id}")

    print("[INFO] Waiting for target to become 'running'...")
    while True:
        inst_desc = run_cmd([
            "aws", "ec2", "describe-instances",
            "--instance-ids", instance_id,
            "--region", AWS_REGION,
            "--output", "json"
        ])
        inst = json.loads(inst_desc)["Reservations"][0]["Instances"][0]
        state = inst["State"]["Name"]
        public_ip = inst.get("PublicIpAddress", "")
        if state == "running" and public_ip:
            print(f"[SUCCESS] Target is RUNNING! Public IP: {public_ip}, Private IP: {TARGET_PRIVATE_IP}")
            return instance_id, public_ip
        time.sleep(3)

def repair_public_ip(instance_id):
    print("==================================================")
    print("Step 4b: Repairing stale public IP association...")
    print("==================================================")
    desc_out = run_cmd([
        "aws", "ec2", "describe-instances",
        "--instance-ids", instance_id,
        "--region", AWS_REGION,
        "--output", "json"
    ])
    inst = json.loads(desc_out)["Reservations"][0]["Instances"][0]
    old_ip = inst.get("PublicIpAddress", "")
    if old_ip:
        try:
            run_cmd([
                "aws", "ec2", "disassociate-address",
                "--public-ip", old_ip,
                "--region", AWS_REGION,
                "--output", "json"
            ])
            print(f"[INFO] Disassociated stale auto public IP {old_ip}")
        except Exception as e:
            print(f"[INFO] Disassociate of {old_ip} reported: {e}")
    time.sleep(5)
    alloc_out = run_cmd([
        "aws", "ec2", "allocate-address",
        "--domain", "vpc",
        "--region", AWS_REGION,
        "--output", "json"
    ])
    alloc = json.loads(alloc_out)
    allocation_id = alloc["AllocationId"]
    new_ip = alloc.get("PublicIp", "")
    run_cmd([
        "aws", "ec2", "associate-address",
        "--allocation-id", allocation_id,
        "--instance-id", instance_id,
        "--allow-reassociation",
        "--region", AWS_REGION,
        "--output", "json"
    ])
    print(f"[SUCCESS] Fresh EIP {new_ip} ({allocation_id}) associated to {instance_id}")
    return new_ip, allocation_id

def launch_client_instance():
    print("==================================================")
    print(f"Step 5: Launching wrk Client Instance ({INSTANCE_TYPE})...")
    print("==================================================")
    wait_for_ip_free(CLIENT_PRIVATE_IP)
    user_data = f"""#!/bin/bash
set -e
exec > >(tee -a /root/diag.txt) 2>&1

echo "=== RUNALL_START $(date) target={TARGET_PRIVATE_IP} ==="
echo "T0_EPOCH=$(date +%s)"

export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y wrk python3 tcpdump

wrk -v || true
echo "WRK_READY $(date)"

ip -4 -o addr show
nohup tcpdump -i eth0 -w /root/cap.pcap -s 0 >/root/cap.log 2>&1 &
CAP_PID=$!
echo "CAP_STARTED pid=$CAP_PID $(date)"
which tcpdump || echo NO_TCPDUMP_BINARY

cd /root
python3 -m http.server 80 &
echo "HTTPD_STARTED $(date)"

echo "Curl gate: waiting for target http://{TARGET_PRIVATE_IP}/ ..."
UK_OK=0
for i in $(seq 1 120); do
    CODE=$(curl -s -o /dev/null --max-time 5 -w "%{{http_code}}" http://{TARGET_PRIVATE_IP}/ || echo "000")
    if [ "$CODE" = "200" ]; then
        UK_OK=1
        echo "UK_HEALTH private=200 $(date) after $i tries"
        break
    fi
    sleep 2
done

kill -INT "$CAP_PID" 2>/dev/null || true
sleep 2
ls -la /root/cap.pcap || echo CAP_MISSING
echo "CAP_STOPPED $(date)"

if [ "$UK_OK" != "1" ]; then
    echo "GATE_FAIL: target never returned HTTP 200; wrk sweep NOT started" > /root/wrk_sweep.log
    echo "GATE_FAIL: target never returned HTTP 200; wrk sweep NOT started" >> /root/diag.txt
    echo "ALL_DONE $(date)" >> /root/wrk_sweep.log
    echo "ALL_DONE $(date)" >> /root/diag.txt
    exit 0
fi

echo "Starting wrk sweep $(date)..." > /root/wrk_sweep.log

CONCS="10 25 50 100 200"
STEP_N=0
for c in $CONCS; do
    CURL_CODE=$(curl -s -o /dev/null -w "code=%{{http_code}} t=%{{time_total}}s" --max-time 3 http://{TARGET_PRIVATE_IP}/ || echo FAIL)
    echo "=== STEP s1 c=$c start (ping=$CURL_CODE) $(date) ===" | tee -a /root/wrk_sweep.log
    RC=0
    STEP_N=$((STEP_N + 1))
    (timeout 40s tcpdump -i any -nn -c 150000 "tcp port 80 and host {TARGET_PRIVATE_IP}" > /root/cap_$STEP_N.txt 2>&1 &)
    sleep 2
    timeout --kill-after=5s 45s wrk -t2 -c$c -d30s --latency http://{TARGET_PRIVATE_IP}/ > /root/wrk_s1_c$c.txt 2>&1 || RC=$?
    echo "=== STEP s1 c=$c end rc=$RC $(date) ===" | tee -a /root/wrk_sweep.log
    echo "STEP_DONE c=$c rc=$RC" >> /root/diag.txt
    if [ ! -s /root/wrk_s1_c$c.txt ]; then
        echo "wrk did not produce output (exit code $RC)" > /root/wrk_s1_c$c.txt
    fi
    sleep 5
done

echo "SWEEP1_DONE $(date)" | tee -a /root/wrk_sweep.log /root/diag.txt
echo "ALL_DONE $(date)" | tee -a /root/wrk_sweep.log /root/diag.txt
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
        "--tag-specifications", "ResourceType=instance,Tags=[{Key=Name,Value=wrk-mc-client}]",
        "--region", AWS_REGION,
        "--output", "json"
    ])
    instance_id = json.loads(launch_out)["Instances"][0]["InstanceId"]
    print(f"[SUCCESS] Launched Client Instance: {instance_id}")

    print("[INFO] Waiting for client to become 'running'...")
    while True:
        inst_desc = run_cmd([
            "aws", "ec2", "describe-instances",
            "--instance-ids", instance_id,
            "--region", AWS_REGION,
            "--output", "json"
        ])
        inst = json.loads(inst_desc)["Reservations"][0]["Instances"][0]
        state = inst["State"]["Name"]
        public_ip = inst.get("PublicIpAddress", "")
        if state == "running" and public_ip:
            print(f"[SUCCESS] Client is RUNNING! Public IP: {public_ip}, Private IP: {CLIENT_PRIVATE_IP}")
            return instance_id, public_ip
        time.sleep(3)

def parse_wrk_file(content):
    data = {
        "requests_sec": 0.0,
        "transfer_kb_sec": 0.0,
        "latency_avg_ms": 0.0,
        "latency_stdev_ms": 0.0,
        "latency_max_ms": 0.0,
        "total_requests": 0,
        "socket_errors": 0,
        "p50_ms": 0.0,
        "p75_ms": 0.0,
        "p90_ms": 0.0,
        "p99_ms": 0.0
    }
    req_match = re.search(r"Requests/sec:\s+([\d\.]+)", content)
    if req_match:
        data["requests_sec"] = float(req_match.group(1))

    tx_match = re.search(r"Transfer/sec:\s+([\d\.]+)\s*([KMG]?B)", content)
    if tx_match:
        val = float(tx_match.group(1))
        unit = tx_match.group(2)
        if unit == "MB": val *= 1024.0
        elif unit == "GB": val *= 1024.0 * 1024.0
        data["transfer_kb_sec"] = val

    def to_ms(v, u):
        v = float(v)
        if u == "us": return v / 1000.0
        if u == "ms": return v
        if u == "s": return v * 1000.0
        if u == "m": return v * 60000.0
        return v

    lat_match = re.search(r"Latency\s+([\d\.]+)(\w+)\s+([\d\.]+)(\w+)\s+([\d\.]+)(\w+)", content)
    if lat_match:
        data["latency_avg_ms"] = to_ms(lat_match.group(1), lat_match.group(2))
        data["latency_stdev_ms"] = to_ms(lat_match.group(3), lat_match.group(4))
        data["latency_max_ms"] = to_ms(lat_match.group(5), lat_match.group(6))

    for p in ["50", "75", "90", "99"]:
        pm = re.search(rf"\s+{p}%\s+([\d\.]+)(\w+)", content)
        if pm:
            data[f"p{p}_ms"] = to_ms(pm.group(1), pm.group(2))

    tot_match = re.search(r"(\d+)\s+requests in", content)
    if tot_match:
        data["total_requests"] = int(tot_match.group(1))

    err_match = re.search(r"Socket errors:\s+connect\s+(\d+),\s+read\s+(\d+),\s+write\s+(\d+),\s+timeout\s+(\d+)", content)
    if err_match:
        data["socket_errors"] = sum(int(err_match.group(i)) for i in range(1, 5))

    return data

def main():
    repo_root = Path(__file__).resolve().parent.parent.parent.parent
    sample_dir = repo_root / "samples/httpreply-mc"
    kernel_path = sample_dir / "build/httpreply-mc_qemu-x86_64"

    if not kernel_path.exists():
        print(f"[ERR] Kernel not found: {kernel_path}")
        sys.exit(1)

    target_id = None
    client_id = None
    ami_id = None
    snapshot_id = None
    eip_allocation_id = None
    gate_failed = False
    date_str = "2026-09-30"

    try:
        disk_raw = create_bootable_disk(kernel_path, sample_dir)
        snapshot_id = upload_ebs_snapshot(disk_raw, sample_dir)
        ami_id = register_ami(snapshot_id)

        target_id, target_pub_ip = launch_target_instance(ami_id)

        # Early boot-console capture. The probe, reset, and MSI-X arm
        # logs scroll out of the 64 KB console ring once the heartbeat
        # loop runs, so grab the ring now, before the benchmark.
        try:
            print("[INFO] Waiting 45s for boot/probe logs (console API lag), then capturing console...")
            time.sleep(45)
            boot_out = run_cmd([
                "aws", "ec2", "get-console-output",
                "--instance-id", target_id,
                "--latest",
                "--region", AWS_REGION,
                "--output", "json",
            ])
            boot_text = json.loads(boot_out).get("Output", "")
            boot_path = sample_dir / f"unikraft_console_boot_{date_str}.txt"
            boot_path.write_text(boot_text)
            print(f"[SUCCESS] Saved {len(boot_text)} bytes of boot console to {boot_path}")
        except Exception as exc:
            print(f"[WARN] Early boot-console capture failed: {exc}")

        # The public-IP path in this VPC black-holes (stale ENI
        # associations), so it is probed once, for diagnostics only.
        # The gate is the client-side curl to the target private IP on
        # port 80. The wrk sweep runs only after that answers 200.
        try:
            with urllib.request.urlopen(f"http://{target_pub_ip}/", timeout=10) as resp:
                if resp.status == 200:
                    body = resp.read().decode("utf-8", "replace").strip()
                    print(f"[INFO] Public IP path answers (diagnostic): {body}")
                else:
                    print(f"[INFO] Public IP path answered status {resp.status} (diagnostic).")
        except Exception:
            print("[INFO] Public IP path does not answer (expected in this VPC).")
            print("[INFO] The gate is the client-side curl to the private IP on port 80.")

        client_id, client_pub_ip = launch_client_instance()

        print("==================================================")
        print("Step 6: Waiting for wrk Client to execute benchmark sweep...")
        print(f"Target Public IP: {target_pub_ip}, Client Public IP: {client_pub_ip}")
        print("==================================================")

        client_ready = False
        for wait_client in range(60):
            try:
                with urllib.request.urlopen(f"http://{client_pub_ip}/diag.txt", timeout=3) as resp:
                    if resp.status == 200:
                        client_ready = True
                        print("[INFO] Client HTTP server is accessible!")
                        break
            except Exception:
                time.sleep(3)

        if not client_ready:
            print("[ERR] wrk client HTTP server did not become ready.")

        # Monitor wrk progress
        last_log = ""
        bench_done = False
        console_poll_count = 0
        last_console = ""
        silent_polls = 0
        start_wait = time.time()
        while time.time() - start_wait < 2400:
            try:
                with urllib.request.urlopen(f"http://{client_pub_ip}/wrk_sweep.log", timeout=3) as resp:
                    curr_log = resp.read().decode("utf-8", errors="replace")
                    if curr_log != last_log:
                        new_lines = curr_log[len(last_log):]
                        sys.stdout.write(new_lines)
                        sys.stdout.flush()
                        last_log = curr_log
            except Exception:
                pass

            try:
                with urllib.request.urlopen(f"http://{client_pub_ip}/diag.txt", timeout=3) as resp:
                    diag = resp.read().decode("utf-8", errors="replace")
                    if "ALL_DONE" in diag:
                        bench_done = True
                        if "GATE_FAIL" in diag:
                            gate_failed = True
                        print("\n[SUCCESS] Benchmark run completed on client!")
                        break
            except Exception:
                pass

            console_poll_count += 1
            if console_poll_count % 12 == 1:
                poll_n = console_poll_count // 12
                try:
                    console_out = run_cmd([
                        "aws", "ec2", "get-console-output",
                        "--instance-id", target_id,
                        "--latest",
                        "--region", AWS_REGION,
                        "--output", "json"
                    ])
                    cdata = json.loads(console_out)
                    ctext = cdata.get("Output", "")
                    if last_console != "" and ctext == last_console:
                        silent_polls += 1
                    else:
                        silent_polls = 0
                    last_console = ctext
                    cpath = sample_dir / f"target_console_poll{poll_n}_{date_str}.txt"
                    cpath.write_text(ctext)
                    last_line = (ctext.rstrip().splitlines() or ["<empty>"])[-1]
                    print(f"[CONSOLE] poll{poll_n} saved {len(ctext)} bytes; last: {last_line[:200]}")
                    if silent_polls >= 3:
                        print(f"[HANG] Target console silent for 180 s (3 identical polls). Stopping monitor early.")
                        break
                except Exception as ce:
                    print(f"[CONSOLE] poll{poll_n} failed: {ce}")

            time.sleep(5)

        if not bench_done:
            print("[WARN] Benchmark timed out after 10 minutes.")

        # Download client artifacts
        print("==================================================")
        print("Step 7: Collecting Artifacts...")
        print("==================================================")
        diag_content = ""
        try:
            with urllib.request.urlopen(f"http://{client_pub_ip}/diag.txt", timeout=5) as resp:
                diag_content = resp.read().decode("utf-8", errors="replace")
                (sample_dir / "diag.txt").write_text(diag_content)
                print("[INFO] Downloaded diag.txt")
        except Exception as e:
            print(f"[WARN] Failed to download diag.txt: {e}")

        sweep_log_content = ""
        try:
            with urllib.request.urlopen(f"http://{client_pub_ip}/wrk_sweep.log", timeout=5) as resp:
                sweep_log_content = resp.read().decode("utf-8", errors="replace")
                (sample_dir / "wrk_sweep.log").write_text(sweep_log_content)
                print("[INFO] Downloaded wrk_sweep.log")
        except Exception as e:
            print(f"[WARN] Failed to download wrk_sweep.log: {e}")

        # Download wrk output files and parse metrics
        concurrencies = [10, 25, 50, 100, 200]
        benchmark_results = []
        try:
            for i in range(1, len(concurrencies) + 1):
                try:
                    with urllib.request.urlopen(f"http://{client_pub_ip}/cap_{i}.txt", timeout=5) as resp:
                        cap = resp.read().decode("utf-8", errors="replace")
                        (sample_dir / f"client_tcpdump_step{i}.txt").write_text(cap)
                        print(f"[INFO] Downloaded client_tcpdump_step{i}.txt")
                except Exception as e:
                    print(f"[WARN] Could not retrieve cap_{i}.txt: {e}")
        except Exception as e:
            print(f"[WARN] cap retrieval failed: {e}")
        for c in concurrencies:
            fname = f"wrk_s1_c{c}.txt"
            try:
                with urllib.request.urlopen(f"http://{client_pub_ip}/{fname}", timeout=5) as resp:
                    txt = resp.read().decode("utf-8", errors="replace")
                    (sample_dir / fname).write_text(txt)
                    print(f"[INFO] Downloaded {fname}")
                    metrics = parse_wrk_file(txt)
                    metrics["concurrency"] = c
                    metrics["target"] = "Unikraft Multi-Core (httpreply-mc) [verified]"
                    metrics["url"] = f"http://{TARGET_PRIVATE_IP}/"
                    benchmark_results.append(metrics)
            except Exception as e:
                print(f"[WARN] Could not retrieve {fname}: {e}")
                benchmark_results.append({
                    "concurrency": c,
                    "target": "Unikraft Multi-Core (httpreply-mc) [verified]",
                    "url": f"http://{TARGET_PRIVATE_IP}/",
                    "requests_sec": 0.0,
                    "transfer_kb_sec": 0.0,
                    "latency_avg_ms": 0.0,
                    "latency_stdev_ms": 0.0,
                    "latency_max_ms": 0.0,
                    "total_requests": 0,
                    "socket_errors": 0,
                    "p50_ms": 0.0,
                    "p75_ms": 0.0,
                    "p90_ms": 0.0,
                    "p99_ms": 0.0
                })

        # Download the gate-phase packet capture (tcpdump on the client eth0)
        try:
            with urllib.request.urlopen(f"http://{client_pub_ip}/cap.pcap", timeout=15) as resp:
                pcap_bytes = resp.read()
                pcap_path = sample_dir / f"cap_{date_str}.pcap"
                pcap_path.write_bytes(pcap_bytes)
                print(f"[INFO] Downloaded cap.pcap ({len(pcap_bytes)} bytes) -> {pcap_path}")
        except Exception as e:
            print(f"[WARN] Could not retrieve cap.pcap: {e}")

        # Save JSON & CSV
        date_str = "2026-09-30"
        json_path = sample_dir / f"benchmark_results_{date_str}.json"
        csv_path = sample_dir / f"benchmark_results_{date_str}.csv"

        with open(json_path, "w") as f:
            json.dump(benchmark_results, f, indent=2)

        csv_fields = ["concurrency", "target", "requests_sec", "latency_avg_ms", "latency_stdev_ms", "latency_max_ms", "transfer_kb_sec", "total_requests", "socket_errors", "p50_ms", "p75_ms", "p90_ms", "p99_ms"]
        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=csv_fields, extrasaction='ignore')
            writer.writeheader()
            for row in benchmark_results:
                writer.writerow(row)
        print(f"[SUCCESS] Saved results to {json_path} and {csv_path}")

        if any(r.get("requests_sec", 0) > 0 for r in benchmark_results):
            with open(sample_dir / "benchmark_results.json", "w") as f:
                json.dump(benchmark_results, f, indent=2)
            with open(sample_dir / "benchmark_results.csv", "w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=csv_fields, extrasaction='ignore')
                writer.writeheader()
                for row in benchmark_results:
                    writer.writerow(row)
            print(f"[SUCCESS] Updated latest {sample_dir / 'benchmark_results.json'} and {sample_dir / 'benchmark_results.csv'}")

        # Capture EC2 console output of Unikraft instance
        print("Waiting 15s for console ring buffer to flush...")
        time.sleep(15)
        console_out = run_cmd([
            "aws", "ec2", "get-console-output",
            "--instance-id", target_id,
            "--latest",
            "--region", AWS_REGION,
            "--output", "json"
        ])
        console_data = json.loads(console_out)
        console_text = console_data.get("Output", "")
        evidence_path = sample_dir / f"unikraft_console_evidence_{date_str}.txt"
        evidence_path.write_text(console_text)
        print(f"[SUCCESS] Saved {len(console_text)} bytes of console output to {evidence_path}")

    finally:
        print("==================================================")
        print("Step 8: Teardown and Cleanup of AWS Resources...")
        print("==================================================")
        evidence_path = sample_dir / f"unikraft_console_evidence_{date_str}.txt"
        if target_id:
            try:
                print("[INFO] Fetching console output before teardown...")
                time.sleep(10)
                console_out = run_cmd([
                    "aws", "ec2", "get-console-output",
                    "--instance-id", target_id,
                    "--latest",
                    "--region", AWS_REGION,
                    "--output", "json"
                ])
                console_data = json.loads(console_out)
                console_text = console_data.get("Output", "")
                if gate_failed:
                    out_path = sample_dir / f"unikraft_console_evidence_gatefail_{date_str}.txt"
                    print(f"[GATE] Saving gate-fail evidence to {out_path} (standard evidence file kept as-is)")
                else:
                    out_path = evidence_path
                out_path.write_text(console_text)
                print(f"[SUCCESS] Saved {len(console_text)} bytes of console output to {out_path}")
            except Exception as e:
                print(f"[WARN] Failed to fetch console output: {e}")

        instances_to_term = [i for i in [target_id, client_id] if i]
        if instances_to_term:
            print(f"[INFO] Terminating instances: {instances_to_term}")
            run_cmd(["aws", "ec2", "terminate-instances", "--instance-ids"] + instances_to_term + ["--region", AWS_REGION])
            print("[INFO] Waiting for instances to terminate...")
            while True:
                t_desc = run_cmd([
                    "aws", "ec2", "describe-instances",
                    "--instance-ids"] + instances_to_term + [
                    "--query", "Reservations[].Instances[].State.Name",
                    "--region", AWS_REGION,
                    "--output", "json"
                ])
                states = json.loads(t_desc)
                if all(s in ("terminated", "shutting-down") for s in states):
                    print("[SUCCESS] All instances terminated.")
                    break
                time.sleep(3)

        if eip_allocation_id:
            print(f"[INFO] Releasing EIP {eip_allocation_id}")
            try:
                run_cmd(["aws", "ec2", "disassociate-address", "--allocation-id", eip_allocation_id, "--region", AWS_REGION])
            except Exception as e:
                print(f"[INFO] EIP disassociate reported: {e}")
            run_cmd(["aws", "ec2", "release-address", "--allocation-id", eip_allocation_id, "--region", AWS_REGION])

        if ami_id:
            print(f"[INFO] Deregistering AMI: {ami_id}")
            run_cmd(["aws", "ec2", "deregister-image", "--image-id", ami_id, "--region", AWS_REGION])

        if snapshot_id:
            print(f"[INFO] Deleting EBS Snapshot: {snapshot_id}")
            run_cmd(["aws", "ec2", "delete-snapshot", "--snapshot-id", snapshot_id, "--region", AWS_REGION])

        print("[SUCCESS] Cloud resources successfully torn down!")

if __name__ == "__main__":
    main()
