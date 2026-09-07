#!/usr/bin/env python3
"""
Deploy Unikraft Hello World Web Server (with AWS ENA driver) to AWS EC2 (t3.nano).
Uses AWS CLI and direct EBS Snapshot upload.

Required environment variables:
    SUBNET_ID    - EC2 subnet to launch the instance into
    PRIVATE_IP   - private IP to assign to the instance
    GATEWAY_IP   - VPC gateway (used in grub.cfg netdev args)
    NETMASK      - VPC netmask (used in grub.cfg netdev args)

Optional environment variables:
    AWS_REGION   - defaults to us-east-1
    INSTANCE_TYPE - defaults to t3.nano
"""

import os
import sys
import json
import time
import base64
import hashlib
import struct
import subprocess
from pathlib import Path

AWS_REGION = os.environ.get("AWS_REGION", "us-east-1")
INSTANCE_TYPE = os.environ.get("INSTANCE_TYPE", "t3.nano")
SUBNET_ID = os.environ.get("SUBNET_ID", "")
PRIVATE_IP = os.environ.get("PRIVATE_IP", "")
GATEWAY_IP = os.environ.get("GATEWAY_IP", "")
NETMASK = os.environ.get("NETMASK", "")
BLOCK_SIZE = 524288  # 512 KiB EBS direct block size

def run_cmd(cmd, check=True, capture=True):
    print(f"[CMD] {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    res = subprocess.run(
        cmd,
        shell=isinstance(cmd, str),
        check=check,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True
    )
    return res.stdout.strip() if capture and res.stdout else ""

def build_unikraft():
    print("==================================================")
    print("Step 1: Building Unikraft Unikernel with ENA...")
    print("==================================================")
    kernels = [p for p in Path("build").glob("*_qemu-x86_64") if not p.name.endswith(".dbg") and not p.name.endswith(".cmd")]
    if not kernels:
        raise RuntimeError("Kernel binary not found in build/*_qemu-x86_64")
    kernel_path = kernels[0]
    print(f"[SUCCESS] Kernel built successfully: {kernel_path} ({kernel_path.stat().st_size} bytes)")
    return kernel_path

def create_bootable_disk(kernel_path):
    print("==================================================")
    print("Step 2: Creating Bootable Disk Image with GRUB...")
    print("==================================================")
    build_dir = Path("build")
    staging_dir = build_dir / "staging"
    boot_dir = staging_dir / "boot"
    grub_dir = boot_dir / "grub" / "i386-pc"
    grub_dir.mkdir(parents=True, exist_ok=True)

    # Copy kernel
    import shutil
    shutil.copy(kernel_path, boot_dir / "unikraft.bin")

    # Copy grub modules
    for f in Path("/usr/lib/grub/i386-pc").glob("*.mod"):
        shutil.copy(f, grub_dir)
    for f in Path("/usr/lib/grub/i386-pc").glob("*.lst"):
        shutil.copy(f, grub_dir)

    # Write grub.cfg
    grub_cfg = f"""set default=0
set timeout=0

serial --unit=0 --speed=115200
terminal_input serial console
terminal_output serial console

menuentry "Unikraft Hello World (AWS ENA)" {{
    multiboot /boot/unikraft.bin netdev.ipv4_addr={PRIVATE_IP} netdev.ipv4_gw_addr={GATEWAY_IP} netdev.ipv4_mask={NETMASK}
    boot
}}
"""
    (boot_dir / "grub" / "grub.cfg").write_text(grub_cfg)

    # 1. Create partition image with mkfs.ext4 -d
    part1_img = build_dir / "part1.img"
    if part1_img.exists():
        part1_img.unlink()
    run_cmd(f"mkfs.ext4 -F -L rootfs -d {staging_dir} {part1_img} 63M")

    # 2. Create raw disk image (64MB)
    disk_raw = build_dir / "disk.raw"
    if disk_raw.exists():
        disk_raw.unlink()
    with open(disk_raw, "wb") as f:
        f.seek(64 * 1024 * 1024 - 1)
        f.write(b"\0")

    # 3. Create MBR partition table with parted
    run_cmd(f"parted -s {disk_raw} mklabel msdos")
    run_cmd(f"parted -s {disk_raw} mkpart primary ext4 1MiB 100%")
    run_cmd(f"parted -s {disk_raw} set 1 boot on")

    # 4. Copy partition data into disk.raw at 1MiB offset
    with open(part1_img, "rb") as src, open(disk_raw, "r+b") as dst:
        dst.seek(1024 * 1024)
        while chunk := src.read(1024 * 1024):
            dst.write(chunk)

    # 5. Build GRUB core.img
    core_img = build_dir / "core.img"
    run_cmd([
        "grub-mkimage", "-O", "i386-pc", "-o", str(core_img),
        "-p", "(hd0,msdos1)/boot/grub",
        "biosdisk", "part_msdos", "ext2", "multiboot", "normal", "configfile", "serial", "terminfo"
    ])

    # 6. Patch boot.img and embed into sector 0 + core.img at sector 1
    with open("/usr/lib/grub/i386-pc/boot.img", "rb") as f:
        boot = bytearray(f.read())
    with open(core_img, "rb") as f:
        core = f.read()

    # Offset 0x1f4 in boot.img points to sector 1
    boot[0x1f4:0x1f8] = struct.pack("<I", 1)

    with open(disk_raw, "r+b") as dst:
        dst.seek(0)
        dst.write(boot[:440])  # keep partition table in MBR (0x1be)
        dst.seek(512)
        dst.write(core)

    print(f"[SUCCESS] Bootable disk image ready at {disk_raw} ({disk_raw.stat().st_size} bytes)")
    return disk_raw

