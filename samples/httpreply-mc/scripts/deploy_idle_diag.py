#!/usr/bin/env python3
"""
Deploy the idle-halt diagnostic image [Ticket 6f89cf874e].

Builds a 64 MB bootable disk image around the freshly built
httpreply-mc kernel, uploads it as an EBS direct snapshot, registers
a HVM ENA AMI, and launches one c6i.large in the standard subnet.
The box stays running; this script only prints the instance and
public IP.

Usage:
    python3 scripts/deploy_idle_diag.py
"""

import base64
import hashlib
import json
import shutil
import struct
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build"
STAGING_DIR = BUILD_DIR / "staging"

AWS_REGION = "us-east-1"
INSTANCE_TYPE = "c6i.large"
SUBNET_ID = "subnet-0e6ba9e8b1e7dcdf0"
SG_ID = "sg-0ac75f6a09207fcfe"
TARGET_PRIVATE_IP = "172.31.16.153"
GATEWAY_IP = "172.31.16.1"
BLOCK_SIZE = 524288  # 512 KiB EBS direct block size
NAME_TAG = "unikraft-mc-apicdiag"


def run_cmd(cmd, check=True, capture=True):
    cmd_str = " ".join(cmd) if isinstance(cmd, list) else cmd
    print(f"[CMD] {cmd_str}")
    res = subprocess.run(
        cmd,
        shell=isinstance(cmd, str),
        check=check,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True,
    )
    if not capture and res.returncode != 0 and check:
        raise RuntimeError(f"command failed: {cmd_str}")
    return res.stdout.strip() if capture and res.stdout else ""


def create_bootable_disk(kernel_path):
    boot_dir = STAGING_DIR / "boot"
    grub_dir = boot_dir / "grub" / "i386-pc"
    grub_dir.mkdir(parents=True, exist_ok=True)

    shutil.copy(kernel_path, boot_dir / "unikraft.bin")
    for f in Path("/usr/lib/grub/i386-pc").glob("*.mod"):
        shutil.copy(f, grub_dir)
    for f in Path("/usr/lib/grub/i386-pc").glob("*.lst"):
        shutil.copy(f, grub_dir)

    grub_cfg = f"""set default=0
set timeout=0

serial --unit=0 --speed=115200
terminal_input serial console
terminal_output serial console

menuentry "Unikraft HTTP Hello World (AWS ENA)" {{
    multiboot /boot/unikraft.bin unikraft netdev.ip={TARGET_PRIVATE_IP}/20:{GATEWAY_IP} --
    boot
}}
"""
    (boot_dir / "grub" / "grub.cfg").write_text(grub_cfg)

    part1_img = BUILD_DIR / "part1.img"
    if part1_img.exists():
        part1_img.unlink()
    run_cmd(f"mkfs.ext4 -F -L rootfs -d {STAGING_DIR} {part1_img} 63M")

    disk_raw = BUILD_DIR / "disk.raw"
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
    core_img = BUILD_DIR / "core.img"
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


def upload_ebs_snapshot(disk_raw):
    desc = f"{NAME_TAG}-{int(time.time())}"
    start_out = run_cmd([
        "aws", "ebs", "start-snapshot",
        "--volume-size", "1",
        "--description", desc,
        "--region", AWS_REGION,
        "--output", "json",
    ])
    snapshot_id = json.loads(start_out)["SnapshotId"]
    print(f"[INFO] Started EBS snapshot {snapshot_id}")

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
            block_file = BUILD_DIR / f"block_{block_idx}.bin"
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
                "--output", "json",
            ])
            block_file.unlink()
            changed_blocks += 1
            print(f"[INFO] uploaded block {block_idx} ({changed_blocks} non-empty)")

    run_cmd([
        "aws", "ebs", "complete-snapshot",
        "--snapshot-id", snapshot_id,
        "--changed-blocks-count", str(changed_blocks),
        "--region", AWS_REGION,
        "--output", "json",
    ])

    print("[INFO] Waiting for snapshot to become 'completed'...")
    while True:
        state = run_cmd([
            "aws", "ec2", "describe-snapshots",
            "--snapshot-ids", snapshot_id,
            "--region", AWS_REGION,
            "--query", "Snapshots[0].State",
            "--output", "text",
        ])
        if state == "completed":
            print(f"[SUCCESS] Snapshot {snapshot_id} completed")
            break
        print(f"[INFO] snapshot state: {state}, waiting 3 s")
        time.sleep(3)
    return snapshot_id