def upload_ebs_snapshot(disk_raw):
    print("==================================================")
    print("Step 3: Uploading Disk to EBS Snapshot via EBS Direct API...")
    print("==================================================")
    # Start snapshot (1 GB minimum volume size)
    desc = f"Unikraft-ENA-Hello-World-{int(time.time())}"
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

    # Read and upload non-zero blocks (512 KiB each)
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

            # Check if block is entirely zero
            if not any(data):
                continue

            # Compute SHA256 checksum in Base64
            sha = hashlib.sha256(data).digest()
            csum_b64 = base64.b64encode(sha).decode("ascii")

            # Write block to temp file
            block_file = Path(f"build/block_{block_idx}.bin")
            block_file.write_bytes(data)

            # Put snapshot block
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
            print(f"[INFO] Uploaded block {block_idx} ({changed_blocks} non-empty blocks uploaded)")

    # Complete snapshot
    print(f"[INFO] Completing EBS snapshot {snapshot_id} (total changed blocks: {changed_blocks})...")
    run_cmd([
        "aws", "ebs", "complete-snapshot",
        "--snapshot-id", snapshot_id,
        "--changed-blocks-count", str(changed_blocks),
        "--region", AWS_REGION,
        "--output", "json"
    ])

    # Wait for snapshot to become completed
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
        print(f"[INFO] Snapshot status: {snap_out}, waiting 3s...")
        time.sleep(3)

    return snapshot_id

def register_ami(snapshot_id):
    print("==================================================")
    print("Step 4: Registering ENA-Enabled AMI...")
    print("==================================================")
    ami_name = f"unikraft-hello-http-{int(time.time())}"
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
        "--description", "Unikraft Hello World Web Server with Native AWS ENA Driver",
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
        print(f"[INFO] AMI status: {state}, waiting 3s...")
        time.sleep(3)

    return ami_id

def setup_security_group():
    print("==================================================")
    print("Step 5: Configuring Security Group for HTTP (Port 80)...")
    print("==================================================")
    # Check if security group exists
    sg_name = "unikraft-http-sg"
    sgs_out = run_cmd([
        "aws", "ec2", "describe-security-groups",
        "--filters", f"Name=group-name,Values={sg_name}",
        "--region", AWS_REGION,
        "--output", "json"
    ])
    sgs = json.loads(sgs_out).get("SecurityGroups", [])
    if sgs:
        sg_id = sgs[0]["GroupId"]
        print(f"[INFO] Found existing security group: {sg_id}")
    else:
        # Create security group
        vpcs_out = run_cmd([
            "aws", "ec2", "describe-vpcs",
            "--filters", "Name=isDefault,Values=true",
            "--region", AWS_REGION,
            "--query", "Vpcs[0].VpcId",
            "--output", "text"
        ])
        vpc_id = vpcs_out.strip()
        create_sg = run_cmd([
            "aws", "ec2", "create-security-group",
            "--group-name", sg_name,
            "--description", "Security group for Unikraft HTTP Web Server",
            "--vpc-id", vpc_id,
            "--region", AWS_REGION,
            "--output", "json"
        ])
        sg_id = json.loads(create_sg)["GroupId"]
        print(f"[INFO] Created security group: {sg_id}")

        # Authorize HTTP (port 80) and ICMP Ping
        run_cmd([
            "aws", "ec2", "authorize-security-group-ingress",
            "--group-id", sg_id,
            "--protocol", "tcp",
            "--port", "80",
            "--cidr", "0.0.0.0/0",
            "--region", AWS_REGION
        ])
        run_cmd([
            "aws", "ec2", "authorize-security-group-ingress",
            "--group-id", sg_id,
            "--protocol", "icmp",
            "--port", "-1",
            "--cidr", "0.0.0.0/0",
            "--region", AWS_REGION
        ])

    return sg_id