def register_ami(snapshot_id):
    ami_name = f"{NAME_TAG}-{int(time.time())}"
    reg_out = run_cmd([
        "aws", "ec2", "register-image",
        "--name", ami_name,
        "--description", "Unikraft httpreply-mc APIC-mode diagnostic (per-vCPU one-shot wake investigation)",
        "--architecture", "x86_64",
        "--virtualization-type", "hvm",
        "--root-device-name", "/dev/sda1",
        "--ena-support",
        "--block-device-mappings", json.dumps([
            {
                "DeviceName": "/dev/sda1",
                "Ebs": {
                    "SnapshotId": snapshot_id,
                    "VolumeSize": 1,
                    "VolumeType": "gp3",
                    "DeleteOnTermination": True,
                },
            }
        ]),
        "--region", AWS_REGION,
        "--output", "json",
    ])
    ami_id = json.loads(reg_out)["ImageId"]
    print(f"[SUCCESS] AMI registered: {ami_id} ({ami_name})")

    while True:
        state = run_cmd([
            "aws", "ec2", "describe-images",
            "--image-ids", ami_id,
            "--region", AWS_REGION,
            "--query", "Images[0].State",
            "--output", "text",
        ])
        if state == "available":
            print(f"[SUCCESS] AMI {ami_id} available")
            break
        print(f"[INFO] AMI state: {state}, waiting 3 s")
        time.sleep(3)
    return ami_id


def wait_for_ip_free(ip):
    while True:
        enis = run_cmd([
            "aws", "ec2", "describe-network-interfaces",
            "--filters", f"Name=addresses.private-ip-address,Values={ip}",
            "--query", "NetworkInterfaces[].NetworkInterfaceId",
            "--region", AWS_REGION,
            "--output", "json",
        ])
        if not json.loads(enis if enis else "[]"):
            print(f"[SUCCESS] IP {ip} is free")
            return
        time.sleep(3)


def launch_instance(ami_id):
    wait_for_ip_free(TARGET_PRIVATE_IP)
    launch_out = run_cmd([
        "aws", "ec2", "run-instances",
        "--image-id", ami_id,
        "--instance-type", INSTANCE_TYPE,
        "--security-group-ids", SG_ID,
        "--subnet-id", SUBNET_ID,
        "--private-ip-address", TARGET_PRIVATE_IP,
        "--count", "1",
        f"--tag-specifications", f"ResourceType=instance,Tags=[{{Key=Name,Value={NAME_TAG}}}]",
        "--region", AWS_REGION,
        "--output", "json",
    ])
    inst = json.loads(launch_out)["Instances"][0]
    instance_id = inst["InstanceId"]
    print(f"[SUCCESS] Launched {instance_id}")

    print("[INFO] Waiting for running state and public IP...")
    while True:
        desc = json.loads(run_cmd([
            "aws", "ec2", "describe-instances",
            "--instance-ids", instance_id,
            "--region", AWS_REGION,
            "--output", "json",
        ]))
        i = desc["Reservations"][0]["Instances"][0]
        state = i["State"]["Name"]
        public_ip = i.get("PublicIpAddress", "")
        if state == "running" and public_ip:
            print(f"[SUCCESS] Instance RUNNING. Public IP: {public_ip}")
            return instance_id, public_ip
        print(f"[INFO] state={state} ip={public_ip}, waiting 3 s")
        time.sleep(3)


def main():
    kernels = [
        p for p in BUILD_DIR.glob("*_qemu-x86_64")
        if not p.name.endswith((".dbg", ".cmd", ".bootinfo"))
    ]
    if not kernels:
        raise RuntimeError("kernel not found in build/; run make first")
    kernel = max(kernels, key=lambda p: p.stat().st_mtime)
    print(f"[INFO] Using kernel {kernel} ({kernel.stat().st_mtime})")

    disk_raw = create_bootable_disk(kernel)
    snapshot_id = upload_ebs_snapshot(disk_raw)
    ami_id = register_ami(snapshot_id)
    instance_id, public_ip = launch_instance(ami_id)

    print("==================================================")
    print("DEPLOYMENT COMPLETE (box left running)")
    print(f"Instance   : {instance_id}")
    print(f"Public IP  : {public_ip}")
    print(f"AMI        : {ami_id}")
    print(f"Snapshot   : {snapshot_id}")
    print("The console output will become available in ~12-15 minutes.")
    print(f"  aws ec2 get-console-output --instance-id {instance_id} --region {AWS_REGION}")
    print("==================================================")


if __name__ == "__main__":
    main()