def launch_ec2_instance(ami_id, sg_id):
    print("==================================================")
    print(f"Step 6: Launching {INSTANCE_TYPE} EC2 Instance...")
    print("==================================================")
    # Clean up old running instances with this tag
    old_insts = run_cmd([
        "aws", "ec2", "describe-instances",
        "--filters", "Name=tag:Name,Values=unikraft-hello-http-server", "Name=instance-state-name,Values=running,pending",
        "--query", "Reservations[].Instances[].InstanceId",
        "--region", AWS_REGION,
        "--output", "json"
    ])
    old_ids = json.loads(old_insts)
    if old_ids:
        print(f"[INFO] Terminating {len(old_ids)} existing instance(s): {old_ids}...")
        run_cmd(["aws", "ec2", "terminate-instances", "--instance-ids"] + old_ids + ["--region", AWS_REGION])
        print("[INFO] Waiting for instance termination to release private IP...")
        while True:
            t_out = run_cmd([
                "aws", "ec2", "describe-instances",
                "--instance-ids"] + old_ids + [
                "--query", "Reservations[].Instances[].State.Name",
                "--region", AWS_REGION,
                "--output", "json"
            ])
            states = json.loads(t_out)
            if all(s == "terminated" for s in states):
                break
            time.sleep(2)

    launch_out = run_cmd([
        "aws", "ec2", "run-instances",
        "--image-id", ami_id,
        "--instance-type", INSTANCE_TYPE,
        "--security-group-ids", sg_id,
        "--subnet-id", SUBNET_ID,
        "--private-ip-address", PRIVATE_IP,
        "--count", "1",
        "--tag-specifications", "ResourceType=instance,Tags=[{Key=Name,Value=unikraft-hello-http-server}]",
        "--region", AWS_REGION,
        "--output", "json"
    ])
    instance_id = json.loads(launch_out)["Instances"][0]["InstanceId"]
    print(f"[SUCCESS] Launched Instance: {instance_id}")

    print("[INFO] Waiting for instance to become 'running'...")
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
            print(f"[SUCCESS] Instance is RUNNING! Public IP: {public_ip}")
            return instance_id, public_ip
        print(f"[INFO] Instance state: {state}, IP: {public_ip}, waiting 3s...")
        time.sleep(3)

def main():
    missing = [name for name in ("SUBNET_ID", "PRIVATE_IP", "GATEWAY_IP", "NETMASK") if not os.environ.get(name)]
    if missing:
        print(f"[ERR] Missing required environment variable(s): {', '.join(missing)}")
        print("[ERR] Example: SUBNET_ID=subnet-xxx PRIVATE_IP=10.0.0.5 GATEWAY_IP=10.0.0.1 NETMASK=255.255.255.0 python3 scripts/deploy_aws.py")
        sys.exit(1)
    start_time = time.time()
    kernel_path = build_unikraft()
    disk_raw = create_bootable_disk(kernel_path)
    snapshot_id = upload_ebs_snapshot(disk_raw)
    ami_id = register_ami(snapshot_id)
    sg_id = setup_security_group()
    instance_id, public_ip = launch_ec2_instance(ami_id, sg_id)

    total_time = time.time() - start_time
    print("==================================================")
    print("DEPLOYMENT COMPLETE!")
    print("==================================================")
    print(f"Instance ID : {instance_id}")
    print(f"Instance Typ: {INSTANCE_TYPE}")
    print(f"Public IPv4 : {public_ip}")
    print(f"AMI ID      : {ami_id}")
    print(f"Snapshot ID : {snapshot_id}")
    print(f"Elapsed Time: {total_time:.1f} seconds")
    print("\nTest your HTTP server with:")
    print(f"  curl -i http://{public_ip}/")
    print("==================================================")

if __name__ == "__main__":
    main()
